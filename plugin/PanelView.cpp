#include "PanelView.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>

#include "HydraProcessor.h"
#include "KeyState.h"

namespace {
using Input = hydra::Emulator::PanelInput;
constexpr int kShiftButton = 62;

// Panel buttons held with a key of the computer keyboard while the mouse is over
// the panel (SHIFT has the Shift key, see followShiftKey). The macOS virtual key
// code, and the character for JUCE's key state elsewhere.
struct KeyButton {
    int macKeyCode;
    juce::juce_wchar character;
    int button;
};
constexpr KeyButton kKeyButtons[] = {
    {0x0F, 'r', 59},  // R: RANDOM (kVK_ANSI_R)
};
// Buttons also held from outside the panel (keys, MIDI CCs): drawn pressed then.
constexpr int kMappedButtons[] = {kShiftButton, 59};
// A button pressed together with SHIFT goes down this much after SHIFT, and
// SHIFT is let go this much after the button: the firmware scans the buttons
// every 5 ms and must see SHIFT held when the other button arrives.
constexpr int kShiftLeadMs = 20, kShiftTrailMs = 80;

// Buttons: label, keyscan id, LED index (-1: none), centre and size on the canvas.
struct Button {
    const char* label;
    int id, led;
    float x, y, w = 70, h = 36;
};
const Button kButtons[] = {
    // Arpeggiator
    {"ON", 26, 30, 385, 412}, {"LATCH", 28, 31, 532, 412}, {"TRIPLET", 46, 32, 680, 412}, {"TAP TEMPO", 48, 29, 827, 412},
    // Knob assignment for cutoff and resonance
    {"FILTER 1", 79, 26, 802, 562}, {"FILTER 2", 80, 27, 802, 630},
    // Patch and system
    {"BROWSE", 45, 34, 955, 124, 76}, {"SAVE", 50, 33, 955, 238, 76}, {"SYSTEM", 27, 38, 955, 314, 76},
    {"INIT", 44, 35, 955, 418, 76}, {"RANDOM", 59, 36, 955, 488, 76}, {"SHIFT", 62, 39, 955, 598, 76},
    // Navigation
    {"<", 51, -1, 1065, 488, 62}, {">", 55, -1, 1175, 488, 62}, {"HOME", 49, -1, 1120, 598, 170, 40},
    {"EXIT", 7, 45, 1870, 124, 76}, {"VOICE", 56, 46, 1870, 234, 76}, {"UP", 60, 47, 1870, 316, 76},
    {"DOWN", 61, 48, 1870, 378, 76}, {"MACRO ASSIGN", 57, 49, 1870, 486, 76}, {"MOD MATRIX", 58, 58, 1870, 596, 76},
    // Encoder push buttons
    {"", 33, 50, 1315, 235, 62, 30}, {"", 34, 51, 1455, 235, 62, 30}, {"", 35, 52, 1597, 235, 62, 30},
    {"", 36, 53, 1737, 235, 62, 30}, {"", 37, 54, 1315, 488, 62, 30}, {"", 38, 55, 1455, 488, 62, 30},
    {"", 39, 56, 1597, 488, 62, 30}, {"", 40, 57, 1737, 488, 62, 30},
    // Module buttons
    {"OSC 1", 11, 0, 125, 770}, {"MUTANT 1", 14, 3, 230, 770, 78}, {"MUTANT 2", 15, 4, 332, 770, 78},
    {"OSC 2", 12, 1, 125, 838}, {"MUTANT 3", 16, 5, 230, 838, 78}, {"MUTANT 4", 3, 6, 332, 838, 78},
    {"OSC 3", 13, 2, 125, 908}, {"RING-NOISE", 17, 7, 332, 908, 78},
    {"MIXER", 0, 8, 437, 838}, {"FILTER 1", 24, 19, 540, 805}, {"FILTER 2", 25, 20, 540, 872},
    {"AMP", 1, 21, 644, 838}, {"PRE-FX", 2, 22, 746, 838}, {"DELAY", 4, 23, 851, 805},
    {"REVERB", 5, 24, 851, 872}, {"POST-FX", 6, 25, 953, 838},
    {"ENV 1", 70, 9, 540, 728}, {"ENV 2", 71, 10, 644, 728}, {"ENV 3", 72, 11, 748, 728},
    {"ENV 4", 73, 12, 850, 728}, {"ENV 5", 18, 13, 955, 728},
    {"LFO 1", 66, 14, 540, 948}, {"LFO 2", 67, 15, 644, 948}, {"LFO 3", 68, 16, 748, 948},
    {"LFO 4", 69, 17, 850, 948}, {"LFO 5", 47, 18, 955, 948},
};

// Secondary functions printed in orange under some buttons (used with SHIFT).
struct Legend {
    int id;
    const char* text;
};
const Legend kShiftLegends[] = {{26, "EDIT"}, {28, "SUSTAIN"}, {45, "FAVORITE"}, {49, "PANIC"}, {51, "-10"}, {55, "+10"}};

// The pads' SHIFT functions, printed under them: scales on the lower two rows,
// octave, pad mode and key on the top row (pad number = row * 8 + column + 1).
const char* const kPadLegends[3][8] = {
    {"Chromatic", "Major", "Maj Pent", "Min Pent", "Blues", "Bebop", "Whole Tone", "Dim Whole"},
    {"Dorian", "Phrygian", "Lydian", "Mixolydian", "Minor", "Locrian", "Harmonic Min", "Melodic Min"},
    {"OCT -", "OCT +", "PAD MODE -", "PAD MODE +", "KEY", "", "", ""},
};

// Section headings, as on the hardware.
struct Heading {
    const char* text;
    float x, y, w;  // centre x, top, width
};
const Heading kHeadings[] = {
    {"ARPEGGIATOR", 606, 200, 560}, {"FILTER CONTROLS", 611, 490, 470}, {"MODULE SELECT", 230, 678, 300},
    {"MAIN SYSTEMS", 1065, 30, 300}, {"MASTER CONTROL", 1590, 30, 600},
};

// Knobs: master volume (the plugin's output gain), the arpeggiator switches and
// the analog knobs.
struct Knob {
    enum class Kind { Volume, Arp, Analog } kind;
    const char* label;
    int index;  // switch or knob
    float x, y, radius;
};
const Knob kKnobs[] = {
    {Knob::Kind::Volume, "MASTER VOLUME", 0, 175, 405, 38},
    {Knob::Kind::Arp, "MODE", hydra::FrontPanel::kArpMode, 385, 300, 27},
    {Knob::Kind::Arp, "OCTAVE", hydra::FrontPanel::kArpOctave, 532, 300, 27},
    {Knob::Kind::Arp, "DIVISION", hydra::FrontPanel::kArpDivision, 680, 300, 27},
    {Knob::Kind::Arp, "SWING", hydra::FrontPanel::kArpSwing, 827, 300, 27},
    {Knob::Kind::Analog, "CUTOFF", hydra::FrontPanel::kCutoff, 422, 600, 34},
    {Knob::Kind::Analog, "RESONANCE", hydra::FrontPanel::kResonance, 547, 600, 34},
    {Knob::Kind::Analog, "DRIVE / MORPH", hydra::FrontPanel::kDriveMorph, 673, 600, 34},
};

constexpr float kEncoderX[4] = {1315, 1455, 1597, 1737}, kEncoderY[2] = {122, 597}, kEncoderRadius = 23;
constexpr float kDataX = 1120, kDataY = 155, kDataRadius = 76;

// Displays: main, then the four above the encoders (top-left corner and size).
const juce::Rectangle<float> kDisplays[] = {
    {1030, 318, 180, 90}, {1247, 325, 135, 68}, {1388, 325, 135, 68}, {1529, 325, 135, 68}, {1670, 325, 135, 68}};

constexpr float kPadW = 82, kPadH = 62, kPadX = 1056, kPadPitch = 108.2f;
constexpr float kPadY[3] = {921, 828, 733};  // row centres, bottom row first
juce::Rectangle<float> padBounds(int row, int column) {
    return {kPadX + kPadPitch * static_cast<float>(column), kPadY[row] - kPadH / 2, kPadW, kPadH};
}

juce::Rectangle<float> buttonBounds(const Button& b) { return {b.x - b.w / 2, b.y - b.h / 2, b.w, b.h}; }
juce::Point<float> encoderCentre(int e) { return {kEncoderX[e & 3], kEncoderY[e >> 2]}; }

constexpr juce::uint32 kResizeSettleMs = 150;  // see paint()
const juce::Rectangle<float> kPlate(20, 20, 1960 - 40, 1000 - 40);  // the faceplate on the canvas
const juce::Colour kBackground(0xff1b1d21), kPanel(0xff26292e), kLabel(0xffb8bcc4), kLed(0xffffd27a), kOrange(0xffe0702a);

// The WS2812 LEDs run dim: show the hue at full brightness, faded by the level.
juce::Colour ledColour(std::uint32_t rgb, juce::Colour off) {
    const int r8 = static_cast<int>(rgb >> 16 & 0xFF), g8 = static_cast<int>(rgb >> 8 & 0xFF), b8 = static_cast<int>(rgb & 0xFF);
    const int peak = std::max({r8, g8, b8});
    if (peak == 0)
        return off;
    const float scale = 255.0f / static_cast<float>(peak);
    const juce::Colour light(static_cast<std::uint8_t>(static_cast<float>(r8) * scale), static_cast<std::uint8_t>(static_cast<float>(g8) * scale),
                             static_cast<std::uint8_t>(static_cast<float>(b8) * scale));
    return off.interpolatedWith(light, 0.35f + 0.6f * std::sqrt(static_cast<float>(peak) / 255.0f));
}
}  // namespace

