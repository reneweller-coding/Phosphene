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
    const int bpmId = params.base(Module::Compose) + compose::Bpm;
    auto offsetFor = [&](double bpm) {
        return params.toNormalised(bpmId, static_cast<float>(bpm)) - params.toNormalised(bpmId, params.get(bpmId));
    };
    const int index = composer_.trackOfBar(params, bar);
    // By value: planning the next track may move the cached ones (see Composer::tempoMap).
    const TrackPlan plan = composer_.track(params, index);
    // The track's own tempo from its hand-over (19.09.2026: the DJ overlap, Form.h), where
    // Composer::tempoMap puts it too.
    if (bar == handoverBar(plan)) {
        ControlEvent e;
        e.beat = static_cast<double>(bar) * kBeatsPerBar;
        e.length = 0.0f;
        e.value = offsetFor(plan.bpm);
        e.param = static_cast<int16_t>(bpmId);
        e.kind = ControlEvent::Kind::Offset;
        out.push_back(e);
    }
    // Over the DJ overlap -- from the next track's first bar, sixteen bars before this one ends -- ramp
    // into its tempo, as Composer::tempoMap does. Only asked for near the end of this track: planning
    // the next one costs a probe render, and at bar zero of a set that would double the wait before the
    // first sound.
    if (bar < plan.firstBar + plan.bars - 24) return;
    const TrackPlan next = composer_.track(params, index + 1);
    if (next.bpm != plan.bpm && bar == juce::jmax(handoverBar(plan), next.firstBar)) {
        ControlEvent e;
        e.beat = static_cast<double>(bar) * kBeatsPerBar;
        e.length = static_cast<float>(16 * kBeatsPerBar);
        e.value = offsetFor(next.bpm);
        e.param = static_cast<int16_t>(bpmId);
        e.kind = ControlEvent::Kind::Offset;
        out.push_back(e);
    }
}

void PlugConductor::cueMarks(const ParamStore& params, int bar, bool first)
{
    if (marks_ == nullptr) return;
    // seek() has already described the bar it landed on. Describing it again here would send the
    // same section twice, which for a visualiser is two scene changes where the music has one.
    if (bar == cueLandingBar_) { cueLandingBar_ = -1; return; }
    if (first) cueKey_ = -1;
    // By value: planning the next track may move the cached ones (see Composer::tempoMap).
    const TrackPlan plan = composer_.track(params, composer_.trackOfBar(params, bar));
    cueMarksForBar(plan.form, plan.firstBar, plan.key, plan.scale, bar, cueKey_, *marks_);
    // The next track's intro starts over this one's outro (the DJ overlap): its marks as well, after
    // this track's, in the order Composer::sections() lists them.
    const int incoming = composer_.incomingOfBar(params, bar);
    if (incoming >= 0) {
        const TrackPlan next = composer_.track(params, incoming);
        cueMarksForBar(next.form, next.firstBar, next.key, next.scale, bar, cueKey_, *marks_);
    }
}

void PlugConductor::seek(const ParamStore& params, int startBar, double beatOffset, bool writeTempo,
                         std::mutex& engineLock)
{
    nextBar_ = juce::jmax(0, startBar);
    beatOffset_ = beatOffset;
    writeTempo_ = writeTempo;
    notes_.clear();
    controls_.clear();
    notePos_ = controlPos_ = 0;
    // The marks in the ring describe beats that are behind us now. The tap would send them all at
    // once the moment it saw them -- a burst of sections that are over. Clearing is the consumer's
    // end of the ring, and this is the producer's thread; it is safe only because a seek is a step
    // of the restart handshake, during which processBlock renders silence and never calls the tap
    // (see the file comment, #genWanted_).
    if (marks_ != nullptr) {
        marks_->clear();
        cueKey_ = -1;
        // Where we landed, so a visualiser that joins in the middle is not left without a section
        // until the next boundary comes round -- the same reason the sound state is replayed above.
        const TrackPlan plan = composer_.track(params, composer_.trackOfBar(params, nextBar_));
        cueLandingMark(plan.form, plan.firstBar, plan.key, plan.scale, nextBar_, cueKey_, *marks_);
        cueLandingBar_ = nextBar_;
    }

    std::vector<ControlEvent> immediate;
    // The sound of the track we are landing in. Everything a track departs from its knobs by is
    // written as control events on and after its first bar; joining a set in the middle without
    // them plays the right notes with the wrong sound.
    if (nextBar_ > 0) {
        const int from = juce::jmax(0, nextBar_ - kCatchUpBars);
        std::vector<NoteEvent> throwAway;
        composer_.composeBars(params, from, nextBar_ - from, throwAway, &immediate);
    }
    // And its tempo, for the same reason.
    if (writeTempo_) {
        const int index = composer_.trackOfBar(params, nextBar_);
        const TrackPlan plan = composer_.track(params, index);
        const int bpmId = params.base(Module::Compose) + compose::Bpm;
        ControlEvent e;
        e.value = params.toNormalised(bpmId, static_cast<float>(plan.bpm)) - params.toNormalised(bpmId, params.get(bpmId));
        e.param = static_cast<int16_t>(bpmId);
        e.kind = ControlEvent::Kind::Offset;
        immediate.push_back(e);
    }
    // Last value wins, and it arrives at once: a ramp that was in flight lands where it was
    // heading, which is the state the following bars are written against. Pushed under the engine lock
    // (by pump()), because that lock is what makes the control ring a single-producer queue.
    // Kept, not pushed: the engine is reset after this step of the handshake (landing_, PluginProcessor.h), and
    // the first pump() after that reset sends them, before the first composed bar.
    (void)engineLock;
    for (ControlEvent& e : immediate) {
        e.beat = 0.0;
        e.length = 0.0f;
    }
    landing_ = std::move(immediate);
}

