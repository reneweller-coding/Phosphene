/**
 * @file Params.cpp
 * @brief Module descriptor tables and the parameter store.
 */
#include "phos/Params.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace phos {

const char* const kKeyNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
const char* const kScaleNames[] = { "Aeolian", "Phrygian", "Harmonic Minor", "Phrygian Dominant", "Double Harmonic", "Dorian" };
const char* const kKickPatternNames[] = { "Four", "Four + Fills", "Off" };
const char* const kBassPatternNames[] = { "Rolling", "Gallop", "Skip", "Offbeat", "Triplet" };

namespace {

const char* const kKickEngineNames[] = { "Sweep", "Resonant" };
const char* const kKickTuneNames[] = { "Free", "Key" };
const char* const kKickClipNames[] = { "Tanh", "Hard" };
const char* const kSubModeNames[] = { "Mixed", "Split" };
const char* const kKickLockNames[] = { "Off", "Bass follows kick", "Kick follows bass" };

const ParamDesc kComposeParams[compose::Count] = {
    { "bpm",             "Tempo",           "BPM", 100.0f, 190.0f, 145.0f, Curve::Linear },
    { "key",             "Key",             "",      0.0f,  11.0f,   6.0f, Curve::Choice, kKeyNames },
    { "scale",           "Scale",           "",      0.0f,   5.0f,   1.0f, Curve::Choice, kScaleNames },
    { "kick_pattern",    "Kick Pattern",    "",      0.0f,   2.0f,   1.0f, Curve::Choice, kKickPatternNames },
    { "bass_pattern",    "Bass Pattern",    "",      0.0f,   4.0f,   0.0f, Curve::Choice, kBassPatternNames },
    { "bass_gate",       "Bass Gate",       "",      0.2f,   1.0f,   0.7f, Curve::Linear },
    { "bass_variation",  "Bass Variation",  "",      0.0f,   1.0f,   0.4f, Curve::Linear },
    { "bass_register",   "Bass Register",   "st",  -12.0f,  12.0f,   0.0f, Curve::Int },
    { "track_bars",      "Track Length",    "bars", 32.0f, 512.0f, 256.0f, Curve::Int },
    { "track_variation", "Track Variation", "",      0.0f,   1.0f,   0.5f, Curve::Linear },
    { "sound_variation", "Sound Variation", "",      0.0f,   1.0f,   0.5f, Curve::Linear },
    { "tempo_range",     "Tempo Range",     "BPM",   0.0f,  10.0f,   3.0f, Curve::Linear },
    { "level_match",     "Level Match",     "",      0.0f,   1.0f,   1.0f, Curve::Toggle },
};

const ParamDesc kKickParams[kick::Count] = {
    { "engine",      "Engine",       "",     0.0f,     1.0f,    0.0f, Curve::Choice, kKickEngineNames },
    { "tune",        "Tune",         "",     0.0f,     1.0f,    1.0f, Curve::Choice, kKickTuneNames },
    { "pitch_end",   "Pitch End",    "Hz",  30.0f,   120.0f,   50.0f, Curve::Log },
    { "pitch_start", "Pitch Start",  "Hz",  60.0f,  1500.0f,  330.0f, Curve::Log },
    { "pitch_decay", "Body Decay",   "ms",   5.0f,   150.0f,   22.0f, Curve::Log },
    { "punch_decay", "Punch Decay",  "ms",   0.5f,    20.0f,    4.0f, Curve::Log },
    { "punch",       "Punch",        "",     0.0f,     1.0f,    0.5f, Curve::Linear },
    { "amp_attack",  "Attack",       "ms",   0.0f,    10.0f,    0.2f, Curve::Linear },
    { "amp_hold",    "Hold",         "ms",   0.0f,   150.0f,   12.0f, Curve::Linear },
    { "amp_decay",   "Decay",        "ms",  20.0f,  1500.0f,  150.0f, Curve::Log },
    { "drive",       "Drive",        "",     0.0f,     1.0f,   0.35f, Curve::Linear },
    { "clip",        "Clip",         "",     0.0f,     1.0f,    0.0f, Curve::Choice, kKickClipNames },
    { "click_level", "Click",        "",     0.0f,     1.0f,    0.2f, Curve::Linear },
    { "click_tone",  "Click Tone",   "Hz", 500.0f, 12000.0f, 4000.0f, Curve::Log },
    { "click_decay", "Click Decay",  "ms",   0.5f,    30.0f,    3.0f, Curve::Log },
    { "tone",        "Tone",         "Hz", 200.0f, 20000.0f, 9000.0f, Curve::Log },
    { "level",       "Level",        "dB", -36.0f,     6.0f,   -2.0f, Curve::Linear },
    { "tail_limit",  "Tail Limit",   "dB", -60.0f,     0.0f,  -24.0f, Curve::Linear },
};

const ParamDesc kBassParams[bass::Count] = {
    { "wave",          "Wave",          "",      0.0f,     1.0f,  0.15f, Curve::Linear },
    { "pulse_width",   "Pulse Width",   "",     0.05f,    0.95f,   0.5f, Curve::Linear },
    { "sub",           "Sub",           "",      0.0f,     1.0f,   0.6f, Curve::Linear },
    { "sub_mode",      "Sub Mode",      "",      0.0f,     1.0f,   1.0f, Curve::Choice, kSubModeNames },
    { "split_ratio",   "Split",         "x f0",  1.2f,     3.0f,   2.0f, Curve::Linear },
    { "kick_lock",     "Kick Lock",     "",      0.0f,     2.0f,   2.0f, Curve::Choice, kKickLockNames },
    { "retrigger",     "Retrigger",     "",      0.0f,     1.0f,   1.0f, Curve::Toggle },
    { "start_phase",   "Start Phase",   "",      0.0f,     1.0f,   0.5f, Curve::Linear },
    { "cutoff",        "Cutoff",        "Hz",   20.0f, 10000.0f, 140.0f, Curve::Log },
    { "resonance",     "Resonance",     "",      0.0f,     1.0f,   0.3f, Curve::Linear },
    { "env_amount",    "Env Amount",    "oct",   0.0f,     8.0f,   4.0f, Curve::Linear },
    { "filter_decay",  "Filter Decay",  "ms",    3.0f,  1000.0f,  75.0f, Curve::Log },
    { "key_track",     "Key Track",     "",      0.0f,     1.0f,   0.6f, Curve::Linear },
    { "vel_to_cutoff", "Vel > Cutoff",  "",      0.0f,     1.0f,  0.25f, Curve::Linear },
    { "drive",         "Drive",         "",      0.0f,     1.0f,   0.3f, Curve::Linear },
    { "amp_attack",    "Attack",        "ms",    0.1f,    50.0f,   0.8f, Curve::Log },
    { "amp_decay",     "Decay",         "ms",    5.0f,  2000.0f, 180.0f, Curve::Log },
    { "amp_sustain",   "Sustain",       "",      0.0f,     1.0f,  0.55f, Curve::Linear },
    { "amp_release",   "Release",       "ms",    1.0f,   500.0f,  10.0f, Curve::Log },
    { "duck_depth",    "Duck Depth",    "",      0.0f,     1.0f,   0.5f, Curve::Linear },
    { "duck_hold",     "Duck Hold",     "ms",    0.0f,   200.0f,  25.0f, Curve::Linear },
    { "duck_release",  "Duck Release",  "ms",    5.0f,   500.0f,  60.0f, Curve::Log },
    { "level",         "Level",         "dB",  -36.0f,     6.0f,  -5.0f, Curve::Linear },
};

const ParamDesc kMixParams[mix::Count] = {
    { "kick_mute", "Kick Mute", "", 0.0f, 1.0f, 0.0f, Curve::Toggle },
    { "bass_mute", "Bass Mute", "", 0.0f, 1.0f, 0.0f, Curve::Toggle },
    { "track_gain", "Track Gain", "dB", -12.0f, 12.0f, 0.0f, Curve::Linear },
};

const ParamDesc kMasterParams[master::Count] = {
    { "gain",    "Gain",    "dB", -24.0f, 12.0f,  0.0f, Curve::Linear },
    { "ceiling", "Ceiling", "dB", -12.0f,  0.0f, -0.3f, Curve::Linear },
    { "clip",    "Clip",    "",     0.0f,  1.0f,  1.0f, Curve::Toggle },
};

/** @brief One module: its prefix, table and how many instances exist. */
struct ModuleSpec {
    const char* prefix;
    const ParamDesc* descs;
    int count;
    int instances;
};

const ModuleSpec kModules[static_cast<int>(Module::Count)] = {
    { "compose", kComposeParams, compose::Count, 1 },
    { "kick",    kKickParams,    kick::Count,    1 },
    { "bass",    kBassParams,    bass::Count,    1 },
    { "mix",     kMixParams,     mix::Count,     1 },
    { "master",  kMasterParams,  master::Count,  1 },
};

bool isDiscrete(Curve c) { return c == Curve::Int || c == Curve::Choice || c == Curve::Toggle; }

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

} // namespace