PanelView::PanelView(HydraProcessor& processor)
    : processor_(processor), lights_(std::make_unique<hydra::FrontPanel::Lights>()) {
    screenTint_ = processor_.screenTint();
    buildPalette();
    for (auto& image : displays_) {
        image = juce::Image(juce::Image::RGB, hydra::Oled::kWidth, hydra::Oled::kHeight, false);
        image.clear(image.getBounds(), palette_[0]);
    }
    setOpaque(true);
    startTimerHz(60);
}

PanelView::~PanelView() = default;

void PanelView::followKeys() {
    const juce::ModifierKeys mods = juce::ModifierKeys::getCurrentModifiersRealtime();
    const bool overPanel = isMouseOver(true);
    for (std::size_t i = 0; i < std::size(kKeyButtons); ++i) {
        const KeyButton& k = kKeyButtons[i];
        // Not with Command, Control or Option (shortcuts such as Command-R).
        const bool down = overPanel && !mods.isCommandDown() && !mods.isCtrlDown() && !mods.isAltDown() &&
                          (isMacKeyDown(k.macKeyCode) || juce::KeyPress::isKeyCurrentlyDown(k.character));
        if (down == keysHeld_[i])
            continue;
        keysHeld_[i] = down;
        processor_.holdButton(k.button, HydraProcessor::kFromKeyboard, down);
    }
}

void PanelView::followShiftKey() {
    const bool down = isMouseOver(true) && juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown();
    if (down == shiftKeyHeld_)
        return;
    shiftKeyHeld_ = down;
    if (!shiftLatched_ && !shiftForGesture_) {
        if (down)
            pressShift();
        else
            releaseShift(kShiftTrailMs);
    }
    repaint();
}

// The filter knobs are pots: the firmware reads their position but never moves
// them. When a MIDI CC changes what a knob controls, the knob is drawn at that
// value (only drawn: the pot the firmware reads stays put, so nothing is sent).
// Which filter the knobs control follows the FILTER 1 / FILTER 2 button lights.
void PanelView::followControllers() {
    if (controllerSerial_ == 0)
        controllersShown_.fill(-1);
    if (!processor_.readControllers(controllers_, controllerSerial_))
        return;
    constexpr int kFilter1Led = 26, kFilter2Led = 27;
    const bool filter2 = live_ && lights_ && lights_->buttons[kFilter2Led] > lights_->buttons[kFilter1Led];
    // Cutoff, resonance, drive/morph: CC 74, 71, 50 (Filter 1) or 55, 56 and none (Filter 2).
    constexpr int kFilter1Cc[hydra::FrontPanel::kKnobs] = {74, 71, 50};
    constexpr int kFilter2Cc[hydra::FrontPanel::kKnobs] = {55, 56, -1};
    bool moved = false;
    for (int k = 0; k < hydra::FrontPanel::kKnobs; ++k) {
        const int cc = filter2 ? kFilter2Cc[k] : kFilter1Cc[k];
        if (cc < 0)
            continue;
        const int value = controllers_[static_cast<std::size_t>(cc)];
        if (value < 0 || value == controllersShown_[static_cast<std::size_t>(cc)])
            continue;
        knobValues_[static_cast<std::size_t>(k)] = static_cast<float>(value) / 127.0f;
        moved = true;
    }
    // The arpeggiator switches: Mode (CC 108, 0-7), Octave (CC 120, 1-4) and
    // Division (CC 106: the switch has 1/4, 1/8, 1/16, 1/32 = values 2-5).
    const auto follow = [&](int cc, int switchIndex, int firstValue) {
        const int value = controllers_[static_cast<std::size_t>(cc)];
        if (value < 0 || value == controllersShown_[static_cast<std::size_t>(cc)])
            return;
        const int position = value - firstValue;
        if (position < 0 || position >= hydra::FrontPanel::kArpPositions[static_cast<std::size_t>(switchIndex)])
            return;  // a value the switch has no position for
        arpPositions_[static_cast<std::size_t>(switchIndex)] = position;
        moved = true;
    };
    follow(108, hydra::FrontPanel::kArpMode, 0);
    follow(120, hydra::FrontPanel::kArpOctave, 1);
    follow(106, hydra::FrontPanel::kArpDivision, 2);
    controllersShown_ = controllers_;
    if (moved)
        repaint();
}

