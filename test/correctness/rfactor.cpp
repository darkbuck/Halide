#include "Halide.h"
#include <assert.h>
#include <stdio.h>
#include <algorithm>
#include <functional>
#include <map>
#include <numeric>

using std::map;
using std::vector;
using std::string;

using namespace Halide;
using namespace Halide::Internal;

typedef map<string, vector<string>> CallGraphs;

class CheckCalls : public IRVisitor {
public:
    CallGraphs calls; // Caller -> vector of callees
    string producer = "";
private:
    using IRVisitor::visit;

    void visit(const ProducerConsumer *op) {
        string old_producer = producer;
        producer = op->name;
        //debug(0) << "*******FOUND producer: " << producer << "\n";
        calls[producer]; // Make sure each producer is allocated a slot
        op->produce.accept(this);
        producer = old_producer;

        if (op->update.defined()) {
            // Just lump all the update stages together
            producer = op->name + ".update(" + std::to_string(0) + ")";
            //debug(0) << "*******FOUND producer: " << producer << "\n";
            calls[producer]; // Make sure each producer is allocated a slot
            op->update.accept(this);
            producer = old_producer;
        }
        op->consume.accept(this);
        producer = old_producer;
    }

    void visit(const Load *op) {
        IRVisitor::visit(op);
        if (!producer.empty()) {
            assert(calls.count(producer) > 0);
            vector<string> &callees = calls[producer];
            if(std::find(callees.begin(), callees.end(), op->name) == callees.end()) {
                //debug(0) << "******producer: " << producer << ", consumer: " << op->name << "\n";
                callees.push_back(op->name);
            }
        }
    }
};


int check_call_graphs(CallGraphs &result, CallGraphs &expected) {
    if (result.size() != expected.size()) {
        printf("Expect %d callers instead of %d\n", (int)expected.size(), (int)result.size());
        return -1;
    }
    for (auto &iter : expected) {
        if (result.count(iter.first) == 0) {
            printf("Expect %s to be in the call graphs\n", iter.first.c_str());
            return -1;
        }
        vector<string> &expected_callees = iter.second;
        vector<string> &result_callees = result[iter.first];
        std::sort(expected_callees.begin(), expected_callees.end());
        std::sort(result_callees.begin(), result_callees.end());
        if (expected_callees != result_callees) {
            string expected_str = std::accumulate(
                expected_callees.begin(), expected_callees.end(), std::string{},
                [](const string &a, const string &b) {
                    return a.empty() ? b : a + ", " + b;
                });
            string result_str = std::accumulate(
                result_callees.begin(), result_callees.end(), std::string{},
                [](const string &a, const string &b) {
                    return a.empty() ? b : a + ", " + b;
                });

            printf("Expect calless of %s to be (%s); got (%s) instead\n",
                    iter.first.c_str(), expected_str.c_str(), result_str.c_str());
            return -1;
        }

    }
    return 0;
}

int check_image(const Image<int> &im, const std::function<int(int,int,int)> &func) {
    for (int z = 0; z < im.channels(); z++) {
        for (int y = 0; y < im.height(); y++) {
            for (int x = 0; x < im.width(); x++) {
                int correct = func(x, y, z);
                if (im(x, y, z) != correct) {
                    printf("im(%d, %d, %d) = %d instead of %d\n",
                           x, y, z, im(x, y, z), correct);
                    return -1;
                }
            }
        }
    }
    return 0;
}

int simple_rfactor_test(bool compile_module) {
    Func f("f"), g("g");
    Var x("x"), y("y");

    f(x, y) = x + y;
    f.compute_root();

    g(x, y) = 1;
    RDom r(10, 20, 30, 40);
    g(r.x , r.y) += f(r.x, r.y);

    Var u("u");
    Func intm = g.update(0).rfactor({{r.y, u}});
    intm.compute_root();
    intm.vectorize(u, 8);
    intm.update(0).vectorize(r.x, 2);

    if (compile_module) {
        // Check the call graphs.
        Module m = g.compile_to_module({g.infer_arguments()});
        CheckCalls checker;
        m.functions().front().body.accept(&checker);

        CallGraphs expected = {
            {g.name(), {}},
            {g.update(0).name(), {intm.name(), g.name()}},
            {intm.name(), {}},
            {intm.update(0).name(), {f.name(), intm.name()}},
            {f.name(), {}},
        };
        if (check_call_graphs(checker.calls, expected) != 0) {
            return -1;
        }
    } else {
        Image<int> im = g.realize(80, 80);
        auto func = [](int x, int y, int z) {
            return (10 <= x && x <= 29) && (30 <= y && y <= 69) ? x + y + 1 : 1;
        };
        if (check_image(im, func)) {
            return -1;
        }
    }
    return 0;
}

