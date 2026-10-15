/**
 * @file SoundPresets.cpp
 * @brief The factory presets (SoundPresets.h): since 26.09.2026 the bank of PresetBank.cpp -- sixteen groups of
 *        sixty-four for each of nine synths -- in place of the recipe points of 24.09.2026, and the preset text itself.
 * @author Rene Weller
 */
#include "phos/SoundPresets.h"

#include <cstdio>
#include <map>
#include <mutex>

namespace phos {

bool presetLeaves(ModuleE m, int k)
{
    switch (m) {
    // 26.09.2026: and what ties kick and bass together -- the kick's tuning to the key (its end pitch is what the tuning
    // snaps: a preset's 40 Hz and 60 Hz landed in different octaves of the root and beat against the bass at 11 Hz),
    // its tail limit, the bass's lock to the kick, its retrigger and start phase.
    case ModuleE::KICK: return k == kick::LEVEL || k == kick::TUNE || k == kick::PITCH_END || k == kick::TAIL_LIMIT;
    case ModuleE::BASS:
        return k == bass::LEVEL || k == bass::DUCK_DEPTH || k == bass::DUCK_HOLD || k == bass::DUCK_RELEASE || k == bass::KICK_LOCK
            || k == bass::RETRIGGER || k == bass::START_PHASE;
    case ModuleE::ACID:
        return k == acid::LEVEL || k == acid::DUCK || k == acid::ROOM_SEND || k == acid::HALL_SEND || k == acid::PLATE_SEND
            || k == acid::HALL_GATE;
    case ModuleE::POLY:
        // 26.09.2026: the sends to the rooms, the pan, the distance and the slow movement too -- where the mix guide
        // puts a voice, as the BerlinSchoolGenerator's presets leave them.
        return k == poly::LEVEL || k == poly::DUCK || k == poly::HP_FLOOR || k == poly::HP_TRACK || k == poly::GATE
            || k == poly::GATE_PATTERN || k == poly::GATE_DEPTH || k == poly::GATE_DUTY || k == poly::GATE_ATTACK
            || k == poly::GATE_RELEASE || k == poly::GATE_TONE || k == poly::ROOM_SEND || k == poly::HALL_SEND
            || k == poly::PLATE_SEND || k == poly::PAN || k == poly::DISTANCE || k == poly::SLOW_MOD || k == poly::HALL_GATE;
    default: return false;
    }
}

namespace {

/**
 * @brief Puts every knob of a module instance to its default.
 * @param[in,out] p         the parameters
 * @param[in]     m         the module
 * @param[in]     instance  its instance
 */
void moduleToDefaults(ParamStore& p, ModuleE m, int instance)
{
    const int b = p.base(m, instance);
    for (int k = 0; k < ParamStore::moduleCount(m); ++k) p.set(b + k, p.defaultValue(b + k));
}

} // namespace

const std::vector<SoundPreset>& factoryPresets(ModuleE module, int instance)
{
    static std::mutex lock;
    static std::map<int, std::vector<SoundPreset>> cache;
    static const std::vector<SoundPreset> none;
    int key = -1;
    if (module == ModuleE::KICK) key = 0;
    else if (module == ModuleE::BASS) key = 1;
    else if (module == ModuleE::ACID) key = 2;
    else if (module == ModuleE::POLY && instance >= 0 && instance < PolyInstances) key = 3 + instance;
    if (key < 0) return none;
    const std::lock_guard<std::mutex> guard(lock);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    return cache.emplace(key, bankPresets(module, instance)).first->second;
}

bool applySoundPreset(ParamStore& p, ModuleE module, int instance, const std::string& text)
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

std::string moduleText(const ParamStore& p, ModuleE module, int instance)
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
