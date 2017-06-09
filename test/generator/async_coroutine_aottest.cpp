#include <math.h>
#include <stdio.h>
#include <assert.h>
#include <queue>

#include "HalideRuntime.h"
#include "HalideBuffer.h"
#include "async_coroutine.h"

// This test runs an async pipeline that requires multiple thread
// using a single thread and coroutines. We start with a basic x86-64
// coroutine implementation.

struct execution_context {
    char *stack_bottom = NULL;
    char *stack = NULL;
    int priority = 0;
    enum {done, running, suspended, waiting_on_semaphore} state;
};

int context_switches = 0;

void switch_context(execution_context *from, execution_context *to) {
    context_switches++;
    if (to->state != execution_context::suspended) {
        printf("Trying to switch to context in state %d\n", (int)(to->state));
        abort();
    }
    to->state = execution_context::running;

    // To switch contexts, we'll push a return address onto our own
    // stack, switch to the target stack, and then issue a ret
    // instruction, which will pop the desired return address off the
    // target stack and jump to it.
    asm volatile (
        // We need to save all callee-saved registers, plus any
        // registers that might be used inside this function after the
        // asm block. The caller of switch_context will take care of
        // caller-saved registers. Saving all GPRs is more than
        // sufficient.
        "pushq %%rax\n"
        "pushq %%rbx\n"
        "pushq %%rcx\n"
        "pushq %%rdx\n"
        "pushq %%rbp\n"
        "pushq %%rsi\n"
        "pushq %%rdi\n"
        "pushq %%r8\n"
        "pushq %%r9\n"
        "pushq %%r10\n"
        "pushq %%r11\n"
        "pushq %%r12\n"
        "pushq %%r13\n"
        "pushq %%r14\n"
        "pushq %%r15\n"
        "pushq $return_loc%=\n"
        "movq %%rsp, (%0)\n" // Save the stack pointer for the 'from' context
        "movq %1, %%rsp\n"   // Restore the stack pointer for the 'to' context
        "ret\n"              // Return into the 'to' context
        "return_loc%=:\n"    // When we re-enter the 'from' context we start here
        "popq %%r15\n"       // Restore all registers
        "popq %%r14\n"
        "popq %%r13\n"
        "popq %%r12\n"
        "popq %%r11\n"
        "popq %%r10\n"
        "popq %%r9\n"
        "popq %%r8\n"
        "popq %%rdi\n"
        "popq %%rsi\n"
        "popq %%rbp\n"
        "popq %%rdx\n"
        "popq %%rcx\n"
        "popq %%rbx\n"
        "popq %%rax\n"
        : // No outputs
        : "r"(&(from->stack)), "r"(to->stack)
        : "memory"
        );
}

// Track the number of stacks allocated
int stacks_allocated = 0;
int stacks_high_water = 0;

void call_in_new_context(execution_context *from,
                         execution_context *to,
                         void (*f)(execution_context *, execution_context *, void *),
                         void *arg) {
    // Allocate a 128k stack
    size_t sz = 128 * 1024;
    to->stack_bottom = (char *)malloc(sz);
    stacks_allocated++;
    stacks_high_water = std::max(stacks_allocated, stacks_high_water);

    // Zero it to aid debugging
    memset(to->stack_bottom, 0, sz);
    to->stack = to->stack_bottom + sz;
    to->stack = (char *)(((uintptr_t)to->stack) & ~((uintptr_t)(15)));
    to->state = execution_context::running;
    from->state = execution_context::suspended;

    // printf("Calling %p(%p, %p, %p) in a new context at %p\n", f, from, to, arg, to->stack);

    // Switching to a new context is much like switching to an
    // existing one, except we have to set up some arguments and we
    // use a callq instruction instead of a ret.
    asm volatile (
        "pushq %%rax\n"
        "pushq %%rbx\n"
        "pushq %%rcx\n"
        "pushq %%rdx\n"
        "pushq %%rbp\n"
        "pushq %%rsi\n"
        "pushq %%rdi\n"
        "pushq %%r8\n"
        "pushq %%r9\n"
        "pushq %%r10\n"
        "pushq %%r11\n"
        "pushq %%r12\n"
        "pushq %%r13\n"
        "pushq %%r14\n"
        "pushq %%r15\n"
        "pushq $return_loc%=\n"
        "movq %%rsp, (%0)\n" // Save the stack pointer for the 'from' context
        "movq %1, %%rsp\n"   // Restore the stack pointer for the 'to' context
        "movq %2, %%rdi\n"   // Set the args for the function call
        "movq %3, %%rsi\n"
        "movq %4, %%rdx\n"
        "callq *%5\n"        // Call the function inside the 'to' context
        "int $3\n"           // The function should never return, instead it should switch contexts elsewhere.
        "return_loc%=:\n"    // When we re-enter the 'from' context we start here
        "popq %%r15\n"       // Restore all registers
        "popq %%r14\n"
        "popq %%r13\n"
        "popq %%r12\n"
        "popq %%r11\n"
        "popq %%r10\n"
        "popq %%r9\n"
        "popq %%r8\n"
        "popq %%rdi\n"
        "popq %%rsi\n"
        "popq %%rbp\n"
        "popq %%rdx\n"
        "popq %%rcx\n"
        "popq %%rbx\n"
        "popq %%rax\n"
        : // No outputs
        : "r"(&(from->stack)), "r"(to->stack), "r"(from), "r"(to), "r"(arg), "r"(f)
        : "memory", "rdi", "rsi", "rdx"
        );
}

