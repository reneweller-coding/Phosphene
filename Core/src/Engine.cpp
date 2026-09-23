/**
 * @file Engine.cpp
 * @brief Engine implementation.
 */
#include "phos/Engine.h"
#include "phos/Patterns.h"
#include "phos/WaveTableFile.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace phos {

const char* const kStemNames[kNumStems] = { "Kick", "Bass", "Perc", "Acid", "Lead", "Counter", "Arp", "Stab", "Pad", "Drone",
                                            "Sfx", "Texture", "Vocal", "Returns" };

namespace {
constexpr double kPiD = 3.141592653589793;
bool isDiscreteCurve(Curve c) { return c == Curve::Int || c == Curve::Choice || c == Curve::Toggle; }

// The gated hall's bar-line window (20.09.2026, round "reverb"; Reverb::barGate()). Fixed, not exposed
// as knobs: the brief names a result, not a set of controls. 8 ms is fast enough to read as a hard cut
// (a raised-cosine taper, so it never clicks) yet the measured floor is already 40+ dB down well inside
// it (Tests/selftest.cpp, testGatedReverb); 40 ms hold, 15 ms reopen -- 63 ms of the bar silenced out of
// 1.655 s at 145 BPM, so most of the bar still rings.
constexpr float kHallGateCloseMs = 8.0f, kHallGateHoldMs = 40.0f, kHallGateOpenMs = 15.0f;

/**
 * @brief Clip with a quadratic knee: unity below 0.7 T, flat at T beyond 1.3 T, and a parabola in
 *        between that meets both with matching slope. Unlike tanh it leaves everything under the knee
 *        untouched, so it rounds the kick's transients without compressing the body of the mix.
 */
inline float kneeClip(float x, float T)
{
    constexpr float k = 0.7f;
    const float a = std::fabs(x);
    if (a <= k * T) return x;
    const float y = a >= (2.0f - k) * T ? T : T - (a - (2.0f - k) * T) * (a - (2.0f - k) * T) / (4.0f * (1.0f - k) * T);
    return x < 0.0f ? -y : y;
}
} // namespace

Engine::Engine() : notes_(16384), controls_(16384), immediate_(256)
{
    var_ = std::make_unique<Variation[]>(static_cast<size_t>(params_.count()));
    eff_.assign(static_cast<size_t>(params_.count()), 0.0f);
    raw_.assign(static_cast<size_t>(params_.count()), 0.0f);
    rawKnob_.assign(static_cast<size_t>(params_.count()), 0.0f);
    rawDirty_.assign(static_cast<size_t>(params_.count()), 1);
    // Which synth each parameter belongs to, for the own-sound switches (mix.*_own, 23.09.2026): -1 for none, and
    // for the parameters that are the arrangement's rather than the sound's -- the pad's and drone's high pass
    // (the sub foundation, rule 20) and the trance gate -- so an own sound still sits where the form puts it.
    ownOf_.assign(static_cast<size_t>(params_.count()), -1);
    auto mark = [&](Module m, int instance, int owner) {
        const int b = params_.base(m, instance);
        for (int k = 0; k < ParamStore::moduleCount(m); ++k) ownOf_[static_cast<size_t>(b + k)] = static_cast<int8_t>(owner);
    };
    mark(Module::Kick, 0, 0);
    mark(Module::Bass, 0, 1);
    mark(Module::Acid, 0, 2);
    for (int v = 0; v < kPolyInstances; ++v) {
        mark(Module::Poly, v, 3 + v);
        const int b = params_.base(Module::Poly, v);
        for (int k : { static_cast<int>(poly::HpFloor), static_cast<int>(poly::HpTrack), static_cast<int>(poly::Gate), static_cast<int>(poly::GatePattern),
                       static_cast<int>(poly::GateDepth), static_cast<int>(poly::GateDuty), static_cast<int>(poly::GateAttack), static_cast<int>(poly::GateRelease),
                       static_cast<int>(poly::GateTone), static_cast<int>(poly::Level) })
            ownOf_[static_cast<size_t>(b + k)] = -1;
    }
    for (int& t : liveTarget_) t = -1;
}

int Engine::keyboardTarget(int channel) const
{
    const int mb = params_.base(Module::Mix);
    const int part = static_cast<int>(std::lround(params_.get(mb + mix::KeyboardPart)));
    if (part <= 0) return -1;
    if (part == 8) return channel >= 0 && channel < 7 ? channel : -1;   // by channel: 1 acid, 2 lead ... 7 drone
    return std::min(part, 7) - 1;                                       // 0 acid, 1 .. 6 the polyphonic voices
}

void Engine::liveNoteOn(int pitch, int velocity, int channel)
{
    if (pitch < 0 || pitch > 127) return;
    const int target = keyboardTarget(channel);
    if (target < 0) return;
    const float vel = static_cast<float>(std::clamp(velocity, 1, 127)) / 127.0f;
    constexpr int kHeld = 1 << 30;   // a gate that does not run out: the key's release ends the note
    if (liveTarget_[pitch] >= 0) liveNoteOff(pitch, channel);
    if (target == 0) acid_.noteOn(pitch, vel, velocity > 110, true, kHeld, 0.0);   // held keys slide, as a 303's legato does
    else poly_[target - 1].noteOnLimited(pitch, vel, 1.0, kHeld, 0.0, false, true);
    liveTarget_[pitch] = target;
    livePlayed_ |= 1u << target;
}

void Engine::liveNoteOff(int pitch, int /*channel*/)
{
    if (pitch < 0 || pitch > 127 || liveTarget_[pitch] < 0) return;
    const int target = liveTarget_[pitch];
    if (target == 0) acid_.noteOff(pitch);
    else poly_[target - 1].noteOff(pitch);
    liveTarget_[pitch] = -1;
}

void Engine::liveAllOff()
{
    for (int k = 0; k < 128; ++k) liveNoteOff(k, 0);
    livePlayed_ = 0;
}

bool Engine::generatedSilenced(Part part) const
{
    const int mb = params_.base(Module::Mix);
    const int kbPart = static_cast<int>(std::lround(params_.get(mb + mix::KeyboardPart)));
    if (kbPart <= 0 || static_cast<int>(std::lround(params_.get(mb + mix::KeyboardMode))) != 0) return false;   // off, or Layer
    int target = -1;
    if (part == Part::Acid) target = 0;
    else if (part == Part::Lead || part == Part::Counter || part == Part::Arp || part == Part::Stab || part == Part::Pad || part == Part::Drone)
        target = 1 + polyOfPart(part);
    if (target < 0) return false;
    if (kbPart == 8) return (livePlayed_ >> target) & 1u;   // by channel: a voice is the player's from its first played note
    return std::min(kbPart, 7) - 1 == target;
}

