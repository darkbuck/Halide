#include <iostream>
#include <thread>

#include "HalideRuntime.h"
#include "HalideBuffer.h"

#include "multiple_thread_pool.h"

static int32_t id_counter;

struct custom_context {
    int32_t uc_id;
    halide_thread_pool *thread_pool;

    custom_context() {
        uc_id = ++id_counter;
        thread_pool = halide_make_new_thread_pool(this);
    }

    ~custom_context() {
        halide_free_thread_pool(this, thread_pool);
    }
};

extern "C" {

struct halide_thread_pool *halide_get_thread_pool(void *user_context) {
    if (user_context == nullptr) {
        return halide_get_default_thread_pool(user_context);
    }

    custom_context *uc = (custom_context *)user_context;
    return uc->thread_pool;
}

thread_local int32_t thread_pool_uc_id = 0;

static void thread_trampoline(int32_t new_thread_pool_uc_id, void (*f)(void *), void *closure) {
    thread_pool_uc_id = new_thread_pool_uc_id;

    f(closure);
}


struct halide_thread *halide_spawn_thread(void *user_context, void (*f)(void *), void *closure) {
    int32_t uc_id = 0;
    if (user_context != nullptr) {
        custom_context *uc = (custom_context *)user_context;
        uc_id = uc->uc_id;
    }
    return (halide_thread *)new std::thread(thread_trampoline, uc_id, f, closure);
}

void halide_join_thread(struct halide_thread *arg) {
    std::thread *thread = (std::thread *)arg;
    thread->join();
    delete(thread);
}

int32_t check_thread(void *user_context) {
    int32_t uc_id = 0;
    if (user_context != nullptr) {
        custom_context *uc = (custom_context *)user_context;
        uc_id = uc->uc_id;
    }  
    assert(thread_pool_uc_id == uc_id);
    return uc_id;
}

} // close extern "C" block

int main(int argc, char **argv) {
    custom_context contexts[3];

    Halide::Runtime::Buffer<int32_t> in_buf(32, 32);
    Halide::Runtime::Buffer<int32_t> out_buf(32, 32);

    for (auto &p : in_buf) {
        p = 21;
    }

    thread_pool_uc_id = 0;
    multiple_thread_pool(nullptr, in_buf.raw_buffer(), 0, out_buf.raw_buffer());
    for (const auto &p : out_buf) {
        assert(p == 21);
    }

    thread_pool_uc_id = 1;
    multiple_thread_pool(&contexts[0], in_buf.raw_buffer(), 1, out_buf.raw_buffer());
    for (const auto &p : out_buf) {
        assert(p == 23);
    }

    thread_pool_uc_id = 2;
    multiple_thread_pool(&contexts[1], in_buf.raw_buffer(), 2, out_buf.raw_buffer());
    for (const auto &p : out_buf) {
        assert(p == 25);
    }

    thread_pool_uc_id = 3;
    multiple_thread_pool(&contexts[2], in_buf.raw_buffer(), 3, out_buf.raw_buffer());
    for (const auto &p : out_buf) {
        assert(p == 27);
    }

    printf("Success!\n");
    return 0;
}
