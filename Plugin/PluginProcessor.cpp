/**
 * @file PluginProcessor.cpp
 * @brief Implementation of the Phosphene processor: parameters, threads, transport and MIDI out.
 */
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "phos/Clock.h"
#include <chrono>
#include <cmath>

using namespace phos;

namespace {

/** @brief How far ahead of the play position the rings are kept filled (PLAN 3: never under 8 bars). */
constexpr double kHorizonBeats = 8.0 * kBeatsPerBar;

/** @brief `PHOS_TRACE=1`, shared with the conductor so its bar-by-bar work can be followed. */
bool gTrace = false;

/**
 * @brief "kick" -> "Kick", "perc3" -> "Perc 3", "compose" -> "Set".
 *
 * The host's parameter list is flat, so every name has to say which module it belongs to. The
 * prefix of the text key is the only source for that, which keeps the naming generated.
 */
juce::String prettyPrefix(const std::string& key)
{
    const size_t dot = key.find('.');
    juce::String p = juce::String(key.substr(0, dot == std::string::npos ? key.size() : dot));
    if (p == "compose") return "Set";
    if (p == "sfx") return "SFX";
    if (p == "fx") return "FX";
    juce::String head, digits;
    for (int i = 0; i < p.length(); ++i) {
        const juce::juce_wchar c = p[i];
        if (c >= '0' && c <= '9') digits += juce::String::charToString(c);
        else head += juce::String::charToString(c);
    }
    head = head.substring(0, 1).toUpperCase() + head.substring(1);
    return digits.isEmpty() ? head : head + " " + digits;
}

/** @brief A value formatted the way phos::ParamStore::format would, but for an arbitrary value. */
juce::String formatValue(const ParamDesc& d, float v)
{
    if (d.curve == Curve::Choice && d.choices != nullptr) {
        const int i = juce::jlimit(0, static_cast<int>(d.maxValue), static_cast<int>(std::lround(v)));
        return d.choices[i];
    }
    if (d.curve == Curve::Toggle) return v >= 0.5f ? "On" : "Off";
    if (d.curve == Curve::Int) return juce::String(static_cast<int>(std::lround(v)));
    const float a = std::fabs(v);
    return juce::String(v, a >= 100.0f ? 0 : (a >= 10.0f ? 1 : 2));
}

} // namespace

// ==================================================================== StoreParameter

StoreParameter::StoreParameter(ParamStore& store, int id, const juce::String& name)
    : juce::RangedAudioParameter(juce::ParameterID(juce::String(store.key(id)), 1), name,
                                 juce::AudioProcessorParameterWithIDAttributes().withLabel(store.desc(id).unit)),
      store_(store), id_(id), name_(name)
{
    const ParamDesc& d = store.desc(id);
    // The mapping is the store's own, so a knob in the plugin and the same knob in phos_render
    // stand at the same place: normalised 0..1 through ParamStore::fromNormalised/toNormalised
    // (logarithmic where the descriptor says so, rounded where it is discrete).
    ParamStore* s = &store;
    range_ = juce::NormalisableRange<float>(
        d.minValue, d.maxValue,
        [s, id](float, float, float n) { return s->fromNormalised(id, n); },
        [s, id](float, float, float v) { return s->toNormalised(id, v); },
        [s, id](float lo, float hi, float v) {
            const ParamDesc& dd = s->desc(id);
            const float c = juce::jlimit(lo, hi, v);
            return (dd.curve == Curve::Linear || dd.curve == Curve::Log) ? c : std::round(c);
        });
}

float StoreParameter::getValue() const { return store_.toNormalised(id_, store_.get(id_)); }

void StoreParameter::setValue(float newValue) { store_.setNormalised(id_, newValue); }

float StoreParameter::getDefaultValue() const { return store_.toNormalised(id_, store_.defaultValue(id_)); }

juce::String StoreParameter::getName(int maximumStringLength) const { return name_.substring(0, maximumStringLength); }

juce::String StoreParameter::getLabel() const { return store_.desc(id_).unit; }