// A semaphore implementation that does the right thing with execution contexts
struct my_semaphore {
    int count = 0;
    execution_context *waiter = nullptr;
};

// We'll use a priority queue of execution contexts to decide what to
// schedule next.
struct compare_contexts {
    bool operator()(execution_context *a, execution_context *b) const {
        return a->priority < b->priority;
    }
};

std::priority_queue<execution_context *,
                    std::vector<execution_context *>,
                    compare_contexts> contexts;

std::vector<execution_context *> dead_contexts;

// The scheduler execution context. Switch to this when stalled.
execution_context scheduler_context;
void scheduler(execution_context *parent, execution_context *this_context, void *arg) {
    // The first time this is called is just to set up the scheduler's
    // context so immediately transfer control back to the parent.
    this_context->state = execution_context::suspended;
    switch_context(this_context, parent);

    while (1) {
        // Dump the queue for debugging
        decltype(contexts) other;
        printf("Priority queue:\n");
        while (!contexts.empty()) {
            auto c = contexts.top();
            contexts.pop();
            printf("  %d : %p (%d)\n", c->priority, c, (int)c->state);
            other.push(c);
        }
        other.swap(contexts);

        // Clean up any finished contexts
        for (execution_context *ctx: dead_contexts) {
            printf("Destroying context %p\n", ctx);
            if (ctx->stack_bottom) {
                stacks_allocated--;
                free(ctx->stack_bottom);
            }
            delete ctx;
        }
        dead_contexts.clear();

        // Run the next highest-priority context
        execution_context *next = contexts.top();
        contexts.pop();
        if (next->state != execution_context::suspended) {
            printf("execution context shouldn't be in queue: %p\n", next);
            abort();
        }

        printf("Running context %p with priority %d\n", next, next->priority);
        this_context->state = execution_context::suspended;
        switch_context(this_context, next);
    }
}

struct do_one_task_arg {
    halide_parallel_task_t *task;
    halide_semaphore_t *parent_semaphore;
};

void do_one_task(execution_context *parent, execution_context *this_context, void *arg) {
    do_one_task_arg *task_arg = (do_one_task_arg *)arg;
    halide_parallel_task_t *task = task_arg->task;
    halide_semaphore_t *parent_sema = task_arg->parent_semaphore;
    this_context->priority = -(task->min_threads);

    for (int i = task->min; i < task->min + task->extent; i++) {
        // Try to acquire the semaphores
        for (int j = 0; j < task->num_semaphores; j++) {
            my_semaphore *sema = (my_semaphore *)(task->semaphores[j].semaphore);
            while (!halide_semaphore_try_acquire(task->semaphores[j].semaphore, task->semaphores[j].count)) {
                if (!sema->waiter) {
                    printf("Sleeping context %p on a semaphore\n", this_context);
                    sema->waiter = this_context;
                    this_context->state = execution_context::waiting_on_semaphore;
                } else {
                    printf("Someone else (%p) already sleeping on my semaphore %p\n", sema->waiter, this_context);
                    abort();
                    this_context->priority--;
                    this_context->state = execution_context::suspended;
                    contexts.push(this_context);
                }
                switch_context(this_context, &scheduler_context);
                if (sema->waiter == this_context) {
                    printf("Woke up %p still attached to semaphore!\n", this_context);
                    abort();
                } else {
                    printf("Woke up %p not attached to semaphore\n", this_context);
                }
            }
            printf("Successfully acquired semaphore %p\n", this_context);
        }
        printf("Entering Halide\n");
        task->fn(nullptr, i, task->closure);
        printf("Leaving Halide\n");
    }
    task->extent = 0;
    this_context->state = execution_context::done;
    halide_semaphore_release(parent_sema, 1);
    dead_contexts.push_back(this_context);
    switch_context(this_context, &scheduler_context);
    // Shouldn't get scheduled again
    printf("Scheduled a finished context\n");
    abort();
}

