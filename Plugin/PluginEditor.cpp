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
    const char* m_title;   ///< the group's title
    int m_first;           ///< first index in the module table
    int m_count;           ///< how many, -1 = to the end
    int m_columns;         ///< cell units per row
};

/** @brief The mixer and the master. */
// The channels up to the SFX strip, then the sidechain, then the two strips of 19.09.2026 (texture, vocal).
const Slice MasterSlices[] = {
    { "Gain", master::GAIN, 3, 3 }, { "Compressor", master::COMP_THRESHOLD, 5, 5 }, { "Output", master::MONO_BASS, 7, 4 },
    { "Monitor", master::MONITOR, 1, 2 },   // 25.09.2026, the addon's monitoring
};

template <size_t N>
/** @brief Adds one group per slice of a module's parameter table to @p page. */
void addSlices(ControlPage& page, PhospheneProcessor& proc, ModuleE m, int instance, const Slice (&slices)[N], juce::Colour tint)
{
    for (const Slice& s : slices) page.addModuleGroup(proc, m, instance, s.m_title, tint, s.m_columns, s.m_first, s.m_count);
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
/** @brief A parameter of a section and the size of its control. */
struct SizedKey {
    const char* m_key;   ///< the parameter's key inside its module ("cutoff")
    CellSizeE m_size;     ///< the size of its control
};
/** @brief A group of a page: its caption, its width, its parameters. */
struct Section {
    const char* m_title;              ///< the group's caption
    int m_columns;                    ///< its width in normal cells
    std::vector<SizedKey> m_keys;     ///< its parameters, in the order they are placed
};
constexpr CellSizeE S = CellSizeE::SMALL, N = CellSizeE::NORMAL, L = CellSizeE::LARGE;

/** @brief The modulation block's groups (Modulation.h; 26.09.2026), the same on every synth that has it. */
const std::vector<Section> ModSections = {
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
    out.insert(out.end(), ModSections.begin(), ModSections.end());
    out.insert(out.end(), own.begin() + static_cast<std::ptrdiff_t>(std::min(at, own.size())), own.end());
    return out;
}
const std::vector<Section> KickSections = {
    { "Pitch", 6, { { "engine", N }, { "tune", L }, { "pitch_start", N }, { "pitch_end", N }, { "pitch_decay", N } } },
    { "Punch & Body", 5, { { "punch", L }, { "punch_decay", N }, { "amp_decay", N }, { "amp_attack", S }, { "amp_hold", S } } },
    { "Click", 3, { { "click_level", N }, { "click_tone", N }, { "click_decay", N } } },
    { "Drive & Output", 5, { { "level", L }, { "drive", N }, { "clip", N }, { "tone", N }, { "tail_limit", S } } },
};
const std::vector<Section> BassSectionsOwn = {
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
const std::vector<Section> BassSections = withModulation(BassSectionsOwn, 5);   // after Amplifier
const std::vector<Section> AcidSectionsOwn = {
    { "Voice", 4, { { "wave", N }, { "slide_time", N }, { "amp_decay", N } } },
    { "Filter", 6, { { "cutoff", L }, { "resonance", L }, { "env_amount", N }, { "decay", N }, { "accent", N }, { "key_track", S } } },
    { "Squelch & Drive", 5, { { "squelch", N }, { "drive", N }, { "squelch_start", S }, { "squelch_time", S }, { "low_cut", S },
                              { "comb_mix", S }, { "comb_feedback", S }, { "disperse", S }, { "disperse_freq", S } } },
    { "Delay", 4, { { "delay_send", N }, { "delay_feedback", N }, { "delay_left", S }, { "delay_right", S },
                    { "delay_high_pass", S }, { "delay_low_pass", S } } },
    { "Space & Output", 5, { { "level", L }, { "duck", N }, { "room_send", S }, { "plate_send", S }, { "hall_send", S },
                             { "hall_gate", S } } },
};
const std::vector<Section> AcidSections = withModulation(AcidSectionsOwn, 3);   // after Squelch & Drive
const std::vector<Section> PolySectionsOwn = {
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
const std::vector<Section> PolySections = withModulation(PolySectionsOwn, 7);   // after Amplifier
// The Field track's sampler (27.09.2026, FieldPlayer.h): the two layers, the loop, the filter and the envelopes, the
// synths' modulation block after the amplifier, and the strip's space.
const std::vector<Section> FieldSectionsOwn = {
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
const std::vector<Section> FieldSections = withModulation(FieldSectionsOwn, 6);   // after Amplifier
// The effects page: the generator, then the three rooms of the mix guide -- A the room, B the plate, C the hall, each
// with its own filters and pre-delay -- and the returns. The effect presets stand in a group of their own.
const std::vector<Section> SfxSections = {
    { "Effect Generator", 6, { { "level", L }, { "noise", N }, { "resonance", N }, { "brightness", N }, { "impact_decay", N },
                               { "swell_decay", N }, { "vowel", S }, { "width", S }, { "sub_level", S }, { "sub_duck", S },
                               { "wander", S }, { "wander_send", S }, { "room_send", S }, { "plate_send", S }, { "hall_send", S },
                               { "duck", S } } },
};
const std::vector<Section> FxSections = {
    { "Room (A)", 4, { { "room_size", N }, { "room_decay", N }, { "room_damping", S }, { "room_pre_delay", S }, { "room_low_cut", S },
                       { "room_high_cut", S } } },
    { "Plate (B)", 4, { { "plate_size", N }, { "plate_decay", N }, { "plate_damping", S }, { "plate_pre_delay", S },
                        { "plate_low_cut", S }, { "plate_high_cut", S }, { "plate_to_hall", S } } },
    { "Hall (C)", 4, { { "hall_size", N }, { "hall_decay", N }, { "hall_damping", S }, { "hall_pre_delay", S }, { "low_cut", S },
                       { "high_cut", S } } },
    { "Returns", 4, { { "room_return", N }, { "plate_return", N }, { "hall_return", N }, { "return_duck", S },
                      { "return_duck_release", S } } },
};
const std::vector<Section> PercSections = {
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
void addSections(ControlPage& page, PhospheneProcessor& proc, ModuleE m, int instance, const std::vector<Section>& sections,
                 juce::Colour tint, int restFrom = 0, int restTo = -1)
{
    const ParamStore& ps = proc.params();
    const int base = ps.base(m, instance), count = ParamStore::moduleCount(m);
    if (base < 0 || count <= 0) return;
    const juce::String prefix = juce::String(ps.key(base)).upToFirstOccurrenceOf(".", false, false);
    std::vector<char> seen(static_cast<size_t>(count), 0);
    for (const Section& sec : sections) {
        std::vector<SizedParam> params;
        for (const SizedKey& k : sec.m_keys) {
            const int id = ps.find((prefix + "." + k.m_key).toStdString());
            jassert(id >= base && id < base + count);   // a key the table misspells
            if (id < base || id >= base + count || seen[static_cast<size_t>(id - base)] != 0) continue;
            seen[static_cast<size_t>(id - base)] = 1;
            params.push_back({ id, k.m_size });
        }
        if (!params.empty()) page.addSizedGroup(proc, sec.m_title, tint, sec.m_columns, params);
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
    m_reading = r;
    m_comp = compDb;
    m_limit = limitDb;
    m_target = target;
    repaint();
}

void LoudnessDisplay::paint(juce::Graphics& g)
{
    const juce::Rectangle<int> r = getLocalBounds().reduced(4, 3);
    g.setColour(Bg0.withAlpha(0.6f));
    g.fillRoundedRectangle(r.toFloat(), 5.0f);
    juce::Rectangle<int> area = r.reduced(7, 5);
    g.setColour(Dim);
    g.setFont(title(10.0f));
    g.drawText("MASTER", area.removeFromTop(13), juce::Justification::topLeft, false);
    const int rowH = juce::jmax(9, (area.getHeight() - 14) / 3);

    // -50 .. 0 LUFS / dBTP on one scale, so the bars can be read against each other.
    auto bar = [&](juce::Rectangle<int> band, const juce::String& name, float value, juce::Colour colour, float over) {
        juce::Rectangle<int> row = band.reduced(0, 2);
        g.setColour(Faint);
        g.setFont(phosui::body(9.5f));
        g.drawText(name, row.removeFromLeft(38), juce::Justification::centredLeft, false);
        const juce::Rectangle<int> num = row.removeFromRight(50);
        const float t = juce::jlimit(0.0f, 1.0f, (value + 50.0f) / 50.0f);
        g.setColour(Edge);
        g.fillRoundedRectangle(row.toFloat(), 2.5f);
        g.setColour(value > over ? Red : colour);
        g.fillRoundedRectangle(row.withWidth(juce::roundToInt(row.getWidth() * t)).toFloat(), 2.5f);
        g.setColour(Text);
        g.drawText(value <= -99.0f ? juce::String("--") : juce::String(value, 1), num, juce::Justification::centredRight, false);
    };
    bar(area.removeFromTop(rowH), "Short", m_reading.m_shortTerm, Green, m_target + 3.0f);
    bar(area.removeFromTop(rowH), "Integr.", m_reading.m_integrated, Accent, m_target + 2.0f);
    // The limiter's ceiling is -1 dBTP, so exactly -1.0 is the target and not an overshoot.
    bar(area.removeFromTop(rowH), "Peak", m_reading.m_truePeak, Green, -0.9f);
    g.setColour(Faint);
    g.setFont(phosui::body(9.5f));
    g.drawText("comp " + juce::String(m_comp, 1) + " dB   limit " + juce::String(m_limit, 1) + " dB   target "
                   + juce::String(m_target, 1) + " LUFS",
               area.removeFromTop(13), juce::Justification::centredLeft, false);
}

// ==================================================================== TrackDisplay

void TrackDisplay::paint(juce::Graphics& g)
{
    const juce::Rectangle<int> r = getLocalBounds().reduced(4, 3);
    g.setColour(Bg0.withAlpha(0.6f));
    g.fillRoundedRectangle(r.toFloat(), 5.0f);
    juce::Rectangle<int> area = r.reduced(7, 5);
    g.setColour(Dim);
    g.setFont(title(10.0f));
    g.drawText("TRACKS", area.removeFromTop(14), juce::Justification::topLeft, false);
    if (m_rows.empty()) {
        g.setColour(Faint);
        g.setFont(phosui::body(11.5f));
        g.drawText("planning...", area, juce::Justification::centred, false);
        return;
    }
    const int rowH = juce::jlimit(13, 20, area.getHeight() / juce::jmax(1, static_cast<int>(m_rows.size())));
    // Over the DJ overlap (Form.h, DjOverlap) two rows contain the play head: the outgoing track, which
    // owns the bar (kick, bass, key), and the incoming one, whose intro is mixing in. The first is marked
    // as playing; the second more faintly, and it says so.
    bool ownerSeen = false;
    for (const Row& row : m_rows) {
        if (area.getHeight() < rowH) break;
        juce::Rectangle<int> line = area.removeFromTop(rowH);
        const bool inside = m_bar >= row.m_bar && m_bar < row.m_bar + row.m_bars;
        const bool here = inside && !ownerSeen;
        const bool mixingIn = inside && ownerSeen;
        ownerSeen = ownerSeen || inside;
        if (mixingIn) {
            g.setColour(Accent.withAlpha(0.07f));
            g.fillRoundedRectangle(line.toFloat(), 3.0f);
            g.setColour(Accent.withAlpha(0.7f));
            g.setFont(phosui::body(9.5f));
            g.drawText("mixing in, " + juce::String(juce::jmax(0, row.m_bar + DjOverlap - m_bar)) + " bars to the hand-over",
                       line.reduced(6, 0), juce::Justification::centredRight, false);
        }
        if (here) {
            g.setColour(Accent.withAlpha(0.18f));
            g.fillRoundedRectangle(line.toFloat(), 3.0f);
            // How far into the track the play head stands.
            const float t = row.m_bars > 0 ? juce::jlimit(0.0f, 1.0f, static_cast<float>(m_bar - row.m_bar) / row.m_bars) : 0.0f;
            g.setColour(Accent);
            g.fillRect(line.getX() + juce::roundToInt(t * line.getWidth()), line.getY(), 2, line.getHeight());
        }
        g.setColour(here ? Text : Dim);
        g.setFont(phosui::body(11.5f));
        g.drawText(row.m_text, line.reduced(4, 0), juce::Justification::centredLeft, false);
    }
}

void TrackDisplay::mouseDown(const juce::MouseEvent& e)
{
    if (m_rows.empty() || !m_onJump) return;
    const juce::Rectangle<int> area = getLocalBounds().reduced(11, 8).withTrimmedTop(14);
    const int rowH = juce::jlimit(13, 20, area.getHeight() / juce::jmax(1, static_cast<int>(m_rows.size())));
    const int index = (e.y - area.getY()) / juce::jmax(1, rowH);
    if (index >= 0 && index < static_cast<int>(m_rows.size())) m_onJump(m_rows[static_cast<size_t>(index)].m_bar);
}

// ==================================================================== PatternDisplay

void PatternDisplay::update(std::vector<NoteEvent> notes, int firstBar, int bars, double beat)
{
    m_notes = std::move(notes);
    m_firstBar = firstBar;
    m_bars = juce::jmax(1, bars);
    m_beat = beat;
    repaint();
}

void PatternDisplay::paint(juce::Graphics& g)
{
    const juce::Rectangle<int> r = getLocalBounds().reduced(4, 3);
    g.setColour(Bg0.withAlpha(0.6f));
    g.fillRoundedRectangle(r.toFloat(), 5.0f);
    juce::Rectangle<int> area = r.reduced(8, 6);
    g.setColour(Dim);
    g.setFont(title(10.0f));
    g.drawText(juce::String(PartNames[static_cast<int>(m_part)]).toUpperCase() + "   bars "
                   + juce::String(m_firstBar + 1) + " to " + juce::String(m_firstBar + m_bars),
               area.removeFromTop(13), juce::Justification::topLeft, false);
    const juce::Rectangle<float> plot = area.toFloat();
    if (plot.getHeight() < 12.0f) return;

    const double from = static_cast<double>(m_firstBar) * BeatsPerBar;
    const double span = static_cast<double>(m_bars) * BeatsPerBar;
    auto xOf = [&](double beat) { return plot.getX() + static_cast<float>((beat - from) / span) * plot.getWidth(); };

    // The grid: a hairline every sixteenth, brighter on beats, brightest on bars.
    for (int s = 0; s <= m_bars * BeatsPerBar * 4; ++s) {
        const double beat = from + s * 0.25;
        const bool bar = s % (BeatsPerBar * 4) == 0, beatLine = s % 4 == 0;
        g.setColour(bar ? Edge.brighter(0.5f) : (beatLine ? Edge : Edge.withAlpha(0.35f)));
        g.drawVerticalLine(juce::roundToInt(xOf(beat)), plot.getY(), plot.getBottom());
    }

    if (m_notes.empty()) {
        g.setColour(Faint);
        g.setFont(phosui::body(11.0f));
        g.drawText("nothing composed here yet", plot, juce::Justification::centred, false);
    } else if (m_part == PartE::PERC) {
        // Twelve lanes, named by the role each lane plays; the lane whose knobs are on screen is lit.
        const float rowH = plot.getHeight() / PercLanes;
        for (int lane = 0; lane < PercLanes; ++lane) {
            const juce::Rectangle<float> row(plot.getX(), plot.getY() + lane * rowH, plot.getWidth(), rowH);
            if (lane == m_lane) {
                g.setColour(Accent.withAlpha(0.10f));
                g.fillRect(row);
            }
            g.setColour(Faint.withAlpha(0.6f));
            g.setFont(phosui::body(juce::jlimit(7.0f, 9.5f, rowH * 0.7f)));
            g.drawText(juce::String(lane + 1), row.withWidth(14.0f), juce::Justification::centredRight, false);
        }
        for (const NoteEvent& e : m_notes) {
            if (e.m_part != PartE::PERC || e.m_lane >= PercLanes) continue;
            const juce::Rectangle<float> row(xOf(e.m_beat), plot.getY() + e.m_lane * rowH + 1.5f,
                                             juce::jmax(3.0f, xOf(e.m_beat + 0.22) - xOf(e.m_beat)), rowH - 3.0f);
            g.setColour((e.m_lane == m_lane ? Accent : partColour(TAB_PERC)).withAlpha(0.35f + 0.65f * e.m_velocity / 127.0f));
            g.fillRoundedRectangle(row, 1.5f);
        }
    } else {
        // A piano roll over whatever pitches this part uses, with two semitones of air.
        int lo = 127, hi = 0;
        for (const NoteEvent& e : m_notes) if (e.m_part == m_part) { lo = juce::jmin(lo, static_cast<int>(e.m_pitch)); hi = juce::jmax(hi, static_cast<int>(e.m_pitch)); }
        if (lo > hi) {
            g.setColour(Faint);
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
            g.setColour(Edge.withAlpha(0.25f));
            g.fillRect(plot.getX(), plot.getBottom() - (p - lo + 1) * rowH, plot.getWidth(), rowH);
        }
        for (const NoteEvent& e : m_notes) {
            if (e.m_part != m_part) continue;
            const float y = plot.getBottom() - (e.m_pitch - lo + 1) * rowH;
            // A note struck before the window (a held chord) starts at its left edge, one that outlasts it ends at
            // the right one.
            const float x0 = juce::jmax(plot.getX(), xOf(e.m_beat)), x1 = juce::jmin(plot.getRight(), xOf(e.m_beat + e.m_length));
            const juce::Rectangle<float> box(x0, y + 0.5f, juce::jmax(3.0f, x1 - x0 - 1.0f), juce::jmax(2.0f, rowH - 1.0f));
            const juce::Colour c = partColourOf(static_cast<int>(m_part));
            g.setColour((e.m_flags & NOTE_ACCENT) ? c.brighter(0.5f) : c.withAlpha(0.45f + 0.55f * e.m_velocity / 127.0f));
            g.fillRoundedRectangle(box, 1.5f);
            // A slide is drawn as a line into the next note, which is what it does.
            if (e.m_flags & NOTE_SLIDE) {
                g.setColour(c.withAlpha(0.7f));
                g.drawLine(box.getRight(), box.getCentreY(), box.getRight() + 5.0f, box.getCentreY(), 1.2f);
            }
        }
    }

    // Where the engine is. It sits behind the composed bars by the horizon the rings are kept at,
    // so most of the time the line stands at the very left of the window the display is showing.
    if (m_beat >= from && m_beat <= from + span) {
        g.setColour(Accent.withAlpha(0.85f));
        g.drawVerticalLine(juce::roundToInt(xOf(m_beat)), plot.getY(), plot.getBottom());
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
        { "Set", { TAB_SET } }, { "Arrange", { TAB_ARRANGE } }, { "Low End", { TAB_KICK, TAB_BASS } }, { "Percussion", { TAB_PERC } },
        { "Acid", { TAB_ACID } }, { "Synths", { TAB_LEAD, TAB_COUNTER, TAB_ARP, TAB_STAB, TAB_PAD, TAB_DRONE } },
        { "Effects", { TAB_FX, TAB_FIELD } }, { "Mixer", { TAB_MIX } }, { "Perform", { TAB_PERFORM } }, { "Export", { TAB_EXPORT } },
        { "Gallery", { TAB_GALLERY } },
    };
    return groups;
}
constexpr int OverviewH = 74;   ///< the set strip under the header
constexpr int TabRowH = 32;     ///< the row of tabs
constexpr int SubRowH = 30;     ///< the row of a group's pages (or the percussion lanes)
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

PhospheneEditor::PhospheneEditor(PhospheneProcessor& p) : juce::AudioProcessorEditor(&p), m_proc(p)
{
    setLookAndFeel(&m_lnf);
    m_content.m_onPaint = [this](juce::Graphics& g) { paintContent(g); };
    m_content.m_onResized = [this] { layoutContent(); };
    addAndMakeVisible(m_content);
    m_viewport.setScrollBarsShown(true, false);
    m_viewport.setViewedComponent(nullptr, false);
    m_content.addAndMakeVisible(m_viewport);

    // The row of tabs (01.10.2026): one per group, drawn as every generator's; a group of several pages shows them in a
    // row of its own under it, each in its family's colour (PhospheneLookAndFeel).
    m_lastTabOfGroup.assign(tabGroups().size(), 0);
    for (int g = 0; g < static_cast<int>(tabGroups().size()); ++g) {
        m_lastTabOfGroup[static_cast<size_t>(g)] = tabsOf(g).front();
        auto* b = m_groupTabs.add(new frame::FlatTab(groupNames()[g]));
        b->onClick = [this, g] { setTab(m_lastTabOfGroup[static_cast<size_t>(g)]); };
        m_content.addAndMakeVisible(b);
        if (tabsOf(g).size() < 2) continue;
        for (int t : tabsOf(g)) {
            auto* s = m_subButtons.add(new juce::TextButton(tabNames()[t]));
            s->setClickingTogglesState(false);
            s->getProperties().set("stripe", static_cast<juce::int64>(partColour(t).getARGB()));
            s->onClick = [this, t] { setTab(t); };
            m_subTab.push_back(t);
            m_content.addChildComponent(s);
        }
    }
    // The frame's header (Frame.h): where the set starts and how long its arc is, Compose set, New seed, Play, Mute; the
    // ratings, the status and where the set is; undo, redo, help and the settings.
    {
        const ParamStore& ps = m_proc.params();
        const int cb = ps.base(ModuleE::COMPOSE);
        auto combo = [&](juce::ComboBox& box, int id, const char* tip) {
            const ParamDesc& d = ps.desc(id);
            for (int k = 0; d.m_choices != nullptr && k <= static_cast<int>(d.m_maxValue); ++k) box.addItem(d.m_choices[k], k + 1);
            m_headerLinks.push_back(std::make_unique<juce::ComboBoxParameterAttachment>(*m_proc.parameterFor(id), box));
            box.setTooltip(tip);
            m_content.addAndMakeVisible(box);
        };
        combo(m_style, cb + compose::STYLE, "The style the set starts in (Style Mix on the Set tab lets it walk)");
        combo(m_key, cb + compose::KEY, "The key the set starts in (it walks from track to track)");
        combo(m_scale, cb + compose::SCALE, "The scale the set starts in");
        m_length.setSliderStyle(juce::Slider::LinearHorizontal);
        m_length.setTextBoxStyle(juce::Slider::TextBoxRight, false, 62, 20);
        m_lengthLink = std::make_unique<juce::SliderParameterAttachment>(*m_proc.parameterFor(cb + compose::SET_MINUTES), m_length);
        m_length.setTextValueSuffix(" min");
        m_length.setTooltip("The set's length: the time its energy arc spans (the set plays on after it)");
        m_lengthLabel.setText("Set", juce::dontSendNotification);
        m_lengthLabel.setColour(juce::Label::textColourId, Dim);
        m_lengthLabel.setJustificationType(juce::Justification::centredRight);
        m_content.addAndMakeVisible(m_length);
        m_content.addAndMakeVisible(m_lengthLabel);
    }
    m_compose.setTooltip("Plans the set again with the knobs as they stand -- its seed, locks and rerolls kept -- and starts it "
                        "from the beginning");
    m_compose.onClick = [this] { m_proc.composeSet(); m_arrangeDirty = true; };
    m_newSeed.setTooltip("A new seed: another night from the same knobs");
    m_newSeed.onClick = [this] {
        m_proc.undoable("New seed", [this] { m_proc.randomiseSeed(); });
        if (m_seedEditor != nullptr) m_seedEditor->setText(juce::String(m_proc.seed()), false);
        m_trackRows.clear();
        m_rowsSeed = 0;
        m_arrangeDirty = true;
    };
    m_play.setTooltip("Play, and stop (Space)");
    m_play.onClick = [this] { if (m_proc.isPlaying()) m_proc.stop(); else m_proc.play(); };
    m_mute.setClickingTogglesState(true);
    m_mute.setToggleState(m_proc.muted(), juce::dontSendNotification);
    m_mute.setEnabled(!m_proc.muteForced());
    m_mute.setColour(juce::TextButton::buttonOnColourId, Red.withAlpha(0.55f));
    m_mute.setTooltip(m_proc.muteForced() ? "Muted by PHOS_MUTE: an automated run makes no sound" : "Silence the output");
    m_mute.onClick = [this] { m_proc.setMuted(m_mute.getToggleState()); };
    m_play.setColour(juce::TextButton::buttonColourId, Accent.withAlpha(0.22f));
    m_compose.setColour(juce::TextButton::buttonColourId, Accent.withAlpha(0.12f));
    for (juce::Component* comp : { static_cast<juce::Component*>(&m_compose), static_cast<juce::Component*>(&m_newSeed),
                                   static_cast<juce::Component*>(&m_play), static_cast<juce::Component*>(&m_mute),
                                   static_cast<juce::Component*>(&m_status), static_cast<juce::Component*>(&m_where),
                                   static_cast<juce::Component*>(&m_titleSpot) })
        m_content.addAndMakeVisible(comp);
    m_titleSpot.setInterceptsMouseClicks(false, false);
    m_status.setColour(juce::Label::textColourId, Text);
    m_status.setMinimumHorizontalScale(0.8f);
    m_where.setColour(juce::Label::textColourId, Dim);
    m_where.setJustificationType(juce::Justification::centredRight);
    // The tools; their names are what the host test presses by (Tests/hosttest.cpp), their tooltips say the keys.
    m_undoIcon.setButtonText("Undo");
    m_redoIcon.setButtonText("Redo");
    m_helpIcon.setButtonText("Help");
    m_settingsIcon.setButtonText("Settings ...");
    m_likeIcon.setButtonText("Good here");
    m_dislikeIcon.setButtonText("Bad here");
    m_undoIcon.onClick = [this] { m_proc.undo(); };
    m_redoIcon.onClick = [this] { m_proc.redo(); };
    m_helpIcon.onClick = [this] { showHelp(!helpShown()); };
    m_settingsIcon.onClick = [this] { showSettings(); };
    m_headsetIcon.onClick = [this] { setTab(TAB_PERFORM); };
    m_likeIcon.onClick = [this] { rateNow(1); };
    m_dislikeIcon.onClick = [this] { rateNow(-1); };
    m_search.onClick = [this] { openPalette(); };
    m_compare.onStateChange = [this] { if (m_compare.isDown() != m_comparing) compare(m_compare.isDown()); };
    for (auto* b : { &m_undoIcon, &m_redoIcon, &m_helpIcon, &m_settingsIcon, &m_likeIcon, &m_dislikeIcon, &m_search, &m_compare }) m_content.addAndMakeVisible(b);
    m_content.addChildComponent(m_headsetIcon);
    // The set as a strip under the header (the Arrange tab's top, to scale): a click on a track jumps to it.
    m_overview.setStripOnly(true);
    m_overview.m_onSeek = [this](int bar) { m_proc.seekToBar(bar); };
    m_content.addAndMakeVisible(m_overview);
    frame::Settings::of("Phosphene").addChangeListener(this);
    m_updateButton.setTooltip("Open the release page");
    m_updateButton.onClick = [this] {
        const juce::String url = m_updates->result().m_url;
        if (url.isNotEmpty()) juce::URL(url).launchInDefaultBrowser();
    };
    m_content.addChildComponent(m_updateButton);
    m_help = std::make_unique<HelpView>(m_proc);
    m_content.addChildComponent(*m_help);
    // The family's ways around the panel (15.10.2026, Frame.h): the info line at the bottom, and over everything while
    // open the command palette, the preset browser and the first steps.
    m_info = std::make_unique<frame::InfoLine>(phosui::skin(), m_content);
    m_info->m_describe = [this](int id, juce::String& name, juce::String& text) { return describeControl(id, name, text); };
    m_content.addAndMakeVisible(*m_info);
    m_palette = std::make_unique<frame::CommandPalette>(phosui::skin());
    m_browser = std::make_unique<frame::PresetBrowser>(phosui::skin(), "Phosphene");
    m_tour = std::make_unique<frame::Tour>(phosui::skin());
    m_tour->m_onDone = [] { frame::Settings::of("Phosphene").setValue("tourDone", "1"); };
    for (juce::Component* c : { static_cast<juce::Component*>(m_palette.get()), static_cast<juce::Component*>(m_browser.get()), static_cast<juce::Component*>(m_tour.get()) })
        m_content.addChildComponent(c);
    {
        frame::ModAccess a = frame::slotMatrix(m_proc.params(),
                                               [this](int id, float v) {
                                                   m_proc.takeOver(id);
                                                   if (StoreParameter* p = m_proc.parameterFor(id)) p->setValueNotifyingHost(m_proc.params().toNormalised(id, v));
                                               },
                                               [this](const juce::String& what) { m_proc.beginStep(what); }, [this] { m_proc.endStep(); }, phosui::skin());
        a.m_say = [this](const juce::String& t) { if (m_info != nullptr) m_info->showText(t); };
        m_modulation = std::make_unique<frame::ModulationAtKnob>(std::move(a));
    }
    // Not while the editor is photographing itself for the manual: that run asks nobody anything.
    const bool shooting = juce::SystemStats::getEnvironmentVariable("PHOS_SHOT", "").isNotEmpty()
                       || juce::SystemStats::getEnvironmentVariable("PHOS_SHOT_ALL", "").isNotEmpty()
                       || juce::SystemStats::getEnvironmentVariable("PHOS_MANUAL", "").isNotEmpty()
                       || juce::SystemStats::getEnvironmentVariable("PHOS_PAGE_REPORT", "").isNotEmpty();
    m_shooting = shooting;
    if (!shooting) m_updates->startIfDue(m_proc.userFolder().getChildFile("update.txt"));
    setWantsKeyboardFocus(true);
    for (int i = 0; i < PercLanes; ++i) {
        auto* b = m_laneButtons.add(new juce::TextButton(juce::String(i + 1)));
        b->onClick = [this, i] { setPercLane(i); };
        m_content.addChildComponent(b);
    }
    buildPages();
    // The modulation at the knob on every page (15.10.2026): the voices' matrix rows on their knobs.
    for (auto& page : m_pages) if (page != nullptr) page->m_modulation = m_modulation.get();
    for (auto& page : m_percPages) if (page != nullptr) page->m_modulation = m_modulation.get();

    // The design size is the size at which no page has to scroll; the window opens scaled to fit
    // the screen, and dragging its corner only zooms (the aspect ratio is fixed).
    int tallest = 0, widest = 0;
    for (auto& page : m_pages) if (page != nullptr) { tallest = juce::jmax(tallest, page->layout(m_designW - 30)); widest = juce::jmax(widest, page->minimumWidth()); }
    for (auto& page : m_percPages) if (page != nullptr) { tallest = juce::jmax(tallest, page->layout(m_designW - 30)); widest = juce::jmax(widest, page->minimumWidth()); }
    // The family's window size (01.10.2026): 1280 x 860 -- a page taller than that is split into sections (Sound |
    // Modulation) or scrolls; the content was laid out at the tallest page's height before, up to 1200.
    (void) tallest;
    m_designW = juce::jmax(m_designW, widest + 30);
    m_designH = 860;

    setResizable(true, true);
    if (auto* con = getConstrainer()) {
        // 23.09.2026: no fixed aspect ratio any more. JUCE's standalone window sizes the editor through this
        // constrainer, so a fixed ratio kept a maximised or full-screen window from filling the screen; the body
        // is fitted and centred instead (resized()), which works for any shape.
        con->setSizeLimits(m_designW / 3, m_designH / 3, m_designW * 4, m_designH * 4);
    }
    float fit = 1.0f;
    if (auto* screen = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) {
        const juce::Rectangle<float> area = screen->userBounds;
        fit = juce::jlimit(0.4f, 1.0f, juce::jmin(area.getWidth() * 0.94f / m_designW, area.getHeight() * 0.90f / m_designH));
    }
    setSize(juce::roundToInt(m_designW * fit), juce::roundToInt(m_designH * fit));

    const int startTab = juce::SystemStats::getEnvironmentVariable("PHOS_TAB", "0").getIntValue();
    setTab(juce::jlimit(0, tabNames().size() - 1, startTab));
    // PHOS_HELP=1: open with the help page up (for its picture in the manual and for checking it by eye).
    if (juce::SystemStats::getEnvironmentVariable("PHOS_HELP", "").isNotEmpty()) showHelp(true);
    // PHOS_SHOT_HEADSET=1: the headset's controls in the pictures, hands as if one sent them.
    if (juce::SystemStats::getEnvironmentVariable("PHOS_SHOT_HEADSET", "").isNotEmpty()) {
        frame::Hands hands;
        hands.m_height[0] = 0.62f;
        hands.m_height[1] = 0.8f;
        hands.m_tracked[0] = hands.m_tracked[1] = true;
        m_proc.headset().inject(hands);
        m_shotHands = true;
    }
    frame::keepKeysForEditor(m_content);
    runScreenshotMode();
    startTimerHz(12);
}

PhospheneEditor::~PhospheneEditor()
{
    frame::Settings::of("Phosphene").removeChangeListener(this);
    m_viewport.setViewedComponent(nullptr, false);
    setLookAndFeel(nullptr);
}

namespace {
/** @brief Which part a generator tab shows in its pattern preview; Count = none. */
PartE partOfTab(int tab)
{
    switch (tab) {
    case TAB_KICK: return PartE::KICK;
    case TAB_BASS: return PartE::BASS;
    case TAB_PERC: return PartE::PERC;
    case TAB_ACID: return PartE::ACID;
    case TAB_LEAD:    return PartE::LEAD;
    case TAB_COUNTER: return PartE::COUNTER;
    case TAB_ARP:     return PartE::ARP;
    case TAB_STAB:    return PartE::STAB;
    case TAB_PAD:     return PartE::PAD;
    case TAB_DRONE:   return PartE::DRONE;
    case TAB_FX:   return PartE::SFX;
    case TAB_FIELD: return PartE::FIELD;
    default: return PartE::COUNT;
    }
}
} // namespace

void PhospheneEditor::buildPages()
{
    m_pages.resize(static_cast<size_t>(tabNames().size()));
    m_patterns.assign(static_cast<size_t>(tabNames().size()), nullptr);
    for (int t = 0; t < tabNames().size(); ++t) {
        if (t == TAB_PERC) continue;   // percussion: one page per lane, built below
        auto page = std::make_unique<ControlPage>();
        const juce::Colour tint = partColour(t);
        switch (t) {
        case TAB_SET: break;       // built in EditorSetTab.cpp, after this loop
        case TAB_ARRANGE: break;   // built in EditorArrange.cpp
        case TAB_PERFORM: break;   // built in EditorPerform.cpp
        case TAB_GALLERY: break;   // built in EditorGallery.cpp
        case TAB_EXPORT: break;    // built in EditorSetTab.cpp (01.10.2026)
        case TAB_KICK: case TAB_BASS: {
            // 24.09.2026, the user: "Koennten wir bei der Kick und beim Bass noch Anzeigen einbauen, wie in (Kick 3 von
            // Sonic Academy)" -- the scope (EditorScope.h) beside the sound group, the knobs under them.
            const bool kickTab = t == TAB_KICK;
            addSoundGroup(*page, kickTab ? ModuleE::KICK : ModuleE::BASS, 0, kickTab ? 0 : 1, tint);
            auto scope = std::make_unique<phosui::SynthScope>(m_proc, kickTab ? phosui::SynthScope::KindE::KICK : phosui::SynthScope::KindE::BASS);
            (kickTab ? m_kickScope : m_bassScope) = scope.get();
            const int g = page->addGroup(kickTab ? "Kick Scope" : "Bass Scope", tint, 12);
            page->addControl(g, std::move(scope), "", 12, true, 3);
            if (kickTab) addSections(*page, m_proc, ModuleE::KICK, 0, KickSections, tint);
            else addSections(*page, m_proc, ModuleE::BASS, 0, BassSections, tint);
            break;
        }
        case TAB_ACID: addSoundGroup(*page, ModuleE::ACID, 0, 2, tint); addSections(*page, m_proc, ModuleE::ACID, 0, AcidSections, tint); break;
        case TAB_LEAD: case TAB_COUNTER: case TAB_ARP: case TAB_STAB: case TAB_PAD: case TAB_DRONE:
            // The six voice pages share one table; the tab order is the instance order (PluginEditor.h).
            addSoundGroup(*page, ModuleE::POLY, t - TAB_LEAD, 3 + t - TAB_LEAD, tint);
            addSections(*page, m_proc, ModuleE::POLY, t - TAB_LEAD, PolySections, tint);
            break;
        case TAB_FX:
            addSections(*page, m_proc, ModuleE::SFX, 0, SfxSections, tint, 0, sfx::FirstPreset);
            addSfxPresetGroup(*page, tint);
            addSections(*page, m_proc, ModuleE::FX, 0, FxSections, tint);
            // 23.09.2026: the modules of round "fx-psychedelia" had no page (see the bass slices).
            page->addModuleGroup(m_proc, ModuleE::PSY_FX, 0, "Psy FX", tint, 4);
            page->addModuleGroup(m_proc, ModuleE::TEXTURE, 0, "Shamanic Bed", tint, 5);
            page->addModuleGroup(m_proc, ModuleE::VOCAL, 0, "Voices", tint, 5);
            break;
        case TAB_FIELD:
            addFieldPresetGroup(*page, tint);
            addSections(*page, m_proc, ModuleE::FIELD, 0, FieldSections, tint);
            break;
        case TAB_MIX: {
            // 24.09.2026, the user: "Koennen wir im Mixer-Tab Meter fuer das Level fuer die einzelnen Kanalzuege
            // anzeigen anstatt einfacher Drehknoepfe? Vielleicht sogar ganze Channel-Strips?" -- the console
            // (EditorMixer.h) in place of the table slices of mutes and level knobs; the track gain and the
            // sidechain's times, which belong to no strip, beside it.
            auto console = std::make_unique<phosui::MixerConsole>(m_proc);
            m_mixer = console.get();
            for (int i = 0; i < m_mixer->stripCount(); ++i)
                for (const auto& [comp, id] : m_mixer->strip(i).controls()) page->enableMidiLearn(m_proc, *comp, id);
            const int g = page->addGroup("Channels", tint, 16);
            page->addControl(g, std::move(console), "", 16, true, 5, m_mixer->params());
            const int mb = m_proc.params().base(ModuleE::MIX);
            page->addParamsGroup(m_proc, "Track & Sidechain", tint, 4,
                                 { mb + mix::TRACK_GAIN, mb + mix::DUCK_ATTACK, mb + mix::DUCK_HOLD, mb + mix::DUCK_RELEASE,
                                   mb + mix::DUCK_RELEASE_LINES, mb + mix::COUNTER_DUCK, mb + mix::PAD_LEAD_DUCK });
            addSlices(*page, m_proc, ModuleE::MASTER, 0, MasterSlices, tint);
            // The score cues went to the Export tab (01.10.2026), as every generator has them.
            break;
        }
        default: break;
        }
        // Every generator page ends with the notes it is shaping, so the knobs and the pattern are
        // on the same screen (PLAN 8.1).
        if (partOfTab(t) != PartE::COUNT) {
            auto roll = std::make_unique<PatternDisplay>();
            roll->setPart(partOfTab(t));
            m_patterns[static_cast<size_t>(t)] = roll.get();
            const int g = page->addGroup("Pattern", tint, 12);
            page->addControl(g, std::move(roll), "", 12, true, 2);
            page->setFillWidth(g);
        }
        m_pages[static_cast<size_t>(t)] = std::move(page);
    }
    m_percPages.resize(PercLanes);
    m_percPatterns.assign(PercLanes, nullptr);
    for (int lane = 0; lane < PercLanes; ++lane) {
        auto page = std::make_unique<ControlPage>();
        addSections(*page, m_proc, ModuleE::PERC, lane, PercSections, partColour(TAB_PERC));
        // The whole kit on every lane's page: the twelve lanes are one pattern, and only the lit
        // row moves as the lane is changed.
        auto roll = std::make_unique<PatternDisplay>();
        roll->setPart(PartE::PERC, lane);
        m_percPatterns[static_cast<size_t>(lane)] = roll.get();
        const int g = page->addGroup("Kit pattern", partColour(TAB_PERC), 12);
        page->addControl(g, std::move(roll), "", 12, true, 3);
        page->setFillWidth(g);
        m_percPages[static_cast<size_t>(lane)] = std::move(page);
    }
    buildSetPage();
    buildArrangePage();
    buildPerformPage();
    buildGalleryPage();
    buildExportPage();
    // A page taller than the window: its groups in sections (01.10.2026, Frame.h planSections) -- the sound and the
    // modulation apart, each cut where the window ends.
    for (size_t t = 0; t < m_pages.size(); ++t)   // the gallery's list scrolls, its buttons stay with it
        if (m_pages[t] != nullptr && static_cast<int>(t) != TAB_GALLERY) m_pages[t]->enableSections(phosui::skin(), [this] { layoutContent(); });
    for (auto& page : m_percPages)
        if (page != nullptr) page->enableSections(phosui::skin(), [this] { layoutContent(); });
}

void PhospheneEditor::fillPresetBox(PresetBox& pb)
{
    pb.m_presets = factoryPresets(pb.m_synth, pb.m_instance);
    for (SoundPreset& u : m_proc.userPresets(pb.m_synth, pb.m_instance)) pb.m_presets.push_back(std::move(u));
    pb.m_box->clear(juce::dontSendNotification);
    // A submenu per group, in the order the groups first appear (the factory's order: oscillators, then the
    // wavetable families; the user's last).
    std::vector<std::string> order;
    std::map<std::string, juce::PopupMenu> menus;
    for (size_t i = 0; i < pb.m_presets.size(); ++i) {
        const SoundPreset& p = pb.m_presets[i];
        if (menus.find(p.m_group) == menus.end()) order.push_back(p.m_group);
        menus[p.m_group].addItem(static_cast<int>(i) + 1, p.m_name);
    }
    for (const std::string& g : order) pb.m_box->getRootMenu()->addSubMenu(g, menus[g]);
    pb.m_box->setTextWhenNothingSelected(juce::String(pb.m_presets.size()) + " presets");
}

void PhospheneEditor::addSoundGroup(ControlPage& page, ModuleE module, int instance, int owner, juce::Colour tint)
{
    // The own-sound switch is a mixer parameter; its group is this one, so it stands next to what it protects.
    // Three cells wide, so the page's first group still stands beside it: the switch and "Save..." on the first
    // row, the chooser across the second.
    const int g = page.addModuleGroup(m_proc, ModuleE::MIX, 0, "Sound", tint, 3, mix::KICK_OWN + owner, 1);
    auto pb = std::make_unique<PresetBox>();
    pb->m_synth = module;
    pb->m_instance = instance;
    auto box = std::make_unique<juce::ComboBox>();
    box->setTooltip("Presets in groups. Choosing one sets the synth's knobs and switches Own Sound on, so the "
                    "generator's per-track sound design leaves it as it is. Level, ducking and (on a voice) the high "
                    "pass and the trance gate stay where they are. Undo takes it back.");
    pb->m_box = box.get();
    PresetBox* raw = pb.get();
    box->onChange = [this, raw] {
        const int id = raw->m_box->getSelectedId();
        if (id <= 0 || id > static_cast<int>(raw->m_presets.size())) return;
        const SoundPreset& preset = raw->m_presets[static_cast<size_t>(id - 1)];
        m_proc.applyPreset(raw->m_synth, raw->m_instance, preset);
        raw->m_loaded = presetTargets(raw->m_synth, raw->m_instance, preset);
        frame::PresetBrowser::played("Phosphene", juce::String(m_proc.params().key(m_proc.params().base(raw->m_synth, raw->m_instance))).upToFirstOccurrenceOf(".", false, false)
                                                      .trimCharactersAtEnd("0123456789"), juce::String(preset.m_name));
    };
    fillPresetBox(*pb);

    auto save = std::make_unique<frame::IconButton>(frame::IconButton::IconE::SAVE, "Saves this synth's knobs as a preset of your own (group \"User\")");
    save->onClick = [this, raw] {
        m_presetNameDialog = std::make_unique<juce::AlertWindow>("Save preset", "A name for this sound:", juce::MessageBoxIconType::NoIcon, this);
        m_presetNameDialog->addTextEditor("name", raw->m_box->getSelectedId() > 0 ? raw->m_box->getText() : juce::String("My sound"));
        m_presetNameDialog->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
        m_presetNameDialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        m_presetNameDialog->enterModalState(true, juce::ModalCallbackFunction::create([this, raw](int result) {
            const juce::String name = m_presetNameDialog != nullptr ? m_presetNameDialog->getTextEditorContents("name") : juce::String();
            if (result == 1 && m_proc.saveUserPreset(raw->m_synth, raw->m_instance, name) != juce::File()) {
                fillPresetBox(*raw);
                for (size_t i = 0; i < raw->m_presets.size(); ++i)
                    if (raw->m_presets[i].m_group == "User" && juce::String(raw->m_presets[i].m_name) == juce::File::createLegalFileName(name.trim()))
                        raw->m_box->setSelectedId(static_cast<int>(i) + 1, juce::dontSendNotification);
            }
        }), false);
    };
    page.addControl(g, std::move(save), "", 2, true);
    auto browse = std::make_unique<frame::IconButton>(frame::IconButton::IconE::SEARCH,
                                                      "Browse the presets: by name, group, favourites, recently played; hold a row's triangle to listen");
    browse->onClick = [this, raw] { browsePresets(*raw); };
    page.addControl(g, std::move(browse), "", 2, true);
    raw->m_page = &page;
    raw->m_nameCell = page.addControl(g, std::move(box), "Preset", 3);
    m_presetBoxes.push_back(std::move(pb));
}

int PhospheneEditor::tabOfSynth(ModuleE synth, int instance)
{
    return synth == ModuleE::KICK ? TAB_KICK : synth == ModuleE::BASS ? TAB_BASS : synth == ModuleE::ACID ? TAB_ACID : TAB_LEAD + instance;
}

std::vector<std::pair<int, float>> PhospheneEditor::presetTargets(ModuleE synth, int instance, const SoundPreset& preset)
{
    // What the preset would make of the synth's knobs: worked out on a copy of the store.
    if (m_scratch == nullptr) m_scratch = std::make_unique<ParamStore>();
    m_scratch->copyValuesFrom(m_proc.params());
    applySoundPreset(*m_scratch, synth, instance, preset.m_text);
    std::vector<std::pair<int, float>> out;
    const int b = m_proc.params().base(synth, instance);
    for (int k = 0; k < ParamStore::moduleCount(synth); ++k)
        if (!presetLeaves(synth, k)) out.emplace_back(b + k, m_scratch->get(b + k));
    return out;
}

void PhospheneEditor::addSfxPresetGroup(ControlPage& page, juce::Colour tint)
{
    // 24.09.2026, the user: "Im SFX-Fenster ist nach wie vor keine Auswahl fuer das Preset". The effects' presets are
    // the bank's (Sfx.h, SfxPreset): 2048 in eleven families, drawn per event and never twice in a track. A family
    // can now be fixed to one of them, and each can be heard before it is chosen.
    const int g = page.addGroup("Effect Presets", tint, 12);
    const int base = m_proc.params().base(ModuleE::SFX);
    for (int i = 0; i < sfx::NumPresetChoices; ++i) {
        const int id = base + sfx::FirstPreset + i;
        StoreParameter* param = m_proc.parameterFor(id);
        if (param == nullptr) continue;
        const SfxTypeE type = PresetChoiceType[i];
        const juce::String family(SfxTypeNames[static_cast<int>(type)]);
        const int n = SfxBankCount[static_cast<int>(type)];
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
        box->setTooltip(juce::String(m_proc.params().key(id)) + ": Auto lets the generator draw a preset of the family for "
                        "every event, never the same twice in a track; a number makes every " + family + " play that one");
        m_sfxPresetLinks.push_back(std::make_unique<juce::ComboBoxParameterAttachment>(*param, *box));
        page.enableMidiLearn(m_proc, *box, id);
        page.addControl(g, std::move(box), family, 2, false, 1, { id });

        auto play = std::make_unique<juce::TextButton>("Hear");
        play->setTooltip("Plays the chosen " + family + " in the running set (Auto: one of the family at random). "
                         "Nothing sounds while the transport stands.");
        play->onClick = [this, i, id, n] {
            int preset = static_cast<int>(std::lround(m_proc.params().get(id)));
            if (preset <= 0) preset = 1 + juce::Random::getSystemRandom().nextInt(juce::jmax(1, n));
            m_proc.previewSfx(i, preset);
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
        const std::string grp = all[i].m_group;
        if (groups.find(grp) == groups.end()) order.push_back(grp);
        groups[grp].addItem(static_cast<int>(i) + 1, all[i].m_name);
    }
    // The scenes first: two recordings as one place is what most tracks want.
    std::stable_partition(order.begin(), order.end(), [](const std::string& s) { return s == "Scenes"; });
    for (const std::string& grp : order) box->getRootMenu()->addSubMenu(grp, groups[grp]);
    box->setTooltip("Loads a factory preset into the Field knobs: its recordings, loop, envelopes, filter and space. "
                    "Set A Category back to Auto to let the composer choose a place for every track again.");
    juce::ComboBox* raw = box.get();
    box->onChange = [this, raw] {
        const int id = raw->getSelectedId();
        if (id > 0) m_proc.applyFieldPreset(id - 1);
    };
    page.addControl(g, std::move(box), "Preset", 4, false, 1);

    auto status = std::make_unique<juce::Label>();
    status->setJustificationType(juce::Justification::centredLeft);
    status->setMinimumHorizontalScale(0.7f);
    m_fieldStatus = status.get();
    page.addControl(g, std::move(status), "Library", 6, false, 1);

    auto folder = std::make_unique<juce::TextButton>("Folder...");
    folder->setTooltip("Adds a folder of your own field recordings (FLAC, named fr-<category>-*.flac, e.g. Noctuary's "
                       "FieldRecordings) beside the ones Phosphene installs. Cancel the dialog to remove it.");
    folder->onClick = [this] {
        m_fieldChooser = std::make_unique<juce::FileChooser>("Folder of field recordings", juce::File(m_proc.fieldFolder()));
        m_fieldChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                   [this](const juce::FileChooser& fc) {
                                       m_proc.setFieldFolder(fc.getResult() == juce::File() ? juce::String() : fc.getResult().getFullPathName());
                                       refreshFieldStatus();
                                   });
    };
    page.addControl(g, std::move(folder), " ", 2);
    refreshFieldStatus();
}

void PhospheneEditor::refreshFieldStatus()
{
    if (m_fieldStatus == nullptr) return;
    const int n = scanFieldLibrary();
    const std::vector<std::string> folders = fieldLibraryFolders();
    juce::String where;
    for (const std::string& f : folders) where << (where.isEmpty() ? "" : ", ") << juce::String(f);
    // Without the recordings the program works as before: the Field track and the NASA shots are silent (the user:
    // "achte darauf, dass das Programm auch dann funktioniert, wenn die Samples nicht heruntergeladen wurden").
    m_fieldStatus->setText(n > 0 ? juce::String(n) + " recordings (" + where + ")"
                                : juce::String("No field recordings installed: the Field track stays silent. Install them with the setup, or add a folder."),
                          juce::dontSendNotification);
    m_fieldStatus->setTooltip(m_fieldStatus->getText());
}

juce::StringArray PhospheneEditor::parametersOnPages() const
{
    const ParamStore& p = m_proc.params();
    juce::StringArray seen;
    auto collect = [&](const phosui::ControlPage* page) {
        if (page == nullptr) return;
        for (int g = 0; g < page->groupCount(); ++g)
            for (int id : page->groupParams(g)) seen.addIfNotAlreadyThere(juce::String(p.key(id)));
    };
    for (auto& page : m_pages) collect(page.get());
    for (auto& page : m_percPages) collect(page.get());
    return seen;
}

ControlPage* PhospheneEditor::activePage() const
{
    if (m_tab == TAB_PERC) return m_percPages[static_cast<size_t>(juce::jlimit(0, PercLanes - 1, m_percLane))].get();
    return m_tab >= 0 && m_tab < static_cast<int>(m_pages.size()) ? m_pages[static_cast<size_t>(m_tab)].get() : nullptr;
}

void PhospheneEditor::setTab(int index)
{
    if (helpShown()) showHelp(false);
    m_tab = juce::jlimit(0, tabNames().size() - 1, index);
    // The group on top, and the row of its pages under it (01.10.2026).
    const int grp = groupOf(m_tab);
    if (grp < static_cast<int>(m_lastTabOfGroup.size())) m_lastTabOfGroup[static_cast<size_t>(grp)] = m_tab;
    for (int i = 0; i < m_groupTabs.size(); ++i) m_groupTabs[i]->setToggleState(i == grp, juce::dontSendNotification);
    for (int i = 0; i < m_subButtons.size(); ++i) {
        const int t = m_subTab[static_cast<size_t>(i)];
        m_subButtons[i]->setVisible(groupOf(t) == grp);
        m_subButtons[i]->setToggleState(t == m_tab, juce::dontSendNotification);
    }
    for (int i = 0; i < m_laneButtons.size(); ++i) m_laneButtons[i]->setVisible(m_tab == TAB_PERC);
    for (int i = 0; i < m_laneButtons.size(); ++i) m_laneButtons[i]->setToggleState(i == m_percLane, juce::dontSendNotification);
    m_viewport.setViewedComponent(activePage(), false);
    layoutContent();
    // Feed the page that has just opened before it is seen for the first time, so a screenshot of
    // a tab shows what it has to say rather than an empty frame waiting for the next tick.
    if (m_tab == TAB_SET) refreshSetPage();
    else if (m_tab == TAB_ARRANGE) { m_arrangeDirty = true; refreshArrangePage(); }
    else if (m_tab == TAB_PERFORM) refreshPerformPage();
    else if (m_tab == TAB_GALLERY && m_gallery != nullptr && m_gallery->rowCount() == 0) refreshGalleryPage();
    else if (m_tab == TAB_EXPORT) refreshExportPage();
    refreshPattern();
    m_content.repaint();
}

void PhospheneEditor::setPercLane(int lane)
{
    m_percLane = juce::jlimit(0, PercLanes - 1, lane);
    for (int i = 0; i < m_laneButtons.size(); ++i) m_laneButtons[i]->setToggleState(i == m_percLane, juce::dontSendNotification);
    m_viewport.setViewedComponent(activePage(), false);
    layoutContent();
    m_content.repaint();
}

void PhospheneEditor::resized()
{
    // The body is drawn in design coordinates and scaled to the window, so a page never reflows:
    // dragging the corner is a zoom. That is what makes a picture taken at design size the truth
    // about the layout.
    // Fitted, not stretched: the width decides unless the window is wider than the design's shape, and then the
    // height does and the body is centred (a maximised or full-screen window on a wide screen). A taller window
    // shows more of the page.
    const float scale = juce::jmax(0.25f, juce::jmin(static_cast<float>(getWidth()) / m_designW,
                                                     static_cast<float>(getHeight()) / juce::jmin(m_designH, 760)));
    const float left = juce::jmax(0.0f, (getWidth() - m_designW * scale) * 0.5f);
    m_content.setTransform(juce::AffineTransform::scale(scale).translated(left, 0.0f));
    m_content.setBounds(0, 0, m_designW, juce::roundToInt(getHeight() / scale));
}

void PhospheneEditor::layoutContent()
{
    // The frame's header (Frame.h), the set strip, the row of tabs and the row of a group's pages.
    juce::Rectangle<int> r = m_content.getLocalBounds().reduced(10, 0).withTrimmedTop(10);
    frame::Header h;
    h.m_logo = &m_logo;
    h.m_title = &m_titleSpot;
    h.m_titleWidth = static_cast<int>(frame::titleWidth(phosui::skin(), 21.0f)) + 12;
    h.m_choices = { { &m_style, 124 }, { &m_key, 60 }, { &m_scale, 150 } };
    h.m_lengthLabel = &m_lengthLabel;
    h.m_length = &m_length;
    h.m_actions = { { &m_compose, 112 }, { &m_newSeed, frame::IconWidth } };
    h.m_play = &m_play;
    h.m_mute = &m_mute;
    h.m_like = &m_likeIcon;
    h.m_dislike = &m_dislikeIcon;
    h.m_status = &m_status;
    h.m_curation = &m_where;
    h.m_update = &m_updateButton;
    h.m_tools = { &m_headsetIcon, nullptr, &m_compare, nullptr, &m_undoIcon, &m_redoIcon, nullptr, &m_search, &m_helpIcon, &m_settingsIcon };
    frame::layoutHeader(r, h);
    m_headerBottom = r.getY();
    // The info line along the bottom (15.10.2026); the overlays over the whole body.
    if (m_info != nullptr) {
        m_info->setBounds(r.removeFromBottom(frame::InfoLine::Height));
        r.removeFromBottom(2);
    }
    for (juce::Component* c : { static_cast<juce::Component*>(m_palette.get()), static_cast<juce::Component*>(m_browser.get()), static_cast<juce::Component*>(m_tour.get()) })
        if (c != nullptr) c->setBounds(m_content.getLocalBounds());
    // The overview, unless the settings fold it away (01.10.2026): then the pages get its height.
    const bool overview = frame::Settings::of("Phosphene").overview();
    m_overview.setVisible(overview);
    m_overview.setBounds(r.removeFromTop(overview ? OverviewH : 0));
    r.removeFromTop(4);
    m_tabRow = r.removeFromTop(TabRowH);
    {
        juce::Rectangle<int> row = m_tabRow;
        for (auto* b : m_groupTabs) b->setBounds(row.removeFromLeft(b->bestWidth()));
    }
    const int grp = groupOf(m_tab);
    if (tabsOf(grp).size() > 1) {
        juce::Rectangle<int> sub = r.removeFromTop(SubRowH).reduced(2, 3);
        for (int i = 0; i < m_subButtons.size(); ++i)
            if (groupOf(m_subTab[static_cast<size_t>(i)]) == grp)
                m_subButtons[i]->setBounds(sub.removeFromLeft(juce::jmax(84, juce::GlyphArrangement::getStringWidthInt(body(13.0f), m_subButtons[i]->getButtonText()) + 30)).reduced(2, 0));
    } else if (m_tab == TAB_PERC) {
        juce::Rectangle<int> lanes = r.removeFromTop(SubRowH).reduced(2, 3);
        const int lw = juce::jmin(52, lanes.getWidth() / juce::jmax(1, m_laneButtons.size()));
        for (auto* b : m_laneButtons) b->setBounds(lanes.removeFromLeft(lw).reduced(2, 0));
    }
    m_viewport.setBounds(r.reduced(8, 4));
    if (m_help != nullptr) m_help->setBounds(m_viewport.getBounds());
    if (auto* page = activePage()) {
        page->setAvailableHeight(m_viewport.getHeight());   // the sections are cut to it (01.10.2026)
        const int first = page->layout(m_viewport.getWidth());
        if (first > m_viewport.getHeight()) page->layout(m_viewport.getWidth() - 12);   // room for the scrollbar
    }
}

void PhospheneEditor::paint(juce::Graphics& g) { g.fillAll(Bg0); }

void PhospheneEditor::paintContent(juce::Graphics& g)
{
    // The frame's backdrop (strong behind the header, faint behind the pages), the logo and the name.
    const frame::Skin& skin = phosui::skin();
    m_backdrop.paint(g, m_content.getLocalBounds(), m_headerBottom, skin, frame::Settings::of("Phosphene").backdrop());
    if (skin.m_logo) skin.m_logo(g, m_logo);
    frame::drawTitle(g, skin, m_titleSpot.getBounds().toFloat(), 21.0f);
    g.setColour(Edge);
    g.fillRect(m_tabRow.getX(), m_tabRow.getBottom() - 1, m_tabRow.getWidth(), 1);
    // The page's background, so a tab reads as a sheet lying on the window (the phosphenes faint through it).
    g.setColour(skin.m_panel);
    g.fillRoundedRectangle(m_viewport.getBounds().toFloat(), 8.0f);
}

bool PhospheneEditor::standalone() const { return findParentComponentOfClass<juce::DocumentWindow>() != nullptr; }

bool PhospheneEditor::fullScreen() const
{
    auto* window = findParentComponentOfClass<juce::DocumentWindow>();
    return window != nullptr && juce::Desktop::getInstance().getKioskModeComponent() == window;
}

void PhospheneEditor::changeListenerCallback(juce::ChangeBroadcaster*) { layoutContent(); m_content.repaint(); }   // the settings changed

void PhospheneEditor::showSettings()
{
    frame::SettingsMenu m;
    m.m_app = "Phosphene";
    m.m_version = JucePlugin_VersionString;
    const juce::File state = m_proc.userFolder().getChildFile("update.txt");
    m.m_updatesOn = [state] { return phosui::UpdateCheck::enabled(state); };
    m.m_setUpdates = [this, state](bool on) {
        phosui::UpdateCheck::setEnabled(state, on);
        if (on) m_updates->startIfDue(state, true);
    };
    m.m_canFullScreen = [this] { return standalone(); };
    m.m_isFullScreen = [this] { return fullScreen(); };
    m.m_toggleFullScreen = [this] { toggleFullScreen(); };
    m.m_setWindowScale = [this](float k) {
        if (!fullScreen()) setSize(juce::roundToInt(static_cast<float>(m_designW) * k), juce::roundToInt(static_cast<float>(m_designH) * k));
    };
    m.m_headsetStatus = [this] { return m_proc.headset().statusText(); };
    if (m_proc.wrapperType == juce::AudioProcessor::wrapperType_Standalone)   // Ableton Link, the standalone only (02.10.2026)
        m.m_linkStatus = [this] { return m_proc.linkStatus(); };
    m.m_jamStatus = [this] { return m_proc.jamStatus(); };   // the family jam (02.10.2026), plugin and standalone
    m.m_about = [] { return juce::String("Whole psytrance sets, composed and synthesised.\ngithub.com/reneweller-coding/Phosphene"); };
    m.m_moreItems = [this](juce::PopupMenu& pm) {
        pm.addItem("Search: any control, preset or action (Ctrl+K) ...", [this] { openPalette(); });
        pm.addItem("First steps (the tour) ...", [this] { startTour(); });
    };
    m.show(m_settingsIcon);
}

void PhospheneEditor::refreshHeader()
{
    const TransportView t = m_proc.transport();
    m_play.show(m_proc.isPlaying() ? frame::IconButton::IconE::STOP : frame::IconButton::IconE::PLAY, m_proc.isPlaying() ? "Stop (Space)" : "Play (Space)");
    const bool muted = m_proc.muted() && !m_shooting;   // the pictures are taken muted by design (26.09.2026)
    m_mute.setToggleState(muted, juce::dontSendNotification);
    m_mute.show(muted ? frame::IconButton::IconE::SPEAKER_OFF : frame::IconButton::IconE::SPEAKER,
               muted ? (m_proc.muteForced() ? "Muted by PHOS_MUTE: an automated run makes no sound" : "Muted: click to hear the output again")
                     : "Silence the output");
    juce::String info;
    info << "seed " << juce::String(m_proc.seed()) << "   " << juce::String(t.m_bpm, 1) << " BPM   " << (t.m_hostSync ? "host clock" : "own clock")
         << "   " << (t.m_restarting ? "planning" : (t.m_playing ? "playing" : "stopped"));
    if (m_proc.isRecording()) info << "   recording " << juce::String(m_proc.recordedSeconds(), 1) << " s";
    m_status.setText(info, juce::dontSendNotification);
    m_where.setText("track " + juce::String(t.m_track + 1) + ", bar " + juce::String(t.m_barInTrack + 1) + "   (bar " + juce::String(t.m_bar + 1) + " of the set)",
                   juce::dontSendNotification);
    m_undoIcon.setEnabled(m_proc.canUndo());
    m_redoIcon.setEnabled(m_proc.canRedo());
    m_undoIcon.setTooltip(m_proc.canUndo() ? "Undo: " + m_proc.undoName() + " (Ctrl+Z)" : juce::String("Nothing to undo"));
    m_redoIcon.setTooltip(m_proc.canRedo() ? "Redo: " + m_proc.redoName() + " (Ctrl+Y)" : juce::String("Nothing to redo"));
    // The headset (the frame): its sign while one sends (or always, if asked), its group on the Perform tab.
    if (m_shotHands) m_proc.headset().inject(m_proc.headset().hands());
    const bool hs = m_proc.headset().shown(frame::Settings::of("Phosphene").headset());
    if (m_headsetIcon.isVisible() != hs) {
        m_headsetIcon.setVisible(hs);
        if (m_headsetGroup >= 0 && m_pages[TAB_PERFORM] != nullptr) m_pages[TAB_PERFORM]->setGroupVisible(m_headsetGroup, hs);
        layoutContent();
    }
}

void PhospheneEditor::refreshPattern()
{
    // Only the roll that is on screen is fed; the others are filled the moment their tab opens,
    // which is also what makes a screenshot of a tab show its pattern without waiting for a tick.
    PatternDisplay* roll = m_tab == TAB_PERC ? m_percPatterns[static_cast<size_t>(juce::jlimit(0, PercLanes - 1, m_percLane))]
                                     : (m_tab < static_cast<int>(m_patterns.size()) ? m_patterns[static_cast<size_t>(m_tab)] : nullptr);
    if (roll == nullptr) return;
    const TransportView t = m_proc.transport();
    constexpr int bars = 4;
    // The window starts at the bar the engine is in, rounded down to a group of four, so the
    // picture does not slide sideways at every tick.
    const int firstBar = juce::jmax(0, (t.m_bar / bars) * bars);
    if (m_proc.readPattern(firstBar, bars, m_patternNotes)) roll->update(m_patternNotes, firstBar, bars, t.m_musicalBeat);
    else roll->update({}, firstBar, bars, t.m_musicalBeat);
}

bool PhospheneEditor::keyPressed(const juce::KeyPress& key)
{
    // The keys every generator has (Frame.h): Space, Ctrl+Z / Ctrl+Y, F1, F11, Esc, Ctrl+S / Ctrl+O / Ctrl+E.
    frame::Keys k;
    if (standalone()) {   // in a host, Space and F11 are the host's
        k.m_playStop = [this] { if (m_proc.isPlaying()) m_proc.stop(); else m_proc.play(); };
        k.m_fullScreen = [this] { toggleFullScreen(); };
    }
    k.m_undo = [this] { m_proc.undo(); };
    k.m_redo = [this] { m_proc.redo(); };
    k.m_help = [this] { showHelp(!helpShown()); };
    k.m_escape = [this] {
        if (helpShown()) showHelp(false);
        else if (fullScreen()) toggleFullScreen();
    };
    k.m_save = [this] { setTab(TAB_EXPORT); if (m_exportSet != nullptr) m_exportSet->triggerClick(); };
    k.m_open = [this] { setTab(TAB_EXPORT); if (m_loadSet != nullptr) m_loadSet->triggerClick(); };
    k.m_exportFile = [this] { setTab(TAB_EXPORT); if (m_exportMidi != nullptr) m_exportMidi->triggerClick(); };
    k.m_palette = [this] { openPalette(); };
    return frame::handleKey(key, k);
}

void PhospheneEditor::timerCallback()
{
    // The first steps on the first start (not in an automated run: the pictures, the manual, the report, PHOS_MUTE).
    if (m_tourTicks >= 0 && ++m_tourTicks > 25) {
        m_tourTicks = -1;
        if (!m_shooting && !m_proc.muteForced() && frame::Settings::of("Phosphene").value("tourDone").isEmpty()) startTour();
    }
    // The stars under the choosers: the knobs have moved from the preset chosen.
    for (auto& pb : m_presetBoxes)
        if (pb->m_page != nullptr) {
            const bool moved = !pb->m_loaded.empty() && !m_comparing && [&] {
                const ParamStore& st = m_proc.params();
                for (const auto& [id, v] : pb->m_loaded) {
                    const ParamDesc& d = st.desc(id);
                    if (std::abs(st.get(id) - v) > 0.001f * std::max(1.0e-6f, d.m_maxValue - d.m_minValue)) return true;
                }
                return false;
            }();
            pb->m_page->setCellName(pb->m_nameCell, moved ? "Preset  *" : "Preset");
        }
    // The live ring of the page on screen (26.09.2026).
    if (m_tab >= 0 && m_tab < static_cast<int>(m_pages.size()) && m_pages[static_cast<size_t>(m_tab)] != nullptr)
        m_pages[static_cast<size_t>(m_tab)]->showPlayed(m_proc);
    // The composer's preset of each synth, in its chooser (26.09.2026): the name, and "(composer)" to say who chose it.
    for (auto& pb : m_presetBoxes) {
        const int k = pb->m_synth == ModuleE::KICK ? 0 : pb->m_synth == ModuleE::BASS ? 1 : pb->m_synth == ModuleE::ACID ? 2 : 3 + pb->m_instance;
        const int idx = m_proc.composerPreset(k);
        const juce::String want = idx >= 0 && idx < static_cast<int>(pb->m_presets.size())
            ? juce::String(pb->m_presets[static_cast<size_t>(idx)].m_group) + ": " + juce::String(pb->m_presets[static_cast<size_t>(idx)].m_name) + " (composer)"
            : juce::String();
        if (want.isNotEmpty() && pb->m_box->getText() != want) pb->m_box->setText(want, juce::dontSendNotification);
        const int tab = pb->m_synth == ModuleE::KICK ? TAB_KICK : pb->m_synth == ModuleE::BASS ? TAB_BASS : pb->m_synth == ModuleE::ACID ? TAB_ACID : TAB_LEAD + pb->m_instance;
        // Every synth page, not only the one on screen (26.09.2026): a tab switched to shows the composer's values at once,
        // not a tick later -- the screenshots caught the pad's page with the knobs' values under the preset's name.
        if (m_pages[static_cast<size_t>(tab)] != nullptr) {
            const ParamStore& store = m_proc.params();
            const std::vector<std::pair<int, float>>* vals =
                idx >= 0 && idx < static_cast<int>(pb->m_presets.size()) ? &pb->m_presets[static_cast<size_t>(idx)].m_values : nullptr;
            m_pages[static_cast<size_t>(tab)]->showValues(store, store.base(pb->m_synth, pb->m_instance), ParamStore::moduleCount(pb->m_synth), vals);
        }
    }
    refreshHeader();
    refreshOverview();
    {
        const phosui::UpdateCheck::Result u = m_updates->result();
        if (u.m_newer != m_updateButton.isVisible()) {
            m_updateButton.setButtonText("Update: Phosphene " + u.m_latest.trimCharactersAtStart("vV") + " is out");
            m_updateButton.setVisible(u.m_newer);
            layoutContent();
        }
        if (helpShown()) m_help->refreshUpdate();
    }
    // Only the page that is on screen is fed. The arrange timeline in particular draws a whole set,
    // and a set that is not being looked at costs nothing at all this way.
    if (m_tab == TAB_SET) refreshSetPage();
    else if (m_tab == TAB_ARRANGE) refreshArrangePage();
    else if (m_tab == TAB_PERFORM) refreshPerformPage();
    else if (m_tab == TAB_GALLERY && m_gallery != nullptr && m_gallery->rowCount() == 0) refreshGalleryPage();
    else if (m_tab == TAB_EXPORT) refreshExportPage();
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
    juce::Component* shown = m_viewport.getViewedComponent();
    const juce::Rectangle<int> vp = m_viewport.getBounds();   // in m_content's coordinates, which are design units here
    if (shown == nullptr || !m_viewport.isVisible() || shown->getHeight() <= vp.getHeight()) return writeScreenshot(file);
    // The header and the tab row as the window shows them, then the page in full below them.
    const juce::Image head = m_content.createComponentSnapshot({ 0, 0, m_designW, vp.getY() }, true, 1.0f);
    const juce::Image page = shown->createComponentSnapshot(shown->getLocalBounds(), true, 1.0f);
    juce::Image img(juce::Image::ARGB, m_designW, vp.getY() + page.getHeight() + 8, true);
    {
        juce::Graphics g(img);
        g.fillAll(Bg0);
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
const char* curveName(CurveE c)
{
    switch (c) {
    case CurveE::LINEAR: return "linear";
    case CurveE::LOG: return "log";
    case CurveE::INT: return "int";
    case CurveE::CHOICE: return "choice";
    default: return "toggle";
    }
}
} // namespace

bool PhospheneEditor::writeManual(const juce::File& dir)
{
    if (!dir.createDirectory()) return false;
    const ParamStore& p = m_proc.params();

    // ---------------------------------------------------------------- the pictures
    for (int i = 0; i < tabNames().size(); ++i) {
        setTab(i);
        setSize(m_designW, m_designH);
        if (!writeFullPage(dir.getChildFile(shotName(i)))) return false;
    }
    // The signal flow (EditorFlow.cpp) at twice its canvas, for print.
    {
        SignalFlow flow;
        flow.setSize(juce::roundToInt(SignalFlow::CanvasW * 2.0f), juce::roundToInt(SignalFlow::CanvasH * 2.0f));
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
        o->setProperty("name", juce::String(d.m_name));
        o->setProperty("unit", juce::String(d.m_unit));
        o->setProperty("min", d.m_minValue);
        o->setProperty("max", d.m_maxValue);
        o->setProperty("default", p.defaultValue(i));
        o->setProperty("curve", juce::String(curveName(d.m_curve)));
        if (d.m_curve == CurveE::CHOICE && d.m_choices != nullptr) {
            juce::Array<juce::var> choices;
            for (int c = 0; c <= static_cast<int>(d.m_maxValue); ++c) choices.add(juce::String(d.m_choices[c]));
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
        o->setProperty("groups", groupsOf(i == TAB_PERC ? m_percPages[0].get() : m_pages[static_cast<size_t>(i)].get()));
        if (i == TAB_PERC) o->setProperty("lanes", PercLanes);
        tabs.add(juce::var(o));
    }

    // Which parameters are on a page somewhere, all twelve percussion lanes included. The generator
    // compares this with the full list; what is in neither is a parameter the editor cannot reach.
    juce::Array<juce::var> shown;
    for (const juce::String& s : parametersOnPages()) shown.add(s);

    // ---------------------------------------------------------------- the macros
    juce::Array<juce::var> macros;
    for (int i = 0; i < NumMacros; ++i) {
        auto* o = new juce::DynamicObject();
        o->setProperty("name", juce::String(MacroNames[i]));
        o->setProperty("help", juce::String(MacroHelp[i]));
        o->setProperty("momentary", macroIsMomentary(static_cast<MacroE>(i)));
        juce::Array<juce::var> moves;
        PhospheneProcessor::MacroTarget t[PhospheneProcessor::MaxMacroTargets];
        const int n = m_proc.macroTargets(static_cast<MacroE>(i), 1.0f, t);
        for (int k = 0; k < n; ++k) {
            auto* m = new juce::DynamicObject();
            m->setProperty("key", juce::String(p.key(t[k].m_param)));
            m->setProperty("value", t[k].m_value);
            m->setProperty("absolute", t[k].m_absolute);
            moves.add(juce::var(m));
        }
        o->setProperty("moves", moves);
        macros.add(juce::var(o));
    }

    auto* root = new juce::DynamicObject();
    root->setProperty("version", juce::String(JucePlugin_VersionString));
    root->setProperty("name", juce::String(JucePlugin_Name));
    root->setProperty("designWidth", m_designW);
    root->setProperty("designHeight", m_designH);
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
        if (wav.isNotEmpty()) m_proc.startRecording(juce::File(wav));
        m_proc.play();
        juce::Timer::callAfterDelay(static_cast<int>(playFor * 1000.0),
                                    [safe = juce::Component::SafePointer<PhospheneEditor>(this)] {
                                        if (safe != nullptr) { safe->m_proc.stopRecording(); safe->m_proc.stop(); }
                                        juce::JUCEApplicationBase::quit();
                                    });
        return;
    }
    const juce::String one = juce::SystemStats::getEnvironmentVariable("PHOS_SHOT", "");
    const juce::String all = juce::SystemStats::getEnvironmentVariable("PHOS_SHOT_ALL", "");
    const juce::String man = juce::SystemStats::getEnvironmentVariable("PHOS_MANUAL", "");
    // PHOS_PAGE_REPORT=<file> (01.10.2026): how far every page reaches past the window at the design size, a line each
    // and one per section (Frame.h, pageLines) -- which page still scrolls.
    const juce::String rep = juce::SystemStats::getEnvironmentVariable("PHOS_PAGE_REPORT", "");
    if (one.isEmpty() && all.isEmpty() && man.isEmpty() && rep.isEmpty()) return;
    // At design size: the pixels of the picture are then the layout's own measurements.
    setSize(m_designW, m_designH);
    // Playing, and silent: PHOS_SHOT implies PHOS_MUTE, and the pictures are meant to show the
    // meters and the pattern rolls doing something rather than an instrument that has not started.
    m_proc.play();
    // A moment for the composer to plan the first tracks, so the Set tab's list is not "planning..."
    // and the arrange timeline has a set to draw. A track's first plan costs a probe render of about
    // two seconds, so a picture of a nine-track set needs `PHOS_SHOT_WAIT=25`.
    const double wait = juce::jlimit(1.0, 600.0, juce::SystemStats::getEnvironmentVariable("PHOS_SHOT_WAIT", "6").getDoubleValue());
    juce::Timer::callAfterDelay(static_cast<int>(wait * 1000.0),
                                [safe = juce::Component::SafePointer<PhospheneEditor>(this), one, all, man, rep] {
        if (safe == nullptr) return;
        if (rep.isNotEmpty()) {
            juce::String out;
            for (int i = 0; i < tabNames().size(); ++i) {
                safe->setTab(i);
                out << frame::pageLines(safe->m_content, tabNames()[i]);
            }
            juce::File(rep).replaceWithText(out);
        }
        if (all.isNotEmpty()) {
            const juce::File dir(all);
            dir.createDirectory();
            for (int i = 0; i < tabNames().size(); ++i) {
                safe->setTab(i);
                safe->setSize(safe->m_designW, safe->m_designH);
                safe->writeFullPage(dir.getChildFile(shotName(i)));
            }
        }
        if (man.isNotEmpty()) safe->writeManual(juce::File(man));
        if (one.isNotEmpty()) safe->writeScreenshot(juce::File(one));
        juce::JUCEApplicationBase::quit();
    });
}

// ---------------------------------------------------------------- ways around the panel (15.10.2026)

namespace {

/** @brief "perc3.decay" -> "Perc 3", "kick.tune" -> "Kick": the module instance a store id belongs to. */
juce::String moduleLabelOf(const ParamStore& s, int id)
{
    const std::string& k = s.key(id);
    const juce::String p(k.substr(0, k.find('.')));
    const juce::String name = p.trimCharactersAtEnd("0123456789");
    const juce::String digits = p.substring(name.length());
    const juce::String nice = name.substring(0, 1).toUpperCase() + name.substring(1);
    return digits.isEmpty() ? nice : nice + " " + digits;
}

} // namespace

void PhospheneEditor::openPalette()
{
    if (m_browser->isVisible()) m_browser->close();
    m_palette->open(paletteCommands());
}

bool PhospheneEditor::revealId(int id)
{
    // The pages of the tabs (the percussion tab's stand-in left out), then a page per lane.
    std::vector<juce::Component*> pages;
    for (size_t t = 0; t < m_pages.size(); ++t) pages.push_back(static_cast<int>(t) == TAB_PERC ? nullptr : m_pages[t].get());
    const size_t lanes = pages.size();
    for (auto& p : m_percPages) pages.push_back(p.get());
    return frame::revealControl(pages, [this, lanes](int i) {
        if (i < static_cast<int>(lanes)) setTab(i);
        else {
            setTab(TAB_PERC);
            setPercLane(i - static_cast<int>(lanes));
        }
    }, id, phosui::skin().m_accent);
}

std::vector<frame::CommandPalette::Command> PhospheneEditor::paletteCommands()
{
    using Command = frame::CommandPalette::Command;
    std::vector<Command> out;
    auto action = [&out](const juce::String& title, const juce::String& detail, std::function<void()> f) {
        out.push_back({ title, detail, "Action", false, [f](const juce::String&) { f(); } });
    };
    action("Play / stop", "Space", [this] { if (m_proc.isPlaying()) m_proc.stop(); else m_proc.play(); });
    action("Compose set", "plans the set again with the knobs as they stand", [this] { m_proc.composeSet(); m_arrangeDirty = true; });
    action("New seed", "another night from the same knobs", [this] { m_newSeed.triggerClick(); });
    action("Undo", "Ctrl+Z", [this] { m_proc.undo(); });
    action("Redo", "Ctrl+Y", [this] { m_proc.redo(); });
    action("Mute / unmute the output", "the speaker", [this] { m_mute.triggerClick(); });
    action("Good here", "rates the bar that plays", [this] { rateNow(1); });
    action("Bad here", "rates the bar that plays", [this] { rateNow(-1); });
    action("Save the set", "Ctrl+S", [this] { setTab(TAB_EXPORT); if (m_exportSet != nullptr) m_exportSet->triggerClick(); });
    action("Open a set", "Ctrl+O", [this] { setTab(TAB_EXPORT); if (m_loadSet != nullptr) m_loadSet->triggerClick(); });
    action("Export MIDI", "Ctrl+E", [this] { setTab(TAB_EXPORT); if (m_exportMidi != nullptr) m_exportMidi->triggerClick(); });
    action("Help", "F1: the manual by topic", [this] { showHelp(true); });
    action("Settings", "the window, the headset, the jam, Link", [this] { showSettings(); });
    action("First steps", "the tour of the panel", [this] { startTour(); });
    action("Overview above the tabs", "show it or fold it away", [] {
        auto& s = frame::Settings::of("Phosphene");
        s.setOverview(!s.overview());
    });
    if (standalone()) action("Full screen", "F11", [this] { toggleFullScreen(); });
    // The pages, and the lanes of the kit.
    for (int t = 0; t < tabNames().size(); ++t)
        out.push_back({ tabNames()[t], "page", "Page", false, [this, t](const juce::String&) { setTab(t); } });
    for (int l = 0; l < PercLanes; ++l)
        out.push_back({ "Percussion: lane " + juce::String(l + 1), "page", "Page", false, [this, l](const juce::String&) {
                           setTab(TAB_PERC);
                           setPercLane(l);
                       } });
    // Every control: its page opens and it lights up; a number after its name sets it.
    const ParamStore& s = m_proc.params();
    for (int id = 0; id < s.count(); ++id) {
        if (m_proc.parameterFor(id) == nullptr) continue;
        const ParamDesc& d = s.desc(id);
        const bool number = d.m_curve != CurveE::CHOICE && d.m_curve != CurveE::TOGGLE;
        out.push_back({ d.m_name, moduleLabelOf(s, id), "Control", number, [this, id](const juce::String& n) {
                           if (n.isNotEmpty()) m_proc.applyKnobs(juce::String(m_proc.params().desc(id).m_name), { { id, n.getFloatValue() } });
                           revealId(id);
                           if (m_info != nullptr) m_info->showControl(id);
                       } });
    }
    // Every preset of every synth, the player's own ("User") as yours.
    for (size_t b = 0; b < m_presetBoxes.size(); ++b) {
        PresetBox& pb = *m_presetBoxes[b];
        const juce::String where = moduleLabelOf(s, s.base(pb.m_synth, pb.m_instance));
        for (size_t i = 0; i < pb.m_presets.size(); ++i) {
            const bool yours = pb.m_presets[i].m_group == "User";
            out.push_back({ pb.m_presets[i].m_name, where + " preset, " + juce::String(pb.m_presets[i].m_group), yours ? "Yours" : "Preset", false,
                            [this, b, i](const juce::String&) {
                                PresetBox& box = *m_presetBoxes[b];
                                if (i >= box.m_presets.size()) return;
                                setTab(tabOfSynth(box.m_synth, box.m_instance));
                                box.m_box->setSelectedId(static_cast<int>(i) + 1, juce::sendNotificationSync);   // applies it (an undo step)
                            } });
        }
    }
    return out;
}

bool PhospheneEditor::describeControl(int id, juce::String& name, juce::String& text)
{
    const ParamStore& s = m_proc.params();
    if (id < 0 || id >= s.count()) return false;
    const ParamDesc& d = s.desc(id);
    auto fmt = [&d](float v) {
        return juce::String(v, std::abs(v) >= 100.0f ? 0 : std::abs(v) >= 10.0f ? 1 : 2) + (d.m_unit[0] != 0 ? juce::String(" ") + d.m_unit : juce::String());
    };
    name = d.m_name;
    text = moduleLabelOf(s, id) + "    now " + juce::String(s.format(id));
    if (d.m_curve != CurveE::CHOICE && d.m_curve != CurveE::TOGGLE) {
        text << "    " << fmt(d.m_minValue) << " .. " << fmt(d.m_maxValue) << ", default " << fmt(s.defaultValue(id));
        const float played = m_proc.playedValue(id);
        if (std::isfinite(played) && std::abs(played - s.get(id)) > 0.004f * std::max(1.0e-6f, d.m_maxValue - d.m_minValue))
            text << "    plays at " << fmt(played) << " (the arrangement moves it)";
    }
    // What moves it: the voice's matrix rows on it.
    if (m_modulation != nullptr && m_modulation->access().m_rowsOn) {
        juce::String by;
        for (const frame::ModAccess::Row& r : m_modulation->access().m_rowsOn(id))
            by << (by.isEmpty() ? "" : ", ") << r.m_name << " (" << (r.m_depth >= 0.0f ? "+" : "") << juce::String(r.m_depth, 2) << ")";
        if (by.isNotEmpty()) text << "    moved by " << by;
    }
    int ch = 0, cc = 0;
    if (m_proc.midiMap().controllerOf(id, ch, cc)) text << "    MIDI CC " << cc << " (ch " << (ch + 1) << ")";
    text << "    [" << juce::String(s.key(id)) << "]";
    return true;
}

void PhospheneEditor::browsePresets(PresetBox& pb)
{
    if (m_palette->isVisible()) m_palette->close();
    const ParamStore& s = m_proc.params();
    frame::PresetBrowser::Bank bank;
    bank.m_key = juce::String(s.key(s.base(pb.m_synth, pb.m_instance))).upToFirstOccurrenceOf(".", false, false).trimCharactersAtEnd("0123456789");
    bank.m_title = moduleLabelOf(s, s.base(pb.m_synth, pb.m_instance)) + " presets";
    for (size_t i = 0; i < pb.m_presets.size(); ++i) {
        const bool yours = pb.m_presets[i].m_group == "User";
        bank.m_entries.push_back({ pb.m_presets[i].m_name, yours ? juce::String("Yours") : juce::String(pb.m_presets[i].m_group), {}, static_cast<int>(i) + 1, yours, {} });
    }
    PresetBox* raw = &pb;
    bank.m_preview = [this, raw](const frame::PresetBrowser::Entry& e) {
        if (e.m_index < 1 || e.m_index > static_cast<int>(raw->m_presets.size())) return;
        auto values = presetTargets(raw->m_synth, raw->m_instance, raw->m_presets[static_cast<size_t>(e.m_index - 1)]);
        const int k = raw->m_synth == ModuleE::KICK ? 0 : raw->m_synth == ModuleE::BASS ? 1 : raw->m_synth == ModuleE::ACID ? 2 : 3 + raw->m_instance;
        values.emplace_back(m_proc.params().base(ModuleE::MIX) + mix::KICK_OWN + k, 1.0f);   // heard: the synth's own sound, not the composer's
        m_proc.previewKnobs(values);
    };
    bank.m_endPreview = [this] { m_proc.endPreview(); };
    bank.m_choose = [raw](const frame::PresetBrowser::Entry& e) { raw->m_box->setSelectedId(e.m_index, juce::sendNotificationSync); };
    m_browser->open(std::move(bank));
}

void PhospheneEditor::compare(bool on)
{
    m_comparing = on;
    if (!on) {
        m_proc.endPreview();
        return;
    }
    // Every synth whose knobs have moved from the preset chosen: that preset again, while A|B is held.
    std::vector<std::pair<int, float>> values;
    for (auto& pb : m_presetBoxes) values.insert(values.end(), pb->m_loaded.begin(), pb->m_loaded.end());
    if (!values.empty()) m_proc.previewKnobs(values);
}

void PhospheneEditor::startTour()
{
    if (helpShown()) showHelp(false);
    std::vector<frame::Tour::Step> steps;
    steps.push_back({ &m_style, "The style", "Where the set starts -- the style, its key and scale; the set wanders from there through the night." });
    steps.push_back({ &m_compose, "Compose", "Plans the set again with the knobs as they stand. The die beside it: a new seed, another night from the same knobs." });
    steps.push_back({ &m_play, "Play", "Plays and stops (Space). The speaker beside it silences the output." });
    if (m_overview.isVisible())
        steps.push_back({ &m_overview, "The overview", "The set as a strip: its tracks, where it plays. A click on a track jumps to it." });
    if (!m_groupTabs.isEmpty())
        steps.push_back({ m_groupTabs[0], "The pages", "Every part of the instrument: the set, the arrangement, the kick and the bass, the percussion, the "
                                                      "acid, the synths (each with its presets and your own, and its modulation), the effects, the mixer, "
                                                      "Perform for playing live, Export, the gallery of saved sets." });
    steps.push_back({ &m_search, "Search", "Ctrl+K: any knob, preset or action by a few letters -- its page opens and it lights up. A number after a "
                                          "knob's name sets it (\"cutoff 800\")." });
    steps.push_back({ &m_compare, "A|B", "Hold it to hear the presets as you chose them; let go for the sound as you have turned it since." });
    if (m_info != nullptr)
        steps.push_back({ m_info.get(), "The info line", "What the control under the mouse is: its value, its range, what moves it, its MIDI controller." });
    steps.push_back({ &m_helpIcon, "Help", "F1: the manual by topic, and the knobs of the page in front." });
    steps.push_back({ &m_settingsIcon, "Settings", "The window's size, the headset, the family jam, Ableton Link -- and these first steps again." });
    m_tour->start(std::move(steps));
}
