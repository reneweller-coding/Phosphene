/**
 * @file PluginEditor.cpp
 * @brief The editor's frame: header, tab bar, lane bar, pages and the screenshot mode.
 */
#include "PluginEditor.h"
#include "phos/Composer.h"
#include "phos/FieldLibrary.h"
#include "phos/FieldPresets.h"
#include <algorithm>
#include <cmath>
#include <map>

using namespace phos;
using namespace phosui;

namespace {

/** @brief One titled slice of a module's descriptor table. */
struct Slice {
    const char* title;   ///< the group's title
    int first;           ///< first index in the module table
    int count;           ///< how many, -1 = to the end
    int columns;         ///< cell units per row
};

/** @brief The mixer and the master. */
// The channels up to the SFX strip, then the sidechain, then the two strips of 19.09.2026 (texture, vocal).
const Slice kMasterSlices[] = {
    { "Gain", master::Gain, 3, 3 }, { "Compressor", master::CompThreshold, 5, 5 }, { "Output", master::MonoBass, 7, 4 },
    { "Monitor", master::Monitor, 1, 2 },   // 25.09.2026, the addon's monitoring
};

template <size_t N>
/** @brief Adds one group per slice of a module's parameter table to @p page. */
void addSlices(ControlPage& page, PhospheneProcessor& proc, Module m, int instance, const Slice (&slices)[N], juce::Colour tint)
{
    for (const Slice& s : slices) page.addModuleGroup(proc, m, instance, s.title, tint, s.columns, s.first, s.count);
}

/**
 * @name The synth pages by signal flow (26.09.2026)
 *
 * The user: "wichtigere Encoder größer machen (sowas wie Cutoff im Filter, und so weiter) und die Funktionsgruppen
 * sinnvoll und platzsparend, ohne zu große Lücken, anordnen". Until then a page was cut into slices of its module's
 * table, in the order the parameters had been appended over the rounds -- which put "Plate", "Colour & Hall" or
 * "Glide & Image" into groups of one or two. A page is now written down the way a synthesizer's panel reads: source,
 * filter, amplifier, movement, space, output, each knob with the size of its weight -- large for what shapes the
 * sound most (cutoff and resonance, the wavetable position, the level), small for sends and fine settings. A
 * parameter the table below forgets is not lost: addSections puts it into a group "More" of its own, where the next
 * look at the page finds it.
 * @{ */
struct SizedKey {
    const char* key;   ///< the parameter's key inside its module ("cutoff")
    CellSize size;     ///< the size of its control
};
struct Section {
    const char* title;              ///< the group's caption
    int columns;                    ///< its width in normal cells
    std::vector<SizedKey> keys;     ///< its parameters, in the order they are placed
};
constexpr CellSize S = CellSize::Small, N = CellSize::Normal, L = CellSize::Large;

/** @brief The modulation block's groups (Modulation.h; 26.09.2026), the same on every synth that has it. */
const std::vector<Section> kModSections = {
    { "Mod Envelope", 4, { { "menv_attack", S }, { "menv_decay", S }, { "menv_sustain", S }, { "menv_release", S } } },
    { "LFO 1", 5, { { "lfo1_shape", N }, { "lfo1_sync", N }, { "lfo1_rate", N }, { "lfo1_retrig", S }, { "lfo1_fade", S } } },
    { "LFO 2", 5, { { "lfo2_shape", N }, { "lfo2_sync", N }, { "lfo2_rate", N }, { "lfo2_retrig", S }, { "lfo2_fade", S } } },
    { "LFO 3", 5, { { "lfo3_shape", N }, { "lfo3_sync", N }, { "lfo3_rate", N }, { "lfo3_retrig", S }, { "lfo3_fade", S } } },
    { "LFO 4", 5, { { "lfo4_shape", N }, { "lfo4_sync", N }, { "lfo4_rate", N }, { "lfo4_retrig", S }, { "lfo4_fade", S } } },
    { "Mod Matrix", 10, { { "mx1_src", N }, { "mx1_dst", N }, { "mx1_amount", S }, { "mx2_src", N }, { "mx2_dst", N }, { "mx2_amount", S },
                          { "mx3_src", N }, { "mx3_dst", N }, { "mx3_amount", S }, { "mx4_src", N }, { "mx4_dst", N }, { "mx4_amount", S },
                          { "mx5_src", N }, { "mx5_dst", N }, { "mx5_amount", S }, { "mx6_src", N }, { "mx6_dst", N }, { "mx6_amount", S },
                          { "mx7_src", N }, { "mx7_dst", N }, { "mx7_amount", S }, { "mx8_src", N }, { "mx8_dst", N }, { "mx8_amount", S } } },
};
/** @brief @p own with the modulation groups after its first @p at groups (the source and the filter come first). */
std::vector<Section> withModulation(const std::vector<Section>& own, size_t at)
{
    std::vector<Section> out(own.begin(), own.begin() + static_cast<std::ptrdiff_t>(std::min(at, own.size())));
    out.insert(out.end(), kModSections.begin(), kModSections.end());
    out.insert(out.end(), own.begin() + static_cast<std::ptrdiff_t>(std::min(at, own.size())), own.end());
    return out;
}
const std::vector<Section> kKickSections = {
    { "Pitch", 6, { { "engine", N }, { "tune", L }, { "pitch_start", N }, { "pitch_end", N }, { "pitch_decay", N } } },
    { "Punch & Body", 5, { { "punch", L }, { "punch_decay", N }, { "amp_decay", N }, { "amp_attack", S }, { "amp_hold", S } } },
    { "Click", 3, { { "click_level", N }, { "click_tone", N }, { "click_decay", N } } },
    { "Drive & Output", 5, { { "level", L }, { "drive", N }, { "clip", N }, { "tone", N }, { "tail_limit", S } } },
};
const std::vector<Section> kBassSectionsOwn = {
    { "Oscillator", 5, { { "wave", N }, { "sub_mode", N }, { "pulse_width", N }, { "sub", N }, { "sub_octave", S },
                         { "split_ratio", S }, { "start_phase", S }, { "retrigger", S }, { "kick_lock", S } } },
    { "Filter", 6, { { "filter_model", N }, { "cutoff", L }, { "resonance", L }, { "env_amount", N },
                     { "filter_mode", S }, { "key_track", S }, { "vel_to_cutoff", S } } },
    { "Filter Envelope", 4, { { "filt_attack", S }, { "filter_decay", S }, { "filt_sustain", S }, { "filt_release", S } } },
    { "Bite", 5, { { "bite", N }, { "bite_cutoff", N }, { "bite_resonance", S }, { "bite_env", S }, { "bite_decay", S },
                   { "bite_drive", S } } },
    { "Amplifier", 4, { { "drive", N }, { "amp_attack", S }, { "amp_decay", S }, { "amp_sustain", S }, { "amp_release", S } } },
    { "Duck & Level", 4, { { "level", L }, { "duck_depth", N }, { "duck_hold", S }, { "duck_release", S } } },
};
const std::vector<Section> kBassSections = withModulation(kBassSectionsOwn, 5);   // after Amplifier
const std::vector<Section> kAcidSectionsOwn = {
    { "Voice", 4, { { "wave", N }, { "slide_time", N }, { "amp_decay", N } } },
    { "Filter", 6, { { "cutoff", L }, { "resonance", L }, { "env_amount", N }, { "decay", N }, { "accent", N }, { "key_track", S } } },
    { "Squelch & Drive", 5, { { "squelch", N }, { "drive", N }, { "squelch_start", S }, { "squelch_time", S }, { "low_cut", S },
                              { "comb_mix", S }, { "comb_feedback", S }, { "disperse", S }, { "disperse_freq", S } } },
    { "Delay", 4, { { "delay_send", N }, { "delay_feedback", N }, { "delay_left", S }, { "delay_right", S },
                    { "delay_high_pass", S }, { "delay_low_pass", S } } },
    { "Space & Output", 5, { { "level", L }, { "duck", N }, { "room_send", S }, { "plate_send", S }, { "hall_send", S },
                             { "hall_gate", S } } },
};
const std::vector<Section> kAcidSections = withModulation(kAcidSectionsOwn, 3);   // after Squelch & Drive
const std::vector<Section> kPolySectionsOwn = {
    { "Oscillator", 6, { { "osc", N }, { "table", N }, { "position", L }, { "detune", N }, { "mix", N }, { "wave", N },
                         { "dynamic_detune", S }, { "pulse_width", S }, { "drift", S } } },
    { "Wavetable Motion", 4, { { "pos_env", N }, { "pos_lfo_depth", N }, { "pos_decay", S }, { "pos_lfo_beats", S } } },
    { "FM", 3, { { "fm_ratio", N }, { "fm_index", N }, { "fm_decay", S } } },
    { "Second Oscillator", 4, { { "osc2", N }, { "osc2_mix", N }, { "osc2_interval", N }, { "osc2_detune", S } } },
    { "Filter", 6, { { "filter_model", N }, { "filter_type", N }, { "cutoff", L }, { "resonance", L }, { "env_amount", N },
                     { "filter_mode", S }, { "key_track", S }, { "hp_floor", S }, { "hp_track", S }, { "disperse", S }, { "disperse_freq", S } } },
    { "Filter Envelope", 4, { { "filt_attack", S }, { "filter_decay", S }, { "filt_sustain", S }, { "filt_release", S } } },
    { "Amplifier", 4, { { "amp_attack", N }, { "amp_decay", N }, { "amp_sustain", N }, { "amp_release", N }, { "vel_sens", S },
                        { "glide", S } } },
    { "Vibrato & Tremolo", 4, { { "lfo_beats", N }, { "lfo_cutoff", S }, { "lfo_pitch", S }, { "lfo_amp", S } } },
    { "Voice FX", 5, { { "mod", N }, { "mod_mix", N }, { "mod_depth", N }, { "mod_beats", S }, { "mod_feedback", S } } },
    { "Trance Gate", 5, { { "gate", N }, { "gate_pattern", N }, { "gate_depth", N }, { "gate_duty", S }, { "gate_attack", S },
                          { "gate_release", S }, { "gate_tone", S } } },
    { "Delay", 4, { { "delay_send", N }, { "delay_feedback", N }, { "delay_left", S }, { "delay_right", S },
                    { "delay_high_pass", S }, { "delay_low_pass", S } } },
    { "Space & Image", 5, { { "distance", N }, { "width", N }, { "room_send", S }, { "plate_send", S }, { "hall_send", S },
                            { "hall_gate", S }, { "pan", S }, { "slow_mod", S } } },
    { "Output", 3, { { "level", L }, { "duck", N } } },
};
const std::vector<Section> kPolySections = withModulation(kPolySectionsOwn, 7);   // after Amplifier
// The Field track's sampler (27.09.2026, FieldPlayer.h): the two layers, the loop, the filter and the envelopes, the
// synths' modulation block after the amplifier, and the strip's space.
const std::vector<Section> kFieldSectionsOwn = {
    { "Layer A", 6, { { "a_category", N }, { "a_variation", N }, { "a_level", L }, { "a_pitch", N }, { "a_fine", S },
                      { "a_start", S }, { "a_start_random", S }, { "a_reverse", S } } },
    { "Layer B", 6, { { "b_on", S }, { "b_category", N }, { "b_variation", N }, { "b_level", L }, { "b_pitch", N },
                      { "b_fine", S }, { "b_start", S }, { "b_start_random", S }, { "b_reverse", S }, { "layer_mix", N } } },
    { "Loop", 4, { { "loop", S }, { "loop_start", N }, { "loop_end", N }, { "loop_xfade", N } } },
    { "Filter", 6, { { "filter_model", N }, { "filter_type", N }, { "cutoff", L }, { "resonance", L }, { "env_amount", N },
                     { "filter_mode", S }, { "low_cut", S } } },
    { "Filter Envelope", 4, { { "filt_attack", S }, { "filter_decay", S }, { "filt_sustain", S }, { "filt_release", S } } },
    { "Amplifier", 4, { { "amp_attack", N }, { "amp_decay", N }, { "amp_sustain", N }, { "amp_release", N } } },
    { "Space & Output", 5, { { "width", N }, { "pan", N }, { "duck", N }, { "room_send", S }, { "plate_send", S },
                             { "hall_send", S } } },
};
const std::vector<Section> kFieldSections = withModulation(kFieldSectionsOwn, 6);   // after Amplifier
// The effects page: the generator, then the three rooms of the mix guide -- A the room, B the plate, C the hall, each
// with its own filters and pre-delay -- and the returns. The effect presets stand in a group of their own.
const std::vector<Section> kSfxSections = {
    { "Effect Generator", 6, { { "level", L }, { "noise", N }, { "resonance", N }, { "brightness", N }, { "impact_decay", N },
                               { "swell_decay", N }, { "vowel", S }, { "width", S }, { "sub_level", S }, { "sub_duck", S },
                               { "wander", S }, { "wander_send", S }, { "room_send", S }, { "plate_send", S }, { "hall_send", S },
                               { "duck", S } } },
};
const std::vector<Section> kFxSections = {
    { "Room (A)", 4, { { "room_size", N }, { "room_decay", N }, { "room_damping", S }, { "room_pre_delay", S }, { "room_low_cut", S },
                       { "room_high_cut", S } } },
    { "Plate (B)", 4, { { "plate_size", N }, { "plate_decay", N }, { "plate_damping", S }, { "plate_pre_delay", S },
                        { "plate_low_cut", S }, { "plate_high_cut", S }, { "plate_to_hall", S } } },
    { "Hall (C)", 4, { { "hall_size", N }, { "hall_decay", N }, { "hall_damping", S }, { "hall_pre_delay", S }, { "low_cut", S },
                       { "high_cut", S } } },
    { "Returns", 4, { { "room_return", N }, { "plate_return", N }, { "hall_return", N }, { "return_duck", S },
                      { "return_duck_release", S } } },
};
const std::vector<Section> kPercSections = {
    { "Lane", 5, { { "active", N }, { "role", N }, { "engine", N }, { "density", N }, { "choke", S }, { "cut_track", S } } },
    { "Source", 6, { { "pitch", L }, { "pitch_amount", N }, { "pitch_decay", N }, { "tune", S }, { "shift", S }, { "fm_ratio", S },
                     { "fm_index", S }, { "mode_set", N }, { "mode_damp", S }, { "metal_scale", S } } },
    { "Noise", 4, { { "noise", N }, { "noise_decay", N }, { "bursts", S }, { "burst_spacing", S } } },
    { "Filter & Shape", 5, { { "filter", N }, { "cutoff", L }, { "resonance", N }, { "low_cut", N }, { "decay", N }, { "drive", S } } },
    { "Output & Motion", 4, { { "level", L }, { "pan", N }, { "pan_depth", S }, { "pan_bars", S } } },
};

/**
 * @brief Adds @p sections of a module instance to @p page, and whatever they leave out as a group "More".
 * @param restFrom first table index a leftover may come from; @param restTo one past the last, -1 = the table's end
 *        (the effect presets of the SFX table have a group of their own, addSfxPresetGroup)
 */
void addSections(ControlPage& page, PhospheneProcessor& proc, Module m, int instance, const std::vector<Section>& sections,
                 juce::Colour tint, int restFrom = 0, int restTo = -1)
{
    const ParamStore& ps = proc.params();
    const int base = ps.base(m, instance), count = ParamStore::moduleCount(m);
    if (base < 0 || count <= 0) return;
    const juce::String prefix = juce::String(ps.key(base)).upToFirstOccurrenceOf(".", false, false);
    std::vector<char> seen(static_cast<size_t>(count), 0);
    for (const Section& sec : sections) {
        std::vector<SizedParam> params;
        for (const SizedKey& k : sec.keys) {
            const int id = ps.find((prefix + "." + k.key).toStdString());
            jassert(id >= base && id < base + count);   // a key the table misspells
            if (id < base || id >= base + count || seen[static_cast<size_t>(id - base)] != 0) continue;
            seen[static_cast<size_t>(id - base)] = 1;
            params.push_back({ id, k.size });
        }
        if (!params.empty()) page.addSizedGroup(proc, sec.title, tint, sec.columns, params);
    }
    // The leftovers of [restFrom, restTo) -- the whole table unless a page shows a part of it elsewhere.
    std::vector<SizedParam> rest;
    const int to = restTo < 0 ? count : juce::jmin(count, restTo);
    for (int i = juce::jmax(0, restFrom); i < to; ++i) if (seen[static_cast<size_t>(i)] == 0) rest.push_back({ base + i, S });
    if (!rest.empty()) page.addSizedGroup(proc, "More", tint, 5, rest);
}
/** @} */

} // namespace

