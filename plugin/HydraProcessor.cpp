#include "HydraProcessor.h"

#include <cmath>
#include <cstdlib>

#include "HydraEditor.h"

namespace {
constexpr int kStateVersion = 1;

juce::PropertiesFile::Options settingsOptions() {
    juce::PropertiesFile::Options options;
    options.applicationName = "sinowi";
#if JUCE_MAC
    options.folderName = "sinowi";  // ~/Library/Application Support/sinowi
#else
    options.folderName = ".config/sinowi";  // on Linux relative to the home directory
#endif
    options.filenameSuffix = "settings";
    options.osxLibrarySubFolder = "Application Support";
    return options;
}

// System settings for a fresh instrument: program change reception on, so the
// host's program changes select patches, and Param RX = CC (0 off, 1 NRPN, 2 CC),
// so MIDI CCs (controllers, the host parameters) change the sound.
std::map<std::pair<int, int>, std::int16_t> initialSystemSettings() { return {{{5, 1}, 2}, {{5, 7}, 1}}; }
}  // namespace

HydraProcessor::HydraProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
    settings_.setStorageParameters(settingsOptions());
    for (std::size_t i = 0; i < ccmap::kEntries.size(); ++i) {
        const auto& entry = ccmap::kEntries[i];
        auto* parameter = new juce::AudioParameterInt(juce::ParameterID{"cc" + juce::String(entry.cc), 1},
                                                      juce::String(entry.name) + " (CC " + juce::String(entry.cc) + ")", 0, 127, 0);
        ccParameters_[i] = parameter;
        addParameter(parameter);
        parameter->addListener(this);
    }
    outputGain_ = new juce::AudioParameterFloat(juce::ParameterID{"outputGainDb", 1}, "Output Gain",
                                                juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f), 0.0f,
                                                juce::AudioParameterFloatAttributes().withLabel("dB"));
    addParameter(outputGain_);
    for (auto& cc : lastCc_)
        cc.store(-1, std::memory_order_relaxed);
}

// Panel buttons played from MIDI CCs (any channel): value >= 64 holds the button,
// < 64 lets it go, as a controller's momentary button sends 127 and 0. Only CCs
// the Hydrasynth's MIDI chart leaves free are used: of 0-127 that is just 2
// (breath) and 4 (foot); 6, 38 and 98-101 carry NRPNs and 121-127 are channel
// mode messages. These CCs are not passed to the instrument (a patch using one
// as a mod matrix source would not see it).
struct ButtonCc {
    int cc, button;
};
constexpr ButtonCc kButtonCcs[] = {
    {4, 62},  // SHIFT
    {2, 59},  // RANDOM
};

// The firmware takes some arpeggiator CCs as the parameter's own value (a list
// index, a percentage) and ignores values outside its range, so a controller's
// 0-127 sweep would mostly do nothing. Those CCs are scaled onto the range the
// firmware accepts (measured on firmware 2.2.0).
int HydraProcessor::scaleController(int number, int value) {
    struct Range {
        int cc, low, high;
    };
    static constexpr Range kRanges[] = {
        {106, 0, 11},   // Arp Division: 1/1 1/2 1/4 1/8 1/16 1/32, then the same as triplets
        {107, 5, 100},  // Arp Gate, %
        {108, 0, 7},    // Arp Mode: Up Down Up/Dn Up&Dn Order Rand Chord Phrase
        {109, 0, 32},   // Arp Ratchet
        {110, 0, 100},  // Arp Chance, %
        {120, 1, 4},    // Arp Octave
        {122, 0, 32},   // Arp Length (0 = Default)
    };
    for (const Range& r : kRanges)
        if (r.cc == number)
            return r.low + (value * (r.high - r.low) + 63) / 127;
    return value;
}

int HydraProcessor::updateButtonSources(int id, ButtonSource source, bool down) {
    if (id < 0 || id >= hydra::FrontPanel::kButtons)
        return 0;
    auto& sources = buttonSources_[static_cast<std::size_t>(id)];
    if (down)
        return sources.fetch_or(source, std::memory_order_acq_rel) == 0 ? 1 : 0;
    const std::uint8_t before = sources.fetch_and(static_cast<std::uint8_t>(~source), std::memory_order_acq_rel);
    return before == source ? -1 : 0;  // this source was the last one holding it
}

