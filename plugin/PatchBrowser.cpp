#include "PatchBrowser.h"

#include "HydraProcessor.h"

namespace {
const juce::Colour kBackground(0xff15171a), kRow(0xff1e2126), kRowSelected(0xff3a4a6a), kText(0xffd8dce3), kDim(0xff6a6f78);
}  // namespace

PatchBrowser::PatchBrowser(HydraProcessor& processor) : processor_(processor) {
    current_.setText("Patches", juce::dontSendNotification);
    current_.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    current_.setColour(juce::Label::textColourId, kText);
    addAndMakeVisible(current_);
    for (int b = 0; b < Patches::kBanks; ++b) {
        juce::TextButton& button = bankButtons_[static_cast<std::size_t>(b)];
        button.setButtonText(juce::String::charToString(static_cast<juce::juce_wchar>('A' + b)));
        button.setClickingTogglesState(true);
        button.setRadioGroupId(1);
        button.setConnectedEdges((b > 0 ? juce::Button::ConnectedOnLeft : 0) |
                                 (b < Patches::kBanks - 1 ? juce::Button::ConnectedOnRight : 0));
        button.setColour(juce::TextButton::buttonOnColourId, kRowSelected);
        button.onClick = [this, b] { showBank(b); };
        addAndMakeVisible(button);
    }
    bankButtons_[0].setToggleState(true, juce::dontSendNotification);
    list_.setModel(this);
    list_.setRowHeight(22);
    list_.setColour(juce::ListBox::backgroundColourId, kBackground);
    list_.setColour(juce::ListBox::outlineColourId, juce::Colour(0xff2a2e35));
    list_.setOutlineThickness(1);
    addAndMakeVisible(list_);
    loadBank_.setTooltip("Send the patches of a .hydra bank (or a .patch) to the shown bank of the instrument");
    loadBank_.onClick = [this] { chooseBank(); };
    addAndMakeVisible(loadBank_);
    timerCallback();
    startTimerHz(10);
}

PatchBrowser::~PatchBrowser() = default;

void PatchBrowser::paint(juce::Graphics& g) { g.fillAll(kRow); }

void PatchBrowser::resized() {
    auto area = getLocalBounds().reduced(8, 6);
    current_.setBounds(area.removeFromTop(24));
    area.removeFromTop(4);
    auto banks = area.removeFromTop(26);
    // The bank tabs share the width evenly (the remainder spread over them).
    const int left = banks.getX(), total = banks.getWidth();
    for (int b = 0; b < Patches::kBanks; ++b) {
        const int x0 = left + total * b / Patches::kBanks, x1 = left + total * (b + 1) / Patches::kBanks;
        bankButtons_[static_cast<std::size_t>(b)].setBounds(x0, banks.getY(), x1 - x0, banks.getHeight());
    }
    area.removeFromTop(6);
    loadBank_.setBounds(area.removeFromBottom(26));
    area.removeFromBottom(6);
    list_.setBounds(area);
}

juce::String PatchBrowser::slotName(int bank, int slot) {
    return juce::String::charToString(static_cast<juce::juce_wchar>('A' + bank)) + juce::String(slot + 1).paddedLeft('0', 3);
}

void PatchBrowser::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) {
    const std::string& name = patches_.names[static_cast<std::size_t>(bank_ * Patches::kSlots + row)];
    g.fillAll(selected ? kRowSelected : (row % 2 ? kRow : kBackground));
    g.setFont(juce::FontOptions(14.0f));
    g.setColour(name.empty() ? kDim : kText);
    g.drawText(slotName(bank_, row), 8, 0, 44, height, juce::Justification::centredLeft);
    g.drawText(name.empty() ? juce::String("(empty)") : juce::String(name), 54, 0, width - 60, height,
               juce::Justification::centredLeft, true);
}

void PatchBrowser::selectedRowsChanged(int lastRowSelected) {
    if (updating_ || lastRowSelected < 0)
        return;
    const int index = bank_ * Patches::kSlots + lastRowSelected;
    if (patches_.names[static_cast<std::size_t>(index)].empty() || !live_) {
        markCurrent();  // a blank slot cannot be selected (it would load silence)
        return;
    }
    if (index != patches_.current)
        processor_.selectPatch(bank_, lastRowSelected);
}

void PatchBrowser::showBank(int bank) {
    bank_ = bank;
    bankButtons_[static_cast<std::size_t>(bank)].setToggleState(true, juce::dontSendNotification);
    list_.updateContent();
    markCurrent();
    list_.repaint();
}

// Selects the current patch's row (when its bank is shown) without selecting it again.
void PatchBrowser::markCurrent() {
    updating_ = true;
    if (patches_.current >= 0 && patches_.current / Patches::kSlots == bank_)
        list_.selectRow(patches_.current % Patches::kSlots);
    else
        list_.deselectAllRows();
    updating_ = false;
}

void PatchBrowser::timerCallback() {
    if (!processor_.readPatches(patches_, generation_))
        return;
    live_ = true;
    current_.setText(patches_.current < 0 ? juce::String("No patch loaded")
                                          : slotName(patches_.current / Patches::kSlots, patches_.current % Patches::kSlots) +
                                                "  " + patches_.currentName,
                     juce::dontSendNotification);
    if (patches_.current != lastCurrent_ && patches_.current >= 0) {
        lastCurrent_ = patches_.current;
        showBank(patches_.current / Patches::kSlots);  // follow patch changes made on the panel
        return;
    }
    list_.updateContent();
    markCurrent();
    list_.repaint();
}

void PatchBrowser::chooseBank() {
    chooser_ = std::make_unique<juce::FileChooser>("Bank (.hydra) or patch (.patch)", juce::File(), "*.hydra;*.patch");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this](const juce::FileChooser& chooser) {
                              const juce::File file = chooser.getResult();
                              if (!file.existsAsFile())
                                  return;
                              const int bank = bank_;
                              const juce::String bankName = juce::String::charToString(static_cast<juce::juce_wchar>('A' + bank));
                              juce::AlertWindow::showOkCancelBox(
                                  juce::MessageBoxIconType::QuestionIcon, "Load bank",
                                  "Send " + file.getFileName() + " to bank " + bankName + "? Patches in the same slots are overwritten.",
                                  "Load", "Cancel", this, juce::ModalCallbackFunction::create([this, file, bank](int result) {
                                      if (result != 1)
                                          return;
                                      const juce::String error = processor_.loadBank(file, bank);
                                      if (error.isNotEmpty())
                                          juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Load bank", error);
                                  }));
                          });
}
