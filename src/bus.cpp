#include "bus.h"

#include "host_arch.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace hydra {

Memory::Region& Memory::addRam(std::string name, std::uint32_t base, std::uint32_t size, std::uint8_t fill) {
    regions_.push_back({std::move(name), base, std::vector<std::uint8_t>(size, fill)});
    return regions_.back();
}

Memory::Region* Memory::regionFor(std::uint32_t addr, std::uint32_t length) {
    for (auto& region : regions_) {
        const std::uint64_t offset = std::uint64_t(addr) - region.base;
        if (region.direct && addr >= region.base && offset + length <= region.bytes.size())
            return &region;
    }
    return nullptr;
}

std::uint8_t* Memory::hostPtr(std::uint32_t addr, std::uint32_t length) {
    Region* region = regionFor(addr, length);
    return region ? region->bytes.data() + (addr - region->base) : nullptr;
}

void Memory::load(std::uint32_t addr, const void* data, std::size_t length) {
    std::uint8_t* dst = hostPtr(addr, static_cast<std::uint32_t>(length));
    if (!dst)
        throw std::runtime_error("Memory::load outside of RAM");
    std::memcpy(dst, data, length);
}

void AccessLog::record(std::string_view device, std::uint32_t addr, bool write, std::uint32_t value) {
    auto [it, inserted] = entries.try_emplace({addr, write});
    Entry& entry = it->second;
    if (inserted) {
        entry.device = device;
        entry.addr = addr;
        entry.write = write;
        entry.firstPc = currentPc;
    }
    ++entry.count;
    entry.lastValue = value;
}

void MmioBus::map(std::uint32_t base, std::uint32_t size, Device* device) {
    Mapping mapping{base, size, device};
    auto it = std::lower_bound(mappings_.begin(), mappings_.end(), base,
                               [](const Mapping& m, std::uint32_t b) { return m.base < b; });
    mappings_.insert(it, mapping);
    last_ = nullptr;
}

const MmioBus::Mapping* MmioBus::find(std::uint32_t addr) const {
    if (last_ && addr - last_->base < last_->size)
        return last_;
    auto it = std::upper_bound(mappings_.begin(), mappings_.end(), addr,
                               [](std::uint32_t a, const Mapping& m) { return a < m.base; });
    if (it == mappings_.begin())
        return nullptr;
    --it;
    if (addr - it->base >= it->size)
        return nullptr;
    last_ = &*it;
    return last_;
}

std::uint32_t MmioBus::read(std::uint32_t addr, int size) {
    if (const Mapping* m = find(addr)) {
        if (profiling) [[unlikely]] {
            const std::uint64_t start = hydra::cycleCounter();
            const std::uint32_t value = m->device->read(addr - m->base, size);
            DeviceStats& s = stats_[m->device];
            ++s.reads;
            s.cycles += hydra::cycleCounter() - start;
            return value;
        }
        const std::uint32_t value = m->device->read(addr - m->base, size);
        if (log_.all && dynamic_cast<StubDevice*>(m->device) == nullptr)
            log_.record(m->device->name(), addr, false, value);
        return value;
    }
    log_.record("unmapped", addr, false, 0);
    return 0;
}

void MmioBus::write(std::uint32_t addr, std::uint32_t value, int size) {
    if (const Mapping* m = find(addr)) {
        if (profiling) [[unlikely]] {
            const std::uint64_t start = hydra::cycleCounter();
            m->device->write(addr - m->base, value, size);
            DeviceStats& s = stats_[m->device];
            ++s.writes;
            s.cycles += hydra::cycleCounter() - start;
            return;
        }
        if (log_.all && dynamic_cast<StubDevice*>(m->device) == nullptr)
            log_.record(m->device->name(), addr, true, value);
        return m->device->write(addr - m->base, value, size);
    }
    log_.record("unmapped", addr, true, value);
}

std::uint32_t StubDevice::read(std::uint32_t offset, int /*size*/) {
    const auto it = regs_.find(offset);
    const std::uint32_t value = it == regs_.end() ? 0 : it->second;
    log_.record(name_, base_ + offset, false, value);
    return value;
}

void StubDevice::write(std::uint32_t offset, std::uint32_t value, int /*size*/) {
    regs_[offset] = value;
    log_.record(name_, base_ + offset, true, value);
}

}  // namespace hydra
