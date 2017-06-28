#include <HalideRuntime.h>
#include <qurt.h>
#include <stdlib.h>

int halide_host_cpu_count() {
    // Assume a Snapdragon 820
    return 4;
}

void halide_mutex_init(halide_mutex *mutex);

struct halide_cond {
    uint64_t _private[8];
};

void halide_cond_init(struct halide_cond *cond);
void halide_cond_destroy(struct halide_cond *cond);
void halide_cond_broadcast(struct halide_cond *cond);
void halide_cond_wait(struct halide_cond *cond, struct halide_mutex *mutex);


#define WEAK
#include "../thread_pool_common.h"

namespace {
// We wrap the closure passed to jobs with extra info we
// need. Currently just the hvx mode to use.
struct wrapped_closure {
    uint8_t *closure;
    int hvx_mode;
};
}

extern "C" {

// There are two locks at play: the thread pool lock and the hvx
// context lock. To ensure there's no way anything could ever
// deadlock, we never attempt to acquire one while holding the
// other.

int halide_do_par_for(void *user_context,
                      halide_task_t task,
                      int min, int size, uint8_t *closure) {
    if (!work_queue.initialized) {
        // The thread pool asssumes that a zero-initialized mutex can
        // be locked. Not true on hexagon, and there doesn't seem to
        // be an init_once mechanism either. In this shim binary, it's
        // safe to assume that the first call to halide_do_par_for is
        // done by the main thread, so there's no race condition on
        // initializing this mutex.
        halide_mutex_init(&work_queue.mutex);
    }
    wrapped_closure c = {closure, qurt_hvx_get_mode()};

    // Set the desired number of threads based on the current HVX
    // mode.
    int old_num_threads =
        halide_set_num_threads((c.hvx_mode == QURT_HVX_MODE_128B) ? 2 : 4);

    // We're about to acquire the thread-pool lock, so we must drop
    // the hvx context lock, even though we'll likely reacquire it
    // immediately to do some work on this thread.
    if (c.hvx_mode != -1) {
        // The docs say that qurt_hvx_get_mode should return -1 when
        // "not available". However, it appears to actually return 0,
        // which is the value of QURT_HVX_MODE_64B!  This means that
        // if we enter a do_par_for with HVX unlocked, we will leave
        // it with HVX locked in 64B mode, which then never gets
        // unlocked (a major bug).

        // To avoid this, we need to know if we are actually locked in
        // 64B mode, or not locked. To do this, we can look at the
        // return value of qurt_hvx_unlock, which returns an error if
        // we weren't already locked.
        if (qurt_hvx_unlock() != QURT_EOK) {
            c.hvx_mode = -1;
        }
    }
    int ret = halide_default_do_par_for(user_context, task, min, size, (uint8_t *)&c);
    if (c.hvx_mode != -1) {
        qurt_hvx_lock((qurt_hvx_mode_t)c.hvx_mode);
    }

    // Set the desired number of threads back to what it was, in case
    // we're a 128 job and we were sharing the machine with a 64 job.
    halide_set_num_threads(old_num_threads);
    return ret;
}

int halide_do_task(void *user_context, halide_task_t f,
                   int idx, uint8_t *closure) {
    // Dig the appropriate hvx mode out of the wrapped closure and lock it.
    wrapped_closure *c = (wrapped_closure *)closure;
    // We don't own the thread-pool lock here, so we can safely
    // acquire the hvx context lock (if needed) to run some code.
    if (c->hvx_mode != -1) {
        qurt_hvx_lock((qurt_hvx_mode_t)c->hvx_mode);
        int ret = f(user_context, idx, c->closure);
        qurt_hvx_unlock();
        return ret;
    } else {
        return f(user_context, idx, c->closure);
    }
}

}