ParamStore::ParamStore()
{
    for (auto& row : bases_) for (int& b : row) b = -1;
    int total = 0;
    for (const ModuleSpec& m : kModules) total += m.count * m.instances;
    entries_.reserve(static_cast<size_t>(total));
    for (int mi = 0; mi < static_cast<int>(Module::Count); ++mi) {
        const ModuleSpec& m = kModules[mi];
        for (int inst = 0; inst < m.instances && inst < kMaxInstances; ++inst) {
            bases_[mi][inst] = static_cast<int>(entries_.size());
            std::string prefix = m.prefix;
            if (m.instances > 1) prefix += std::to_string(inst + 1);
            for (int p = 0; p < m.count; ++p) {
                Entry e{ &m.descs[p], prefix + "." + m.descs[p].key, static_cast<Module>(mi), inst };
                index_.emplace(e.key, static_cast<int>(entries_.size()));
                entries_.push_back(std::move(e));
            }
        }
    }
    values_ = std::make_unique<std::atomic<float>[]>(entries_.size());
    resetDefaults();
}

int ParamStore::base(Module m, int instance) const
{
    const int mi = static_cast<int>(m);
    if (mi < 0 || mi >= static_cast<int>(Module::Count) || instance < 0 || instance >= kMaxInstances) return -1;
    return bases_[mi][instance];
}