void Engine::prepare(double sampleRate, int /*maxBlockSize*/, const Quality& quality)
{
    sr_ = sampleRate;
    quality_ = quality;
    kickBuf_.assign(static_cast<size_t>(kChunk), 0.0f);
    bassBuf_.assign(static_cast<size_t>(kChunk), 0.0f);
    percL_.assign(static_cast<size_t>(kChunk), 0.0f);
    percR_.assign(static_cast<size_t>(kChunk), 0.0f);
    acidL_.assign(static_cast<size_t>(kChunk), 0.0f);
    acidR_.assign(static_cast<size_t>(kChunk), 0.0f);
    // The library's frame limit is read when the library is built, and that happens in the first
    // Poly::prepare() below -- for the whole process, whichever engine gets there first. The
    // desktop level asks for every frame (0), which is what the limit already is, so a desktop
    // build is untouched by this line; a Quest build has to get here before the first load, which
    // is why it sits in prepare() and not in the audio path.
    setWaveTableFrameLimit(quality_.waveTableFrames);
    for (int i = 0; i < kPolyInstances; ++i) {
        polyL_[i].assign(static_cast<size_t>(kChunk), 0.0f);
        polyR_[i].assign(static_cast<size_t>(kChunk), 0.0f);
        poly_[i].prepare(sr_);
        poly_[i].setQuality(quality_.polyUnison[i], quality_.polyVoices[i]);
    }
    // Whatever the parameters name right now -- a preset, a `--set`, a restored plugin state -- is
    // expanded here, on this thread. Anything the composer decides later it asks for itself.
    ensureVoiceTables();
    for (auto* b : { &sfxL_, &sfxR_, &sfxWetL_, &sfxWetR_, &roomInL_, &roomInR_, &hallInL_, &hallInR_, &roomOutL_, &roomOutR_, &hallOutL_, &hallOutR_,
                     &texL_, &texR_, &vocL_, &vocR_, &vocThrow_, &subBuf_, &throwIn_, &sendL_, &sendR_,
                     &hallGateInL_, &hallGateInR_, &hallGateOutL_, &hallGateOutR_ })
        b->assign(static_cast<size_t>(kChunk), 0.0f);
    acid_.prepare(sr_);
    acid_.setOversampling(quality_.acidOversampling);
    sfx_.prepare(sr_);
    texture_.prepare(sr_);
    vocal_.prepare(sr_);   // loads the voice pack on the first call (Vocal.h); never on the audio thread
    sfxFx_.prepare(sr_);
    sendFx_.prepare(sr_);
    for (int i = 0; i < kPolyInstances; ++i) { polyFlanger_[i].prepare(sr_); polyPhaser_[i].prepare(sr_); }
    throw_.prepare(sr_);
    stutter_.prepare(sr_);
    subDuck_.prepare(sr_);
    for (Ducker& d : duck_) d.prepare(sr_);
    returnDuck_.prepare(sr_);
    for (TranceGate& g : gate_) g.prepare(sr_);
    room_.prepare(sr_);
    hall_.prepare(sr_);
    hallGate_.prepare(sr_);
    // The gated hall's fixed recipe (20.09.2026, round "reverb"; docs/PLAN.md has the measurements).
    // Big hall, 4 s decay (the brief's "3-5 s"), no pre-delay so the gate's timing is exact from the
    // very first sample; the same return band as the plain hall's own default. Duck: -50 dBFS is a
    // floor well under any real send, so any voice that is actually sounding trips it -- 15 ms in
    // (fast enough to catch the attack), 300 ms out, the "opens as the voice's envelope releases" time
    // constant the brief asks to report; 0.7 depth pulls the send about 10.5 dB down while it holds.
    hallGate_.set(1.8f, 4.0f, 0.35f, 0.0f, 200.0f, 9000.0f);
    hallGate_.setDuck(0.7f, -50.0f, 0.015f, 0.30f);
    comp_.prepare(sr_);
    bandLimitL_.prepare(sr_);
    bandLimitR_.prepare(sr_);
    limiter_.prepare(sr_, 1.5f);
    {
        const HalfbandDesign d = designHalfband(96.0, 0.1);
        clipUpL_.setup(d); clipUpR_.setup(d); clipDownL_.setup(d); clipDownR_.setup(d);
    }
    meter_.prepare(sr_);
    kick_.prepare(sr_);
    bass_.prepare(sr_);
    bass_.setOversampling(quality_.bassOversampling);
    perc_.prepare(sr_);
    reset();
}

void Engine::ensureVoiceTables()
{
    int want[kPolyInstances];
    int n = 0;
    for (int i = 0; i < kPolyInstances; ++i) {
        const int t = static_cast<int>(std::lround(params_.get(params_.base(Module::Poly, i) + poly::Table)));
        if (t >= kNumBuiltinWaveTables && t < kNumWaveTables) want[n++] = t;
    }
    if (n > 0) ensureWaveTables(want, n);
}

void Engine::reset()
{
    NoteEvent e;
    while (notes_.pop(e)) {}
    ControlEvent c;
    while (controls_.pop(c)) {}
    while (immediate_.pop(c)) {}
    for (int i = 0; i < params_.count(); ++i) var_[static_cast<size_t>(i)] = Variation{};
    rawValid_ = false;   // every offset and override is gone: recompute every value
    paramsPending_ = false;
    for (int& t : liveTarget_) t = -1;   // the voices are cleared below; no played note is held any more
    livePlayed_ = 0;
    samples_ = 0;
    chunkBeat_ = 0.0;
    chunkPos_ = 0;
    slotBeats_ = -1.0;
    beatsPerSample_ = 0.0;
    beatNow_.store(0.0, std::memory_order_relaxed);
    kick_.reset();
    bass_.reset();
    perc_.reset();
    acid_.reset();
    for (Poly& p : poly_) p.reset();
    // Distinct, fixed phase streams per instance: a render is the same every time.
    for (int i = 0; i < kPolyInstances; ++i) poly_[i].seedPhases(0x9E3779B97F4A7C15ull * static_cast<uint64_t>(i + 1));
    clipL_.reset();
    clipR_.reset();
    sfx_.reset();
    texture_.reset();
    vocal_.reset();
    sfxFx_.reset();
    sendFx_.reset();
    for (int i = 0; i < kPolyInstances; ++i) { polyFlanger_[i].reset(); polyPhaser_[i].reset(); }
    throw_.reset();
    stutter_.reset();
    subDuck_.reset();
    sfxShift_ = Motion{};
    sfxFlange_ = Motion{};
    sendMotion_ = Motion{};
    for (Ducker& d : duck_) d.reset();
    returnDuck_.reset();
    for (TranceGate& g : gate_) g.reset();
    room_.reset();
    hall_.reset();
    hallGate_.reset();
    comp_.reset();
    sideHp1_.reset();
    sideHp2_.reset();
    bandLimitL_.reset();
    bandLimitR_.reset();
    limiter_.reset();
    clipUpL_.reset(); clipUpR_.reset(); clipDownL_.reset(); clipDownR_.reset();
    meter_.reset();
}

