#include "Associativity.h"
#include "Substitute.h"
#include "Simplify.h"
#include "IROperator.h"
#include "IREquality.h"
#include "IRMutator.h"
#include "Solve.h"
#include "ExprUsesVar.h"
#include "CSE.h"
#include "Util.h"
#include "IRMatch.h"

#include <algorithm>
#include <iterator>
#include <sstream>

namespace Halide {
namespace Internal {

using std::map;
using std::pair;
using std::set;
using std::string;
using std::vector;

namespace {

class GetExprOpcode : public IRVisitor {
public:
    vector<int> opcode;

    using IRVisitor::visit;

    template<typename T>
    void visit_binary_operator(const T *op, int code) {
        opcode.push_back(code);
        op->a.accept(this);
        op->b.accept(this);
    }

    void visit(const Add *op) {visit_binary_operator(op, 0);}
    void visit(const Mul *op) {visit_binary_operator(op, 1);}
    void visit(const Max *op) {visit_binary_operator(op, 2);}
    void visit(const Min *op) {visit_binary_operator(op, 3);}
    void visit(const Sub *op) {visit_binary_operator(op, 4);}
};

vector<int> get_expr_opcode(Expr e) {
    GetExprOpcode c;
    e.accept(&c);
    return c.opcode;
}

// Replace self-reference to Func 'func' with arguments 'args' at index
// 'value_index' in the Expr/Stmt with Var 'substitute'
class ConvertSelfRef : public IRMutator {
    using IRMutator::visit;

    const string func;
    const vector<Expr> args;
    // If that function has multiple values, which value does this
    // call node refer to?
    int value_index;
    vector<string> op_x_names;
    map<int, Expr> *x_subs;
    bool is_conditional;

    void visit(const Call *op) {
        if (!is_solvable) {
            return;
        }
        IRMutator::visit(op);
        op = expr.as<Call>();
        internal_assert(op);

        if ((op->call_type == Call::Halide) && (func == op->name)) {
            internal_assert(!op->func.defined())
                << "Func should not have been defined for a self-reference\n";
            internal_assert(args.size() == op->args.size())
                << "Self-reference should have the same number of args as the original\n";
            if (is_conditional && (op->value_index == value_index)) {
                debug(4) << "Self-reference of " << op->name
                         << " inside a conditional. Operation is not associative\n";
                is_solvable = false;
                return;
            }
            for (size_t i = 0; i < op->args.size(); i++) {
                if (!equal(op->args[i], args[i])) {
                    debug(4) << "Self-reference of " << op->name
                             << " with different args from the LHS. Operation is not associative\n";
                    is_solvable = false;
                    return;
                }
            }
            // Substitute the call
            const auto &iter = x_subs->find(op->value_index);
            if (iter != x_subs->end()) {
                const Variable *v = iter->second.as<Variable>();
                internal_assert(v && (v->type == op->type));
                debug(4) << "   Substituting Call " << op->name << " at value index "
                         << op->value_index << " with " << v->name << "\n";
                expr = iter->second;
            } else {
                internal_assert(op->value_index < (int)op_x_names.size());
                debug(4) << "   Substituting Call " << op->name << " at value index "
                         << op->value_index << " with " << op_x_names[op->value_index] << "\n";
                expr = Variable::make(op->type, op_x_names[op->value_index]);
                x_subs->emplace(op->value_index, expr);
            }
            if (op->value_index == value_index) {
                x_part = op;
            } else {
                x_dependencies.insert(op->value_index);
            }
        }
    }

    void visit(const Select *op) {
        is_conditional = true;
        Expr cond = mutate(op->condition);
        is_conditional = false;

        Expr t = mutate(op->true_value);
        Expr f = mutate(op->false_value);
        if (cond.same_as(op->condition) &&
            t.same_as(op->true_value) &&
            f.same_as(op->false_value)) {
            expr = op;
        } else {
            expr = Select::make(cond, t, f);
        }
    }

public:
    ConvertSelfRef(const string &f, const vector<Expr> &args, int idx,
                   const vector<string> &x_names, map<int, Expr> *subs) :
        func(f), args(args), value_index(idx), op_x_names(x_names), x_subs(subs), is_conditional(false) {}

