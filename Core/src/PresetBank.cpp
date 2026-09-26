/**
 * @file PresetBank.cpp
 * @brief The factory preset bank (26.09.2026; SoundPresets.h, bankPresets): sixteen groups of sixty-four presets for
 *        each of nine synths, built from the table Tools/presets/gen_bank.py writes (PresetBankData.inl).
 *
 * The user: "mindestens 1024 (sinnvolle!) Presets pro Synth ... mit ordentlichem Gebrauch der neuen
 * Modulationsfähigkeiten", named "cool ... angelehnt an die Preset-Namen im BerlinSchoolGenerator oder im AmbientSynth
 * ... sinnvoll untergliedert". The scheme is the BerlinSchoolGenerator's: a group is a submenu and an eight by eight
 * grid, its rows the synth's adjectives from dark to bright, its columns the group's nouns, so a name is two words and
 * says where on the grid the sound lies. A group's knobs each have a range and an axis -- the row's ('A'), the
 * column's ('B'), a seeded draw ('R') or a constant ('C') -- and are set in the knob's normalised space, so a
 * logarithmic range (a cutoff, a decay) spreads its eight steps evenly to the ear. A group also names the filter
 * models that suit it (one is drawn) and its modulation recipes (each with a chance; '@' becomes the next free LFO,
 * '#' the next free slot of the matrix, a source of -1 that LFO). The same index is always the same sound.
 */
#include "phos/SoundPresets.h"
#include "phos/Dsp.h"
#include "phos/Modulation.h"
#include "phos/WaveTable.h"
#include "phos/WaveTableFile.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace phos {

