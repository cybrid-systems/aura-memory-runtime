// fiber.hpp — minimal ucontext fiber bound to an Aura fiber execution
// context (third_party/aura/src/core/fiber_context.{hh,cpp}, linked as-is).
//
// Aura's full scheduler Fiber (src/serve/fiber.h) cannot be linked on its own:
// fiber.cpp pulls in the evaluator, GC coordinator, mailbox and mutation
// boundary machinery. What IS reusable is the per-fiber execution Context
// that Fiber::resume() attaches / detaches; we drive it the same way.
#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <ucontext.h>

namespace aura::core::fiber_ctx {
struct Context;
}

namespace amr {

class Fiber {
public:
    explicit Fiber(std::uint64_t id, std::size_t stack_bytes = 256 * 1024);
    ~Fiber();
    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;

    // Run `job` on this fiber's stack (with this fiber's Aura Context
    // attached) until it finishes. The job may call Fiber::yield(); run()
    // keeps resuming it until completion. Exceptions are rethrown here.
    void run(const std::function<void()>& job);
    static void yield();
    static Fiber* current();
    [[nodiscard]] std::uint64_t id() const { return id_; }
    [[nodiscard]] std::uint64_t switches() const { return switches_; }
    [[nodiscard]] aura::core::fiber_ctx::Context* aura_context() const { return ctx_; }

private:
    static void trampoline();
    void resume_once();
    std::uint64_t id_;
    std::unique_ptr<char[]> stack_;
    std::size_t stack_bytes_;
    ucontext_t self_{};
    ucontext_t caller_{};
    const std::function<void()>* job_ = nullptr;
    bool job_done_ = true;
    bool started_ = false;
    std::exception_ptr err_;
    std::uint64_t switches_ = 0;
    aura::core::fiber_ctx::Context* ctx_ = nullptr;
};

} // namespace amr