// ==================================================================== LoudnessDisplay

void LoudnessDisplay::update(const LoudnessReading& r, float compDb, float limitDb, float target)
{
    reading_ = r;
    comp_ = compDb;
    limit_ = limitDb;
    target_ = target;
    repaint();
}

void LoudnessDisplay::paint(juce::Graphics& g)
{
    const juce::Rectangle<int> r = getLocalBounds().reduced(4, 3);
    g.setColour(bg0.withAlpha(0.6f));
    g.fillRoundedRectangle(r.toFloat(), 5.0f);
    juce::Rectangle<int> area = r.reduced(7, 5);
    g.setColour(dim);
    g.setFont(title(10.0f));
    g.drawText("MASTER", area.removeFromTop(13), juce::Justification::topLeft, false);
    const int rowH = juce::jmax(9, (area.getHeight() - 14) / 3);

    // -50 .. 0 LUFS / dBTP on one scale, so the bars can be read against each other.
    auto bar = [&](juce::Rectangle<int> band, const juce::String& name, float value, juce::Colour colour, float over) {
        juce::Rectangle<int> row = band.reduced(0, 2);
        g.setColour(faint);
        g.setFont(phosui::body(9.5f));
        g.drawText(name, row.removeFromLeft(38), juce::Justification::centredLeft, false);
        const juce::Rectangle<int> num = row.removeFromRight(50);
        const float t = juce::jlimit(0.0f, 1.0f, (value + 50.0f) / 50.0f);
        g.setColour(edge);
        g.fillRoundedRectangle(row.toFloat(), 2.5f);
        g.setColour(value > over ? red : colour);
        g.fillRoundedRectangle(row.withWidth(juce::roundToInt(row.getWidth() * t)).toFloat(), 2.5f);
        g.setColour(text);
        g.drawText(value <= -99.0f ? juce::String("--") : juce::String(value, 1), num, juce::Justification::centredRight, false);
    };
    bar(area.removeFromTop(rowH), "Short", reading_.shortTerm, green, target_ + 3.0f);
    bar(area.removeFromTop(rowH), "Integr.", reading_.integrated, accent, target_ + 2.0f);
    // The limiter's ceiling is -1 dBTP, so exactly -1.0 is the target and not an overshoot.
    bar(area.removeFromTop(rowH), "Peak", reading_.truePeak, green, -0.9f);
    g.setColour(faint);
    g.setFont(phosui::body(9.5f));
    g.drawText("comp " + juce::String(comp_, 1) + " dB   limit " + juce::String(limit_, 1) + " dB   target "
                   + juce::String(target_, 1) + " LUFS",
               area.removeFromTop(13), juce::Justification::centredLeft, false);
}

// ==================================================================== TrackDisplay

void TrackDisplay::paint(juce::Graphics& g)
{
    const juce::Rectangle<int> r = getLocalBounds().reduced(4, 3);
    g.setColour(bg0.withAlpha(0.6f));
    g.fillRoundedRectangle(r.toFloat(), 5.0f);
    juce::Rectangle<int> area = r.reduced(7, 5);
    g.setColour(dim);
    g.setFont(title(10.0f));
    g.drawText("TRACKS", area.removeFromTop(14), juce::Justification::topLeft, false);
    if (rows_.empty()) {
        g.setColour(faint);
        g.setFont(phosui::body(11.5f));
        g.drawText("planning...", area, juce::Justification::centred, false);
        return;
    }
    const int rowH = juce::jlimit(13, 20, area.getHeight() / juce::jmax(1, static_cast<int>(rows_.size())));
    // Over the DJ overlap (Form.h, kDjOverlap) two rows contain the play head: the outgoing track, which
    // owns the bar (kick, bass, key), and the incoming one, whose intro is mixing in. The first is marked
    // as playing; the second more faintly, and it says so.
    bool ownerSeen = false;
    for (const Row& row : rows_) {
        if (area.getHeight() < rowH) break;
        juce::Rectangle<int> line = area.removeFromTop(rowH);
        const bool inside = bar_ >= row.bar && bar_ < row.bar + row.bars;
        const bool here = inside && !ownerSeen;
        const bool mixingIn = inside && ownerSeen;
        ownerSeen = ownerSeen || inside;
        if (mixingIn) {
            g.setColour(accent.withAlpha(0.07f));
            g.fillRoundedRectangle(line.toFloat(), 3.0f);
            g.setColour(accent.withAlpha(0.7f));
            g.setFont(phosui::body(9.5f));
            g.drawText("mixing in, " + juce::String(juce::jmax(0, row.bar + kDjOverlap - bar_)) + " bars to the hand-over",
                       line.reduced(6, 0), juce::Justification::centredRight, false);
        }
        if (here) {
            g.setColour(accent.withAlpha(0.18f));
            g.fillRoundedRectangle(line.toFloat(), 3.0f);
            // How far into the track the play head stands.
            const float t = row.bars > 0 ? juce::jlimit(0.0f, 1.0f, static_cast<float>(bar_ - row.bar) / row.bars) : 0.0f;
            g.setColour(accent);
            g.fillRect(line.getX() + juce::roundToInt(t * line.getWidth()), line.getY(), 2, line.getHeight());
        }
        g.setColour(here ? text : dim);
        g.setFont(phosui::body(11.5f));
        g.drawText(row.text, line.reduced(4, 0), juce::Justification::centredLeft, false);
    }
}