int StoreParameter::getNumSteps() const
{
    const ParamDesc& d = store_.desc(id_);
    if (d.curve == Curve::Toggle) return 2;
    if (d.curve == Curve::Choice || d.curve == Curve::Int)
        return static_cast<int>(std::lround(d.maxValue - d.minValue)) + 1;
    return juce::AudioProcessor::getDefaultNumParameterSteps();
}

bool StoreParameter::isDiscrete() const
{
    const Curve c = store_.desc(id_).curve;
    return c == Curve::Int || c == Curve::Choice || c == Curve::Toggle;
}

bool StoreParameter::isBoolean() const { return store_.desc(id_).curve == Curve::Toggle; }

juce::String StoreParameter::getText(float normalisedValue, int maximumStringLength) const
{
    const juce::String s = formatValue(store_.desc(id_), store_.fromNormalised(id_, normalisedValue));
    return maximumStringLength > 0 ? s.substring(0, maximumStringLength) : s;
}

float StoreParameter::getValueForText(const juce::String& text) const
{
    const ParamDesc& d = store_.desc(id_);
    if (d.curve == Curve::Choice && d.choices != nullptr) {
        for (int i = 0; i <= static_cast<int>(d.maxValue); ++i)
            if (text.trim().equalsIgnoreCase(d.choices[i])) return store_.toNormalised(id_, static_cast<float>(i));
    }
    if (d.curve == Curve::Toggle) {
        const juce::String t = text.trim().toLowerCase();
        if (t == "on" || t == "true" || t == "yes") return 1.0f;
        if (t == "off" || t == "false" || t == "no") return 0.0f;
    }
    return store_.toNormalised(id_, static_cast<float>(text.getDoubleValue()));
}

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
    // cosine here and a straight line there (see docs/PLAN.md, Phase 6).
    const int bpmId = params.base(Module::Compose) + compose::Bpm;
    auto offsetFor = [&](double bpm) {
        return params.toNormalised(bpmId, static_cast<float>(bpm)) - params.toNormalised(bpmId, params.get(bpmId));
    };
    const int index = composer_.trackOfBar(params, bar);
    // By value: planning the next track may move the cached ones (see Composer::tempoMap).
    const TrackPlan plan = composer_.track(params, index);
    if (bar == plan.firstBar) {
        ControlEvent e;
        e.beat = static_cast<double>(bar) * kBeatsPerBar;
        e.length = 0.0f;
        e.value = offsetFor(plan.bpm);
        e.param = static_cast<int16_t>(bpmId);
        e.kind = ControlEvent::Kind::Offset;
        out.push_back(e);
    }
    // Sixteen bars before the next track, ramp into its tempo, as Composer::tempoMap does. Only
    // asked for near the end of this track: planning the next one costs a probe render, and at bar
    // zero of a set that would double the wait before the first sound.
    if (bar < plan.firstBar + plan.bars - 24) return;
    const TrackPlan next = composer_.track(params, index + 1);
    if (next.bpm != plan.bpm && bar == juce::jmax(plan.firstBar, next.firstBar - 16)) {
        ControlEvent e;
        e.beat = static_cast<double>(bar) * kBeatsPerBar;
        e.length = static_cast<float>(16 * kBeatsPerBar);
        e.value = offsetFor(next.bpm);
        e.param = static_cast<int16_t>(bpmId);
        e.kind = ControlEvent::Kind::Offset;
        out.push_back(e);
    }
}

