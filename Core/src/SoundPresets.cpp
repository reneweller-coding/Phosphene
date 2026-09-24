/**
 * @file SoundPresets.cpp
 * @brief The factory presets (SoundPresets.h).
 *
 * 24.09.2026, the user: "Koennten wir den Presets etwas coolere Namen geben und noch deutlich mehr Presets
 * erzeugen?" Until then a synth had eight hand-placed characters ("Plain", "Bright", "Dark", ...) and the names
 * said what the knobs did ("Supersaw Bright", "Sweep Punchy"). Now:
 *
 * - **More points.** A synth's characters are points in its five directions, spread evenly by a Halton sequence
 *   (bases 2, 3, 5, 7, 11: a low-discrepancy set, so fifty points cover the space without the clumps and holes of
 *   fifty random draws), after the neutral point and the ten poles (each direction alone, either way). The same
 *   recipe code the composer uses turns each point into knob values, so every preset is still a sound the
 *   generator could have played on that voice.
 * - **Names from the sound.** A preset is named by its strongest direction -- an adjective for its pole ("Razor"
 *   for a hard attack, "Nebula" for a wide one, "Obsidian" for a dark one) -- and a noun of the synth's role ("Comet"
 *   for a lead, "Aurora" for a pad, "Anvil" for a kick). The name is a function of the point and its place in the
 *   list, so it is the same in every build, and no two presets of a synth share one.
 * - **Groups by character.** The kick, the bass and the acid are listed by their strongest direction ("Deep &
 *   Heavy", "Squelchy", "Molten"), the voices by oscillator and wavetable family as before.
 */
#include "phos/SoundPresets.h"
#include "phos/Composer.h"
#include "phos/WaveTableFile.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>