void HydraProcessor::holdButton(int id, ButtonSource source, bool down, int delayMs) {
    const int change = updateButtonSources(id, source, down);
    if (change != 0)
        panelInput({hydra::Emulator::PanelInput::Kind::Button, id, change > 0 ? 1 : 0, 0, delayMs});
}

bool HydraProcessor::buttonHeldBy(int id, std::uint8_t sources) const {
    return id >= 0 && id < hydra::FrontPanel::kButtons &&
           (buttonSources_[static_cast<std::size_t>(id)].load(std::memory_order_acquire) & sources) != 0;
}

bool HydraProcessor::buttonController(hydra::Emulator& emulator, int number, int value) {
    for (const ButtonCc& mapping : kButtonCcs)
        if (mapping.cc == number) {
            // On the audio thread: only a change of the button's state reaches
            // the emulator (a short queue append), repeats of the same value do not.
            const int change = updateButtonSources(mapping.button, kFromMidi, value >= 64);
            if (change != 0)
                emulator.panelInput({hydra::Emulator::PanelInput::Kind::Button, mapping.button, change > 0 ? 1 : 0, 0, 0});
            return true;
        }
    return false;
}

void HydraProcessor::noteController(int number, int value) {
    lastCc_[static_cast<std::size_t>(number & 127)].store(value, std::memory_order_relaxed);
    ccSerial_.fetch_add(1, std::memory_order_release);
}

bool HydraProcessor::readControllers(std::array<int, 128>& out, std::uint32_t& serial) const {
    const std::uint32_t now = ccSerial_.load(std::memory_order_acquire);
    if (now == serial)
        return false;
    serial = now;
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = lastCc_[i].load(std::memory_order_relaxed);
    return true;
}

HydraProcessor::~HydraProcessor() {
    *alive_ = false;
    stopTimer();
    const std::lock_guard life(lifeMutex_);
    std::unique_ptr<hydra::Emulator> emulator;
    {
        const juce::SpinLock::ScopedLockType lock(emulatorLock_);
        emulator = std::move(emulator_);
    }
}

bool HydraProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

juce::File HydraProcessor::defaultBankFile() const {
    const auto found = firmwareFile().getParentDirectory().findChildFiles(juce::File::findFiles, false, "*.hydra");
    return found.isEmpty() ? juce::File() : found.getFirst();
}

void HydraProcessor::timerCallback() {
    const std::lock_guard life(lifeMutex_);
    if (!emulator_ || autoBank_ == AutoBank::Off) {
        stopTimer();
        return;
    }
    if (emulator_->state() == hydra::Emulator::State::Failed) {
        autoBank_ = AutoBank::Off;
        stopTimer();
        return;
    }
    if (emulator_->state() != hydra::Emulator::State::Running || emulator_->uploading())
        return;
    if (autoBank_ == AutoBank::WaitForBoot) {
        const juce::File bank = defaultBankFile();
        if (!bank.existsAsFile() || loadBank(bank, 0).isNotEmpty()) {
            autoBank_ = AutoBank::Off;
            stopTimer();
            return;
        }
        autoBank_ = AutoBank::Uploading;
        return;
    }
    if (autoBank_ == AutoBank::Uploading) {
        // Upload finished: select A001, then give the firmware a moment to write
        // its settings before the flash is kept for later instruments.
        emulator_->selectPatch(0, 0);
        autoBank_ = AutoBank::Settle;
        settleUntil_ = juce::Time::getMillisecondCounter() + 3000;
        return;
    }
    if (juce::Time::getMillisecondCounter() < settleUntil_)
        return;
    const auto flash = emulator_->captureFlash();
    if (!flash.empty())
        saveDefaultFlash(flash);
    autoBank_ = AutoBank::Off;
    stopTimer();
}

juce::File HydraProcessor::defaultFlashFile() const { return firmwareFile().getParentDirectory().getChildFile("default-bank.flash"); }

juce::String HydraProcessor::defaultFlashKey() const {
    // By name, size and date (not the folder), so the kept flash survives the
    // folder being moved or copied with its dates.
    const auto describe = [](const juce::File& f) {
        return f.getFileName() + "|" + juce::String(f.getSize()) + "|" + juce::String(f.getLastModificationTime().toMilliseconds());
    };
    return "v2|" + describe(firmwareFile()) + "|" + describe(defaultBankFile());
}