void PlugConductor::seek(const ParamStore& params, int startBar, double beatOffset, bool writeTempo)
{
    nextBar_ = juce::jmax(0, startBar);
    beatOffset_ = beatOffset;
    writeTempo_ = writeTempo;
    notes_.clear();
    controls_.clear();
    notePos_ = controlPos_ = 0;

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
    // heading, which is the state the following bars are written against.
    for (ControlEvent& e : immediate) {
        e.beat = 0.0;
        e.length = 0.0f;
        engine_.pushControl(e);
    }
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
        if (gTrace && nextBar_ % 32 == 0)
            std::fprintf(stderr, "[phos conductor] composing bar %d, filled to beat %.1f\n", nextBar_, target);
        composer_.composeBars(params, nextBar_, 1, notes_, &controls_);
        {
            // A copy for the editor's pattern preview, in musical beats and before the offset is
            // taken off: what the display draws is what the composer wrote, not where it landed in
            // the engine's timeline. Old bars fall off the front.
            const std::lock_guard<std::mutex> lock(previewLock_);
            const double oldest = static_cast<double>(nextBar_ - kPreviewBars) * kBeatsPerBar;
            const auto cut = std::find_if(preview_.begin(), preview_.end(),
                                          [oldest](const NoteEvent& e) { return e.beat >= oldest; });
            preview_.erase(preview_.begin(), cut);
            preview_.insert(preview_.end(), notes_.begin(), notes_.end());
        }
        if (writeTempo_) tempoControls(params, nextBar_, controls_);
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
    for (const NoteEvent& e : preview_) if (e.beat >= from && e.beat < to) out.push_back(e);
    return true;
}

// ==================================================================== PhospheneProcessor

PhospheneProcessor::PhospheneProcessor()
    : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    engine_ = std::make_unique<Engine>();
    composer_ = std::make_unique<Composer>(seed_.load());
    conductor_ = std::make_unique<PlugConductor>(*engine_, *composer_);
    buildParameters();

    // PHOS_MUTE=1: silent from the first sample and never unmuted from inside. The house rule for
    // every automated run -- tests, the screenshot export -- is that nothing makes a sound.
    forceMute_ = juce::SystemStats::getEnvironmentVariable("PHOS_MUTE", "").isNotEmpty()
              || juce::SystemStats::getEnvironmentVariable("PHOS_SHOT", "").isNotEmpty()
              || juce::SystemStats::getEnvironmentVariable("PHOS_SHOT_ALL", "").isNotEmpty();
    mute_.store(forceMute_);
    trace_ = juce::SystemStats::getEnvironmentVariable("PHOS_TRACE", "").isNotEmpty();
    gTrace = trace_;

    followHost_ = wrapperType != wrapperType_Standalone;
    followHostAtomic_.store(followHost_);

    composerRun_.store(true);
    composerThread_ = std::thread([this] { composerLoop(); });
    startTimer(250);
}

PhospheneProcessor::~PhospheneProcessor()
{
    stopTimer();
    stopRecording();
    composerRun_.store(false, std::memory_order_release);
    if (composerThread_.joinable()) composerThread_.join();
}

void PhospheneProcessor::buildParameters()
{
    ParamStore& p = params();
    byId_.assign(static_cast<size_t>(p.count()), nullptr);
    for (int i = 0; i < p.count(); ++i) {
        const juce::String name = prettyPrefix(p.key(i)) + " " + p.desc(i).name;
        auto* param = new StoreParameter(p, i, name);
        byId_[static_cast<size_t>(i)] = param;
        addParameter(param);
    }
}

bool PhospheneProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainInputChannels() != 0) return false;
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void PhospheneProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    {
        // The engine allocates here and never again. Only the composer thread's short pushing
        // section is locked out; its planning, which can take seconds, is not -- a host must not
        // wait for a level-match probe to finish before it can open a plugin.
        const std::lock_guard<std::mutex> lock(engineLock_);
        sampleRate_ = sampleRate;
        scratch_.setSize(2, juce::jmax(64, samplesPerBlock), false, true, true);
        engine_->prepare(sampleRate, samplesPerBlock);
        for (HeldNote& h : held_) h = HeldNote{};
        NoteEvent junk;
        while (midiRing_.pop(junk)) {}
    }
    // A fresh generation: the conductor repositions and the rings are filled again before the next
    // sample is rendered. Nothing here writes the composer thread's own counter.
    wasPlaying_ = false;
    genWanted_.fetch_add(1, std::memory_order_release);
    setLatencySamples(engine_->latencySamples());
}