void PanelView::timerCallback() {
    followShiftKey();
    followKeys();
    // Buttons held by a key or a MIDI CC: show them pressed.
    for (const int id : kMappedButtons) {
        const bool held = processor_.buttonHeldBy(id, HydraProcessor::kFromMidi | HydraProcessor::kFromKeyboard);
        if (held == heldElsewhere_[static_cast<std::size_t>(id)])
            continue;
        heldElsewhere_[static_cast<std::size_t>(id)] = held;
        for (const Button& b : kButtons)
            if (b.id == id)
                repaint(buttonBounds(b).expanded(4).transformedBy(juce::AffineTransform::scale(
                    static_cast<float>(getWidth()) / kCanvasWidth, static_cast<float>(getHeight()) / kCanvasHeight)).getSmallestIntegerContainer());
    }
    followControllers();
    // After a resize, draw the background anew once the size has settled.
    if (background_.isValid() && backgroundSize_ != getLocalBounds().getBottomRight() &&
        juce::Time::getMillisecondCounter() - resizedAt_ > kResizeSettleMs)
        repaint();
    if (!processor_.readPanel(*lights_, generation_))
        return;
    const bool first = !live_;
    live_ = true;
    if (!shown_)
        shown_ = std::make_unique<hydra::FrontPanel::Lights>();
    // Only what changed is repainted: the displays that differ, the buttons
    // whose LED level moved by more than a 32nd (the levels are PWM samples and
    // jitter), the encoder rings and pads that differ.
    juce::Rectangle<int> dirty;
    const auto mark = [&](juce::Rectangle<float> canvas) {
        const float sx = static_cast<float>(getWidth()) / kCanvasWidth, sy = static_cast<float>(getHeight()) / kCanvasHeight;
        const juce::Rectangle<int> r(static_cast<int>(std::floor(canvas.getX() * sx)) - 1, static_cast<int>(std::floor(canvas.getY() * sy)) - 1,
                                     static_cast<int>(std::ceil(canvas.getWidth() * sx)) + 3, static_cast<int>(std::ceil(canvas.getHeight() * sy)) + 3);
        dirty = dirty.isEmpty() ? r : dirty.getUnion(r);
    };
    for (int d = 0; d < hydra::PanelDisplays::kCount; ++d) {
        const auto i = static_cast<std::size_t>(d);
        const auto& pixels = lights_->displays[i];
        if (!first && pixels == shown_->displays[i])
            continue;
        shown_->displays[i] = pixels;
        convertDisplay(d, pixels);
        mark(kDisplays[d].expanded(4));
    }
    for (const Button& b : kButtons) {
        if (b.led < 0)
            continue;
        const auto i = static_cast<std::size_t>(b.led);
        if (!first && std::lround(lights_->buttons[i] * 32) == std::lround(shown_->buttons[i] * 32))
            continue;
        shown_->buttons[i] = lights_->buttons[i];
        mark(buttonBounds(b));
    }
    for (int e = 0; e < hydra::FrontPanel::kEncoders; ++e) {
        const auto i = static_cast<std::size_t>(e);
        if (!first && lights_->rings[i] == shown_->rings[i])
            continue;
        shown_->rings[i] = lights_->rings[i];
        const float reach = kEncoderRadius + 14;
        mark(juce::Rectangle<float>(2 * reach, 2 * reach).withCentre(encoderCentre(e)));
    }
    for (int row = 0; row < hydra::FrontPanel::kPadRows; ++row)
        for (int column = 0; column < hydra::FrontPanel::kPadColumns; ++column) {
            const auto i = static_cast<std::size_t>(row * hydra::FrontPanel::kPadColumns + column);
            if (!first && lights_->pads[i] == shown_->pads[i])
                continue;
            shown_->pads[i] = lights_->pads[i];
            mark(padBounds(row, column));
        }
    if (first || lights_->patchRing != shown_->patchRing) {
        shown_->patchRing = lights_->patchRing;
        const float reach = kDataRadius + 22;
        mark(juce::Rectangle<float>(2 * reach, 2 * reach).withCentre({kDataX, kDataY}));
    }
    if (first)
        repaint();
    else if (!dirty.isEmpty())
        repaint(dirty);
}

juce::Point<float> PanelView::toCanvas(juce::Point<float> p) const {
    return {p.x * kCanvasWidth / static_cast<float>(getWidth()), p.y * kCanvasHeight / static_cast<float>(getHeight())};
}

namespace {
// Screen colours (lit, unlit), switched by right-clicking a screen.
struct ScreenTint {
    const char* name;
    juce::uint32 lit, unlit;
};
const ScreenTint kScreenTints[] = {
    {"White", 0xffeef4ff, 0xff060709},
    {"Ice blue", 0xffa6e0ff, 0xff03060a},
    {"Amber", 0xffffb54d, 0xff0a0603},
    {"Green", 0xff86ffb8, 0xff030a06},
};
constexpr float kPixelFull = 175.0f;  // the brightest level the OLED model produces

// The encoder rings: 15 LEDs over 270 degrees, from bottom left to bottom right.
float ringAngle(int led) { return juce::MathConstants<float>::pi * (-0.75f + 1.5f * static_cast<float>(led) / 14.0f); }

juce::Rectangle<float> circle(juce::Point<float> c, float r) { return juce::Rectangle<float>(2 * r, 2 * r).withCentre(c); }

// A tile of brushed-metal grain: horizontal streaks that vary slowly along the
// row, plus fine noise; white and black at low alpha over the panel colour.
// Seamless (periodic in both directions), made once.
juce::Image makeBrushedTile() {
    constexpr int kSize = 512;
    juce::Image tile(juce::Image::ARGB, kSize, kSize, true);
    juce::Random random(0x5D1A);
    std::vector<float> streak(kSize), phase(kSize);
    std::vector<int> cycles(kSize);
    for (int y = 0; y < kSize; ++y) {
        streak[static_cast<std::size_t>(y)] = random.nextFloat() * 2 - 1;
        phase[static_cast<std::size_t>(y)] = random.nextFloat() * juce::MathConstants<float>::twoPi;
        cycles[static_cast<std::size_t>(y)] = 1 + random.nextInt(3);
    }
    juce::Image::BitmapData data(tile, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < kSize; ++y) {
        const auto at = [&](int row) { return streak[static_cast<std::size_t>((row + kSize) % kSize)]; };
        const float s = 0.5f * at(y) + 0.25f * (at(y - 1) + at(y + 1));
        const auto row = static_cast<std::size_t>(y);
        for (int x = 0; x < kSize; ++x) {
            const float along = 0.55f + 0.45f * std::sin(juce::MathConstants<float>::twoPi * static_cast<float>(cycles[row] * x) / kSize + phase[row]);
            const float n = s * along * 0.75f + (random.nextFloat() * 2 - 1) * 0.25f;
            const auto alpha = static_cast<std::uint8_t>(std::min(255.0f, std::abs(n) * 20.0f));
            data.setPixelColour(x, y, (n > 0 ? juce::Colours::white : juce::Colours::black).withAlpha(alpha));
        }
    }
    return tile;
}

// Text printed on the panel, with a faint shadow below it so it sits in the metal.
void engrave(juce::Graphics& g, const juce::String& text, juce::Rectangle<float> r, juce::Justification j, juce::Colour colour) {
    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.drawText(text, r.translated(0, 1.2f), j);
    g.setColour(colour);
    g.drawText(text, r, j);
}

void drawScrew(juce::Graphics& g, juce::Point<float> c) {
    constexpr float r = 7;
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.fillEllipse(circle(c.translated(0, 1), r + 1));
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff9aa0a8), c.x - r, c.y - r, juce::Colour(0xff3b3f45), c.x + r, c.y + r, false));
    g.fillEllipse(circle(c, r));
    g.setColour(juce::Colours::black.withAlpha(0.65f));
    g.drawLine(c.x - r * 0.6f, c.y - r * 0.6f, c.x + r * 0.6f, c.y + r * 0.6f, 1.6f);
    g.drawLine(c.x - r * 0.6f, c.y + r * 0.6f, c.x + r * 0.6f, c.y - r * 0.6f, 1.6f);
}