float Engine::effective(int id) const
{
    return id >= 0 && id < static_cast<int>(eff_.size()) ? eff_[static_cast<size_t>(id)] : 0.0f;
}

void Engine::advanceRamps()
{
    for (int i = 0; i < params_.count(); ++i) {
        Variation& v = var_[static_cast<size_t>(i)];
        if (v.length <= 0.0) continue;
        const double x = (chunkBeat_ - v.start) / v.length;
        rawDirty_[static_cast<size_t>(i)] = 1;
        if (x >= 1.0) { v.offset = v.to; v.length = 0.0; continue; }
        const double s = x <= 0.0 ? 0.0 : 0.5 - 0.5 * std::cos(kPiD * x);   // raised cosine: C1 at both ends
        v.offset = v.from + static_cast<float>(s) * (v.to - v.from);
    }
}

void Engine::dispatchControl(const ControlEvent& e)
{
    if (e.kind == ControlEvent::Kind::BassSlot) {
        // The composer says where the first bass note of this beat is (Score.h). It arrives at the
        // beat itself, before that beat's kick, because controls are dispatched before notes at
        // equal beats -- so Kick::constrain and setPhaseTarget below see the gap this kick really
        // has, and bassPhase_ is the kick's phase at the instant the note really starts.
        slotBeats_ = static_cast<double>(e.value);
        applyParams();
        return;
    }
    if (e.param < 0 || e.param >= params_.count()) return;
    rawDirty_[static_cast<size_t>(e.param)] = 1;
    Variation& v = var_[static_cast<size_t>(e.param)];
    if (e.kind == ControlEvent::Kind::Override) {
        v.override = e.value;
    } else if (e.length <= 0.0f) {
        v.offset = e.value;
        v.length = 0.0;
    } else {
        v.from = v.offset;
        v.to = e.value;
        v.start = e.beat;
        v.length = e.length;
    }
    // At a chunk's first sample the pass also reads the tempo, which times every later event of the chunk, so it
    // runs now, as it always did. Anywhere else the events of one instant are applied together, once
    // (flushParams(), before the next note, render or chunk): a track start is hundreds of events at one beat.
    if (chunkPos_ == 0 || beatsPerSample_ <= 0.0) applyParams();
    else paramsPending_ = true;
}

double Engine::firstSlotSeconds() const
{
    // The slot the composer sent for this beat, or -- while none has been sent -- the one the bass
    // pattern family implies, which is what this function always returned (Engine.h, slotBeats_).
    const double beats = slotBeats_ > 0.0 ? slotBeats_ : firstBassSlot(pattern_);
    return beats / (beatsPerSample_ * sr_);
}

