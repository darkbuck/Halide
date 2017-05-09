#include "Halide.h"

using namespace Halide;

extern "C" int expensive(int x) {
    usleep(1000);
    return x;
}
HalideExtern_1(int, expensive, int);

int main(int argc, char **argv) {

    // Basic compute-root async producer
    if (0) {
        Func producer("async_producer"), consumer;
        Var x, y;

        producer(x, y) = x + y;
        consumer(x, y) = expensive(producer(x-1, y-1) + producer(x+1, y+1));
        consumer.compute_root();
        producer.compute_root();

        Buffer<int> out = consumer.realize(16, 16);

        out.for_each_element([&](int x, int y) {
                int correct = 2*(x + y);
                if (out(x, y) != correct) {
                    printf("out(%d, %d) = %d instead of %d\n",
                           x, y, out(x, y), correct);
                    exit(-1);
                }
            });
    }

    // Sliding and folding over y
    if (0) {
        Func producer("async_producer"), consumer;
        Var x, y;

        producer(x, y) = x + y;
        consumer(x, y) = expensive(producer(x-1, y-1) + producer(x+1, y+1));
        consumer.compute_root();
        // Producer can run 5 scanlines ahead
        producer.store_root().fold_storage(y, 8).compute_at(consumer, y);

        Buffer<int> out = consumer.realize(16, 16);

        out.for_each_element([&](int x, int y) {
                int correct = 2*(x + y);
                if (out(x, y) != correct) {
                    printf("out(%d, %d) = %d instead of %d\n",
                           x, y, out(x, y), correct);
                    exit(-1);
                }
            });
    }

    // Sliding over x and y, folding over y
    if (0) {
        Func producer("async_producer"), consumer;
        Var x, y;

        producer(x, y) = x + y;
        consumer(x, y) = expensive(producer(x-1, y-1) + producer(x+1, y+1));
        consumer.compute_root();
        // Producer can still run 5 scanlines ahead
        producer.store_root().fold_storage(y, 8).compute_at(consumer, x);

        Buffer<int> out = consumer.realize(16, 16);

        out.for_each_element([&](int x, int y) {
                int correct = 2*(x + y);
                if (out(x, y) != correct) {
                    printf("out(%d, %d) = %d instead of %d\n",
                           x, y, out(x, y), correct);
                    exit(-1);
                }
            });
    }

    // Sliding over x, folding over x and y. Folding over multiple
    // dimensions implies separate semaphores for each dimension
    // folded to prevent clobbering along each axis. The outer
    // semaphore never actually does anything, because the inner
    // semaphore stops it from getting that far ahead.
    if (0) {
        Func producer("async_producer"), consumer;
        Var x, y;

        producer(x, y) = x + y;
        // No longer a stencil in y, so that multiple dimensions can be folded
        consumer(x, y) = expensive(producer(x-1, y) + producer(x+1, y));
        consumer.compute_root();
        // Producer can run 5 pixels ahead within each scanline, also
        // give it some slop in y so it can run ahead to do the first
        // few pixels of the next scanline while the producer is still
        // chewing on the previous one.

        // The producer doesn't run into the new scanline as much as
        // it could, because we're sharing one semaphore for x in
        // between the two scanlines, so we're a little conservative.
        producer.store_root().fold_storage(x, 8).fold_storage(y, 2).compute_at(consumer, x);

        Buffer<int> out = consumer.realize(16, 16);

        out.for_each_element([&](int x, int y) {
                int correct = 2*(x + y);
                if (out(x, y) != correct) {
                    printf("out(%d, %d) = %d instead of %d\n",
                           x, y, out(x, y), correct);
                    exit(-1);
                }
            });
    }

    // Multiple async producers at root. This doesn't currently get
    // the producers running at the same time, because one is nested
    // inside the other's consume node. Need to tighten this up.
    if (0) {
        Func producer_1("async_producer_1");
        Func producer_2("async_producer_2");
        Func consumer;
        Var x, y;

        producer_1(x, y) = x;
        producer_2(x, y) = y;
        // Use different stencils to get different fold factors.
        consumer(x, y) = (producer_1(x-1, y) + producer_1(x+1, y) +
                          producer_2(x-2, y) + producer_2(x+2, y));

        producer_1.compute_root();
        producer_2.compute_root();

        Buffer<int> out = consumer.realize(16, 16);
        out.for_each_element([&](int x, int y) {
                int correct = 2*(x + y);
                if (out(x, y) != correct) {
                    printf("out(%d, %d) = %d instead of %d\n",
                           x, y, out(x, y), correct);
                    exit(-1);
                }
            });
    }

    // Multiple async producers inside an outer parallel for loop
    if (0) {
        Func producer_1("async_producer_1");
        Func producer_2("async_producer_2");
        Func consumer;
        Var x, y;

        producer_1(x, y) = x;
        producer_2(x, y) = y;
        consumer(x, y) = (producer_1(x-1, y) + producer_1(x+1, y) +
                          producer_2(x-2, y) + producer_2(x+2, y));

        producer_1.compute_at(consumer, y);
        producer_2.compute_at(consumer, y);
        consumer.parallel(y);

        Buffer<int> out = consumer.realize(16, 16);
        out.for_each_element([&](int x, int y) {
                int correct = 2*(x + y);
                if (out(x, y) != correct) {
                    printf("out(%d, %d) = %d instead of %d\n",
                           x, y, out(x, y), correct);
                    exit(-1);
                }
            });
    }

    // Multiple async producers inside an outer parallel for loop
    // with sliding within the inner serial loop
    if (1) {
        Func producer_1("async_producer_1");
        Func producer_2("async_producer_2");
        Func consumer;
        Var x, y;

        producer_1(x, y) = x;
        producer_2(x, y) = y;
        // Use different stencils to get different fold factors.
        consumer(x, y) = (producer_1(x-1, y) + producer_1(x+1, y) +
                          producer_2(x-2, y) + producer_2(x+2, y));

        producer_1.compute_at(consumer, x).store_at(consumer, y);
        producer_2.compute_at(consumer, x).store_at(consumer, y);
        //consumer.parallel(y);

        Buffer<int> out = consumer.realize(16, 16);
        out.for_each_element([&](int x, int y) {
                int correct = 2*(x + y);
                if (out(x, y) != correct) {
                    printf("out(%d, %d) = %d instead of %d\n",
                           x, y, out(x, y), correct);
                    exit(-1);
                }
            });
    }

    // Nested asynchronous tasks. Currently deadlocks :(
    if (1) {
        Func f0("async_f0"), f1("async_f1"), f2;
        Var x, y;

        f0(x, y) = x + y;
        f1(x, y) = f0(x-1, y-1) + f0(x+1, y+1);
        f2(x, y) = f1(x-1, y-1) + f1(x+1, y+1);

        f2.compute_root();
        f1.compute_at(f2, y);
        f0.compute_at(f1, x);

        Buffer<int> out = f2.realize(16, 16);
        out.for_each_element([&](int x, int y) {
                int correct = 4*(x + y);
                if (out(x, y) != correct) {
                    printf("out(%d, %d) = %d instead of %d\n",
                           x, y, out(x, y), correct);
                    exit(-1);
                }
            });
    }

    printf("Success!\n");
    return 0;
}
