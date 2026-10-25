#include "dsp_block.h"

#include <algorithm>
#include <bit>
#include <chrono>

#include "host_arch.h"

#include "threads.h"

namespace hydra {

DspBlock::DspBlock() : shared_(dsp::DspCore::kSharedWords), spareMailboxes_(0x300), ports_(kPortWords) {
    for (int i = 0; i < kCores; ++i) {
        Core& c = cores_[i];
        c.dsp = std::make_unique<dsp::DspCore>(
            i, *this, dsp::DspCore::Memory{c.program.data(), c.data.data(), 0x1FF, shared_.data()});
    }
}

DspBlock::~DspBlock() { stopWorkers(); }

namespace {
constexpr std::uint64_t kOpenCores = 0xFF;  // in claims_
// A worker spins this long for the next sample before it sleeps. While the
// emulator runs, samples come every few microseconds, so a worker that keeps
// up spins; at real-time pace the emulator also pauses for hundreds of
// microseconds at a time, which the workers sleep through. A time, not a
// count of pauses: a pause takes about 140 cycles on recent Intel cores and
// far fewer on AMD's, and a count fit for one spun most of the pauses away on
// the other. A worker that comes late (its cores already run by others, as
// when there are more threads than free cores) spins less each time, down to
// kMinSpin, and leaves its core to the threads that do the work.
constexpr std::chrono::nanoseconds kMaxSpin = std::chrono::microseconds(30), kMinSpin = std::chrono::microseconds(1);

void pause() {
    hydra::cpuRelax();
}

// Busy-waits briefly, then yields, so idle workers do not occupy a CPU for long.
template <typename Done>
void waitUntil(Done done) {
    for (int spins = 0; !done(); ++spins) {
        if (spins < 4096)
            pause();
        else
            std::this_thread::yield();
    }
}
}  // namespace

void DspBlock::setThreads(int threads) {
    threads = std::clamp(threads, 1, kCores);
    if (threads == threads_)
        return;
    stopWorkers();
    threads_ = threads;
    quit_ = false;
    // The sample a worker starts from is taken here, not by the worker: a
    // worker that starts late would otherwise take a sample already released
    // for it as seen.
    const std::uint64_t seen = claims_.load(std::memory_order_acquire) >> 8;
    for (int t = 1; t < threads_; ++t)
        workers_.emplace_back([this, t, seen] {
            initEmulatorThread("dsp-worker");
            workerLoop(t, seen);
        });
}

void DspBlock::stopWorkers() {
    quit_ = true;
    claims_.store(((claims_.load(std::memory_order_relaxed) >> 8) + 1) << 8, std::memory_order_release);
    claims_.notify_all();
    for (std::thread& worker : workers_)
        worker.join();
    workers_.clear();
    threads_ = 1;
}

std::uint64_t DspBlock::runCores(int thread, std::uint64_t generation) {
    std::uint64_t own = 0;
    for (int i = thread; i < kCores; i += threads_)
        own |= 1u << i;
    std::uint64_t executed = 0;
    std::uint64_t claims = claims_.load(std::memory_order_acquire);
    while (claims >> 8 == generation && (claims & kOpenCores)) {
        // Own cores from the bottom; others' from the top, away from their owner.
        const std::uint64_t open = claims & kOpenCores;
        const int core = open & own ? std::countr_zero(open & own) : 63 - std::countl_zero(open);
        if (!claims_.compare_exchange_weak(claims, claims & ~(1ull << core), std::memory_order_acq_rel,
                                           std::memory_order_acquire))
            continue;
        const std::uint64_t n = cores_[core].dsp->run(kInstructionsPerSample);
        if (thread == 0)
            executed += n;
        else
            workerExecuted_.fetch_add(n, std::memory_order_relaxed);
        finished_.fetch_add(1, std::memory_order_release);
        claims = claims_.load(std::memory_order_acquire);
    }
    return executed;
}

void DspBlock::workerLoop(int thread, std::uint64_t seen) {
    using Clock = std::chrono::steady_clock;
    std::chrono::nanoseconds spinLimit = kMaxSpin;
    for (;;) {
        // Spin while samples come quickly; sleep when the DSP side pauses.
        std::uint64_t claims = claims_.load(std::memory_order_acquire);
        if (claims >> 8 == seen) {
            const Clock::time_point until = Clock::now() + spinLimit;
            for (int spins = 1; claims >> 8 == seen; ++spins) {
                pause();
                if (spins % 16 == 0 && Clock::now() > until) {
                    claims_.wait(claims, std::memory_order_acquire);
                    spins = 0;
                }
                claims = claims_.load(std::memory_order_acquire);
            }
        }
        seen = claims >> 8;
        if (quit_)
            return;
        if (claims & kOpenCores) {
            spinLimit = std::min(spinLimit * 2, kMaxSpin);
            runCores(thread, seen);
        } else {
            spinLimit = std::max(spinLimit / 2, kMinSpin);
        }
    }
}

std::uint32_t* DspBlock::mailboxWord(std::uint32_t word) {
    const std::uint32_t core = (~word >> 5) & 0x1F;
    if (core < kCores)
        return &cores_[core].mailbox[word & 0x1F];
    return &spareMailboxes_[word - 0x1FC00];
}

std::uint32_t* DspBlock::cpuWord(std::uint32_t offset) {
    const std::uint32_t word = offset >> 2;
    if (offset < 0x20000)
        return &cores_[offset >> 14].program[word & 0xFFF];
    if (offset < 0x7F000)
        return &shared_[word - 0x8000];
    if (offset < 0x80000)
        return mailboxWord(word);
    return nullptr;
}

std::uint32_t DspBlock::read(std::uint32_t offset, int) {
    const std::uint32_t* w = cpuWord(offset & ~3u);
    return w ? *w : 0;
}

void DspBlock::write(std::uint32_t offset, std::uint32_t value, int) {
    if (std::uint32_t* w = cpuWord(offset & ~3u))
        *w = value;
    if (offset < 0x20000)
        cores_[offset >> 14].dsp->invalidate(offset >> 2);
    ++cpuWrites_[offset & ~0xFFu];
    if (offset >= 0x7FC00 && offset < 0x80000 && (offset & 0x7F) == 0 && value != 0 && value != 0x55AA)
        ++commands_[{static_cast<int>((~offset >> 7) & 0x1F), value}];
}

std::uint32_t* DspBlock::ioWord(int core, std::uint32_t a) {
    a &= 0x1FFFF;
    if (a & 0x10000) {
        if (a - 0x8000 < dsp::DspCore::kSharedWords)
            return &shared_[a - 0x8000];
        return mailboxWord(a);
    }
    if (a <= 0xFFF)
        return &cores_[core].program[a];
    if (a >= 0xFF00)
        return &cores_[(~a >> 5) & 7].mailbox[a & 0x1F];
    if (a <= 0x7FFF)
        return &cores_[a >> 12].program[a & 0xFFF];
    return &shared_[a - 0x8000];
}

std::uint32_t* DspBlock::extWord(int core, std::uint32_t a) {
    a &= 0xFFFF;
    if (a <= 0xFFF)
        return &cores_[core].program[a];
    if (a >= 0xFC00)
        return mailboxWord(0x10000 + a);
    return &shared_[a + 0x8000];
}

std::uint32_t DspBlock::ioRead(int core, std::uint32_t a) { return *ioWord(core, a); }
void DspBlock::ioWrite(int core, std::uint32_t a, std::uint32_t v) {
    *ioWord(core, a) = v;
    a &= 0x1FFFF;
    if (a <= 0xFFF)
        cores_[core].dsp->invalidate(a);
    else if (a <= 0x7FFF)
        cores_[a >> 12].dsp->invalidate(a);
}
std::uint32_t DspBlock::extRead(int core, std::uint32_t a) { return *extWord(core, a); }
void DspBlock::extWrite(int core, std::uint32_t a, std::uint32_t v) {
    *extWord(core, a) = v;
    if ((a & 0xFFFF) <= 0xFFF)
        cores_[core].dsp->invalidate(a);
}

void DspBlock::setRunMask(std::uint32_t mask) {
    mask &= 0xFF;
    for (int i = 0; i < kCores; ++i)
        if ((mask >> i & 1) && !(runMask_ >> i & 1))
            cores_[i].dsp->reset();
    runMask_ = mask;
}

void DspBlock::advanceToSample(std::uint64_t target) {
    while (sample_ < target) {
        if (onSampleStart)
            onSampleStart(ports_.data());
        std::uint64_t open = 0;
        for (int i = 0; i < kCores; ++i)
            if ((runMask_ >> i & 1) && !cores_[i].dsp->trapped()) {
                cores_[i].dsp->raiseInterrupt(2);
                open |= 1u << i;
            }
        if (threads_ == 1) {
            for (int i = 0; i < kCores; ++i)
                if (open >> i & 1)
                    executed_ += cores_[i].dsp->run(kInstructionsPerSample);
        } else {
            finished_.store(0, std::memory_order_relaxed);
            const std::uint64_t generation = (claims_.load(std::memory_order_relaxed) >> 8) + 1;
            claims_.store(generation << 8 | open, std::memory_order_release);
            claims_.notify_all();  // a syscall only if a worker sleeps
            executed_ += runCores(0, generation);
            const auto waitStart = profileSync ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const int cores = std::popcount(open);
            waitUntil([&] { return finished_.load(std::memory_order_acquire) == cores; });
            if (profileSync)
                syncNs += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count());
            executed_ += workerExecuted_.exchange(0, std::memory_order_relaxed);
        }
        ++sample_;
        if ((sample_ & 0x1F) == 0 && onBlock)
            onBlock(sample_);
    }
}

std::uint32_t DspControl::read(std::uint32_t offset, int) {
    if (offset == 0)
        return block_.runMask();
    return offset < 0x10 ? regs_[offset >> 2] : 0;
}

void DspControl::write(std::uint32_t offset, std::uint32_t value, int) {
    if (offset == 0) {
        block_.setRunMask(value);
        for (int core = 0; core < DspBlock::kCores; ++core)
            if (value >> (8 + core) & 1)
                block_.requestAttention(core);
    } else if (offset < 0x10) {
        regs_[offset >> 2] = value;
    }
}

}  // namespace hydra
