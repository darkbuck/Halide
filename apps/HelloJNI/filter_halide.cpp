#include "Halide.h"

using namespace Halide;

int main(int argc, char **argv) {
    Param<float> time;

    const float pi = 3.1415926536f;

    Var x, y, c;
    Func result{"result"};

    Expr kx, ky;
    Expr xx, yy;
    kx = x / 150.0f;
    ky = y / 150.0f;

    xx = kx + sin(time/3.0f);
    yy = ky + sin(time/2.0f);

    Expr angle;
    angle = 2 * pi * sin(time/20.0f);
    kx = kx * cos(angle) - ky * sin(angle);
    ky = kx * sin(angle) + ky * cos(angle);

    Expr v = 0.0f;
    v += sin((ky + time) / 2.0f);
    v += sin((kx + ky + time) / 2.0f);
    v += sin(sqrt(xx * xx + yy * yy + 1.0f) + time);

    result(x, y, c) = cast<uint8_t>(
        select(c == 0, 32,
               select(c == 1, cos(pi * v),
                      sin(pi * v)) * 80 + (255 - 80)));

    //result.output_buffer().set_stride(0, 4);
    //result.bound(c, 0, 4);

    // TODO(zstern): This forces namespaces to match.
    result.compile_to_file("filter", {time}, "MyFilter::filter");

    // TODO(zstern): float args don't have the proper name.
    // namespaces should be Capitalized.
    // filename has to match namespaces and need directories to match the
    // package path.

    // 1. first argument is "prefix". This needs to be a fully qualified Java
    // class, including a package name. 'prefix' is currently a filename.
    // 2. The final argument should reference
    //
    // java-side method gets the same name as the C function with all names
    // stripped off.
    // or always call it "apply()" and if you have a collection of them, use
    // packages.
    //result.compile_to_jni("org.halide.MyFilter", {time}, "MyFilter::filter");
    result.compile_to_jni("MyFilter", {time}, "MyFilter::filter");

    return 0;
}