bool HydraProcessor::loadDefaultFlash(std::vector<hydra::Machine::Config::FlashBlock>& out) const {
    const juce::File file = defaultFlashFile();
    if (!file.existsAsFile() || !defaultBankFile().existsAsFile())
        return false;
    juce::FileInputStream input(file);
    if (!input.openedOk())
        return false;
    juce::GZIPDecompressorInputStream gzip(input);
    const juce::ValueTree tree = juce::ValueTree::readFromStream(gzip);
    // Also the flash kept by Sydrahynth (Sinowi's former name) before its folder was
    // carried over: same files, under ".../sydrahynth/", keyed by full path.
    const auto legacyKey = [this] {
        const auto describe = [](const juce::File& f) {
            const juce::String path = f.getFullPathName().replace("Application Support/sinowi/", "Application Support/sydrahynth/");
            return path + "|" + juce::String(f.getSize()) + "|" + juce::String(f.getLastModificationTime().toMilliseconds());
        };
        return "v1|" + describe(firmwareFile()) + "|" + describe(defaultBankFile());
    };
    const juce::String key = tree.getProperty("key").toString();
    if ((!tree.hasType("SinowiDefaultFlash") && !tree.hasType("SydrahynthDefaultFlash")) ||
        (key != defaultFlashKey() && key != legacyKey()))
        return false;
    std::vector<hydra::Machine::Config::FlashBlock> flash;
    for (const auto& child : tree) {
        const juce::MemoryBlock* data = child.getProperty("data").getBinaryData();
        if (!data)
            return false;
        hydra::Machine::Config::FlashBlock block;
        block.offset = static_cast<std::uint32_t>(static_cast<juce::int64>(child.getProperty("offset")));
        const auto* bytes = static_cast<const std::uint8_t*>(data->getData());
        block.bytes.assign(bytes, bytes + data->getSize());
        flash.push_back(std::move(block));
    }
    if (flash.empty())
        return false;
    out = std::move(flash);
    return true;
}

void HydraProcessor::saveDefaultFlash(const std::vector<hydra::Machine::Config::FlashBlock>& flash) const {
    juce::ValueTree tree("SinowiDefaultFlash");
    tree.setProperty("key", defaultFlashKey(), nullptr);
    for (const auto& block : flash) {
        juce::ValueTree child("flash");
        child.setProperty("offset", static_cast<juce::int64>(block.offset), nullptr);
        child.setProperty("data", juce::var(juce::MemoryBlock(block.bytes.data(), block.bytes.size())), nullptr);
        tree.appendChild(child, nullptr);
    }
    // Written to a temporary file and moved into place, so a reader never sees half a file.
    const juce::File file = defaultFlashFile();
    juce::TemporaryFile temporary(file);
    {
        juce::FileOutputStream output(temporary.getFile());
        if (!output.openedOk())
            return;
        juce::GZIPCompressorOutputStream gzip(output, 6);
        tree.writeToStream(gzip);
    }
    temporary.overwriteTargetFileWithTemporary();
}

juce::File HydraProcessor::firmwareFile() const {
    auto& self = const_cast<HydraProcessor&>(*this);
    const juce::String stored = self.settings_.getUserSettings()->getValue("firmware");
    if (stored.isNotEmpty())
        return juce::File(stored);
    if (const char* fromEnvironment = std::getenv("HYDRA_FIRMWARE"))
        return juce::File(juce::String(fromEnvironment));
#if JUCE_MAC
    const juce::File folder = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                  .getChildFile("Application Support/sinowi");
#else
    const juce::File folder =
        juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".local/share/sinowi");
#endif
    const auto found = folder.findChildFiles(juce::File::findFiles, false, "*.dat");
    return found.isEmpty() ? juce::File() : found.getFirst();
}

void HydraProcessor::setFirmwareFile(const juce::File& file) {
    const std::lock_guard life(lifeMutex_);
    settings_.getUserSettings()->setValue("firmware", file.getFullPathName());
    settings_.saveIfNeeded();
    std::vector<hydra::Machine::Config::FlashBlock> flash;
    if (emulator_ && emulator_->state() == hydra::Emulator::State::Running)
        flash = emulator_->captureFlash();
    restartEmulator(flash.empty() ? pendingFlash_ : std::move(flash));
}