namespace bank {

/** @brief One knob of a group: its key inside the module, its range, its axis; a table's spec in place of a range. */
struct Knob {
    const char* key;     ///< e.g. "cutoff", "lfo@_rate", "mx#_amount"
    float lo, hi;        ///< the range ('C': lo)
    char axis;           ///< 'A' the row, 'B' the column, 'R' drawn, 'C' constant
    const char* table;   ///< for "table": "lane:<WaveTableLane>" or "list:<index>,<index>..."; else null
};
/** @brief A filter model a group allows, with the range of its mode. */
struct Filter { int model; float modeLo, modeHi; };
/** @brief A modulation recipe: its chance and its knobs. */
struct Mod { float chance; std::vector<Knob> knobs; };
/** @brief A group: its name, its eight nouns, its knobs, its filters and its modulation recipes. */
struct Group {
    const char* name;
    const char* nouns[8];
    float style[5];   ///< the group's weight in each style (StyleId order), for the composer's choice
    std::vector<Knob> knobs;
    std::vector<Filter> filters;
    std::vector<Mod> mods;
};
/** @brief A synth: its label, module and instance, its eight adjectives and its sixteen groups. */
struct Synth {
    const char* label;
    Module module;
    int instance;
    const char* adj[8];
    std::vector<Group> groups;
};

#include "PresetBankData.inl"
#include "PresetTrims.inl"

namespace {

/** @brief Replaces every @p from in @p s with @p to. */
std::string replaceAll(std::string s, char from, int to)
{
    std::string out;
    for (char c : s) {
        if (c == from) out += std::to_string(to);
        else out += c;
    }
    return out;
}

/** @brief The tables a "table" spec names (a library lane's or a list of indices). */
std::vector<int> tablesOf(const char* spec)
{
    std::vector<int> out;
    const std::string s(spec != nullptr ? spec : "");
    if (s.rfind("lane:", 0) == 0) {
        const int lane = std::atoi(s.c_str() + 5);
        if (lane >= 0 && lane < kNumWaveTableLanes) out = waveTableLaneTables(static_cast<WaveTableLane>(lane));
    } else if (s.rfind("list:", 0) == 0) {
        size_t i = 5;
        while (i < s.size()) {
            out.push_back(std::atoi(s.c_str() + i));
            const size_t comma = s.find(',', i);
            if (comma == std::string::npos) break;
            i = comma + 1;
        }
    }
    if (out.empty()) out.push_back(0);
    return out;
}

/** @brief Sets knob @p key of the synth to the value its range and axis give at row @p row, column @p col. */
bool setKnob(ParamStore& p, const Synth& s, const std::string& key, float lo, float hi, char axis, int row, int col, Rng& rng)
{
    const std::string full = (s.module == Module::Poly ? std::string(kPolyInstanceNames[s.instance]) : std::string(s.label)) + "." + key;
    const int id = p.find(full);
    if (id < 0) return false;
    float t = 0.0f;
    switch (axis) {
    case 'A': t = static_cast<float>(row) / 7.0f; break;
    case 'B': t = static_cast<float>(col) / 7.0f; break;
    case 'R': t = rng.uniform(); break;
    default: t = 0.0f; break;
    }
    const ParamDesc& d = p.desc(id);
    if (d.curve == Curve::Choice || d.curve == Curve::Int || d.curve == Curve::Toggle) {
        p.set(id, std::round(lo + t * (hi - lo)));
    } else if (axis == 'C' || lo == hi) {
        p.set(id, lo);
    } else {
        const float a = p.toNormalised(id, lo), b = p.toNormalised(id, hi);
        p.setNormalised(id, a + t * (b - a));
    }
    return true;
}

} // namespace

/** @brief The presets of one synth of the bank. */
std::vector<SoundPreset> build(const Synth& s)
{
    std::vector<SoundPreset> out;
    ParamStore p;
    const int base = p.base(s.module, s.instance);
    const int count = ParamStore::moduleCount(s.module);
    uint64_t salt = 0x50524553'45544231ull;   // "PRESETB1"
    for (const char* c = s.label; *c != 0; ++c) salt = mixSeed(salt, static_cast<uint64_t>(*c));
    out.reserve(s.groups.size() * 64);
    for (size_t g = 0; g < s.groups.size(); ++g) {
        const Group& grp = s.groups[g];
        for (int row = 0; row < 8; ++row)
            for (int col = 0; col < 8; ++col) {
                for (int k = 0; k < count; ++k) p.set(base + k, p.defaultValue(base + k));
                Rng rng;
                rng.seed(mixSeed(salt, static_cast<uint64_t>(g * 64 + row * 8 + col)));
                for (const Knob& k : grp.knobs) {
                    if (k.table != nullptr) {
                        const std::vector<int> t = tablesOf(k.table);
                        setKnob(p, s, "table", static_cast<float>(t[static_cast<size_t>(rng.below(static_cast<int>(t.size())))]), 0.0f, 'C', row, col, rng);
                        continue;
                    }
                    setKnob(p, s, k.key, k.lo, k.hi, k.axis, row, col, rng);
                }
                if (!grp.filters.empty()) {
                    const Filter& f = grp.filters[static_cast<size_t>(rng.below(static_cast<int>(grp.filters.size())))];
                    setKnob(p, s, "filter_model", static_cast<float>(f.model), 0.0f, 'C', row, col, rng);
                    setKnob(p, s, "filter_mode", f.modeLo, f.modeHi, 'R', row, col, rng);
                }
                int lfoNext = 1, slotNext = 1;
                for (const Mod& m : grp.mods) {
                    const float roll = rng.uniform();
                    if (roll >= m.chance || slotNext > kModSlots) continue;
                    bool usesLfo = false;
                    for (const Knob& k : m.knobs) usesLfo = usesLfo || std::string(k.key).find('@') != std::string::npos;
                    if (usesLfo && lfoNext > kLfos) continue;
                    for (const Knob& k : m.knobs) {
                        const std::string key = replaceAll(replaceAll(k.key, '@', lfoNext), '#', slotNext);
                        // A source of -1 is the LFO this recipe took (ModSource: LFO n is n).
                        const bool theLfo = key.find("_src") != std::string::npos && k.lo < 0.0f;
                        if (theLfo) setKnob(p, s, key, static_cast<float>(lfoNext), 0.0f, 'C', row, col, rng);
                        else setKnob(p, s, key, k.lo, k.hi, k.axis, row, col, rng);
                    }
                    if (usesLfo) ++lfoNext;
                    ++slotNext;
                }
                SoundPreset sp;
                sp.group = grp.name;
                sp.name = std::string(s.adj[row]) + " " + grp.nouns[col];
                sp.text = moduleText(p, s.module, s.instance);
                sp.groupIndex = static_cast<int>(g);
                for (int k = 0; k < 5; ++k) sp.style[k] = grp.style[k];
                const int synth = s.module == Module::Kick ? 0 : s.module == Module::Bass ? 1 : s.module == Module::Acid ? 2 : 3 + s.instance;
                if (out.size() < 1024) sp.trimDb = 0.1f * static_cast<float>(kPresetTrimTenths[synth][out.size()]);
                for (int k = 0; k < count; ++k)
                    if (!presetLeaves(s.module, k)) sp.values.emplace_back(k, p.get(base + k));
                out.push_back(std::move(sp));
            }
    }
    return out;
}

} // namespace bank

int bankUnknownKeys(std::string* first)
{
    ParamStore p;
    int bad = 0;
    for (const bank::Synth& s : bank::synths()) {
        const std::string prefix = s.module == Module::Poly ? std::string(kPolyInstanceNames[s.instance]) : std::string(s.label);
        auto check = [&](const std::string& key) {
            if (p.find(prefix + "." + key) >= 0) return;
            if (bad++ == 0 && first != nullptr) *first = prefix + "." + key;
        };
        for (const bank::Group& g : s.groups) {
            for (const bank::Knob& k : g.knobs) check(k.key);
            if (!g.filters.empty()) { check("filter_model"); check("filter_mode"); }
            for (const bank::Mod& m : g.mods)
                for (const bank::Knob& k : m.knobs) {
                    std::string key = k.key;
                    for (char& c : key) if (c == '@' || c == '#') c = '1';
                    check(key);
                }
        }
    }
    return bad;
}

std::vector<SoundPreset> bankPresets(Module module, int instance)
{
    for (const bank::Synth& s : bank::synths())
        if (s.module == module && (module != Module::Poly || s.instance == instance)) return bank::build(s);
    return {};
}

} // namespace phos
