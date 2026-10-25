# Sinowi: changes from Sydrahynth

Sinowi is Sydrahynth (systemfreund/sydrahynth at commit 12cd2bc,
https://github.com/systemfreund/sydrahynth), renamed and modified:

- Renamed Sydrahynth -> Sinowi (product, files, settings folder, panel). The
  plugin/manufacturer codes and the LV2 URI are unchanged, so hosts open projects
  saved with Sydrahynth with Sinowi, and Sinowi reads Sydrahynth's saved state.

- Builds natively on arm64 (Apple Silicon Macs and arm64 Linux):
  - `src/host_arch.h`: portable spin-wait and cycle-counter helpers in place of
    x86-only intrinsics (`_mm_pause`, `__rdtsc`).
  - `src/dsp_jit_arm64.cpp`: an arm64 port of the x86-64 DSP JIT, bit-exact with
    the interpreter (checked by rendering the same notes both ways).
  - `patches/dynarmic-arm64-host-call-arguments.patch`: Dynarmic's arm64 backend
    crashed on host calls with unused argument slots.
  - `patches/dynarmic-arm64-halt-on-memory-access.patch`: Dynarmic's arm64 backend
    applied register get/set elimination even with `check_halt_on_memory_access`,
    leaving guest registers stale when a block stopped at a memory-access halt.
    This broke the panel MCU (encoders, LEDs). With the fix the panel MCU runs
    identically to the x86-64 build.
  - `patches/fmt-cstdlib.patch`: a missing include with newer libc++.
  - CMake: Boost headers found or fetched by `scripts/fetch-deps.sh`, oaknut for
    the arm64 JIT, AU instead of LV2 on macOS, app icon (`resources/`).
- macOS: settings in `~/Library/Application Support/sinowi/`, where the
  firmware is also looked for; `scripts/build-macos.sh` builds and installs.
- Plugin:
  - A fresh instrument loads the first `.hydra` bank next to the firmware into
    bank A, and starts with Param RX = CC (so MIDI CCs change the sound).
  - Arpeggiator CCs (106-110, 120, 122) are scaled from 0-127 onto the values the
    firmware accepts (it ignores out-of-range values).
  - The filter knobs and arp switches on the panel follow incoming MIDI CCs.
  - New panel look (brushed faceplate, metal knobs, screen colours: right-click a
    screen); static parts are drawn once into a cached image.

The full set of changes against 12cd2bc is in this repository's history
(`git diff 12cd2bc`).
