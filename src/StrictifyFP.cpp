#include "StrictifyFP.h"

#include "IRMutator.h"
#include "IROperator.h"

namespace Halide {
namespace Internal {

class StrictifyFP : public IRMutator2 {
    enum Strictness {
        FastMath,
        NoFPSimplify,
        StrictFP,
    } strictness;

    using IRMutator2::visit;

    Expr visit(const Call *call) override {
        Strictness new_strictness = strictness;

        if (call->is_intrinsic(Call::strict_fp)) {
            new_strictness = StrictFP;
        } else if (call->is_intrinsic(Call::no_fp_simplify) && new_strictness != StrictFP) {
            new_strictness = NoFPSimplify;
        }

        ScopedValue<Strictness> save_strictness(strictness, new_strictness);

        return IRMutator2::visit(call);
    }

    Expr mutate(const Expr &expr) override {
        Expr e = IRMutator2::mutate(expr);
        if (e.type().is_float()) {
            switch (strictness) {
            case FastMath:
                return e;
                break;
            case NoFPSimplify:
                return no_fp_simplify(e);
                break;
            case StrictFP:
                return strict_fp(e);
                break;
            }
        }
        return e;
    }

public:
    StrictifyFP(bool strict) : strictness(strict ? StrictFP : FastMath) { }
};

void strictify_fp(std::map<std::string, Function> &env, const Target &t) {
    for (auto &iter : env) {
        Function &func = iter.second;

        StrictifyFP strictify(t.has_feature(Target::StrictFP));
        func.mutate(&strictify);
    }
}

}
}