void HydraProcessor::restartEmulator(std::vector<hydra::Machine::Config::FlashBlock> flash) {
    const std::lock_guard life(lifeMutex_);
    // A fresh instrument: start from the kept flash with the default bank if
    // there is one, else upload the bank after booting (see timerCallback).
    if (flash.empty())
        loadDefaultFlash(flash);
    pendingFlash_ = flash;
    autoBank_ = flash.empty() ? AutoBank::WaitForBoot : AutoBank::Off;
    if (autoBank_ != AutoBank::Off) {
        juce::MessageManager::callAsync([this, alive = alive_] {
            if (*alive)
                startTimer(250);
        });
    }
    hydra::Emulator::Config config;
    config.firmwarePath = firmwareFile().getFullPathName().toStdString();
    config.flash = std::move(flash);
    config.systemSettings = initialSystemSettings();
    config.targetFill = targetFill();
    auto next = std::make_unique<hydra::Emulator>();
    next->start(config);  // without a firmware file it fails, with a status saying so
    std::unique_ptr<hydra::Emulator> previous;
    {
        const juce::SpinLock::ScopedLockType lock(emulatorLock_);
        previous = std::move(emulator_);
        emulator_ = std::move(next);
    }
    previous.reset();  // stops its threads, outside the lock
}

void HydraProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    ratio_ = hydra::Emulator::kSampleRate / sampleRate;
    for (auto& resampler : resamplers_)
        resampler.reset();
    const auto capacity = static_cast<std::size_t>(std::ceil(4 * samplesPerBlock * ratio_) + 64);
    for (auto& fifo : fifo_)
        fifo.assign(capacity, 0.0f);
    fifoCount_ = 0;
    scratchLeft_.assign(capacity, 0.0f);
    scratchRight_.assign(capacity, 0.0f);
    blockSize_ = samplesPerBlock;
    {
        const std::lock_guard life(lifeMutex_);
        if (!started_) {
            started_ = true;
            restartEmulator(pendingFlash_);
        }
    }
    applyLatency();
}

int HydraProcessor::latencyChoice() const {
    if (const char* forced = std::getenv("HYDRA_LATENCY"))  // for tests: 0-3
        return juce::jlimit(0, static_cast<int>(kLatencyFrames.size()) - 1, std::atoi(forced));
    auto& self = const_cast<HydraProcessor&>(*this);
    return juce::jlimit(0, static_cast<int>(kLatencyFrames.size()) - 1, self.settings_.getUserSettings()->getIntValue("latency", 1));
}

bool HydraProcessor::browserVisible() const {
    auto& self = const_cast<HydraProcessor&>(*this);
    return self.settings_.getUserSettings()->getBoolValue("browserVisible", true);
}

void HydraProcessor::setBrowserVisible(bool visible) {
    const std::lock_guard life(lifeMutex_);
    settings_.getUserSettings()->setValue("browserVisible", visible);
    settings_.saveIfNeeded();
}

int HydraProcessor::screenTint() const {
    auto& self = const_cast<HydraProcessor&>(*this);
    return self.settings_.getUserSettings()->getIntValue("screenTint", 0);
}

void HydraProcessor::setScreenTint(int tint) {
    const std::lock_guard life(lifeMutex_);
    settings_.getUserSettings()->setValue("screenTint", tint);
    settings_.saveIfNeeded();
}

void HydraProcessor::setLatencyChoice(int choice) {
    {
        const std::lock_guard life(lifeMutex_);
        settings_.getUserSettings()->setValue("latency", choice);
        settings_.saveIfNeeded();
    }
    applyLatency();
}

std::size_t HydraProcessor::targetFill() const {
    const int block = static_cast<int>(std::ceil(blockSize_ * ratio_));
    return static_cast<std::size_t>(std::max(kLatencyFrames[static_cast<std::size_t>(latencyChoice())], block + 128));
}

void HydraProcessor::applyLatency() {
    const std::size_t fill = targetFill();
    {
        const std::lock_guard life(lifeMutex_);
        if (emulator_)
            emulator_->setTargetFill(fill);
    }
    setLatencySamples(static_cast<int>(std::lround(static_cast<double>(fill) / ratio_)));
}