void PhospheneProcessor::timerCallback()
{
    const int latency = engine_->latencySamples();
    if (latency != getLatencySamples()) setLatencySamples(latency);
    // The composer's own knobs decide how the set is planned, and the composer throws its cached
    // plans away when they move. The published copy the editor draws has to follow, so a cheap
    // fingerprint of those parameters is watched here rather than a listener on six hundred of them.
    const ParamStore& p = params();
    const int cb = p.base(Module::Compose);
    float sum = 0.0f;
    for (int i = 0; i < ParamStore::moduleCount(Module::Compose); ++i) sum += (i + 1) * p.get(cb + i);
    if (sum != composeFingerprint_) {
        composeFingerprint_ = sum;
        plansStale_.store(true, std::memory_order_release);
    }
}

// ------------------------------------------------------------------ the composer thread

void PhospheneProcessor::setNonRealtime(bool offline) noexcept
{
    juce::AudioProcessor::setNonRealtime(offline);
    offline_.store(offline, std::memory_order_release);
}

void PhospheneProcessor::composerLoop()
{
    unsigned turns = 0;
    while (composerRun_.load(std::memory_order_acquire)) {
        if (trace_ && ++turns % 500 == 0)
            std::fprintf(stderr, "[phos composer] turn %u, generation %d, next bar %d, %d tracks planned\n",
                         turns, genLocal_, conductor_->nextBar(), publishedTracks_);
        if (!offline_.load(std::memory_order_acquire)) serviceComposer(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void PhospheneProcessor::serviceComposer(bool warmUp)
{
    const std::lock_guard<std::mutex> lock(composeLock_);
    const int want = genWanted_.load(std::memory_order_acquire);
    if (trace_ && want != genLocal_)
        std::fprintf(stderr, "[phos composer] generation %d wanted, was %d, engine reset for %d\n",
                     want, genLocal_, genReset_.load());
    if (want != genLocal_) {
        // Step 2 of the handshake: position the conductor. The engine's own tempo map is never
        // used -- the tempo arrives as control events (PlugConductor::tempoControls) -- so nothing
        // here allocates inside the engine, and a seek costs one composed bar.
        const int bar = pendingBar_.load(std::memory_order_relaxed);
        const double offset = pendingOffset_.load(std::memory_order_relaxed);
        const bool ownClock = !(followHostAtomic_.load(std::memory_order_relaxed)
                                && hostSyncNow_.load(std::memory_order_relaxed));
        {
            const std::lock_guard<std::mutex> eng(engineLock_);
            engine_->clearTempoMap();
        }
        conductor_->seek(params(), bar, offset, ownClock);
        genLocal_ = want;
        genAck_.store(want, std::memory_order_release);
        return;
    }
    if (genReset_.load(std::memory_order_acquire) != genLocal_) return;   // waiting for step 3
    conductor_->pump(params(), kHorizonBeats, &midiRing_, engineLock_);
    genPrimed_.store(genLocal_, std::memory_order_release);

    // With the rings filled, the spare time goes into planning the tracks ahead: the editor's list
    // of tracks comes from here, and a seek into a track that is already planned is instant. One
    // track per turn, because the first plan of a track measures its level by rendering it.
    // Only the composer thread does this. Offline, serviceComposer runs on the audio thread, and
    // planning a track there would stall the render for seconds per track for no gain.
    if (!warmUp) return;
    // Not while a restart is waiting: planning a track takes seconds, and the transport would
    // stand silent for all of them. The warm-up is what there is spare time for, nothing more.
    if (genWanted_.load(std::memory_order_acquire) != genLocal_) return;
    if (plansStale_.exchange(false, std::memory_order_acq_rel)) publishedTracks_ = 0;
    if (publishedTracks_ < kPublishedTracks) {
        const TrackPlan plan = composer_->track(params(), publishedTracks_);
        const std::lock_guard<std::mutex> pl(plansLock_);
        if (publishedTracks_ == 0) plans_.clear();
        plans_.push_back(plan);
        ++publishedTracks_;
    }
}

void PhospheneProcessor::requestSeek(int bar, double beatOffset)
{
    pendingBar_.store(bar, std::memory_order_relaxed);
    pendingOffset_.store(beatOffset, std::memory_order_relaxed);
    genWanted_.fetch_add(1, std::memory_order_release);
}

// ------------------------------------------------------------------ transport

void PhospheneProcessor::play() { playRequest_.store(true, std::memory_order_release); }

void PhospheneProcessor::stop()
{
    playRequest_.store(false, std::memory_order_release);
    seekToBar(0);
}

void PhospheneProcessor::seekToBar(int bar)
{
    pendingBar_.store(juce::jmax(0, bar), std::memory_order_relaxed);
    pendingOffset_.store(static_cast<double>(juce::jmax(0, bar)) * kBeatsPerBar, std::memory_order_relaxed);
    restartRequest_.store(true, std::memory_order_release);
}

void PhospheneProcessor::setFollowHost(bool on)
{
    followHost_ = on;
    followHostAtomic_.store(on, std::memory_order_release);
    requestRestart();
}

void PhospheneProcessor::setSeed(uint64_t s)
{
    {
        const std::lock_guard<std::mutex> lock(composeLock_);
        seed_.store(s, std::memory_order_relaxed);
        composer_->setSeed(s);
    }
    plansStale_.store(true, std::memory_order_release);
    seekToBar(0);
}

void PhospheneProcessor::randomiseSeed()
{
    // The clock and the address of this object: two things that differ between runs, so "another
    // set" never gives the same one twice in a session.
    const uint64_t t = static_cast<uint64_t>(juce::Time::getHighResolutionTicks());
    uint64_t x = t ^ (reinterpret_cast<uintptr_t>(this) * 0x9E3779B97F4A7C15ull);
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
    setSeed(1 + (x % 1000000000ull));
}

bool PhospheneProcessor::tryReadTrack(int index, TrackPlan& out) const
{
    // The published copy, not the composer itself: the editor must never wait on a probe render.
    const std::lock_guard<std::mutex> lock(plansLock_);
    if (index < 0 || index >= static_cast<int>(plans_.size())) return false;
    out = plans_[static_cast<size_t>(index)];
    return true;
}

TransportView PhospheneProcessor::transport() const
{
    TransportView v;
    v.playing = playRequest_.load(std::memory_order_relaxed) || hostSyncNow_.load(std::memory_order_relaxed);
    v.hostSync = hostSyncNow_.load(std::memory_order_relaxed);
    v.musicalBeat = musicalBeat_.load(std::memory_order_relaxed);
    v.bpm = bpmNow_.load(std::memory_order_relaxed);
    v.bar = static_cast<int>(v.musicalBeat / kBeatsPerBar);
    v.restarting = genPrimed_.load(std::memory_order_relaxed) != genWanted_.load(std::memory_order_relaxed);
    return v;
}

// ------------------------------------------------------------------ MIDI out

void PhospheneProcessor::emitMidi(juce::MidiBuffer& midi, double beatAtStart, double beatsPerSample, int n)
{
    const double endBeat = beatAtStart + beatsPerSample * n;
    auto sampleOf = [&](double beat) {
        if (beatsPerSample <= 0.0) return 0;
        return juce::jlimit(0, n - 1, static_cast<int>((beat - beatAtStart) / beatsPerSample));
    };
    // Note-offs that fall in this block, first: a re-used slot must be free before a note-on takes it.
    for (HeldNote& h : held_) {
        if (!h.active || h.endBeat >= endBeat) continue;
        midi.addEvent(juce::MidiMessage::noteOff(h.channel + 1, h.pitch), sampleOf(h.endBeat));
        h.active = false;
    }
    NoteEvent e;
    while (midiRing_.peek(e)) {
        if (e.beat >= endBeat) break;
        midiRing_.pop(e);
        const int channel = midiChannelOf(e.part);
        const int at = sampleOf(e.beat);
        midi.addEvent(juce::MidiMessage::noteOn(channel + 1, e.pitch, static_cast<juce::uint8>(juce::jlimit(1, 127, static_cast<int>(e.velocity)))), at);
        const double off = e.beat + juce::jmax(0.02, static_cast<double>(e.length));
        if (off < endBeat) {
            midi.addEvent(juce::MidiMessage::noteOff(channel + 1, e.pitch), juce::jmax(at, sampleOf(off)));
            continue;
        }
        HeldNote* slot = nullptr;
        for (HeldNote& h : held_) if (!h.active) { slot = &h; break; }
        if (slot == nullptr) continue;   // more than 128 notes sounding at once: the off would be lost anyway
        slot->active = true;
        slot->endBeat = off;
        slot->channel = channel;
        slot->pitch = e.pitch;
    }
}

// ------------------------------------------------------------------ the audio thread

void PhospheneProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    const juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    buffer.clear();
    midiMessages.clear();   // the generator produces MIDI; it consumes none
    if (n <= 0) return;

    // ---------------------------------------------------------------- where the clock comes from
    bool playing = playRequest_.load(std::memory_order_relaxed);
    bool hostSync = false;
    double hostBeat = 0.0;
    if (followHostAtomic_.load(std::memory_order_relaxed)) {
        if (auto* ph = getPlayHead()) {
            if (const auto pos = ph->getPosition()) {
                if (const auto ppq = pos->getPpqPosition()) {
                    hostSync = true;
                    hostBeat = *ppq;
                    playing = pos->getIsPlaying();
                    if (const auto bpm = pos->getBpm()) if (*bpm > 1.0) hostBpm_.store(*bpm, std::memory_order_relaxed);
                }
            }
        }
    }
    hostSyncNow_.store(hostSync, std::memory_order_relaxed);
    // PHOS_TRACE=1: a line every hundred blocks on stderr saying what the transport and the
    // handshake are doing. The only way to see inside a plugin that a host has loaded and that is
    // not making a sound.
    if (trace_ && ++traceCount_ % 100 == 0)
        std::fprintf(stderr, "[phos] play=%d sync=%d beat=%.2f want=%d ack=%d reset=%d primed=%d mute=%d follow=%d wrapper=%d\n",
                     static_cast<int>(playing), static_cast<int>(hostSync), hostBeat,
                     genWanted_.load(), genAck_.load(), genReset_.load(), genPrimed_.load(),
                     static_cast<int>(mute_.load()), static_cast<int>(followHostAtomic_.load()),
                     static_cast<int>(wrapperType));
    // Under a host the tempo is the host's. compose.bpm is written directly rather than through the
    // host parameter, so the value the user sees follows the transport without an automation fight;
    // "Follow host" in the Set tab switches the whole arrangement off.
    if (hostSync) {
        const double bpm = hostBpm_.load(std::memory_order_relaxed);
        const int id = params().base(Module::Compose) + compose::Bpm;
        if (bpm > 1.0 && std::fabs(static_cast<double>(params().get(id)) - bpm) > 0.005) params().set(id, static_cast<float>(bpm));
    }

    // ---------------------------------------------------------------- restarts
    //
    // Never ask for a second restart while one is in flight. The composer can need seconds for the
    // first bar of a track it has not planned yet, and the host's play head keeps running through
    // all of them -- so the distance between the two clocks grows while we wait, and asking again
    // for every block of that wait would cancel the answer just before it arrived, over and over.
    // That livelock is what the VST3 test caught: the plugin planned forever and never played.
    const bool restarting = genPrimed_.load(std::memory_order_acquire) != genWanted_.load(std::memory_order_acquire);
    if (restartRequest_.exchange(false, std::memory_order_acq_rel)) {
        requestSeek(pendingBar_.load(std::memory_order_relaxed), pendingOffset_.load(std::memory_order_relaxed));
        wasPlaying_ = false;
    } else if (hostSync && !restarting) {
        const double expected = engine_->beatPosition() + conductor_->beatOffset();
        const double drift = hostBeat - expected;
        if (playing && (!wasPlaying_ || std::fabs(drift) > 0.25)) {
            // The transport started, or the host moved the play head. Everything below a bar is
            // cheap to redo, because composeBars is deterministic per bar. If the plan for that bar
            // is not cached yet this takes a moment, and the next block's drift check picks up
            // whatever the host moved on by in the meantime.
            requestSeek(static_cast<int>(std::floor(hostBeat / kBeatsPerBar)), hostBeat);
        } else if (playing && std::fabs(drift) > 1.0e-9) {
            // Rounding between two clocks, not a jump: slide the offset instead of resetting, so
            // the events stay where the host expects them and nothing clicks.
            conductor_->nudgeOffset(drift);
        }
    } else if (!hostSync && playing && !wasPlaying_ && !restarting) {
        requestSeek(pendingBar_.load(std::memory_order_relaxed), pendingOffset_.load(std::memory_order_relaxed));
    }
    const bool justStopped = wasPlaying_ && !playing;
    wasPlaying_ = playing;
    if (justStopped) {
        // A transport that stops in the middle of a note leaves it sounding in whatever the MIDI
        // output is wired to; the notes we started are the notes we end.
        for (HeldNote& h : held_) {
            if (!h.active) continue;
            midiMessages.addEvent(juce::MidiMessage::noteOff(h.channel + 1, h.pitch), 0);
            h.active = false;
        }
    }

    // Step 3: the composer has positioned itself, so the engine may be cleared for the new start.
    const int want = genWanted_.load(std::memory_order_acquire);
    auto resetForNewStart = [this, want] {
        if (genAck_.load(std::memory_order_acquire) != want || genReset_.load(std::memory_order_relaxed) == want) return;
        engine_->reset();
        NoteEvent junk;
        while (midiRing_.pop(junk)) {}
        for (HeldNote& h : held_) h = HeldNote{};
        genReset_.store(want, std::memory_order_release);
    };
    resetForNewStart();
    // Offline (a bounce, or the host test): this thread does the composer's work itself, once per
    // block, exactly where phos_render does it -- so the block is self-contained and the render is
    // bit-identical. The loop is for the handshake, which needs up to three steps on a restart.
    if (isNonRealtime()) {
        for (int i = 0; i < 3; ++i) {
            serviceComposer(false);
            resetForNewStart();
            if (genPrimed_.load(std::memory_order_relaxed) == want) break;
        }
    }

    const bool ready = genPrimed_.load(std::memory_order_acquire) == want && genReset_.load(std::memory_order_relaxed) == want;
    if (!playing || !ready) {
        musicalBeat_.store(engine_->beatPosition() + conductor_->beatOffset(), std::memory_order_relaxed);
        return;   // silence: either stopped, or a restart is in flight
    }

    // ---------------------------------------------------------------- render
    const double beatAtStart = engine_->beatPosition();
    float* L = buffer.getWritePointer(0);
    float* R = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
    if (R == nullptr) {
        // Mono device: render the stereo pair into the scratch and fold it down.
        const int m = juce::jmin(n, scratch_.getNumSamples());
        engine_->process(scratch_.getWritePointer(0), scratch_.getWritePointer(1), m);
        for (int i = 0; i < m; ++i) L[i] = 0.5f * (scratch_.getReadPointer(0)[i] + scratch_.getReadPointer(1)[i]);
    } else {
        engine_->process(L, R, n);
    }
    const double beatAfter = engine_->beatPosition();
    const double beatsPerSample = n > 0 ? (beatAfter - beatAtStart) / n : 0.0;
    musicalBeat_.store(beatAfter + conductor_->beatOffset(), std::memory_order_relaxed);
    bpmNow_.store(beatsPerSample * sampleRate_ * 60.0, std::memory_order_relaxed);

    emitMidi(midiMessages, beatAtStart, beatsPerSample, n);

    {
        const juce::ScopedTryLock lock(recordLock_);
        if (lock.isLocked() && recordWriter_ != nullptr) {
            recordWriter_->write(buffer.getArrayOfReadPointers(), n);
            recordedSamples_.fetch_add(n, std::memory_order_relaxed);
        }
    }
    if (mute_.load(std::memory_order_relaxed)) buffer.clear();
}

// ------------------------------------------------------------------ state

void PhospheneProcessor::writeStateTo(juce::MemoryBlock& dest) const
{
    juce::XmlElement xml("PHOSPHENE");
    xml.setAttribute("version", 1);
    xml.setAttribute("seed", juce::String(seed_.load()));
    xml.setAttribute("followHost", followHost_);
    // Every value, not only the changed ones: "%.9g" round-trips a float exactly, and a state that
    // leaves defaults out would silently change meaning if a default ever moved.
    xml.createNewChildElement("params")->addTextElement(juce::String(params().toText(false)));
    juce::AudioProcessor::copyXmlToBinary(xml, dest);
}

void PhospheneProcessor::getStateInformation(juce::MemoryBlock& destData) { writeStateTo(destData); }

void PhospheneProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml == nullptr || !xml->hasTagName("PHOSPHENE")) return;
    if (auto* p = xml->getChildByName("params")) params().parseText(p->getAllSubText().toStdString());
    followHost_ = xml->getBoolAttribute("followHost", followHost_);
    followHostAtomic_.store(followHost_, std::memory_order_release);
    const uint64_t s = static_cast<uint64_t>(xml->getStringAttribute("seed", "1").getLargeIntValue());
    setSeed(juce::jmax<uint64_t>(1, s));
}

