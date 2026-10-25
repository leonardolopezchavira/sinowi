#pragma once

#include <atomic>
#include <chrono>

#include "host_arch.h"

namespace hydra {

// Sets up the calling thread as one of the emulator's: names it (shown by
// top -H and ps -L) and keeps it off cores with less than half the capacity
// of the fastest core it may use, such as the low-power E-cores of Intel's
// Core Ultra chips. The DSP side syncs every sample, so one thread on such a
// core holds up all the others: on a Core Ultra 5 235H (P-cores 1024, E-cores
// 793, LP E-cores 320) the emulator ran below real time at times and the
// output had underruns, without the two LP E-cores none. Cores of equal
// capacity, the usual case, are all kept. HYDRA_ALL_CORES=1 keeps every core.
void initEmulatorThread(const char* name);

// Waits until `ready(value)` holds for the value of `atomic` and returns that
// value: spins for up to `spin` (the other thread is usually that quick, and
// waking a sleeping thread takes far longer, especially on power-saving cores),
// then sleeps until the value changes (the writer must notify).
template <typename T, typename Ready>
T spinThenWait(const std::atomic<T>& atomic, Ready ready, std::chrono::nanoseconds spin) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point until = Clock::now() + spin;
    for (int spins = 1;; ++spins) {
        const T value = atomic.load(std::memory_order_acquire);
        if (ready(value))
            return value;
        if (spins % 64 == 0 && Clock::now() > until)
            atomic.wait(value, std::memory_order_acquire);
        else
            cpuRelax();
    }
}

}  // namespace hydra