int ParamStore::find(std::string_view key) const
{
    const auto it = index_.find(std::string(key));
    return it == index_.end() ? -1 : it->second;
}

int ParamStore::getInt(int id) const
{
    return static_cast<int>(std::lround(get(id)));
}

void ParamStore::set(int id, float value)
{
    if (id < 0 || id >= count()) return;
    const ParamDesc& d = desc(id);
    if (!(value == value)) value = d.defValue;   // NaN
    float v = value < d.minValue ? d.minValue : (value > d.maxValue ? d.maxValue : value);
    if (isDiscrete(d.curve)) v = std::round(v);
    values_[static_cast<size_t>(id)].store(v, std::memory_order_relaxed);
}

float ParamStore::toNormalised(int id, float value) const
{
    const ParamDesc& d = desc(id);
    if (d.maxValue <= d.minValue) return 0.0f;
    float n;
    if (d.curve == Curve::Log) n = std::log(value / d.minValue) / std::log(d.maxValue / d.minValue);
    else n = (value - d.minValue) / (d.maxValue - d.minValue);
    return n < 0.0f ? 0.0f : (n > 1.0f ? 1.0f : n);
}

float ParamStore::fromNormalised(int id, float norm) const
{
    const ParamDesc& d = desc(id);
    const float n = norm < 0.0f ? 0.0f : (norm > 1.0f ? 1.0f : norm);
    float v;
    if (d.curve == Curve::Log) v = d.minValue * std::pow(d.maxValue / d.minValue, n);
    else v = d.minValue + n * (d.maxValue - d.minValue);
    if (isDiscrete(d.curve)) v = std::round(v);
    return v;
}

void ParamStore::resetDefaults()
{
    for (int i = 0; i < count(); ++i) values_[static_cast<size_t>(i)].store(desc(i).defValue, std::memory_order_relaxed);
}

int ParamStore::moduleCount(Module m)
{
    const int mi = static_cast<int>(m);
    return mi >= 0 && mi < static_cast<int>(Module::Count) ? kModules[mi].count : 0;
}

void ParamStore::readModule(Module m, int instance, float* out) const
{
    const int b = base(m, instance);
    if (b < 0) return;
    const int n = moduleCount(m);
    for (int i = 0; i < n; ++i) out[i] = get(b + i);
}

void ParamStore::copyValuesFrom(const ParamStore& other)
{
    const int n = count() < other.count() ? count() : other.count();
    for (int i = 0; i < n; ++i) values_[static_cast<size_t>(i)].store(other.get(i), std::memory_order_relaxed);
}