void Engine::applyParams()
{
    const ParamStore& p = params_;
    paramsPending_ = false;
    // The own-sound switches, read once (23.09.2026): kick, bass, acid, then the six polyphonic voices. A switch
    // that moved changes every value it governs, so those are recomputed however their knobs stand.
    bool own[9];
    bool ownMoved[9];
    for (int k = 0; k < 9; ++k) {
        own[k] = p.get(p.base(Module::Mix) + mix::KickOwn + k) >= 0.5f;
        ownMoved[k] = !rawValid_ || own[k] != ownSeen_[k];
        ownSeen_[k] = own[k];
    }
    // Effective values: knob + normalised offset for continuous parameters, override for discrete. Only where
    // something they are made of moved (Engine.h, raw_); the rest keep last pass's value, which is the same number.
    // A polyphonic voice whose values all stayed is not updated either (below): Poly::update only derives
    // coefficients from its inputs, so the same inputs are the same state.
    const bool full = !rawValid_;
    const int polyFirst = p.base(Module::Poly, 0), polyCount = ParamStore::moduleCount(Module::Poly);
    bool polyMoved[kPolyInstances] = {};
    auto store = [&](int i, float value) {
        const size_t si = static_cast<size_t>(i);
        // Bit for bit, not ==: -0.0f == 0.0f, and keeping the old zero's sign changed the render.
        if (!full && std::memcmp(&value, &raw_[si], sizeof value) == 0) return;
        raw_[si] = value;
        if (i >= polyFirst && i < polyFirst + kPolyInstances * polyCount) polyMoved[(i - polyFirst) / polyCount] = true;
    };
    for (int i = 0; i < p.count(); ++i) {
        const size_t si = static_cast<size_t>(i);
        const float knob = p.get(i);
        const int owner = ownOf_[si];
        if (rawValid_ && !rawDirty_[si] && knob == rawKnob_[si] && (owner < 0 || !ownMoved[owner])) continue;
        rawDirty_[si] = 0;
        rawKnob_[si] = knob;
        const Variation& v = var_[si];
        float value = knob;
        // An own sound (mix.*_own, 23.09.2026): the synth plays its knobs, whatever the composer's recipes say.
        if (owner >= 0 && own[owner]) { store(i, knob); continue; }
        if (isDiscreteCurve(p.desc(i).curve)) {
            if (v.override >= 0.0f) value = v.override;
        } else if (v.offset != 0.0f) {
            value = p.fromNormalised(i, p.toNormalised(i, knob) + v.offset);
        }
        store(i, value);
    }
    rawValid_ = true;
    std::copy(raw_.begin(), raw_.end(), eff_.begin());   // Kick::constrain below edits the kick's copy in place

    const int cb = p.base(Module::Compose);
    keyRoot_ = static_cast<int>(std::lround(eff_[static_cast<size_t>(cb + compose::Key)]));
    pattern_ = static_cast<int>(std::lround(eff_[static_cast<size_t>(cb + compose::BassPattern)]));
    scale_ = static_cast<int>(std::lround(eff_[static_cast<size_t>(cb + compose::Scale)]));
    for (int l = 0; l < kPercLanes; ++l) perc_.update(l, eff_.data() + p.base(Module::Perc, l), keyRoot_, scale_);
    // The tempo is read at chunk starts only: a control event inside a chunk must not change the
    // beats per sample the rest of that chunk is timed with.
    if (chunkPos_ == 0 || beatsPerSample_ <= 0.0) {
        const double bpm = useTempoMap_ ? tempo_.bpmAt(chunkBeat_) : static_cast<double>(eff_[static_cast<size_t>(cb + compose::Bpm)]);
        beatsPerSample_ = bpm / 60.0 / sr_;
    }

    float* kv = eff_.data() + p.base(Module::Kick);
    float* bv = eff_.data() + p.base(Module::Bass);
    const double slot = firstSlotSeconds();
    Kick::constrain(kv, slot, keyRoot_);
    kick_.update(kv, keyRoot_);
    bass_.update(bv);
    const double bpmNow = beatsPerSample_ * 60.0 * sr_;
    // The kit's auto-pan counts its period in bars (perc.pan_bars), so it needs the bar length. The
    // tempo here is the one this chunk is timed with (read at chunk starts only, above), so the pan
    // period follows a tempo ramp between tracks without ever changing inside a chunk.
    perc_.setTempo(bpmNow);
    acid_.update(eff_.data() + p.base(Module::Acid), bpmNow);
    for (int i = 0; i < kPolyInstances; ++i) {
        if (!full && !polyMoved[i] && polyBpm_[i] == bpmNow && !poly_[i].tableStale()) {
            poly_[i].refreshEnvelopeTimes();   // the one thing update() does even with the same inputs (Poly.h)
            continue;
        }
        poly_[i].update(eff_.data() + p.base(Module::Poly, i), bpmNow);
        polyBpm_[i] = bpmNow;
    }

    // The kick lock: either the kick's tail meets the bass's own start phase, or the bass starts
    // where the kick's phase is when the first bass note begins.
    lockMode_ = static_cast<int>(std::lround(bv[bass::KickLock]));
    if (lockMode_ == static_cast<int>(KickLock::KickFollowsBass)) {
        kick_.setPhaseTarget(slot, bass_.knobPhase());
        bassPhase_ = bass_.knobPhase();
    } else {
        kick_.setPhaseTarget(0.0, 0.0);
        if (lockMode_ == static_cast<int>(KickLock::BassFollowsKick)) {
            const double ph = kick_.outputPhaseAt(slot);
            bassPhase_ = ph - std::floor(ph);
        } else {
            bassPhase_ = bass_.knobPhase();
        }
    }

    const int mb = p.base(Module::Mix);
    kickMute_ = eff_[static_cast<size_t>(mb + mix::KickMute)] >= 0.5f;
    bassMute_ = eff_[static_cast<size_t>(mb + mix::BassMute)] >= 0.5f;
    percMute_ = eff_[static_cast<size_t>(mb + mix::PercMute)] >= 0.5f;
    percGain_ = percMute_ ? 0.0f : dbToGain(eff_[static_cast<size_t>(mb + mix::PercLevel)]);
    auto e = [&](int id) { return eff_[static_cast<size_t>(id)]; };
    auto partGain = [&](int mute, int level) { return e(mb + mute) >= 0.5f ? 0.0f : dbToGain(e(mb + level)); };
    const int ab = p.base(Module::Acid), sb = p.base(Module::Sfx), fb = p.base(Module::Fx);
    stripGain_[StripPerc] = percGain_;
    stripGain_[StripAcid] = partGain(mix::AcidMute, mix::AcidLevel);
    for (int k = 0; k < kPolyInstances; ++k)
        stripGain_[StripLead + k] = partGain(mix::polyMute(static_cast<PolyInstance>(k)), mix::polyLevel(static_cast<PolyInstance>(k)));
    stripGain_[StripSfx] = partGain(mix::SfxMute, mix::SfxLevel);
    const float duckA = e(mb + mix::DuckAttack), duckH = e(mb + mix::DuckHold), duckR = e(mb + mix::DuckRelease);
    stripRoom_[StripPerc] = e(mb + mix::PercRoom);
    stripHall_[StripPerc] = e(mb + mix::PercHall);
    duck_[StripPerc].set(0.0f, duckA, duckH, duckR);
    stripRoom_[StripAcid] = e(ab + acid::RoomSend);
    stripHall_[StripAcid] = e(ab + acid::HallSend);
    duck_[StripAcid].set(e(ab + acid::Duck), duckA, duckH, duckR);
    hallGateOn_[StripAcid] = e(ab + acid::HallGate) >= 0.5f;
    stripRoom_[StripSfx] = e(sb + sfx::RoomSend);
    stripHall_[StripSfx] = e(sb + sfx::HallSend);
    duck_[StripSfx].set(e(sb + sfx::Duck), duckA, duckH, duckR);
    sfx_.update(eff_.data() + sb, keyRoot_);
    // The psychedelic layer (Engine.h): the bed's and the voices' strips, their sends, the modulation
    // chains, the delay throw and the sub drop's own duck.
    {
        const int tb = p.base(Module::Texture), vb = p.base(Module::Vocal), xb = p.base(Module::PsyFx);
        stripGain_[StripTexture] = partGain(mix::TextureMute, mix::TextureLevel);
        stripGain_[StripVocal] = partGain(mix::VocalMute, mix::VocalLevel);
        stripRoom_[StripTexture] = e(tb + texture::RoomSend);
        stripHall_[StripTexture] = e(tb + texture::HallSend);
        stripFx_[StripTexture] = e(tb + texture::FxSend);
        duck_[StripTexture].set(e(tb + texture::Duck), duckA, duckH, duckR);
        stripRoom_[StripVocal] = 0.0f;
        stripHall_[StripVocal] = e(vb + vocal::HallSend);
        stripFx_[StripVocal] = e(vb + vocal::FxSend);
        duck_[StripVocal].set(e(vb + vocal::Duck), duckA, duckH, duckR);
        texture_.update(eff_.data() + tb, keyRoot_);
        vocal_.update(eff_.data() + vb, keyRoot_);
        sfxFx_.update(eff_.data() + xb);
        sendFx_.update(eff_.data() + xb);
        sendReturn_ = dbToGain(e(xb + psyfx::Return));
        motion_ = e(xb + psyfx::Motion);
        throwSend_ = e(vb + vocal::ThrowSend);
        const int tIdx = std::clamp(static_cast<int>(std::lround(e(vb + vocal::ThrowBeats))), 0, kNumDelayTimes - 1);
        // The throw: the knob's time on the left, the next shorter one on the right (a dotted eighth
        // against a quarter at the default), so the echoes of a word bounce across the field; a band of
        // 300 Hz .. 3.5 kHz inside the loop thins them out like a radio instead of piling up body.
        throw_.set(kDelayBeats[tIdx], kDelayBeats[std::max(0, tIdx - 1)], beatsPerSample_ > 0.0 ? beatsPerSample_ * 60.0 * sr_ : 145.0,
                   e(vb + vocal::ThrowFeedback), 300.0f, 3500.0f);
        // The kick's hold on the sub drop: the kit's duck times, 40 ms more hold for the kick's body.
        subDuck_.set(e(sb + sfx::SubDuck), duckA, duckH + 40.0f, duckR);
    }
    const double beatSeconds = beatsPerSample_ > 0.0 ? 1.0 / (beatsPerSample_ * sr_) : 60.0 / 145.0;
    // The gated hall's bar-line window (Reverb::barGate() takes beats, not seconds, for the same reason
    // poly.gate_attack/_release do above: it stays the same fraction of a bar under a tempo ramp).
    hallGateCloseBeats_ = static_cast<double>(kHallGateCloseMs) * 0.001 / beatSeconds;
    hallGateHoldBeats_ = static_cast<double>(kHallGateHoldMs) * 0.001 / beatSeconds;
    hallGateOpenBeats_ = static_cast<double>(kHallGateOpenMs) * 0.001 / beatSeconds;
    for (int k = 0; k < kPolyInstances; ++k) {
        const int pb = p.base(Module::Poly, k);
        const int strip = StripLead + k;
        stripRoom_[strip] = e(pb + poly::RoomSend);
        stripHall_[strip] = e(pb + poly::HallSend);
        duck_[strip].set(e(pb + poly::Duck), duckA, duckH, duckR);
        hallGateOn_[strip] = e(pb + poly::HallGate) >= 0.5f;
        gateOn_[k] = e(pb + poly::Gate) >= 0.5f;
        gatePattern_[k] = static_cast<int>(std::lround(e(pb + poly::GatePattern)));
        gateDepth_[k] = e(pb + poly::GateDepth);
        gateDuty_[k] = e(pb + poly::GateDuty);
        gateTone_[k] = e(pb + poly::GateTone);
        gateAttack_[k] = e(pb + poly::GateAttack) * 0.001 / beatSeconds;
        gateRelease_[k] = e(pb + poly::GateRelease) * 0.001 / beatSeconds;
        // 20.09.2026, round "dialogue": the voice's own modulation insert (PsyFx.h). A comb is the
        // flanger with its sweep stopped, so both share one object; the type decides which of the two
        // runs and whether either runs at all. Both are tempo-synchronised by construction (their LFO
        // reads the absolute beat), so the period stays a period in bars however the tempo ramps.
        polyMod_[k] = static_cast<int>(std::lround(e(pb + poly::Mod)));
        const float mix = e(pb + poly::ModMix);
        const bool comb = polyMod_[k] == static_cast<int>(PolyMod::Comb);
        polyFlanger_[k].set(e(pb + poly::ModBeats), comb ? 0.0f : e(pb + poly::ModDepth), e(pb + poly::ModFeedback),
                            polyMod_[k] == static_cast<int>(PolyMod::Flanger) || comb ? mix : 0.0f);
        polyPhaser_[k].set(e(pb + poly::ModBeats), e(pb + poly::ModDepth), e(pb + poly::ModFeedback),
                           polyMod_[k] == static_cast<int>(PolyMod::Phaser) ? mix : 0.0f);
    }
    // Send effects: the hall's pre-delay follows the tempo.
    room_.set(e(fb + fx::RoomSize), e(fb + fx::RoomDecay), e(fb + fx::RoomDamping), 0.0f, e(fb + fx::LowCut), e(fb + fx::HighCut));
    hall_.set(e(fb + fx::HallSize), e(fb + fx::HallDecay), e(fb + fx::HallDamping),
              static_cast<float>(e(fb + fx::HallPreDelay) * beatSeconds * sr_), e(fb + fx::LowCut), e(fb + fx::HighCut));
    roomReturn_ = dbToGain(e(fb + fx::RoomReturn));
    hallReturn_ = dbToGain(e(fb + fx::HallReturn));
    returnDuck_.set(e(fb + fx::ReturnDuck), duckA, duckH, duckR);

    const int ms = p.base(Module::Master);
    masterGain_ = dbToGain(e(ms + master::Gain) + e(mb + mix::TrackGain));
    ceiling_ = dbToGain(e(ms + master::Ceiling));
    clip_ = e(ms + master::Clip) >= 0.5f;
    comp_.set(e(ms + master::CompThreshold), e(ms + master::CompRatio), e(ms + master::CompKnee), e(ms + master::CompAttack), e(ms + master::CompRelease));
    sideHp1_.setQ(e(ms + master::MonoBass), 0.70710678f, static_cast<float>(sr_));
    sideHp2_.copyCoefficients(sideHp1_);
    const bool lim = e(ms + master::Limiter) >= 0.5f;
    if (lim != limiterOn_) limiter_.reset();
    limiterOn_ = lim;
    limiter_.set(e(ms + master::Ceiling), e(ms + master::LimiterRelease));
    clipperOn_ = e(ms + master::Clipper) >= 0.5f;
    clipperT_ = dbToGain(e(ms + master::Ceiling) + e(ms + master::ClipperThreshold));
}