void TrackDisplay::mouseDown(const juce::MouseEvent& e)
{
    if (rows_.empty() || !onJump) return;
    const juce::Rectangle<int> area = getLocalBounds().reduced(11, 8).withTrimmedTop(14);
    const int rowH = juce::jlimit(13, 20, area.getHeight() / juce::jmax(1, static_cast<int>(rows_.size())));
    const int index = (e.y - area.getY()) / juce::jmax(1, rowH);
    if (index >= 0 && index < static_cast<int>(rows_.size())) onJump(rows_[static_cast<size_t>(index)].bar);
}

// ==================================================================== PatternDisplay

void PatternDisplay::update(std::vector<NoteEvent> notes, int firstBar, int bars, double beat)
{
    notes_ = std::move(notes);
    firstBar_ = firstBar;
    bars_ = juce::jmax(1, bars);
    beat_ = beat;
    repaint();
}

void PatternDisplay::paint(juce::Graphics& g)
{
    const juce::Rectangle<int> r = getLocalBounds().reduced(4, 3);
    g.setColour(bg0.withAlpha(0.6f));
    g.fillRoundedRectangle(r.toFloat(), 5.0f);
    juce::Rectangle<int> area = r.reduced(8, 6);
    g.setColour(dim);
    g.setFont(title(10.0f));
    g.drawText(juce::String(kPartNames[static_cast<int>(part_)]).toUpperCase() + "   bars "
                   + juce::String(firstBar_ + 1) + " to " + juce::String(firstBar_ + bars_),
               area.removeFromTop(13), juce::Justification::topLeft, false);
    const juce::Rectangle<float> plot = area.toFloat();
    if (plot.getHeight() < 12.0f) return;

    const double from = static_cast<double>(firstBar_) * kBeatsPerBar;
    const double span = static_cast<double>(bars_) * kBeatsPerBar;
    auto xOf = [&](double beat) { return plot.getX() + static_cast<float>((beat - from) / span) * plot.getWidth(); };

    // The grid: a hairline every sixteenth, brighter on beats, brightest on bars.
    for (int s = 0; s <= bars_ * kBeatsPerBar * 4; ++s) {
        const double beat = from + s * 0.25;
        const bool bar = s % (kBeatsPerBar * 4) == 0, beatLine = s % 4 == 0;
        g.setColour(bar ? edge.brighter(0.5f) : (beatLine ? edge : edge.withAlpha(0.35f)));
        g.drawVerticalLine(juce::roundToInt(xOf(beat)), plot.getY(), plot.getBottom());
    }

    if (notes_.empty()) {
        g.setColour(faint);
        g.setFont(phosui::body(11.0f));
        g.drawText("nothing composed here yet", plot, juce::Justification::centred, false);
    } else if (part_ == Part::Perc) {
        // Twelve lanes, named by the role each lane plays; the lane whose knobs are on screen is lit.
        const float rowH = plot.getHeight() / kPercLanes;
        for (int lane = 0; lane < kPercLanes; ++lane) {
            const juce::Rectangle<float> row(plot.getX(), plot.getY() + lane * rowH, plot.getWidth(), rowH);
            if (lane == lane_) {
                g.setColour(accent.withAlpha(0.10f));
                g.fillRect(row);
            }
            g.setColour(faint.withAlpha(0.6f));
            g.setFont(phosui::body(juce::jlimit(7.0f, 9.5f, rowH * 0.7f)));
            g.drawText(juce::String(lane + 1), row.withWidth(14.0f), juce::Justification::centredRight, false);
        }
        for (const NoteEvent& e : notes_) {
            if (e.part != Part::Perc || e.lane >= kPercLanes) continue;
            const juce::Rectangle<float> row(xOf(e.beat), plot.getY() + e.lane * rowH + 1.5f,
                                             juce::jmax(3.0f, xOf(e.beat + 0.22) - xOf(e.beat)), rowH - 3.0f);
            g.setColour((e.lane == lane_ ? accent : partColour(TabPerc)).withAlpha(0.35f + 0.65f * e.velocity / 127.0f));
            g.fillRoundedRectangle(row, 1.5f);
        }
    } else {
        // A piano roll over whatever pitches this part uses, with two semitones of air.
        int lo = 127, hi = 0;
        for (const NoteEvent& e : notes_) if (e.part == part_) { lo = juce::jmin(lo, static_cast<int>(e.pitch)); hi = juce::jmax(hi, static_cast<int>(e.pitch)); }
        if (lo > hi) {
            g.setColour(faint);
            g.setFont(phosui::body(11.0f));
            g.drawText("this part is silent here", plot, juce::Justification::centred, false);
            return;
        }
        lo -= 2;
        hi += 2;
        const float rowH = plot.getHeight() / juce::jmax(1, hi - lo + 1);
        // The black keys as darker stripes, so the pitches can be read off.
        for (int p = lo; p <= hi; ++p) {
            static const bool black[12] = { false, true, false, true, false, false, true, false, true, false, true, false };
            if (!black[((p % 12) + 12) % 12]) continue;
            g.setColour(edge.withAlpha(0.25f));
            g.fillRect(plot.getX(), plot.getBottom() - (p - lo + 1) * rowH, plot.getWidth(), rowH);
        }
        for (const NoteEvent& e : notes_) {
            if (e.part != part_) continue;
            const float y = plot.getBottom() - (e.pitch - lo + 1) * rowH;
            // A note struck before the window (a held chord) starts at its left edge, one that outlasts it ends at
            // the right one.
            const float x0 = juce::jmax(plot.getX(), xOf(e.beat)), x1 = juce::jmin(plot.getRight(), xOf(e.beat + e.length));
            const juce::Rectangle<float> box(x0, y + 0.5f, juce::jmax(3.0f, x1 - x0 - 1.0f), juce::jmax(2.0f, rowH - 1.0f));
            const juce::Colour c = partColourOf(static_cast<int>(part_));
            g.setColour((e.flags & kNoteAccent) ? c.brighter(0.5f) : c.withAlpha(0.45f + 0.55f * e.velocity / 127.0f));
            g.fillRoundedRectangle(box, 1.5f);
            // A slide is drawn as a line into the next note, which is what it does.
            if (e.flags & kNoteSlide) {
                g.setColour(c.withAlpha(0.7f));
                g.drawLine(box.getRight(), box.getCentreY(), box.getRight() + 5.0f, box.getCentreY(), 1.2f);
            }
        }
    }

    // Where the engine is. It sits behind the composed bars by the horizon the rings are kept at,
    // so most of the time the line stands at the very left of the window the display is showing.
    if (beat_ >= from && beat_ <= from + span) {
        g.setColour(accent.withAlpha(0.85f));
        g.drawVerticalLine(juce::roundToInt(xOf(beat_)), plot.getY(), plot.getBottom());
    }
}

// ==================================================================== PhospheneEditor

const juce::StringArray& PhospheneEditor::tabNames()
{
    static const juce::StringArray names{ "Set", "Arrange", "Kick", "Bass", "Percussion", "Acid",
                                          "Lead", "Counter", "Arp", "Stab", "Pad", "Drone", "SFX / FX", "Field", "Mixer / Master", "Perform",
                                          "Export", "Gallery" };
    jassert(names.size() == TabCount);
    return names;
}

namespace {
/** @brief The groups of tabs (01.10.2026): the row on top, as short as every generator's. */
const std::vector<std::pair<juce::String, std::vector<int>>>& tabGroups()
{
    static const std::vector<std::pair<juce::String, std::vector<int>>> groups = {
        { "Set", { TabSet } }, { "Arrange", { TabArrange } }, { "Low End", { TabKick, TabBass } }, { "Percussion", { TabPerc } },
        { "Acid", { TabAcid } }, { "Synths", { TabLead, TabCounter, TabArp, TabStab, TabPad, TabDrone } },
        { "Effects", { TabFx, TabField } }, { "Mixer", { TabMix } }, { "Perform", { TabPerform } }, { "Export", { TabExport } },
        { "Gallery", { TabGallery } },
    };
    return groups;
}
constexpr int kOverviewH = 74;   ///< the set strip under the header
constexpr int kTabRowH = 32;     ///< the row of tabs
constexpr int kSubRowH = 30;     ///< the row of a group's pages (or the percussion lanes)
} // namespace

const juce::StringArray& PhospheneEditor::groupNames()
{
    static const juce::StringArray names = [] {
        juce::StringArray n;
        for (const auto& g : tabGroups()) n.add(g.first);
        return n;
    }();
    return names;
}

int PhospheneEditor::groupOf(int tab)
{
    for (size_t g = 0; g < tabGroups().size(); ++g)
        for (int t : tabGroups()[g].second) if (t == tab) return static_cast<int>(g);
    return 0;
}

const std::vector<int>& PhospheneEditor::tabsOf(int g) { return tabGroups()[static_cast<size_t>(juce::jlimit(0, static_cast<int>(tabGroups().size()) - 1, g))].second; }

