#include "printer.h"

extern "C" int pthread_self();

namespace Halide { namespace Runtime { namespace Internal {

struct work;

struct work_queue_cond {
    halide_cond cond_var;
    int sleepers;
    void init() {
        sleepers = 0;
        halide_cond_init(&cond_var);
    }
    void destroy() {
        halide_cond_destroy(&cond_var);
    }
    void broadcast() {
        halide_cond_broadcast(&cond_var);
    }
    void signal() {
        halide_cond_signal(&cond_var);
    }
    void sleep(work *);
};

struct work {
    halide_parallel_task_t task;
    work *next_job;
    void *user_context;
    int active_workers;
    int exit_status;
    // which condition variable is the owner sleeping on. NULL if it isn't sleeping.
    work_queue_cond *owner_sleeping_on;
    int make_runnable() {
        if (task.semaphore == NULL) {
            return 0x7fffffff;
        }
        return halide_semaphore_try_acquire(task.semaphore);
    }
    void release() {
        if (task.semaphore) {
            halide_semaphore_release(task.semaphore);
        }
    }
    bool running() {
        return task.extent || active_workers;
    }
};

// The work queue and thread pool is weak, so one big work queue is shared by all halide functions
#define MAX_THREADS 64
struct work_queue_t {
    // all fields are protected by this mutex.
    halide_mutex mutex;

    // Singly linked list for job stack
    work *jobs;

    // The number threads created
    int threads_created;

    // The number of threads doing work
    int threads_working;

    // The desired number threads doing work (HL_NUM_THREADS).
    int desired_threads_working;

    // Below is a list of all the reasons a thread might sleep inside
    // the task system. These get broadcast or signalled whenever
    // something happens that might change the corresponding
    // condition. There are points where we need to count the number
    // of threads in various states, so we we also track the number of
    // threads waiting on each condition. Note that a thread where the
    // condition variable was signalled but hasn't yet been scheduled
    // still counts as sleeping for the purposes of the sleepers
    // field.

    // A thread is ready to work, but there's no work to do.
    struct work_queue_cond worker_no_runnable_jobs;

    // A thread is ready to work, but threads_working >= desired_threads_working.
    //struct work_queue_cond too_many_threads_awake;

    // There aren't enough other uncommited threads to assist for it
    // to be safe to start with a guarantee of completion.
    struct work_queue_cond worker_not_enough_threads_for_next_job;

    // The next runnable job can't safely be stolen by this thread,
    // due to deadlock-avoidance checks.
    struct work_queue_cond worker_may_not_steal_next_job;

    // Same as above, plus I own a piece of work, so I may not be able
    // to assist with any old job.
    struct work_queue_cond owner_not_enough_threads_for_next_job;

    // The next runnable job can't safely be stolen by this thread,
    // due to deadlock-avoidance checks.
    struct work_queue_cond owner_may_not_steal_next_job;

    // A thread's own job is has been stolen or is not runnable, and
    // there are no other runnable jobs.
    struct work_queue_cond owner_no_runnable_jobs;

    // Keep track of threads so they can be joined at shutdown
    halide_thread *threads[MAX_THREADS];

    // Global flags indicating the threadpool should shut down, and
    // whether the thread pool has been initialized.
    bool shutdown, initialized;

