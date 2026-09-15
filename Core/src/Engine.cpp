/**
 * @file Engine.cpp
 * @brief Engine implementation.
 */
#include "phos/Engine.h"
#include "phos/Patterns.h"
#include <algorithm>
#include <cmath>

namespace phos {

namespace {
constexpr double kPiD = 3.141592653589793;
bool isDiscreteCurve(Curve c) { return c == Curve::Int || c == Curve::Choice || c == Curve::Toggle; }

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

Engine::Engine() : notes_(16384), controls_(16384)
{
    var_ = std::make_unique<Variation[]>(static_cast<size_t>(params_.count()));
    eff_.assign(static_cast<size_t>(params_.count()), 0.0f);
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
    for (int i = 0; i < kPolyInstances; ++i) {
        polyL_[i].assign(static_cast<size_t>(kChunk), 0.0f);
        polyR_[i].assign(static_cast<size_t>(kChunk), 0.0f);
        poly_[i].prepare(sr_);
        poly_[i].setQuality(quality_.polyUnison[i], quality_.polyVoices[i]);
    }
    for (auto* b : { &sfxL_, &sfxR_, &roomInL_, &roomInR_, &hallInL_, &hallInR_, &roomOutL_, &roomOutR_, &hallOutL_, &hallOutR_ })
        b->assign(static_cast<size_t>(kChunk), 0.0f);
    acid_.prepare(sr_);
    acid_.setOversampling(quality_.acidOversampling);
    sfx_.prepare(sr_);
    for (Ducker& d : duck_) d.prepare(sr_);
    returnDuck_.prepare(sr_);
    for (TranceGate& g : gate_) g.prepare(sr_);
    room_.prepare(sr_);
    hall_.prepare(sr_);
    comp_.prepare(sr_);
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

void Engine::reset()
{
    NoteEvent e;
    while (notes_.pop(e)) {}
    ControlEvent c;
    while (controls_.pop(c)) {}
    for (int i = 0; i < params_.count(); ++i) var_[static_cast<size_t>(i)] = Variation{};
    samples_ = 0;
    chunkBeat_ = 0.0;
    chunkPos_ = 0;
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
    for (Ducker& d : duck_) d.reset();
    returnDuck_.reset();
    for (TranceGate& g : gate_) g.reset();
    room_.reset();
    hall_.reset();
    comp_.reset();
    sideHp1_.reset();
    sideHp2_.reset();
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
        if (x >= 1.0) { v.offset = v.to; v.length = 0.0; continue; }
        const double s = x <= 0.0 ? 0.0 : 0.5 - 0.5 * std::cos(kPiD * x);   // raised cosine: C1 at both ends
        v.offset = v.from + static_cast<float>(s) * (v.to - v.from);
    }
}

void Engine::dispatchControl(const ControlEvent& e)
{
    if (e.param < 0 || e.param >= params_.count()) return;
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
    applyParams();
}

double Engine::firstSlotSeconds() const
{
    return firstBassSlot(pattern_) / (beatsPerSample_ * sr_);
}

void Engine::applyParams()
{
    const ParamStore& p = params_;
    // Effective values: knob + normalised offset for continuous parameters, override for discrete.
    for (int i = 0; i < p.count(); ++i) {
        const Variation& v = var_[static_cast<size_t>(i)];
        const float knob = p.get(i);
        float value = knob;
        if (isDiscreteCurve(p.desc(i).curve)) {
            if (v.override >= 0.0f) value = v.override;
        } else if (v.offset != 0.0f) {
            value = p.fromNormalised(i, p.toNormalised(i, knob) + v.offset);
        }
        eff_[static_cast<size_t>(i)] = value;
    }

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
    acid_.update(eff_.data() + p.base(Module::Acid), bpmNow);
    for (int i = 0; i < kPolyInstances; ++i) poly_[i].update(eff_.data() + p.base(Module::Poly, i), bpmNow);

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
    stripGain_[StripLead] = partGain(mix::LeadMute, mix::LeadLevel);
    stripGain_[StripArp] = partGain(mix::ArpMute, mix::ArpLevel);
    stripGain_[StripPad] = partGain(mix::PadMute, mix::PadLevel);
    stripGain_[StripSfx] = partGain(mix::SfxMute, mix::SfxLevel);
    const float duckA = e(mb + mix::DuckAttack), duckH = e(mb + mix::DuckHold), duckR = e(mb + mix::DuckRelease);
    stripRoom_[StripPerc] = e(mb + mix::PercRoom);
    stripHall_[StripPerc] = e(mb + mix::PercHall);
    duck_[StripPerc].set(0.0f, duckA, duckH, duckR);
    stripRoom_[StripAcid] = e(ab + acid::RoomSend);
    stripHall_[StripAcid] = e(ab + acid::HallSend);
    duck_[StripAcid].set(e(ab + acid::Duck), duckA, duckH, duckR);
    stripRoom_[StripSfx] = e(sb + sfx::RoomSend);
    stripHall_[StripSfx] = e(sb + sfx::HallSend);
    duck_[StripSfx].set(e(sb + sfx::Duck), duckA, duckH, duckR);
    sfx_.update(eff_.data() + sb, keyRoot_);
    const double beatSeconds = beatsPerSample_ > 0.0 ? 1.0 / (beatsPerSample_ * sr_) : 60.0 / 145.0;
    for (int k = 0; k < kPolyInstances; ++k) {
        const int pb = p.base(Module::Poly, k);
        const int strip = StripLead + k;
        stripRoom_[strip] = e(pb + poly::RoomSend);
        stripHall_[strip] = e(pb + poly::HallSend);
        duck_[strip].set(e(pb + poly::Duck), duckA, duckH, duckR);
        gateOn_[k] = e(pb + poly::Gate) >= 0.5f;
        gatePattern_[k] = static_cast<int>(std::lround(e(pb + poly::GatePattern)));
        gateDepth_[k] = e(pb + poly::GateDepth);
        gateDuty_[k] = e(pb + poly::GateDuty);
        gateTone_[k] = e(pb + poly::GateTone);
        gateAttack_[k] = e(pb + poly::GateAttack) * 0.001 / beatSeconds;
        gateRelease_[k] = e(pb + poly::GateRelease) * 0.001 / beatSeconds;
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
    const float vel = static_cast<float>(e.velocity) / 127.0f;
    switch (e.part) {
    case Part::Kick:
        kick_.trigger(vel, late);
        bass_.duck(late);
        for (Ducker& d : duck_) d.trigger(late);
        returnDuck_.trigger(late);
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
    case Part::Arp:
    case Part::Pad: {
        const double samples = static_cast<double>(e.length) / beatsPerSample_;
        const int inst = e.part == Part::Lead ? 0 : (e.part == Part::Arp ? 1 : 2);
        // noteOnLimited applies the quality level's unison and voice limits (Quality.h, Poly.h); at
        // the desktop level it is noteOn() itself.
        poly_[inst].noteOnLimited(e.pitch, vel, e.length, std::max(1, static_cast<int>(std::lround(samples))), late);
        break;
    }
    case Part::Sfx: {
        const int type = static_cast<int>(e.pitch) - kSfxBaseNote;
        if (type >= 0 && type < kNumSfxTypes) {
            const double samples = static_cast<double>(e.length) / beatsPerSample_;
            sfx_.trigger(static_cast<SfxType>(type), std::max(1, static_cast<int>(std::lround(samples))), vel, late);
        }
        break;
    }
    default:
        break;
    }
}

void Engine::renderSegment(float* L, float* R, int offset, int count)
{
    if (count <= 0) return;
    kick_.process(kickBuf_.data(), count);
    bass_.process(bassBuf_.data(), count);
    perc_.process(percL_.data(), percR_.data(), count);
    acid_.process(acidL_.data(), acidR_.data(), count);
    for (int k = 0; k < kPolyInstances; ++k) poly_[k].process(polyL_[k].data(), polyR_[k].data(), count);
    sfx_.process(sfxL_.data(), sfxR_.data(), count);
    const float kg = kickMute_ ? 0.0f : 1.0f, bg = bassMute_ ? 0.0f : 1.0f;
    const float* srcL[StripCount] = { percL_.data(), acidL_.data(), polyL_[0].data(), polyL_[1].data(), polyL_[2].data(), sfxL_.data() };
    const float* srcR[StripCount] = { percR_.data(), acidR_.data(), polyR_[0].data(), polyR_[1].data(), polyR_[2].data(), sfxR_.data() };
    float* outL = L + offset;
    float* outR = R + offset;
    for (int i = 0; i < count; ++i) {
        const size_t si = static_cast<size_t>(i);
        // Kick and bass are mono and centred; everything else brings the stereo.
        const float mono = kg * kickBuf_[si] + bg * bassBuf_[si];
        float l = mono, r = mono, rl = 0.0f, rr = 0.0f, hl = 0.0f, hr = 0.0f;
        const double beat = chunkBeat_ + static_cast<double>(chunkPos_ + i) * beatsPerSample_;
        for (int s = 0; s < StripCount; ++s) {
            float sl = srcL[s][si], sr = srcR[s][si];
            if (s >= StripLead && s <= StripPad) {
                const int k = s - StripLead;
                if (gateOn_[k]) {
                    const float o = TranceGate::open(beat, gatePattern_[k], gateDuty_[k], gateAttack_[k], gateRelease_[k]);
                    gate_[k].apply(sl, sr, o, gateDepth_[k], gateTone_[k]);
                }
            }
            const float g = stripGain_[s] * duck_[s].next();
            sl *= g;
            sr *= g;
            l += sl;
            r += sr;
            rl += stripRoom_[s] * sl;
            rr += stripRoom_[s] * sr;
            hl += stripHall_[s] * sl;
            hr += stripHall_[s] * sr;
        }
        outL[i] = l;
        outR[i] = r;
        roomInL_[si] = rl; roomInR_[si] = rr;
        hallInL_[si] = hl; hallInR_[si] = hr;
    }
    room_.process(roomInL_.data(), roomInR_.data(), roomOutL_.data(), roomOutR_.data(), count);
    hall_.process(hallInL_.data(), hallInR_.data(), hallOutL_.data(), hallOutR_.data(), count);
    for (int i = 0; i < count; ++i) {
        const size_t si = static_cast<size_t>(i);
        const float d = returnDuck_.next();
        outL[i] = (outL[i] + d * (roomReturn_ * roomOutL_[si] + hallReturn_ * hallOutL_[si])) * masterGain_;
        outR[i] = (outR[i] + d * (roomReturn_ * roomOutR_[si] + hallReturn_ * hallOutR_[si])) * masterGain_;
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
            advanceRamps();
            applyParams();
        }
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
