#include "Halide.h"
#include "halide_benchmark.h"

using namespace Halide;
using namespace Halide::Tools;

double run_test(bool auto_schedule) {
    /* THE ALGORITHM */

    // This implements the basic description of non-local means found at
    // https://en.wikipedia.org/wiki/Non-local_means

    int W = 1536;
    int H = 2560;
    Buffer<float> img(W, H, 3);

    for (int y = 0; y < img.height(); y++) {
        for (int x = 0; x < img.width(); x++) {
            for (int c = 0; c < 3; c++) {
                img(x, y, c) = rand() & 0xfff;
            }
        }
    }

    /*int patch_size = 7;
    int search_area = 7;
    float sigma = 0.12f;*/

    Param<int> patch_size;
    Param<int> search_area;
    Param<float> sigma;

    patch_size.set_range(2, 10);

    ImageParam input(Float(32), 3, "input");

    Var x("x"), y("y"), c("c");

    Expr inv_sigma_sq = -1.0f/(sigma*sigma*patch_size*patch_size);

    // Add a boundary condition
    Func clamped = BoundaryConditions::repeat_edge(input);

    // Define the difference images
    Var dx("dx"), dy("dy");
    Func dc("d");
    dc(x, y, dx, dy, c) = pow(clamped(x, y, c) - clamped(x + dx, y + dy, c), 2);

    // Sum across color channels
    RDom channels(0, 3);
    Func d("d");
    d(x, y, dx, dy) = sum(dc(x, y, dx, dy, channels));

    // Find the patch differences by blurring the difference images
    RDom patch_dom(-patch_size/2, patch_size);
    Func blur_d_y("blur_d_y");
    //blur_d_y(x, y, dx, dy) = sum(d(x, y + patch_dom, dx, dy));

    Func blur_d("blur_d");
    //blur_d(x, y, dx, dy) = sum(blur_d_y(x + patch_dom, y, dx, dy));
    blur_d(x, y, dx, dy) = sum(d(x, y + patch_dom, dx, dy));

    // Compute the weights from the patch differences
    Func w("w");
    w(x, y, dx, dy) = fast_exp(blur_d(x, y, dx, dy)*inv_sigma_sq);

    // Add an alpha channel
    Func clamped_with_alpha("clamped_with_alpha");
    clamped_with_alpha(x, y, c) = select(c == 0, clamped(x, y, 0),
                                         c == 1, clamped(x, y, 1),
                                         c == 2, clamped(x, y, 2),
                                         1.0f);

    // Define a reduction domain for the search area
    RDom s_dom(-search_area/2, search_area, -search_area/2, search_area);

    // Compute the sum of the pixels in the search area
    Func non_local_means_sum("non_local_means_sum");
    non_local_means_sum(x, y, c) += w(x, y, s_dom.x, s_dom.y) * clamped_with_alpha(x + s_dom.x, y + s_dom.y, c);

    Func non_local_means("non_local_means");
    non_local_means(x, y, c) =
        clamp(non_local_means_sum(x, y, c) / non_local_means_sum(x, y, 3), 0.0f, 1.0f);

    /* THE SCHEDULE */

    // Require 3 channels for output
    non_local_means.output_buffer().dim(2).set_bounds(0, 3);

    Var tx("tx"), ty("ty"), xi, yi;

    Target target = get_target_from_environment();
    std::cout << "Target: " << target.to_string() << "\n";
    Pipeline p(non_local_means);

    if (auto_schedule) {
        // Provide estimates on the input image
        input.dim(0).set_bounds_estimate(0, 614);
        input.dim(1).set_bounds_estimate(0, 1024);
        input.dim(2).set_bounds_estimate(0, 3);
        // Provide estimates on the parameters
        patch_size.set_estimate(7);
        search_area.set_estimate(7);
        sigma.set_estimate(0.12f);
        // Provide estimates on the output pipeline
        non_local_means.estimate(x, 0, 614)
            .estimate(y, 0, 1024)
            .estimate(c, 0, 3);
        // Auto-schedule the pipeline
        Pipeline p(non_local_means);
        p.auto_schedule(target);
    } else if (target.has_gpu_feature()) {
        std::cout << "*******TARGET GPU********\n";
        non_local_means.compute_root()
            .reorder(c, x, y).unroll(c)
            .gpu_tile(x, y, xi, yi, 16, 8);
        d.compute_at(non_local_means_sum, s_dom.x)
            .tile(x, y, xi, yi, 2, 2)
            .unroll(xi)
            .unroll(yi)
            .gpu_threads(x, y);
        /*blur_d_y.compute_at(non_local_means_sum, s_dom.x)
            .unroll(x, 2).gpu_threads(x, y);*/
        blur_d.compute_at(non_local_means_sum, s_dom.x)
            .gpu_threads(x, y);
        non_local_means_sum.compute_at(non_local_means, x)
            .gpu_threads(x, y)
            .update()
            .reorder(x, y, c, s_dom.x, s_dom.y)
            .gpu_threads(x, y);
    } else {
        non_local_means.compute_root()
            .reorder(c, x, y)
            .tile(x, y, tx, ty, x, y, 16, 8)
            .parallel(ty)
            .vectorize(x, 8);
        /*blur_d_y.compute_at(non_local_means, tx)
            .reorder(y, x)
            .vectorize(x, 8);*/
        d.compute_at(non_local_means, tx)
            .vectorize(x, 8);
        non_local_means_sum.compute_at(non_local_means, x)
            .reorder(c, x, y)
            .bound(c, 0, 4).unroll(c)
            .vectorize(x, 8);
        non_local_means_sum.update(0)
            .reorder(c, x, y, s_dom.x, s_dom.y)
            .unroll(c)
            .vectorize(x, 8);
        blur_d.compute_at(non_local_means_sum, x)
            .vectorize(x, 8);
    }

    //non_local_means.print_loop_nest();

    input.set(img);
    patch_size.set(7);
    search_area.set(7);
    sigma.set(0.12f);

    // Benchmark the schedule
    Buffer<float> out(img.width(), img.height(), img.channels());
    /*double t = benchmark(3, 10, [&]() {
        p.realize(out);
    });*/

    p.realize(out);
    out.copy_to_host();

    return 0;
}

int main(int argc, char **argv) {
    run_test(false);
    /*double auto_time = run_test(true);

    std::cout << "======================" << std::endl;
    std::cout << "Manual time: " << manual_time << "ms" << std::endl;
    std::cout << "Auto time: " << auto_time << "ms" << std::endl;
    std::cout << "======================" << std::endl;

    if (!get_target_from_environment().has_gpu_feature() &&
        (auto_time > manual_time * 3.5)) {
        printf("Auto-scheduler is much much slower than it should be.\n");
        return -1;
    }*/

    printf("Success!\n");
    return 0;
}