// A machined knob: knurled skirt, spun-metal cap with fixed light from the top
// left. Silver for the analog knobs and switches, gunmetal for the encoders.
// Static: only the pointer moves (drawPointer).
constexpr float kCapRatio = 0.76f;
void drawMetalKnob(juce::Graphics& g, juce::Point<float> c, float r, bool dark) {
    {  // shadow on the panel, falling slightly downwards
        juce::ColourGradient shadow(juce::Colours::black.withAlpha(0.6f), c.x, c.y + r * 0.18f, juce::Colours::transparentBlack,
                                    c.x + r * 1.4f, c.y + r * 0.18f, true);
        g.setGradientFill(shadow);
        g.fillEllipse(circle(c.translated(0, r * 0.18f), r * 1.4f));
    }
    const juce::Colour skirtTop = dark ? juce::Colour(0xff40444b) : juce::Colour(0xff969ca4);
    const juce::Colour skirtBottom = dark ? juce::Colour(0xff111215) : juce::Colour(0xff2a2d32);
    g.setGradientFill(juce::ColourGradient(skirtTop, c.x, c.y - r, skirtBottom, c.x, c.y + r, false));
    g.fillEllipse(circle(c, r));
    const int ridges = std::max(28, static_cast<int>(r * 1.7f));
    const float ridgeWidth = std::max(0.8f, r * 0.035f);
    for (int i = 0; i < ridges; ++i) {
        const float angle = juce::MathConstants<float>::twoPi * static_cast<float>(i) / static_cast<float>(ridges);
        g.setColour((i & 1) ? juce::Colours::white.withAlpha(dark ? 0.08f : 0.17f) : juce::Colours::black.withAlpha(0.32f));
        g.drawLine(juce::Line<float>(c.getPointOnCircumference(r * 0.80f, angle), c.getPointOnCircumference(r * 0.985f, angle)), ridgeWidth);
    }
    g.setColour(juce::Colours::black.withAlpha(0.75f));
    g.drawEllipse(circle(c, r), 1.2f);

    const float rc = r * kCapRatio;
    const juce::Colour capLight = dark ? juce::Colour(0xff80868f) : juce::Colour(0xffeef0f2);
    const juce::Colour capDark = dark ? juce::Colour(0xff22252a) : juce::Colour(0xff767c85);
    juce::ColourGradient cap(capLight, c.x - rc * 0.7f, c.y - rc * 0.8f, capDark, c.x + rc * 0.7f, c.y + rc * 0.9f, false);
    cap.addColour(0.5, capLight.interpolatedWith(capDark, 0.45f));
    g.setGradientFill(cap);
    g.fillEllipse(circle(c, rc));
    // Concentric machining marks.
    const float step = std::max(0.9f, rc / 22.0f);
    int ring = 0;
    for (float rr = rc * 0.1f; rr < rc - 0.5f; rr += step, ++ring) {
        g.setColour((ring & 1) ? juce::Colours::white.withAlpha(0.05f) : juce::Colours::black.withAlpha(0.06f));
        g.drawEllipse(circle(c, rr), 0.6f);
    }
    // The cones of light a spun finish shows, opposite each other.
    for (const float a : {-0.80f, juce::MathConstants<float>::pi - 0.80f}) {
        juce::Path wedge;
        wedge.addPieSegment(circle(c, rc), a - 0.24f, a + 0.24f, 0.0f);
        g.setColour(juce::Colours::white.withAlpha(dark ? 0.08f : 0.22f));
        g.fillPath(wedge);
    }
    // Rim: bright where the light hits, dark below.
    g.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(0.6f), c.x, c.y - rc, juce::Colours::black.withAlpha(0.55f),
                                           c.x, c.y + rc, false));
    g.drawEllipse(circle(c, rc), std::max(1.0f, r * 0.045f));
}

// The knob's position mark on the cap (orange, as the panel's accents).
void drawPointer(juce::Graphics& g, juce::Point<float> c, float r, float angle, juce::Colour colour) {
    const float rc = r * kCapRatio;
    const juce::Point<float> a = c.getPointOnCircumference(rc * 0.28f, angle), b = c.getPointOnCircumference(rc * 0.9f, angle);
    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.drawLine(juce::Line<float>(a, b).withShortenedStart(-0.5f), r * 0.12f);
    g.setColour(colour);
    g.drawLine(juce::Line<float>(a, b), r * 0.075f);
}

// A rubber button: body, top highlight, the LED slot at its bottom.
juce::Rectangle<float> ledSlot(juce::Rectangle<float> button) { return button.reduced(4, 3).removeFromBottom(5); }
void drawButtonBody(juce::Graphics& g, juce::Rectangle<float> r) {
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.fillRoundedRectangle(r.translated(0, 2.5f).expanded(0.5f), 6);
    g.setGradientFill(juce::ColourGradient(juce::Colour(0xff4a4e56), 0, r.getY(), juce::Colour(0xff2a2d32), 0, r.getBottom(), false));
    g.fillRoundedRectangle(r, 5);
    g.setColour(juce::Colours::white.withAlpha(0.13f));
    g.drawLine(r.getX() + 5, r.getY() + 1.2f, r.getRight() - 5, r.getY() + 1.2f, 1.2f);
    g.setColour(juce::Colours::black.withAlpha(0.6f));
    g.drawRoundedRectangle(r, 5, 1.0f);
}

bool shows(juce::Graphics& g, juce::Rectangle<float> canvas) { return g.clipRegionIntersects(canvas.getSmallestIntegerContainer().expanded(2)); }
}  // namespace

namespace {
// The detent of an arp switch position in the shared raster of eight (0 = bottom
// left, clockwise), checked against the firmware's arp edit page.
int arpSlot(int sw, int position) { return sw == hydra::FrontPanel::kArpOctave ? position + 4 : position; }

// The printed position names, in the raster's order (empty: no detent).
const char* const kArpLegends[hydra::FrontPanel::kArpSwitches][8] = {
    {"UP", "DOWN", "UP / DN", "UP & DN", "ORDER", "RANDOM", "CHORD", "PHRASE"},
    {"", "", "", "", "1", "2", "3", "4"},
    {"1/4", "1/8", "1/16", "1/32", "", "", "", ""},
    {"OFF", "52%", "54%", "58%", "62%", "66%", "70%", "75%"},
};

// The position names around an arp switch, each joined to its detent by a
// leader line: the left four bottom to top, the right four top to bottom.
void paintArpLegends(juce::Graphics& g, const Knob& k) {
    const juce::Point<float> c(k.x, k.y);
    constexpr float kRow = 12.5f;
    g.setFont(juce::FontOptions(9.5f, juce::Font::bold));
    for (int slot = 0; slot < 8; ++slot) {
        const char* legend = kArpLegends[k.index][slot];
        if (!*legend)
            continue;
        const bool left = slot < 4;
        const float angle = juce::MathConstants<float>::pi * (-0.75f + 1.5f * static_cast<float>(slot) / 7.0f);
        const float y = left ? k.y + (1.5f - static_cast<float>(slot)) * kRow : k.y + (static_cast<float>(slot) - 5.5f) * kRow;
        const float side = left ? -1.0f : 1.0f;
        const juce::Point<float> from = c.getPointOnCircumference(k.radius + 3, angle);
        const juce::Point<float> elbow(k.x + side * (k.radius + 12), y), end(k.x + side * (k.radius + 18), y);
        g.setColour(juce::Colour(0xff7d828b));
        g.drawLine(juce::Line<float>(from, elbow), 1.2f);
        g.drawLine(juce::Line<float>(elbow, end), 1.2f);
        const juce::Rectangle<float> box = left ? juce::Rectangle<float>(end.x - 2 - 60, y - 7, 60, 14) : juce::Rectangle<float>(end.x + 2, y - 7, 60, 14);
        engrave(g, legend, box, left ? juce::Justification::centredRight : juce::Justification::centredLeft, kLabel);
    }
}
}  // namespace