PhospheneEditor::PhospheneEditor(PhospheneProcessor& p) : juce::AudioProcessorEditor(&p), proc_(p)
{
    setLookAndFeel(&lnf_);
    content_.onPaint = [this](juce::Graphics& g) { paintContent(g); };
    content_.onResized = [this] { layoutContent(); };
    addAndMakeVisible(content_);
    viewport_.setScrollBarsShown(true, false);
    viewport_.setViewedComponent(nullptr, false);
    content_.addAndMakeVisible(viewport_);

    // The row of tabs (01.10.2026): one per group, drawn as every generator's; a group of several pages shows them in a
    // row of its own under it, each in its family's colour (PhospheneLookAndFeel).
    lastTabOfGroup_.assign(tabGroups().size(), 0);
    for (int g = 0; g < static_cast<int>(tabGroups().size()); ++g) {
        lastTabOfGroup_[static_cast<size_t>(g)] = tabsOf(g).front();
        auto* b = groupTabs_.add(new frame::FlatTab(groupNames()[g]));
        b->onClick = [this, g] { setTab(lastTabOfGroup_[static_cast<size_t>(g)]); };
        content_.addAndMakeVisible(b);
        if (tabsOf(g).size() < 2) continue;
        for (int t : tabsOf(g)) {
            auto* s = subButtons_.add(new juce::TextButton(tabNames()[t]));
            s->setClickingTogglesState(false);
            s->getProperties().set("stripe", static_cast<juce::int64>(partColour(t).getARGB()));
            s->onClick = [this, t] { setTab(t); };
            subTab_.push_back(t);
            content_.addChildComponent(s);
        }
    }
    // The frame's header (Frame.h): where the set starts and how long its arc is, Compose set, New seed, Play, Mute; the
    // ratings, the status and where the set is; undo, redo, help and the settings.
    {
        const ParamStore& ps = proc_.params();
        const int cb = ps.base(Module::Compose);
        auto combo = [&](juce::ComboBox& box, int id, const char* tip) {
            const ParamDesc& d = ps.desc(id);
            for (int k = 0; d.choices != nullptr && k <= static_cast<int>(d.maxValue); ++k) box.addItem(d.choices[k], k + 1);
            headerLinks_.push_back(std::make_unique<juce::ComboBoxParameterAttachment>(*proc_.parameterFor(id), box));
            box.setTooltip(tip);
            content_.addAndMakeVisible(box);
        };
        combo(style_, cb + compose::Style, "The style the set starts in (Style Mix on the Set tab lets it walk)");
        combo(key_, cb + compose::Key, "The key the set starts in (it walks from track to track)");
        combo(scale_, cb + compose::Scale, "The scale the set starts in");
        length_.setSliderStyle(juce::Slider::LinearHorizontal);
        length_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 62, 20);
        lengthLink_ = std::make_unique<juce::SliderParameterAttachment>(*proc_.parameterFor(cb + compose::SetMinutes), length_);
        length_.setTextValueSuffix(" min");
        length_.setTooltip("The set's length: the time its energy arc spans (the set plays on after it)");
        lengthLabel_.setText("Set", juce::dontSendNotification);
        lengthLabel_.setColour(juce::Label::textColourId, dim);
        lengthLabel_.setJustificationType(juce::Justification::centredRight);
        content_.addAndMakeVisible(length_);
        content_.addAndMakeVisible(lengthLabel_);
    }
    compose_.setTooltip("Plans the set again with the knobs as they stand -- its seed, locks and rerolls kept -- and starts it "
                        "from the beginning");
    compose_.onClick = [this] { proc_.composeSet(); arrangeDirty_ = true; };
    newSeed_.setTooltip("A new seed: another night from the same knobs");
    newSeed_.onClick = [this] {
        proc_.undoable("New seed", [this] { proc_.randomiseSeed(); });
        if (seedEditor_ != nullptr) seedEditor_->setText(juce::String(proc_.seed()), false);
        trackRows_.clear();
        rowsSeed_ = 0;
        arrangeDirty_ = true;
    };
    play_.setTooltip("Play, and stop (Space)");
    play_.onClick = [this] { if (proc_.isPlaying()) proc_.stop(); else proc_.play(); };
    mute_.setClickingTogglesState(true);
    mute_.setToggleState(proc_.muted(), juce::dontSendNotification);
    mute_.setEnabled(!proc_.muteForced());
    mute_.setColour(juce::TextButton::buttonOnColourId, red.withAlpha(0.55f));
    mute_.setTooltip(proc_.muteForced() ? "Muted by PHOS_MUTE: an automated run makes no sound" : "Silence the output");
    mute_.onClick = [this] { proc_.setMuted(mute_.getToggleState()); };
    play_.setColour(juce::TextButton::buttonColourId, accent.withAlpha(0.22f));
    compose_.setColour(juce::TextButton::buttonColourId, accent.withAlpha(0.12f));
    for (juce::Component* comp : { static_cast<juce::Component*>(&compose_), static_cast<juce::Component*>(&newSeed_),
                                   static_cast<juce::Component*>(&play_), static_cast<juce::Component*>(&mute_),
                                   static_cast<juce::Component*>(&status_), static_cast<juce::Component*>(&where_),
                                   static_cast<juce::Component*>(&titleSpot_) })
        content_.addAndMakeVisible(comp);
    titleSpot_.setInterceptsMouseClicks(false, false);
    status_.setColour(juce::Label::textColourId, text);
    status_.setMinimumHorizontalScale(0.8f);
    where_.setColour(juce::Label::textColourId, dim);
    where_.setJustificationType(juce::Justification::centredRight);
    // The tools; their names are what the host test presses by (Tests/hosttest.cpp), their tooltips say the keys.
    undoIcon_.setButtonText("Undo");
    redoIcon_.setButtonText("Redo");
    helpIcon_.setButtonText("Help");
    settingsIcon_.setButtonText("Settings ...");
    likeIcon_.setButtonText("Good here");
    dislikeIcon_.setButtonText("Bad here");
    undoIcon_.onClick = [this] { proc_.undo(); };
    redoIcon_.onClick = [this] { proc_.redo(); };
    helpIcon_.onClick = [this] { showHelp(!helpShown()); };
    settingsIcon_.onClick = [this] { showSettings(); };
    headsetIcon_.onClick = [this] { setTab(TabPerform); };
    likeIcon_.onClick = [this] { rateNow(1); };
    dislikeIcon_.onClick = [this] { rateNow(-1); };
    for (auto* b : { &undoIcon_, &redoIcon_, &helpIcon_, &settingsIcon_, &likeIcon_, &dislikeIcon_ }) content_.addAndMakeVisible(b);
    content_.addChildComponent(headsetIcon_);
    // The set as a strip under the header (the Arrange tab's top, to scale): a click on a track jumps to it.
    overview_.setStripOnly(true);
    overview_.onSeek = [this](int bar) { proc_.seekToBar(bar); };
    content_.addAndMakeVisible(overview_);
    frame::Settings::of("Phosphene").addChangeListener(this);
    updateButton_.setTooltip("Open the release page");
    updateButton_.onClick = [this] {
        const juce::String url = updates_->result().url;
        if (url.isNotEmpty()) juce::URL(url).launchInDefaultBrowser();
    };
    content_.addChildComponent(updateButton_);
    help_ = std::make_unique<HelpView>(proc_);
    content_.addChildComponent(*help_);
    // Not while the editor is photographing itself for the manual: that run asks nobody anything.
    const bool shooting = juce::SystemStats::getEnvironmentVariable("PHOS_SHOT", "").isNotEmpty()
                       || juce::SystemStats::getEnvironmentVariable("PHOS_SHOT_ALL", "").isNotEmpty()
                       || juce::SystemStats::getEnvironmentVariable("PHOS_MANUAL", "").isNotEmpty();
    shooting_ = shooting;
    if (!shooting) updates_->startIfDue(proc_.userFolder().getChildFile("update.txt"));
    setWantsKeyboardFocus(true);
    for (int i = 0; i < kPercLanes; ++i) {
        auto* b = laneButtons_.add(new juce::TextButton(juce::String(i + 1)));
        b->onClick = [this, i] { setPercLane(i); };
        content_.addChildComponent(b);
    }
    buildPages();

    // The design size is the size at which no page has to scroll; the window opens scaled to fit
    // the screen, and dragging its corner only zooms (the aspect ratio is fixed).
    int tallest = 0, widest = 0;
    for (auto& page : pages_) if (page != nullptr) { tallest = juce::jmax(tallest, page->layout(designW_ - 30)); widest = juce::jmax(widest, page->minimumWidth()); }
    for (auto& page : percPages_) if (page != nullptr) { tallest = juce::jmax(tallest, page->layout(designW_ - 30)); widest = juce::jmax(widest, page->minimumWidth()); }
    designW_ = juce::jmax(designW_, widest + 30);
    designH_ = juce::jlimit(760, 1200, tallest + 64 + kOverviewH + kTabRowH + kSubRowH + 24);

    setResizable(true, true);
    if (auto* con = getConstrainer()) {
        // 23.09.2026: no fixed aspect ratio any more. JUCE's standalone window sizes the editor through this
        // constrainer, so a fixed ratio kept a maximised or full-screen window from filling the screen; the body
        // is fitted and centred instead (resized()), which works for any shape.
        con->setSizeLimits(designW_ / 3, designH_ / 3, designW_ * 4, designH_ * 4);
    }
    float fit = 1.0f;
    if (auto* screen = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) {
        const juce::Rectangle<float> area = screen->userBounds;
        fit = juce::jlimit(0.4f, 1.0f, juce::jmin(area.getWidth() * 0.94f / designW_, area.getHeight() * 0.90f / designH_));
    }
    setSize(juce::roundToInt(designW_ * fit), juce::roundToInt(designH_ * fit));

    const int startTab = juce::SystemStats::getEnvironmentVariable("PHOS_TAB", "0").getIntValue();
    setTab(juce::jlimit(0, tabNames().size() - 1, startTab));
    // PHOS_HELP=1: open with the help page up (for its picture in the manual and for checking it by eye).
    if (juce::SystemStats::getEnvironmentVariable("PHOS_HELP", "").isNotEmpty()) showHelp(true);
    // PHOS_SHOT_HEADSET=1: the headset's controls in the pictures, hands as if one sent them.
    if (juce::SystemStats::getEnvironmentVariable("PHOS_SHOT_HEADSET", "").isNotEmpty()) {
        frame::Hands hands;
        hands.height[0] = 0.62f;
        hands.height[1] = 0.8f;
        hands.tracked[0] = hands.tracked[1] = true;
        proc_.headset().inject(hands);
        shotHands_ = true;
    }
    frame::keepKeysForEditor(content_);
    runScreenshotMode();
    startTimerHz(12);
}

PhospheneEditor::~PhospheneEditor()
{
    frame::Settings::of("Phosphene").removeChangeListener(this);
    viewport_.setViewedComponent(nullptr, false);
    setLookAndFeel(nullptr);
}

namespace {
/** @brief Which part a generator tab shows in its pattern preview; Count = none. */
Part partOfTab(int tab)
{
    switch (tab) {
    case TabKick: return Part::Kick;
    case TabBass: return Part::Bass;
    case TabPerc: return Part::Perc;
    case TabAcid: return Part::Acid;
    case TabLead:    return Part::Lead;
    case TabCounter: return Part::Counter;
    case TabArp:     return Part::Arp;
    case TabStab:    return Part::Stab;
    case TabPad:     return Part::Pad;
    case TabDrone:   return Part::Drone;
    case TabFx:   return Part::Sfx;
    case TabField: return Part::Field;
    default: return Part::Count;
    }
}
} // namespace

