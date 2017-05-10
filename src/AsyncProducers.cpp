#include "AsyncProducers.h"
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
            Expr release = Call::make(Int(32), "halide_semaphore_release", {sema}, Call::Extern);
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
        debug(0) << "Hit Acquire: " << op->semaphore << "\n";
        internal_assert(var);
        if (is_no_op(body)) {
            stmt = body;
        } else if (starts_with(var->name, func + ".folding_semaphore.")) {
            // This is a storage-folding semaphore. Keep it.
            stmt = Acquire::make(op->semaphore, body);
        } else {
            // Uh-oh, the consumer also has a copy of this acquire! Make
            // a distinct one for the producer
            string forked_acquire = var->name + unique_name('_');
            debug(0) << "Queueing up forked acquire: " << var->name << " -> " << forked_acquire << "\n";
            forked_acquires[var->name] = forked_acquire;
            stmt = Acquire::make(Variable::make(type_of<halide_semaphore_t *>(), forked_acquire), body);
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

    map<string, string> &forked_acquires;
    set<string> inner_semaphores;

public:
    GenerateProducerBody(const string &f, Expr s, map<string, string> &a) :
        func(f), sema(s), forked_acquires(a) {}
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
                stmt = Acquire::make(sema, op);
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

class ForkAcquire : public IRMutator {
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
    ForkAcquire(const string &o, const string &new_name) : old_name(o) {
        new_var = Variable::make(type_of<halide_semaphore_t *>(), new_name);
    }
};

class ForkAsyncProducers : public IRMutator {
    using IRMutator::visit;

    const map<string, Function> &env;

    map<string, string> forked_acquires;

    void visit(const Realize *op) {
        auto it = env.find(op->name);
        internal_assert(it != env.end());
        Function f = it->second;
        if (/* f is scheduled async */
            starts_with(f.name(), "async_")) {
            // Make two copies of the body, one which only does the
            // producer, and one which only does the consumer. Inject
            // synchronization to preserve dependencies. Put them in a
            // task-parallel block.

            // Make a semaphore.
            string sema_name = op->name + ".semaphore";
            Expr sema_var = Variable::make(Handle(), sema_name);

            Stmt body = op->body;
            Stmt producer = GenerateProducerBody(op->name, sema_var, forked_acquires).mutate(body);
            Stmt consumer = GenerateConsumerBody(op->name, sema_var).mutate(body);

            debug(0) << "*****************\nForking on " << op->name << "\n";
            debug(0) << "Producer:\n" << producer << "\n";
            debug(0) << "Consumer:\n" << consumer << "\n";
            debug(0) << "Done forking on " << op->name << "\n";

            // Recurse on both sides
            producer = mutate(producer);
            consumer = mutate(consumer);

            // Poor man's task parallel block
            string task = unique_name('t');
            Expr task_var = Variable::make(Int(32), task);
            body = IfThenElse::make(task_var == 0, producer, consumer);
            body = For::make(task, 0, 2, ForType::Parallel, DeviceAPI::None, body);

            // The semaphore is just an int on the stack
            Expr sema_space = Call::make(type_of<halide_semaphore_t *>(), Call::alloca,
                                         {(int)sizeof(halide_semaphore_t)}, Call::Intrinsic);
            Expr sema_init = Call::make(type_of<halide_semaphore_t *>(), "halide_semaphore_init",
                                        {sema_var, 0}, Call::Extern);
            body = Block::make(Evaluate::make(sema_init), body);

            // If there's a nested async producer, we may have
            // recursively forked this semaphore inside the mutation
            // of the producer and consumer.
            auto it = forked_acquires.find(sema_name);
            if (it != forked_acquires.end()) {
                body = ForkAcquire(sema_name, it->second).mutate(body);
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

Stmt fork_async_producers(Stmt s, const map<string, Function> &env) {
    // TODO: tighten up the scope of consume nodes first
    s = ForkAsyncProducers(env).mutate(s);
    return s;
}

}
}
