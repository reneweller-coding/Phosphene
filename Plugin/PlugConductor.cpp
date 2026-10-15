/**
 * @file PlugConductor.cpp
 * @brief Keeps the engine's rings filled from the composer, at an offset, from any bar (PluginProcessor.h, PlugConductor).
 */
#include "PluginProcessor.h"
#include "phos/Clock.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace phos;

bool gPlugTrace = false;

// ==================================================================== PlugConductor

void PlugConductor::tempoControls(const ParamStore& params, int bar, std::vector<ControlEvent>& out) const
{
    // The tempo of a set is the composer's, and the plugin hands it to the engine the way every
    // other departure from a knob is handed over: as a control event on compose.bpm, in the knob's
    // normalised domain. phos_render instead builds a phos::TempoMap in advance -- which is right
    // for a render of a known length and wrong here, because building one plans every track it
    // covers, and planning a track renders probes for its level match (two seconds each). Playing a
    // set would then begin with half a minute of silence. The two agree wherever a track's tempo is
    // the knob's, which is what the host test compares; the ramp between two tracks is a raised
    // cosine here and a straight line there (see docs/rounds/2026-09.md, Phase 6).
    const int bpmId = params.base(ModuleE::COMPOSE) + compose::BPM;
    auto offsetFor = [&](double bpm) {
        return params.toNormalised(bpmId, static_cast<float>(bpm)) - params.toNormalised(bpmId, params.get(bpmId));
    };
    const int index = m_composer.trackOfBar(params, bar);
    // By value: the next call may throw the plans away, when a knob they depend on moved (Composer::track).
    const TrackPlan plan = m_composer.track(params, index);
    // The track's own tempo from its hand-over (19.09.2026: the DJ overlap, Form.h), where
    // Composer::tempoMap puts it too.
    if (bar == handoverBar(plan)) {
        ControlEvent e;
        e.m_beat = static_cast<double>(bar) * BeatsPerBar;
        e.m_length = 0.0f;
        e.m_value = offsetFor(plan.m_bpm);
        e.m_param = static_cast<int16_t>(bpmId);
        e.m_kind = ControlEvent::KindE::OFFSET;
        out.push_back(e);
    }
    // Over the DJ overlap -- from the next track's first bar, sixteen bars before this one ends -- ramp
    // into its tempo, as Composer::tempoMap does. Only asked for near the end of this track: planning
    // the next one costs a probe render, and at bar zero of a set that would double the wait before the
    // first sound.
    if (bar < plan.m_firstBar + plan.m_bars - 24) return;
    const TrackPlan next = m_composer.track(params, index + 1);
    if (next.m_bpm != plan.m_bpm && bar == juce::jmax(handoverBar(plan), next.m_firstBar)) {
        ControlEvent e;
        e.m_beat = static_cast<double>(bar) * BeatsPerBar;
        e.m_length = static_cast<float>(16 * BeatsPerBar);
        e.m_value = offsetFor(next.m_bpm);
        e.m_param = static_cast<int16_t>(bpmId);
        e.m_kind = ControlEvent::KindE::OFFSET;
        out.push_back(e);
    }
}

void PlugConductor::cueMarks(const ParamStore& params, int bar, bool first)
{
    if (m_marks == nullptr) return;
    // seek() has already described the bar it landed on. Describing it again here would send the
    // same section twice, which for a visualiser is two scene changes where the music has one.
    if (bar == m_cueLandingBar) { m_cueLandingBar = -1; return; }
    if (first) m_cueKey = -1;
    // By value: the next call may throw the plans away, when a knob they depend on moved (Composer::track).
    const TrackPlan plan = m_composer.track(params, m_composer.trackOfBar(params, bar));
    cueMarksForBar(plan.m_form, plan.m_firstBar, plan.m_key, plan.m_scale, bar, m_cueKey, *m_marks);
    // The next track's intro starts over this one's outro (the DJ overlap): its marks as well, after
    // this track's, in the order Composer::sections() lists them.
    const int incoming = m_composer.incomingOfBar(params, bar);
    if (incoming >= 0) {
        const TrackPlan next = m_composer.track(params, incoming);
        cueMarksForBar(next.m_form, next.m_firstBar, next.m_key, next.m_scale, bar, m_cueKey, *m_marks);
    }
}