int reorder_split_rfactor_test(bool compile_module) {
    Func f("f"), g("g");
    Var x("x"), y("y");

    RDom r(10, 20, 20, 30);

    f(x, y) = x - y;
    f.compute_root();

    g(x, y) = 1;
    g(r.x, r.y) += f(r.x, r.y);
    g.update(0).reorder({r.y, r.x});

    RVar rxi("rxi"), rxo("rxo");
    g.update(0).split(r.x, rxo, rxi, 2);

    Var u("u"), v("v");
    Func intm1 = g.update(0).rfactor({{rxo, u}, {r.y, v}});
    Func intm2 = g.update(0).rfactor({{r.y, v}});
    intm2.compute_root();
    intm1.compute_at(intm2, rxo);

    if (compile_module) {
        // Check the call graphs.
        Module m = g.compile_to_module({g.infer_arguments()});
        CheckCalls checker;
        m.functions().front().body.accept(&checker);

        CallGraphs expected = {
            {g.name(), {}},
            {g.update(0).name(), {intm2.name(), g.name()}},
            {intm2.name(), {}},
            {intm2.update(0).name(), {intm1.name(), intm2.name()}},
            {intm1.name(), {}},
            {intm1.update(0).name(), {f.name(), intm1.name()}},
            {f.name(), {}},
        };
        if (check_call_graphs(checker.calls, expected) != 0) {
            return -1;
        }
    } else {
        Image<int> im = g.realize(80, 80);
        auto func = [](int x, int y, int z) {
            return ((10 <= x && x <= 29) && (20 <= y && y <= 49)) ? x - y + 1 : 1;
        };
        if (check_image(im, func)) {
            return -1;
        }
    }
    return 0;
}

int reorder_fuse_wrapper_rfactor_test(bool compile_module) {
    Func f("f"), g("g");
    Var x("x"), y("y"), z("z");

    RDom r(5, 10, 5, 10, 5, 10);

    f(x, y, z) = x + y + z;
    g(x, y, z) = 1;
    g(r.x, r.y, r.z) += f(r.x, r.y, r.z);
    g.update(0).reorder({r.y, r.x});

    RVar rf("rf");
    g.update(0).fuse(r.x, r.y, rf);
    g.update(0).reorder({r.z, rf});

    Var u("u"), v("v");
    Func intm = g.update(0).rfactor({{r.z, u}});
    RVar rfi("rfi"), rfo("rfo");
    intm.update(0).split(rf, rfi, rfo, 2);

    Func wrapper = f.in(intm).compute_root();
    f.compute_root();

    if (compile_module) {
        // Check the call graphs.
        Module m = g.compile_to_module({g.infer_arguments()});
        CheckCalls checker;
        m.functions().front().body.accept(&checker);

        CallGraphs expected = {
            {g.name(), {}},
            {g.update(0).name(), {intm.name(), g.name()}},
            {wrapper.name(), {f.name()}},
            {intm.name(), {}},
            {intm.update(0).name(), {wrapper.name(), intm.name()}},
            {f.name(), {}},
        };
        if (check_call_graphs(checker.calls, expected) != 0) {
            return -1;
        }
    } else {
        Image<int> im = g.realize(20, 20, 20);
        auto func = [](int x, int y, int z) {
            return ((5 <= x && x <= 14) && (5 <= y && y <= 14) && (5 <= z && z <= 14)) ? x + y + z + 1 : 1;
        };
        if (check_image(im, func)) {
            return -1;
        }
    }
    return 0;
}

