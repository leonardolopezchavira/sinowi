cmake_minimum_required(VERSION 3.25)
project(sinowi VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# The plugin is a shared library, so everything linked into it must be PIC.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Dynarmic (vendored, patched: see patches/). Only the A32 frontend is needed.
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
# Boost (headers only) is needed by Dynarmic and by its IR headers that hydra_core uses.
# scripts/fetch-deps.sh puts the headers into external/boost when the system has none.
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/external/boost/boost/version.hpp AND NOT Boost_INCLUDE_DIR)
    set(Boost_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/boost)
endif()
find_package(Boost 1.57 REQUIRED)
add_subdirectory(external/dynarmic EXCLUDE_FROM_ALL)

add_library(hydra_core STATIC
    src/a5_peripherals.cpp
    src/a5pack.cpp
    src/bus.cpp
    src/arm_system.cpp
    src/cp15.cpp
    src/cortex_m3.cpp
    src/cpu.cpp
    src/dsp_block.cpp
    src/dsp_core.cpp
    src/dsp_jit.cpp
    src/dsp_jit_arm64.cpp
    src/effects_rack.cpp
    src/emulator.cpp
    src/fx_dsp.cpp
    src/i2c.cpp
    src/machine.cpp
    src/nor_flash.cpp
    src/front_panel.cpp
    src/oled.cpp
    src/panel_mcu.cpp
    src/patch_bank.cpp
    src/spi.cpp
    src/stm32.cpp
    src/sysex.cpp
    src/threads.cpp
    src/tone_generator.cpp
    src/wave_rom.cpp
)
target_include_directories(hydra_core PUBLIC src)
target_link_libraries(hydra_core PUBLIC dynarmic merry::mcl)
# Dynarmic's IR headers (used by cpu.cpp, cortex_m3.cpp) need Boost headers.
target_link_libraries(hydra_core PRIVATE Boost::boost)
if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    target_link_libraries(hydra_core PRIVATE xbyak::xbyak)
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    # The DSP JIT for arm64 (dsp_jit_arm64.cpp) uses oaknut, which Dynarmic vendors.
    target_link_libraries(hydra_core PRIVATE merry::oaknut)
endif()
# Same Xbyak configuration as Dynarmic: its inline functions are shared (ODR).
target_compile_definitions(hydra_core PRIVATE XBYAK_STRICT_CHECK_MEM_REG_SIZE=0)
target_compile_options(hydra_core PRIVATE -Wall -Wextra)

add_executable(hydraboot tools/hydraboot.cpp)
target_link_libraries(hydraboot PRIVATE hydra_core)
target_compile_options(hydraboot PRIVATE -Wall -Wextra)

add_executable(armdis tools/armdis.cpp)
target_link_libraries(armdis PRIVATE hydra_core)

add_executable(dsptest tools/dsptest.cpp)
target_link_libraries(dsptest PRIVATE hydra_core)
target_compile_options(dsptest PRIVATE -Wall -Wextra)

add_executable(waverom tools/waverom.cpp)
target_link_libraries(waverom PRIVATE hydra_core)

add_executable(panelboot tools/panelboot.cpp)
target_link_libraries(panelboot PRIVATE hydra_core)
target_compile_options(panelboot PRIVATE -Wall -Wextra)

add_executable(emurun tools/emurun.cpp)
target_link_libraries(emurun PRIVATE hydra_core)
target_compile_options(emurun PRIVATE -Wall -Wextra)

# The plugin (needs JUCE in external/JUCE: scripts/fetch-deps.sh).
option(HYDRA_BUILD_PLUGIN "Build the VST3/LV2/standalone plugin" ON)
if (HYDRA_BUILD_PLUGIN AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE/CMakeLists.txt)
    add_subdirectory(external/JUCE EXCLUDE_FROM_ALL)
    if (APPLE)
        set(HYDRA_PLUGIN_FORMATS AU VST3 Standalone)
    else()
        set(HYDRA_PLUGIN_FORMATS VST3 LV2 Standalone)
    endif()
    # Sinowi was called Sydrahynth: the plugin and manufacturer codes (and the LV2
    # URI) stay the same, so hosts open projects saved with Sydrahynth with Sinowi.
    juce_add_plugin(Sinowi
        PRODUCT_NAME "Sinowi"
        COMPANY_NAME "sinowi"
        PLUGIN_MANUFACTURER_CODE Sydr
        PLUGIN_CODE Sdhy
        FORMATS ${HYDRA_PLUGIN_FORMATS}
        BUNDLE_ID org.sinowi.Sinowi
        ICON_BIG ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.png
        AU_MAIN_TYPE kAudioUnitType_MusicDevice
        IS_SYNTH TRUE
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        VST3_CATEGORIES Instrument Synth
        LV2URI "urn:sydrahynth:plugin"
        COPY_PLUGIN_AFTER_BUILD FALSE)
    target_sources(Sinowi PRIVATE
        plugin/HydraProcessor.cpp
        plugin/HydraEditor.cpp
        plugin/PanelView.cpp
        plugin/PatchBrowser.cpp)
    target_include_directories(Sinowi PRIVATE plugin)
    target_compile_definitions(Sinowi PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)
    target_link_libraries(Sinowi
        PRIVATE hydra_core juce::juce_audio_utils
        PUBLIC juce::juce_recommended_config_flags)
    target_compile_options(Sinowi PRIVATE -Wall -Wextra)

    # Headless host that loads the built VST3 (tools/plugintest.cpp).
    juce_add_console_app(plugintest PRODUCT_NAME "plugintest")
    target_sources(plugintest PRIVATE tools/plugintest.cpp)
    target_compile_definitions(plugintest PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
    target_link_libraries(plugintest PRIVATE hydra_core juce::juce_audio_processors juce::juce_audio_formats
        juce::juce_recommended_config_flags)
endif()


add_executable(dsp_start_race tools/dsp_start_race.cpp)
target_link_libraries(dsp_start_race PRIVATE hydra_core)