bool PlugConductor::flush(EventRing<NoteEvent>* midiOut)
{
    while (controlPos_ < controls_.size()) {
        ControlEvent e = controls_[controlPos_];
        e.beat -= beatOffset_;
        if (e.beat < 0.0) e.beat = 0.0;
        if (!engine_.pushControl(e)) return false;
        ++controlPos_;
    }
    while (notePos_ < notes_.size()) {
        NoteEvent e = notes_[notePos_];
        e.beat -= beatOffset_;
        if (e.beat < 0.0) { ++notePos_; continue; }   // already past when we landed here
        if (!engine_.pushEvent(e)) return false;
        if (midiOut != nullptr) midiOut->push(e);
        ++notePos_;
    }
    return true;
}

void PlugConductor::pump(const ParamStore& params, double horizonBeats, EventRing<NoteEvent>* midiOut,
                         std::mutex& engineLock)
{
    const double target = engine_.beatPosition() + beatOffset_ + horizonBeats;
    if (!landing_.empty()) {
        const std::lock_guard<std::mutex> lock(engineLock);
        for (const ControlEvent& e : landing_) engine_.pushControl(e);
        landing_.clear();
    }
    for (;;) {
        {
            // The engine is only touched here, and only for as long as pushing takes. Composing the
            // next bar happens with the lock open, because the first bar of a track plans it.
            const std::lock_guard<std::mutex> lock(engineLock);
            if (!flush(midiOut)) return;
        }
        if (static_cast<double>(nextBar_) * kBeatsPerBar >= target) return;
        notes_.clear();
        controls_.clear();
        notePos_ = controlPos_ = 0;
        if (gPlugTrace && nextBar_ % 32 == 0)
            std::fprintf(stderr, "[phos conductor] composing bar %d, filled to beat %.1f\n", nextBar_, target);
        composer_.composeBars(params, nextBar_, 1, notes_, &controls_);
        {
            // A copy for the editor's pattern preview, in musical beats and before the offset is
            // taken off: what the display draws is what the composer wrote, not where it landed in
            // the engine's timeline. Old bars fall off the front.
            const std::lock_guard<std::mutex> lock(previewLock_);
            // A note falls off once it has *ended* before the kept bars (24.09.2026): a pad chord or a drone
            // tone struck sixteen bars ago still sounds, and it was cut with the bar it started in.
            const double oldest = static_cast<double>(nextBar_ - kPreviewBars) * kBeatsPerBar;
            preview_.erase(std::remove_if(preview_.begin(), preview_.end(),
                                          [oldest](const NoteEvent& e) { return e.beat + std::max(0.0, static_cast<double>(e.length)) < oldest; }),
                           preview_.end());
            preview_.insert(preview_.end(), notes_.begin(), notes_.end());
        }
        if (writeTempo_) tempoControls(params, nextBar_, controls_);
        cueMarks(params, nextBar_, false);
        std::sort(controls_.begin(), controls_.end(),
                  [](const ControlEvent& a, const ControlEvent& b) { return a.beat < b.beat; });
        ++nextBar_;
    }
}

bool PlugConductor::readPreview(int firstBar, int bars, std::vector<NoteEvent>& out) const
{
    out.clear();
    const std::lock_guard<std::mutex> lock(previewLock_);
    if (preview_.empty()) return false;
    const double from = static_cast<double>(firstBar) * kBeatsPerBar;
    const double to = static_cast<double>(firstBar + bars) * kBeatsPerBar;
    if (preview_.front().beat > from || preview_.back().beat < from) return false;
    // Every note that *sounds* in the window, not only those struck in it (24.09.2026, the user: "spielte das Pad
    // (oder die Drone) und es wurde nichts angezeigt"): a chord held over sixteen bars was struck before the
    // four-bar window of twelve of them and was missing from all twelve.
    for (const NoteEvent& e : preview_)
        if (e.beat < to && e.beat + std::max(0.0, static_cast<double>(e.length)) > from) out.push_back(e);
    return true;
}
