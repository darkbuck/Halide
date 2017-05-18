#include "printer.h"

extern "C" int pthread_self();

namespace Halide { namespace Runtime { namespace Internal {

struct work {
    halide_parallel_task_t task;
    work *next_job; void *user_context;
    int active_workers;
    int exit_status;
    bool owner_is_on_b_team;
    bool owner_is_sleeping;
    bool make_runnable() {
        return !task.semaphore || halide_semaphore_try_acquire(task.semaphore);
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

    // Worker threads are divided into an 'A' team and a 'B' team. The
    // B team sleeps on the wakeup_b_team condition variable. The A
    // team does work. Threads transition to the B team if they wake
    // up and find that a_team_size > target_a_team_size.  Threads
    // move into the A team whenever they wake up and find that
    // a_team_size < target_a_team_size.
    int a_team_size, b_team_size, target_a_team_size;

    // The number of top-level threads currently sleeping due to lack of work
    int idle_a_team_workers, idle_b_team_workers;

    int sleeping_a_team_owners, sleeping_b_team_owners;

    // Broadcast when a job completes.
    halide_cond wakeup_owners;

    // Broadcast whenever items are added to the work queue.
    halide_cond wakeup_a_team;

    // May also be broadcast when items are added to the work queue if
    // more threads are required than are currently in the A team.
    halide_cond wakeup_b_team;

    // Keep track of threads so they can be joined at shutdown
    halide_thread *threads[MAX_THREADS];

    // The number threads created
    int threads_created;

    // The desired number threads doing work.
    int desired_num_threads;

    // Global flags indicating the threadpool should shut down, and
    // whether the thread pool has been initialized.
    bool shutdown, initialized;

    bool running() {
        return !shutdown;
    }

};
WEAK work_queue_t work_queue;

WEAK int clamp_num_threads(int desired_num_threads) {
    if (desired_num_threads > MAX_THREADS) {
        desired_num_threads = MAX_THREADS;
    } else if (desired_num_threads < 1) {
        desired_num_threads = 1;
    }
    return desired_num_threads;
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

WEAK bool add_one_worker_thread_to_pool() {
    if (work_queue.target_a_team_size > work_queue.a_team_size) {
        // Just wake up the B team
        halide_cond_broadcast(&work_queue.wakeup_b_team);
    } else if (work_queue.threads_created < MAX_THREADS) {
        int id = work_queue.threads_created++;
        work_queue.threads[id] = halide_spawn_thread(worker_thread, NULL);
        print(NULL) << "Created thread: " << id << "\n";
    } else {
        return false;
    }
    return true;
}

WEAK void sleep_on_b_team(work *owned_job) {
    work_queue.a_team_size--;
    work_queue.b_team_size++;
    if (owned_job) {
        work_queue.sleeping_b_team_owners++;
        owned_job->owner_is_on_b_team = true;
        owned_job->owner_is_sleeping = true;
        halide_cond_wait(&work_queue.wakeup_owners, &work_queue.mutex);
        owned_job->owner_is_sleeping = false;
        owned_job->owner_is_on_b_team = false;
        work_queue.sleeping_b_team_owners--;
    } else {
        work_queue.idle_b_team_workers++;
        halide_cond_wait(&work_queue.wakeup_b_team, &work_queue.mutex);
        work_queue.idle_b_team_workers--;
    }
    work_queue.b_team_size--;
    work_queue.a_team_size++;
}

WEAK void sleep_on_a_team(work *owned_job) {
    if (owned_job) {
        work_queue.sleeping_a_team_owners++;
        owned_job->owner_is_sleeping = true;
        halide_cond_wait(&work_queue.wakeup_owners, &work_queue.mutex);
        owned_job->owner_is_sleeping = false;
        work_queue.sleeping_a_team_owners--;
    } else {
        work_queue.idle_a_team_workers++;
        halide_cond_wait(&work_queue.wakeup_a_team, &work_queue.mutex);
        work_queue.idle_a_team_workers--;
    }
}

WEAK void worker_thread_already_locked(work *owned_job) {
    if (!owned_job) {
        work_queue.idle_a_team_workers--;
    }

    while (owned_job ? owned_job->running() : work_queue.running()) {

        print(NULL) << "\n"
                    << "TID: " << pthread_self() << "\n"
                    << "threads created: " << work_queue.threads_created << "\n"
                    << "threads desired: " << work_queue.desired_num_threads << "\n"
                    << "a team size: " << work_queue.a_team_size << "\n"
                    << "target a team size: " << work_queue.target_a_team_size << "\n"
                    << "idle a team workers: " << work_queue.idle_a_team_workers << "\n"
                    << "sleeping a team owners: " << work_queue.sleeping_a_team_owners << "\n"
                    << "b team size: " << work_queue.b_team_size << "\n"
                    << "idle b team workers: " << work_queue.idle_b_team_workers << "\n"
                    << "sleeping b team owners: " << work_queue.sleeping_b_team_owners << "\n";

        print(NULL) << "Task queue:\n";
        for (work *job = work_queue.jobs; job; job = job->next_job) {
            print(NULL) << job->task.depth << ": " <<
                (job ?
                 (job->task.name ? job->task.name : "unnamed") :
                 "none") << "\n";
        }

        // Check invariants
        halide_assert(NULL,
                      work_queue.a_team_size + work_queue.b_team_size == work_queue.threads_created + 1);

        halide_assert(NULL, !owned_job || !owned_job->owner_is_sleeping);

        print(NULL) << "Owned job: "
                    << (owned_job ?
                        (owned_job->task.name ? owned_job->task.name : "unnamed") :
                        "none") << "\n";

        // Transition threads that aren't supposed to be awake to the b team
        if (work_queue.a_team_size > work_queue.target_a_team_size) {
            print(NULL) << "Worker should not be awake. Joining B team\n";
            sleep_on_b_team(owned_job);
            continue;
        }

        // 1) Figure out which job should be scheduled next: The deepest runnable job.
        work *job = work_queue.jobs;
        work **prev_ptr = &work_queue.jobs;
        while (job && !job->make_runnable()) {
            print(NULL) << "Unrunnable job: "
                        << (job ?
                            (job->task.name ? job->task.name : "unnamed") :
                            "none") << "\n";
            prev_ptr = &(job->next_job);
            job = job->next_job;
        }

        print(NULL) << "Next runnable job: "
                    << (job ?
                        (job->task.name ? job->task.name : "unnamed") :
                        "none") << "\n";

        // 2) Figure out which thread should run it, wake them up, and figure out what this thread should do.
        bool should_join_b_team = work_queue.a_team_size > work_queue.target_a_team_size;
        bool should_run_job = false;
        if (job == NULL) {
            print(NULL) << "No runnable jobs\n";
            // There is no job. Go to sleep.
        } else if (job == owned_job) {
            should_run_job = true;
            print(NULL) << "Working on my own job\n";
        } else if (owned_job && job->task.depth == owned_job->task.depth && owned_job->make_runnable()) {
            // I am permitted to work on my own job, as it has the same depth as the deepest runnable task
            job->release();
            job = owned_job;
            should_run_job = true;
            print(NULL) << "Working on my own job instead\n";
        } else if (job->owner_is_sleeping) {
            // The job's owner should run it. Wake them up and go to sleep.
            should_join_b_team = job->owner_is_on_b_team;
            halide_cond_broadcast(&work_queue.wakeup_owners);
            print(NULL) << "Waking owner to run it\n";
        } else if (!owned_job || !job->task.may_block) {
            should_run_job = true;
            // I should run it
            print(NULL) << "Stealing job\n";
        } else if (work_queue.idle_a_team_workers) {
            // An idle A-team worker should run it.
            halide_cond_broadcast(&work_queue.wakeup_a_team);
            print(NULL) << "Waking an A team worker to run it\n";
        } else if (work_queue.sleeping_a_team_owners && !job->task.may_block) {
            // A sleeping A-team owner could run it
            halide_cond_broadcast(&work_queue.wakeup_owners);
            print(NULL) << "Waking an A team owner to run it\n";
        } else if (work_queue.idle_b_team_workers) {
            // An idle B-team worker could run it
            halide_cond_broadcast(&work_queue.wakeup_b_team);
            should_join_b_team = true;
            print(NULL) << "Waking a B team worker to run it\n";
        } else if (work_queue.sleeping_b_team_owners && !job->task.may_block) {
            // A sleeping B-team owner could run it
            halide_cond_broadcast(&work_queue.wakeup_owners);
            should_join_b_team = true;
            print(NULL) << "Waking a B team owner to run it\n";
        } else if (work_queue.threads_created < 2) {
            // Nobody can run it, but we're allowed to add a new worker
            int id = work_queue.threads_created++;
            work_queue.a_team_size++;
            work_queue.idle_a_team_workers++;
            work_queue.threads[id] = halide_spawn_thread(worker_thread, NULL);
            should_join_b_team = true;
            print(NULL) << "Making a new thread to run it\n";
        } else {
            // Uh...
            print(NULL) << "***** Out of threads!\n";
            abort();
        }

        // 3) If I should run it, do so, otherwise sleep on the appropriate condition variable.
        if (job && should_run_job) {
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
            if (job != owned_job && !job->running()) {
                print(NULL) << "Finished " << (job ?
                                               (job->task.name ? job->task.name : "unnamed") :
                                               "none") << "\n";
                halide_cond_broadcast(&work_queue.wakeup_owners);
                if (job->owner_is_on_b_team) {
                    // Take the owner's place on the B team to preserve the active thread count.
                    sleep_on_b_team(owned_job);
                }
            }
        } else {
            if (job) {
                // We can't run it. Give up our claim on it.
                job->release();
            }
            if (should_join_b_team) {
                sleep_on_b_team(owned_job);
            } else {
                sleep_on_a_team(owned_job);
            }
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
        halide_cond_init(&work_queue.wakeup_owners);
        halide_cond_init(&work_queue.wakeup_a_team);
        halide_cond_init(&work_queue.wakeup_b_team);
        work_queue.jobs = NULL;

        // Compute the desired number of threads to use. Other code
        // can also mess with this value, but only when the work queue
        // is locked.
        if (!work_queue.desired_num_threads) {
            work_queue.desired_num_threads = default_desired_num_threads();
        }
        work_queue.desired_num_threads = clamp_num_threads(work_queue.desired_num_threads);
        work_queue.threads_created = 0;

        // Everyone starts on the a team.
        work_queue.a_team_size = work_queue.desired_num_threads;
        work_queue.b_team_size = 0;
        work_queue.idle_a_team_workers = 0;
        work_queue.idle_b_team_workers = 0;
        work_queue.sleeping_a_team_owners = 0;
        work_queue.sleeping_b_team_owners = 0;

        work_queue.initialized = true;
    }

    while (work_queue.threads_created < work_queue.desired_num_threads - 1) {
        // We might need to make some new threads, if work_queue.desired_num_threads has
        // increased.
        work_queue.threads[work_queue.threads_created++] =
            halide_spawn_thread(worker_thread, NULL);
        work_queue.idle_a_team_workers++;
    }

    int size = 0;
    for (int i = 0; i < num_jobs; i++) {
        size += jobs[i].task.extent;
    }

    if (!work_queue.jobs && size < work_queue.desired_num_threads) {
        // If there's no nested parallelism happening and there are
        // fewer tasks to do than threads, then set the target A team
        // size so that some threads will put themselves to sleep
        // until a larger job arrives.
        work_queue.target_a_team_size = size;
    } else {
        // Otherwise the target A team size is
        // desired_num_threads. This may still be less than
        // threads_created if desired_num_threads has been reduced by
        // other code.
        work_queue.target_a_team_size = work_queue.desired_num_threads;
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

    // Wake up our A team.
    halide_cond_broadcast(&work_queue.wakeup_a_team);

    // If there are fewer threads than we would like on the a team,
    // wake up the b team too.
    if (work_queue.target_a_team_size > work_queue.a_team_size) {
        halide_cond_broadcast(&work_queue.wakeup_b_team);
    }

    // TODO: What if all threads are owners sleeping on runnable jobs?
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
    job.task.may_block = false; // May only call do_par_for if it there are no inner forks or acquires.
    job.task.serial = false;
    job.task.semaphore = NULL;
    job.task.closure = closure;
    job.task.depth = 0; // TODO: We actually need this information!
    job.task.name = NULL;
    job.user_context = user_context;
    job.exit_status = 0;
    job.active_workers = 0;
    job.owner_is_on_b_team = false;
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
        jobs[i].owner_is_on_b_team = false;
        jobs[i].owner_is_sleeping = false;
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
    int old = work_queue.desired_num_threads;
    work_queue.desired_num_threads = clamp_num_threads(n);
    halide_mutex_unlock(&work_queue.mutex);
    return old;
}

WEAK void halide_shutdown_thread_pool() {
    if (!work_queue.initialized) return;

    // Wake everyone up and tell them the party's over and it's time
    // to go home
    halide_mutex_lock(&work_queue.mutex);
    work_queue.shutdown = true;
    halide_cond_broadcast(&work_queue.wakeup_owners);
    halide_cond_broadcast(&work_queue.wakeup_a_team);
    halide_cond_broadcast(&work_queue.wakeup_b_team);
    halide_mutex_unlock(&work_queue.mutex);

    // Wait until they leave
    for (int i = 0; i < work_queue.threads_created; i++) {
        halide_join_thread(work_queue.threads[i]);
    }

    // Tidy up
    halide_mutex_destroy(&work_queue.mutex);
    halide_cond_destroy(&work_queue.wakeup_owners);
    halide_cond_destroy(&work_queue.wakeup_a_team);
    halide_cond_destroy(&work_queue.wakeup_b_team);
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
    return new_val;
}

WEAK bool halide_semaphore_try_acquire(halide_semaphore_t *s) {
    halide_semaphore_impl_t *sem = (halide_semaphore_impl_t *)s;
    // Decrement and get new value
    int new_val = __sync_add_and_fetch(&(sem->value), -1);
    if (new_val < 0) {
        // Oops, increment and return failure
        __sync_add_and_fetch(&(sem->value), 1);
        return false;
    } else {
        return true;
    }
}

}