void Engine::dispatch(const NoteEvent& e, double late)
{
    flushParams();   // a note reads what the controls before it at this instant set
    // A voice the keyboard plays in Replace mode leaves the composer's notes out (23.09.2026, liveNoteOn).
    if (generatedSilenced(e.part)) return;
    const float vel = static_cast<float>(e.velocity) / 127.0f;
    switch (e.part) {
    case Part::Kick:
        kick_.trigger(vel, late);
        bass_.duck(late);
        for (Ducker& d : duck_) d.trigger(late);
        returnDuck_.trigger(late);
        subDuck_.trigger(late);
        break;
    case Part::Bass: {
        const double samples = static_cast<double>(e.length) / beatsPerSample_;
        bass_.noteOn(e.pitch, vel, std::max(1, static_cast<int>(std::lround(samples))), late, bassPhase_);
        break;
    }
    case Part::Perc: {
        const int lane = e.lane < kPercLanes ? e.lane : 0;
        const int shift = static_cast<int>(e.pitch) - kPercRoleNote[static_cast<int>(perc_.role(lane))];
        perc_.trigger(lane, vel, shift, late);
        break;
    }
    case Part::Acid: {
        const double samples = static_cast<double>(e.length) / beatsPerSample_;
        acid_.noteOn(e.pitch, vel, (e.flags & kNoteAccent) != 0, (e.flags & kNoteSlide) != 0,
                     std::max(1, static_cast<int>(std::lround(samples))), late);
        break;
    }
    case Part::Lead:
    case Part::Counter:
    case Part::Arp:
    case Part::Stab:
    case Part::Pad:
    case Part::Drone: {
        const double samples = static_cast<double>(e.length) / beatsPerSample_;
        const int inst = polyOfPart(e.part);
        // noteOnLimited applies the quality level's unison and voice limits (Quality.h, Poly.h); at
        // the desktop level it is noteOn() itself.
        // 22.09.2026 (round "Lead"): the note's accent and slide flags. Every voice but the lead slides on
        // every note where its glide time says so, as before; the lead only where the composer flagged it.
        poly_[inst].noteOnLimited(e.pitch, vel, e.length, std::max(1, static_cast<int>(std::lround(samples))), late,
                                  (e.flags & kNoteAccent) != 0, e.part != Part::Lead || (e.flags & kNoteSlide) != 0);
        break;
    }
    case Part::Sfx:
    case Part::Texture:
    case Part::Vocal: {
        // Routed by type, not by the note's part (Sfx.h, sfxTypePart): the composer writes every effect
        // as a Part::Sfx note, a MIDI file may carry the bed and the voices on tracks of their own.
        const int type = static_cast<int>(e.pitch) - kSfxBaseNote;
        if (type < 0 || type >= kNumSfxTypes) break;
        const SfxType t = static_cast<SfxType>(type);
        const double samples = static_cast<double>(e.length) / beatsPerSample_;
        const int n = std::max(1, static_cast<int>(std::lround(samples)));
        const double spb = 1.0 / beatsPerSample_;
        // The event's own seed: its position on the sixteenth grid and its type. Everything a texture or
        // a voice draws hangs off it, so a bar rendered alone sounds like the same bar in the set. Since
        // 19.09.2026 the composer also sends a variant drawn from the track's seed in the lane (Form.h,
        // SfxEvent::variant), so which phrase speaks is the seed's decision; a note without one (lane 0:
        // an older MIDI file, a hand-made test event) keeps the position-only seed it always had.
        const uint64_t where = static_cast<uint64_t>(std::llround(e.beat * 16.0));
        const uint64_t pick = e.lane != 0 ? mixSeed(where ^ (static_cast<uint64_t>(e.lane) << 40), static_cast<uint64_t>(type) + 0x5641ull)
                                          : mixSeed(where, static_cast<uint64_t>(type) + 0x5053ull);
        switch (sfxTypePart(t)) {
        case Part::Texture:
            texture_.trigger(t, n, vel, late, spb, pick);
            break;
        case Part::Vocal: {
            vocal_.trigger(t, n, vel, late, spb, pick);
            // The send chain detunes each phrase by its own few hertz, up or down, and opens the flanger.
            Rng r;
            r.seed(pick ^ 0x4D4F54ull);
            const float hz = (8.0f + 17.0f * r.uniform()) * (r.below(2) == 0 ? 1.0f : -1.0f);
            sendMotion_ = Motion{ e.beat, std::max(1.0, static_cast<double>(e.length)), hz, 0.3f };
            break;
        }
        default:
            if (t == SfxType::Stutter) {
                // A sixteenth repeated, or a thirty-second for a stutter of half a beat or less.
                const int slice = static_cast<int>(std::lround(spb * (e.length <= 0.5f ? 0.125 : 0.25)));
                stutter_.trigger(n, slice);
                break;
            }
            sfx_.trigger(t, n, vel, late, e.lane);   // the lane carries the bank preset (23.09.2026, SfxEvent::variant)
            // The event half of the SFX chain's automation: a riser drags the shifter up with it, a
            // downlifter down, the reverse effects lift it a little, and a sweep opens the flanger.
            // The shift and the flanger are separate strands, so that the sweep that falls into a drop
            // opens the flanger without taking the riser's climb away from its last two bars.
            switch (t) {
            case SfxType::Riser:        sfxShift_ = Motion{ e.beat, e.length, 150.0f, 0.0f }; sfxFlange_ = Motion{ e.beat, e.length, 0.0f, 0.2f }; break;
            case SfxType::Downlifter:   sfxShift_ = Motion{ e.beat, e.length, -120.0f, 0.0f }; sfxFlange_ = Motion{ e.beat, e.length, 0.0f, 0.2f }; break;
            case SfxType::ReverseSwell: sfxShift_ = Motion{ e.beat, e.length, 40.0f, 0.0f }; sfxFlange_ = Motion{ e.beat, e.length, 0.0f, 0.3f }; break;
            case SfxType::ReverseCrash: sfxShift_ = Motion{ e.beat, e.length, 80.0f, 0.0f }; sfxFlange_ = Motion{ e.beat, e.length, 0.0f, 0.3f }; break;
            case SfxType::Sweep:        sfxFlange_ = Motion{ e.beat, e.length, 0.0f, 0.5f }; break;
            default: break;
            }
            applyMotion();
            break;
        }
        break;
    }
    default:
        break;
    }
}

