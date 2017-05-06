#include <stdio.h>
#include "HalideRuntime.h"
#include "HalideBuffer.h"
#include <assert.h>

#include "async_memcpy.h"

struct memcpy_args {
    void *dst;
    void *src;
    size_t size;
};

void memcpy_on_thread(void *args) {
    struct memcpy_args *memcpy_args = (struct memcpy_args *)args;
    memcpy(memcpy_args->dst, memcpy_args->src, memcpy_args->size);
    delete(memcpy_args);
}

extern "C" int32_t async_memcpy_extern(halide_buffer_t *input, halide_buffer_t *output) {
    if (input->host == nullptr) {
        input->dimensions = 1;
        input->dim[0].extent = output->dim[0].extent;
        input->dim[0].stride = 1;
	input->type = halide_type_t(halide_type_uint, 8, 1);
    }

    memcpy_args *args = new memcpy_args();
    args->dst = output->host + output->dim[0].min;
    args->src = input->host + input->dim[0].min;
    args->size = output->dim[0].extent;
    halide_spawn_thread(memcpy_on_thread, args);

    return 0;
}

int main(int argc, char **argv) {
    using Halide::Runtime::Buffer;

    Buffer<uint8_t> input(1024 * 1024);
    Buffer<uint8_t> output(1024 * 1024);

    for (int i = 0; i < 1024 * 1024; i++) {
        input(i) = (uint8_t)(i * i);
    }

    int result = async_memcpy(input, output);
    assert(result == 0);

    for (int i = 0; i < 1024 * 1024; i++) {
        assert(input(i) == output(i));
    }

    printf("Success!\n");
    return 0;
}
