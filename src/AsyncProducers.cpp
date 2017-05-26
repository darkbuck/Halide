#include "AsyncProducers.h"
#include "ExprUsesVar.h"
#include "IRMutator.h"
#include "IROperator.h"

namespace Halide {
namespace Internal {

using std::vector;
using std::set;
using std::pair;
using std::string;
using std::map;

class GenerateProducerBody : public IRMutator {
    const string &func;
    Expr sema;

    using IRMutator::visit;

    // Preserve produce nodes and add synchronization
    void visit(const ProducerConsumer *op) {
        if (op->name == func && op->is_producer) {
            // Add post-synchronization
            Expr release = Call::make(Int(32), "halide_semaphore_release", {sema, 1}, Call::Extern);
            Stmt body = Block::make(op->body, Evaluate::make(release));
            stmt = ProducerConsumer::make_produce(op->name, body);
        } else {
            Stmt body = mutate(op->body);
            if (is_no_op(body)) {
                stmt = body;
            } else {
                stmt = ProducerConsumer::make(op->name, op->is_producer, body);
            }
        }
    }

    // Other stmt leaves get replaced with no-ops
    void visit(const Evaluate *) {
        stmt = Evaluate::make(0);
    }

    void visit(const Provide *) {
        stmt = Evaluate::make(0);
    }

    void visit(const AssertStmt *) {
        stmt = Evaluate::make(0);
    }

    void visit(const Prefetch *) {
        stmt = Evaluate::make(0);
    }

    void visit(const LetStmt *op) {
        Stmt body = mutate(op->body);
        if (is_no_op(body)) {
            stmt = body;
        } else {
            stmt = LetStmt::make(op->name, op->value, body);
        }
    }

    void visit(const For *op) {
        Stmt body = mutate(op->body);
        if (is_no_op(body)) {
            stmt = body;
        } else {
            stmt = For::make(op->name, op->min, op->extent, op->for_type, op->device_api, body);
        }
    }

    void visit(const Block *op) {
        Stmt first = mutate(op->first);
        Stmt rest = mutate(op->rest);
        if (is_no_op(first)) {
            stmt = rest;
        } else if (is_no_op(rest)) {
            stmt = first;
        } else {
            stmt = Block::make(first, rest);
        }
    }

    void visit(const Fork *op) {
        Stmt first = mutate(op->first);
        Stmt rest = mutate(op->rest);
        if (is_no_op(first)) {
            stmt = rest;
        } else if (is_no_op(rest)) {
            stmt = first;
        } else {
            stmt = Fork::make(first, rest);
        }
    }

    void visit(const Realize *op) {
        Stmt body = mutate(op->body);
        if (is_no_op(body)) {
            stmt = body;
        } else {
            stmt = Realize::make(op->name, op->types, op->bounds,
                                 op->condition, body);
        }
    }

    void visit(const IfThenElse *op) {
        Stmt then_case = mutate(op->then_case);
        Stmt else_case = mutate(op->else_case);
        if (is_no_op(then_case) && is_no_op(else_case)) {
            stmt = then_case;
        } else {
            stmt = IfThenElse::make(op->condition, then_case, else_case);
        }
    }

    void visit(const Acquire *op) {
        Stmt body = mutate(op->body);
        const Variable *var = op->semaphore.as<Variable>();
        internal_assert(var);
        if (is_no_op(body)) {
            stmt = body;
        } else if (starts_with(var->name, func + ".folding_semaphore.")) {
            // This is a storage-folding semaphore. Keep it.
            stmt = Acquire::make(op->semaphore, op->count, body);
        } else {
            // Uh-oh, the consumer also has a copy of this acquire! Make
            // a distinct one for the producer
            string cloned_acquire = var->name + unique_name('_');
            cloned_acquires[var->name] = cloned_acquire;
            stmt = Acquire::make(Variable::make(type_of<halide_semaphore_t *>(), cloned_acquire), op->count, body);
        }
    }

    void visit(const Call *op) {
        if (op->name == "halide_semaphore_init") {
            internal_assert(op->args.size() == 2);
            const Variable *var = op->args[0].as<Variable>();
            internal_assert(var);
            inner_semaphores.insert(var->name);
        }
        expr = op;
    }

    map<string, string> &cloned_acquires;
    set<string> inner_semaphores;

public:
    GenerateProducerBody(const string &f, Expr s, map<string, string> &a) :
        func(f), sema(s), cloned_acquires(a) {}
};

class GenerateConsumerBody : public IRMutator {
    const string &func;
    Expr sema;

    using IRMutator::visit;

    void visit(const ProducerConsumer *op) {
        if (op->name == func) {
            if (op->is_producer) {
                // Remove the work entirely
                stmt = Evaluate::make(0);
            } else {
                // Synchronize on the work done by the producer before beginning consumption
                stmt = Acquire::make(sema, 1, op);
            }
        } else {
            IRMutator::visit(op);
        }
    }

