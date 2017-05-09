#include "printer.h"

namespace Halide { namespace Runtime { namespace Internal {

struct work {
    work *next_job;
    int (*f)(void *, int, uint8_t *);
    void *user_context;
    uint64_t id;
    int next, max;
    uint8_t *closure;
    int active_workers;
    int exit_status;
    int *semaphore;
    bool running() {
        return next < max || active_workers > 0;
    }
    bool make_runnable() {
        return semaphore == NULL || halide_semaphore_try_acquire(semaphore);
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
    int a_team_size, target_a_team_size;

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

    // Counts the number of tasks enqueued
    uint64_t job_id_counter;

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

WEAK void worker_thread_already_locked(work *owned_job) {
    // If I'm a job owner, then I was the thread that called
    // do_par_for, and I should only stay in this function until my
    // job is complete. If I'm a lowly worker thread, I should stay in
    // this function as long as the work queue is running.
    while (owned_job != NULL ? owned_job->running()
           : work_queue.running()) {

        #if 0
        print(NULL) << "Work queue:\n";
        for (work *j = work_queue.jobs; j; j = j->next_job) {
            if (j->semaphore) {
                print(NULL) << j << ", " << j->semaphore << ", " << *(j->semaphore) << "\n";
            } else {
                print(NULL) << j << "\n";
            }
        }
        print(NULL) << "NULL\n";
        #endif

        // Grab the next runnable job.
        work **prev_ptr = &work_queue.jobs;
        work *job = work_queue.jobs;
        while (job) {
            if (job->make_runnable() &&
               (!owned_job || job->id >= owned_job->id)) {
                // We can run this job.
                break;
            }
            prev_ptr = &job->next_job;
            job = job->next_job;
        }

        #if 0
        print(NULL) << "Next runnable job: " << job << "\n";
        #endif

        bool all_jobs_blocked = (job == NULL) && (work_queue.jobs);

        if (job == NULL) {
            if (owned_job) {
                // There are no runnable jobs pending. Wait for the last worker
                // to signal that the job is finished.
                print(NULL) << "WAIT: Master..."<< all_jobs_blocked << "\n";
                halide_cond_wait(&work_queue.wakeup_owners, &work_queue.mutex);
                print(NULL) << "AWAKE: Master...\n";
            } else if (work_queue.a_team_size <= work_queue.target_a_team_size) {
                // There are no jobs pending. Wait until more jobs are enqueued.
                print(NULL) << "WAIT: Worker..." << all_jobs_blocked << "\n";
                halide_cond_wait(&work_queue.wakeup_a_team, &work_queue.mutex);
                print(NULL) << "AWAKE: Worker...\n";
            } else {
                // There are no jobs pending, and there are too many
                // threads in the A team. Transition to the B team
                // until the wakeup_b_team condition is fired.
                print(NULL) << "WAIT: Worker..." << all_jobs_blocked << "\n";
                work_queue.a_team_size--;
                halide_cond_wait(&work_queue.wakeup_b_team, &work_queue.mutex);
                work_queue.a_team_size++;
                print(NULL) << "AWAKE: Worker...\n";
            }
        } else {
            // Claim a task from it.
            work myjob = *job;
            job->next++;

            // If there were no more tasks pending for this job,
            // remove it from the stack.
            if (job->next == job->max) {
                if (job != owned_job) {
                    print(NULL) << "CLAIMING LAST TASK ON SOMEONE ELSE'S JOB!\n";
                }
                *prev_ptr = job->next_job;
            }

            // Increment the active_worker count so that other threads
            // are aware that this job is still in progress even
            // though there are no outstanding tasks for it.
            job->active_workers++;

            print(NULL) << "BEGIN: " << job << ": " << myjob.next << "\n";

            // Release the lock and do the task.
            halide_mutex_unlock(&work_queue.mutex);
            int result = halide_do_task(myjob.user_context, myjob.f, myjob.next,
                                        myjob.closure);
            halide_mutex_lock(&work_queue.mutex);

            print(NULL) << "END: " << job << ": " << myjob.next << "\n";

            // If this task failed, set the exit status on the job.
            if (result) {
                job->exit_status = result;
            }

            // We are no longer active on this job
            job->active_workers--;

            // If the job is done and I'm not the owner of it, wake up
            // the owner.
            if (!job->running() && job != owned_job) {
                print(NULL) << "WAKING MASTER: " << job << "\n";
                halide_cond_broadcast(&work_queue.wakeup_owners);
            }
        }
    }
}

WEAK void worker_thread(void *) {
    halide_mutex_lock(&work_queue.mutex);
    worker_thread_already_locked(NULL);
    halide_mutex_unlock(&work_queue.mutex);
}


WEAK int enqueue_work_and_enter_thread_pool(void *user_context, halide_task_t f,
                                            int min, int size, int *semaphore, uint8_t *closure) {
    print(NULL) << "Locking mutex...\n";

    // Grab the lock. If it hasn't been initialized yet, then the
    // field will be zero-initialized because it's a static global.
    halide_mutex_lock(&work_queue.mutex);

    print(NULL) << "Enqueueing task: " << (void *)f << ", " << min << ", " << size << ", " << semaphore << "\n";

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

         work_queue.initialized = true;
     }

     while (work_queue.threads_created < work_queue.desired_num_threads - 1) {
        // We might need to make some new threads, if work_queue.desired_num_threads has
        // increased.
        work_queue.threads[work_queue.threads_created++] =
            halide_spawn_thread(worker_thread, NULL);
     }

    // Make the job.
    work job;
    job.f = f;               // The job should call this function. It takes an index and a closure.
    job.user_context = user_context;
    job.next = min;          // Start at this index.
    job.max  = min + size;   // Keep going until one less than this index.
    job.closure = closure;   // Use this closure.
    job.exit_status = 0;     // The job hasn't failed yet
    job.active_workers = 0;  // Nobody is working on this yet
    job.semaphore = semaphore;  // The job can't start until this is > 0
    job.id = work_queue.job_id_counter++; // Prevents work inversions that cause deadlocks

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

    // Push the job onto the stack.
    job.next_job = work_queue.jobs;
    work_queue.jobs = &job;

    // Wake up our A team.
    halide_cond_broadcast(&work_queue.wakeup_a_team);
    // Sleeping owners are also A-team threads.
    halide_cond_broadcast(&work_queue.wakeup_owners);

    // If there are fewer threads than we would like on the a team,
    // wake up the b team too.
    if (work_queue.target_a_team_size > work_queue.a_team_size) {
        halide_cond_broadcast(&work_queue.wakeup_b_team);
    }

    // Do some work myself.
    print(NULL) << "Master entering worker pool\n";
    worker_thread_already_locked(&job);
    print(NULL) << "Master leaving worker pool\n";

    halide_mutex_unlock(&work_queue.mutex);

    // Return zero if the job succeeded, otherwise return the exit
    // status of one of the failing jobs (whichever one failed last).
    return job.exit_status;
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
    return enqueue_work_and_enter_thread_pool(user_context, f, min, size, NULL, closure);
}

// TODO: Make this default_do_async and add the function pointer indirection junk
WEAK int halide_do_async_consumer(void *user_context, halide_task_t f, int *semaphore, uint8_t *closure) {
    return enqueue_work_and_enter_thread_pool(user_context, f, 0, 1, semaphore, closure);
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

}
