#include "printer.h"

extern "C" int pthread_self();

namespace Halide { namespace Runtime { namespace Internal {

struct work {
    halide_parallel_task_t task;
    work *next_job;
    void *user_context;
    int active_workers;
    int exit_status;
    // which condition variable is the owner sleeping on. NULL if it isn't sleeping.
    bool owner_is_sleeping;
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

    // The condition variables that workers and owners sleep on. We
    // may want to wake them up independently. Any code that may
    // invalidate any of the reasons a worker or owner may have slept
    // must signal or broadcast the appropriate condition variable.
    halide_cond worker_cond_var, owner_cond_var;

    // The number of sleeping workers and owners. An over-estimate - a
    // waking-up thread may not have decremented this yet.
    int workers_sleeping, owners_sleeping;

    // Keep track of threads so they can be joined at shutdown
    halide_thread *threads[MAX_THREADS];

    // Global flags indicating the threadpool should shut down, and
    // whether the thread pool has been initialized.
    bool shutdown, initialized;

    bool running() {
        return !shutdown;
    }

    void wake_owners() {
        halide_cond_broadcast(&owner_cond_var);
    }

    void wake_one_worker() {
        halide_cond_signal(&worker_cond_var);
    }

    void wake_workers() {
        halide_cond_broadcast(&worker_cond_var);
    }

    void wake_all() {
        wake_workers();
        wake_owners();
    }

    void sleep(work *owned_job) {
        if (owned_job) {
            owners_sleeping++;
            owned_job->owner_is_sleeping = true;
            halide_cond_wait(&owner_cond_var, &mutex);
            owned_job->owner_is_sleeping = false;
            owners_sleeping--;
        } else {
            workers_sleeping++;
            halide_cond_wait(&worker_cond_var, &mutex);
            workers_sleeping--;
        }
    }

};
WEAK work_queue_t work_queue;

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
                    << "workers sleeping: " << work_queue.workers_sleeping << "\n"
                    << "owners sleeping: " << work_queue.owners_sleeping << "\n";
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

        // Find a job to run, prefering things near the top of the stack.
        work *job = work_queue.jobs;
        work **prev_ptr = &work_queue.jobs;
        while (job) {
            int threads_that_could_assist = 1 + work_queue.workers_sleeping;
            if (!job->task.may_block) {
                threads_that_could_assist += work_queue.owners_sleeping;
            } else if (job->owner_is_sleeping) {
                threads_that_could_assist++;
            }
            bool enough_threads = job->task.min_threads <= threads_that_could_assist;
            bool may_try = (job == owned_job || !owned_job || !job->task.may_block);
            if (may_try && enough_threads) {
                if (job->make_runnable()) break;
            }

            print(NULL) << "Unrunnable job "
                        << (job ?
                            (job->task.name ? job->task.name : "unnamed") :
                            "none") << "\n";
            prev_ptr = &(job->next_job);
            job = job->next_job;
        }

        // Determine if I can run it, if not sleep on the appropriate condition variable.
        if (job == NULL) {
            print(NULL) << "No runnable jobs\n";
            // There is no runnable job. Go to sleep.
            work_queue.sleep(owned_job);
            continue;
        }

        print(NULL) << "Running job: "
                    << (job ?
                        (job->task.name ? job->task.name : "unnamed") :
                        "none") << " (" << job->task.min_threads << ")\n";

        // Claim a task from it.
        work myjob = *job;
        job->task.min++;
        job->task.extent--;

        // If there were no more tasks pending for this job,
        // remove it from the stack.
        if (job->task.extent == 0) {
            *prev_ptr = job->next_job;
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

        // If this task failed, set the exit status on the job.
        if (result) {
            job->exit_status = result;
        }

        // We are no longer active on this job
        job->active_workers--;

        // Wake up the owner if the job is done.
        if (!job->running() && job->owner_is_sleeping) {
            work_queue.wake_owners();
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
        halide_cond_init(&work_queue.worker_cond_var);
        halide_cond_init(&work_queue.owner_cond_var);
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
        work_queue.workers_sleeping = 0;
        work_queue.owners_sleeping = 0;
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
        // We could bubble it downwards based on some heuristics, but
        // it's not strictly necessary to do so.
        jobs[i].next_job = work_queue.jobs;
        work_queue.jobs = jobs + i;
    }

    if (size < work_queue.workers_sleeping) {
        // Wake up enough workers
        for (int i = 0; i < size; i++) {
            work_queue.wake_one_worker();
        }
    } else {
        // Wake up everyone
        work_queue.wake_workers();
        if (stealable_jobs) {
            work_queue.wake_owners();
        }
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
    job.task.may_block = true; // May only call do_par_for if it there are no inner forks or acquires, so TODO: set this to false
    job.task.serial = false;
    job.task.semaphore = NULL;
    job.task.closure = closure;
    job.task.depth = 0; // TODO: We actually need this information! If there are no inner forks or acquires, maybe set it to int max? It'll have the same priority as any parallel loops it spawns.
    job.task.min_threads = 3; // TODO: We need this information too! 3 is enough to make correctness_async run. 1 would be the correct value here.
    job.task.name = NULL;
    job.user_context = user_context;
    job.exit_status = 0;
    job.active_workers = 0;
    job.owner_is_sleeping = false;
    halide_mutex_lock(&work_queue.mutex);
    enqueue_work_already_locked(1, &job);
    worker_thread_already_locked(&job);
    halide_mutex_unlock(&work_queue.mutex);
    return job.exit_status;
}

WEAK int halide_do_parallel_tasks(void *user_context, int num_tasks,
                                  halide_parallel_task_t *tasks) {
    // Avoid entering the task system if possible
    if (num_tasks == 1 &&
        tasks->extent == 1 &&
        (tasks->semaphore == NULL ||
         halide_semaphore_try_acquire(tasks->semaphore))) {
        return tasks->fn(user_context, tasks->min, tasks->closure);
    }

    work *jobs = (work *)__builtin_alloca(sizeof(work) * num_tasks);

    for (int i = 0; i < num_tasks; i++) {
        jobs[i].task = tasks[i];
        jobs[i].user_context = user_context;
        jobs[i].exit_status = 0;
        jobs[i].active_workers = 0;
        jobs[i].owner_is_sleeping = false;
    }

    halide_mutex_lock(&work_queue.mutex);
    ////print(NULL) << (void *)(&jobs) << " Enqueuing " << num_tasks << " jobs\n";
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

    work_queue.wake_all();
    halide_mutex_unlock(&work_queue.mutex);

    // Wait until they leave
    for (int i = 0; i < work_queue.threads_created; i++) {
        halide_join_thread(work_queue.threads[i]);
    }

    // Tidy up
    halide_mutex_destroy(&work_queue.mutex);
    halide_cond_destroy(&work_queue.worker_cond_var);
    halide_cond_destroy(&work_queue.owner_cond_var);
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
        work_queue.wake_all();
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
