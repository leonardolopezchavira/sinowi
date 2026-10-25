// arm64 (Apple Silicon, arm64 Linux) translation of the DSP cores' programs.
// A port of the x86-64 DspJit in dsp_jit.cpp: same block structure, dispatch,
// budget handling, idle detection and interpreter fallbacks, so it stays
// bit-exact with DspCore::interpret.
#include "dsp_jit.h"

#if defined(__aarch64__)

#include <vector>

namespace hydra::dsp {

using namespace oaknut::util;
using oaknut::Cond;
using oaknut::IndexExt;
using oaknut::Label;
using oaknut::WReg;
using oaknut::XReg;

namespace {
constexpr std::size_t kCodeSize = 4u << 20;
constexpr std::size_t kBlockReserve = 64u << 10;  // more than the largest block can take
constexpr std::uint32_t kMaxBlock = 64;           // instructions

bool isControlFlow(Op op) {
    switch (op) {
    case Op::BranchGreater: case Op::BranchGreaterEqual: case Op::BranchEqual: case Op::BranchNotEqual:
    case Op::BranchHighEqualLow: case Op::BranchHighNotEqualLow: case Op::BranchLowEqual: case Op::BranchLowNotEqual:
    case Op::BranchBitSet: case Op::BranchBitClear:
    case Op::JumpIfZero: case Op::JumpIfNotZero: case Op::Loop: case Op::Call: case Op::JumpLink: case Op::Jump:
    case Op::Return: case Op::JumpRegister: case Op::ReturnFromInterrupt:
        return true;
    default:
        return false;
    }
}

template <typename T>
std::int32_t offsetIn(const DspCore& core, const T& member) {
    return static_cast<std::int32_t>(reinterpret_cast<const char*>(&member) - reinterpret_cast<const char*>(&core));
}

Cond invertCond(Cond c) { return static_cast<Cond>(static_cast<int>(c) ^ 1); }

std::uint64_t u64(const void* p) { return reinterpret_cast<std::uint64_t>(p); }

// Register use: x19 core, x20 executed, x21 budget, x22 registers, x23 data memory,
// x24 &executed, x25 block table, x26 &flush_ (all callee-saved).
// w8 accumulator ("eax"), w9 ("ecx"), w10 value ("edx"), w11/w12 temporaries,
// x14 immediates, x15 addresses, x16 call target.
}  // namespace

// Entry points for compiled code (AAPCS64: core in x0, arguments in w1, w2).
struct JitThunks {
    static std::uint32_t step(DspCore* core) { return static_cast<std::uint32_t>(core->interpret(2, 1)); }
    static std::uint32_t idle(DspCore* core, std::uint32_t target) { return core->branchIdle(target) ? 1 : 0; }
    static std::uint32_t dataRegRead(DspCore* core, std::uint32_t address) {
        return core->host_.dataRegRead(core->id_, address);
    }
    static void dataRegWrite(DspCore* core, std::uint32_t address, std::uint32_t value) {
        core->host_.dataRegWrite(core->id_, address, value);
    }
    static std::uint32_t ioRead(DspCore* core, std::uint32_t address) { return core->host_.ioRead(core->id_, address); }
    static void ioWrite(DspCore* core, std::uint32_t address, std::uint32_t value) {
        core->host_.ioWrite(core->id_, address, value);
    }
};

DspJit::DspJit(DspCore& core) : core_(core), block_(kCodeSize), code_(block_.ptr()) {
    pcOffset_ = offsetIn(core, core.pc_);
    pendingOffset_ = offsetIn(core, core.pending_);
    writesOffset_ = offsetIn(core, core.writes_);
    spOffset_ = offsetIn(core, core.sp_);
    stackOffset_ = offsetIn(core, core.stack_);
    inInterruptOffset_ = offsetIn(core, core.inInterrupt_);
    regsOffset_ = offsetIn(core, core.r_);
    block_.unprotect();
    emitCommon();
    block_.protect();
    block_.invalidate_all();
}

const void* DspJit::here() const { return code_.xptr<const std::uint32_t*>(); }

void DspJit::flushAll() {
    code_.set_offset(0);
    blocks_.fill(nullptr);
    emitCommon();
}

void DspJit::emitCall(const void* function) {
    code_.MOV(X16, u64(function));
    code_.BLR(X16);
}

void DspJit::emitBranchFar(Cond cond, const void* target) {
    Label skip;
    code_.B(invertCond(cond), skip);
    code_.B(target);
    code_.l(skip);
}

void DspJit::emitCbzFar(bool nonZero, WReg w, const void* target) {
    Label skip;
    if (nonZero)
        code_.CBZ(w, skip);
    else
        code_.CBNZ(w, skip);
    code_.B(target);
    code_.l(skip);
}

void DspJit::loadCore32(WReg w, std::int32_t offset) {
    if (offset >= 0 && offset % 4 == 0 && offset < 16384) {
        code_.LDR(w, X19, offset);
    } else {
        code_.MOV(X15, static_cast<std::uint64_t>(static_cast<std::int64_t>(offset)));
        code_.LDR(w, X19, X15);
    }
}

void DspJit::storeCore32(WReg w, std::int32_t offset) {
    if (offset >= 0 && offset % 4 == 0 && offset < 16384) {
        code_.STR(w, X19, offset);
    } else {
        code_.MOV(X15, static_cast<std::uint64_t>(static_cast<std::int64_t>(offset)));
        code_.STR(w, X19, X15);
    }
}

void DspJit::loadReg(WReg w, int index) { code_.LDR(w, X22, (index & 15) * 4); }
void DspJit::storeReg(WReg w, int index) { code_.STR(w, X22, (index & 15) * 4); }

void DspJit::emitCommon() {
    auto& c = code_;
    // enter(core, code, &executed, budget)
    enter_ = reinterpret_cast<Enter>(const_cast<void*>(here()));
    c.STP(X29, X30, SP, oaknut::PreIndexed{}, -96);
    c.STP(X19, X20, SP, 16);
    c.STP(X21, X22, SP, 32);
    c.STP(X23, X24, SP, 48);
    c.STP(X25, X26, SP, 64);
    c.MOV(X19, X0);
    c.MOV(X24, X2);
    c.LDR(X20, X2);
    c.MOV(X21, X3);
    c.MOV(X14, static_cast<std::uint64_t>(regsOffset_));
    c.ADD(X22, X19, X14);
    c.MOV(X23, u64(core_.mem_.data));
    c.MOV(X25, u64(blocks_.data()));
    c.MOV(X26, u64(&flush_));
    c.BR(X1);

    exitCommon_ = here();
    c.STR(X20, X24);
    c.LDP(X25, X26, SP, 64);
    c.LDP(X23, X24, SP, 48);
    c.LDP(X21, X22, SP, 32);
    c.LDP(X19, X20, SP, 16);
    c.LDP(X29, X30, SP, oaknut::PostIndexed{}, 96);
    c.RET();

    exitNormal_ = here();
    c.MOV(W0, static_cast<std::uint32_t>(kNormal));
    c.B(exitCommon_);

    exitIdle_ = here();
    c.MOV(W0, static_cast<std::uint32_t>(kIdle));
    c.B(exitCommon_);

    // w8 = next pc: on to its block if it is compiled, nothing is pending and
    // the budget is not used up; else back to run().
    dispatch_ = here();
    storeCore32(W8, pcOffset_);
    loadCore32(W9, pendingOffset_);
    c.CBNZ(W9, exitNormal_);
    c.LDRB(W9, X26);
    c.CBNZ(W9, exitNormal_);
    c.CMP(X20, X21);
    c.B(Cond::HS, exitNormal_);
    c.AND(W8, W8, 0xFFF);
    c.LDR(X9, X25, X8, IndexExt::LSL, 3);
    c.CBZ(X9, exitNormal_);
    c.BR(X9);
}

std::uint64_t DspJit::run(std::uint64_t budget) {
    std::uint64_t executed = 0;
    while (executed < budget) {
        if (core_.trapped_)
            return executed;
        if (flush_.exchange(false, std::memory_order_acq_rel)) {
            block_.unprotect();
            flushAll();
            block_.protect();
            block_.invalidate(block_.ptr(), static_cast<std::size_t>(code_.offset()));
        }
        if (core_.pending_ != 0) {
            core_.takeInterrupt();
            if (core_.pending_ != 0) {  // not deliverable now: step as the interpreter does
                executed += core_.interpret(budget - executed, 1);
                continue;
            }
        }
        const std::uint32_t pc = core_.pc_ & 0xFFF;
        const void* code = blocks_[pc] ? blocks_[pc] : compile(pc);
        if (!code) {
            executed += core_.interpret(budget - executed, 1);
            continue;
        }
        switch (enter_(&core_, code, &executed, budget)) {
        case kIdle: return budget;
        case kBudget: return executed + core_.interpret(budget - executed, budget - executed);
        default: break;
        }
    }
    return executed;
}

const void* DspJit::compile(std::uint32_t start) {
    std::vector<Instruction> block;
    for (std::uint32_t pc = start; block.size() < kMaxBlock; pc = (pc + 1) & 0xFFF) {
        const Decoded d = decode(core_.mem_.program[pc]);
        if (d.op == Op::Invalid)
            break;
        block.push_back({pc, d});
        if (isControlFlow(d.op))
            break;
    }
    if (block.empty())
        return nullptr;

    block_.unprotect();
    std::ptrdiff_t from = code_.offset();
    if (static_cast<std::size_t>(from) + kBlockReserve > kCodeSize) {
        flushAll();
        from = 0;
    }

    auto& c = code_;
    const auto length = static_cast<std::uint32_t>(block.size());
    c.align(16);
    const void* entry = here();
    Label overBudget;
    c.ADD(X9, X20, length);
    c.CMP(X9, X21);
    c.B(Cond::HI, overBudget);
    for (const Instruction& in : block)
        emitInstruction(in, length, entry, start);
    if (!isControlFlow(block.back().d.op))
        emitGoto((block.back().pc + 1) & 0xFFF, length, entry, start);
    c.l(overBudget);
    c.MOV(W9, start);
    storeCore32(W9, pcOffset_);
    c.MOV(W0, static_cast<std::uint32_t>(kBudget));
    c.B(exitCommon_);

    block_.protect();
    block_.invalidate(block_.ptr() + from / 4, static_cast<std::size_t>(code_.offset() - from));
    blocks_[start] = entry;
    return entry;
}

void DspJit::emitGoto(std::uint32_t target, std::uint32_t length, const void* blockEntry, std::uint32_t blockStart) {
    code_.ADD(X20, X20, length);
    code_.MOV(W8, target);
    const void* direct = target == blockStart ? blockEntry : target <= 0xFFF ? blocks_[target] : nullptr;
    if (direct) {
        // Straight on to a compiled block unless the host raised an interrupt
        // meanwhile (the dispatcher hands that over to run()).
        loadCore32(W9, pendingOffset_);
        emitCbzFar(true, W9, dispatch_);
        code_.B(direct);
    } else {
        code_.B(dispatch_);
    }
}

void DspJit::emitGotoW8(std::uint32_t length) {
    code_.ADD(X20, X20, length);
    code_.B(dispatch_);
}

void DspJit::emitBackwardBranch(std::uint32_t target, std::uint32_t length, const void* blockEntry, std::uint32_t blockStart) {
    // As the interpreter: pc_ = target, then ask the idle-loop detection.
    code_.MOV(W9, target);
    storeCore32(W9, pcOffset_);
    code_.MOV(X0, X19);
    code_.MOV(W1, target);
    emitCall(reinterpret_cast<const void*>(&JitThunks::idle));
    emitCbzFar(true, W0, exitIdle_);
    emitGoto(target, length, blockEntry, blockStart);
}

void DspJit::emitFallback(const Instruction& in, bool controlFlow, std::uint32_t length) {
    code_.MOV(W9, in.pc);
    storeCore32(W9, pcOffset_);
    code_.MOV(X0, X19);
    emitCall(reinterpret_cast<const void*>(&JitThunks::step));
    if (controlFlow) {
        code_.CMP(W0, 1);
        emitBranchFar(Cond::NE, exitIdle_);
        loadCore32(W8, pcOffset_);
        emitGotoW8(length);
    }
}

void DspJit::emitIncrementWrites() {
    code_.MOV(X15, static_cast<std::uint64_t>(writesOffset_));
    code_.LDR(X12, X19, X15);
    code_.ADD(X12, X12, 1);
    code_.STR(X12, X19, X15);
}

void DspJit::emitClampS32(XReg x) {
    code_.MOV(X14, 0x7FFFFFFFull);
    code_.CMP(x, X14);
    code_.CSEL(x, X14, x, Cond::GT);
    code_.MOV(X14, 0xFFFFFFFF80000000ull);
    code_.CMP(x, X14);
    code_.CSEL(x, X14, x, Cond::LT);
}

// Address in w8, result in w8.
void DspJit::emitDataLoad() {
    const DspCore::Memory& m = core_.mem_;
    auto& c = code_;
    Label notRegister, host, done;
    c.MOV(W14, m.dataMask);
    c.AND(W8, W8, W14);
    c.CMP(W8, 15);
    c.B(Cond::HI, notRegister);
    c.LDR(W8, X22, X8, IndexExt::LSL, 2);
    c.B(done);
    c.l(notRegister);
    if (m.dataLimit <= m.dataMask) {
        c.MOV(W14, m.dataLimit);
        c.CMP(W8, W14);
        c.B(Cond::HS, host);
    }
    c.LDR(W8, X23, X8, IndexExt::LSL, 2);
    c.B(done);
    c.l(host);
    if (m.dataLimit <= m.dataMask) {
        c.MOV(W1, W8);
        c.MOV(X0, X19);
        emitCall(reinterpret_cast<const void*>(&JitThunks::dataRegRead));
        c.MOV(W8, W0);
    }
    c.l(done);
}

void DspJit::emitDataLoad(std::uint32_t address) {
    const DspCore::Memory& m = core_.mem_;
    address &= m.dataMask;
    if (address <= 15) {
        loadReg(W8, static_cast<int>(address));
    } else if (address >= m.dataLimit) {
        code_.MOV(W1, address);
        code_.MOV(X0, X19);
        emitCall(reinterpret_cast<const void*>(&JitThunks::dataRegRead));
        code_.MOV(W8, W0);
    } else if (address < 4096) {
        code_.LDR(W8, X23, address * 4);
    } else {
        code_.MOV(W15, address);
        code_.LDR(W8, X23, X15, IndexExt::LSL, 2);
    }
}

// Address in w8, value in w10.
void DspJit::emitDataStore() {
    const DspCore::Memory& m = core_.mem_;
    auto& c = code_;
    Label notRegister, host, done;
    c.MOV(W14, m.dataMask);
    c.AND(W8, W8, W14);
    c.CMP(W8, 15);
    c.B(Cond::HI, notRegister);
    c.STR(W10, X22, X8, IndexExt::LSL, 2);
    c.B(done);
    c.l(notRegister);
    if (m.dataLimit <= m.dataMask) {
        c.MOV(W14, m.dataLimit);
        c.CMP(W8, W14);
        c.B(Cond::HS, host);
    }
    c.STR(W10, X23, X8, IndexExt::LSL, 2);
    c.B(done);
    c.l(host);
    if (m.dataLimit <= m.dataMask) {
        c.MOV(W1, W8);
        c.MOV(W2, W10);
        c.MOV(X0, X19);
        emitCall(reinterpret_cast<const void*>(&JitThunks::dataRegWrite));
    }
    c.l(done);
}

void DspJit::emitDataStore(std::uint32_t address) {
    const DspCore::Memory& m = core_.mem_;
    address &= m.dataMask;
    if (address <= 15) {
        storeReg(W10, static_cast<int>(address));
    } else if (address >= m.dataLimit) {
        code_.MOV(W1, address);
        code_.MOV(W2, W10);
        code_.MOV(X0, X19);
        emitCall(reinterpret_cast<const void*>(&JitThunks::dataRegWrite));
    } else if (address < 4096) {
        code_.STR(W10, X23, address * 4);
    } else {
        code_.MOV(W15, address);
        code_.STR(W10, X23, X15, IndexExt::LSL, 2);
    }
}

// Shared RAM at io 0x8000-0xFEFF and 0x10000-0x1FBFF (word = address - 0x8000);
// the rest goes to the host. Address in w8 (17 bits), result in w8.
void DspJit::emitIoRead() {
    auto& c = code_;
    Label shared, done;
    if (core_.mem_.shared) {
        c.SUB(W9, W8, 0x8000);
        c.MOV(W14, 0xFF00 - 0x8000);
        c.CMP(W9, W14);
        c.B(Cond::LO, shared);
        c.SUB(W11, W9, 0x8000);
        c.MOV(W14, 0x1FC00 - 0x10000);
        c.CMP(W11, W14);
        c.B(Cond::LO, shared);
    }
    c.MOV(W1, W8);
    c.MOV(X0, X19);
    emitCall(reinterpret_cast<const void*>(&JitThunks::ioRead));
    c.MOV(W8, W0);
    if (core_.mem_.shared) {
        c.B(done);
        c.l(shared);
        c.MOV(X14, u64(core_.mem_.shared));
        c.LDR(W8, X14, X9, IndexExt::LSL, 2);
    }
    c.l(done);
}

// Address in w8, value in w10.
void DspJit::emitIoWrite() {
    auto& c = code_;
    emitIncrementWrites();
    Label shared, done;
    if (core_.mem_.shared) {
        c.SUB(W9, W8, 0x8000);
        c.MOV(W14, 0xFF00 - 0x8000);
        c.CMP(W9, W14);
        c.B(Cond::LO, shared);
        c.SUB(W12, W9, 0x8000);
        c.MOV(W14, 0x1FC00 - 0x10000);
        c.CMP(W12, W14);
        c.B(Cond::LO, shared);
    }
    c.MOV(W1, W8);
    c.MOV(W2, W10);
    c.MOV(X0, X19);
    emitCall(reinterpret_cast<const void*>(&JitThunks::ioWrite));
    if (core_.mem_.shared) {
        c.B(done);
        c.l(shared);
        c.MOV(X14, u64(core_.mem_.shared));
        c.STR(W10, X14, X9, IndexExt::LSL, 2);
    }
    c.l(done);
}

void DspJit::emitInstruction(const Instruction& in, std::uint32_t length, const void* blockEntry, std::uint32_t blockStart) {
    auto& c = code_;
    const Decoded& d = in.d;
    const std::uint32_t pc = in.pc, next = (pc + 1) & 0xFFF;
    const int ra = d.a, rb = d.b, rc = d.c;
    const bool ioHook = static_cast<bool>(core_.onIoWrite);

    // Conditional relative branch: taken -> pc + offset + 1, backward ones with
    // idle-loop detection.
    const auto branch = [&](auto emitJumpIfTaken) {
        Label taken;
        emitJumpIfTaken(taken);
        emitGoto(next, length, blockEntry, blockStart);
        c.l(taken);
        const std::uint32_t target = (pc + static_cast<std::uint32_t>(d.offset) + 1) & 0xFFF;
        if (d.offset < 0)
            emitBackwardBranch(target, length, blockEntry, blockStart);
        else
            emitGoto(target, length, blockEntry, blockStart);
    };
    const auto compareBranch = [&](Cond cond) {
        branch([&](Label& taken) {
            loadReg(W8, ra);
            loadReg(W9, rb);
            c.CMP(W8, W9);
            c.B(cond, taken);
        });
    };
    // Absolute jump; idle detection when enabled and backward.
    const auto jumpTo = [&](std::uint32_t target) {
        if (core_.jumpIdleDetection_ && target <= pc)
            emitBackwardBranch(target & 0xFFF, length, blockEntry, blockStart);
        else
            emitGoto(target & 0xFFF, length, blockEntry, blockStart);
    };
    const auto addImmediate = [&](WReg dst, WReg src, std::uint32_t value) {
        c.MOV(W14, value);
        c.ADD(dst, src, W14);
    };

    switch (d.op) {
    case Op::Nop: break;

    case Op::Load:
        emitDataLoad(d.operand);
        storeReg(W8, ra);
        break;
    case Op::Store:
        emitIncrementWrites();
        loadReg(W10, ra);
        emitDataStore(d.operand);
        break;
    case Op::LoadIndexed:
        loadReg(W8, rb);
        addImmediate(W8, W8, d.operand);
        emitDataLoad();
        storeReg(W8, ra);
        break;
    case Op::StoreIndexed:
        emitIncrementWrites();
        loadReg(W8, rb);
        addImmediate(W8, W8, d.operand);
        loadReg(W10, ra);
        emitDataStore();
        break;
    case Op::LoadReg:
        loadReg(W8, rb);
        emitDataLoad();
        storeReg(W8, ra);
        break;
    case Op::LoadRegInc:
        loadReg(W8, rb);
        c.ADD(W9, W8, 1);
        storeReg(W9, rb);
        emitDataLoad();
        storeReg(W8, ra);
        break;
    case Op::StoreReg:
        emitIncrementWrites();
        loadReg(W8, ra);
        loadReg(W10, rb);
        emitDataStore();
        break;
    case Op::StoreRegInc:
        loadReg(W8, ra);
        c.ADD(W9, W8, 1);
        storeReg(W9, ra);
        emitIncrementWrites();
        loadReg(W10, rb);
        emitDataStore();
        break;

    case Op::IoLoad:
        c.MOV(W8, d.operand & 0x1FFFF);
        emitIoRead();
        storeReg(W8, ra);
        break;
    case Op::IoLoadReg:
        loadReg(W8, rb);
        c.AND(W8, W8, 0x1FFFF);
        emitIoRead();
        storeReg(W8, ra);
        break;
    case Op::IoLoadRegInc:
        loadReg(W8, rb);
        c.ADD(W9, W8, 1);
        storeReg(W9, rb);
        c.AND(W8, W8, 0x1FFFF);
        emitIoRead();
        storeReg(W8, ra);
        break;
    case Op::IoStore:
        if (ioHook) {
            emitFallback(in, false, length);
            break;
        }
        c.MOV(W8, d.operand & 0x1FFFF);
        loadReg(W10, ra);
        emitIoWrite();
        break;
    case Op::IoStoreReg:
        if (ioHook) {
            emitFallback(in, false, length);
            break;
        }
        loadReg(W8, ra);
        c.AND(W8, W8, 0x1FFFF);
        loadReg(W10, rb);
        emitIoWrite();
        break;
    case Op::IoStoreRegInc:
        if (ioHook) {
            emitFallback(in, false, length);
            break;
        }
        loadReg(W8, ra);
        c.ADD(W9, W8, 1);
        storeReg(W9, ra);
        c.AND(W8, W8, 0x1FFFF);
        loadReg(W10, rb);
        emitIoWrite();
        break;

    case Op::AddImm:
    case Op::SubImm:
        loadReg(W8, ra);
        c.MOV(W14, d.operand);
        if (d.op == Op::AddImm)
            c.ADD(W8, W8, W14);
        else
            c.SUB(W8, W8, W14);
        storeReg(W8, ra);
        break;
    case Op::MovImm:
        c.MOV(W8, d.operand);
        storeReg(W8, ra);
        break;
    case Op::AddImmLow:
    case Op::SubImmLow:
        loadReg(W8, ra);
        c.MOV(W14, d.operand);
        if (d.op == Op::AddImmLow)
            c.ADD(W9, W8, W14);
        else
            c.SUB(W9, W8, W14);
        c.BFI(W8, W9, 0, 16);
        storeReg(W8, ra);
        break;
    case Op::MovImmLow:
        loadReg(W8, ra);
        c.MOV(W14, 0xFFFF0000u);
        c.AND(W8, W8, W14);
        c.MOV(W14, d.operand);
        c.ORR(W8, W8, W14);
        storeReg(W8, ra);
        break;
    case Op::AndImmLow:
        loadReg(W8, ra);
        c.MOV(W14, d.operand | 0xFFFF0000u);
        c.AND(W8, W8, W14);
        storeReg(W8, ra);
        break;
    case Op::OrImmLow:
        loadReg(W8, ra);
        c.MOV(W14, d.operand);
        c.ORR(W8, W8, W14);
        storeReg(W8, ra);
        break;
    case Op::MovImmHigh:
        loadReg(W8, ra);
        c.MOV(W14, 0xFFFFu);
        c.AND(W8, W8, W14);
        c.MOV(W14, d.operand << 16);
        c.ORR(W8, W8, W14);
        storeReg(W8, ra);
        break;
    case Op::AndImmHigh:
        loadReg(W8, ra);
        c.MOV(W14, d.operand << 16 | 0xFFFFu);
        c.AND(W8, W8, W14);
        storeReg(W8, ra);
        break;

    case Op::Mul:
    case Op::MulAcc:
        loadReg(W8, ra);
        loadReg(W9, rb);
        c.SMULL(X8, W8, W9);
        if (d.shift & 63)
            c.ASR(X8, X8, d.shift & 63);
        if (d.op == Op::Mul) {
            storeReg(W8, rc);
        } else {
            loadReg(W9, rc);
            c.ADD(W9, W9, W8);
            storeReg(W9, rc);
        }
        break;
    case Op::MulMem:
        emitDataLoad(d.operand);
        loadReg(W9, ra);
        c.SMULL(X8, W9, W8);
        if (d.shift & 63)
            c.ASR(X8, X8, d.shift & 63);
        storeReg(W8, ra);
        break;
    case Op::AddMem:
    case Op::SubMem:
        emitDataLoad(d.operand);
        loadReg(W9, ra);
        if (d.op == Op::AddMem)
            c.ADD(W9, W9, W8);
        else
            c.SUB(W9, W9, W8);
        storeReg(W9, ra);
        break;
    case Op::AddMemSat:
    case Op::AddMemSatStore:
        if (d.op == Op::AddMemSatStore)
            emitIncrementWrites();
        emitDataLoad(d.operand);
        c.SXTW(X10, W8);
        loadReg(W9, ra);
        c.SXTW(X8, W9);
        c.ADD(X8, X8, X10);
        emitClampS32(X8);
        if (d.op == Op::AddMemSat) {
            storeReg(W8, ra);
        } else {
            c.MOV(W10, W8);
            emitDataStore(d.operand);
        }
        break;

    case Op::Add:
    case Op::Sub:
    case Op::And:
    case Op::Or:
        loadReg(W8, ra);
        loadReg(W9, rb);
        switch (d.op) {
        case Op::Add: c.ADD(W8, W8, W9); break;
        case Op::Sub: c.SUB(W8, W8, W9); break;
        case Op::And: c.AND(W8, W8, W9); break;
        default: c.ORR(W8, W8, W9); break;
        }
        storeReg(W8, rc);
        break;
    case Op::AddSat:
    case Op::SubSat:
        loadReg(W8, ra);
        loadReg(W9, rb);
        c.SXTW(X8, W8);
        c.SXTW(X10, W9);
        if (d.op == Op::AddSat)
            c.ADD(X8, X8, X10);
        else
            c.SUB(X8, X8, X10);
        emitClampS32(X8);
        storeReg(W8, rc);
        break;
    case Op::Shl:
    case Op::Sar:
        loadReg(W8, ra);
        loadReg(W9, rb);
        c.AND(W9, W9, 31);
        if (d.op == Op::Shl)
            c.LSL(W8, W8, W9);
        else
            c.ASR(W8, W8, W9);
        storeReg(W8, rc);
        break;
    case Op::Max:
    case Op::Min:
        loadReg(W8, ra);
        loadReg(W9, rb);
        c.CMP(W8, W9);
        c.CSEL(W8, W9, W8, d.op == Op::Max ? Cond::LE : Cond::GE);
        storeReg(W8, rc);
        break;
    case Op::SarImm:
    case Op::ShlImm:
    case Op::ShrImm:
        loadReg(W8, ra);
        c.MOV(W9, static_cast<std::uint32_t>(d.shift));  // modulo 32, as the interpreter's variable shift
        if (d.op == Op::SarImm)
            c.ASR(W8, W8, W9);
        else if (d.op == Op::ShlImm)
            c.LSL(W8, W8, W9);
        else
            c.LSR(W8, W8, W9);
        storeReg(W8, rc);
        break;

    case Op::Mov:
        loadReg(W8, rb);
        storeReg(W8, ra);
        break;
    case Op::MovHighToLow:
        loadReg(W8, rb);
        c.LSR(W8, W8, 16);
        storeReg(W8, ra);
        break;
    case Op::MovLowToHigh:
        loadReg(W8, rb);
        c.LSL(W8, W8, 16);
        storeReg(W8, ra);
        break;
    case Op::MovHigh:
        loadReg(W8, rb);
        c.MOV(W14, 0xFFFF0000u);
        c.AND(W8, W8, W14);
        storeReg(W8, ra);
        break;
    case Op::NegSat:
        loadReg(W8, ra);
        c.SXTW(X8, W8);
        c.NEG(X8, X8);
        c.MOV(X14, 0x7FFFFFFFull);
        c.CMP(X8, X14);
        c.CSEL(X8, X14, X8, Cond::GT);
        storeReg(W8, ra);
        break;
    case Op::AbsSat:
        loadReg(W8, ra);
        c.SXTW(X8, W8);
        c.NEG(X10, X8);
        c.TST(X10, X10);
        c.CSEL(X10, X8, X10, Cond::MI);  // -x negative: x was positive
        c.MOV(X14, 0x7FFFFFFFull);
        c.CMP(X10, X14);
        c.CSEL(X10, X14, X10, Cond::GT);
        storeReg(W10, rc);
        break;

    case Op::BranchGreater: compareBranch(Cond::GT); break;
    case Op::BranchGreaterEqual: compareBranch(Cond::GE); break;
    case Op::BranchEqual: compareBranch(Cond::EQ); break;
    case Op::BranchNotEqual: compareBranch(Cond::NE); break;
    case Op::BranchBitSet:
        branch([&](Label& taken) { loadReg(W8, ra); c.TBNZ(W8, d.c & 31, taken); });
        break;
    case Op::BranchBitClear:
        branch([&](Label& taken) { loadReg(W8, ra); c.TBZ(W8, d.c & 31, taken); });
        break;
    case Op::JumpIfZero:
    case Op::JumpIfNotZero: {
        Label taken;
        loadReg(W8, ra);
        if (d.op == Op::JumpIfZero)
            c.CBZ(W8, taken);
        else
            c.CBNZ(W8, taken);
        emitGoto(next, length, blockEntry, blockStart);
        c.l(taken);
        jumpTo(d.operand);
        break;
    }
    case Op::Jump: jumpTo(d.operand); break;
    case Op::Loop: {
        // if (ra-- != 1) jump to the operand
        Label done;
        loadReg(W8, ra);
        c.SUB(W9, W8, 1);
        storeReg(W9, ra);
        c.CMP(W8, 1);
        c.B(Cond::EQ, done);
        emitGoto(d.operand, length, blockEntry, blockStart);
        c.l(done);
        emitGoto(next, length, blockEntry, blockStart);
        break;
    }
    case Op::Call:
        loadCore32(W8, spOffset_);
        c.ADD(W9, W8, 1);
        storeCore32(W9, spOffset_);
        c.AND(W8, W8, 0xF);
        c.MOV(W10, next);
        c.MOV(X15, static_cast<std::uint64_t>(stackOffset_));
        c.ADD(X15, X19, X15);
        c.STR(W10, X15, X8, IndexExt::LSL, 2);
        emitGoto(d.operand, length, blockEntry, blockStart);
        break;
    case Op::Return:
        loadCore32(W8, spOffset_);
        c.SUB(W8, W8, 1);
        storeCore32(W8, spOffset_);
        c.AND(W8, W8, 0xF);
        c.MOV(X15, static_cast<std::uint64_t>(stackOffset_));
        c.ADD(X15, X19, X15);
        c.LDR(W8, X15, X8, IndexExt::LSL, 2);
        c.AND(W8, W8, 0xFFF);
        emitGotoW8(length);
        break;
    case Op::JumpLink:
        c.MOV(W9, next);
        storeReg(W9, ra);
        emitGoto(d.operand, length, blockEntry, blockStart);
        break;
    case Op::JumpRegister:
        loadReg(W8, ra);
        c.AND(W8, W8, 0xFFF);
        emitGotoW8(length);
        break;
    case Op::ReturnFromInterrupt:
        loadReg(W8, 15);
        c.LSR(W8, W8, 16);
        c.AND(W8, W8, 0xFFF);
        c.MOV(W9, 0u);
        c.MOV(X15, static_cast<std::uint64_t>(inInterruptOffset_));
        c.STRB(W9, X19, X15);
        loadReg(W9, 15);
        c.ORR(W9, W9, 1);
        storeReg(W9, 15);
        emitGotoW8(length);
        break;

    default:  // rare or complex: one step of the interpreter
        emitFallback(in, isControlFlow(d.op), length);
        break;
    }
}

}  // namespace hydra::dsp

#endif  // __aarch64__
