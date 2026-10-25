#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "front_panel.h"

class HydraProcessor;

// The Hydrasynth Desktop's front panel, drawn from the emulated hardware: the
// five displays, the buttons with their LEDs, the eight encoders with their LED
// rings, the data encoder, the filter knobs, the arpeggiator switches and the
// pads in their colours. Mouse and wheel operate the controls on the emulated
// panel. Everything is laid out on a fixed canvas and scaled to the component.
class PanelView : public juce::Component, private juce::Timer {
public:
    static constexpr float kCanvasWidth = 1960, kCanvasHeight = 1000;

    explicit PanelView(HydraProcessor& processor);
    ~PanelView() override;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

private:
    // What the mouse is on: a button, knob, encoder or pad (index into its table).
    struct Target {
        enum class Kind { None, Button, Knob, Encoder, DataEncoder, Pad } kind = Kind::None;
        int index = 0;
    };

    void timerCallback() override;
    juce::Point<float> toCanvas(juce::Point<float> p) const;
    Target targetAt(juce::Point<float> canvas) const;
    void turn(const Target& target, int detents);
    // SHIFT: a click latches it until the next button (or another click on
    // SHIFT); the computer's Shift key, while the mouse is over the panel,
    // holds it (polled, as plugin windows often get no key events).
    void pressShift();
    void releaseShift(int delayMs);
    void setKnob(int knob, float value);

    // Drawing: what never changes while playing (panel, printing, knob and
    // button bodies, bezels) is drawn once into background_ (paintStatic) at the
    // screen's pixel resolution; paint() copies it and draws only the live parts
    // (screens, LEDs, knob pointers, rings, pads), each only where it is dirty.
    void resized() override;
    void renderBackground(float pixelScale);
    void paintStatic(juce::Graphics& g);
    void paintHeadings(juce::Graphics& g);
    void paintModuleLines(juce::Graphics& g);
    void paintDisplays(juce::Graphics& g);
    void paintButtonStates(juce::Graphics& g);
    void paintKnobPointers(juce::Graphics& g);
    void paintEncoderLights(juce::Graphics& g);
    void paintPads(juce::Graphics& g);
    // Screen colours: a palette from the OLED levels; right-click a screen to switch.
    void buildPalette();
    void convertDisplay(int d, const hydra::Oled::Image& pixels);
    void cycleScreenTint();

    HydraProcessor& processor_;
    std::unique_ptr<hydra::FrontPanel::Lights> lights_, shown_;  // the latest capture, and what is drawn
    std::uint64_t generation_ = 0;
    bool live_ = false;
    std::array<juce::Image, hydra::PanelDisplays::kCount> displays_;
    juce::Image background_, brushed_;
    juce::Point<int> backgroundSize_;
    float backgroundScale_ = 0;
    juce::uint32 resizedAt_ = 0;
    int screenTint_ = 0;
    std::array<juce::Colour, 256> palette_;

    // Mouse gesture state.
    Target active_;
    float dragStartY_ = 0, dragValue_ = 0, dragAccumulator_ = 0;
    float wheelAccumulator_ = 0;
    bool shiftLatched_ = false, shiftForGesture_ = false;
    bool shiftKeyHeld_ = false;  // the computer's Shift key, held while the mouse is over the panel
    // Keys of the computer keyboard that hold panel buttons (kKeyButtons), and
    // which buttons a key or MIDI CC holds (drawn pressed).
    void followKeys();
    std::array<bool, 4> keysHeld_{};
    std::array<bool, hydra::FrontPanel::kButtons> heldElsewhere_{};
    void followShiftKey();
    std::array<float, hydra::FrontPanel::kKnobs> knobValues_{0.5f, 0.5f, 0.5f};  // cutoff, resonance, drive/morph
    // The filter knobs follow incoming MIDI CCs for the filter they control.
    std::array<int, 128> controllers_{}, controllersShown_{};
    std::uint32_t controllerSerial_ = 0;
    void followControllers();
    std::array<int, hydra::FrontPanel::kArpSwitches> arpPositions_{};
};
