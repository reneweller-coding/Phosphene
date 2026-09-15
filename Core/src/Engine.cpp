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
} // namespace

Engine::Engine() : notes_(16384), controls_(16384)
{
    var_ = std::make_unique<Variation[]>(static_cast<size_t>(params_.count()));
    eff_.assign(static_cast<size_t>(params_.count()), 0.0f);
}

void Engine::prepare(double sampleRate, int /*maxBlockSize*/)
{
    sr_ = sampleRate;
    kickBuf_.assign(static_cast<size_t>(kChunk), 0.0f);
    bassBuf_.assign(static_cast<size_t>(kChunk), 0.0f);
    percL_.assign(static_cast<size_t>(kChunk), 0.0f);
    percR_.assign(static_cast<size_t>(kChunk), 0.0f);
    kick_.prepare(sr_);
    bass_.prepare(sr_);
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
    clipL_.reset();
    clipR_.reset();
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
    const int ms = p.base(Module::Master);
    masterGain_ = dbToGain(eff_[static_cast<size_t>(ms + master::Gain)] + eff_[static_cast<size_t>(mb + mix::TrackGain)]);
    ceiling_ = dbToGain(eff_[static_cast<size_t>(ms + master::Ceiling)]);
    clip_ = eff_[static_cast<size_t>(ms + master::Clip)] >= 0.5f;
}

void Engine::dispatch(const NoteEvent& e, double late)
{
    const float vel = static_cast<float>(e.velocity) / 127.0f;
    switch (e.part) {
    case Part::Kick:
        kick_.trigger(vel, late);
        bass_.duck(late);
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
    const float kg = kickMute_ ? 0.0f : 1.0f, bg = bassMute_ ? 0.0f : 1.0f;
    const float invCeil = 1.0f / ceiling_;
    for (int i = 0; i < count; ++i) {
        // Kick and bass are mono and centred; the percussion brings the stereo.
        const float mono = kg * kickBuf_[static_cast<size_t>(i)] + bg * bassBuf_[static_cast<size_t>(i)];
        float l = (mono + percGain_ * percL_[static_cast<size_t>(i)]) * masterGain_;
        float r = (mono + percGain_ * percR_[static_cast<size_t>(i)]) * masterGain_;
        if (clip_) {
            l = ceiling_ * clipL_(l * invCeil);
            r = ceiling_ * clipR_(r * invCeil);
        }
        L[offset + i] = l;
        R[offset + i] = r;
    }
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