    bool is_solvable = true;
    set<int> x_dependencies; // Contains dependencies on self-reference at different tuple indices
    Expr x_part; // Undefined if there is no self-reference at value_index
};

template<typename T>
bool visit_associative_binary_op(int index, const string &op_x, const string &op_y,
                                 Expr x_part, Expr lhs, Expr rhs, AssociativeOps &assoc_ops) {
    const Variable *var_a = lhs.as<Variable>();
    if (!var_a || (var_a->name != op_x)) {
        debug(4) << "Can't prove associativity of " << T::make(lhs, rhs) << "\n";
        return false;
    } else if (expr_uses_var(rhs, op_x)) {
        debug(4) << "Can't prove associativity of " << T::make(lhs, rhs) << "\n";
        return false;
    } else {
        // op(x, y)
        assoc_ops.x[index] = {op_x, x_part};
        assoc_ops.y[index] = {op_y, rhs};
    }
    return true;
}

template <typename T>
std::ostream &operator<<(std::ostream &out, const std::vector<T> &v) {
    out << '[';
    for (size_t i = 0; i < v.size(); ++i) {
        out << v[i];
        if (i < v.size() - 1) {
            out << ", ";
        }
    }
    out << "]";
    return out;
}

template <typename T>
std::ostream &operator<<(std::ostream &out, const std::set<T> &v) {
    out << '[';
    for (auto iter = v.begin(); iter != v.end(); ++iter) {
        out << (*iter);
        if (iter != (--v.end())) {
            out << ", ";
        }
    }
    out << "]";
    return out;
}

bool compare_expr_opcode(const vector<AssociativePair>& lhs, const vector<AssociativePair>& rhs) {
    vector<vector<int>> lhs_opcode, rhs_opcode;
    for (const auto &pair : lhs) {
        debug(5) << "--lhs op: " << pair.op << "\n";
        lhs_opcode.push_back(get_expr_opcode(pair.op));
    }
    for (const auto &pair : rhs) {
        debug(5) << "--rhs op: " << pair.op << "\n";
        rhs_opcode.push_back(get_expr_opcode(pair.op));
    }
    debug(5) << "lhs: " << lhs_opcode << ", rhs: " << rhs_opcode << ", lhs < rhs? " << (lhs_opcode < rhs_opcode) << "\n";
    return lhs_opcode < rhs_opcode;
}

bool associative_op_pattern_match(Expr e,
                                  const AssociativePair &pattern,
                                  const vector<string> &x_names,
                                  const vector<string> &y_names,
                                  const Scope<int> &x_scope,
                                  map<string, Expr> &match) {

    map<string, Expr> result;
    if (expr_match(pattern.op, e, result)) {
        debug(5) << "Find associative ops for " << e << " -> " << pattern.op
                 << " with identity: " << pattern.identity
                 << ", y_part: " << result["y0"] << "\n";

        for (const auto &x_name : x_names) {
            const auto &iter = result.find(x_name);
            if (iter != result.end()) {
                const Variable *xvar = iter->second.as<Variable>();
                if ((xvar == nullptr) || (xvar->name != x_name)) {
                    debug(5) << "...Skipping match since the x_part is different than expected "
                             << iter->second << "\n";
                    return false;
                }
                debug(5) << "...x: " << iter->first << " -> " << iter->second << "\n";
            }
        }
        for (const auto &y_name : y_names) {
            debug(5) << "y_name: " << y_name << "\n";
            const auto &iter = result.find(y_name);
            if (iter != result.end()) {
                // Make sure that y_part should not depend on x vars
                if (expr_uses_vars(iter->second, x_scope)) {
                    debug(5) << "...Skipping match since the y_part depends on x vars\n";
                    return false;
                }
                debug(5) << "...y: " << iter->first << " -> " << iter->second << "\n";
            }
        }

        // Make sure that the new matches are in agreement with any previous matches
        for (const auto &iter : result) {
            const auto &match_iter = match.find(iter.first);
            if (match_iter == match.end()) {
                debug(5) << "Adding result: " << iter.first << " -> " << iter.second << "\n";
                match.emplace(iter.first, iter.second);
            } else {
                if (!equal(iter.first, match_iter->first) || !equal(iter.second, match_iter->second)) {
                    return false;
                }
            }
        }

        return true;
    }
    return false;
}

bool find_match(const vector<vector<AssociativePair>> &table, const vector<string> &op_x_names,
                const vector<string> &op_y_names, const vector<Expr> &x_parts,
                const vector<Expr> &exprs, AssociativeOps &assoc_ops) {
    internal_assert(op_x_names.size() == op_y_names.size());
    internal_assert(op_x_names.size() == x_parts.size());
    internal_assert(op_x_names.size() == exprs.size());
    internal_assert(op_x_names.size() == assoc_ops.size());

    vector<AssociativePair> expr_ops(exprs.size());
    for (size_t i = 0; i < exprs.size(); ++i) {
        expr_ops[i] = AssociativePair(exprs[i]);
    }

    //TODO(psuriana): Find a tighter bound
    auto lower = table.begin();
    auto upper = std::upper_bound(table.begin(), table.end(), expr_ops, compare_expr_opcode);

    //auto upper = table.end();

    //std::cout << "****lower: " << lower << ", upper: " << upper << "\n";

    // We need to convert the variable names into x{tuple_idx}
    // to match it with the associative ops table.
    vector<string> pattern_x_names(op_x_names.size());
    vector<string> pattern_y_names(op_y_names.size());
    Scope<int> pattern_x_scope;
    vector<Expr> sub_exprs(exprs);
    for (size_t i = 0; i < op_x_names.size(); ++i) {
        pattern_x_names[i] = "x" + std::to_string(i);
        pattern_x_scope.push(pattern_x_names[i], 0);
        pattern_y_names[i] = "y" + std::to_string(i);
        for (size_t j = 0; j < sub_exprs.size(); ++j) {
            sub_exprs[j] = substitute(op_x_names[i], Variable::make(x_parts[i].type(),
                                      pattern_x_names[i]), sub_exprs[j]);
        }
        debug(5) << "**** expr: " << exprs[i] << " -> " << sub_exprs[i] << "\n";
    }

    for (; lower < upper; lower++) {
        vector<AssociativePair> patterns = *lower;
        internal_assert(patterns.size() == op_x_names.size());
        map<string, Expr> pattern_match;
        bool matched = true;
        for (size_t i = 0; i < patterns.size(); ++i) {
            if (!associative_op_pattern_match(sub_exprs[i], patterns[i], pattern_x_names,
                                              pattern_y_names, pattern_x_scope, pattern_match)) {
                matched = false;
                break;
            }
        }

        if (!matched) {
            continue;
        }

        map<Expr, Expr, ExprCompare> replacement;
        for (const auto &y_name : pattern_y_names) {
            debug(5) << "---Finding match for " << y_name << "\n";
            const auto &iter = pattern_match.find(y_name);
            if (iter == pattern_match.end()) {
                // Didn't find y{index} during pattern matching. Try next pattern.
                matched = false;
                break;
            }
            Expr y_part = iter->second;
            int index = atoi(y_name.substr(1, y_name.size()-1).c_str());
            internal_assert(y_name == "y" + std::to_string(index));

            debug(5) << "Pattern: " << op_x_names[index] << " -> " << x_parts[index]
                     << ", " << op_y_names[index] << " -> " << y_part << "\n";
            assoc_ops.x[index] = {op_x_names[index], x_parts[index]};
            assoc_ops.y[index] = {op_y_names[index], y_part};
            replacement.emplace(y_part, Variable::make(y_part.type(), op_y_names[index]));
        }
        if (!matched) {
            continue;
        }
        for (size_t index = 0; index < exprs.size(); ++index) {
            Expr e = exprs[index];
            for (const auto &iter : replacement) {
                e = substitute(iter.first, iter.second, e);
            }
            assoc_ops.ops[index] = {e, patterns[index].identity};
        }
        return true;
    }

    return false;
}

bool extract_associative_op_single_element(int index, const vector<string> &op_x_names,
                                           const vector<string> &op_y_names, Expr x_part,
                                           Expr e, AssociativeOps &assoc_ops) {
    Type t = e.type();
    const string &op_x = op_x_names[index];
    const string &op_y = op_y_names[index];
    Expr x = Variable::make(t, op_x);
    Expr y = Variable::make(t, op_y);

    debug(5) << "\n\nProving associativity of:  " << e << "\n";

    if (!x_part.defined()) { // op(y)
        // Update with no self-recurrence is associative and the identity can be
        // anything since it's going to be replaced anyway
        assoc_ops.ops[index] = {y, make_const(t, 0)};
        assoc_ops.x[index] = {"", Expr()};
        assoc_ops.y[index] = {op_y, e};
        return true;
    }

    bool success = false;
    if (const Add *a = e.as<Add>()) {
        assoc_ops.ops[index] = {x + y, make_const(t, 0)};
        success = visit_associative_binary_op<Add>(index, op_x, op_y, x_part, a->a, a->b, assoc_ops);
    } else if (const Sub *s = e.as<Sub>()) {
        assoc_ops.ops[index] = {x + y, make_const(t, 0)};
        success = visit_associative_binary_op<Sub>(index, op_x, op_y, x_part, s->a, s->b, assoc_ops);
    } else if (const Mul *m = e.as<Mul>()) {
        assoc_ops.ops[index] = {x * y, make_const(t, 1)};
        success = visit_associative_binary_op<Mul>(index, op_x, op_y, x_part, m->a, m->b, assoc_ops);
    } else if (const Min *m = e.as<Min>()) {
        assoc_ops.ops[index] = {Min::make(x, y), t.max()};
        success = visit_associative_binary_op<Min>(index, op_x, op_y, x_part, m->a, m->b, assoc_ops);
    } else if (const Max *m = e.as<Max>()) {
        assoc_ops.ops[index] = {Max::make(x, y), t.min()};
        success = visit_associative_binary_op<Max>(index, op_x, op_y, x_part, m->a, m->b, assoc_ops);
    } else if (const And *a = e.as<And>()) {
        assoc_ops.ops[index] = {And::make(x, y), make_const(t, 1)};
        success = visit_associative_binary_op<And>(index, op_x, op_y, x_part, a->a, a->b, assoc_ops);
    } else if (const Or *o = e.as<Or>()) {
        assoc_ops.ops[index] = {Or::make(x, y), make_const(t, 0)};
        success = visit_associative_binary_op<Or>(index, op_x, op_y, x_part, o->a, o->b, assoc_ops);
    } else if (e.as<Let>()) {
        internal_error << "Let should have been substituted before calling this function\n";
    }

    if (!success && t.is_int() && (t.bits() == 32)) {
        // It's non-trivial binary ops. Try looking at the associative ops table for int32
        debug(5) << "Look-up associativity table for: " << e << "\n";
        AssociativeOps temp(1);
        success = find_match(get_i32_ops_table({e}), {op_x}, {op_y}, {x_part}, {e}, temp);
        if (success) {
            // Copy the result over
            assoc_ops.ops[index] = temp.ops[0];
            assoc_ops.x[index] = temp.x[0];
            assoc_ops.y[index] = temp.y[0];
        }
    }
    debug(5) << e << " is associative? " << success << "\n";
    return success;
}

class FindConflict : public IRGraphVisitor {
    using IRGraphVisitor::visit;