// ------------------------------------------------------------------ export

bool PhospheneProcessor::exportMidi(const juce::File& file, int bars)
{
    // Blocks on the composer, which is what a user action with a file dialog in front of it may do.
    const std::lock_guard<std::mutex> lock(composeLock_);
    const int total = juce::jlimit(1, 4096, bars);
    Score score;
    score.tempo = composer_->tempoMap(params(), total);
    score.keyRoot = composer_->track(params(), 0).key;
    score.scale = composer_->track(params(), 0).scale;
    for (int t = 0; composer_->track(params(), t).firstBar < total; ++t) {
        const TrackPlan p = composer_->track(params(), t);
        const double beat = static_cast<double>(p.firstBar) * kBeatsPerBar;
        if (t > 0 && p.key != composer_->track(params(), t - 1).key) score.keyChanges.push_back(KeyChange{ beat, p.key });
        score.sections.push_back(SectionMark{ beat, SectionType::Groove, 0.5f, t });
    }
    std::vector<NoteEvent> notes;
    composer_->composeBars(params(), 0, total, notes, nullptr);
    score.notes = std::move(notes);
    score.sort();
    return writeMidiFile(score, file.getFullPathName().toRawUTF8());
}

bool PhospheneProcessor::exportSet(const juce::File& file)
{
    juce::String text;
    text << "# Phosphene set\n";
    text << "# seed=" << juce::String(seed_.load()) << "\n";
    text << juce::String(params().toText(false));
    return file.replaceWithText(text);
}