void PhospheneEditor::buildPages()
{
    pages_.resize(static_cast<size_t>(tabNames().size()));
    patterns_.assign(static_cast<size_t>(tabNames().size()), nullptr);
    for (int t = 0; t < tabNames().size(); ++t) {
        if (t == TabPerc) continue;   // percussion: one page per lane, built below
        auto page = std::make_unique<ControlPage>();
        const juce::Colour tint = partColour(t);
        switch (t) {
        case TabSet: break;       // built in EditorSetTab.cpp, after this loop
        case TabArrange: break;   // built in EditorArrange.cpp
        case TabPerform: break;   // built in EditorPerform.cpp
        case TabGallery: break;   // built in EditorGallery.cpp
        case TabExport: break;    // built in EditorSetTab.cpp (01.10.2026)
        case TabKick: case TabBass: {
            // 24.09.2026, the user: "Koennten wir bei der Kick und beim Bass noch Anzeigen einbauen, wie in (Kick 3 von
            // Sonic Academy)" -- the scope (EditorScope.h) beside the sound group, the knobs under them.
            const bool kickTab = t == TabKick;
            addSoundGroup(*page, kickTab ? Module::Kick : Module::Bass, 0, kickTab ? 0 : 1, tint);
            auto scope = std::make_unique<phosui::SynthScope>(proc_, kickTab ? phosui::SynthScope::Kind::Kick : phosui::SynthScope::Kind::Bass);
            (kickTab ? kickScope_ : bassScope_) = scope.get();
            const int g = page->addGroup(kickTab ? "Kick Scope" : "Bass Scope", tint, 12);
            page->addControl(g, std::move(scope), "", 12, true, 3);
            if (kickTab) addSections(*page, proc_, Module::Kick, 0, kKickSections, tint);
            else addSections(*page, proc_, Module::Bass, 0, kBassSections, tint);
            break;
        }
        case TabAcid: addSoundGroup(*page, Module::Acid, 0, 2, tint); addSections(*page, proc_, Module::Acid, 0, kAcidSections, tint); break;
        case TabLead: case TabCounter: case TabArp: case TabStab: case TabPad: case TabDrone:
            // The six voice pages share one table; the tab order is the instance order (PluginEditor.h).
            addSoundGroup(*page, Module::Poly, t - TabLead, 3 + t - TabLead, tint);
            addSections(*page, proc_, Module::Poly, t - TabLead, kPolySections, tint);
            break;
        case TabFx:
            addSections(*page, proc_, Module::Sfx, 0, kSfxSections, tint, 0, sfx::kFirstPreset);
            addSfxPresetGroup(*page, tint);
            addSections(*page, proc_, Module::Fx, 0, kFxSections, tint);
            // 23.09.2026: the modules of round "fx-psychedelia" had no page (see the bass slices).
            page->addModuleGroup(proc_, Module::PsyFx, 0, "Psy FX", tint, 4);
            page->addModuleGroup(proc_, Module::Texture, 0, "Shamanic Bed", tint, 5);
            page->addModuleGroup(proc_, Module::Vocal, 0, "Voices", tint, 5);
            break;
        case TabField:
            addFieldPresetGroup(*page, tint);
            addSections(*page, proc_, Module::Field, 0, kFieldSections, tint);
            break;
        case TabMix: {
            // 24.09.2026, the user: "Koennen wir im Mixer-Tab Meter fuer das Level fuer die einzelnen Kanalzuege
            // anzeigen anstatt einfacher Drehknoepfe? Vielleicht sogar ganze Channel-Strips?" -- the console
            // (EditorMixer.h) in place of the table slices of mutes and level knobs; the track gain and the
            // sidechain's times, which belong to no strip, beside it.
            auto console = std::make_unique<phosui::MixerConsole>(proc_);
            mixer_ = console.get();
            for (int i = 0; i < mixer_->stripCount(); ++i)
                for (const auto& [comp, id] : mixer_->strip(i).controls()) page->enableMidiLearn(proc_, *comp, id);
            const int g = page->addGroup("Channels", tint, 16);
            page->addControl(g, std::move(console), "", 16, true, 5, mixer_->params());
            const int mb = proc_.params().base(Module::Mix);
            page->addParamsGroup(proc_, "Track & Sidechain", tint, 4,
                                 { mb + mix::TrackGain, mb + mix::DuckAttack, mb + mix::DuckHold, mb + mix::DuckRelease,
                                   mb + mix::DuckReleaseLines, mb + mix::CounterDuck, mb + mix::PadLeadDuck });
            addSlices(*page, proc_, Module::Master, 0, kMasterSlices, tint);
            // The score cues went to the Export tab (01.10.2026), as every generator has them.
            break;
        }
        default: break;
        }
        // Every generator page ends with the notes it is shaping, so the knobs and the pattern are
        // on the same screen (PLAN 8.1).
        if (partOfTab(t) != Part::Count) {
            auto roll = std::make_unique<PatternDisplay>();
            roll->setPart(partOfTab(t));
            patterns_[static_cast<size_t>(t)] = roll.get();
            const int g = page->addGroup("Pattern", tint, 12);
            page->addControl(g, std::move(roll), "", 12, true, 2);
            page->setFillWidth(g);
        }
        pages_[static_cast<size_t>(t)] = std::move(page);
    }
    percPages_.resize(kPercLanes);
    percPatterns_.assign(kPercLanes, nullptr);
    for (int lane = 0; lane < kPercLanes; ++lane) {
        auto page = std::make_unique<ControlPage>();
        addSections(*page, proc_, Module::Perc, lane, kPercSections, partColour(TabPerc));
        // The whole kit on every lane's page: the twelve lanes are one pattern, and only the lit
        // row moves as the lane is changed.
        auto roll = std::make_unique<PatternDisplay>();
        roll->setPart(Part::Perc, lane);
        percPatterns_[static_cast<size_t>(lane)] = roll.get();
        const int g = page->addGroup("Kit pattern", partColour(TabPerc), 12);
        page->addControl(g, std::move(roll), "", 12, true, 3);
        page->setFillWidth(g);
        percPages_[static_cast<size_t>(lane)] = std::move(page);
    }
    buildSetPage();
    buildArrangePage();
    buildPerformPage();
    buildGalleryPage();
    buildExportPage();
}

void PhospheneEditor::fillPresetBox(PresetBox& pb)
{
    pb.presets = factoryPresets(pb.synth, pb.instance);
    for (SoundPreset& u : proc_.userPresets(pb.synth, pb.instance)) pb.presets.push_back(std::move(u));
    pb.box->clear(juce::dontSendNotification);
    // A submenu per group, in the order the groups first appear (the factory's order: oscillators, then the
    // wavetable families; the user's last).
    std::vector<std::string> order;
    std::map<std::string, juce::PopupMenu> menus;
    for (size_t i = 0; i < pb.presets.size(); ++i) {
        const SoundPreset& p = pb.presets[i];
        if (menus.find(p.group) == menus.end()) order.push_back(p.group);
        menus[p.group].addItem(static_cast<int>(i) + 1, p.name);
    }
    for (const std::string& g : order) pb.box->getRootMenu()->addSubMenu(g, menus[g]);
    pb.box->setTextWhenNothingSelected(juce::String(pb.presets.size()) + " presets");
}

void PhospheneEditor::addSoundGroup(ControlPage& page, Module module, int instance, int owner, juce::Colour tint)
{
    // The own-sound switch is a mixer parameter; its group is this one, so it stands next to what it protects.
    // Three cells wide, so the page's first group still stands beside it: the switch and "Save..." on the first
    // row, the chooser across the second.
    const int g = page.addModuleGroup(proc_, Module::Mix, 0, "Sound", tint, 3, mix::KickOwn + owner, 1);
    auto pb = std::make_unique<PresetBox>();
    pb->synth = module;
    pb->instance = instance;
    auto box = std::make_unique<juce::ComboBox>();
    box->setTooltip("Presets in groups. Choosing one sets the synth's knobs and switches Own Sound on, so the "
                    "generator's per-track sound design leaves it as it is. Level, ducking and (on a voice) the high "
                    "pass and the trance gate stay where they are. Undo takes it back.");
    pb->box = box.get();
    PresetBox* raw = pb.get();
    box->onChange = [this, raw] {
        const int id = raw->box->getSelectedId();
        if (id <= 0 || id > static_cast<int>(raw->presets.size())) return;
        proc_.applyPreset(raw->synth, raw->instance, raw->presets[static_cast<size_t>(id - 1)]);
    };
    fillPresetBox(*pb);

    auto save = std::make_unique<juce::TextButton>("Save...");
    save->setTooltip("Saves this synth's knobs as a preset of your own (group \"User\")");
    save->onClick = [this, raw] {
        presetNameDialog_ = std::make_unique<juce::AlertWindow>("Save preset", "A name for this sound:", juce::MessageBoxIconType::NoIcon, this);
        presetNameDialog_->addTextEditor("name", raw->box->getSelectedId() > 0 ? raw->box->getText() : juce::String("My sound"));
        presetNameDialog_->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
        presetNameDialog_->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        presetNameDialog_->enterModalState(true, juce::ModalCallbackFunction::create([this, raw](int result) {
            const juce::String name = presetNameDialog_ != nullptr ? presetNameDialog_->getTextEditorContents("name") : juce::String();
            if (result == 1 && proc_.saveUserPreset(raw->synth, raw->instance, name) != juce::File()) {
                fillPresetBox(*raw);
                for (size_t i = 0; i < raw->presets.size(); ++i)
                    if (raw->presets[i].group == "User" && juce::String(raw->presets[i].name) == juce::File::createLegalFileName(name.trim()))
                        raw->box->setSelectedId(static_cast<int>(i) + 1, juce::dontSendNotification);
            }
        }), false);
    };
    page.addControl(g, std::move(save), "", 2, true);
    page.addControl(g, std::move(box), "Preset", 3);
    presetBoxes_.push_back(std::move(pb));
}

