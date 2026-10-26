#pragma once

#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>

#include "PanelView.h"
#include "PatchBrowser.h"

class HydraProcessor;

// Keeps the front panel's proportions when the window is resized: the panel's
// area (the window less fixed extras, the status bar and the patch browser)
// keeps a fixed aspect ratio, whichever edge or corner is dragged.
class PanelConstrainer : public juce::ComponentBoundsConstrainer {
public:
    void setLayout(int extraWidth, int extraHeight, double aspect, int minPanelWidth, int maxPanelWidth);
    // The window size for a panel width.
    juce::Point<int> sizeFor(int panelWidth) const;
    void checkBounds(juce::Rectangle<int>& bounds, const juce::Rectangle<int>& previous, const juce::Rectangle<int>& limits,
                     bool isStretchingTop, bool isStretchingLeft, bool isStretchingBottom, bool isStretchingRight) override;

private:
    int extraWidth_ = 0, extraHeight_ = 0, minPanelWidth_ = 700, maxPanelWidth_ = 2400;
    double aspect_ = 1.96;
};

// The editor: a status bar (emulator status, firmware file) above the patch
// browser and the emulated front panel.
class HydraEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit HydraEditor(HydraProcessor& owner);
    ~HydraEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    static constexpr int kBarHeight = 36, kBrowserWidth = 270;

    void timerCallback() override;
    void chooseFirmware();
    void saveState();
    void loadState();
    // Shows or hides the patch browser; the window grows or shrinks by its width.
    void showBrowser(bool visible, bool resizeWindow);
    void updateResizeLimits();

    HydraProcessor& processor_;
    juce::Label title_, firmware_, status_;
    juce::TextButton chooseFirmware_{"Firmware..."}, togglePatches_{"Patches"},
        saveState_{"Save state..."}, loadState_{"Load state..."};
    juce::ComboBox latency_;
    std::unique_ptr<juce::FileChooser> chooser_;
    PatchBrowser browser_;
    PanelView panel_;
    PanelConstrainer constrainer_;
};
