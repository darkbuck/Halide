#include "Halide.h"

// Include the machine-generated .stub.h header file.
#include "example.stub.h"

using Halide::Buffer;

const int kSize = 32;

void verify(const Buffer<int32_t> &img, float compiletime_factor, float runtime_factor, int channels) {
    img.for_each_element([=](int x, int y, int c) {
        int expected = (int32_t)(compiletime_factor * runtime_factor * c * (x > y ? x : y));
        int actual = img(x, y, c);
        assert(expected == actual);
    });
}

int main(int argc, char **argv) {
    Halide::JITGeneratorContext context(Halide::get_jit_target_from_environment());
    const float runtime_factor = 4.5f;

    {
        // Use the static "apply" method. The inputs are {in braces}.
        Halide::Buffer<int32_t> img = example::apply(context, {runtime_factor}).realize(kSize, kSize, 3);
        verify(img, 1.f, runtime_factor, 3);
    }

    {
        // Pass in non-default values for GeneratorParams. (Note that example::GeneratorParams
        // is initialized to the default values, so we usually prefer to specify just the
        // fields we want to change, and rarely specify this by {initializer-list}.)
        example::GeneratorParams gp;
        gp.compiletime_factor = 2.5f;

        Halide::Buffer<int32_t> img = example::apply(context, {runtime_factor}, gp).realize(kSize, kSize, 3);
        verify(img, gp.compiletime_factor, runtime_factor, 3);
    }

    {
        // We can also fill in the Inputs struct by name if we like.
        example::Inputs inputs;
        inputs.runtime_factor = runtime_factor;

        example::GeneratorParams gp;
        gp.compiletime_factor = 2.5f;

        Halide::Buffer<int32_t> img = example::apply(context, inputs, gp).realize(kSize, kSize, 3);
        verify(img, gp.compiletime_factor, runtime_factor, 3);
    }

    {
        // apply() actually returns an Outputs struct; in the previous examples,
        // we just called .realize() on it directly. In this case we'll save it
        // to a temporary so we can set some of its ScheduleParams.
        example::Outputs result = example::apply(context, {runtime_factor});

        // For purposes of example, don't vectorize or parallelize. (Note that
        // we can set ScheduleParams any time before we realize.)
        result.vectorize.set(false);
        result.parallelize.set(false);

        Halide::Buffer<int32_t> img = result.realize(kSize, kSize, 3);
        verify(img, 1.f, runtime_factor, 3);
    }

    {
        // If the Generator has a single Output<Func>, we provide an
        // overload of operator Func() so you can just do this (assuming
        // you don't care about setting any ScheduleParams):
        Halide::Func f = example::apply(context, {runtime_factor});

        Halide::Buffer<int32_t> img = f.realize(kSize, kSize, 3);
        verify(img, 1.f, runtime_factor, 3);
    }

#ifdef OLD_AND_MAYBE_DEPRECATED
    {
        // Create a Generator and set its Inputs and GeneratorParams.
        // We could just use initializer-list syntax, but we'll explicitly
        // set the fields by name for clarity.
        example::Inputs inputs;
        inputs.runtime_factor = runtime_factor;

        // The fields of the GeneratorParams struct are initialized to the
        // default values specified in the Generator, so we can just omit
        // any we don't want to change
        example::GeneratorParams gp;
        gp.compiletime_factor = 2.5f;
        gp.enummy = Enum_enummy::foo;
        // gp.channels = 3;  -- this is the default; no need to set

        auto gen = example(context, inputs, gp);

        // We must call schedule() before calling realize()
        gen.schedule();

        Halide::Buffer<int32_t> img = gen.realize(kSize, kSize, 3);
        verify(img, 2.5f, 1, 3);
    }

    {
        // Here, we'll use an initializer list for inputs, and omit
        // the GeneratorParams entirely to use their default values.
        auto gen = example(context, /* inputs: */ { 1.f });

        // We'll set "vectorize=false parallelize=false" in the ScheduleParams, just to
        // show that we can:
        gen.vectorize.set(false);
        gen.parallelize.set(false);
        gen.schedule();

        Halide::Buffer<int32_t> img(kSize, kSize, 3);
        gen.realize(img);
        verify(img, 1, 1, 3);
    }

    {
        auto gen = example(context, /* inputs: */ { 1.f });

        // Same as before, but we'll use chained setters for the ScheduleParams;
        // this is identical in function to the previous block, but a style that
        // some people prefer. Note that we can also chain the "schedule()"
        // call on the end.
        gen.set_vectorize(false).set_parallelize(false).schedule();

        Halide::Buffer<int32_t> img(kSize, kSize, 3);
        gen.realize(img);
        verify(img, 1, 1, 3);
    }
#endif

    printf("Success!\n");
    return 0;
}