namespace {
/** @brief Lower case without spaces, for matching choice names ("Double Harmonic" = "doubleharmonic"). */
std::string foldName(std::string_view s)
{
    std::string out;
    for (char ch : s) {
        if (ch == ' ' || ch == '_' || ch == '-') continue;
        out += (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
    }
    return out;
}
} // namespace

bool ParamStore::parseText(std::string_view text, std::string* error)
{
    // Split into assignments. Newlines and ';' always separate; whitespace separates only where the
    // next word contains '=' -- so a choice name with a space ("compose.scale=Double Harmonic") stays
    // one value while "a=1 b=2" is still two assignments. '#' starts a comment to the end of the line.
    std::vector<std::string> items;
    size_t pos = 0;
    bool newItem = true;
    while (pos < text.size()) {
        const char ch = text[pos];
        if (ch == '\n' || ch == ';' || ch == '\r') { newItem = true; ++pos; continue; }
        if (ch == ' ' || ch == '\t') { ++pos; continue; }
        if (ch == '#') { while (pos < text.size() && text[pos] != '\n') ++pos; continue; }
        size_t end = pos;
        while (end < text.size() && text[end] != '\n' && text[end] != ';' && text[end] != '\r' && text[end] != ' ' && text[end] != '\t') ++end;
        const std::string_view word = text.substr(pos, end - pos);
        pos = end;
        if (newItem || word.find('=') != std::string_view::npos || items.empty()) items.emplace_back(word);
        else { items.back() += ' '; items.back() += word; }
        newItem = false;
    }

    bool ok = true;
    for (const std::string& item : items) {
        const std::string_view tok = trim(item);
        const size_t eq = tok.find('=');
        if (eq == std::string_view::npos) {
            if (error && ok) *error = "missing '=' in \"" + std::string(tok) + "\"";
            ok = false;
            continue;
        }
        const std::string_view k = trim(tok.substr(0, eq)), v = trim(tok.substr(eq + 1));
        const int id = find(k);
        if (id < 0) {
            if (error && ok) *error = "unknown parameter \"" + std::string(k) + "\"";
            ok = false;
            continue;
        }
        const ParamDesc& d = desc(id);
        bool matched = false;
        if ((d.curve == Curve::Choice && d.choices != nullptr) || d.curve == Curve::Toggle) {
            const std::string fv = foldName(v);
            if (d.curve == Curve::Toggle && (fv == "on" || fv == "off")) { set(id, fv == "on" ? 1.0f : 0.0f); matched = true; }
            for (int c = 0; !matched && d.choices != nullptr && c <= static_cast<int>(d.maxValue); ++c) {
                if (fv == foldName(d.choices[c])) { set(id, static_cast<float>(c)); matched = true; }
            }
        }
        if (!matched) {
            const std::string vs(v);
            char* stop = nullptr;
            const double x = std::strtod(vs.c_str(), &stop);
            if (vs.empty() || stop == nullptr || *stop != 0) {
                if (error && ok) *error = "bad value \"" + vs + "\" for " + std::string(k);
                ok = false;
                continue;
            }
            set(id, static_cast<float>(x));
        }
    }
    return ok;
}

std::string ParamStore::toText(bool onlyChanged) const
{
    std::string out;
    char buf[64];
    for (int i = 0; i < count(); ++i) {
        const float v = get(i);
        if (onlyChanged && v == desc(i).defValue) continue;
        // %.9g round-trips every float exactly.
        std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
        out += key(i);
        out += '=';
        out += buf;
        out += '\n';
    }
    return out;
}

std::string ParamStore::format(int id) const
{
    const ParamDesc& d = desc(id);
    const float v = get(id);
    if (d.curve == Curve::Choice && d.choices != nullptr) return d.choices[getInt(id)];
    if (d.curve == Curve::Toggle) return v >= 0.5f ? "On" : "Off";
    char buf[64];
    if (d.curve == Curve::Int) std::snprintf(buf, sizeof(buf), "%d", getInt(id));
    else if (std::fabs(v) >= 100.0f) std::snprintf(buf, sizeof(buf), "%.0f", static_cast<double>(v));
    else if (std::fabs(v) >= 10.0f) std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(v));
    else std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
    std::string s = buf;
    if (d.unit != nullptr && d.unit[0] != 0) { s += ' '; s += d.unit; }
    return s;
}

} // namespace phos