    void visit(const Acquire *op) {
        // Don't want to duplicate any semaphore acquires. Ones from folding should go to the producer side.
        const Variable *var = op->semaphore.as<Variable>();
        internal_assert(var);
        if (starts_with(var->name, func + ".folding_semaphore.")) {
            stmt = mutate(op->body);
        } else {
            IRMutator::visit(op);
        }
    }

public:
    GenerateConsumerBody(const string &f, Expr s) :
        func(f), sema(s) {}
};

class CloneAcquire : public IRMutator {
    using IRMutator::visit;

    const string &old_name;
    Expr new_var;

    void visit(const Evaluate *op) {
        const Call *call = op->value.as<Call>();
        const Variable *var = ((call && !call->args.empty()) ?
                               call->args[0].as<Variable>() :
                               nullptr);
        if (var && var->name == old_name &&
            (call->name == "halide_semaphore_release" ||
             call->name == "halide_semaphore_init")) {
            vector<Expr> args = call->args;
            args[0] = new_var;
            Stmt new_stmt =
                Evaluate::make(Call::make(call->type, call->name, args, call->call_type));
            stmt = Block::make(op, new_stmt);
        } else {
            stmt = op;
        }
    }

public:
    CloneAcquire(const string &o, const string &new_name) : old_name(o) {
        new_var = Variable::make(type_of<halide_semaphore_t *>(), new_name);
    }
};

class ForkAsyncProducers : public IRMutator {
    using IRMutator::visit;

    const map<string, Function> &env;

    map<string, string> cloned_acquires;

    void visit(const Realize *op) {
        auto it = env.find(op->name);
        internal_assert(it != env.end());
        Function f = it->second;
        if (f.schedule().async()) {
            // Make two copies of the body, one which only does the
            // producer, and one which only does the consumer. Inject
            // synchronization to preserve dependencies. Put them in a
            // task-parallel block.

            // Make a semaphore.
            string sema_name = op->name + ".semaphore";
            Expr sema_var = Variable::make(Handle(), sema_name);

            Stmt body = op->body;
            Stmt producer = GenerateProducerBody(op->name, sema_var, cloned_acquires).mutate(body);
            Stmt consumer = GenerateConsumerBody(op->name, sema_var).mutate(body);

            // Recurse on both sides
            producer = mutate(producer);
            consumer = mutate(consumer);

            // Run them concurrently
            body = Fork::make(producer, consumer);

            // Make a semaphore on the stack
            Expr sema_space = Call::make(type_of<halide_semaphore_t *>(), "halide_make_semaphore",
                                         {0}, Call::Extern);

            // If there's a nested async producer, we may have
            // recursively cloned this semaphore inside the mutation
            // of the producer and consumer.
            auto it = cloned_acquires.find(sema_name);
            if (it != cloned_acquires.end()) {
                body = CloneAcquire(sema_name, it->second).mutate(body);
                body = LetStmt::make(it->second, sema_space, body);
            }

            body = LetStmt::make(sema_name, sema_space, body);

            stmt = Realize::make(op->name, op->types, op->bounds, op->condition, body);
        } else {
            IRMutator::visit(op);
        }
    }

public:
    ForkAsyncProducers(const map<string, Function> &e) : env(e) {}
};

// Lowers semaphore initialization from a call to
// "halide_make_semaphore" to an alloca followed by a call into the
// runtime to initialize. TODO: what if something crashes before
// releasing a semaphore. Do we need a destructor? The acquire task
// needs to leave the task queue somehow without running. We need a
// destructor that unblocks all waiters somewhere.
class InitializeSemaphores : public IRMutator {
    using IRMutator::visit;

    void visit(const LetStmt *op) {
        Stmt body = mutate(op->body);
        if (op->value.type() == type_of<halide_semaphore_t *>()) {
            vector<pair<string, Expr>> lets;
            // Peel off any enclosing lets
            Expr value = op->value;
            while (const Let *l = value.as<Let>()) {
                lets.emplace_back(l->name, l->value);
                value = l->body;
            }
            const Call *call = value.as<Call>();
            if (call && call->name == "halide_make_semaphore") {
                internal_assert(call->args.size() == 1);

                Expr sema_var = Variable::make(type_of<halide_semaphore_t *>(), op->name);
                Expr sema_init = Call::make(Int(32), "halide_semaphore_init",
                                            {sema_var, call->args[0]}, Call::Extern);
                Expr sema_allocate = Call::make(type_of<halide_semaphore_t *>(), Call::alloca,
                                                {(int)sizeof(halide_semaphore_t)}, Call::Intrinsic);
                stmt = Block::make(Evaluate::make(sema_init), body);
                stmt = LetStmt::make(op->name, sema_allocate, stmt);

                // Re-wrap any other lets
                while (lets.size()) {
                    stmt = LetStmt::make(lets.back().first, lets.back().second, stmt);
                }
                return;
            }
        }
        stmt = LetStmt::make(op->name, op->value, body);
    }

