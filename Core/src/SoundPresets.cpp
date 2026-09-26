/**
 * @file SoundPresets.cpp
 * @brief The factory presets (SoundPresets.h): since 26.09.2026 the bank of PresetBank.cpp -- sixteen groups of
 *        sixty-four for each of nine synths -- in place of the recipe points of 24.09.2026, and the preset text itself.
 */
#include "phos/SoundPresets.h"

#include <cstdio>
#include <map>
#include <mutex>

namespace phos {

bool presetLeaves(Module m, int k)
{
    switch (m) {
    // 26.09.2026: and what ties kick and bass together -- the kick's tuning to the key (its end pitch is what the tuning
    // snaps: a preset's 40 Hz and 60 Hz landed in different octaves of the root and beat against the bass at 11 Hz),
    // its tail limit, the bass's lock to the kick, its retrigger and start phase.
    case Module::Kick: return k == kick::Level || k == kick::Tune || k == kick::PitchEnd || k == kick::TailLimit;
    case Module::Bass:
        return k == bass::Level || k == bass::DuckDepth || k == bass::DuckHold || k == bass::DuckRelease || k == bass::KickLock
            || k == bass::Retrigger || k == bass::StartPhase;
    case Module::Acid:
        return k == acid::Level || k == acid::Duck || k == acid::RoomSend || k == acid::HallSend || k == acid::PlateSend
            || k == acid::HallGate;
    case Module::Poly:
        // 26.09.2026: the sends to the rooms, the pan, the distance and the slow movement too -- where the mix guide
        // puts a voice, as the BerlinSchoolGenerator's presets leave them.
        return k == poly::Level || k == poly::Duck || k == poly::HpFloor || k == poly::HpTrack || k == poly::Gate
            || k == poly::GatePattern || k == poly::GateDepth || k == poly::GateDuty || k == poly::GateAttack
            || k == poly::GateRelease || k == poly::GateTone || k == poly::RoomSend || k == poly::HallSend
            || k == poly::PlateSend || k == poly::Pan || k == poly::Distance || k == poly::SlowMod || k == poly::HallGate;
    default: return false;
    }
}

namespace {

/** @brief Puts every knob of a module instance to its default. */
void moduleToDefaults(ParamStore& p, Module m, int instance)
{
    const int b = p.base(m, instance);
    for (int k = 0; k < ParamStore::moduleCount(m); ++k) p.set(b + k, p.defaultValue(b + k));
}

} // namespace

const std::vector<SoundPreset>& factoryPresets(Module module, int instance)
{
    static std::mutex lock;
    static std::map<int, std::vector<SoundPreset>> cache;
    static const std::vector<SoundPreset> none;
    int key = -1;
    if (module == Module::Kick) key = 0;
    else if (module == Module::Bass) key = 1;
    else if (module == Module::Acid) key = 2;
    else if (module == Module::Poly && instance >= 0 && instance < kPolyInstances) key = 3 + instance;
    if (key < 0) return none;
    const std::lock_guard<std::mutex> guard(lock);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    return cache.emplace(key, bankPresets(module, instance)).first->second;
}

bool applySoundPreset(ParamStore& p, Module module, int instance, const std::string& text)
{
    const int b = p.base(module, instance);
    const int n = ParamStore::moduleCount(module);
    std::vector<float> kept(static_cast<size_t>(n));
    for (int k = 0; k < n; ++k) kept[static_cast<size_t>(k)] = p.get(b + k);
    moduleToDefaults(p, module, instance);
    const bool ok = p.parseText(text);
    for (int k = 0; k < n; ++k) if (presetLeaves(module, k)) p.set(b + k, kept[static_cast<size_t>(k)]);
    return ok;
}

std::string moduleText(const ParamStore& p, Module module, int instance)
{
    const int b = p.base(module, instance);
    std::string s;
    for (int k = 0; k < ParamStore::moduleCount(module); ++k) {
        const int id = b + k;
        if (presetLeaves(module, k) || p.get(id) == p.defaultValue(id)) continue;
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(p.get(id)));
        s += p.key(id) + "=" + buf + "\n";
    }
    return s;
}

} // namespace phos