void PhospheneEditor::addSfxPresetGroup(ControlPage& page, juce::Colour tint)
{
    // 24.09.2026, the user: "Im SFX-Fenster ist nach wie vor keine Auswahl fuer das Preset". The effects' presets are
    // the bank's (Sfx.h, SfxPreset): 2048 in eleven families, drawn per event and never twice in a track. A family
    // can now be fixed to one of them, and each can be heard before it is chosen.
    const int g = page.addGroup("Effect Presets", tint, 12);
    const int base = proc_.params().base(Module::Sfx);
    for (int i = 0; i < sfx::kNumPresetChoices; ++i) {
        const int id = base + sfx::kFirstPreset + i;
        StoreParameter* param = proc_.parameterFor(id);
        if (param == nullptr) continue;
        const SfxType type = kPresetChoiceType[i];
        const juce::String family(kSfxTypeNames[static_cast<int>(type)]);
        const int n = kSfxBankCount[static_cast<int>(type)];
        auto box = std::make_unique<juce::ComboBox>();
        // Item i + 1 is preset i (0 = Auto): the attachment maps the parameter onto the item's *position*, and the
        // position counts through the submenus in order (juce_ComboBox.cpp, getItemForIndex).
        box->getRootMenu()->addItem(1, "Auto (drawn per event)");
        for (int from = 1; from <= n; from += 32) {
            juce::PopupMenu sub;
            const int to = juce::jmin(n, from + 31);
            for (int k = from; k <= to; ++k) sub.addItem(k + 1, family + " " + juce::String(k));
            box->getRootMenu()->addSubMenu(family + " " + juce::String(from) + " - " + juce::String(to), sub);
        }
        box->setTooltip(juce::String(proc_.params().key(id)) + ": Auto lets the generator draw a preset of the family for "
                        "every event, never the same twice in a track; a number makes every " + family + " play that one");
        sfxPresetLinks_.push_back(std::make_unique<juce::ComboBoxParameterAttachment>(*param, *box));
        page.enableMidiLearn(proc_, *box, id);
        page.addControl(g, std::move(box), family, 2, false, 1, { id });

        auto play = std::make_unique<juce::TextButton>("Hear");
        play->setTooltip("Plays the chosen " + family + " in the running set (Auto: one of the family at random). "
                         "Nothing sounds while the transport stands.");
        play->onClick = [this, i, id, n] {
            int preset = static_cast<int>(std::lround(proc_.params().get(id)));
            if (preset <= 0) preset = 1 + juce::Random::getSystemRandom().nextInt(juce::jmax(1, n));
            proc_.previewSfx(i, preset);
        };
        page.addControl(g, std::move(play), " ", 1);   // a (blank) name line: the button gets a combo's height, not the cell's
    }
}

void PhospheneEditor::addFieldPresetGroup(ControlPage& page, juce::Colour tint)
{
    // 27.09.2026, the user: "Sollten wir für die vorhandenen Samples dann Presets schreiben, die einfach in den Sampler
    // geladen werden können?" -- the factory presets by category and the scenes; choosing one sets the knobs (one undo
    // step). Layer A's category back on Auto hands the choice to the composer again, a track at a time.
    const int g = page.addGroup("Field Preset & Library", tint, 12);
    auto box = std::make_unique<juce::ComboBox>();
    box->setTextWhenNothingSelected("Load a preset...");
    const std::vector<FieldPreset>& all = fieldPresets();
    std::map<std::string, juce::PopupMenu> groups;
    std::vector<std::string> order;
    for (size_t i = 0; i < all.size(); ++i) {
        const std::string grp = all[i].group;
        if (groups.find(grp) == groups.end()) order.push_back(grp);
        groups[grp].addItem(static_cast<int>(i) + 1, all[i].name);
    }
    // The scenes first: two recordings as one place is what most tracks want.
    std::stable_partition(order.begin(), order.end(), [](const std::string& s) { return s == "Scenes"; });
    for (const std::string& grp : order) box->getRootMenu()->addSubMenu(grp, groups[grp]);
    box->setTooltip("Loads a factory preset into the Field knobs: its recordings, loop, envelopes, filter and space. "
                    "Set A Category back to Auto to let the composer choose a place for every track again.");
    juce::ComboBox* raw = box.get();
    box->onChange = [this, raw] {
        const int id = raw->getSelectedId();
        if (id > 0) proc_.applyFieldPreset(id - 1);
    };
    page.addControl(g, std::move(box), "Preset", 4, false, 1);

    auto status = std::make_unique<juce::Label>();
    status->setJustificationType(juce::Justification::centredLeft);
    status->setMinimumHorizontalScale(0.7f);
    fieldStatus_ = status.get();
    page.addControl(g, std::move(status), "Library", 6, false, 1);

    auto folder = std::make_unique<juce::TextButton>("Folder...");
    folder->setTooltip("Adds a folder of your own field recordings (FLAC, named fr-<category>-*.flac, e.g. Noctuary's "
                       "FieldRecordings) beside the ones Phosphene installs. Cancel the dialog to remove it.");
    folder->onClick = [this] {
        fieldChooser_ = std::make_unique<juce::FileChooser>("Folder of field recordings", juce::File(proc_.fieldFolder()));
        fieldChooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                   [this](const juce::FileChooser& fc) {
                                       proc_.setFieldFolder(fc.getResult() == juce::File() ? juce::String() : fc.getResult().getFullPathName());
                                       refreshFieldStatus();
                                   });
    };
    page.addControl(g, std::move(folder), " ", 2);
    refreshFieldStatus();
}

void PhospheneEditor::refreshFieldStatus()
{
    if (fieldStatus_ == nullptr) return;
    const int n = scanFieldLibrary();
    const std::vector<std::string> folders = fieldLibraryFolders();
    juce::String where;
    for (const std::string& f : folders) where << (where.isEmpty() ? "" : ", ") << juce::String(f);
    // Without the recordings the program works as before: the Field track and the NASA shots are silent (the user:
    // "achte darauf, dass das Programm auch dann funktioniert, wenn die Samples nicht heruntergeladen wurden").
    fieldStatus_->setText(n > 0 ? juce::String(n) + " recordings (" + where + ")"
                                : juce::String("No field recordings installed: the Field track stays silent. Install them with the setup, or add a folder."),
                          juce::dontSendNotification);
    fieldStatus_->setTooltip(fieldStatus_->getText());
}

juce::StringArray PhospheneEditor::parametersOnPages() const
{
    const ParamStore& p = proc_.params();
    juce::StringArray seen;
    auto collect = [&](const phosui::ControlPage* page) {
        if (page == nullptr) return;
        for (int g = 0; g < page->groupCount(); ++g)
            for (int id : page->groupParams(g)) seen.addIfNotAlreadyThere(juce::String(p.key(id)));
    };
    for (auto& page : pages_) collect(page.get());
    for (auto& page : percPages_) collect(page.get());
    return seen;
}

ControlPage* PhospheneEditor::activePage() const
{
    if (tab_ == TabPerc) return percPages_[static_cast<size_t>(juce::jlimit(0, kPercLanes - 1, percLane_))].get();
    return tab_ >= 0 && tab_ < static_cast<int>(pages_.size()) ? pages_[static_cast<size_t>(tab_)].get() : nullptr;
}

void PhospheneEditor::setTab(int index)
{
    if (helpShown()) showHelp(false);
    tab_ = juce::jlimit(0, tabNames().size() - 1, index);
    // The group on top, and the row of its pages under it (01.10.2026).
    const int grp = groupOf(tab_);
    if (grp < static_cast<int>(lastTabOfGroup_.size())) lastTabOfGroup_[static_cast<size_t>(grp)] = tab_;
    for (int i = 0; i < groupTabs_.size(); ++i) groupTabs_[i]->setToggleState(i == grp, juce::dontSendNotification);
    for (int i = 0; i < subButtons_.size(); ++i) {
        const int t = subTab_[static_cast<size_t>(i)];
        subButtons_[i]->setVisible(groupOf(t) == grp);
        subButtons_[i]->setToggleState(t == tab_, juce::dontSendNotification);
    }
    for (int i = 0; i < laneButtons_.size(); ++i) laneButtons_[i]->setVisible(tab_ == TabPerc);
    for (int i = 0; i < laneButtons_.size(); ++i) laneButtons_[i]->setToggleState(i == percLane_, juce::dontSendNotification);
    viewport_.setViewedComponent(activePage(), false);
    layoutContent();
    // Feed the page that has just opened before it is seen for the first time, so a screenshot of
    // a tab shows what it has to say rather than an empty frame waiting for the next tick.
    if (tab_ == TabSet) refreshSetPage();
    else if (tab_ == TabArrange) { arrangeDirty_ = true; refreshArrangePage(); }
    else if (tab_ == TabPerform) refreshPerformPage();
    else if (tab_ == TabGallery && gallery_ != nullptr && gallery_->rowCount() == 0) refreshGalleryPage();
    else if (tab_ == TabExport) refreshExportPage();
    refreshPattern();
    content_.repaint();
}

void PhospheneEditor::setPercLane(int lane)
{
    percLane_ = juce::jlimit(0, kPercLanes - 1, lane);
    for (int i = 0; i < laneButtons_.size(); ++i) laneButtons_[i]->setToggleState(i == percLane_, juce::dontSendNotification);
    viewport_.setViewedComponent(activePage(), false);
    layoutContent();
    content_.repaint();
}

void PhospheneEditor::resized()
{
    // The body is drawn in design coordinates and scaled to the window, so a page never reflows:
    // dragging the corner is a zoom. That is what makes a picture taken at design size the truth
    // about the layout.
    // Fitted, not stretched: the width decides unless the window is wider than the design's shape, and then the
    // height does and the body is centred (a maximised or full-screen window on a wide screen). A taller window
    // shows more of the page.
    const float scale = juce::jmax(0.25f, juce::jmin(static_cast<float>(getWidth()) / designW_,
                                                     static_cast<float>(getHeight()) / juce::jmin(designH_, 760)));
    const float left = juce::jmax(0.0f, (getWidth() - designW_ * scale) * 0.5f);
    content_.setTransform(juce::AffineTransform::scale(scale).translated(left, 0.0f));
    content_.setBounds(0, 0, designW_, juce::roundToInt(getHeight() / scale));
}

void PhospheneEditor::layoutContent()
{
    // The frame's header (Frame.h), the set strip, the row of tabs and the row of a group's pages.
    juce::Rectangle<int> r = content_.getLocalBounds().reduced(10, 0).withTrimmedTop(10);
    frame::Header h;
    h.logo = &logo_;
    h.title = &titleSpot_;
    h.titleWidth = static_cast<int>(frame::titleWidth(phosui::skin(), 21.0f)) + 12;
    h.choices = { { &style_, 124 }, { &key_, 60 }, { &scale_, 150 } };
    h.lengthLabel = &lengthLabel_;
    h.length = &length_;
    h.actions = { { &compose_, 112 }, { &newSeed_, 88 } };
    h.play = &play_;
    h.mute = &mute_;
    h.like = &likeIcon_;
    h.dislike = &dislikeIcon_;
    h.status = &status_;
    h.curation = &where_;
    h.update = &updateButton_;
    h.tools = { &headsetIcon_, nullptr, &undoIcon_, &redoIcon_, nullptr, &helpIcon_, &settingsIcon_ };
    frame::layoutHeader(r, h);
    headerBottom_ = r.getY();
    overview_.setBounds(r.removeFromTop(kOverviewH));
    r.removeFromTop(4);
    tabRow_ = r.removeFromTop(kTabRowH);
    {
        juce::Rectangle<int> row = tabRow_;
        for (auto* b : groupTabs_) b->setBounds(row.removeFromLeft(b->bestWidth()));
    }
    const int grp = groupOf(tab_);
    if (tabsOf(grp).size() > 1) {
        juce::Rectangle<int> sub = r.removeFromTop(kSubRowH).reduced(2, 3);
        for (int i = 0; i < subButtons_.size(); ++i)
            if (groupOf(subTab_[static_cast<size_t>(i)]) == grp)
                subButtons_[i]->setBounds(sub.removeFromLeft(juce::jmax(84, juce::GlyphArrangement::getStringWidthInt(body(13.0f), subButtons_[i]->getButtonText()) + 30)).reduced(2, 0));
    } else if (tab_ == TabPerc) {
        juce::Rectangle<int> lanes = r.removeFromTop(kSubRowH).reduced(2, 3);
        const int lw = juce::jmin(52, lanes.getWidth() / juce::jmax(1, laneButtons_.size()));
        for (auto* b : laneButtons_) b->setBounds(lanes.removeFromLeft(lw).reduced(2, 0));
    }
    viewport_.setBounds(r.reduced(8, 4));
    if (help_ != nullptr) help_->setBounds(viewport_.getBounds());
    if (auto* page = activePage()) {
        const int first = page->layout(viewport_.getWidth());
        if (first > viewport_.getHeight()) page->layout(viewport_.getWidth() - 12);   // room for the scrollbar
    }
}