int non_trivial_lhs_rfactor_test(bool compile_module) {
    Func a("a"), b("b"), c("c");
    Var x("x"), y("y"), z("z");

    RDom r(5, 10, 5, 10, 5, 10);

    a(x, y, z) = x;
    b(x, y, z) = x + y;
    c(x, y, z) = x + y + z;

    a.compute_root();
    b.compute_root();
    c.compute_root();

    Image<int> im_ref(20, 20, 20);

    {
        Func f("f"), g("g");
        f(x, y) = 1;
        Expr x_clamped = clamp(a(r.x, r.y, r.z), 0, 19);
        Expr y_clamped = clamp(b(r.x, r.y, r.z), 0, 29);
        f(x_clamped, y_clamped) += c(r.x, r.y, r.z);
        f.compute_root();

        g(x, y, z) = 2*f(x, y);
        im_ref = g.realize(20, 20, 20);
    }

    {
        Func f("f"), g("g");
        f(x, y) = 1;
        Expr x_clamped = clamp(a(r.x, r.y, r.z), 0, 19);
        Expr y_clamped = clamp(b(r.x, r.y, r.z), 0, 29);
        f(x_clamped, y_clamped) += c(r.x, r.y, r.z);
        f.compute_root();

        g(x, y, z) = 2*f(x, y);

        Var u("u"), v("v");
        RVar rzi("rzi"), rzo("rzo");
        Func intm = f.update(0).rfactor({{r.x, u}, {r.y, v}});
        intm.update(0).split(r.z, rzo, rzi, 2);

        if (compile_module) {
            // Check the call graphs.
            Module m = g.compile_to_module({g.infer_arguments()});
            CheckCalls checker;
            m.functions().front().body.accept(&checker);

            CallGraphs expected = {
                {g.name(), {f.name()}},
                {f.name(), {}},
                {f.update(0).name(), {f.name(), intm.name()}},
                {intm.name(), {}},
                {intm.update(0).name(), {a.name(), b.name(), c.name(), intm.name()}},
                {a.name(), {}},
                {b.name(), {}},
                {c.name(), {}},
            };
            if (check_call_graphs(checker.calls, expected) != 0) {
                return -1;
            }
        } else {
            Image<int> im = g.realize(20, 20, 20);
            auto func = [im_ref](int x, int y, int z) {
                return im_ref(x, y, z);
            };
            if (check_image(im, func)) {
                return -1;
            }
        }
    }
    return 0;
}

int simple_rfactor_with_specialize_test(bool compile_module) {
    Func f("f"), g("g");
    Var x("x"), y("y");

    f(x, y) = x + y;
    f.compute_root();

    g(x, y) = 1;
    RDom r(10, 20, 30, 40);
    g(r.x , r.y) += f(r.x, r.y);

    Param<int> p;
    Var u("u");
    Func intm = g.update(0).specialize(p >= 10).rfactor({{r.y, u}});
    intm.compute_root();
    intm.vectorize(u, 8);
    intm.update(0).vectorize(r.x, 2);

    if (compile_module) {
        p.set(20);
        // Check the call graphs.
        Module m = g.compile_to_module({g.infer_arguments()});
        CheckCalls checker;
        m.functions().front().body.accept(&checker);

        CallGraphs expected = {
            {g.name(), {}},
            {g.update(0).name(), {f.name(), intm.name(), g.name()}},
            {intm.name(), {}},
            {intm.update(0).name(), {f.name(), intm.name()}},
            {f.name(), {}},
        };
        if (check_call_graphs(checker.calls, expected) != 0) {
            return -1;
        }
    } else {
        {
            p.set(0);
            Image<int> im = g.realize(80, 80);
            auto func = [](int x, int y, int z) {
                return (10 <= x && x <= 29) && (30 <= y && y <= 69) ? x + y + 1 : 1;
            };
            if (check_image(im, func)) {
                return -1;
            }
        }
        {
            p.set(20);
            Image<int> im = g.realize(80, 80);
            auto func = [](int x, int y, int z) {
                return (10 <= x && x <= 29) && (30 <= y && y <= 69) ? x + y + 1 : 1;
            };
            if (check_image(im, func)) {
                return -1;
            }
        }
    }
    return 0;
}