bool PhospheneProcessor::importSet(const juce::File& file)
{
    const juce::String text = file.loadFileAsString();
    if (text.isEmpty()) return false;
    uint64_t s = seed_.load();
    for (const auto& line : juce::StringArray::fromLines(text))
        if (line.trim().startsWithIgnoreCase("# seed=")) s = static_cast<uint64_t>(line.fromFirstOccurrenceOf("=", false, false).getLargeIntValue());
    const bool ok = params().parseText(text.toStdString());
    setSeed(juce::jmax<uint64_t>(1, s));
    return ok;
}

// ------------------------------------------------------------------ recording

bool PhospheneProcessor::startRecording(const juce::File& file)
{
    stopRecording();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    if (stream == nullptr) return false;
    juce::WavAudioFormat wav;
    // 32-bit float, as the offline renderer writes: a recording of a set is a master, not a mixdown.
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriter::Options{}
                                                  .withSampleRate(sampleRate_)
                                                  .withNumChannels(2)
                                                  .withBitsPerSample(32)
                                                  .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    if (writer == nullptr) return false;
    if (!recordThread_.isThreadRunning()) recordThread_.startThread();
    auto threaded = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(writer.release(), recordThread_, 1 << 16);
    recordedSamples_.store(0);
    {
        const juce::ScopedLock lock(recordLock_);
        recordWriter_ = std::move(threaded);
    }
    recording_.store(true);
    return true;
}

void PhospheneProcessor::stopRecording()
{
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> gone;
    {
        const juce::ScopedLock lock(recordLock_);
        gone = std::move(recordWriter_);
    }
    gone.reset();
    recording_.store(false);
}

// ------------------------------------------------------------------ editor and entry point

juce::AudioProcessorEditor* PhospheneProcessor::createEditor() { return new PhospheneEditor(*this); }

/** @brief JUCE's entry point: one processor per instance. */
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PhospheneProcessor(); }