void Engine::applyMotion()
{
    // The event half of the chains' automation (Engine.h): evaluated from the chunk's beat, so it is a
    // function of the score alone. The shift ramps with the event (a riser's shift climbs to its end),
    // the flanger opens and closes on a half sine; once the event is over both are back at zero.
    auto eval = [&](const Motion& m, float& hz, float& flange) {
        hz = 0.0f;
        flange = 0.0f;
        if (m.length <= 0.0) return;
        const double x = (chunkBeat_ - m.start) / m.length;
        if (x < 0.0 || x >= 1.0) return;
        hz = static_cast<float>(m.shiftHz * x) * motion_;
        flange = static_cast<float>(m.flange * std::sin(kPiD * x)) * motion_;
    };
    float hz, fl, unused;
    eval(sfxShift_, hz, unused);
    eval(sfxFlange_, unused, fl);
    sfxFx_.setMotion(hz, fl);
    // A phrase's detune holds while it speaks and fades over its last quarter.
    eval(sendMotion_, hz, fl);
    if (sendMotion_.length > 0.0 && sendMotion_.shiftHz != 0.0f) {
        const double x = (chunkBeat_ - sendMotion_.start) / sendMotion_.length;
        hz = (x >= 0.0 && x < 1.0) ? sendMotion_.shiftHz * motion_ * static_cast<float>(std::min(1.0, 4.0 * (1.0 - x))) : 0.0f;
    }
    sendFx_.setMotion(hz, fl);
}