void HydraProcessor::parameterValueChanged(int index, float) {
    if (!restoringState_ && index >= 0 && static_cast<std::size_t>(index) < ccDirty_.size())
        ccDirty_[static_cast<std::size_t>(index)] = true;
}

void HydraProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    const juce::SpinLock::ScopedTryLockType lock(emulatorLock_);
    if (!lock.isLocked() || !emulator_)
        return;
    hydra::Emulator& emulator = *emulator_;

    for (const auto metadata : midi) {
        const juce::MidiMessage message = metadata.getMessage();
        if (message.isController()) {
            const int number = message.getControllerNumber();
            if (buttonController(emulator, number, message.getControllerValue()))
                continue;
            const int value = scaleController(number, message.getControllerValue());
            const std::uint8_t cc[3] = {message.getRawData()[0], static_cast<std::uint8_t>(number), static_cast<std::uint8_t>(value)};
            emulator.sendMidi(cc, 3);
            noteController(number, value);
        } else {
            emulator.sendMidi(message.getRawData(), static_cast<std::size_t>(message.getRawDataSize()));
        }
    }
    for (std::size_t i = 0; i < ccParameters_.size(); ++i)
        if (ccDirty_[i].exchange(false)) {
            const std::uint8_t cc[3] = {0xB0, ccmap::kEntries[i].cc,
                                        static_cast<std::uint8_t>(scaleController(ccmap::kEntries[i].cc, ccParameters_[i]->get()))};
            emulator.sendMidi(cc, 3);
            noteController(cc[1], cc[2]);
        }

    pullAudio(buffer, emulator);
    buffer.applyGain(juce::Decibels::decibelsToGain(outputGain_->get()));
}

void HydraProcessor::pullAudio(juce::AudioBuffer<float>& buffer, hydra::Emulator& emulator) {
    const int frames = buffer.getNumSamples();
    float* left = buffer.getWritePointer(0);
    float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : scratchRight_.data();
    if (std::abs(ratio_ - 1.0) < 1e-9) {  // 44.1 kHz host: no resampling
        emulator.readAudio(left, right, static_cast<std::size_t>(frames));
        return;
    }
    const int needed = static_cast<int>(std::ceil(frames * ratio_)) + 2;
    if (needed > static_cast<int>(fifo_[0].size()))
        return;  // a block far larger than announced: skip rather than allocate here
    if (fifoCount_ < needed) {
        emulator.readAudio(fifo_[0].data() + fifoCount_, fifo_[1].data() + fifoCount_,
                           static_cast<std::size_t>(needed - fifoCount_));
        fifoCount_ = needed;
    }
    const int used = resamplers_[0].process(ratio_, fifo_[0].data(), left, frames);
    resamplers_[1].process(ratio_, fifo_[1].data(), right, frames);
    const int remaining = std::max(0, fifoCount_ - used);
    for (auto& fifo : fifo_)
        std::memmove(fifo.data(), fifo.data() + used, static_cast<std::size_t>(remaining) * sizeof(float));
    fifoCount_ = remaining;
}