int do_par_tasks(void *user_context, int num_tasks, halide_parallel_task_t *tasks) {

    // Make this stack frame schedulable
    execution_context *this_context = new execution_context;
    for (int i = 0; i < num_tasks; i++) {
        this_context->priority -= tasks[i].min_threads;
    }
    this_context->state = execution_context::running;

    halide_semaphore_t parent_sema;
    halide_semaphore_init(&parent_sema, 1 - num_tasks);

    // Queue up the children
    for (int i = 0; i < num_tasks; i++) {
        execution_context *ctx = new execution_context;
        do_one_task_arg arg = {tasks + i, &parent_sema};
        this_context->state = execution_context::suspended;
        contexts.push(this_context);
        call_in_new_context(this_context, ctx, do_one_task, &arg);
    }

    if (!halide_semaphore_try_acquire(&parent_sema, 1)) {
        ((my_semaphore *)(&parent_sema))->waiter = this_context;
        this_context->state = execution_context::waiting_on_semaphore;
        switch_context(this_context, &scheduler_context);
    }
    return 0;
}


int semaphore_init(halide_semaphore_t *s, int count) {
    my_semaphore *sema = (my_semaphore *)s;
    sema->count = count;
    sema->waiter = nullptr;
    return count;
}

bool semaphore_try_acquire(halide_semaphore_t *s, int count) {
    my_semaphore *sema = (my_semaphore *)s;
    if (sema->count >= count) {
        sema->count -= count;
        return true;
    } else {
        return false;
    }
}

int semaphore_release(halide_semaphore_t *s, int count) {
    my_semaphore *sema = (my_semaphore *)s;
    sema->count += count;
    if (sema->waiter) {
        // Re-enqueue the blocked context
        printf("Waking context %p\n", sema->waiter);
        if (sema->waiter->state != execution_context::waiting_on_semaphore) {
            printf("Waiting semaphore in wrong state: %d\n", (int)(sema->waiter->state));
            abort();
        }
        sema->waiter->state = execution_context::suspended;
        contexts.push(sema->waiter);
        sema->waiter = nullptr;
    }
    return sema->count;
}

int main(int argc, char **argv) {
    Halide::Runtime::Buffer<int> out(16, 16, 16);

    halide_set_custom_parallel_runtime(
        nullptr, // This pipeline shouldn't call do_par_for, and our custom override never calls do_task.
        nullptr,
        do_par_tasks,
        semaphore_init,
        semaphore_try_acquire,
        semaphore_release);

    // Start up the scheduler
    printf("Starting scheduler context\n");
    execution_context root_context;
    call_in_new_context(&root_context, &scheduler_context, scheduler, nullptr);
    printf("Scheduler running... calling into Halide.\n");

    async_coroutine(out);

    printf("Left Halide\n");

    out.for_each_element([&](int x, int y, int z) {
            int correct = 8*(x + y + z);
            if (out(x, y, z) != correct) {
                printf("out(%d, %d) = %d instead of %d\n",
                       x, y, out(x, y), correct);
                exit(-1);
            }
        });

    printf("Context switches: %d\n", context_switches);
    printf("Max stacks allocated: %d\n", stacks_high_water);
    printf("Stacks still allocated: %d\n", stacks_allocated);

    printf("Success!\n");
    return 0;
}
