#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "emulator.h"

class HydraProcessor;

// The instrument's patches: one bank at a time (A-H) as a list of its 128
// slots, read from the emulated instrument, with the current patch marked.
// Choosing a slot selects it on the instrument (program change); the browser
// follows patch changes made on the panel. "Load bank..." sends a .hydra bank
// (or a .patch) into the shown bank.
class PatchBrowser : public juce::Component, private juce::ListBoxModel, private juce::Timer {
public:
    explicit PatchBrowser(HydraProcessor& processor);
    ~PatchBrowser() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    using Patches = hydra::Emulator::Patches;

    int getNumRows() override { return Patches::kSlots; }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void timerCallback() override;
    void showBank(int bank);
    void markCurrent();
    void chooseBank();
    static juce::String slotName(int bank, int slot);

    HydraProcessor& processor_;
    Patches patches_;
    std::uint64_t generation_ = 0;
    bool live_ = false, updating_ = false;
    int bank_ = 0, lastCurrent_ = -1;

    juce::Label current_;
    std::array<juce::TextButton, Patches::kBanks> bankButtons_;
    juce::ListBox list_;
    juce::TextButton loadBank_{"Load bank..."};
    std::unique_ptr<juce::FileChooser> chooser_;
};
