/**
 * @file SoundPresets.cpp
 * @brief The factory presets (SoundPresets.h).
 */
#include "phos/SoundPresets.h"
#include "phos/Composer.h"
#include "phos/WaveTableFile.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>

namespace phos {

namespace {

/** @brief A character: a name and a point in a synth's five directions. */
struct Character { const char* name; float d[5]; };

// The voices' directions: brightness, softness, thickness, space, motion (Composer.h, kVoiceMacroNames).
constexpr Character kVoiceCharacters[] = {
    { "Plain",  { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } },   { "Bright", { 0.8f, 0.0f, 0.0f, 0.0f, 0.0f } },
    { "Dark",   { -0.8f, 0.0f, 0.0f, 0.0f, 0.0f } },  { "Soft",   { 0.0f, 0.8f, 0.0f, 0.0f, 0.0f } },
    { "Hard",   { 0.3f, -0.8f, 0.0f, 0.0f, 0.0f } },  { "Thick",  { 0.0f, 0.0f, 0.8f, 0.0f, 0.0f } },
    { "Wide",   { 0.0f, 0.0f, 0.3f, 0.8f, 0.0f } },   { "Moving", { 0.0f, 0.0f, 0.0f, 0.2f, 0.8f } },
};
// The kick's: length, punch, body, grit, click.
constexpr Character kKickCharacters[] = {
    { "Plain",  { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } },   { "Tight",  { -0.7f, 0.5f, 0.0f, 0.0f, 0.2f } },
    { "Punchy", { 0.0f, 0.8f, 0.2f, 0.0f, 0.0f } },   { "Deep",   { 0.6f, 0.0f, 0.7f, 0.0f, -0.3f } },
    { "Boomy",  { 0.8f, -0.2f, 0.6f, 0.0f, -0.5f } }, { "Hard",   { 0.0f, 0.5f, 0.0f, 0.8f, 0.2f } },
    { "Clicky", { -0.2f, 0.3f, 0.0f, 0.0f, 0.9f } },  { "Soft",   { 0.2f, -0.6f, 0.3f, -0.5f, -0.6f } },
};
// The bass's: brightness, pluck, squelch, grit, weight.
struct BassCharacter { const char* group; Character c; };
constexpr BassCharacter kBassCharacters[] = {
    { "Clean",  { "Clean sub",   { -0.3f, 0.0f, 0.0f, -0.5f, 0.3f } } },
    { "Clean",  { "Round",       { -0.5f, -0.3f, 0.0f, -0.3f, 0.5f } } },
    { "Clean",  { "Heavy",       { 0.0f, 0.0f, 0.0f, 0.0f, 0.9f } } },
    { "Short",  { "Plucky",      { 0.3f, 0.9f, 0.0f, 0.0f, 0.0f } } },
    { "Short",  { "Tight pluck", { 0.5f, 0.8f, 0.0f, 0.2f, -0.4f } } },
    { "Short",  { "Bright",      { 0.9f, 0.3f, 0.0f, 0.0f, 0.0f } } },
    { "Dirty",  { "Gritty",      { 0.3f, 0.0f, 0.0f, 0.9f, 0.0f } } },
    { "Dirty",  { "Rubbery",     { 0.2f, 0.2f, 0.9f, 0.0f, 0.0f } } },
    { "Dirty",  { "Squelch grit",{ 0.4f, 0.0f, 0.7f, 0.6f, 0.0f } } },
};
// The acid's voicings (clean, driven, liquid) as barycentric points, with the acid's own knobs on top.
struct AcidPoint { const char* group; const char* name; float w[3]; };
constexpr AcidPoint kAcidPoints[] = {
    { "Clean", "Clean", { 1.0f, 0.0f, 0.0f } },         { "Driven", "Driven", { 0.0f, 1.0f, 0.0f } },
    { "Liquid", "Liquid", { 0.0f, 0.0f, 1.0f } },       { "Blends", "Clean-driven", { 0.5f, 0.5f, 0.0f } },
    { "Blends", "Driven-liquid", { 0.0f, 0.5f, 0.5f } }, { "Blends", "Clean-liquid", { 0.5f, 0.0f, 0.5f } },
    { "Blends", "Middle", { 0.34f, 0.33f, 0.33f } },
};
struct AcidTweak { const char* name; float cutoff, resonance, env; };   // normalised steps
constexpr AcidTweak kAcidTweaks[] = { { "", 0.0f, 0.0f, 0.0f }, { " squelchy", 0.0f, 0.15f, 0.2f }, { " dark", -0.2f, 0.0f, -0.1f } };

} // namespace

bool presetLeaves(Module m, int k)
{
    switch (m) {
    case Module::Kick: return k == kick::Level;
    case Module::Bass: return k == bass::Level || k == bass::DuckDepth || k == bass::DuckHold || k == bass::DuckRelease;
    case Module::Acid: return k == acid::Level || k == acid::Duck;
    case Module::Poly:
        return k == poly::Level || k == poly::Duck || k == poly::HpFloor || k == poly::HpTrack || k == poly::Gate
            || k == poly::GatePattern || k == poly::GateDepth || k == poly::GateDuty || k == poly::GateAttack
            || k == poly::GateRelease || k == poly::GateTone;
    default: return false;
    }
}

namespace {

void moduleToDefaults(ParamStore& p, Module m, int instance)
{
    const int b = p.base(m, instance);
    for (int k = 0; k < ParamStore::moduleCount(m); ++k) p.set(b + k, p.defaultValue(b + k));
}

/** @brief Adds normalised offsets to the module's defaults. */
void applyOffsets(ParamStore& p, Module m, int instance, const float* off)
{
    const int b = p.base(m, instance);
    for (int k = 0; k < ParamStore::moduleCount(m); ++k) {
        if (off[k] == 0.0f) continue;
        const float n = std::clamp(p.toNormalised(b + k, p.defaultValue(b + k)) + off[k], 0.0f, 1.0f);
        p.setNormalised(b + k, n);
    }
}

std::vector<SoundPreset> buildVoice(int v)
{
    std::vector<SoundPreset> out;
    const PolyInstance inst = static_cast<PolyInstance>(v);
    ParamStore p;
    const int b = p.base(inst);
    const Composer::VoicePaletteView pal = Composer::voicePalette(inst);
    auto argmax = [](const double* w, int n, int from) { int best = from; for (int i = from; i < n; ++i) if (w[i] > w[best]) best = i; return best; };
    const int filter = argmax(pal.filter, static_cast<int>(PolyFilter::Count), 0);
    const int osc2 = argmax(pal.osc2, static_cast<int>(PolyOsc2::Count), 1);   // the palette's favourite partner (not "off")
    const int interval = argmax(pal.interval, static_cast<int>(PolyOsc2Interval::Count), 0);
    static const char* const kOscGroup[] = { "Supersaw", "VA", "FM", "Wavetable" };
    std::set<std::string> seen;
    auto make = [&](const std::string& group, const std::string& name, int osc, int table, const Character& c, bool withOsc2) {
        moduleToDefaults(p, Module::Poly, v);
        VoiceRecipe r;
        r.osc = osc;
        r.table = table;
        r.filter = filter;
        r.hasOsc2 = withOsc2;
        r.osc2 = withOsc2 ? osc2 : 0;
        r.osc2Semis = interval;
        for (int k = 0; k < kNumVoiceMacros; ++k) r.macro[k] = c.d[k];
        p.set(b + poly::Osc, static_cast<float>(osc));
        if (table >= 0) p.set(b + poly::Table, static_cast<float>(table));
        p.set(b + poly::FilterType, static_cast<float>(filter));
        if (withOsc2) { p.set(b + poly::Osc2, static_cast<float>(osc2)); p.set(b + poly::Osc2Interval, static_cast<float>(interval)); }
        float off[poly::Count] = {};
        Composer::voiceRecipeOffsets(inst, r, 1.0f, off);
        const int b2 = p.base(inst);
        for (int k = 0; k < poly::Count; ++k) {
            if (off[k] == 0.0f) continue;
            const float n = std::clamp(p.toNormalised(b2 + k, p.get(b2 + k)) + off[k], 0.0f, 1.0f);
            p.setNormalised(b2 + k, n);
        }
        // A direction a voice's loadings leave flat (the arp's and the stab's softness) would give a twin of
        // "Plain"; a list without twins is the honest one.
        std::string text = moduleText(p, Module::Poly, v);
        if (seen.insert(text).second) out.push_back({ group, name, std::move(text) });
    };
    // The palette's analogue and FM oscillators: every character each, with and without the second oscillator.
    for (int o = 0; o < 3; ++o) {
        if (pal.osc[o] <= 0.0) continue;
        for (const Character& c : kVoiceCharacters) make(kOscGroup[o], std::string(kOscGroup[o]) + " " + c.name, o, -1, c, false);
        make(kOscGroup[o], std::string(kOscGroup[o]) + " layered", o, -1, kVoiceCharacters[0], true);
    }
    // The wavetables: the palette's built-in ones, then every library table of its lanes, grouped by the pack's
    // families (the folder of the table's file), each with one character in turn.
    int turn = 0;
    const ParamDesc& td = p.desc(b + poly::Table);
    std::set<int> done;   // a table in two lanes is one preset
    for (int k = 0; k < 6 && pal.builtin[k] >= 0; ++k) {
        const int t = pal.builtin[k];
        if (!done.insert(t).second) continue;
        const Character& c = kVoiceCharacters[static_cast<size_t>(turn++ % 8)];
        make("Wavetables: built-in", std::string(td.choices[t]) + " " + c.name, 3, t, c, false);
    }
    for (int l = 0; l < 3 && pal.lane[l] >= 0; ++l) {
        for (int t : waveTableLaneTables(static_cast<WaveTableLane>(pal.lane[l]))) {
            const int li = t - kNumBuiltinWaveTables;
            if (li < 0 || li >= kNumLibraryWaveTables || !done.insert(t).second) continue;
            const std::string id = kLibraryTables[li].id;
            const std::string family = id.substr(0, id.find('/'));
            const Character& c = kVoiceCharacters[static_cast<size_t>(turn++ % 8)];
            make("Wavetables: " + family, std::string(kLibraryTables[li].name) + " " + c.name, 3, t, c, false);
        }
    }
    return out;
}

std::vector<SoundPreset> buildKick()
{
    std::vector<SoundPreset> out;
    ParamStore p;
    static const char* const kEngines[] = { "Sweep", "Resonant" };
    for (int e = 0; e < 2; ++e)
        for (const Character& c : kKickCharacters) {
            moduleToDefaults(p, Module::Kick, 0);
            p.set(p.base(Module::Kick) + kick::Engine, static_cast<float>(e));
            float off[kick::Count] = {};
            Composer::recipeOffsets(true, c.d, 1.0f, off);
            applyOffsets(p, Module::Kick, 0, off);
            out.push_back({ kEngines[e], std::string(kEngines[e]) + " " + c.name, moduleText(p, Module::Kick) });
        }
    return out;
}

std::vector<SoundPreset> buildBass()
{
    std::vector<SoundPreset> out;
    ParamStore p;
    for (const BassCharacter& bc : kBassCharacters) {
        moduleToDefaults(p, Module::Bass, 0);
        float off[bass::Count] = {};
        Composer::recipeOffsets(false, bc.c.d, 1.0f, off);
        applyOffsets(p, Module::Bass, 0, off);
        out.push_back({ bc.group, bc.c.name, moduleText(p, Module::Bass) });
    }
    return out;
}

std::vector<SoundPreset> buildAcid()
{
    std::vector<SoundPreset> out;
    ParamStore p;
    const int b = p.base(Module::Acid);
    for (const AcidPoint& a : kAcidPoints)
        for (const AcidTweak& t : kAcidTweaks) {
            moduleToDefaults(p, Module::Acid, 0);
            float off[acid::Count] = {};
            int disperse = -1;
            Composer::acidVoicingOffsets(p, a.w, 1.0f, off, disperse);
            off[acid::Cutoff] += t.cutoff;
            off[acid::Resonance] += t.resonance;
            off[acid::EnvAmount] += t.env;
            applyOffsets(p, Module::Acid, 0, off);
            if (disperse >= 0) p.set(b + acid::Disperse, static_cast<float>(disperse));
            out.push_back({ a.group, std::string(a.name) + t.name, moduleText(p, Module::Acid) });
        }
    return out;
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
    std::vector<SoundPreset> built = key == 0 ? buildKick() : key == 1 ? buildBass() : key == 2 ? buildAcid() : buildVoice(key - 3);
    return cache.emplace(key, std::move(built)).first->second;
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