void PhospheneEditor::paint(juce::Graphics& g) { g.fillAll(bg0); }

void PhospheneEditor::paintContent(juce::Graphics& g)
{
    // The frame's backdrop (strong behind the header, faint behind the pages), the logo and the name.
    const frame::Skin& skin = phosui::skin();
    backdrop_.paint(g, content_.getLocalBounds(), headerBottom_, skin, frame::Settings::of("Phosphene").backdrop());
    if (skin.logo) skin.logo(g, logo_);
    frame::drawTitle(g, skin, titleSpot_.getBounds().toFloat(), 21.0f);
    g.setColour(edge);
    g.fillRect(tabRow_.getX(), tabRow_.getBottom() - 1, tabRow_.getWidth(), 1);
    // The page's background, so a tab reads as a sheet lying on the window (the phosphenes faint through it).
    g.setColour(skin.panel);
    g.fillRoundedRectangle(viewport_.getBounds().toFloat(), 8.0f);
}

bool PhospheneEditor::standalone() const { return findParentComponentOfClass<juce::DocumentWindow>() != nullptr; }

bool PhospheneEditor::fullScreen() const
{
    auto* window = findParentComponentOfClass<juce::DocumentWindow>();
    return window != nullptr && juce::Desktop::getInstance().getKioskModeComponent() == window;
}

void PhospheneEditor::changeListenerCallback(juce::ChangeBroadcaster*) { content_.repaint(); }

void PhospheneEditor::showSettings()
{
    frame::SettingsMenu m;
    m.app = "Phosphene";
    m.version = JucePlugin_VersionString;
    const juce::File state = proc_.userFolder().getChildFile("update.txt");
    m.updatesOn = [state] { return phosui::UpdateCheck::enabled(state); };
    m.setUpdates = [this, state](bool on) {
        phosui::UpdateCheck::setEnabled(state, on);
        if (on) updates_->startIfDue(state, true);
    };
    m.canFullScreen = [this] { return standalone(); };
    m.isFullScreen = [this] { return fullScreen(); };
    m.toggleFullScreen = [this] { toggleFullScreen(); };
    m.setWindowScale = [this](float k) {
        if (!fullScreen()) setSize(juce::roundToInt(static_cast<float>(designW_) * k), juce::roundToInt(static_cast<float>(juce::jmin(designH_, 860)) * k));
    };
    m.headsetStatus = [this] { return proc_.headset().statusText(); };
    m.about = [] { return juce::String("Whole psytrance sets, composed and synthesised.\ngithub.com/reneweller-coding/Phosphene"); };
    m.show(settingsIcon_);
}

void PhospheneEditor::refreshHeader()
{
    const TransportView t = proc_.transport();
    play_.setButtonText(proc_.isPlaying() ? "Stop" : "Play");
    const bool muted = proc_.muted() && !shooting_;   // the pictures are taken muted by design (26.09.2026)
    mute_.setToggleState(muted, juce::dontSendNotification);
    mute_.setButtonText(muted ? (proc_.muteForced() ? "Muted (env)" : "Muted") : "Mute");
    juce::String info;
    info << "seed " << juce::String(proc_.seed()) << "   " << juce::String(t.bpm, 1) << " BPM   " << (t.hostSync ? "host clock" : "own clock")
         << "   " << (t.restarting ? "planning" : (t.playing ? "playing" : "stopped"));
    if (proc_.isRecording()) info << "   recording " << juce::String(proc_.recordedSeconds(), 1) << " s";
    status_.setText(info, juce::dontSendNotification);
    where_.setText("track " + juce::String(t.track + 1) + ", bar " + juce::String(t.barInTrack + 1) + "   (bar " + juce::String(t.bar + 1) + " of the set)",
                   juce::dontSendNotification);
    undoIcon_.setEnabled(proc_.canUndo());
    redoIcon_.setEnabled(proc_.canRedo());
    undoIcon_.setTooltip(proc_.canUndo() ? "Undo: " + proc_.undoName() + " (Ctrl+Z)" : juce::String("Nothing to undo"));
    redoIcon_.setTooltip(proc_.canRedo() ? "Redo: " + proc_.redoName() + " (Ctrl+Y)" : juce::String("Nothing to redo"));
    // The headset (the frame): its sign while one sends (or always, if asked), its group on the Perform tab.
    if (shotHands_) proc_.headset().inject(proc_.headset().hands());
    const bool hs = proc_.headset().shown(frame::Settings::of("Phosphene").headset());
    if (headsetIcon_.isVisible() != hs) {
        headsetIcon_.setVisible(hs);
        if (headsetGroup_ >= 0 && pages_[TabPerform] != nullptr) pages_[TabPerform]->setGroupVisible(headsetGroup_, hs);
        layoutContent();
    }
}

void PhospheneEditor::refreshPattern()
{
    // Only the roll that is on screen is fed; the others are filled the moment their tab opens,
    // which is also what makes a screenshot of a tab show its pattern without waiting for a tick.
    PatternDisplay* roll = tab_ == TabPerc ? percPatterns_[static_cast<size_t>(juce::jlimit(0, kPercLanes - 1, percLane_))]
                                     : (tab_ < static_cast<int>(patterns_.size()) ? patterns_[static_cast<size_t>(tab_)] : nullptr);
    if (roll == nullptr) return;
    const TransportView t = proc_.transport();
    constexpr int bars = 4;
    // The window starts at the bar the engine is in, rounded down to a group of four, so the
    // picture does not slide sideways at every tick.
    const int firstBar = juce::jmax(0, (t.bar / bars) * bars);
    if (proc_.readPattern(firstBar, bars, patternNotes_)) roll->update(patternNotes_, firstBar, bars, t.musicalBeat);
    else roll->update({}, firstBar, bars, t.musicalBeat);
}

bool PhospheneEditor::keyPressed(const juce::KeyPress& key)
{
    // The keys every generator has (Frame.h): Space, Ctrl+Z / Ctrl+Y, F1, F11, Esc, Ctrl+S / Ctrl+O / Ctrl+E.
    frame::Keys k;
    if (standalone()) {   // in a host, Space and F11 are the host's
        k.playStop = [this] { if (proc_.isPlaying()) proc_.stop(); else proc_.play(); };
        k.fullScreen = [this] { toggleFullScreen(); };
    }
    k.undo = [this] { proc_.undo(); };
    k.redo = [this] { proc_.redo(); };
    k.help = [this] { showHelp(!helpShown()); };
    k.escape = [this] {
        if (helpShown()) showHelp(false);
        else if (fullScreen()) toggleFullScreen();
    };
    k.save = [this] { setTab(TabExport); if (exportSet_ != nullptr) exportSet_->triggerClick(); };
    k.open = [this] { setTab(TabExport); if (loadSet_ != nullptr) loadSet_->triggerClick(); };
    k.exportFile = [this] { setTab(TabExport); if (exportMidi_ != nullptr) exportMidi_->triggerClick(); };
    return frame::handleKey(key, k);
}

void PhospheneEditor::timerCallback()
{
    // The live ring of the page on screen (26.09.2026).
    if (tab_ >= 0 && tab_ < static_cast<int>(pages_.size()) && pages_[static_cast<size_t>(tab_)] != nullptr)
        pages_[static_cast<size_t>(tab_)]->showPlayed(proc_);
    // The composer's preset of each synth, in its chooser (26.09.2026): the name, and "(composer)" to say who chose it.
    for (auto& pb : presetBoxes_) {
        const int k = pb->synth == Module::Kick ? 0 : pb->synth == Module::Bass ? 1 : pb->synth == Module::Acid ? 2 : 3 + pb->instance;
        const int idx = proc_.composerPreset(k);
        const juce::String want = idx >= 0 && idx < static_cast<int>(pb->presets.size())
            ? juce::String(pb->presets[static_cast<size_t>(idx)].group) + ": " + juce::String(pb->presets[static_cast<size_t>(idx)].name) + " (composer)"
            : juce::String();
        if (want.isNotEmpty() && pb->box->getText() != want) pb->box->setText(want, juce::dontSendNotification);
        const int tab = pb->synth == Module::Kick ? TabKick : pb->synth == Module::Bass ? TabBass : pb->synth == Module::Acid ? TabAcid : TabLead + pb->instance;
        // Every synth page, not only the one on screen (26.09.2026): a tab switched to shows the composer's values at once,
        // not a tick later -- the screenshots caught the pad's page with the knobs' values under the preset's name.
        if (pages_[static_cast<size_t>(tab)] != nullptr) {
            const ParamStore& store = proc_.params();
            const std::vector<std::pair<int, float>>* vals =
                idx >= 0 && idx < static_cast<int>(pb->presets.size()) ? &pb->presets[static_cast<size_t>(idx)].values : nullptr;
            pages_[static_cast<size_t>(tab)]->showValues(store, store.base(pb->synth, pb->instance), ParamStore::moduleCount(pb->synth), vals);
        }
    }
    refreshHeader();
    refreshOverview();
    {
        const phosui::UpdateCheck::Result u = updates_->result();
        if (u.newer != updateButton_.isVisible()) {
            updateButton_.setButtonText("Update: Phosphene " + u.latest.trimCharactersAtStart("vV") + " is out");
            updateButton_.setVisible(u.newer);
            layoutContent();
        }
        if (helpShown()) help_->refreshUpdate();
    }
    // Only the page that is on screen is fed. The arrange timeline in particular draws a whole set,
    // and a set that is not being looked at costs nothing at all this way.
    if (tab_ == TabSet) refreshSetPage();
    else if (tab_ == TabArrange) refreshArrangePage();
    else if (tab_ == TabPerform) refreshPerformPage();
    else if (tab_ == TabGallery && gallery_ != nullptr && gallery_->rowCount() == 0) refreshGalleryPage();
    else if (tab_ == TabExport) refreshExportPage();
    refreshPattern();
}