    bool running() {
        return !shutdown;
    }
};
WEAK work_queue_t work_queue;

void work_queue_cond::sleep(work *owned_job) {
    sleepers++;
    if (owned_job) {
        owned_job->owner_sleeping_on = this;
        halide_cond_wait(&cond_var, &work_queue.mutex);
        owned_job->owner_sleeping_on = NULL;
    } else {
        halide_cond_wait(&cond_var, &work_queue.mutex);
    }
    sleepers--;
}

WEAK int clamp_num_threads(int threads) {
    if (threads > MAX_THREADS) {
        threads = MAX_THREADS;
    } else if (threads < 1) {
        threads = 1;
    }
    return threads;
}

WEAK int default_desired_num_threads() {
    int desired_num_threads = 0;
    char *threads_str = getenv("HL_NUM_THREADS");
    if (!threads_str) {
        // Legacy name for HL_NUM_THREADS
        threads_str = getenv("HL_NUMTHREADS");
    }
    if (threads_str) {
        desired_num_threads = atoi(threads_str);
    } else {
        desired_num_threads = halide_host_cpu_count();
    }
    return desired_num_threads;
}

WEAK void worker_thread(void *);

WEAK void worker_thread_already_locked(work *owned_job) {
    while (owned_job ? owned_job->running() : work_queue.running()) {

        print(NULL) << "\n"
                    << "TID: " << pthread_self() << "\n"
                    << "threads created: " << work_queue.threads_created << "\n"
                    << "threads working: " << work_queue.threads_working << "\n"
                    << "desired threads working: " << work_queue.desired_threads_working << "\n"
                    << "worker no runnable jobs: " << work_queue.worker_no_runnable_jobs.sleepers << "\n"
                    << "owner no runnable jobs: " << work_queue.owner_no_runnable_jobs.sleepers << "\n"
                    << "worker not enough threads: " << work_queue.worker_not_enough_threads_for_next_job.sleepers << "\n"
                    << "owner not enough threads: " << work_queue.owner_not_enough_threads_for_next_job.sleepers << "\n"
                    << "worker may not steal: " << work_queue.worker_may_not_steal_next_job.sleepers << "\n"
                    << "owner may not steal: " << work_queue.owner_may_not_steal_next_job.sleepers << "\n";
        print(NULL) << "Task queue:\n";
        for (work *job = work_queue.jobs; job; job = job->next_job) {
            print(NULL) << job->task.depth << ": " <<
                (job ?
                 (job->task.name ? job->task.name : "unnamed") :
                 "none") << "\n";
        }

        print(NULL) << "Owned job: "
                    << (owned_job ?
                        (owned_job->task.name ? owned_job->task.name : "unnamed") :
                        "none") << "\n";

        // Figure out which job should be scheduled next: The deepest
        // runnable job. The stack is already in order of task depth.
        work *job = work_queue.jobs;
        work **prev_ptr = &work_queue.jobs;
        int old_semaphore_value = 0;
        while (job && !(old_semaphore_value = job->make_runnable())) {
            print(NULL) << "Unrunnable job "
                        << (job ?
                            (job->task.name ? job->task.name : "unnamed") :
                            "none") << "\n";
            prev_ptr = &(job->next_job);
            job = job->next_job;
        }

        if (job) {
            print(NULL) << "Next runnable job: "
                        << (job ?
                            (job->task.name ? job->task.name : "unnamed") :
                            "none") << " (" << job->task.min_threads << ")\n";
        }

        // Count the number of threads that could assist with this job
        // if no other work was scheduled in the meantime.
        int threads_that_could_assist =
            (work_queue.worker_no_runnable_jobs.sleepers +
             work_queue.worker_not_enough_threads_for_next_job.sleepers +
             work_queue.worker_may_not_steal_next_job.sleepers);

        if (job) {
            if (!job->task.may_block) {
                threads_that_could_assist += work_queue.owner_may_not_steal_next_job.sleepers;
                threads_that_could_assist += work_queue.owner_no_runnable_jobs.sleepers;
                threads_that_could_assist += work_queue.owner_not_enough_threads_for_next_job.sleepers;
            } else if (job->owner_sleeping_on) {
                threads_that_could_assist++;
            }
        }

        // Determine if I can run it, if not sleep on the appropriate condition variable.
        if (job == NULL) {
            print(NULL) << "No runnable jobs\n";
            // There is no runnable job. Go to sleep.
            if (owned_job) {
                work_queue.owner_no_runnable_jobs.sleep(owned_job);
            } else {
                work_queue.worker_no_runnable_jobs.sleep(NULL);
            }
            continue;
        } else if (job != owned_job && job->owner_sleeping_on) {
            print(NULL) << "Not running it because the owner is sleeping\n";
            job->release();
            job->owner_sleeping_on->broadcast();
            if (owned_job) {
                work_queue.owner_may_not_steal_next_job.sleep(owned_job);
            } else {
                work_queue.worker_may_not_steal_next_job.sleep(NULL);
            }
            continue;
        } else if (owned_job && job != owned_job && job->task.may_block) {
            print(NULL) << "Can't steal blocking task\n";
            job->release();
            work_queue.owner_may_not_steal_next_job.sleep(owned_job);
            continue;
        } else if (job->task.min_threads > threads_that_could_assist + 1) {
            print(NULL) << "Not running it because there aren't enough threads to safely start ("
                << (threads_that_could_assist + 1) << "/" << job->task.min_threads << ")\n";
            job->release();
            if (owned_job) {
                work_queue.owner_not_enough_threads_for_next_job.sleep(owned_job);
            } else {
                work_queue.worker_not_enough_threads_for_next_job.sleep(NULL);
            }
            continue;
        }
        print(NULL) << "Running it!\n";

        // Claim a task from it.
        work myjob = *job;
        job->task.min++;
        job->task.extent--;

        // If there were no more tasks pending for this job,
        // remove it from the stack.
        if (job->task.extent == 0) {
            *prev_ptr = job->next_job;
        }

        // Check if we may have changed what counts as the next runnable job
        if (old_semaphore_value == 1 || job->task.extent == 0) {
            work_queue.owner_not_enough_threads_for_next_job.broadcast();
            work_queue.worker_not_enough_threads_for_next_job.broadcast();
            work_queue.owner_may_not_steal_next_job.broadcast();
            work_queue.worker_may_not_steal_next_job.broadcast();
        }

        // Increment the active_worker count so that other threads
        // are aware that this job is still in progress even
        // though there are no outstanding tasks for it.
        job->active_workers++;

        // Release the lock and do the task.
        halide_mutex_unlock(&work_queue.mutex);
        int result = halide_do_task(myjob.user_context, myjob.task.fn, myjob.task.min,
                                    myjob.task.closure);
        halide_mutex_lock(&work_queue.mutex);
        print(NULL) << "\nTID: " << pthread_self() << "\n"
                    << "Done with task: "
                    << (job ?
                        (job->task.name ? job->task.name : "unnamed") :
                        "none") << "\n";

        // If this task failed, set the exit status on the job.
        if (result) {
            job->exit_status = result;
        }

        // We are no longer active on this job
        job->active_workers--;

        // Wake up the owner if the job is done.
        if (!job->running() && job->owner_sleeping_on) {
            job->owner_sleeping_on->broadcast();
            print(NULL) << "Finished someone else's task " << (job ?
                                           (job->task.name ? job->task.name : "unnamed") :
                                           "none") << "\n";
        }
    }
}

WEAK void worker_thread(void *arg) {
    halide_mutex_lock(&work_queue.mutex);
    worker_thread_already_locked((work *)arg);
    halide_mutex_unlock(&work_queue.mutex);
}

WEAK void enqueue_work_already_locked(int num_jobs, work *jobs) {
    if (!work_queue.initialized) {
        work_queue.shutdown = false;
        work_queue.worker_no_runnable_jobs.init();
        work_queue.worker_not_enough_threads_for_next_job.init();
        work_queue.worker_may_not_steal_next_job.init();
        work_queue.owner_not_enough_threads_for_next_job.init();
        work_queue.owner_may_not_steal_next_job.init();
        work_queue.owner_no_runnable_jobs.init();
        work_queue.jobs = NULL;

        // Compute the desired number of threads to use. Other code
        // can also mess with this value, but only when the work queue
        // is locked.
        if (!work_queue.desired_threads_working) {
            work_queue.desired_threads_working = default_desired_num_threads();
        }
        work_queue.desired_threads_working = clamp_num_threads(work_queue.desired_threads_working);
        work_queue.threads_created = 0;
        work_queue.threads_working = 1;
        work_queue.initialized = true;
    }

    // Some tasks require a minimum number of threads to make forward
    // progress. They won't influence the size of the A team, so
    // they'll take turns running.
    int min_threads = 0;
    for (int i = 0; i < num_jobs; i++) {
        if (jobs[i].task.min_threads > min_threads) {
            min_threads = jobs[i].task.min_threads;
        }
    }

    while ((work_queue.threads_created < work_queue.desired_threads_working - 1) ||
           (work_queue.threads_created < min_threads - 1)) {
        // We might need to make some new threads, if work_queue.desired_threads_working has
        // increased.
        work_queue.threads[work_queue.threads_created++] =
            halide_spawn_thread(worker_thread, NULL);
    }

    int size = 0;
    bool stealable_jobs = false;
    for (int i = 0; i < num_jobs; i++) {
        if (!jobs[i].task.may_block) {
            stealable_jobs = true;
        }
        size += jobs[i].task.extent;
    }

    // Push the jobs onto the stack.

    for (int i = num_jobs - 1; i >= 0; i--) {
        // Bubble it downwards to the appropriate level given its depth.
        work **prev_ptr = &work_queue.jobs;
        work *job = work_queue.jobs;
        int my_depth = jobs[i].task.depth;
        while (job && job->task.depth > my_depth) {
            prev_ptr = &(job->next_job);
            job = job->next_job;
        }
        *prev_ptr = jobs + i;
        jobs[i].next_job = job;
    }

    // Wake up all threads that care about there potentially being a
    // new runnable job (TODO: Don't wake up too many threads)
    work_queue.worker_no_runnable_jobs.broadcast();
    work_queue.worker_not_enough_threads_for_next_job.broadcast();
    work_queue.worker_may_not_steal_next_job.broadcast();
    if (stealable_jobs) {
        work_queue.owner_not_enough_threads_for_next_job.broadcast();
        work_queue.owner_may_not_steal_next_job.broadcast();
        work_queue.owner_no_runnable_jobs.broadcast();
    }
}

}}}  // namespace Halide::Runtime::Internal