void Engine::renderSegment(float* L, float* R, int offset, int count)
{
    flushParams();
    if (count <= 0) return;
    kick_.process(kickBuf_.data(), count);
    bass_.process(bassBuf_.data(), count);
    perc_.process(percL_.data(), percR_.data(), count);
    acid_.process(acidL_.data(), acidR_.data(), count);
    for (int k = 0; k < kPolyInstances; ++k) poly_[k].process(polyL_[k].data(), polyR_[k].data(), count);
    // The voice's modulation insert, between the voice and its strip (Engine.h, polyFlanger_). Its LFO
    // reads the absolute beat of each sample, so a bar rendered alone moves exactly as the same bar
    // inside a whole render and the host's block boundaries decide nothing.
    {
        const double beatAt0 = chunkBeat_ + static_cast<double>(chunkPos_) * beatsPerSample_;
        for (int k = 0; k < kPolyInstances; ++k) {
            if (polyMod_[k] == static_cast<int>(PolyMod::Off)) continue;
            const bool ph = polyMod_[k] == static_cast<int>(PolyMod::Phaser);
            for (int i = 0; i < count; ++i) {
                const double beat = beatAt0 + static_cast<double>(i) * beatsPerSample_;
                if (ph) polyPhaser_[k].tick(polyL_[k][static_cast<size_t>(i)], polyR_[k][static_cast<size_t>(i)], beat);
                else polyFlanger_[k].tick(polyL_[k][static_cast<size_t>(i)], polyR_[k][static_cast<size_t>(i)], beat);
            }
        }
    }
    sfx_.processSplit(sfxL_.data(), sfxR_.data(), subBuf_.data(), sfxWetL_.data(), sfxWetR_.data(), count);
    texture_.process(texL_.data(), texR_.data(), count);
    vocal_.process(vocL_.data(), vocR_.data(), vocThrow_.data(), count);
    const double beat0 = chunkBeat_ + static_cast<double>(chunkPos_) * beatsPerSample_;
    // The SFX strip's insert: flanger, phaser, frequency shifter (PsyFx.h).
    sfxFx_.process(sfxL_.data(), sfxR_.data(), count, beat0, beatsPerSample_);
    const float kg = kickMute_ ? 0.0f : 1.0f, bg = bassMute_ ? 0.0f : 1.0f;
    const float* srcL[StripCount] = {};
    const float* srcR[StripCount] = {};
    srcL[StripPerc] = percL_.data(); srcR[StripPerc] = percR_.data();
    srcL[StripAcid] = acidL_.data(); srcR[StripAcid] = acidR_.data();
    for (int k = 0; k < kPolyInstances; ++k) { srcL[StripLead + k] = polyL_[k].data(); srcR[StripLead + k] = polyR_[k].data(); }
    srcL[StripSfx] = sfxL_.data(); srcR[StripSfx] = sfxR_.data();
    srcL[StripTexture] = texL_.data(); srcR[StripTexture] = texR_.data();
    srcL[StripVocal] = vocL_.data(); srcR[StripVocal] = vocR_.data();
    float* outL = L + offset;
    float* outR = R + offset;
    for (int i = 0; i < count; ++i) {
        const size_t si = static_cast<size_t>(i);
        // Kick and bass are mono and centred, and so is the sub drop, under its own duck; everything
        // else brings the stereo.
        const float sub = stripGain_[StripSfx] * subDuck_.next() * subBuf_[si];
        const float mono = kg * kickBuf_[si] + bg * bassBuf_[si] + sub;
        const size_t oi = static_cast<size_t>(offset + i);   // this sample's index in the caller's buffers (and the stem tap's)
        if (tap_ != nullptr) {
            // The stems (StemTap): the mono parts here, the strips in the loop below, the returns after it.
            const float k = masterGain_ * kg * kickBuf_[si], bs = masterGain_ * bg * bassBuf_[si];
            tap_->L[static_cast<int>(Part::Kick)][oi] = k;  tap_->R[static_cast<int>(Part::Kick)][oi] = k;
            tap_->L[static_cast<int>(Part::Bass)][oi] = bs; tap_->R[static_cast<int>(Part::Bass)][oi] = bs;
        }
        float l = mono, r = mono, rl = 0.0f, rr = 0.0f, hl = 0.0f, hr = 0.0f, fl = 0.0f, fr = 0.0f;
        float hgl = 0.0f, hgr = 0.0f;   // the gated hall's input: the strips whose hall_gate is on
        float ml = 0.0f, mr = 0.0f;   // the melodic bus a stutter may replace (acid, lead, counter, arp, stab)
        const double beat = chunkBeat_ + static_cast<double>(chunkPos_ + i) * beatsPerSample_;
        for (int s = 0; s < StripCount; ++s) {
            float sl = srcL[s][si], sr = srcR[s][si];
            if (s >= StripLead && s <= StripDrone) {
                const int k = s - StripLead;
                if (gateOn_[k]) {
                    const float o = TranceGate::open(beat, gatePattern_[k], gateDuty_[k], gateAttack_[k], gateRelease_[k]);
                    gate_[k].apply(sl, sr, o, gateDepth_[k], gateTone_[k]);
                }
            }
            const float g = stripGain_[s] * duck_[s].next();
            sl *= g;
            sr *= g;
            if (tap_ != nullptr) {
                // Strip s is part s + 2 (Perc .. Vocal follow Kick and Bass in both enums); the sub drop is the effects'.
                const int part = s + static_cast<int>(Part::Perc);
                const float extra = s == StripSfx ? sub : 0.0f;
                tap_->L[part][oi] = masterGain_ * (sl + extra);
                tap_->R[part][oi] = masterGain_ * (sr + extra);
            }
            if (s >= StripAcid && s <= StripStab) { ml += sl; mr += sr; }
            else { l += sl; r += sr; }
            rl += stripRoom_[s] * sl;
            rr += stripRoom_[s] * sr;
            // hall_gate on: this strip's hall send feeds the gated bus instead of the plain hall (never
            // both -- a voice picks one hall or the other).
            if (hallGateOn_[s]) { hgl += stripHall_[s] * sl; hgr += stripHall_[s] * sr; }
            else { hl += stripHall_[s] * sl; hr += stripHall_[s] * sr; }
            fl += stripFx_[s] * sl;
            fr += stripFx_[s] * sr;
            if (s == StripVocal) throwIn_[si] = 0.5f * (sl + sr) * vocThrow_[si] * throwSend_;
        }
        // The glitch: while a stutter runs, the melodic bus is its repeat (the sends above keep the
        // live signal, so the reverb tails run on underneath).
        stutter_.tick(ml, mr);
        outL[i] = l + ml;
        outR[i] = r + mr;
        // The returns' stem holds the dry sum for now; after the returns are added it becomes their share.
        if (tap_ != nullptr) { tap_->L[kNumParts][oi] = outL[i]; tap_->R[kNumParts][oi] = outR[i]; }
        roomInL_[si] = rl; roomInR_[si] = rr;
        // The wandering SFX voices' growing reverb-send trajectory (Sfx.h, sfx.wander) joins the plain
        // hall directly here -- it already left the dry mix in Sfx::processSplit(), so it is not counted
        // again through stripHall_[StripSfx] above, and it bypasses the gated hall (Engine.h has why).
        hallInL_[si] = hl + sfxWetL_[si]; hallInR_[si] = hr + sfxWetR_[si];
        hallGateInL_[si] = hgl; hallGateInR_[si] = hgr;
        sendL_[si] = fl; sendR_[si] = fr;
    }
    room_.process(roomInL_.data(), roomInR_.data(), roomOutL_.data(), roomOutR_.data(), count);
    hall_.process(hallInL_.data(), hallInR_.data(), hallOutL_.data(), hallOutR_.data(), count);
    // The gated hall: ducked by its own send (Reverb.h, processDucked), then cut hard on the absolute
    // bar line (Reverb::barGate, a function of the beat alone -- the same reason TranceGate::open above
    // is called per sample instead of once per block: a bar rendered alone must close at the same
    // instant as the same bar inside a whole set).
    hallGate_.processDucked(hallGateInL_.data(), hallGateInR_.data(), hallGateOutL_.data(), hallGateOutR_.data(), count);
    for (int i = 0; i < count; ++i) {
        const double beat = chunkBeat_ + static_cast<double>(chunkPos_ + i) * beatsPerSample_;
        const float barCut = Reverb::barGate(beat, hallGateCloseBeats_, hallGateHoldBeats_, hallGateOpenBeats_, 0.0f);
        hallGateOutL_[static_cast<size_t>(i)] *= barCut;
        hallGateOutR_[static_cast<size_t>(i)] *= barCut;
    }
    // The modulation send of the bed and the voices, and the voices' delay throw.
    sendFx_.process(sendL_.data(), sendR_.data(), count, beat0, beatsPerSample_);
    for (int i = 0; i < count; ++i) {
        outL[i] += sendReturn_ * sendL_[static_cast<size_t>(i)];
        outR[i] += sendReturn_ * sendR_[static_cast<size_t>(i)];
    }
    throw_.process(throwIn_.data(), outL, outR, count);
    for (int i = 0; i < count; ++i) {
        const size_t si = static_cast<size_t>(i);
        const float d = returnDuck_.next();
        // The gated hall shares the plain hall's return trim (fx.hall_return): it is the same hall
        // knob-wise, only gated for the voices routed into it. It ducks under the kick with everyone
        // else's returns too (returnDuck_), on top of its own gate.
        outL[i] = (outL[i] + d * (roomReturn_ * roomOutL_[si] + hallReturn_ * (hallOutL_[si] + hallGateOutL_[si]))) * masterGain_;
        outR[i] = (outR[i] + d * (roomReturn_ * roomOutR_[si] + hallReturn_ * (hallOutR_[si] + hallGateOutR_[si]))) * masterGain_;
        if (tap_ != nullptr) {
            const size_t oi = static_cast<size_t>(offset + i);
            tap_->L[kNumParts][oi] = outL[i] - masterGain_ * tap_->L[kNumParts][oi];
            tap_->R[kNumParts][oi] = outR[i] - masterGain_ * tap_->R[kNumParts][oi];
        }
    }
    // Master: bus compressor, mono bass, limiter, safety clip, meter.
    comp_.process(outL, outR, count);
    for (int i = 0; i < count; ++i) {
        const float m = 0.5f * (outL[i] + outR[i]);
        float s = 0.5f * (outL[i] - outR[i]);
        float lp, bp, hp;
        sideHp1_.tick(s, lp, bp, hp);
        sideHp2_.tick(hp, lp, bp, s);
        outL[i] = m + s;
        outR[i] = m - s;
    }
    if (clipperOn_) {
        // Soft clip at twice the rate: the kick's transients are rounded here instead of pulling the
        // limiter's gain down for the whole mix.
        const float T = clipperT_;
        for (int i = 0; i < count; ++i) {
            float a, b;
            clipUpL_.process(outL[i], a, b);
            outL[i] = clipDownL_.process(kneeClip(a, T), kneeClip(b, T));
            clipUpR_.process(outR[i], a, b);
            outR[i] = clipDownR_.process(kneeClip(a, T), kneeClip(b, T));
        }
    }
    // The upper end of the programme, before the limiter reads it. Everything above 20 kHz is cost
    // without a listener: it throws the true-peak estimate off by 0.9 dB, it folds down when anything
    // resamples the render to 44.1 kHz, and the clipper above turns it into difference tones inside
    // the mix. See Dsp.h (BandLimit) for the measurements behind the corner.
    for (int i = 0; i < count; ++i) {
        outL[i] = bandLimitL_.process(outL[i]);
        outR[i] = bandLimitR_.process(outR[i]);
    }
    if (limiterOn_) limiter_.process(outL, outR, count);
    if (clip_) {
        const float invCeil = 1.0f / ceiling_;
        for (int i = 0; i < count; ++i) {
            if (limiterOn_) {
                // Only a safety net: the limiter already keeps the true peak at the ceiling. A plain clip,
                // not an antiderivative one -- that would average neighbouring samples even below the ceiling.
                outL[i] = clampv(outL[i], -ceiling_, ceiling_);
                outR[i] = clampv(outR[i], -ceiling_, ceiling_);
            } else {
                outL[i] = ceiling_ * clipL_(outL[i] * invCeil);
                outR[i] = ceiling_ * clipR_(outR[i] * invCeil);
            }
        }
    }
    meter_.process(outL, outR, count);
    chunkPos_ += count;
    samples_ += static_cast<uint64_t>(count);
}
void Engine::process(float* L, float* R, int n)
{
    const DenormalGuard guard;
    int done = 0;
    while (done < n) {
        if (chunkPos_ == 0) {
            // Out-of-band corrections first (pushImmediate): due now, whatever the queue holds ahead of them.
            ControlEvent now;
            while (immediate_.pop(now)) {
                if (now.beat < chunkBeat_) now.beat = chunkBeat_;
                dispatchControl(now);
            }
            advanceRamps();
            applyParams();
            applyMotion();
        }
        flushParams();
        int left = std::min(n - done, kChunk - chunkPos_);

        for (;;) {
            NoteEvent ne;
            ControlEvent ce;
            const bool hasNote = notes_.peek(ne), hasCtl = controls_.peek(ce);
            if ((!hasNote && !hasCtl) || left <= 0) break;
            // Controls first at equal beats.
            const bool ctl = hasCtl && (!hasNote || ce.beat <= ne.beat);
            const double beat = ctl ? ce.beat : ne.beat;
            const double rel = (beat - chunkBeat_) / beatsPerSample_;
            int m = rel <= 0.0 ? 0 : static_cast<int>(std::ceil(rel - 1e-7));
            if (m < chunkPos_) m = chunkPos_;
            if (m >= chunkPos_ + left) break;
            const int before = m - chunkPos_;
            renderSegment(L, R, done, before);
            done += before;
            left -= before;
            // How far past the ideal instant this sample lies; late events count as on time.
            double late = static_cast<double>(m) - rel;
            if (late < 0.0 || late >= 1.0) late = 0.0;
            if (ctl) { controls_.pop(ce); dispatchControl(ce); }
            else { notes_.pop(ne); dispatch(ne, late); }
        }
        renderSegment(L, R, done, left);
        done += left;

        if (chunkPos_ >= kChunk) {
            chunkBeat_ += kChunk * beatsPerSample_;
            chunkPos_ = 0;
        }
        beatNow_.store(chunkBeat_ + chunkPos_ * beatsPerSample_, std::memory_order_relaxed);
    }
}

} // namespace phos