void HydraProcessor::getStateInformation(juce::MemoryBlock& destData) {
    juce::ValueTree state("Sinowi");
    state.setProperty("version", kStateVersion, nullptr);
    for (std::size_t i = 0; i < ccParameters_.size(); ++i)
        state.setProperty("cc" + juce::String(ccmap::kEntries[i].cc), ccParameters_[i]->get(), nullptr);
    state.setProperty("outputGainDb", outputGain_->get(), nullptr);

    const std::lock_guard life(lifeMutex_);
    std::vector<hydra::Machine::Config::FlashBlock> flash;
    if (emulator_ && emulator_->state() == hydra::Emulator::State::Running)
        flash = emulator_->captureFlash();
    if (flash.empty())
        flash = pendingFlash_;
    for (const auto& block : flash) {
        juce::MemoryOutputStream compressed;
        {
            juce::GZIPCompressorOutputStream gzip(compressed, 9);
            gzip.write(block.bytes.data(), block.bytes.size());
        }
        juce::ValueTree child("flash");
        child.setProperty("offset", static_cast<juce::int64>(block.offset), nullptr);
        child.setProperty("size", static_cast<juce::int64>(block.bytes.size()), nullptr);
        child.setProperty("data", compressed.getMemoryBlock().toBase64Encoding(), nullptr);
        state.appendChild(child, nullptr);
    }
    if (const auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void HydraProcessor::setStateInformation(const void* data, int sizeInBytes) {
    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if (!xml)
        return;
    const juce::ValueTree state = juce::ValueTree::fromXml(*xml);
    if (!state.hasType("Sinowi") && !state.hasType("Sydrahynth"))  // Sinowi's former name
        return;

    restoringState_ = true;
    for (std::size_t i = 0; i < ccParameters_.size(); ++i) {
        const juce::var value = state.getProperty("cc" + juce::String(ccmap::kEntries[i].cc));
        if (!value.isVoid())
            *ccParameters_[i] = static_cast<int>(value);
    }
    if (const juce::var gain = state.getProperty("outputGainDb"); !gain.isVoid())
        *outputGain_ = static_cast<float>(gain);
    restoringState_ = false;

    std::vector<hydra::Machine::Config::FlashBlock> flash;
    for (const auto& child : state) {
        if (!child.hasType("flash"))
            continue;
        juce::MemoryBlock compressed;
        if (!compressed.fromBase64Encoding(child.getProperty("data").toString()))
            continue;
        juce::MemoryInputStream input(compressed, false);
        juce::GZIPDecompressorInputStream gzip(input);
        hydra::Machine::Config::FlashBlock block;
        block.offset = static_cast<std::uint32_t>(static_cast<juce::int64>(child.getProperty("offset")));
        block.bytes.resize(static_cast<std::size_t>(static_cast<juce::int64>(child.getProperty("size"))));
        if (gzip.read(block.bytes.data(), static_cast<int>(block.bytes.size())) != static_cast<int>(block.bytes.size()))
            continue;
        flash.push_back(std::move(block));
    }
    const std::lock_guard life(lifeMutex_);
    if (started_)
        restartEmulator(std::move(flash));
    else
        pendingFlash_ = std::move(flash);
}

juce::String HydraProcessor::statusText() const {
    const std::lock_guard life(lifeMutex_);
    if (!emulator_)
        return "not started";
    const hydra::Emulator& emulator = *emulator_;
    if (emulator.uploading()) {
        const auto [done, total] = emulator.uploadProgress();
        return "loading patches: " + juce::String(static_cast<int>(done)) + " of " + juce::String(static_cast<int>(total));
    }
    return juce::String(emulator.status()) + " | latency " +
           juce::String(1000.0 * static_cast<double>(emulator.targetFill()) / hydra::Emulator::kSampleRate, 1) +
           " ms | underruns " + juce::String(static_cast<juce::int64>(emulator.underruns()));
}

juce::String HydraProcessor::loadBank(const juce::File& file, int bank) {
    std::vector<hydra::Patch> patches;
    try {
        patches = hydra::loadPatches(file.getFullPathName().toStdString());
    } catch (const std::exception& e) {
        return "Cannot read " + file.getFileName() + ": " + e.what();
    }
    if (patches.empty())
        return file.getFileName() + " contains no patches";
    const std::lock_guard life(lifeMutex_);
    if (!emulator_ || !emulator_->uploadPatches(std::move(patches), bank))
        return "The instrument is not ready (still starting, or already receiving patches)";
    return {};
}

bool HydraProcessor::readPatches(hydra::Emulator::Patches& out, std::uint64_t& generation) const {
    const std::lock_guard life(lifeMutex_);
    return emulator_ && emulator_->readPatches(out, generation);
}

void HydraProcessor::selectPatch(int bank, int slot) {
    const std::lock_guard life(lifeMutex_);
    if (emulator_)
        emulator_->selectPatch(bank, slot);
}

void HydraProcessor::panelInput(const hydra::Emulator::PanelInput& input) {
    const std::lock_guard life(lifeMutex_);
    if (emulator_)
        emulator_->panelInput(input);
}

bool HydraProcessor::readPanel(hydra::FrontPanel::Lights& out, std::uint64_t& generation) const {
    const std::lock_guard life(lifeMutex_);
    return emulator_ && emulator_->readPanel(out, generation);
}

juce::AudioProcessorEditor* HydraProcessor::createEditor() { return new HydraEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new HydraProcessor(); }