using namespace Halide::Runtime::Internal;

extern "C" {

WEAK int halide_default_do_task(void *user_context, halide_task_t f, int idx,
                                uint8_t *closure) {
    return f(user_context, idx, closure);
}

WEAK int halide_default_do_par_for(void *user_context, halide_task_t f,
                                   int min, int size, uint8_t *closure) {
    work job;
    job.task.fn = f;
    job.task.min = min;
    job.task.extent = size;
    job.task.may_block = true; // May only call do_par_for if it there are no inner forks or acquires.
    job.task.serial = false;
    job.task.semaphore = NULL;
    job.task.closure = closure;
    job.task.depth = 0; // TODO: We actually need this information!
    job.task.min_threads = 3; // TODO: We need this information too!
    job.task.name = NULL;
    job.user_context = user_context;
    job.exit_status = 0;
    job.active_workers = 0;
    job.owner_sleeping_on = NULL;
    halide_mutex_lock(&work_queue.mutex);
    enqueue_work_already_locked(1, &job);
    worker_thread_already_locked(&job);
    halide_mutex_unlock(&work_queue.mutex);
    return job.exit_status;
}

WEAK int halide_do_parallel_tasks(void *user_context, int num_tasks,
                                  halide_parallel_task_t *tasks) {
    // Avoid entering the task system if possible
    /* Disabled while debugging the task system
    if (num_tasks == 1 &&
        tasks->extent == 1 &&
        (tasks->semaphore == NULL ||
         halide_semaphore_try_acquire(tasks->semaphore))) {
        return tasks->fn(user_context, tasks->min, tasks->closure);
    }
    */

    work *jobs = (work *)__builtin_alloca(sizeof(work) * num_tasks);

    for (int i = 0; i < num_tasks; i++) {
        jobs[i].task = tasks[i];
        jobs[i].user_context = user_context;
        jobs[i].exit_status = 0;
        jobs[i].active_workers = 0;
        jobs[i].owner_sleeping_on = NULL;
    }

    halide_mutex_lock(&work_queue.mutex);
    //print(NULL) << (void *)(&jobs) << " Enqueuing " << num_tasks << " jobs\n";
    enqueue_work_already_locked(num_tasks, jobs);
    int exit_status = 0;
    for (int i = 0; i < num_tasks; i++) {
        //print(NULL) << (void *)(&jobs) << " Joining task " << i << "\n";
        // TODO: Is it bad to join the tasks in a specific order?
        worker_thread_already_locked(jobs + i);
        if (jobs[i].exit_status != 0) {
            exit_status = jobs[i].exit_status;
        }
    }
    //print(NULL) << (void *)(&jobs) << " All tasks joined\n";
    halide_mutex_unlock(&work_queue.mutex);
    return exit_status;
}

WEAK int halide_set_num_threads(int n) {
    if (n < 0) {
        halide_error(NULL, "halide_set_num_threads: must be >= 0.");
    }
    // Don't make this an atomic swap - we don't want to be changing
    // the desired number of threads while another thread is in the
    // middle of a sequence of non-atomic operations.
    halide_mutex_lock(&work_queue.mutex);
    if (n == 0) {
        n = default_desired_num_threads();
    }
    int old = work_queue.desired_threads_working;
    work_queue.desired_threads_working = clamp_num_threads(n);
    halide_mutex_unlock(&work_queue.mutex);
    return old;
}

WEAK void halide_shutdown_thread_pool() {
    if (!work_queue.initialized) return;

    // Wake everyone up and tell them the party's over and it's time
    // to go home
    halide_mutex_lock(&work_queue.mutex);
    work_queue.shutdown = true;

    work_queue.worker_no_runnable_jobs.broadcast();
    work_queue.worker_not_enough_threads_for_next_job.broadcast();
    work_queue.worker_may_not_steal_next_job.broadcast();
    work_queue.owner_not_enough_threads_for_next_job.broadcast();
    work_queue.owner_may_not_steal_next_job.broadcast();
    work_queue.owner_no_runnable_jobs.broadcast();
    halide_mutex_unlock(&work_queue.mutex);

    // Wait until they leave
    for (int i = 0; i < work_queue.threads_created; i++) {
        halide_join_thread(work_queue.threads[i]);
    }

    // Tidy up
    halide_mutex_destroy(&work_queue.mutex);
    work_queue.worker_no_runnable_jobs.destroy();
    work_queue.worker_not_enough_threads_for_next_job.destroy();
    work_queue.worker_may_not_steal_next_job.destroy();
    work_queue.owner_not_enough_threads_for_next_job.destroy();
    work_queue.owner_may_not_steal_next_job.destroy();
    work_queue.owner_no_runnable_jobs.destroy();
    work_queue.initialized = false;
}

struct halide_semaphore_impl_t {
    int value;
};

WEAK int halide_semaphore_init(halide_semaphore_t *s, int val) {
    halide_semaphore_impl_t *sem = (halide_semaphore_impl_t *)s;
    sem->value = val;
    return val;
}

WEAK int halide_semaphore_release(halide_semaphore_t *s) {
    halide_semaphore_impl_t *sem = (halide_semaphore_impl_t *)s;
    int new_val = __sync_add_and_fetch(&(sem->value), 1);
    if (new_val == 1) {
        // We may have just made a job runnable
        work_queue.worker_no_runnable_jobs.broadcast();
        work_queue.worker_not_enough_threads_for_next_job.broadcast();
        work_queue.worker_may_not_steal_next_job.broadcast();
        work_queue.owner_not_enough_threads_for_next_job.broadcast();
        work_queue.owner_may_not_steal_next_job.broadcast();
        work_queue.owner_no_runnable_jobs.broadcast();
    }
    return new_val;
}

WEAK int halide_semaphore_try_acquire(halide_semaphore_t *s) {
    halide_semaphore_impl_t *sem = (halide_semaphore_impl_t *)s;
    // Decrement and get new value
    int old_val = __sync_fetch_and_add(&(sem->value), -1);
    if (old_val < 1) {
        // Oops, increment and return failure
        __sync_add_and_fetch(&(sem->value), 1);
        old_val = 0;
    }
    return old_val;
}

}
