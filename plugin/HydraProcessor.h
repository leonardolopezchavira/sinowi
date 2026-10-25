#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "cc_map.h"
#include "emulator.h"

// The plugin: an emulated Hydrasynth Desktop (hydra::Emulator) as an instrument.
// MIDI goes to the instrument's MIDI input, the host parameters are its MIDI
// CCs (sent when the host or the editor changes them), and the 44.1 kHz output
// is resampled to the host rate. The plugin state holds the parameters and the
// instrument's flash (settings, banks); the firmware file is a global setting.
class HydraProcessor : public juce::AudioProcessor,
                       private juce::AudioProcessorParameter::Listener,
                       private juce::Timer {
public:
    HydraProcessor();
    ~HydraProcessor() override;

    // juce::AudioProcessor
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // For the editor.
    juce::File firmwareFile() const;
    void setFirmwareFile(const juce::File& file);
    juce::String statusText() const;
    // Latency: how far the emulator runs ahead of the host (a global setting).
    static constexpr std::array<int, 4> kLatencyFrames = {256, 512, 1024, 2048};  // at 44.1 kHz
    int latencyChoice() const;
    void setLatencyChoice(int choice);
    // Whether the editor shows the patch browser (a global setting).
    bool browserVisible() const;
    void setBrowserVisible(bool visible);
    // The panel screens' colour preset (a global setting).
    int screenTint() const;
    void setScreenTint(int tint);
    juce::AudioParameterFloat& outputGain() { return *outputGain_; }
    // Sends the patches of a .hydra bank (or a single .patch) to a bank (0 = A
    // ... 4 = E) of the instrument over its MIDI input. Returns an error message,
    // or an empty string once the upload is under way (see statusText()).
    juce::String loadBank(const juce::File& file, int bank);
    // The instrument's patches and the current one (see Emulator::readPatches),
    // and selecting one by program change.
    bool readPatches(hydra::Emulator::Patches& out, std::uint64_t& generation) const;
    void selectPatch(int bank, int slot);
    // The emulated front panel (no-ops while no emulator runs).
    void panelInput(const hydra::Emulator::PanelInput& input);
    // Panel buttons held from more than one place: the editor's mouse (and the
    // computer's Shift key for SHIFT), keys of the computer keyboard (R for
    // RANDOM, see PanelView) and MIDI CCs (see kButtonCcs). The instrument sees a
    // button down while any source holds it, so the sources do not fight.
    enum ButtonSource : std::uint8_t { kFromPanel = 1, kFromMidi = 2, kFromKeyboard = 4 };
    void holdButton(int id, ButtonSource source, bool down, int delayMs = 0);
    // Whether any of the `sources` (ButtonSource bits) holds the button.
    bool buttonHeldBy(int id, std::uint8_t sources) const;
    bool readPanel(hydra::FrontPanel::Lights& out, std::uint64_t& generation) const;
    // The last value of each MIDI CC sent to the instrument (from the MIDI input
    // or the host parameters, after scaleController; -1 = none yet). Returns false if nothing changed
    // since `serial`, which it updates.
    bool readControllers(std::array<int, 128>& out, std::uint32_t& serial) const;

private:
    void parameterValueChanged(int index, float newValue) override;
    void parameterGestureChanged(int, bool) override {}

    // A fresh instrument (no stored flash) has empty banks and stays silent: it
    // gets the first .hydra bank from the firmware folder into bank A, then A001.
    void timerCallback() override;
    juce::File defaultBankFile() const;
    enum class AutoBank { Off, WaitForBoot, Uploading, Settle };
    AutoBank autoBank_ = AutoBank::Off;
    juce::uint32 settleUntil_ = 0;
    // The instrument's flash right after that first upload, kept next to the
    // firmware (default-bank.flash), so later fresh instruments start with the
    // bank already in place instead of uploading it again (over a minute). It is
    // tied to the bank and firmware files (path, size, date) and remade if they change.
    juce::File defaultFlashFile() const;
    juce::String defaultFlashKey() const;
    bool loadDefaultFlash(std::vector<hydra::Machine::Config::FlashBlock>& out) const;
    void saveDefaultFlash(const std::vector<hydra::Machine::Config::FlashBlock>& flash) const;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);  // for callAsync

    // Starts (or restarts) the emulator with the current firmware and `flash`.
    void restartEmulator(std::vector<hydra::Machine::Config::FlashBlock> flash);
    void pullAudio(juce::AudioBuffer<float>& buffer, hydra::Emulator& emulator);
    // Emulator frames to keep buffered: the chosen latency, but at least a host
    // block plus some margin.
    std::size_t targetFill() const;
    void applyLatency();

    juce::ApplicationProperties settings_;

    std::array<juce::AudioParameterInt*, ccmap::kEntries.size()> ccParameters_{};
    std::array<std::atomic<bool>, ccmap::kEntries.size()> ccDirty_{};
    juce::AudioParameterFloat* outputGain_ = nullptr;
    // For readControllers(); written on the audio thread.
    std::array<std::atomic<int>, 128> lastCc_{};
    std::atomic<std::uint32_t> ccSerial_{0};
    void noteController(int number, int value);
    // Which sources hold each panel button (ButtonSource bits).
    std::array<std::atomic<std::uint8_t>, hydra::FrontPanel::kButtons> buttonSources_{};
    // Updates the sources of a button; returns +1 if it went down, -1 if it went
    // up for the instrument (no source left), 0 if that did not change.
    int updateButtonSources(int id, ButtonSource source, bool down);
    // A MIDI CC mapped to a panel button: handled here, not passed to the
    // instrument. Returns false if `number` is not such a CC.
    bool buttonController(hydra::Emulator& emulator, int number, int value);
    static int scaleController(int number, int value);
    std::atomic<bool> restoringState_{false};

    // The audio thread only try-locks emulatorLock_, which is held just for swapping
    // the emulator. Other threads (restart, state, status) serialise on lifeMutex_,
    // so they can wait for the emulator without stalling the audio thread.
    juce::SpinLock emulatorLock_;
    mutable std::recursive_mutex lifeMutex_;
    std::unique_ptr<hydra::Emulator> emulator_;
    std::vector<hydra::Machine::Config::FlashBlock> pendingFlash_;
    bool started_ = false;

    // Resampling from 44.1 kHz to the host rate.
    double ratio_ = 1.0;  // emulator samples per host sample
    int blockSize_ = 512;  // host samples
    std::array<juce::LagrangeInterpolator, 2> resamplers_;
    std::array<std::vector<float>, 2> fifo_;
    int fifoCount_ = 0;
    std::vector<float> scratchLeft_, scratchRight_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HydraProcessor)
};