namespace phos {

namespace {

/** @brief A point in a synth's five directions. */
struct Point { float d[5]; };

/** @brief The radical inverse of @p i in base @p b: the Halton sequence's i-th coordinate, in [0, 1). */
double halton(int i, int b)
{
    double f = 1.0, r = 0.0;
    for (int n = i; n > 0; n /= b) {
        f /= b;
        r += f * (n % b);
    }
    return r;
}

/**
 * @brief @p n character points: the neutral one, the ten poles at 0.8, then the Halton sequence over [-0.85, 0.85]^5.
 */
std::vector<Point> characterPoints(int n)
{
    std::vector<Point> out;
    out.push_back(Point{ { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } });
    for (int k = 0; k < 5 && static_cast<int>(out.size()) < n; ++k)
        for (float s : { 0.8f, -0.8f }) {
            Point p{ { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } };
            p.d[k] = s;
            out.push_back(p);
        }
    static const int kBases[5] = { 2, 3, 5, 7, 11 };
    for (int i = 1; static_cast<int>(out.size()) < n; ++i) {
        Point p{};
        for (int k = 0; k < 5; ++k) p.d[k] = static_cast<float>(1.7 * (halton(i, kBases[k]) - 0.5));
        out.push_back(p);
    }
    out.resize(static_cast<size_t>(n));
    return out;
}

/** @brief The words a synth's names are made of: per direction and pole, adjectives; per role, nouns. */
struct Vocabulary {
    const char* const* adj[5][2];   ///< [direction][0 = positive pole, 1 = negative], each null-terminated
    const char* family[5][2];       ///< the group a point whose strongest direction is that pole is listed in
    const char* const* nouns;       ///< null-terminated
};

/** @brief The strongest direction of @p p and its pole (0 positive, 1 negative); -1 for a point near neutral. */
int strongest(const Point& p, int& pole)
{
    int k = -1;
    float best = 0.22f;
    for (int i = 0; i < 5; ++i)
        if (std::fabs(p.d[i]) > best) { best = std::fabs(p.d[i]); k = i; }
    pole = k >= 0 && p.d[k] < 0.0f ? 1 : 0;
    return k;
}

int count(const char* const* words) { int n = 0; while (words[n] != nullptr) ++n; return n; }

/**
 * @brief A name for point @p p, the @p index-th of its synth: the pole's adjective and a role noun, the first
 *        combination not in @p taken (which it joins). The index turns the words, so neighbours in a list differ.
 */
std::string nameFor(const Point& p, int index, const Vocabulary& v, std::set<std::string>& taken, const char* const* nouns = nullptr)
{
    static const char* const kNeutral[] = { "Classic", "Pure", "Prime", "True", "Core", nullptr };
    int pole = 0;
    const int k = strongest(p, pole);
    const char* const* adj = k < 0 ? kNeutral : v.adj[k][pole];
    const char* const* nn = nouns != nullptr ? nouns : v.nouns;
    const int na = count(adj), nn_ = count(nn);
    // Walk the combinations from a start that depends on the index (a multiplicative hash: 2654435761 is Knuth's).
    const uint32_t h = static_cast<uint32_t>(index + 1) * 2654435761u;
    for (int t = 0; t < na * nn_; ++t) {
        const int a = static_cast<int>((h >> 7) + static_cast<uint32_t>(t / nn_)) % na;
        const int b = static_cast<int>((h >> 17) + static_cast<uint32_t>(t)) % nn_;
        std::string name = std::string(adj[a]) + " " + nn[b];
        if (taken.insert(name).second) return name;
    }
    // Every combination used: a numbered one.
    for (int i = 2;; ++i) {
        std::string name = std::string(adj[0]) + " " + nn[0] + " " + std::to_string(i);
        if (taken.insert(name).second) return name;
    }
}

/** @brief The group of point @p p: its strongest pole's family, or "Classic". */
std::string familyOf(const Point& p, const Vocabulary& v)
{
    int pole = 0;
    const int k = strongest(p, pole);
    return k < 0 ? std::string("Classic") : std::string(v.family[k][pole]);
}

// ------------------------------------------------------------------ the words

// The voices' directions: brightness, softness, thickness, space, motion (Composer.h, kVoiceMacroNames).
const char* const kBrightUp[] = { "Solar", "Neon", "Crystal", "Radiant", "Prism", "Laser", "Golden", "Starlit", nullptr };
const char* const kBrightDown[] = { "Obsidian", "Shadow", "Midnight", "Umbra", "Smoky", "Ember", "Nocturnal", nullptr };
const char* const kSoftUp[] = { "Velvet", "Silken", "Misty", "Dreamy", "Feather", "Hazy", "Tender", nullptr };
const char* const kSoftDown[] = { "Razor", "Steel", "Serrated", "Blade", "Venom", "Jagged", "Iron", nullptr };
const char* const kThickUp[] = { "Titan", "Colossal", "Monolith", "Massive", "Thunder", "Mammoth", nullptr };
const char* const kThickDown[] = { "Glass", "Needle", "Silver", "Slender", "Wire", "Filament", nullptr };
const char* const kSpaceUp[] = { "Cosmic", "Nebula", "Astral", "Cathedral", "Endless", "Orbital", "Galactic", nullptr };
const char* const kSpaceDown[] = { "Dry", "Close", "Direct", "Naked", "Compact", "Focused", nullptr };
const char* const kMotionUp[] = { "Serpent", "Vortex", "Spiral", "Morphing", "Liquid", "Fractal", "Shifting", nullptr };
const char* const kMotionDown[] = { "Still", "Frozen", "Static", "Steady", "Stone", "Fixed", nullptr };

const char* const kLeadNouns[] = { "Comet", "Dragon", "Phoenix", "Siren", "Beam", "Flare", "Scream", "Oracle", "Hawk", "Lightning", nullptr };
const char* const kCounterNouns[] = { "Echo", "Mirror", "Reply", "Ghost", "Twin", "Whisper", "Shadow Call", "Answer", nullptr };
const char* const kArpNouns[] = { "Cascade", "Sparks", "Runner", "Circuit", "Rain", "Ripples", "Firefly", "Clockwork", "Swarm", nullptr };
const char* const kStabNouns[] = { "Strike", "Blast", "Punch", "Shard", "Impact", "Spear", "Hit", "Bolt", nullptr };
const char* const kPadNouns[] = { "Aurora", "Veil", "Horizon", "Choir", "Ocean", "Dawn", "Cloud", "Halo", "Mirage", "Tide", nullptr };
const char* const kDroneNouns[] = { "Mantra", "Om", "Temple", "Abyss", "Earth", "Monk", "Cavern", "Bourdon", nullptr };

constexpr int kVoiceNouns = kPolyInstances;
const char* const* const kNounsOf[kVoiceNouns] = { kLeadNouns, kCounterNouns, kArpNouns, kStabNouns, kPadNouns, kDroneNouns };

Vocabulary voiceVocabulary(int v)
{
    Vocabulary w{};
    w.adj[0][0] = kBrightUp;  w.adj[0][1] = kBrightDown;
    w.adj[1][0] = kSoftUp;    w.adj[1][1] = kSoftDown;
    w.adj[2][0] = kThickUp;   w.adj[2][1] = kThickDown;
    w.adj[3][0] = kSpaceUp;   w.adj[3][1] = kSpaceDown;
    w.adj[4][0] = kMotionUp;  w.adj[4][1] = kMotionDown;
    w.nouns = kNounsOf[std::clamp(v, 0, kVoiceNouns - 1)];
    return w;
}

// The kick's: length, punch, body, grit, click.
const char* const kLongUp[] = { "Rolling", "Booming", "Endless", "Swelling", "Long", nullptr };
const char* const kLongDown[] = { "Tight", "Snappy", "Short", "Clipped", "Staccato", nullptr };
const char* const kPunchUp[] = { "Slamming", "Knockout", "Punchy", "Brutal", "Pounding", nullptr };
const char* const kPunchDown[] = { "Gentle", "Round", "Soft", "Cushioned", "Rounded", nullptr };
const char* const kBodyUp[] = { "Deep", "Fat", "Heavy", "Tectonic", "Subsonic", nullptr };
const char* const kBodyDown[] = { "Lean", "Dry", "Wiry", "Hollow", "Slim", nullptr };
const char* const kGritUp[] = { "Crushed", "Dirty", "Distorted", "Molten", "Scorched", "Rusty", nullptr };
const char* const kGritDown[] = { "Clean", "Polished", "Pure", "Smooth", "Chrome", nullptr };
const char* const kClickUp[] = { "Clicky", "Laser", "Piercing", "Needle", "Sharp", nullptr };
const char* const kClickDown[] = { "Muffled", "Warm", "Woolly", "Mellow", "Buried", nullptr };
const char* const kKickNouns[] = { "Thump", "Hammer", "Stomp", "Quake", "Piston", "Heart", "Anvil", "Engine", "Boot", "Drum", "Titan", "Pulse", nullptr };
/** @brief The resonant engine's nouns: a struck, tuned body -- so its presets say which engine they are. */
const char* const kResonantNouns[] = { "Gong", "Bell", "Membrane", "Resonator", "Chamber", "Tom", "Timpani", "Ring", "Cauldron", nullptr };

Vocabulary kickVocabulary()
{
    Vocabulary w{};
    w.adj[0][0] = kLongUp;  w.adj[0][1] = kLongDown;
    w.adj[1][0] = kPunchUp; w.adj[1][1] = kPunchDown;
    w.adj[2][0] = kBodyUp;  w.adj[2][1] = kBodyDown;
    w.adj[3][0] = kGritUp;  w.adj[3][1] = kGritDown;
    w.adj[4][0] = kClickUp; w.adj[4][1] = kClickDown;
    w.family[0][0] = "Long & Rolling"; w.family[0][1] = "Tight & Short";
    w.family[1][0] = "Punchy";         w.family[1][1] = "Soft & Round";
    w.family[2][0] = "Deep & Heavy";   w.family[2][1] = "Lean";
    w.family[3][0] = "Dirty & Hard";   w.family[3][1] = "Clean";
    w.family[4][0] = "Clicky";         w.family[4][1] = "Warm & Muffled";
    w.nouns = kKickNouns;
    return w;
}

// The bass's: brightness, pluck, squelch, grit, weight.
const char* const kBassBrightUp[] = { "Buzzing", "Bright", "Electric", "Neon", "Sizzling", nullptr };
const char* const kBassBrightDown[] = { "Dark", "Round", "Deep", "Murky", "Shadow", nullptr };
const char* const kPluckUp[] = { "Plucky", "Snappy", "Bouncing", "Staccato", "Springy", nullptr };
const char* const kPluckDown[] = { "Legato", "Sustained", "Gliding", "Flowing", "Long", nullptr };
const char* const kSquelchUp[] = { "Rubbery", "Squelchy", "Wobbly", "Elastic", "Gooey", nullptr };
const char* const kSquelchDown[] = { "Straight", "Firm", "Solid", "Rigid", "Plain", nullptr };
const char* const kBassGritUp[] = { "Growling", "Gritty", "Snarling", "Filthy", "Rough", nullptr };
const char* const kBassGritDown[] = { "Clean", "Smooth", "Glassy", "Silky", "Pure", nullptr };
const char* const kWeightUp[] = { "Massive", "Subby", "Heavy", "Tectonic", "Earthquake", nullptr };
const char* const kWeightDown[] = { "Light", "Lean", "Nimble", "Airy", "Agile", nullptr };
const char* const kBassNouns[] = { "Roller", "Rumble", "Gallop", "Motor", "Groove", "Driver", "Serpent", "Machine", "Runner", "Pulse", nullptr };

Vocabulary bassVocabulary()
{
    Vocabulary w{};
    w.adj[0][0] = kBassBrightUp; w.adj[0][1] = kBassBrightDown;
    w.adj[1][0] = kPluckUp;      w.adj[1][1] = kPluckDown;
    w.adj[2][0] = kSquelchUp;    w.adj[2][1] = kSquelchDown;
    w.adj[3][0] = kBassGritUp;   w.adj[3][1] = kBassGritDown;
    w.adj[4][0] = kWeightUp;     w.adj[4][1] = kWeightDown;
    w.family[0][0] = "Bright";   w.family[0][1] = "Dark & Round";
    w.family[1][0] = "Plucky";   w.family[1][1] = "Long";
    w.family[2][0] = "Squelchy"; w.family[2][1] = "Straight";
    w.family[3][0] = "Dirty";    w.family[3][1] = "Clean";
    w.family[4][0] = "Heavy";    w.family[4][1] = "Light";
    w.nouns = kBassNouns;
    return w;
}

// The acid's voicings (clean, driven, liquid) as barycentric points, each with a turn of its own knobs.
const char* const kCleanAdj[] = { "Crystal", "Glassy", "Pure", "Clear", nullptr };
const char* const kDrivenAdj[] = { "Burning", "Scorched", "Fuzzed", "Screaming", nullptr };
const char* const kLiquidAdj[] = { "Molten", "Liquid", "Flowing", "Dripping", nullptr };
const char* const kBlendAdj[] = { "Twisted", "Hybrid", "Mutant", "Shifting", "Chimera", nullptr };
struct AcidTweak { const char* const* nouns; float cutoff, resonance, env; };   // normalised steps
const char* const kPlainNouns[] = { "Line", "Worm", "Snake", "Acid", nullptr };
const char* const kSquelchNouns[] = { "Squelch", "Gurgle", "Slime", "Goo", nullptr };
const char* const kDarkNouns[] = { "Swamp", "Cave", "Tar", "Bog", nullptr };
const char* const kBrightNouns[] = { "Laser", "Spark", "Needle", "Wire", nullptr };
const char* const kScreamNouns[] = { "Siren", "Howl", "Banshee", "Shriek", nullptr };
const char* const kMellowNouns[] = { "Bubble", "Drop", "Ripple", "Tide", nullptr };
const AcidTweak kAcidTweaks[] = {
    { kPlainNouns, 0.0f, 0.0f, 0.0f },     { kSquelchNouns, 0.0f, 0.15f, 0.2f }, { kDarkNouns, -0.2f, 0.0f, -0.1f },
    { kBrightNouns, 0.15f, 0.0f, 0.05f },  { kScreamNouns, 0.1f, 0.25f, 0.1f },  { kMellowNouns, -0.1f, -0.1f, -0.15f },
};

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

constexpr int kVoicePoints = 28;       ///< characters per oscillator of a voice
constexpr int kVoiceLayered = 10;      ///< of them again with the palette's second oscillator
constexpr int kKickPoints = 56;        ///< characters per kick engine
constexpr int kBassPoints = 90;        ///< bass characters

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
    const Vocabulary vocab = voiceVocabulary(v);
    std::set<std::string> seen, names;
    int index = 0;
    auto make = [&](const std::string& group, const std::string& name, int osc, int table, const Point& c, bool withOsc2) {
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
        for (int k = 0; k < poly::Count; ++k) {
            if (off[k] == 0.0f) continue;
            const float n = std::clamp(p.toNormalised(b + k, p.get(b + k)) + off[k], 0.0f, 1.0f);
            p.setNormalised(b + k, n);
        }
        // A direction a voice's loadings leave flat (the arp's and the stab's softness) would give a twin of
        // another point; a list without twins is the honest one.
        std::string text = moduleText(p, Module::Poly, v);
        if (seen.insert(text).second) out.push_back({ group, name, std::move(text) });
    };
    // The palette's analogue and FM oscillators: every character each, and some of them with the second oscillator.
    const std::vector<Point> points = characterPoints(kVoicePoints);
    for (int o = 0; o < 3; ++o) {
        if (pal.osc[o] <= 0.0) continue;
        for (const Point& c : points) make(kOscGroup[o], nameFor(c, index++, vocab, names), o, -1, c, false);
        for (int i = 0; i < kVoiceLayered; ++i) {
            const Point& c = points[static_cast<size_t>((3 * i + o) % kVoicePoints)];
            make(std::string(kOscGroup[o]) + " layered", nameFor(c, index++, vocab, names), o, -1, c, true);
        }
    }
    // The wavetables: the palette's built-in ones, then every library table of its lanes, grouped by the pack's
    // families (the folder of the table's file); each table with two characters in turn, named "<pole> <table>".
    const std::vector<Point> turns = characterPoints(64);
    int turn = 1;   // the neutral point is the table as it stands, which the palette's own draw plays anyway
    const ParamDesc& td = p.desc(b + poly::Table);
    std::set<int> done;   // a table in two lanes is one preset
    auto twoOf = [&](const std::string& group, const std::string& table, int t) {
        for (int k = 0; k < 2; ++k) {
            const Point& c = turns[static_cast<size_t>(1 + (turn++ % 63))];
            const char* const nouns[] = { table.c_str(), nullptr };
            make(group, nameFor(c, index++, vocab, names, nouns), 3, t, c, false);
        }
    };
    for (int k = 0; k < 6 && pal.builtin[k] >= 0; ++k) {
        const int t = pal.builtin[k];
        if (!done.insert(t).second) continue;
        twoOf("Wavetables: built-in", td.choices[t], t);
    }
    for (int l = 0; l < 3 && pal.lane[l] >= 0; ++l) {
        for (int t : waveTableLaneTables(static_cast<WaveTableLane>(pal.lane[l]))) {
            const int li = t - kNumBuiltinWaveTables;
            if (li < 0 || li >= kNumLibraryWaveTables || !done.insert(t).second) continue;
            const std::string id = kLibraryTables[li].id;
            twoOf("Wavetables: " + id.substr(0, id.find('/')), kLibraryTables[li].name, t);
        }
    }
    return out;
}

std::vector<SoundPreset> buildKick()
{
    std::vector<SoundPreset> out;
    ParamStore p;
    const Vocabulary vocab = kickVocabulary();
    std::set<std::string> names, texts;
    int index = 0;
    const std::vector<Point> points = characterPoints(kKickPoints);
    for (int e = 0; e < 2; ++e)
        for (const Point& c : points) {
            moduleToDefaults(p, Module::Kick, 0);
            p.set(p.base(Module::Kick) + kick::Engine, static_cast<float>(e));
            float off[kick::Count] = {};
            Composer::recipeOffsets(true, c.d, 1.0f, off);
            applyOffsets(p, Module::Kick, 0, off);
            std::string text = moduleText(p, Module::Kick);
            // Both engines in the character's group; the resonant one's names are its own nouns.
            if (!texts.insert(text).second) continue;
            out.push_back({ familyOf(c, vocab), nameFor(c, index++, vocab, names, e == 1 ? kResonantNouns : nullptr), std::move(text) });
        }
    return out;
}

std::vector<SoundPreset> buildBass()
{
    std::vector<SoundPreset> out;
    ParamStore p;
    const Vocabulary vocab = bassVocabulary();
    std::set<std::string> names, texts;
    int index = 0;
    for (const Point& c : characterPoints(kBassPoints)) {
        moduleToDefaults(p, Module::Bass, 0);
        float off[bass::Count] = {};
        Composer::recipeOffsets(false, c.d, 1.0f, off);
        applyOffsets(p, Module::Bass, 0, off);
        std::string text = moduleText(p, Module::Bass);
        if (!texts.insert(text).second) continue;
        out.push_back({ familyOf(c, vocab), nameFor(c, index++, vocab, names), std::move(text) });
    }
    return out;
}

std::vector<SoundPreset> buildAcid()
{
    std::vector<SoundPreset> out;
    ParamStore p;
    const int b = p.base(Module::Acid);
    std::set<std::string> names, texts;
    int index = 0;
    // The voicing triangle in quarters: its three corners, the points on its edges and inside, fifteen in all.
    for (int i = 0; i <= 4; ++i)
        for (int j = 0; i + j <= 4; ++j) {
            const float w[3] = { static_cast<float>(i) / 4.0f, static_cast<float>(j) / 4.0f, static_cast<float>(4 - i - j) / 4.0f };
            const int top = w[0] >= w[1] && w[0] >= w[2] ? 0 : (w[1] >= w[2] ? 1 : 2);
            const bool corner = w[top] >= 0.75f;
            static const char* const kGroups[] = { "Clean", "Driven", "Liquid" };
            const std::string group = corner ? kGroups[top] : "Blends";
            const char* const* adj = !corner ? kBlendAdj : (top == 0 ? kCleanAdj : (top == 1 ? kDrivenAdj : kLiquidAdj));
            for (const AcidTweak& t : kAcidTweaks) {
                moduleToDefaults(p, Module::Acid, 0);
                float off[acid::Count] = {};
                int disperse = -1;
                Composer::acidVoicingOffsets(p, w, 1.0f, off, disperse);
                off[acid::Cutoff] += t.cutoff;
                off[acid::Resonance] += t.resonance;
                off[acid::EnvAmount] += t.env;
                applyOffsets(p, Module::Acid, 0, off);
                if (disperse >= 0) p.set(b + acid::Disperse, static_cast<float>(disperse));
                std::string text = moduleText(p, Module::Acid);
                if (!texts.insert(text).second) continue;
                Vocabulary v{};
                for (int k = 0; k < 5; ++k) v.adj[k][0] = v.adj[k][1] = adj;
                const Point strong{ { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f } };
                out.push_back({ group, nameFor(strong, index++, v, names, t.nouns), std::move(text) });
            }
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
