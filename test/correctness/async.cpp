#include "Halide.h"

using namespace Halide;

extern "C" int expensive(int x) {
    float f = 3.0f;
    for (int i = 0; i < (1 << 10); i++) {
        f = sqrtf(sinf(cosf(f)));
    }
    if (f < 0) return 3;
    return x;
}
HalideExtern_1(int, expensive, int);

int main(int argc, char **argv) {

    // Basic compute-root async producer
    if (1) {
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

    // Sliding and folding over a single variable
    if (1) {
        Func producer("async_producer"), consumer;
        Var x, y;

        producer(x) = expensive(x);
        consumer(x) = expensive(producer(x) + producer(x-1));
        consumer.compute_root();
        producer.store_root().fold_storage(x, 8).compute_at(consumer, x);

        Buffer<int> out = consumer.realize(16);

        out.for_each_element([&](int x) {
                int correct = 2*x - 1;
                if (out(x) != correct) {
                    printf("out(%d) = %d instead of %d\n",
                           x, out(x), correct);
                    exit(-1);
                }
            });
    }

    // Sliding and folding over y
    if (1) {
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
    if (1) {
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
    if (1) {
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
    if (1) {
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

        producer_1(x, y) = expensive(x);
        producer_2(x, y) = expensive(y);
        // Use different stencils to get different fold factors.
        consumer(x, y) = expensive((producer_1(x-1, y) + producer_1(x+1, y) +
                                    producer_2(x-2, y) + producer_2(x+2, y)));

        producer_1.compute_at(consumer, x).store_at(consumer, y);
        producer_2.compute_at(consumer, x).store_at(consumer, y);
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

    // Nested asynchronous tasks.
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

    // Two async producer-consumer pairs over x in a producer-consumer
    // relationship over y. TODO: Currently generates junk IR w.r.t. semaphores.
    if (1) {
        Func producer_1("async_producer_1");
        Func consumer_1("async_consumer_1");
        Func producer_2("async_producer_2");
        Func consumer_2("consumer_2");

        Var x, y;

        producer_1(x, y) = x + y;
        consumer_1(x, y) = producer_1(x-1, y) + producer_1(x+1, y);
        producer_2(x, y) = consumer_1(x, y-1) + consumer_1(x, y+1);
        consumer_2(x, y) = producer_2(x-1, y) + producer_2(x+1, y);

        consumer_2.compute_root();
        producer_2.store_at(consumer_2, y).compute_at(consumer_2, x);
        consumer_1.store_root().compute_at(consumer_2, y);
        producer_1.store_at(consumer_2, y).compute_at(consumer_1, x);

        Buffer<int> out = consumer_2.realize(16, 16);
        out.for_each_element([&](int x, int y) {
                int correct = 8*(x + y);
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
