# Sinowi

Sinowi is a plugin (VST3, AU, standalone) that emulates ASM's Hydrasynth Desktop,
for Apple Silicon Macs (and Linux). It runs ASM's own Hydrasynth Desktop firmware
on an emulated main board. No firmware is included: you supply
`Hydrasynth_Desktop_Firmware_2.2.0.dat` from ASM's firmware pack.

Sinowi is based on [Sydrahynth](https://github.com/systemfreund/sydrahynth), the
Linux plugin by systemfreund, ported to Apple Silicon with further changes; see
`CHANGES-APPLE-SILICON.md`. It keeps Sydrahynth's plugin IDs, so projects saved
with Sydrahynth open with Sinowi.



https://github.com/user-attachments/assets/9b7ed8db-5732-41ab-bc41-2bdd0075680c



## Build

    scripts/fetch-deps.sh
    cmake -S . -B build -G Ninja
    cmake --build build

This builds the tools and, with JUCE fetched, the plugin as VST3, LV2 and a
standalone app in `build/Sinowi_artefacts/<config>/`. Install by
copying `VST3/Sinowi.vst3` to `~/.vst3/` and `LV2/Sinowi.lv2` to
`~/.lv2/`.

The default configuration is `RelWithDebInfo` (`-O2` with debug symbols, a
standalone of over 200 MB). For everyday use, build `Release` (`-O3`, no
debug symbols: 15 MB, about 7% faster, 6% less CPU at real-time pace) in a
directory of its own:

    cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build-release

The plugin is then in `build-release/Sinowi_artefacts/Release/`. The gain
is modest because most of the time goes to code the JITs generate at run
time, which the compiler flags do not affect.

## macOS (Apple Silicon)

The emulator also builds natively on arm64: the DSP cores then run on
`src/dsp_jit_arm64.cpp`, an arm64 port of the x86-64 DSP JIT (bit-exact with
the interpreter, like the original), and Dynarmic uses its arm64 backend.
Needs the Xcode command line tools, CMake and Ninja.

```
scripts/fetch-deps.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This builds `Sinowi.app` (standalone), `Sinowi.component` (AU) and
`Sinowi.vst3` in `build/Sinowi_artefacts/Release/`. Copy the AU to
`~/Library/Audio/Plug-Ins/Components/` and the VST3 to
`~/Library/Audio/Plug-Ins/VST3/`. The firmware is looked for in
`~/Library/Application Support/sinowi/` unless chosen in the plugin window.

## License

Sinowi is free software, licensed under the GNU Affero General Public
License version 3 (SPDX: `AGPL-3.0-only`); see `LICENSE`. It is a modified version
of Sydrahynth (https://github.com/systemfreund/sydrahynth), under the same licence. It builds on JUCE
(used here under its AGPLv3 option), Dynarmic and Xbyak, each under its own
licence.

The firmware is not part of this project: it is ASM's, and you supply it from
ASM's firmware pack. Hydrasynth is a trademark of Ashun Sound Machines (ASM);
this project is not affiliated with or endorsed by ASM.

## Plugin

- Firmware: choose the `.dat` file in the plugin window (stored in
  `~/.config/sinowi/sinowi.settings`), or put it into
  `~/.local/share/sinowi/`, or set `HYDRA_FIRMWARE`.
- The emulator runs on its own threads, ahead of the host by the latency chosen
  in the editor (6, 12, 23 or 46 ms; default 12 ms, and always at least a host
  block plus about 3 ms); MIDI goes to the instrument's MIDI input, the output
  is resampled from 44.1 kHz to the host rate. The emulator runs at about three
  times real time. What can still make it crackle, once, is firmware code running
  for the first time (its translation by the JIT takes a  few milliseconds), e.g.
  the first note after a patch was sent.
- Host parameters: the instrument's 111 MIDI CCs and an output gain.
- The plugin state holds those parameters and the instrument's flash user
  area (settings and banks), so stored patches survive saving a project.
- Panel view: the emulated displays and lights are captured every 10 ms of
  emulated time and drawn at 60 Hz; only what changed is repainted. SHIFT
  latches on a click and applies to the next button, or is released by another
  click; the computer's Shift key, while the mouse is over the panel, holds SHIFT.
- Patches: the editor's patch browser (shown or hidden with "Patches" in the
  top bar; the window grows or shrinks by its width) lists the five banks (A-E, 128 slots
  each) as the instrument stores them, marks the current patch and follows
  changes made on the panel; clicking a slot selects it (bank select LSB and a
  program change on the MIDI input; reception is switched on for a fresh
  instrument). "Load bank..." sends a `.hydra` bank (or a `.patch`) into the
  shown bank. The patches live in the instrument's flash, which the plugin state
  saves and restores, as does the standalone app after quitting. The firmware
  file holds no factory patches; get the banks from ASM as `.hydra` files. Or
  send a bank or patch as SysEx (e.g. from a librarian).
- The editor shows the instrument's front panel, driven by the emulated
  hardware: the five displays, button LEDs, encoder rings and pad colours.
  Click buttons and pads; drag or scroll encoders, the data encoder and knobs
  (the volume knob is the plugin's output gain).

Code: `src/emulator.*` (`hydra::Emulator`) runs a `hydra::Machine` on its own
threads (audio ring, MIDI queue, panel input and capture, patch upload, flash
capture for the state); `src/front_panel.*` (`hydra::FrontPanel`) turns panel
input into what the boards read and reads the lights back; `plugin/` holds the
JUCE side: `HydraProcessor` (parameters, state, resampling), `HydraEditor`
(status bar), `PanelView` (the front panel, SHIFT handling) and `PatchBrowser`.

`build/emurun firmware.dat out.wav [bank.hydra [seconds]]` drives the same
emulator like a host (real-time pulls, a note, a state restore) and reports
throughput and buffer underruns (`HYDRA_TARGET_FILL` and `HYDRA_BLOCK` set the
buffering and pull size); `build/plugintest_artefacts/*/plugintest
<plugin.vst3> <bank.hydra> <prefix>` loads the built VST3 headless at 48 kHz
and checks that a saved state restores the stored patch (`HYDRA_HOST_BLOCK` sets
its block size).
`HYDRA_SOAK=<seconds>` turns emurun into a panel soak test instead: it runs that
long (unpaced; `HYDRA_SOAK_PACED=1` at real-time pace; `HYDRA_SOAK_HOLD=1` holds
a note), turns an encoder every 5 s, plays notes, and reports panel captures,
display redraws, link frames by type and lost link bytes per 10 s (this found
the panel freeze after 10 min 40 s and the display's frame budget).

## Status

The main ARM firmware boots to its steady-state main loop.

    build/hydraboot Hydrasynth_Desktop_Firmware_2.2.0.dat [cpu ticks] [--note] [--wav file] [--bank file.hydra [--bank-first i] [--bank-slot s]] [--program n] [--midi "hex bytes"] [--setting page.entry=value]

The firmware boots to its main loop with the main DSP block running for real
(8 cores of the custom DSP, verified against the reference), the wave ROM
generated from the firmware, tone generator voices, NOR/SPI flash, ADC, UARTs,
MIDI and the panel MCU (STM32, with its own firmware) linked over SPI. Patches can be uploaded over SysEx (`--bank`), and with a
patch loaded a MIDI note produces a voice at the right pitch at the DSP output
(ports 64/65).

Patches go to any slot of bank A and, with `--setting 5.7=1` (PgmChgRX on), are
selected by program change (`--program`).

Both effects DSPs run, so the stereo output carries the patch's effects
(`--wav file` writes it as a 24-bit WAV).

Speed: with `HYDRA_DSP_ASYNC=1 HYDRA_DSP_THREADS=4` (DSP side and effects on their
own threads, DSP cores on four, as in the plugin) the emulator runs at about
3.2 x real time on a Ryzen 7 5800X (`emurun` measures it, with the buffer target raised for the measurement: at the default 512 frames its 5 ms pulls would cap the reading at ~2.3 x). The limit is the
main thread (main CPU and panel MCU); the DSP thread is about a quarter busy;
`HYDRA_HOST_PROFILE=1` breaks the main thread's time down (JIT, panel, link,
device accesses, JIT exits, interrupts, exception handling), `HYDRA_BLOCK_PROFILE=1`
and `HYDRA_PANEL_BLOCK_PROFILE=1` give exact instruction profiles of the two
firmwares by translated block (slow).
What makes it fast: RAM accessed directly by the JIT (page table), count-down
delay loops in the firmware fast-forwarded (they take over 90% of the main CPU's
time at times), the CPU sleeping while it polls a running ADC conversion
(otherwise about 40% of its time) with conversions taking 20 us so that the
firmware's free-running main loop, which rebuilds the panel's LED and screen
frames every iteration, runs at a hardware-like pace rather than as fast as the
host allows, the panel MCU sleeping while it polls a DMA flag or SysTick and
stopping exactly when it has read a byte from the main CPU (instead of running
in 50-cycle steps), the panel lagging up to 50 us between interactions so that
it runs in longer stretches, the DSP cores compiled to x86-64
(`src/dsp_jit.*`, bit-exact with the interpreter; `HYDRA_DSP_JIT=0` switches it
off) and run in parallel, and the DSP side overlapped with the CPU.
The DSP cores sync every sample; rather than wait for a thread that has no
CPU at the moment (more threads than free cores, as in a busy DAW), the others
claim and run its DSP cores for that sample. So four threads are the fastest or
close to it on any machine (2.4 x real time on four cores, 1.4 x on two, where
a fixed split ran at 0.17 x and 0.10 x). `HYDRA_DSP_THREADS` sets the count.
For the same reason the emulator's threads stay off cores with less than half
the capacity of the fastest (the low-power E-cores of Intel's Core Ultra chips):
one thread there held up every sample, with underruns on a Core Ultra 5 235H.
`HYDRA_ALL_CORES=1` lets them use every core.
`HYDRA_PROFILE=1` prints
the time per component; `HYDRA_SAMPLE=file` samples the main thread (all threads
with `HYDRA_SAMPLE_ALL=1`) for `tools/symbolize_samples.py`;
`HYDRA_GUEST_PROFILE=1` lists the firmware's hottest code; `build/armdis image
start end` disassembles it.

`--panel "b45 d2 e0:3 w500 s/tmp/screen.pgm"` operates the front panel after
boot (press button 45, turn the data encoder 2 detents and encoder 1 three, wait,
write the displays as an image); `HYDRA_SCREEN=file.pgm` writes the displays at
the end of a run.

Other tools: `panelboot` boots the panel MCU alone, `waverom` writes the generated wave ROM, `dsptest` and
`tools/oracle/` compare components with the reference plugin, `tools/dspdis.py`
disassembles DSP programs.