int rdom_with_predicate_rfactor_test(bool compile_module) {
    Func f("f"), g("g");
    Var x("x"), y("y"), z("z");

    f(x, y, z) = x + y + z;
    f.compute_root();

    g(x, y, z) = 1;
    RDom r(5, 10, 5, 10, 0, 20);
    r.where(r.x < r.y);
    r.where(r.x + 2*r.y <= r.z);
    g(r.x, r.y, r.z) += f(r.x, r.y, r.z);

    Var u("u"), v("v");
    Func intm = g.update(0).rfactor({{r.y, u}, {r.x, v}});
    intm.compute_root();
    Var ui("ui"), vi("vi"), t("t");
    intm.tile(u, v, ui, vi, 2, 2).fuse(u, v, t).parallel(t);
    intm.update(0).vectorize(r.z, 2);

    if (compile_module) {
        // Check the call graphs.
        Module m = g.compile_to_module({g.infer_arguments()});
        CheckCalls checker;
        m.functions().front().body.accept(&checker);

        CallGraphs expected = {
            {g.name(), {}},
            {g.update(0).name(), {intm.name(), g.name()}},
            {intm.name(), {}},
            {intm.update(0).name(), {f.name(), intm.name()}},
            {f.name(), {}},
        };
        if (check_call_graphs(checker.calls, expected) != 0) {
            return -1;
        }
    } else {
        Image<int> im = g.realize(20, 20, 20);
        auto func = [](int x, int y, int z) {
            return (5 <= x && x <= 14) && (5 <= y && y <= 14) && (0 <= z && z <= 19) && (x < y) && (x + 2*y <= z) ? x + y + z + 1 : 1;
        };
        if (check_image(im, func)) {
            return -1;
        }
    }
    return 0;
}

int histogram_rfactor_test(bool compile_module) {
    int W = 128, H = 128;

    // Compute a random image and its true histogram
    int reference_hist[256];
    for (int i = 0; i < 256; i++) {
        reference_hist[i] = 0;
    }

    Image<float> in(W, H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            in(x, y) = float(rand() & 0x000000ff);
            reference_hist[uint8_t(in(x, y))] += 1;
        }
    }

    Func hist("hist"), g("g");
    Var x("x");

    RDom r(in);
    hist(x) = 0;
    hist(clamp(cast<int>(in(r.x, r.y)), 0, 255)) += 1;
    hist.compute_root();

    Var u("u");
    Func intm = hist.update(0).rfactor({{r.y, u}});
    intm.compute_root();
    intm.update(0).parallel(u);

    g(x) = hist(x+10);

    if (compile_module) {
        // Check the call graphs.
        Module m = g.compile_to_module({g.infer_arguments()});
        CheckCalls checker;
        m.functions().front().body.accept(&checker);

        CallGraphs expected = {
            {g.name(), {hist.name()}},
            {hist.name(), {}},
            {hist.update(0).name(), {intm.name(), hist.name()}},
            {intm.name(), {}},
            {intm.update(0).name(), {in.name(), intm.name()}},

        };
        if (check_call_graphs(checker.calls, expected) != 0) {
            return -1;
        }
    } else {
        Image<int32_t> histogram = g.realize(10); // buckets 10-20 only
        for (int i = 10; i < 20; i++) {
            if (histogram(i-10) != reference_hist[i]) {
                printf("Error: bucket %d is %d instead of %d\n", i, histogram(i), reference_hist[i]);
                return -1;
            }
        }
    }
    return 0;
}