    string var;

    void visit(const Variable *v) {
        if (var == v->name) {
            if (expr.defined()) {
                internal_assert(equal(expr, Expr(v)));
            } else {
                expr = Expr(v);
            }
        }
    }
public:
    FindConflict(const string &v) : var(v) {}
    Expr expr;
};

Expr find_conflict(Expr e, const string &v) {
    FindConflict f(v);
    e.accept(&f);
    return f.expr;
}

// Given dependencies of each tuple element, compute the set of subgraphs:
// all vertices that are reachable from a given vertex. If a subgraph is fully
// contained in another subgraph, remove it from the final output.
vector<set<int>> compute_subgraphs(vector<set<int>> dependencies) {
    vector<set<int>> subgraphs(dependencies.size());
    for (size_t i = 0; i < dependencies.size(); ++i) {
        // Check if the current subgraph is a subset of another
        const auto &current = dependencies[i];
        if (current.empty()) {
            continue;
        }
        bool should_remove = false;
        for (size_t j = 0; j < dependencies.size(); ++j) {
            const auto &other = dependencies[j];
            if ((i == j) || (current.size() > other.size()) || (j < i && subgraphs[i].empty())) {
                continue;
            }
            vector<int> diff;
            // Compute the vertices in the current set that are not contained in the other
            std::set_difference(current.begin(), current.end(), other.begin(), other.end(),
                                std::inserter(diff, diff.begin()));
            if (diff.empty()) {
                // 'current' is fully contained in 'other'
                should_remove = true;
                break;
            }
        }
        if (!should_remove) {
            subgraphs[i] = current;
        }
    }
    return subgraphs;
}


} // anonymous namespace


AssociativityProverResult prove_associativity(const string &f, vector<Expr> args,
                                              vector<Expr> exprs) {
    AssociativeOps assoc_ops(exprs.size());
    map<int, Expr> x_subs;

    for (Expr &arg : args) {
        arg = common_subexpression_elimination(arg);
        arg = simplify(arg);
        arg = substitute_in_all_lets(arg);
    }

    // Need to cleanup all occurences of x0, y0, etc, to avoid clashing during
    // the wildcard matching later on.
    map<string, Expr> dummy_subs;
    map<string, Expr> reverse_dummy_subs;
    for (size_t idx = 0; idx < exprs.size(); ++idx) {
        {
            string x_name = "x" + std::to_string(idx);
            Expr conflict = find_conflict(exprs[idx], x_name);
            if (conflict.defined()) {
                const auto &iter = dummy_subs.find(x_name);
                if (iter == dummy_subs.end()) {
                    string x_dummy = unique_name("x_dummy" + std::to_string(idx));
                    dummy_subs.emplace(x_name, Variable::make(conflict.type(), x_dummy));
                    reverse_dummy_subs.emplace(x_dummy, conflict);
                }
            }
        }
        {
            string y_name = "y" + std::to_string(idx);
            Expr conflict = find_conflict(exprs[idx], y_name);
            if (conflict.defined()) {
                const auto &iter = dummy_subs.find(y_name);
                if (iter == dummy_subs.end()) {
                    string y_dummy = unique_name("y_dummy" + std::to_string(idx));
                    dummy_subs.emplace(y_name, Variable::make(conflict.type(), y_dummy));
                    reverse_dummy_subs.emplace(y_dummy, conflict);
                }
            }
        }
    }
    for (size_t idx = 0; idx < exprs.size(); ++idx) {
        exprs[idx] = substitute(dummy_subs, exprs[idx]);
    }

    vector<string> op_x_names(exprs.size()), op_y_names(exprs.size());
    for (size_t idx = 0; idx < exprs.size(); ++idx) {
        op_x_names[idx] = unique_name("_x_" + std::to_string(idx));
        op_y_names[idx] = unique_name("_y_" + std::to_string(idx));
    }

    vector<set<int>> dependencies(exprs.size());
    vector<Expr> x_parts(exprs.size());
    bool all_independent = true;
    bool all_i32 = true;

    // For a Tuple of exprs to be associative, each element of the Tuple
    // has to be associative.
    for (int idx = exprs.size()-1; idx >= 0; --idx) {
        string op_x = op_x_names[idx];
        string op_y = op_y_names[idx];

        if (!exprs[idx].type().is_int() || exprs[idx].type().bits() != 32) {
            all_i32 = false;
        }
        exprs[idx] = simplify(exprs[idx]);

        // Replace any self-reference to Func 'f' with a Var
        ConvertSelfRef csr(f, args, idx, op_x_names, &x_subs);
        exprs[idx] = csr.mutate(exprs[idx]);
        if (!csr.is_solvable) {
            return AssociativityProverResult();
        }
        if (!csr.x_dependencies.empty()) {
            all_independent = false;
        }
        x_parts[idx] = csr.x_part;
        dependencies[idx] = csr.x_dependencies;
        if (csr.x_part.defined()) {
            // Dependency on itself
            dependencies[idx].insert(idx);
        }

        exprs[idx] = common_subexpression_elimination(exprs[idx]);
        exprs[idx] = simplify(exprs[idx]);
        exprs[idx] = solve_expression(exprs[idx], op_x).result; // Move 'x' to the left as possible
        exprs[idx] = substitute_in_all_lets(exprs[idx]);
    }

    if (all_independent || (exprs.size() == 1)) {
        debug(5) << "All tuple elements are independent. Try proving associativity of "
                 << "each element separately.\n";
        // Since there is no x-cross-dependencies, check associativity of each tuple
        // element separately.
        for (size_t idx = 0; idx < exprs.size(); ++idx) {
            // Try to infer the 'y' part of the operator. If we couldn't find
            // a single 'y' that satisfy the operator, give up
            bool is_associative = extract_associative_op_single_element(
                idx, op_x_names, op_y_names, x_parts[idx], exprs[idx], assoc_ops);
            if (!is_associative) {
                return AssociativityProverResult();
            }
        }
    } else {
        debug(5) << "There is cross-dependencies. Need to prove associativity in bulk.\n";
        //TODO(psuriana): currently only works for 32-bit integers and maximum of 2 tuple elements
        if (exprs.size() > 2 || !all_i32) {
            return AssociativityProverResult();
        }
        if (!find_match(get_i32_ops_table(exprs), op_x_names, op_y_names, x_parts, exprs, assoc_ops)) {
            debug(5) << "Cannot find matching associative ops\n";
            return AssociativityProverResult();
        }

    }

    // Replace the dummy variables
    for (size_t idx = 0; idx < exprs.size(); ++idx) {
        assoc_ops.ops[idx].op = substitute(reverse_dummy_subs, assoc_ops.ops[idx].op);
        assoc_ops.x[idx].expr = substitute(reverse_dummy_subs, assoc_ops.x[idx].expr);
        assoc_ops.y[idx].expr = substitute(reverse_dummy_subs, assoc_ops.y[idx].expr);
    }

    return AssociativityProverResult(true, assoc_ops);
}

namespace {

std::string print_args(const string &f, const vector<Expr> &args, const vector<Expr> &exprs) {
    std::ostringstream stream;
    stream << f << "(";
    for (size_t i = 0; i < args.size(); ++i) {
        stream << args[i];
        if (i != args.size() - 1) {
            stream << ", ";
        }
    }
    stream << ") = ";

    if (exprs.size() == 1) {
        stream << exprs[0];
    } else if (exprs.size() > 1) {
        stream << "Tuple(";
        for (size_t i = 0; i < exprs.size(); ++i) {
            stream << exprs[i];
            if (i != exprs.size() - 1) {
                stream << ", ";
            }
        }
        stream << ")";
    }
    return stream.str();
}

void check_associativity(const string &f, vector<Expr> args, vector<Expr> exprs,
                         bool is_associative, const AssociativeOps &assoc_ops) {
    auto result = prove_associativity(f, args, exprs);
    internal_assert(result.is_associative == is_associative)
        << "Checking associativity: " << print_args(f, args, exprs) << "\n"
        << "  Expect is_associative: " << is_associative << "\n"
        << "  instead of " << result.is_associative << "\n";
    if (is_associative) {
        map<string, Expr> replacement;
        for (size_t i = 0; i < assoc_ops.size(); ++i) {
            internal_assert(equal(result.ops()[i].identity, assoc_ops.ops[i].identity))
                << "Checking associativity: " << print_args(f, args, exprs) << "\n"
                << "  Index: " << i << "\n"
                << "  Expect identity: " << assoc_ops.ops[i].identity << "\n"
                << "  instead of " << result.ops()[i].identity << "\n";
            internal_assert(equal(result.x()[i].expr, assoc_ops.x[i].expr))
                << "Checking associativity: " << print_args(f, args, exprs) << "\n"
                << "  Index: " << i << "\n"
                << "  Expect x: " << assoc_ops.x[i].expr << "\n"
                << "  instead of " << result.x()[i].expr << "\n";
            internal_assert(equal(result.y()[i].expr, assoc_ops.y[i].expr))
                << "Checking associativity: " << print_args(f, args, exprs) << "\n"
                << "  Index: " << i << "\n"
                << "  Expect y: " << assoc_ops.x[i].expr << "\n"
                << "  instead of " << result.y()[i].expr << "\n";

            if (result.x()[i].expr.defined()) {
                replacement.emplace(assoc_ops.x[i].var, Variable::make(result.x()[i].expr.type(), result.x()[i].var));
            }
            if (result.y()[i].expr.defined()) {
                replacement.emplace(assoc_ops.y[i].var, Variable::make(result.y()[i].expr.type(), result.y()[i].var));
            }
        }

        for (size_t i = 0; i < assoc_ops.size(); ++i) {
            Expr expected_op = substitute(replacement, assoc_ops.ops[i].op);

            internal_assert(equal(result.ops()[i].op, expected_op))
                << "Checking associativity: " << print_args(f, args, exprs) << "\n"
                << "  Index: " << i << "\n"
                << "  Expect bin op: " << expected_op << "\n"
                << "  instead of " << result.ops()[i].op << "\n";

            debug(4) << "\nExpected op: " << expected_op << "\n";
            debug(4) << "Operator: " << result.ops()[i].op << "\n";
            debug(4) << "   identity: " << result.ops()[i].identity << "\n";
            debug(4) << "   x: " << result.x()[i].var << " -> " << result.x()[i].expr << "\n";
            debug(4) << "   y: " << result.y()[i].var << " -> " << result.y()[i].expr << "\n";
        }
    }
}

} // anonymous namespace

void associativity_test() {
    typedef AssociativeOps::Replacement Replacement;

    Type t = Int(32);
    Expr x = Variable::make(t, "x");
    Expr y = Variable::make(t, "y");
    Expr z = Variable::make(t, "z");
    Expr rx = Variable::make(t, "rx");

    vector<Expr> xs(3), ys(3), zs(3);
    for (size_t i = 0; i < 3; ++i) {
        xs[i] = Variable::make(t, "x" + std::to_string(i));
        ys[i] = Variable::make(t, "y" + std::to_string(i));
        zs[i] = Variable::make(t, "z" + std::to_string(i));
    }

    Expr f_call_0 = Call::make(t, "f", {x}, Call::CallType::Halide, nullptr, 0);
    Expr f_call_1 = Call::make(t, "f", {x}, Call::CallType::Halide, nullptr, 1);
    Expr f_call_2 = Call::make(t, "f", {x}, Call::CallType::Halide, nullptr, 2);
    Expr g_call_0 = Call::make(t, "g", {rx}, Call::CallType::Halide, nullptr, 0);
    Expr g_call_1 = Call::make(t, "g", {rx}, Call::CallType::Halide, nullptr, 1);

    // f(x) = f(x) - g(rx) -> Is associative given that the merging operator is +
    /*check_associativity("f", {x}, {f_call_0 - g_call_0}, true,
                        {{AssociativePair(x + y, 0)},
                         {Replacement("x", f_call_0)},
                         {Replacement("y", g_call_0)}
                        });*/

    // f(x) = min(f(x), int16(z))
    check_associativity("f", {x}, {min(f_call_0, y + Cast::make(Int(16), z))}, true,
                        {{AssociativePair(min(x, y), t.max())},
                         {Replacement("x", f_call_0)},
                         {Replacement("y", y + Cast::make(Int(16), z))}
                        });

    // f(x) = f(x) + g(rx) + y + z
    check_associativity("f", {x}, {y + z + f_call_0}, true,
                        {{AssociativePair(x + y, make_const(t, 0))},
                         {Replacement("x", f_call_0)},
                         {Replacement("y", y + z)}
                        });

    // f(x) = max(y, f(x))
    check_associativity("f", {x}, {max(y, f_call_0)}, true,
                        {{AssociativePair(max(x, y), t.min())},
                         {Replacement("x", f_call_0)},
                         {Replacement("y", y)}
                        });

    // f(x) = Tuple(2, 3, f(x)[2] + z)
    check_associativity("f", {x}, {2, 3, f_call_2 + z}, true,
                        {{AssociativePair(ys[0], make_const(t, 0)),
                            AssociativePair(ys[1], make_const(t, 0)),
                            AssociativePair(xs[2] + ys[2], make_const(t, 0))},
                         {Replacement("", Expr()), Replacement("", Expr()), Replacement("x2", f_call_2)},
                         {Replacement("y0", 2), Replacement("y1", 3), Replacement("y2", z)},
                        });

    // f(x) = Tuple(min(f(x)[0], g(rx)), f(x)[1]*g(x)*2, f(x)[2] + z)
    check_associativity("f", {x}, {min(f_call_0, g_call_0), f_call_1*g_call_0*2, f_call_2 + z}, true,
                        {{AssociativePair(min(xs[0], ys[0]), t.max()),
                            AssociativePair(xs[1] * ys[1], make_const(t, 1)),
                            AssociativePair(xs[2] + ys[2], make_const(t, 0))},
                         {Replacement("x0", f_call_0), Replacement("x1", f_call_1), Replacement("x2", f_call_2)},
                         {Replacement("y0", g_call_0), Replacement("y1", g_call_0*2), Replacement("y2", z)},
                        });

    // f(x) = max(f(x) + g(rx), g(rx)) -> not associative
    check_associativity("f", {x}, {max(f_call_0 + g_call_0, g_call_0)}, false, AssociativeOps());

    // f(x) = max(f(x) + g(rx), f(x) - 3) -> f(x) + max(g(rx) - 3)
    check_associativity("f", {x}, {max(f_call_0 + g_call_0, f_call_0 - 3)}, true,
                        {{AssociativePair(x + y, 0)},
                         {Replacement("x", f_call_0)},
                         {Replacement("y", max(g_call_0, -3))}
                        });

    // f(x) = min(4, g(rx)) -> trivially associative
    check_associativity("f", {x}, {min(4, g_call_0)}, true,
                        {{AssociativePair(y, make_const(t, 0))},
                         {Replacement("", Expr())},
                         {Replacement("y", min(g_call_0, 4))}
                        });

    // f(x) = f(x) -> associative but doesn't really make any sense, so we'll treat it as non-associative
    check_associativity("f", {x}, {f_call_0}, false, AssociativeOps());

    // f(x) = max(max(min(f(x), g(rx) + 2), f(x)), g(rx) + 2)
    check_associativity("f", {x}, {max(max(min(f_call_0, g_call_0 + 2), f_call_0), g_call_0 + 2)}, true,
                        {{AssociativePair(max(max(min(x, y), x), y), t.min())},
                         {Replacement("x", f_call_0)},
                         {Replacement("y", g_call_0 + 2)}
                        });

    // f(x) = ((min(max((f(x)*g(rx)), g(rx)), (f(x)*g(rx))) + g(rx)) + f(x))
    check_associativity("f", {x}, {((min(max((g_call_0*f_call_0), g_call_0), (f_call_0*g_call_0)) + g_call_0) + f_call_0)},
                        true,
                        {{AssociativePair(((min(max((x*y), y), (x*y)) + y) + x), make_const(t, 0))},
                         {Replacement("x", f_call_0)},
                         {Replacement("y", g_call_0)}
                        });

    // f(x) = Tuple(f(x)[0]*g(r.x)[0] - f(x)[1]*g(r.x)[1], f(x)[0]*g(r.x)[1] + f(x)[1]*g(r.x)[0])
    check_associativity("f", {x}, {f_call_0*g_call_0 - f_call_1*g_call_1, f_call_0*g_call_1 + f_call_1*g_call_0}, true,
                        {{AssociativePair(xs[0]*ys[0] - xs[1]*ys[1], make_const(t, 1)),
                            AssociativePair(xs[1]*ys[0] + xs[0]*ys[1], make_const(t, 0))},
                         {Replacement("x0", f_call_0), Replacement("x1", f_call_1)},
                         {Replacement("y0", g_call_0), Replacement("y1", g_call_1)},
                        });

    // f(x) = Tuple(f(x)[0]*g(r.x)[0] - f(x)[1]*g(r.x)[1], f(x)[0]*g(r.x)[1] + f(x)[1]*g(r.x)[0])
    check_associativity("f", {x}, {min(f_call_0, g_call_0), select(f_call_0 < g_call_0, f_call_1, rx)}, true,
                        {{AssociativePair(min(xs[0], ys[0]), t.max()),
                            AssociativePair(select(xs[0] < ys[0], xs[1], ys[1]), make_const(t, 0))},
                         {Replacement("x0", f_call_0), Replacement("x1", f_call_1)},
                         {Replacement("y0", g_call_0), Replacement("y1", rx)},
                        });

    // f(x) = max(x0, f(x)) -> x0 may conflict with the wildcard associative op patterns
    check_associativity("f", {x}, {max(xs[0], f_call_0)}, true,
                        {{AssociativePair(max(x, y), t.min())},
                         {Replacement("x", f_call_0)},
                         {Replacement("y", xs[0])}
                        });

    {
        vector<set<int>> dependencies(4);
        dependencies[0] = {0};
        dependencies[1] = {1};
        dependencies[2] = {0, 1, 2};
        dependencies[3] = {0, 1, 3};
        vector<set<int>> subgraphs = compute_subgraphs(dependencies);
        std::cout << "Computing subgraphs of: " << dependencies << "\n";
        std::cout << "Result: " << subgraphs << "\n";
    }
    {
        vector<set<int>> dependencies(2);
        dependencies[0] = {0, 1};
        dependencies[1] = {1, 0};
        vector<set<int>> subgraphs = compute_subgraphs(dependencies);
        std::cout << "Computing subgraphs of: " << dependencies << "\n";
        std::cout << "Result: " << subgraphs << "\n";
    }

    std::cout << "Associativity test passed" << std::endl;
}


}
}
