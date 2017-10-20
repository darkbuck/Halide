#include "DynamicShuffle.h"
#include "CSE.h"
#include "Scope.h"
#include "IRMutator.h"
#include "IREquality.h"
#include "Simplify.h"

namespace Halide {
namespace Internal {

using std::pair;
using std::string;
using std::vector;

namespace {

// Find an upper bound of bounds.max - bounds.min.
Expr span_of_bounds(Interval bounds) {
    internal_assert(bounds.is_bounded());

    const Min *min_min = bounds.min.as<Min>();
    const Max *min_max = bounds.min.as<Max>();
    const Min *max_min = bounds.max.as<Min>();
    const Max *max_max = bounds.max.as<Max>();
    const Add *min_add = bounds.min.as<Add>();
    const Add *max_add = bounds.max.as<Add>();
    const Sub *min_sub = bounds.min.as<Sub>();
    const Sub *max_sub = bounds.max.as<Sub>();

    if (min_min && max_min && equal(min_min->b, max_min->b)) {
        return span_of_bounds({min_min->a, max_min->a});
    } else if (min_max && max_max && equal(min_max->b, max_max->b)) {
        return span_of_bounds({min_max->a, max_max->a});
    } else if (min_add && max_add && equal(min_add->b, max_add->b)) {
        return span_of_bounds({min_add->a, max_add->a});
    } else if (min_sub && max_sub && equal(min_sub->b, max_sub->b)) {
        return span_of_bounds({min_sub->a, max_sub->a});
    } else {
        return bounds.max - bounds.min;
    }
}

// Replace indirect loads with dynamic_shuffle intrinsics where
// possible.
class DynamicShuffle : public IRMutator {
    Type index_type;
    int max_size;
    int alignment;
    Scope<Interval> bounds;
    vector<pair<string, Expr>> lets;

    using IRMutator::visit;

    template <typename T>
    void visit_let(const T *op) {
        // We only care about vector lets.
        if (op->value.type().is_vector()) {
            bounds.push(op->name, bounds_of_expr_in_scope(op->value, bounds));
        }
        IRMutator::visit(op);
        if (op->value.type().is_vector()) {
            bounds.pop(op->name);
        }
    }

    void visit(const Let *op) {
        lets.push_back({op->name, op->value});
        visit_let(op);
        lets.pop_back();
    }
    void visit(const LetStmt *op) { visit_let(op); }

    void visit(const Load *op) {
        if (!is_one(op->predicate)) {
            // TODO(psuriana): We shouldn't mess with predicated load for now.
            IRMutator::visit(op);
            return;
        }
        if (!op->type.is_vector() || op->index.as<Ramp>()) {
            // Don't handle scalar or simple vector loads.
            IRMutator::visit(op);
            return;
        }

        Expr index = mutate(op->index);
        Interval unaligned_index_bounds = bounds_of_expr_in_scope(index, bounds);
        if (unaligned_index_bounds.is_bounded()) {
            // We want to try both the unaligned and aligned
            // bounds. The unaligned bounds might fit in 256 elements,
            // while the aligned bounds do not.
            int align = alignment / op->type.bytes();
            Interval aligned_index_bounds = {
                (unaligned_index_bounds.min / align) * align,
                ((unaligned_index_bounds.max + align) / align) * align - 1
            };

            for (Interval index_bounds : {aligned_index_bounds, unaligned_index_bounds}) {
                Expr index_span = span_of_bounds(index_bounds);
                index_span = common_subexpression_elimination(index_span);
                index_span = simplify(index_span);

                if (can_prove(index_span < max_size)) {
                    // This is a lookup within an up to max_size element array. We
                    // can use dynamic_shuffle for this.
                    int const_extent = as_const_int(index_span) ? *as_const_int(index_span) + 1 : max_size;
                    Expr base = simplify(index_bounds.min);

                    // Load all of the possible indices loaded from the
                    // LUT. Note that for clamped ramps, this loads up to 1
                    // vector past the max. CodeGen_Hexagon::allocation_padding
                    // returns a native vector size to account for this.
                    Expr lut = Load::make(op->type.with_lanes(const_extent), op->name,
                                          Ramp::make(base, 1, const_extent),
                                          op->image, op->param, const_true(const_extent));

                    index = simplify(cast(index_type.with_lanes(op->type.lanes()), index - base));

                    expr = Call::make(op->type, Call::dynamic_shuffle,
                                      {lut, index, 0, const_extent - 1}, Call::PureIntrinsic);
                    return;
                }
            }
        }
        if (!index.same_as(op->index)) {
            expr = Load::make(op->type, op->name, index, op->image, op->param, op->predicate);
        } else {
            expr = op;
        }
    }

public:
    DynamicShuffle(Type index_type, int max_size, int alignment)
        : index_type(index_type), max_size(max_size), alignment(alignment) {}
};

}  // namespace

Stmt find_dynamic_shuffles(Stmt s, Type index_type, int max_size, int alignment) {
    return DynamicShuffle(index_type, max_size, alignment).mutate(s);
}

}  // namespace Internal
}  // namespace Halide