    void visit(const Call *op) {
        internal_assert(op->name != "halide_make_semaphore")
            << "Call to halide_make_semaphore in unexpected place\n";
        expr = op;
    }
};

// Tighten the scope of consume nodes as much as possible to avoid needless synchronization.
class TightenConsumeNodes : public IRMutator {
    using IRMutator::visit;

    void visit(const ProducerConsumer *op) {
        Stmt body = mutate(op->body);
        if (op->is_producer) {
            stmt = ProducerConsumer::make(op->name, true, body);
        } else if (const LetStmt *let = body.as<LetStmt>()) {
            stmt = mutate(ProducerConsumer::make(op->name, false, let->body));
            stmt = LetStmt::make(let->name, let->value, stmt);
        } else if (const Block *block = body.as<Block>()) {
            // Check which sides it's used on
            Scope<int> scope;
            scope.push(op->name, 0);
            scope.push(op->name + ".buffer", 0);
            bool first = stmt_uses_vars(block->first, scope);
            bool rest = stmt_uses_vars(block->rest, scope);
            if (first && rest) {
                IRMutator::visit(op);
            } else if (first) {
                stmt = Block::make(
                    mutate(ProducerConsumer::make(op->name, false, block->first)),
                    mutate(block->rest));
            } else if (rest) {
                stmt = Block::make(
                    mutate(block->first),
                    mutate(ProducerConsumer::make(op->name, false, block->rest)));
            } else {
                stmt = mutate(op->body);
            }
        } else if (const ProducerConsumer *pc = body.as<ProducerConsumer>()) {
            Stmt new_body = mutate(ProducerConsumer::make(op->name, false, pc->body));
            stmt = ProducerConsumer::make(pc->name, pc->is_producer, new_body);
        } else if (const Realize *r = body.as<Realize>()) {
            Stmt new_body = mutate(ProducerConsumer::make(op->name, false, r->body));
            stmt = Realize::make(r->name, r->types, r->bounds, r->condition, new_body);
        } else {
            stmt = ProducerConsumer::make(op->name, op->is_producer, body);
        }
    }
};

// Broaden the scope of acquire nodes to pack trailing work into the
// same task and to potentially reduce the nesting depth of tasks.
class ExpandAcquireNodes : public IRMutator {
    using IRMutator::visit;

    void visit(const Block *op) {
        Stmt first = mutate(op->first), rest = mutate(op->rest);
        if (const Acquire *a = first.as<Acquire>()) {
            // May as well nest the rest stmt inside the acquire
            // node. It's also blocked on it.
            stmt = Acquire::make(a->semaphore, a->count,
                                 mutate(Block::make(a->body, op->rest)));
        } else {
            stmt = Block::make(first, rest);
        }
    }

    void visit(const Realize *op) {
        Stmt body = mutate(op->body);
        if (const Acquire *a = body.as<Acquire>()) {
            // Don't do the allocation until we have the
            // semaphore. Reduces peak memory use.
            stmt = Acquire::make(a->semaphore, a->count,
                                 mutate(Realize::make(op->name, op->types, op->bounds, op->condition, a->body)));
        } else {
            stmt = Realize::make(op->name, op->types, op->bounds, op->condition, body);
        }
    }

    void visit(const LetStmt *op) {
        Stmt body = mutate(op->body);
        const Acquire *a = body.as<Acquire>();
        if (a &&
            !expr_uses_var(a->semaphore, op->name) &&
            !expr_uses_var(a->count, op->name)) {
            stmt = Acquire::make(a->semaphore, a->count,
                                 LetStmt::make(op->name, op->value, a->body));
        } else {
            stmt = LetStmt::make(op->name, op->value, body);
        }
    }

    void visit(const ProducerConsumer *op) {
        Stmt body = mutate(op->body);
        if (const Acquire *a = body.as<Acquire>()) {
            stmt = Acquire::make(a->semaphore, a->count,
                                 mutate(ProducerConsumer::make(op->name, op->is_producer, a->body)));
        } else {
            stmt = ProducerConsumer::make(op->name, op->is_producer, body);
        }
    }
};

Stmt fork_async_producers(Stmt s, const map<string, Function> &env) {
    s = TightenConsumeNodes().mutate(s);
    s = ForkAsyncProducers(env).mutate(s);
    s = ExpandAcquireNodes().mutate(s);
    s = InitializeSemaphores().mutate(s);
    return s;
}

}
}