// The signal-flow lines of the Module Select section, drawn behind its buttons.
void PanelView::paintModuleLines(juce::Graphics& g) {
    const auto box = [](int id) {
        for (const Button& b : kButtons)
            if (b.id == id && (b.y > 700))  // the module buttons (FILTER 1/2 also exist in the filter section)
                return buttonBounds(b);
        return juce::Rectangle<float>();
    };
    enum : int { kOsc1 = 11, kOsc2 = 12, kOsc3 = 13, kMut1 = 14, kMut2 = 15, kMut3 = 16, kMut4 = 3, kRing = 17, kMixer = 0,
                 kFilter1 = 24, kFilter2 = 25, kAmp = 1, kPreFx = 2, kDelay = 4, kReverb = 5, kPostFx = 6,
                 kEnv1 = 70, kEnv2 = 71, kEnv5 = 18, kLfo1 = 66, kLfo2 = 67, kLfo5 = 47 };
    const auto line = [&g](float x1, float y1, float x2, float y2) { g.drawLine(x1, y1, x2, y2, 2.0f); };
    const auto dashed = [&g](float x1, float y1, float x2, float y2, float width, float dash, float gap) {
        const float pattern[] = {dash, gap};
        g.drawDashedLine(juce::Line<float>(x1, y1, x2, y2), pattern, 2, width);
    };

    // Orange lines behind the envelope and LFO rows.
    g.setColour(kOrange);
    for (const int id : {kEnv1, kLfo1}) {
        const float y = box(id).getCentreY();
        g.drawLine(box(id).getX() - 45, y, box(id == kEnv1 ? kEnv5 : kLfo5).getRight() + 35, y, 4.0f);
    }

    g.setColour(juce::Colour(0xff9a9fa8));
    // Oscillators to mutants, and on to the mixer bus.
    for (const auto& [a, b] : {std::pair{kOsc1, kMut1}, {kMut1, kMut2}, {kOsc2, kMut3}, {kMut3, kMut4}, {kOsc3, kRing}})
        line(box(a).getRight(), box(a).getCentreY(), box(b).getX(), box(a).getCentreY());
    const float mixBus = (box(kMut2).getRight() + box(kMixer).getX()) / 2;
    for (const int id : {kMut2, kMut4, kRing})
        line(box(id).getRight(), box(id).getCentreY(), mixBus, box(id).getCentreY());
    line(mixBus, box(kMut2).getCentreY(), mixBus, box(kRing).getCentreY());
    line(mixBus, box(kMixer).getCentreY(), box(kMixer).getX(), box(kMixer).getCentreY());

    // Mixer to the filters (series or parallel) and on to the amp.
    const float inBus = (box(kMixer).getRight() + box(kFilter1).getX()) / 2;
    const float outBus = (box(kFilter1).getRight() + box(kAmp).getX()) / 2;
    line(box(kMixer).getRight(), box(kMixer).getCentreY(), inBus, box(kMixer).getCentreY());
    line(outBus, box(kAmp).getCentreY(), box(kAmp).getX(), box(kAmp).getCentreY());
    for (const float bus : {inBus, outBus}) {
        line(bus, box(kFilter1).getCentreY(), bus, box(kFilter2).getCentreY());
        for (const int id : {kFilter1, kFilter2})
            line(bus, box(id).getCentreY(), bus == inBus ? box(id).getX() : box(id).getRight(), box(id).getCentreY());
    }
    line(box(kFilter1).getCentreX(), box(kFilter1).getBottom(), box(kFilter1).getCentreX(), box(kFilter2).getY());

    // The pre-wired modulators: ENV 1 / LFO 1 to the filters, ENV 2 / LFO 2 to the amp.
    for (const auto& [top, middleTop, middleBottom, bottom] :
         {std::tuple{kEnv1, kFilter1, kFilter2, kLfo1}, std::tuple{kEnv2, kAmp, kAmp, kLfo2}}) {
        const float x = box(middleTop).getCentreX();
        dashed(x, box(top).getBottom(), x, box(middleTop).getY(), 2.0f, 6.0f, 5.0f);
        dashed(x, box(middleBottom).getBottom(), x, box(bottom).getY(), 2.0f, 6.0f, 5.0f);
    }

    // Amp, pre-FX, delay and reverb (in series or parallel), post-FX.
    line(box(kAmp).getRight(), box(kAmp).getCentreY(), box(kPreFx).getX(), box(kPreFx).getCentreY());
    const float fxIn = (box(kPreFx).getRight() + box(kDelay).getX()) / 2;
    const float fxOut = (box(kDelay).getRight() + box(kPostFx).getX()) / 2;
    line(box(kPreFx).getRight(), box(kPreFx).getCentreY(), fxIn, box(kPreFx).getCentreY());
    line(fxOut, box(kPostFx).getCentreY(), box(kPostFx).getX(), box(kPostFx).getCentreY());
    for (const float bus : {fxIn, fxOut}) {
        line(bus, box(kDelay).getCentreY(), bus, box(kReverb).getCentreY());
        for (const int id : {kDelay, kReverb})
            line(bus, box(id).getCentreY(), bus == fxIn ? box(id).getX() : box(id).getRight(), box(id).getCentreY());
    }
    line(box(kDelay).getCentreX(), box(kDelay).getBottom(), box(kDelay).getCentreX(), box(kReverb).getY());
}

void PanelView::paintHeadings(juce::Graphics& g) {
    g.setFont(juce::FontOptions(20.0f, juce::Font::bold));
    for (const Heading& h : kHeadings) {
        const juce::Rectangle<float> r(h.x - h.w / 2, h.y, h.w, 26);
        // A recessed plate with the heading in orange.
        g.setColour(juce::Colours::white.withAlpha(0.06f));
        g.fillRoundedRectangle(r.translated(0, 1.2f), 4);
        g.setColour(juce::Colour(0xff17191c));
        g.fillRoundedRectangle(r, 4);
        g.setColour(kOrange);
        g.drawText(h.text, r, juce::Justification::centred);
    }
}


void PanelView::buildPalette() {
    const ScreenTint& tint = kScreenTints[static_cast<std::size_t>(screenTint_) % std::size(kScreenTints)];
    const juce::Colour lit(tint.lit), unlit(tint.unlit);
    for (int v = 0; v < 256; ++v)
        palette_[static_cast<std::size_t>(v)] = unlit.interpolatedWith(lit, std::min(1.0f, static_cast<float>(v) / kPixelFull));
}

void PanelView::convertDisplay(int d, const hydra::Oled::Image& pixels) {
    juce::Image::BitmapData bitmap(displays_[static_cast<std::size_t>(d)], juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < hydra::Oled::kHeight; ++y)
        for (int x = 0; x < hydra::Oled::kWidth; ++x)
            bitmap.setPixelColour(x, y, palette_[pixels[static_cast<std::size_t>(y * hydra::Oled::kWidth + x)]]);
}

void PanelView::cycleScreenTint() {
    screenTint_ = (screenTint_ + 1) % static_cast<int>(std::size(kScreenTints));
    processor_.setScreenTint(screenTint_);
    buildPalette();
    if (shown_)
        for (int d = 0; d < hydra::PanelDisplays::kCount; ++d)
            convertDisplay(d, shown_->displays[static_cast<std::size_t>(d)]);
    background_ = {};  // the glass behind the screens has the unlit colour
    repaint();
}

void PanelView::resized() { resizedAt_ = juce::Time::getMillisecondCounter(); }

// Everything that does not change while playing, drawn once into background_
// at the screen's pixel resolution: panel, printed text, the module diagram,
// knob and button bodies, screen bezels, pad frames. paint() copies it and
// draws only the live parts on top.
void PanelView::renderBackground(float pixelScale) {
    const int w = std::max(1, juce::roundToInt(static_cast<float>(getWidth()) * pixelScale));
    const int h = std::max(1, juce::roundToInt(static_cast<float>(getHeight()) * pixelScale));
    background_ = juce::Image(juce::Image::RGB, w, h, false);
    backgroundSize_ = getLocalBounds().getBottomRight();
    backgroundScale_ = pixelScale;
    if (!brushed_.isValid())
        brushed_ = makeBrushedTile();
    juce::Graphics g(background_);
    g.fillAll(kBackground);
    const juce::AffineTransform canvas = juce::AffineTransform::scale(static_cast<float>(w) / kCanvasWidth, static_cast<float>(h) / kCanvasHeight);

    // The faceplate, in pixels: the grain tile is copied unscaled (cheap), not
    // resampled through the canvas transform.
    const juce::Rectangle<float> plate = kPlate.transformedBy(canvas);
    const float corner = 16 * static_cast<float>(w) / kCanvasWidth;
    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.fillRoundedRectangle(plate.translated(0, 6 * static_cast<float>(h) / kCanvasHeight), corner * 1.1f);
    juce::ColourGradient base(juce::Colour(0xff383b42), 0, plate.getY(), juce::Colour(0xff1d1f23), 0, plate.getBottom(), false);
    base.addColour(0.4, juce::Colour(0xff2b2e33));
    g.setGradientFill(base);
    g.fillRoundedRectangle(plate, corner);
    {
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(plate.reduced(corner * 0.6f).getSmallestIntegerContainer());
        const int tile = brushed_.getWidth();
        for (int y = static_cast<int>(plate.getY()); y < static_cast<int>(plate.getBottom()); y += tile)
            for (int x = static_cast<int>(plate.getX()); x < static_cast<int>(plate.getRight()); x += tile)
                g.drawImageAt(brushed_, x, y);
    }
    g.addTransform(canvas);
    paintStatic(g);
}

