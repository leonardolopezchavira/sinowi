#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "dsp_core.h"

#if defined(__x86_64__)
#include <xbyak/xbyak.h>
#elif defined(__aarch64__)
#include <oaknut/code_block.hpp>
#include <oaknut/oaknut.hpp>
#endif

namespace hydra::dsp {

#if defined(__aarch64__)
// arm64 version of the x86-64 DSP JIT below (dsp_jit_arm64.cpp), same design and
// semantics. x19 = core, x20 = instructions executed, x21 = budget,
// x22 = registers, x23 = data memory, x24 = &executed, x25 = block table, x26 = &flush_.
class DspJit {
public:
    explicit DspJit(DspCore& core);

    std::uint64_t run(std::uint64_t budget);
    void invalidate() { flush_.store(true, std::memory_order_release); }

private:
    enum Exit : std::uint32_t { kNormal, kIdle, kBudget };
    using Enter = std::uint32_t (*)(DspCore* core, const void* code, std::uint64_t* executed, std::uint64_t budget);

    struct Instruction {
        std::uint32_t pc;
        Decoded d;
    };

    void flushAll();
    void emitCommon();
    const void* compile(std::uint32_t pc);
    void emitInstruction(const Instruction& in, std::uint32_t length, const void* blockEntry, std::uint32_t blockStart);
    void emitGoto(std::uint32_t target, std::uint32_t length, const void* blockEntry, std::uint32_t blockStart);
    void emitGotoW8(std::uint32_t length);
    void emitBackwardBranch(std::uint32_t target, std::uint32_t length, const void* blockEntry, std::uint32_t blockStart);
    void emitFallback(const Instruction& in, bool controlFlow, std::uint32_t length);
    void emitDataLoad();
    void emitDataLoad(std::uint32_t address);
    void emitDataStore();
    void emitDataStore(std::uint32_t address);
    void emitIoRead();
    void emitIoWrite();
    void emitIncrementWrites();
    void emitClampS32(oaknut::XReg x);
    void emitCall(const void* function);
    // Conditional or compare-and-branch to an address that may be out of short range.
    void emitBranchFar(oaknut::Cond cond, const void* target);
    void emitCbzFar(bool nonZero, oaknut::WReg w, const void* target);
    // Core fields by byte offset from x19 (any offset).
    void loadCore32(oaknut::WReg w, std::int32_t offset);
    void storeCore32(oaknut::WReg w, std::int32_t offset);
    void loadReg(oaknut::WReg w, int index);
    void storeReg(oaknut::WReg w, int index);
    const void* here() const;

    DspCore& core_;
    oaknut::CodeBlock block_;
    oaknut::CodeGenerator code_;
    std::array<const void*, DspCore::kProgramWords> blocks_{};
    std::atomic<bool> flush_{false};
    Enter enter_ = nullptr;
    const void* dispatch_ = nullptr;  // w8 = next pc
    const void* exitNormal_ = nullptr;
    const void* exitIdle_ = nullptr;
    const void* exitCommon_ = nullptr;

    std::int32_t pcOffset_, pendingOffset_, writesOffset_, spOffset_, stackOffset_, inInterruptOffset_, regsOffset_;
};
#elif !defined(__x86_64__)
// The DSP JIT emits x86-64; on other hosts (arm64) the interpreter runs the cores.
class DspJit {
public:
    explicit DspJit(DspCore&) {}
    std::uint64_t run(std::uint64_t) { return 0; }
    void invalidate() {}
};
#else

// Translates a DspCore's program to x86-64, a basic block at a time, with the
// interpreter's exact semantics (DspCore::interpret stays the reference):
//  - The core's registers and state stay in the DspCore; compiled code works on
//    them directly. rbx = core, r12 = instructions executed, r13 = budget,
//    r14 = registers, r15 = data memory.
//  - A block runs only if it fits into the remaining budget (else the
//    interpreter finishes the run), so runs end on the same instruction.
//  - Interrupts: run() enters compiled code only while none is pending, and
//    within a run only the host can raise one (from another thread, when the
//    DSP side runs concurrently); compiled code notices it at the next block
//    boundary. While one is pending, the interpreter steps single instructions
//    as before.
//  - Backward branches report to the idle-loop detection as in the interpreter.
//  - Rare or complex instructions call the interpreter for that one instruction.
//  - Any write to program memory (DspCore::invalidate) drops all translations
//    at the next block boundary.
class DspJit : private Xbyak::CodeGenerator {
public:
    explicit DspJit(DspCore& core);

    // DspCore::run with compiled code.
    std::uint64_t run(std::uint64_t budget);
    void invalidate() { flush_.store(true, std::memory_order_release); }

private:
    enum Exit : std::uint32_t { kNormal, kIdle, kBudget };
    using Enter = std::uint32_t (*)(DspCore* core, const void* code, std::uint64_t* executed, std::uint64_t budget);

    struct Instruction {
        std::uint32_t pc;
        Decoded d;
    };

    void flushAll();
    void emitCommon();
    const void* compile(std::uint32_t pc);
    void emitInstruction(const Instruction& in, std::uint32_t length, const void* blockEntry, std::uint32_t blockStart);
    // Control flow out of a block: count the block's instructions and go to `target`.
    void emitGoto(std::uint32_t target, std::uint32_t length, const void* blockEntry, std::uint32_t blockStart);
    void emitGotoEax(std::uint32_t length);
    void emitBackwardBranch(std::uint32_t target, std::uint32_t length, const void* blockEntry, std::uint32_t blockStart);
    void emitFallback(const Instruction& in, bool controlFlow, std::uint32_t length);
    // Data memory with the address in eax (result in eax), and stores of edx.
    void emitDataLoad();
    void emitDataLoad(std::uint32_t address);
    void emitDataStore();
    void emitDataStore(std::uint32_t address);
    void emitIoRead();
    void emitIoWrite();
    void emitIncrementWrites();
    void emitClampS32();  // rax (signed 64-bit) -> eax saturated to 32 bits
    void emitCall(const void* function);

    Xbyak::Address reg(int index) { return dword[r14 + 4 * index]; }

    DspCore& core_;
    std::array<const void*, DspCore::kProgramWords> blocks_{};
    std::atomic<bool> flush_{false};
    Enter enter_ = nullptr;
    const void* dispatch_ = nullptr;  // eax = next pc
    const void* exitNormal_ = nullptr;
    const void* exitIdle_ = nullptr;
    const void* exitCommon_ = nullptr;

    // Offsets of the core's state from the DspCore pointer.
    std::int32_t pcOffset_, pendingOffset_, writesOffset_, spOffset_, stackOffset_, inInterruptOffset_, regsOffset_;
};

#endif  // __x86_64__

}  // namespace hydra::dsp
