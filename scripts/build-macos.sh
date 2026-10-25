#!/bin/bash
# Builds and installs Sinowi on an Apple Silicon Mac.
#   scripts/build-macos.sh [path/to/Hydrasynth_Desktop_Firmware_2.2.0.dat]
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
SUPPORT="$HOME/Library/Application Support"
DATA="$SUPPORT/sinowi"          # firmware, banks, settings, the kept default bank
OLD_DATA="$SUPPORT/sydrahynth"  # the same under Sinowi's former name, Sydrahynth

# The firmware: the argument, or the ASM firmware pack in Downloads.
FIRMWARE=""
for candidate in "${1:-}" \
    "$HOME/Downloads/Hydrasynth/Hydrasynth_Desktop_Firmware_Pack_2.2.0/Hydrasynth_Desktop_Firmware_2.2.0.dat" \
    "$HOME/Downloads/Hydrasynth_Desktop_Firmware_Pack_2.2.0/Hydrasynth_Desktop_Firmware_2.2.0.dat"; do
    if [ -n "$candidate" ] && [ -f "$candidate" ]; then FIRMWARE="$candidate"; break; fi
done

if ! xcode-select -p >/dev/null 2>&1; then
    echo "The Xcode command line tools are missing: run 'xcode-select --install', then run this again."
    exit 1
fi

# CMake and Ninja: use installed ones, else a private copy from PyPI.
export PATH="$ROOT/.tools/bin:$PATH"
if ! command -v cmake >/dev/null || ! command -v ninja >/dev/null; then
    python3 -m venv "$ROOT/.tools"
    "$ROOT/.tools/bin/pip" install -q cmake ninja
fi

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -Wno-dev
cmake --build build --target Sinowi_Standalone Sinowi_AU Sinowi_VST3
# Command-line test tools (optional).
cmake --build build --target hydraboot emurun || echo "(test tools failed to build; the app and plugins are fine)"

OUT=build/Sinowi_artefacts/Release
APPS="$HOME/Applications" COMPONENTS="$HOME/Library/Audio/Plug-Ins/Components" VST3="$HOME/Library/Audio/Plug-Ins/VST3"
mkdir -p "$APPS" "$COMPONENTS" "$VST3"
ditto "$OUT/Standalone/Sinowi.app" "$APPS/Sinowi.app"
ditto "$OUT/AU/Sinowi.component" "$COMPONENTS/Sinowi.component"
ditto "$OUT/VST3/Sinowi.vst3" "$VST3/Sinowi.vst3"

# Sinowi was called Sydrahynth. Once: carry its data over (ditto keeps the file
# dates, so the kept default bank stays valid) and the standalone app's settings
# and patches; then remove the old app and plug-ins (same plugin IDs as Sinowi).
if [ -d "$OLD_DATA" ] && [ ! -d "$DATA" ]; then
    ditto "$OLD_DATA" "$DATA"
    if [ -f "$DATA/sydrahynth.settings" ]; then
        sed 's#Application Support/sydrahynth/#Application Support/sinowi/#g' "$DATA/sydrahynth.settings" > "$DATA/sinowi.settings"
        rm -f "$DATA/sydrahynth.settings"
    fi
    echo "Carried Sydrahynth's firmware, banks and settings over to $DATA"
fi
if [ -f "$SUPPORT/Sydrahynth.settings" ] && [ ! -f "$SUPPORT/Sinowi.settings" ]; then
    cp -p "$SUPPORT/Sydrahynth.settings" "$SUPPORT/Sinowi.settings"  # the standalone app's audio setup and patches
fi
for old in "$APPS/Sydrahynth.app" "$COMPONENTS/Sydrahynth.component" "$VST3/Sydrahynth.vst3"; do
    if [ -e "$old" ]; then
        rm -rf "$old"
        echo "Removed the old $(basename "$old")"
    fi
done

mkdir -p "$DATA"
if [ -n "$FIRMWARE" ]; then
    [ -f "$DATA/$(basename "$FIRMWARE")" ] || cp -p "$FIRMWARE" "$DATA/"
    # Banks next to the firmware: a fresh instrument loads the first one into bank A.
    for bank in "$(dirname "$FIRMWARE")"/*.hydra; do
        [ -f "$bank" ] && { [ -f "$DATA/$(basename "$bank")" ] || cp -p "$bank" "$DATA/"; }
    done
fi
touch "$APPS/Sinowi.app"  # refresh the Finder/Dock icon
killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true  # rescan AUs
echo
echo "Done: ~/Applications/Sinowi.app, AU and VST3 installed."