void PlugConductor::seek(const ParamStore& params, int startBar, double beatOffset, bool writeTempo,
                         std::mutex& engineLock)
{
    m_nextBar = juce::jmax(0, startBar);
    m_beatOffset = beatOffset;
    m_writeTempo = writeTempo;
    m_notes.clear();
    m_controls.clear();
    m_notePos = m_controlPos = 0;
    // The marks in the ring describe beats that are behind us now. The tap would send them all at
    // once the moment it saw them -- a burst of sections that are over. Clearing is the consumer's
    // end of the ring, and this is the producer's thread; it is safe only because a seek is a step
    // of the restart handshake, during which processBlock renders silence and never calls the tap
    // (see the file comment, #m_genWanted).
    if (m_marks != nullptr) {
        m_marks->clear();
        m_cueKey = -1;
        // Where we landed, so a visualiser that joins in the middle is not left without a section
        // until the next boundary comes round -- the same reason the sound state is replayed above.
        const TrackPlan plan = m_composer.track(params, m_composer.trackOfBar(params, m_nextBar));
        cueLandingMark(plan.m_form, plan.m_firstBar, plan.m_key, plan.m_scale, m_nextBar, m_cueKey, *m_marks);
        m_cueLandingBar = m_nextBar;
    }

    std::vector<ControlEvent> immediate;
    // The sound of the track we are landing in. Everything a track departs from its knobs by is
    // written as control events on and after its first bar; joining a set in the middle without
    // them plays the right notes with the wrong sound.
    if (m_nextBar > 0) {
        const int from = juce::jmax(0, m_nextBar - CatchUpBars);
        std::vector<NoteEvent> throwAway;
        m_composer.composeBars(params, from, m_nextBar - from, throwAway, &immediate);
    }
    // And its tempo, for the same reason.
    if (m_writeTempo) {
        const int index = m_composer.trackOfBar(params, m_nextBar);
        const TrackPlan plan = m_composer.track(params, index);
        const int bpmId = params.base(ModuleE::COMPOSE) + compose::BPM;
        ControlEvent e;
        e.m_value = params.toNormalised(bpmId, static_cast<float>(plan.m_bpm)) - params.toNormalised(bpmId, params.get(bpmId));
        e.m_param = static_cast<int16_t>(bpmId);
        e.m_kind = ControlEvent::KindE::OFFSET;
        immediate.push_back(e);
    }
    // Last value wins, and it arrives at once: a ramp that was in flight lands where it was
    // heading, which is the state the following bars are written against. Pushed under the engine lock
    // (by pump()), because that lock is what makes the control ring a single-producer queue.
    // Kept, not pushed: the engine is reset after this step of the handshake (m_landing, PluginProcessor.h), and
    // the first pump() after that reset sends them, before the first composed bar.
    (void)engineLock;
    for (ControlEvent& e : immediate) {
        e.m_beat = 0.0;
        // A base's negative length is not a ramp but its clear (Score.h, 26.09.2026): a zero here made the composer's
        // clearing of a preset a base of 0 -- acid.disperse_freq at 0 Hz, the disperser's coefficients NaN, whenever a
        // host started a set in the middle with the presets off (hosttest.realhost).
        if (!(e.m_kind == ControlEvent::KindE::BASE && e.m_length < 0.0f)) e.m_length = 0.0f;
    }
    m_landing = std::move(immediate);
}

