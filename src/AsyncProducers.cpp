#include "AsyncProducers.h"
#include "IRMutator.h"
#include "IROperator.h"

namespace Halide {
namespace Internal {

namespace {
bool is_semaphore_acquire(Stmt s, const std::string &func) {
    const Evaluate *eval = s.as<Evaluate>();
    const Call *call = eval ? eval->value.as<Call>() : nullptr;
    const Variable *var = call ? call->args[0].as<Variable>() : nullptr;
    return var && call->name == "halide_semaphore_acquire" && starts_with(var->name, func + ".");
}
}

class GenerateProducerBody : public IRMutator {
    const std::string &func;
    Expr sema;

    using IRMutator::visit;

    // Preserve produce nodes and add synchronization
    void visit(const ProducerConsumer *op) {
        if (op->name == func && op->is_producer) {
            // If the body is just a semaphore acquire injected by
            // storage folding, then this doesn't need additional
            // synchronization. It would be incorrect anyway, because
            // there's no matching consume node to do the acquire.
            if (is_semaphore_acquire(op->body, func)) {
                stmt = op->body;
            } else {
                // Add post-synchronization
                Expr release = Call::make(Int(32), "halide_semaphore_release", {sema}, Call::Extern);
                Stmt body = Block::make(op->body, Evaluate::make(release));
                stmt = ProducerConsumer::make_produce(op->name, body);
            }
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

public:
    GenerateProducerBody(const std::string &f, Expr s) : func(f), sema(s) {}
};

class GenerateConsumerBody : public IRMutator {
    const std::string &func;
    Expr sema;

    using IRMutator::visit;

    void visit(const ProducerConsumer *op) {
        if (op->name == func) {
            if (op->is_producer) {
                // Remove the work entirely
                stmt = Evaluate::make(0);
            } else {
                // Synchronize on the work done by the producer before beginning consumption
                Expr acquire = Call::make(Int(32), "halide_semaphore_acquire", {sema}, Call::Extern);
                IRMutator::visit(op);
                stmt = Block::make(Evaluate::make(acquire), stmt);
            }
        } else {
            IRMutator::visit(op);
        }
    }

public:
    GenerateConsumerBody(const std::string &f, Expr s) : func(f), sema(s) {}
};

class ForkAsyncProducers : public IRMutator {
    using IRMutator::visit;

    const std::map<std::string, Function> &env;

    void visit(const Realize *op) {
        auto it = env.find(op->name);
        internal_assert(it != env.end());
        Function f = it->second;
        if (/* f is scheduled async */
            starts_with(f.name(), "magic_prefix")) {
            // Make two copies of the body, one which only does the
            // producer, and one which only does the consumer. Inject
            // synchronization to preserve dependencies. Put them in a
            // task-parallel block.

            // Make a semaphore.
            std::string sema_name = op->name + ".semaphore";
            Expr sema_var = Variable::make(Handle(), sema_name);

            Stmt body = op->body;
            Stmt producer = GenerateProducerBody(op->name, sema_var).mutate(body);
            Stmt consumer = GenerateConsumerBody(op->name, sema_var).mutate(body);

            // Recurse on both sides
            producer = mutate(producer);
            consumer = mutate(consumer);

            // Poor man's task parallel block
            std::string task = unique_name('t');
            Expr task_var = Variable::make(Int(32), task);
            body = IfThenElse::make(task_var == 0, producer, consumer);
            body = For::make(task, 0, 2, ForType::Parallel, DeviceAPI::None, body);

            // My poor man's semaphore will just be an int on the stack
            Expr sema_space = Call::make(Handle(), Call::alloca, {4}, Call::Intrinsic);
            Expr sema_init = Call::make(Handle(), "halide_semaphore_init", {sema_var, 0}, Call::Extern);
            body = Block::make(Evaluate::make(sema_init), body);
            body = LetStmt::make(sema_name, sema_space, body);
            stmt = Realize::make(op->name, op->types, op->bounds, op->condition, body);
        } else {
            IRMutator::visit(op);
        }
    }

public:
    ForkAsyncProducers(const std::map<std::string, Function> &e) : env(e) {}
};

Stmt fork_async_producers(Stmt s, const std::map<std::string, Function> &env) {
    // TODO: tighten up the scope of consume nodes first
    return ForkAsyncProducers(env).mutate(s);
}

}
}
