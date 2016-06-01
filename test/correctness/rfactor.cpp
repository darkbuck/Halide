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
        calls[producer]; // Make sure each producer is allocated a slot
        op->produce.accept(this);
        producer = old_producer;

        if (op->update.defined()) {
            // Just lump all the update stages together
            producer = op->name + ".update(" + std::to_string(0) + ")";
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

int simple_rfactor_test() {
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

    // Check the call graphs.
    // Expect 'g' to call 'wrapper', 'wrapper' to call 'f', 'f' to call nothing
    /*Module m = g.compile_to_module({});
    CheckCalls c;
    m.functions().front().body.accept(&c);

    CallGraphs expected = {
        {g.name(), {wrapper.name()}},
        {wrapper.name(), {f.name()}},
        {f.name(), {}},
    };
    if (check_call_graphs(c.calls, expected) != 0) {
        return -1;
    }*/

    Image<int> im = g.realize(80, 80);
    auto func = [](int x, int y, int z) {
        return (10 <= x && x <= 29) && (30 <= y && y <= 69) ? x + y + 1 : 1;
    };
    if (check_image(im, func)) {
        return -1;
    }
    return 0;
}

int reorder_split_rfactor_test() {
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

    // Check the call graphs.
    // Expect 'g' to call 'wrapper', 'wrapper' to call 'f', 'f' to call nothing
    /*Module m = g.compile_to_module({});
    CheckCalls c;
    m.functions().front().body.accept(&c);

    CallGraphs expected = {
        {g.name(), {wrapper.name()}},
        {wrapper.name(), {f.name()}},
        {f.name(), {}},
    };
    if (check_call_graphs(c.calls, expected) != 0) {
        return -1;
    }*/

    Image<int> im = g.realize(80, 80);
    auto func = [](int x, int y, int z) {
        return ((10 <= x && x <= 29) && (20 <= y && y <= 49)) ? x - y + 1 : 1;
    };
    if (check_image(im, func)) {
        return -1;
    }
    return 0;
}

int reorder_fuse_wrapper_rfactor_test() {
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
    Func intm1 = g.update(0).rfactor({{r.z, u}});
    RVar rfi("rfi"), rfo("rfo");
    intm1.update(0).split(rf, rfi, rfo, 2);

    Func wrapper = f.in(intm1).compute_root();
    f.compute_root();

    // Check the call graphs.
    // Expect 'g' to call 'wrapper', 'wrapper' to call 'f', 'f' to call nothing
    /*Module m = g.compile_to_module({});
    CheckCalls c;
    m.functions().front().body.accept(&c);

    CallGraphs expected = {
        {g.name(), {wrapper.name()}},
        {wrapper.name(), {f.name()}},
        {f.name(), {}},
    };
    if (check_call_graphs(c.calls, expected) != 0) {
        return -1;
    }*/

    Image<int> im = g.realize(20, 20, 20);
    auto func = [](int x, int y, int z) {
        return ((5 <= x && x <= 14) && (5 <= y && y <= 14) && (5 <= z && z <= 14)) ? x + y + z + 1 : 1;
    };
    if (check_image(im, func)) {
        return -1;
    }
    return 0;
}

int non_trivial_lhs_rfactor_test() {
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

        // Check the call graphs.
        // Expect 'g' to call 'wrapper', 'wrapper' to call 'f', 'f' to call nothing
        /*Module m = g.compile_to_module({});
        CheckCalls c;
        m.functions().front().body.accept(&c);

        CallGraphs expected = {
            {g.name(), {wrapper.name()}},
            {wrapper.name(), {f.name()}},
            {f.name(), {}},
        };
        if (check_call_graphs(c.calls, expected) != 0) {
            return -1;
        }*/

        Image<int> im = g.realize(20, 20, 20);
        auto func = [im_ref](int x, int y, int z) {
            return im_ref(x, y, z);
        };
        if (check_image(im, func)) {
            return -1;
        }
    }
    return 0;
}

int simple_rfactor_with_specialize_test() {
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

    {
        p.set(0);

        // Check the call graphs.
        // Expect 'g' to call 'wrapper', 'wrapper' to call 'f', 'f' to call nothing
        /*Module m = g.compile_to_module({});
        CheckCalls c;
        m.functions().front().body.accept(&c);

        CallGraphs expected = {
            {g.name(), {wrapper.name()}},
            {wrapper.name(), {f.name()}},
            {f.name(), {}},
        };
        if (check_call_graphs(c.calls, expected) != 0) {
            return -1;
        }*/

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

        // Check the call graphs.
        // Expect 'g' to call 'wrapper', 'wrapper' to call 'f', 'f' to call nothing
        /*Module m = g.compile_to_module({});
        CheckCalls c;
        m.functions().front().body.accept(&c);

        CallGraphs expected = {
            {g.name(), {wrapper.name()}},
            {wrapper.name(), {f.name()}},
            {f.name(), {}},
        };
        if (check_call_graphs(c.calls, expected) != 0) {
            return -1;
        }*/

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

int main(int argc, char **argv) {
    /*printf("Running simple rfactor test\n");
    if (simple_rfactor_test() != 0) {
        return -1;
    }

    printf("Running reorder split rfactor test\n");
    if (reorder_split_rfactor_test() != 0) {
        return -1;
    }

    printf("Running reorder fuse wrapper rfactor test\n");
    if (reorder_fuse_wrapper_rfactor_test() != 0) {
        return -1;
    }

    printf("Running non trivial lhs rfactor test\n");
    if (non_trivial_lhs_rfactor_test() != 0) {
        return -1;
    }*/

    printf("Running simple rfactor with specialization test\n");
    if (simple_rfactor_with_specialize_test() != 0) {
        return -1;
    }

    printf("Success!\n");
    return 0;
}