void PanelView::paintStatic(juce::Graphics& g) {
    // The faceplate's edges (its surface is drawn in renderBackground).
    const juce::Rectangle<float> plate = kPlate;
    g.setColour(juce::Colours::white.withAlpha(0.10f));
    g.drawRoundedRectangle(plate.reduced(1.5f), 15, 1.5f);
    g.setColour(juce::Colours::black.withAlpha(0.7f));
    g.drawRoundedRectangle(plate, 16, 2);
    for (const juce::Point<float> corner : {plate.getTopLeft().translated(22, 22), plate.getTopRight().translated(-22, 22),
                                            plate.getBottomLeft().translated(22, -22), plate.getBottomRight().translated(-22, -22)})
        drawScrew(g, corner);

    g.setFont(juce::FontOptions(40.0f, juce::Font::bold));
    engrave(g, "SINOWI", juce::Rectangle<float>(70, 70, 420, 60), juce::Justification::centredLeft, kLabel);
    paintHeadings(g);
    paintModuleLines(g);

    // Screen bezels, with the glass in the screens' unlit colour.
    // The four small screens share one window, as on the hardware.
    const juce::Colour glass = palette_[0];
    const juce::Rectangle<float> smallScreens = kDisplays[1].getUnion(kDisplays[4]);
    for (const juce::Rectangle<float>& bezel : {kDisplays[0].expanded(11), smallScreens.expanded(11)}) {
        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.fillRoundedRectangle(bezel.translated(0, 2.5f), 7);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff17191c), 0, bezel.getY(), juce::Colour(0xff0b0c0e), 0, bezel.getBottom(), false));
        g.fillRoundedRectangle(bezel, 7);
        g.setColour(juce::Colours::white.withAlpha(0.08f));
        g.drawRoundedRectangle(bezel.reduced(0.6f), 7, 1.0f);
    }
    for (const juce::Rectangle<float>& r : kDisplays) {
        g.setColour(glass);
        g.fillRect(r.expanded(4));
        g.setColour(juce::Colours::black.withAlpha(0.8f));
        g.drawRect(r.expanded(4), 1.2f);
    }

    // Buttons, with their printed labels and legends.
    for (const Button& b : kButtons) {
        const juce::Rectangle<float> r = buttonBounds(b);
        drawButtonBody(g, r);
        if (b.led >= 0) {
            g.setColour(juce::Colour(0xff141518));
            g.fillRoundedRectangle(ledSlot(r), 2);
        }
        g.setFont(juce::FontOptions(b.w < 72 && std::strlen(b.label) > 7 ? 10.0f : 12.0f));
        g.setColour(juce::Colours::black.withAlpha(0.5f));
        g.drawFittedText(b.label, r.reduced(3, 2).toNearestInt().withTrimmedBottom(5).translated(0, 1), juce::Justification::centred, 2);
        g.setColour(kLabel);
        g.drawFittedText(b.label, r.reduced(3, 2).toNearestInt().withTrimmedBottom(5), juce::Justification::centred, 2);
        for (const Legend& legend : kShiftLegends)
            if (legend.id == b.id) {
                g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
                engrave(g, legend.text, juce::Rectangle<float>(r.getX() - 20, r.getBottom() + 4, r.getWidth() + 40, 14),
                        juce::Justification::centred, kOrange);
            }
        if (b.id >= 33 && b.id <= 40) {  // the Control buttons' numbers
            const int number = b.id - 32;
            const float y = number <= 4 ? r.getBottom() + 4 : r.getY() - 18;
            g.setFont(juce::FontOptions(12.0f));
            engrave(g, juce::String(number), juce::Rectangle<float>(r.getX(), y, r.getWidth(), 14), juce::Justification::centred, kLabel);
        }
        if (b.id == 61) {  // DOWN: the pair is labelled PAGE
            g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
            engrave(g, "PAGE", juce::Rectangle<float>(r.getX() - 10, r.getBottom() + 5, r.getWidth() + 20, 14), juce::Justification::centred, kLabel);
        }
    }

    // Knob bodies, the arp switch legends and the names.
    for (const Knob& k : kKnobs) {
        const juce::Point<float> c(k.x, k.y);
        drawMetalKnob(g, c, k.radius, false);
        if (k.kind == Knob::Kind::Arp)
            paintArpLegends(g, k);
        if (k.kind == Knob::Kind::Volume) {
            g.setFont(juce::FontOptions(12.0f));
            engrave(g, k.label, juce::Rectangle<float>(140, 16).withCentre(c.translated(0, -k.radius - 16)), juce::Justification::centred, kLabel);
        } else {
            g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
            engrave(g, k.label, juce::Rectangle<float>(140, 16).withCentre(c.translated(0, k.radius + 18)), juce::Justification::centred, kLabel);
        }
    }

    // Encoders: gunmetal knobs inside rings of LED holes; the patch knob.
    for (int e = 0; e < hydra::FrontPanel::kEncoders; ++e) {
        const juce::Point<float> c = encoderCentre(e);
        for (int l = 0; l < hydra::FrontPanel::kRingLeds; ++l) {
            const juce::Point<float> at = c.getPointOnCircumference(kEncoderRadius + 10, ringAngle(l));
            g.setColour(juce::Colour(0xff121316));
            g.fillEllipse(circle(at, 3.6f));
            g.setColour(juce::Colours::white.withAlpha(0.06f));
            g.drawEllipse(circle(at.translated(0, 0.6f), 3.6f), 0.8f);
        }
        drawMetalKnob(g, c, kEncoderRadius, true);
    }
    {
        const juce::Point<float> c(kDataX, kDataY);
        g.setColour(juce::Colour(0xff111215));
        g.drawEllipse(circle(c, kDataRadius + 9), 11.0f);  // the ring's channel
        drawMetalKnob(g, c, kDataRadius, true);
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        engrave(g, "PATCH", juce::Rectangle<float>(80, 16).withCentre(c), juce::Justification::centred, kLabel);
    }

    // Pad frames and their SHIFT legends.
    for (int row = 0; row < hydra::FrontPanel::kPadRows; ++row)
        for (int column = 0; column < hydra::FrontPanel::kPadColumns; ++column) {
            const juce::Rectangle<float> r = padBounds(row, column);
            g.setColour(juce::Colours::black.withAlpha(0.6f));
            g.fillRoundedRectangle(r.expanded(4), 9);
            g.setColour(juce::Colours::white.withAlpha(0.07f));
            g.drawRoundedRectangle(r.expanded(4).translated(0, 1), 9, 1.0f);
            const char* legend = kPadLegends[row][column];
            if (!*legend)
                continue;
            const juce::Rectangle<float> label(r.getX() - 10, r.getBottom() + 6, r.getWidth() + 20, 14);
            g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            if (std::strcmp(legend, "KEY") == 0) {  // printed on an orange field
                const juce::Rectangle<float> box = label.withSizeKeepingCentre(36, 14);
                g.setColour(kOrange);
                g.fillRoundedRectangle(box, 2);
                g.setColour(juce::Colour(0xff1b1d21));
                g.drawText(legend, box, juce::Justification::centred);
            } else {
                engrave(g, legend, label, juce::Justification::centred, kOrange);
            }
        }
}

