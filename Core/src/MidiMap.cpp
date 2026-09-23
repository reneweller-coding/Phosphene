/**
 * @file MidiMap.cpp
 * @brief MIDI controllers on any parameter (MidiMap.h).
 */
#include "phos/MidiMap.h"

#include <cstdio>
#include <sstream>

namespace phos {

MidiMap::MidiMap()
{
    for (auto& t : table_) t.store(0, std::memory_order_relaxed);
}

void MidiMap::bindLocked(int channel, int cc, int target)
{
    // One knob, one parameter: the target lets go of any other controller, the controller of any other target.
    for (auto& t : table_) if (t.load(std::memory_order_relaxed) == target + 1) t.store(0, std::memory_order_relaxed);
    table_[channel * kControllers + cc].store(target + 1, std::memory_order_release);
    revision_.fetch_add(1, std::memory_order_acq_rel);
}

int MidiMap::handleCc(int channel, int cc, int value, float& normalised)
{
    if (channel < 0 || channel >= kChannels || cc < 0 || cc >= kControllers) return -1;
    normalised = static_cast<float>(value < 0 ? 0 : (value > 127 ? 127 : value)) / 127.0f;
    const int armed = armed_.load(std::memory_order_acquire);
    if (armed >= 0) {
        int expected = armed;
        // Only the first controller after arming learns; a second message in the same block finds it disarmed.
        if (armed_.compare_exchange_strong(expected, -1, std::memory_order_acq_rel)) {
            bindLocked(channel, cc, armed);
            return armed;
        }
    }
    return table_[channel * kControllers + cc].load(std::memory_order_acquire) - 1;
}

void MidiMap::bind(int channel, int cc, int target)
{
    if (channel < 0 || channel >= kChannels || cc < 0 || cc >= kControllers || target < 0) return;
    bindLocked(channel, cc, target);
}

void MidiMap::unbind(int target)
{
    for (auto& t : table_) if (t.load(std::memory_order_relaxed) == target + 1) t.store(0, std::memory_order_release);
    revision_.fetch_add(1, std::memory_order_acq_rel);
}

void MidiMap::clear()
{
    for (auto& t : table_) t.store(0, std::memory_order_release);
    armed_.store(-1, std::memory_order_release);
    revision_.fetch_add(1, std::memory_order_acq_rel);
}

bool MidiMap::controllerOf(int target, int& channel, int& cc) const
{
    for (int i = 0; i < kChannels * kControllers; ++i) {
        if (table_[i].load(std::memory_order_acquire) == target + 1) { channel = i / kControllers; cc = i % kControllers; return true; }
    }
    return false;
}

std::vector<MidiBinding> MidiMap::bindings() const
{
    std::vector<MidiBinding> out;
    for (int i = 0; i < kChannels * kControllers; ++i) {
        const int t = table_[i].load(std::memory_order_acquire) - 1;
        if (t >= 0) out.push_back({ i / kControllers, i % kControllers, t });
    }
    return out;
}

std::string MidiMap::toText(const std::function<std::string(int)>& name) const
{
    std::string s;
    for (const MidiBinding& b : bindings()) {
        char head[32];
        std::snprintf(head, sizeof(head), "cc %d %d ", b.channel + 1, b.cc);
        s += head + name(b.target) + "\n";
    }
    return s;
}

int MidiMap::fromText(const std::string& text, const std::function<int(const std::string&)>& find)
{
    for (auto& t : table_) t.store(0, std::memory_order_relaxed);
    int n = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string word, key;
        int channel = 0, cc = -1;
        if (!(ls >> word >> channel >> cc >> key) || word != "cc") continue;
        const int target = find(key);
        if (target < 0 || channel < 1 || channel > kChannels || cc < 0 || cc >= kControllers) continue;
        bindLocked(channel - 1, cc, target);
        ++n;
    }
    revision_.fetch_add(1, std::memory_order_acq_rel);
    return n;
}

} // namespace phos
