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
        return halide_semaphore_try_acquire(task.semaphore, task.count);
    }
    void release() {
        if (task.semaphore) {
            halide_semaphore_release(task.semaphore, task.count);
        }
    }
    bool running() {
        return task.extent || active_workers;
    }
};

#define MAX_THREADS 256

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

// The work queue and thread pool is weak, so one big work queue is shared by all halide functions
struct work_queue_t {
    // all fields are protected by this mutex.
    halide_mutex mutex;

    // Singly linked list for job stack
    work *jobs;

    // The number threads created
    int threads_created;

    // The desired number threads doing work (HL_NUM_THREADS).
    int desired_threads_working;

    // Workers sleep on one of two condition variables, to make it
    // easier to wake up the right number if a small number of tasks
    // are enqueued. There are A-team workers and B-team workers. The
    // following variables track the current size and the desired size
    // of the A team.
    int a_team_size, target_a_team_size;

    // The condition variables that workers and owners sleep on. We
    // may want to wake them up independently. Any code that may
    // invalidate any of the reasons a worker or owner may have slept
    // must signal or broadcast the appropriate condition variable.
    halide_cond wake_a_team, wake_b_team, wake_owners;

    // The number of sleeping workers and owners. An over-estimate - a
    // waking-up thread may not have decremented this yet.
    int workers_sleeping, owners_sleeping;

    // Keep track of threads so they can be joined at shutdown
    halide_thread *threads[MAX_THREADS];

    // Global flags indicating the threadpool should shut down, and
    // whether the thread pool has been initialized.
    bool shutdown, initialized;
};
WEAK work_queue_t work_queue;

__attribute__((constructor))
WEAK void initialize_work_queue() {
    halide_mutex_init(&work_queue.mutex);
}

WEAK void worker_thread(void *);

