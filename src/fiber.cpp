#include "amr/fiber.hpp"

#include "core/fiber_context.hh"

#include <stdexcept>

namespace amr {

namespace fc = aura::core::fiber_ctx;

namespace {
thread_local Fiber* t_current = nullptr;
thread_local Fiber* t_starting = nullptr;
} // namespace

Fiber::Fiber(std::uint64_t id, std::size_t stack_bytes)
    : id_(id), stack_(new char[stack_bytes]), stack_bytes_(stack_bytes) {
    ctx_ = fc::aura_fiber_ctx_create(id);
    if (!ctx_)
        throw std::bad_alloc();
}

Fiber::~Fiber() { fc::aura_fiber_ctx_destroy(ctx_); }

Fiber* Fiber::current() { return t_current; }

void Fiber::trampoline() {
    Fiber* self = t_starting;
    for (;;) {
        if (self->job_) {
            try {
                (*self->job_)();
            } catch (...) {
                self->err_ = std::current_exception();
            }
            self->job_ = nullptr;
            self->job_done_ = true;
        }
        swapcontext(&self->self_, &self->caller_);
    }
}

void Fiber::resume_once() {
    if (!started_) {
        getcontext(&self_);
        self_.uc_stack.ss_sp = stack_.get();
        self_.uc_stack.ss_size = stack_bytes_;
        self_.uc_link = nullptr;
        makecontext(&self_, &Fiber::trampoline, 0);
        started_ = true;
        t_starting = this;
    }
    Fiber* prev = t_current;
    t_current = this;
    fc::aura_fiber_ctx_attach(ctx_); // same attach/detach discipline as Aura Fiber::resume
    ++switches_;
    swapcontext(&caller_, &self_);
    fc::aura_fiber_ctx_detach();
    t_current = prev;
}

void Fiber::run(const std::function<void()>& job) {
    if (t_current == this)
        throw std::logic_error("Fiber::run re-entered from its own body");
    job_ = &job;
    job_done_ = false;
    err_ = nullptr;
    while (!job_done_)
        resume_once();
    if (err_)
        std::rethrow_exception(err_);
}

void Fiber::yield() {
    Fiber* f = t_current;
    if (!f)
        return;
    swapcontext(&f->self_, &f->caller_);
}

} // namespace amr