// ---------------------------------------------------------------- screenshots

bool PhospheneEditor::writeScreenshot(const juce::File& file)
{
    const juce::Image img = createComponentSnapshot(getLocalBounds(), true, 1.0f);
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    auto out = std::unique_ptr<juce::FileOutputStream>(file.createOutputStream());
    if (out == nullptr) return false;
    juce::PNGImageFormat png;
    return png.writeImageToStream(img, *out);
}

bool PhospheneEditor::writeFullPage(const juce::File& file)
{
    juce::Component* shown = viewport_.getViewedComponent();
    const juce::Rectangle<int> vp = viewport_.getBounds();   // in content_'s coordinates, which are design units here
    if (shown == nullptr || !viewport_.isVisible() || shown->getHeight() <= vp.getHeight()) return writeScreenshot(file);
    // The header and the tab row as the window shows them, then the page in full below them.
    const juce::Image head = content_.createComponentSnapshot({ 0, 0, designW_, vp.getY() }, true, 1.0f);
    const juce::Image page = shown->createComponentSnapshot(shown->getLocalBounds(), true, 1.0f);
    juce::Image img(juce::Image::ARGB, designW_, vp.getY() + page.getHeight() + 8, true);
    {
        juce::Graphics g(img);
        g.fillAll(bg0);
        g.drawImageAt(head, 0, 0);
        g.drawImageAt(page, vp.getX(), vp.getY());
    }
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    auto out = std::unique_ptr<juce::FileOutputStream>(file.createOutputStream());
    if (out == nullptr) return false;
    juce::PNGImageFormat png;
    return png.writeImageToStream(img, *out);
}

namespace {
/** @brief The file name a tab's picture gets, from the tab's own name. */
juce::String shotName(int index)
{
    return "tab-" + juce::String(index) + "-"
         + PhospheneEditor::tabNames()[index].toLowerCase().replace(" / ", "-").replace(" ", "-") + ".png";
}
/** @brief The name of a curve, for the manual. */
const char* curveName(Curve c)
{
    switch (c) {
    case Curve::Linear: return "linear";
    case Curve::Log: return "log";
    case Curve::Int: return "int";
    case Curve::Choice: return "choice";
    default: return "toggle";
    }
}
} // namespace

bool PhospheneEditor::writeManual(const juce::File& dir)
{
    if (!dir.createDirectory()) return false;
    const ParamStore& p = proc_.params();

    // ---------------------------------------------------------------- the pictures
    for (int i = 0; i < tabNames().size(); ++i) {
        setTab(i);
        setSize(designW_, designH_);
        if (!writeFullPage(dir.getChildFile(shotName(i)))) return false;
    }
    // The signal flow (EditorFlow.cpp) at twice its canvas, for print.
    {
        SignalFlow flow;
        flow.setSize(juce::roundToInt(SignalFlow::kCanvasW * 2.0f), juce::roundToInt(SignalFlow::kCanvasH * 2.0f));
        const juce::Image img = flow.createComponentSnapshot(flow.getLocalBounds(), true, 1.0f);
        const juce::File f = dir.getChildFile("flow.png");
        f.deleteFile();
        auto out = std::unique_ptr<juce::FileOutputStream>(f.createOutputStream());
        if (out == nullptr || !juce::PNGImageFormat().writeImageToStream(img, *out)) return false;
    }

    // ---------------------------------------------------------------- every parameter
    juce::Array<juce::var> params;
    for (int i = 0; i < p.count(); ++i) {
        const ParamDesc& d = p.desc(i);
        auto* o = new juce::DynamicObject();
        const juce::String key(p.key(i));
        o->setProperty("key", key);
        o->setProperty("module", key.upToFirstOccurrenceOf(".", false, false));
        o->setProperty("name", juce::String(d.name));
        o->setProperty("unit", juce::String(d.unit));
        o->setProperty("min", d.minValue);
        o->setProperty("max", d.maxValue);
        o->setProperty("default", p.defaultValue(i));
        o->setProperty("curve", juce::String(curveName(d.curve)));
        if (d.curve == Curve::Choice && d.choices != nullptr) {
            juce::Array<juce::var> choices;
            for (int c = 0; c <= static_cast<int>(d.maxValue); ++c) choices.add(juce::String(d.choices[c]));
            o->setProperty("choices", choices);
        }
        params.add(juce::var(o));
    }

    // ---------------------------------------------------------------- the tabs, as they were built
    auto groupsOf = [&p](const phosui::ControlPage* page) {
        juce::Array<juce::var> out;
        if (page == nullptr) return out;
        for (int g = 0; g < page->groupCount(); ++g) {
            auto* o = new juce::DynamicObject();
            o->setProperty("title", page->groupTitle(g));
            juce::Array<juce::var> keys;
            for (int id : page->groupParams(g)) keys.add(juce::String(p.key(id)));
            o->setProperty("params", keys);
            out.add(juce::var(o));
        }
        return out;
    };
    juce::Array<juce::var> tabs;
    for (int i = 0; i < tabNames().size(); ++i) {
        auto* o = new juce::DynamicObject();
        o->setProperty("index", i);
        o->setProperty("name", tabNames()[i]);
        o->setProperty("image", shotName(i));
        // The percussion tab is twelve pages behind one bar; its table is the same twelve times, so
        // the manual prints lane 1 and says so.
        o->setProperty("groups", groupsOf(i == TabPerc ? percPages_[0].get() : pages_[static_cast<size_t>(i)].get()));
        if (i == TabPerc) o->setProperty("lanes", kPercLanes);
        tabs.add(juce::var(o));
    }

    // Which parameters are on a page somewhere, all twelve percussion lanes included. The generator
    // compares this with the full list; what is in neither is a parameter the editor cannot reach.
    juce::Array<juce::var> shown;
    for (const juce::String& s : parametersOnPages()) shown.add(s);

    // ---------------------------------------------------------------- the macros
    juce::Array<juce::var> macros;
    for (int i = 0; i < kNumMacros; ++i) {
        auto* o = new juce::DynamicObject();
        o->setProperty("name", juce::String(kMacroNames[i]));
        o->setProperty("help", juce::String(kMacroHelp[i]));
        o->setProperty("momentary", macroIsMomentary(static_cast<Macro>(i)));
        juce::Array<juce::var> moves;
        PhospheneProcessor::MacroTarget t[PhospheneProcessor::kMaxMacroTargets];
        const int n = proc_.macroTargets(static_cast<Macro>(i), 1.0f, t);
        for (int k = 0; k < n; ++k) {
            auto* m = new juce::DynamicObject();
            m->setProperty("key", juce::String(p.key(t[k].param)));
            m->setProperty("value", t[k].value);
            m->setProperty("absolute", t[k].absolute);
            moves.add(juce::var(m));
        }
        o->setProperty("moves", moves);
        macros.add(juce::var(o));
    }

    auto* root = new juce::DynamicObject();
    root->setProperty("version", juce::String(JucePlugin_VersionString));
    root->setProperty("name", juce::String(JucePlugin_Name));
    root->setProperty("designWidth", designW_);
    root->setProperty("designHeight", designH_);
    root->setProperty("params", params);
    root->setProperty("tabs", tabs);
    root->setProperty("shown", shown);
    root->setProperty("macros", macros);
    return dir.getChildFile("manual.json").replaceWithText(juce::JSON::toString(juce::var(root), false));
}

void PhospheneEditor::runScreenshotMode()
{
    // PHOS_PLAY=<seconds> [+ PHOS_RECORD=<file.wav>]: start the standalone, play for a while,
    // optionally record what it played, and quit. The dev aid for hearing the live path -- the
    // composer on its own thread, blocks arriving from a real device -- without a hand on the mouse.
    const double playFor = juce::SystemStats::getEnvironmentVariable("PHOS_PLAY", "0").getDoubleValue();
    if (playFor > 0.0) {
        const juce::String wav = juce::SystemStats::getEnvironmentVariable("PHOS_RECORD", "");
        if (wav.isNotEmpty()) proc_.startRecording(juce::File(wav));
        proc_.play();
        juce::Timer::callAfterDelay(static_cast<int>(playFor * 1000.0),
                                    [safe = juce::Component::SafePointer<PhospheneEditor>(this)] {
                                        if (safe != nullptr) { safe->proc_.stopRecording(); safe->proc_.stop(); }
                                        juce::JUCEApplicationBase::quit();
                                    });
        return;
    }
    const juce::String one = juce::SystemStats::getEnvironmentVariable("PHOS_SHOT", "");
    const juce::String all = juce::SystemStats::getEnvironmentVariable("PHOS_SHOT_ALL", "");
    const juce::String man = juce::SystemStats::getEnvironmentVariable("PHOS_MANUAL", "");
    if (one.isEmpty() && all.isEmpty() && man.isEmpty()) return;
    // At design size: the pixels of the picture are then the layout's own measurements.
    setSize(designW_, designH_);
    // Playing, and silent: PHOS_SHOT implies PHOS_MUTE, and the pictures are meant to show the
    // meters and the pattern rolls doing something rather than an instrument that has not started.
    proc_.play();
    // A moment for the composer to plan the first tracks, so the Set tab's list is not "planning..."
    // and the arrange timeline has a set to draw. A track's first plan costs a probe render of about
    // two seconds, so a picture of a nine-track set needs `PHOS_SHOT_WAIT=25`.
    const double wait = juce::jlimit(1.0, 600.0, juce::SystemStats::getEnvironmentVariable("PHOS_SHOT_WAIT", "6").getDoubleValue());
    juce::Timer::callAfterDelay(static_cast<int>(wait * 1000.0),
                                [safe = juce::Component::SafePointer<PhospheneEditor>(this), one, all, man] {
        if (safe == nullptr) return;
        if (all.isNotEmpty()) {
            const juce::File dir(all);
            dir.createDirectory();
            for (int i = 0; i < tabNames().size(); ++i) {
                safe->setTab(i);
                safe->setSize(safe->designW_, safe->designH_);
                safe->writeFullPage(dir.getChildFile(shotName(i)));
            }
        }
        if (man.isNotEmpty()) safe->writeManual(juce::File(man));
        if (one.isNotEmpty()) safe->writeScreenshot(juce::File(one));
        juce::JUCEApplicationBase::quit();
    });
}