WEAK void worker_thread_already_locked(work *owned_job) {
    while (owned_job ? owned_job->running() : !work_queue.shutdown) {

        // Find a job to run, prefering things near the top of the stack.
        work *job = work_queue.jobs;
        work **prev_ptr = &work_queue.jobs;
        while (job) {
            // Only schedule tasks with enough free worker threads
            // around to complete. They may get stolen later, but only
            // by tasks which can themselves use them to complete
            // work, so forward progress is made.
            int threads_that_could_assist = 1 + work_queue.workers_sleeping;
            if (!job->task.may_block) {
                threads_that_could_assist += work_queue.owners_sleeping;
            } else if (job->owner_is_sleeping) {
                threads_that_could_assist++;
            }
            bool enough_threads = job->task.min_threads <= threads_that_could_assist;
            bool may_try = ((job == owned_job || !owned_job || !job->task.may_block) &&
                            (!job->task.serial || (job->active_workers == 0)));
            if (may_try && enough_threads && job->make_runnable()) {
                break;
            }
            prev_ptr = &(job->next_job);
            job = job->next_job;
        }

        if (!job) {
            // There is no runnable job. Go to sleep.
            if (owned_job) {
                work_queue.owners_sleeping++;
                owned_job->owner_is_sleeping = true;
                halide_cond_wait(&work_queue.wake_owners, &work_queue.mutex);
                owned_job->owner_is_sleeping = false;
                work_queue.owners_sleeping--;
            } else {
                work_queue.workers_sleeping++;
                if (work_queue.a_team_size > work_queue.target_a_team_size) {
                    // Transition to B team
                    work_queue.a_team_size--;
                    halide_cond_wait(&work_queue.wake_b_team, &work_queue.mutex);
                    work_queue.a_team_size++;
                } else {
                    halide_cond_wait(&work_queue.wake_a_team, &work_queue.mutex);
                }
                work_queue.workers_sleeping--;
            }
            continue;
        }

        // Claim a task from it.
        work myjob = *job;
        job->task.min++;
        job->task.extent--;

        // If there were no more tasks pending for this job, remove it
        // from the stack.
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

        if (!job->running() && job->owner_is_sleeping) {
            // The job is done. Wake up the owner.
            halide_cond_broadcast(&work_queue.wake_owners);
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
        halide_cond_init(&work_queue.wake_a_team);
        halide_cond_init(&work_queue.wake_b_team);
        halide_cond_init(&work_queue.wake_owners);
        work_queue.jobs = NULL;

        // Compute the desired number of threads to use. Other code
        // can also mess with this value, but only when the work queue
        // is locked.
        if (!work_queue.desired_threads_working) {
            work_queue.desired_threads_working = default_desired_num_threads();
        }
        work_queue.desired_threads_working = clamp_num_threads(work_queue.desired_threads_working);
        work_queue.a_team_size = 0;
        work_queue.target_a_team_size = 0;
        work_queue.threads_created = 0;
        work_queue.workers_sleeping = 0;
        work_queue.owners_sleeping = 0;
        work_queue.initialized = true;
    }

    // Gather some information about the work.

    // Some tasks require a minimum number of threads to make forward
    // progress. Also assume the tasks need to run concurrently.
    int min_threads = 0;

    // Count how many workers to wake. Start at -1 because this thread
    // will contribute.
    int workers_to_wake = -1;

    // Could stalled owners of other tasks conceivably help with one
    // of these jobs.
    bool stealable_jobs = false;

    for (int i = 0; i < num_jobs; i++) {
        min_threads += jobs[i].task.min_threads;
        if (!jobs[i].task.may_block) {
            stealable_jobs = true;
        }
        if (jobs[i].task.serial) {
            workers_to_wake++;
        } else {
            workers_to_wake += jobs[i].task.extent;
        }
    }

    // Are there already jobs enqueued.
    bool nested_parallelism = work_queue.jobs;

    // Spawn more threads if necessary.
    while ((work_queue.threads_created < work_queue.desired_threads_working - 1) ||
           (work_queue.threads_created < min_threads - 1)) {
        // We might need to make some new threads, if work_queue.desired_threads_working has
        // increased, or if there aren't enough threads to complete this new task.
        work_queue.a_team_size++;
        work_queue.threads[work_queue.threads_created++] =
            halide_spawn_thread(worker_thread, NULL);
    }

    // Push the jobs onto the stack.
    for (int i = num_jobs - 1; i >= 0; i--) {
        // We could bubble it downwards based on some heuristics, but
        // it's not strictly necessary to do so.
        jobs[i].next_job = work_queue.jobs;
        work_queue.jobs = jobs + i;
    }

    // Wake up an appropriate number of threads
    if (workers_to_wake) {
        if (nested_parallelism || workers_to_wake > work_queue.desired_threads_working - 1) {
            // If there's nested parallelism going on, we just wake up
            // everyone. TODO: make this more precise.
            work_queue.target_a_team_size = work_queue.desired_threads_working - 1;
        } else {
            work_queue.target_a_team_size = workers_to_wake;
        }
        halide_cond_broadcast(&work_queue.wake_a_team);
        if (work_queue.target_a_team_size > work_queue.a_team_size) {
            halide_cond_broadcast(&work_queue.wake_b_team);
            if (stealable_jobs) {
                halide_cond_broadcast(&work_queue.wake_owners);
            }
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
    job.task.may_block = false; // May only call do_par_for if it there are no inner forks or acquires, so TODO: set this to false
    job.task.serial = false;
    job.task.semaphore = NULL;
    job.task.closure = closure;
    job.task.min_threads = 1;
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
         halide_semaphore_try_acquire(tasks->semaphore, tasks->count))) {
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
    // print(NULL) << (void *)(&jobs) << " Enqueuing " << num_tasks << " jobs\n";
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
    if (work_queue.initialized) {
        // Wake everyone up and tell them the party's over and it's time
        // to go home
        halide_mutex_lock(&work_queue.mutex);
        work_queue.shutdown = true;

        halide_cond_broadcast(&work_queue.wake_a_team);
        halide_cond_broadcast(&work_queue.wake_b_team);
        halide_cond_broadcast(&work_queue.wake_owners);
        halide_mutex_unlock(&work_queue.mutex);

        // Wait until they leave
        for (int i = 0; i < work_queue.threads_created; i++) {
            halide_join_thread(work_queue.threads[i]);
        }

        // Tidy up
        halide_mutex_destroy(&work_queue.mutex);
        halide_cond_destroy(&work_queue.wake_a_team);
        halide_cond_destroy(&work_queue.wake_b_team);
        halide_cond_destroy(&work_queue.wake_owners);
        work_queue.initialized = false;
    }
}

struct halide_semaphore_impl_t {
    int value;
};

WEAK int halide_semaphore_init(halide_semaphore_t *s, int n) {
    halide_semaphore_impl_t *sem = (halide_semaphore_impl_t *)s;
    sem->value = n;
    return n;
}

WEAK int halide_semaphore_release(halide_semaphore_t *s, int n) {
    halide_semaphore_impl_t *sem = (halide_semaphore_impl_t *)s;
    int new_val = __sync_add_and_fetch(&(sem->value), n);
    if (new_val == n) {
        // We may have just made a job runnable
        halide_cond_broadcast(&work_queue.wake_a_team);
        halide_cond_broadcast(&work_queue.wake_owners);
    }
    return new_val;
}

WEAK bool halide_semaphore_try_acquire(halide_semaphore_t *s, int n) {
    halide_semaphore_impl_t *sem = (halide_semaphore_impl_t *)s;
    // Decrement and get new value
    int new_val = __sync_add_and_fetch(&(sem->value), -n);
    if (new_val < 0) {
        // Oops, increment and return failure
        __sync_add_and_fetch(&(sem->value), n);
        return false;
    }
    return true;
}

}