void PanelView::paint(juce::Graphics& g) {
    const float pixelScale = g.getInternalContext().getPhysicalPixelScaleFactor();
    const bool sizeChanged = backgroundSize_ != getLocalBounds().getBottomRight() || backgroundScale_ != pixelScale;
    // While the window is being resized the old background is stretched; it is
    // drawn anew once the size has settled (see timerCallback).
    if (!background_.isValid() || (sizeChanged && juce::Time::getMillisecondCounter() - resizedAt_ > kResizeSettleMs))
        renderBackground(pixelScale);
    g.setImageResamplingQuality(sizeChanged ? juce::Graphics::mediumResamplingQuality : juce::Graphics::lowResamplingQuality);
    g.drawImage(background_, getLocalBounds().toFloat());

    g.addTransform(juce::AffineTransform::scale(static_cast<float>(getWidth()) / kCanvasWidth,
                                                static_cast<float>(getHeight()) / kCanvasHeight));
    paintDisplays(g);
    paintButtonStates(g);
    paintKnobPointers(g);
    paintEncoderLights(g);
    paintPads(g);
    if (!live_) {
        g.setColour(kLabel);
        g.setFont(juce::FontOptions(28.0f));
        g.drawText("waiting for the emulator...", kDisplays[0].expanded(200, 0).translated(0, 100),
                   juce::Justification::centred);
    }
}

void PanelView::paintDisplays(juce::Graphics& g) {
    g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
    for (int d = 0; d < hydra::PanelDisplays::kCount; ++d) {
        const juce::Rectangle<float> r = kDisplays[d];
        if (!shows(g, r))
            continue;
        g.setOpacity(1.0f);
        g.drawImage(displays_[static_cast<std::size_t>(d)], r, juce::RectanglePlacement::stretchToFit);
        // The glass: a soft reflection over the top half.
        const juce::Rectangle<float> glass = r.expanded(4);
        g.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(0.07f), 0, glass.getY(), juce::Colours::transparentWhite, 0,
                                               glass.getY() + glass.getHeight() * 0.55f, false));
        g.fillRect(glass);
    }
}

void PanelView::paintButtonStates(juce::Graphics& g) {
    for (const Button& b : kButtons) {
        const juce::Rectangle<float> r = buttonBounds(b);
        if (!shows(g, r))
            continue;
        const bool held = (active_.kind == Target::Kind::Button && &kButtons[active_.index] == &b) ||
                          (b.id == kShiftButton && (shiftLatched_ || shiftForGesture_ || shiftKeyHeld_)) ||
                          heldElsewhere_[static_cast<std::size_t>(b.id)];
        if (held) {
            g.setColour(juce::Colours::black.withAlpha(0.28f));
            g.fillRoundedRectangle(r, 5);
        }
        if (b.led >= 0) {
            const float level = lights_->buttons[static_cast<std::size_t>(b.led)];
            if (level > 0.01f) {
                const juce::Rectangle<float> slot = ledSlot(r);
                g.setColour(kLed.withAlpha(0.18f * level));
                g.fillRoundedRectangle(slot.expanded(3, 2), 4);
                g.setColour(kLed.withAlpha(0.3f + 0.7f * level));
                g.fillRoundedRectangle(slot, 2);
            }
        }
    }
}

void PanelView::paintKnobPointers(juce::Graphics& g) {
    for (const Knob& k : kKnobs) {
        const juce::Point<float> c(k.x, k.y);
        const bool volume = k.kind == Knob::Kind::Volume;
        if (!shows(g, circle(c, k.radius).withHeight(volume ? 2 * k.radius + 34 : 2 * k.radius)))
            continue;
        float value = 0;
        switch (k.kind) {
        case Knob::Kind::Volume: value = processor_.outputGain().convertTo0to1(processor_.outputGain().get()); break;
        case Knob::Kind::Arp:
            // All four arp switches share one raster of eight detents over 270
            // degrees; OCTAVE uses the right four, DIVISION the left four.
            value = static_cast<float>(arpSlot(k.index, arpPositions_[static_cast<std::size_t>(k.index)])) / 7.0f;
            break;
        case Knob::Kind::Analog: value = knobValues_[static_cast<std::size_t>(k.index)]; break;
        }
        drawPointer(g, c, k.radius, juce::MathConstants<float>::pi * (-0.75f + 1.5f * value), kOrange);
        if (volume) {
            g.setFont(juce::FontOptions(12.0f));
            engrave(g, juce::String(processor_.outputGain().get(), 1) + " dB",
                    juce::Rectangle<float>(140, 16).withCentre(c.translated(0, k.radius + 16)), juce::Justification::centred, kLabel);
        }
    }
}

void PanelView::paintEncoderLights(juce::Graphics& g) {
    const juce::Colour lit(0xff6fd3ff);
    for (int e = 0; e < hydra::FrontPanel::kEncoders; ++e) {
        const juce::Point<float> c = encoderCentre(e);
        if (!shows(g, circle(c, kEncoderRadius + 16)))
            continue;
        for (int l = 0; l < hydra::FrontPanel::kRingLeds; ++l) {
            if (!lights_->rings[static_cast<std::size_t>(e)][static_cast<std::size_t>(l)])
                continue;
            const juce::Point<float> at = c.getPointOnCircumference(kEncoderRadius + 10, ringAngle(l));
            g.setColour(lit.withAlpha(0.22f));
            g.fillEllipse(circle(at, 6.5f));
            g.setColour(lit);
            g.fillEllipse(circle(at, 3.2f));
        }
    }
    // The patch knob's light ring, in the patch's colour.
    const juce::Point<float> c(kDataX, kDataY);
    if (lights_->patchRing != 0 && shows(g, circle(c, kDataRadius + 22))) {
        const juce::Rectangle<float> ring = circle(c, kDataRadius + 9);
        const juce::Colour colour = ledColour(lights_->patchRing, juce::Colour(0xff32363c));
        g.setColour(colour.withAlpha(0.25f));
        g.drawEllipse(ring.expanded(5), 6.0f);
        g.setColour(colour);
        g.drawEllipse(ring, 7.0f);
    }
}

void PanelView::paintPads(juce::Graphics& g) {
    for (int row = 0; row < hydra::FrontPanel::kPadRows; ++row)
        for (int column = 0; column < hydra::FrontPanel::kPadColumns; ++column) {
            const juce::Rectangle<float> r = padBounds(row, column);
            if (!shows(g, r))
                continue;
            const std::uint32_t rgb = lights_->pads[static_cast<std::size_t>(row * hydra::FrontPanel::kPadColumns + column)];
            g.setColour(ledColour(rgb, juce::Colour(0xff2e3238)));
            g.fillRoundedRectangle(r, 6);
            // Soft silicone: lighter towards the top.
            g.setGradientFill(juce::ColourGradient(juce::Colours::white.withAlpha(0.10f), 0, r.getY(), juce::Colours::transparentWhite, 0,
                                                   r.getCentreY(), false));
            g.fillRoundedRectangle(r, 6);
            const bool held = active_.kind == Target::Kind::Pad &&
                              active_.index == row * hydra::FrontPanel::kPadColumns + column;
            g.setColour(held ? juce::Colours::white : juce::Colours::black.withAlpha(0.5f));
            g.drawRoundedRectangle(r, 6, held ? 3.0f : 1.2f);
        }
}