int parallel_dot_product_rfactor_test(bool compile_module) {
    int size = 1024;

    Func f("f"), g("g"), a("a"), b("b");
    Var x("x");

    a(x) = x;
    b(x) = x+2;
    a.compute_root();
    b.compute_root();

    RDom r(0, size);

    Func dot_ref("dot");
    dot_ref() = 0;
    dot_ref() += a(r.x)*b(r.x);
    Image<int32_t> ref = dot_ref.realize();

    Func dot("dot");
    dot() = 0;
    dot() += a(r.x)*b(r.x);
    RVar rxo("rxo"), rxi("rxi");
    dot.update(0).split(r.x, rxo, rxi, 128);

    Var u("u");
    Func intm1 = dot.update(0).rfactor({{rxo, u}});
    RVar rxio("rxio"), rxii("rxii");
    intm1.compute_root();
    intm1.update(0).parallel(u);
    intm1.update(0).split(rxi, rxio, rxii, 8);

    Var v("v");
    Func intm2 = intm1.update(0).rfactor({{rxii, v}});
    intm2.compute_at(intm1, u);
    intm2.update(0).vectorize(v, 8);

    Image<int32_t> im = dot.realize();

    if (compile_module) {
        // Check the call graphs.
        Module m = dot.compile_to_module({dot.infer_arguments()});
        CheckCalls checker;
        m.functions().front().body.accept(&checker);

        CallGraphs expected = {
            {dot.name(), {}},
            {dot.update(0).name(), {intm1.name(), dot.name()}},
            {intm1.name(), {}},
            {intm1.update(0).name(), {intm2.name(), intm1.name()}},
            {intm2.name(), {}},
            {intm2.update(0).name(), {a.name(), b.name(), intm2.name()}},
            {a.name(), {}},
            {b.name(), {}},
        };
        if (check_call_graphs(checker.calls, expected) != 0) {
            return -1;
        }
    } else {
        Image<int32_t> im = dot.realize();
        if (ref(0) != im(0)) {
            printf("result = %d instead of %d\n", im(0), ref(0));
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    printf("Running simple rfactor test\n");
    printf("    checking call graphs...\n");
    if (simple_rfactor_test(true) != 0) {
        return -1;
    }
    printf("    checking output img correctness...\n");
    if (simple_rfactor_test(false) != 0) {
        return -1;
    }

    printf("Running reorder split rfactor test\n");
    printf("    checking call graphs...\n");
    if (reorder_split_rfactor_test(true) != 0) {
        return -1;
    }
    printf("    checking output img correctness...\n");
    if (reorder_split_rfactor_test(false) != 0) {
        return -1;
    }

    printf("Running reorder fuse wrapper rfactor test\n");
    printf("    checking call graphs...\n");
    if (reorder_fuse_wrapper_rfactor_test(true) != 0) {
        return -1;
    }
    printf("    checking output img correctness...\n");
    if (reorder_fuse_wrapper_rfactor_test(false) != 0) {
        return -1;
    }

    printf("Running non trivial lhs rfactor test\n");
    printf("    checking call graphs...\n");
    if (non_trivial_lhs_rfactor_test(true) != 0) {
        return -1;
    }
    printf("    checking output img correctness...\n");
    if (non_trivial_lhs_rfactor_test(false) != 0) {
        return -1;
    }

    printf("Running simple rfactor with specialization test\n");
    printf("    checking call graphs...\n");
    if (simple_rfactor_with_specialize_test(true) != 0) {
        return -1;
    }
    printf("    checking output img correctness...\n");
    if (simple_rfactor_with_specialize_test(false) != 0) {
        return -1;
    }

    printf("Running rdom with predicate rfactor test\n");
    printf("    checking call graphs...\n");
    if (rdom_with_predicate_rfactor_test(true) != 0) {
        return -1;
    }
    printf("    checking output img correctness...\n");
    if (rdom_with_predicate_rfactor_test(false) != 0) {
        return -1;
    }

    printf("Running histogram rfactor test\n");
    printf("    checking call graphs...\n");
    if (histogram_rfactor_test(true) != 0) {
        return -1;
    }
    printf("    checking output img correctness...\n");
    if (histogram_rfactor_test(false) != 0) {
        return -1;
    }

    printf("Running parallel dot product rfactor test\n");
    printf("    checking call graphs...\n");
    if (parallel_dot_product_rfactor_test(true) != 0) {
        return -1;
    }
    printf("    checking output img correctness...\n");
    if (parallel_dot_product_rfactor_test(false) != 0) {
        return -1;
    }

    printf("Success!\n");
    return 0;
}