bool PlugConductor::flush(EventRing<NoteEvent>* midiOut)
{
    while (m_controlPos < m_controls.size()) {
        ControlEvent e = m_controls[m_controlPos];
        e.m_beat -= m_beatOffset;
        if (e.m_beat < 0.0) e.m_beat = 0.0;
        if (!m_engine.pushControl(e)) return false;
        ++m_controlPos;
    }
    while (m_notePos < m_notes.size()) {
        NoteEvent e = m_notes[m_notePos];
        e.m_beat -= m_beatOffset;
        if (e.m_beat < 0.0) { ++m_notePos; continue; }   // already past when we landed here
        if (!m_engine.pushEvent(e)) return false;
        if (midiOut != nullptr) midiOut->push(e);
        ++m_notePos;
    }
    return true;
}

void PlugConductor::pump(const ParamStore& params, double horizonBeats, EventRing<NoteEvent>* midiOut,
                         std::mutex& engineLock)
{
    const double target = m_engine.beatPosition() + m_beatOffset + horizonBeats;
    if (!m_landing.empty()) {
        const std::lock_guard<std::mutex> lock(engineLock);
        for (const ControlEvent& e : m_landing) m_engine.pushControl(e);
        m_landing.clear();
    }
    for (;;) {
        {
            // The engine is only touched here, and only for as long as pushing takes. Composing the
            // next bar happens with the lock open, because the first bar of a track plans it.
            const std::lock_guard<std::mutex> lock(engineLock);
            if (!flush(midiOut)) return;
        }
        if (static_cast<double>(m_nextBar) * BeatsPerBar >= target) return;
        m_notes.clear();
        m_controls.clear();
        m_notePos = m_controlPos = 0;
        if (gPlugTrace && m_nextBar % 32 == 0)
            std::fprintf(stderr, "[phos conductor] composing bar %d, filled to beat %.1f\n", m_nextBar, target);
        m_composer.composeBars(params, m_nextBar, 1, m_notes, &m_controls);
        {
            // A copy for the editor's pattern preview, in musical beats and before the offset is
            // taken off: what the display draws is what the composer wrote, not where it landed in
            // the engine's timeline. Old bars fall off the front.
            const std::lock_guard<std::mutex> lock(m_previewLock);
            // A note falls off once it has *ended* before the kept bars (24.09.2026): a pad chord or a drone
            // tone struck sixteen bars ago still sounds, and it was cut with the bar it started in.
            const double oldest = static_cast<double>(m_nextBar - PreviewBars) * BeatsPerBar;
            m_preview.erase(std::remove_if(m_preview.begin(), m_preview.end(),
                                          [oldest](const NoteEvent& e) { return e.m_beat + std::max(0.0, static_cast<double>(e.m_length)) < oldest; }),
                           m_preview.end());
            m_preview.insert(m_preview.end(), m_notes.begin(), m_notes.end());
        }
        if (m_writeTempo) tempoControls(params, m_nextBar, m_controls);
        cueMarks(params, m_nextBar, false);
        std::sort(m_controls.begin(), m_controls.end(),
                  [](const ControlEvent& a, const ControlEvent& b) { return a.m_beat < b.m_beat; });
        ++m_nextBar;
    }
}

bool PlugConductor::readPreview(int firstBar, int bars, std::vector<NoteEvent>& out) const
{
    out.clear();
    const std::lock_guard<std::mutex> lock(m_previewLock);
    if (m_preview.empty()) return false;
    const double from = static_cast<double>(firstBar) * BeatsPerBar;
    const double to = static_cast<double>(firstBar + bars) * BeatsPerBar;
    if (m_preview.front().m_beat > from || m_preview.back().m_beat < from) return false;
    // Every note that *sounds* in the window, not only those struck in it (24.09.2026, the user: "spielte das Pad
    // (oder die Drone) und es wurde nichts angezeigt"): a chord held over sixteen bars was struck before the
    // four-bar window of twelve of them and was missing from all twelve.
    for (const NoteEvent& e : m_preview)
        if (e.m_beat < to && e.m_beat + std::max(0.0, static_cast<double>(e.m_length)) > from) out.push_back(e);
    return true;
}