PanelView::Target PanelView::targetAt(juce::Point<float> p) const {
    for (std::size_t i = 0; i < std::size(kButtons); ++i)
        if (buttonBounds(kButtons[i]).contains(p))
            return {Target::Kind::Button, static_cast<int>(i)};
    for (std::size_t i = 0; i < std::size(kKnobs); ++i)
        if (p.getDistanceFrom({kKnobs[i].x, kKnobs[i].y}) <= kKnobs[i].radius + 4)
            return {Target::Kind::Knob, static_cast<int>(i)};
    for (int e = 0; e < hydra::FrontPanel::kEncoders; ++e)
        if (p.getDistanceFrom(encoderCentre(e)) <= kEncoderRadius + 12)
            return {Target::Kind::Encoder, e};
    if (p.getDistanceFrom({kDataX, kDataY}) <= kDataRadius)
        return {Target::Kind::DataEncoder, 0};
    for (int row = 0; row < hydra::FrontPanel::kPadRows; ++row)
        for (int column = 0; column < hydra::FrontPanel::kPadColumns; ++column)
            if (padBounds(row, column).contains(p))
                return {Target::Kind::Pad, row * hydra::FrontPanel::kPadColumns + column};
    return {};
}

void PanelView::setKnob(int index, float value) {
    const Knob& k = kKnobs[index];
    value = juce::jlimit(0.0f, 1.0f, value);
    switch (k.kind) {
    case Knob::Kind::Volume: processor_.outputGain().setValueNotifyingHost(value); break;
    case Knob::Kind::Arp: {
        const int positions = hydra::FrontPanel::kArpPositions[static_cast<std::size_t>(k.index)];
        const int position = juce::roundToInt(value * static_cast<float>(positions - 1));
        if (position != arpPositions_[static_cast<std::size_t>(k.index)]) {
            arpPositions_[static_cast<std::size_t>(k.index)] = position;
            processor_.panelInput({Input::Kind::ArpSwitch, k.index, position, 0});
        }
        break;
    }
    case Knob::Kind::Analog:
        knobValues_[static_cast<std::size_t>(k.index)] = value;
        processor_.panelInput({Input::Kind::Knob, k.index, 0, value});
        break;
    }
    repaint();
}

void PanelView::turn(const Target& target, int detents) {
    if (detents == 0)
        return;
    switch (target.kind) {
    case Target::Kind::Encoder: processor_.panelInput({Input::Kind::Encoder, target.index, detents, 0}); break;
    case Target::Kind::DataEncoder: processor_.panelInput({Input::Kind::DataEncoder, 0, detents, 0}); break;
    case Target::Kind::Knob: {
        const Knob& k = kKnobs[target.index];
        if (k.kind == Knob::Kind::Arp) {
            const int positions = hydra::FrontPanel::kArpPositions[static_cast<std::size_t>(k.index)];
            const int position = arpPositions_[static_cast<std::size_t>(k.index)] + detents;
            setKnob(target.index, static_cast<float>(position) / static_cast<float>(positions - 1));
        } else {
            const float value = k.kind == Knob::Kind::Volume ? processor_.outputGain().convertTo0to1(processor_.outputGain().get())
                                                             : knobValues_[static_cast<std::size_t>(k.index)];
            setKnob(target.index, value + 0.02f * static_cast<float>(detents));
        }
        break;
    }
    default: break;
    }
}

void PanelView::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) {  // right-click (or ctrl-click) on a screen: next screen colour
        const juce::Point<float> p = toCanvas(e.position);
        for (const juce::Rectangle<float>& r : kDisplays)
            if (r.expanded(11).contains(p)) {
                active_ = {};
                cycleScreenTint();
                return;
            }
    }
    active_ = targetAt(toCanvas(e.position));
    dragStartY_ = e.position.y;
    dragAccumulator_ = 0;
    switch (active_.kind) {
    case Target::Kind::Button: {
        const int id = kButtons[active_.index].id;
        if (id == kShiftButton) {
            if (shiftLatched_)
                releaseShift(0);
            else
                pressShift();
            shiftLatched_ = !shiftLatched_;
            break;
        }
        int delay = 0;
        if (shiftKeyHeld_) {
            delay = kShiftLeadMs;  // SHIFT is down already; keep the order safe
        } else if (!shiftLatched_ && e.mods.isShiftDown()) {
            pressShift();
            shiftForGesture_ = true;
            delay = kShiftLeadMs;
        }
        processor_.holdButton(id, HydraProcessor::kFromPanel, true, delay);
        break;
    }
    case Target::Kind::Encoder:
    case Target::Kind::DataEncoder:
        if (!shiftLatched_ && !shiftKeyHeld_ && e.mods.isShiftDown()) {
            pressShift();
            shiftForGesture_ = true;
        }
        break;
    case Target::Kind::Pad: {
        // Pressure from where the pad is hit: harder towards its top.
        const int row = active_.index / hydra::FrontPanel::kPadColumns, column = active_.index % hydra::FrontPanel::kPadColumns;
        const juce::Rectangle<float> r = padBounds(row, column);
        const float pressure = juce::jlimit(0.2f, 1.0f, 1.0f - (toCanvas(e.position).y - r.getY()) / r.getHeight() * 0.8f);
        processor_.panelInput({Input::Kind::Pad, active_.index, 0, pressure});
        break;
    }
    case Target::Kind::Knob: {
        const Knob& k = kKnobs[active_.index];
        dragValue_ = k.kind == Knob::Kind::Volume ? processor_.outputGain().convertTo0to1(processor_.outputGain().get())
                     : k.kind == Knob::Kind::Arp
                         ? static_cast<float>(arpPositions_[static_cast<std::size_t>(k.index)]) /
                               static_cast<float>(hydra::FrontPanel::kArpPositions[static_cast<std::size_t>(k.index)] - 1)
                         : knobValues_[static_cast<std::size_t>(k.index)];
        break;
    }
    default: break;
    }
    repaint();
}

void PanelView::mouseDrag(const juce::MouseEvent& e) {
    const float dy = dragStartY_ - e.position.y;  // up is positive
    switch (active_.kind) {
    case Target::Kind::Knob: setKnob(active_.index, dragValue_ + dy / 200.0f); break;
    case Target::Kind::Encoder:
    case Target::Kind::DataEncoder: {
        // One detent per 8 pixels of vertical movement.
        const float total = dy / 8.0f;
        const int detents = static_cast<int>(total - dragAccumulator_);
        dragAccumulator_ += static_cast<float>(detents);
        turn(active_, detents);
        break;
    }
    default: break;
    }
}

void PanelView::mouseUp(const juce::MouseEvent&) {
    switch (active_.kind) {
    case Target::Kind::Button: {
        const int id = kButtons[active_.index].id;
        if (id == kShiftButton)
            break;  // latched or released by the click itself
        const int delay = shiftForGesture_ || shiftKeyHeld_ ? kShiftLeadMs : 0;
        processor_.holdButton(id, HydraProcessor::kFromPanel, false, delay);
        if (shiftLatched_ || shiftForGesture_) {
            releaseShift(delay + kShiftTrailMs);
            shiftLatched_ = shiftForGesture_ = false;
        }
        break;
    }
    case Target::Kind::Encoder:
    case Target::Kind::DataEncoder:
        if (shiftForGesture_) {
            releaseShift(kShiftTrailMs + 100);  // after the last detents
            shiftForGesture_ = false;
        }
        break;
    case Target::Kind::Pad: processor_.panelInput({Input::Kind::Pad, active_.index, 0, 0.0f}); break;
    default: break;
    }
    active_ = {};
    repaint();
}

// SHIFT from the panel (a click, the computer's Shift key) is one source; a MIDI
// CC can hold it too (HydraProcessor::holdButton combines them).
void PanelView::pressShift() { processor_.holdButton(kShiftButton, HydraProcessor::kFromPanel, true); }

void PanelView::releaseShift(int delayMs) { processor_.holdButton(kShiftButton, HydraProcessor::kFromPanel, false, delayMs); }

void PanelView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) {
    // A wheel notch (0.1-0.25 per notch, depending on the platform) is one detent.
    wheelAccumulator_ += wheel.deltaY * 8.0f;
    const int detents = static_cast<int>(wheelAccumulator_);
    wheelAccumulator_ -= static_cast<float>(detents);
    turn(targetAt(toCanvas(e.position)), detents);
}
