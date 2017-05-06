#include "HalideRuntime.h"
#include "device_interface.h"

namespace Halide { namespace Runtime { namespace Internal { namespace ThreadAsync {

extern WEAK halide_device_interface_t thread_async_device_interface;

}}}}

extern "C" {

WEAK const struct halide_device_interface_t *halide_thread_async_device_interface() {
    return &Halide::Runtime::Internal::ThreadAsync::thread_async_device_interface;
}

WEAK int halide_thread_async_device_malloc(void *user_context, halide_buffer_t *buffer) {
    return 0;
}

WEAK int halide_thread_async_device_free(void *user_context, halide_buffer_t *buffer) {
    if (buffer->device != 0 && buffer->device_dirty()) {
        halide_join_thread((halide_thread *)buffer->device);
    }
    return 0;
}

WEAK int halide_thread_async_device_sync(void *user_context, struct halide_buffer_t *buffer) {
    return 0;
}

WEAK int halide_thread_async_device_release(void *user_context) {
    return 0;
}

WEAK int halide_thread_async_copy_to_device(void *user_context, halide_buffer_t *buffer) {
    return 0;
}

WEAK int halide_thread_async_copy_to_host(void *user_context, halide_buffer_t *buffer) {
    if (buffer->device != 0) {
        halide_join_thread((halide_thread *)buffer->device);
    }
    return 0;
}

WEAK int halide_thread_async_run(void *user_context,
                           void *state_ptr,
                           const char *entry_name,
                           int blocksX, int blocksY, int blocksZ,
                           int threadsX, int threadsY, int threadsZ,
                           int shared_mem_bytes,
                           size_t arg_sizes[],
                           void *args[],
                           int8_t arg_is_buffer[],
                           int num_attributes,
                           float *vertex_buffer,
                           int num_coords_dim0,
                           int num_coords_dim1) {
    return 0;
}

WEAK int halide_thread_async_device_and_host_malloc(void *user_context, struct halide_buffer_t *buffer) {
    int result = halide_thread_async_device_malloc(user_context, buffer);
    return result;
}

WEAK int halide_thread_async_device_and_host_free(void *user_context, struct halide_buffer_t *buffer) {
    return 0;
}

namespace {
__attribute__((destructor))
WEAK void halide_thread_async_cleanup() {
    halide_thread_async_device_release(NULL);
}
}

} // extern "C" linkage

namespace Halide { namespace Runtime { namespace Internal { namespace ThreadAsync {
WEAK halide_device_interface_t thread_async_device_interface = {
    halide_use_jit_module,
    halide_release_jit_module,
    halide_thread_async_device_malloc,
    halide_thread_async_device_free,
    halide_thread_async_device_sync,
    halide_thread_async_device_release,
    halide_thread_async_copy_to_host,
    halide_thread_async_copy_to_device,
    halide_thread_async_device_and_host_malloc,
    halide_thread_async_device_and_host_free,
};

}}}} // namespace Halide::Runtime::Internal::AsyncMemcpyDevice
