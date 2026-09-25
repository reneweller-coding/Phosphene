/**
 * @file Bass.cpp
 * @brief Bass implementation.
 */
#include "phos/Bass.h"
#include "phos/Params.h"
#include <cmath>

namespace phos {

namespace {
/** @brief The decimator for every 2x stage in the core: 96 dB stopband, passband to 0.2 of the high rate. */
const HalfbandDesign& bassHalfband()
{
    static const HalfbandDesign d = designHalfband(96.0, 0.1);
    return d;
}
constexpr float kResonanceMax = 3.9f;   ///< feedback at Resonance 1: just short of self-oscillation
constexpr float kSubScale = 0.7f;       ///< sub (and sub octave) knob to amplitude: at 0.6 the sine matches the filtered voice's old fundamental
/**
 * @brief Bite knob to amplitude.
 *
 * The layer is a band of a full-scale saw, so its raw level is already comparable to the saw path's;
 * the scale only sets where the knob's middle lands. Calibrated on 19.09.2026 so that the default
 * sound meets the references' bite band: 300 Hz .. 2 kHz at -9.8 dB against 20 .. 120 Hz between the
 * kicks, the median of Tools/ref_bass.py over 24 recordings (docs/rounds/2026-09.md, 19.09.2026).
 */
constexpr float kBiteScale = 1.344f;
/** @brief Butterworth dampings of the two sections: 2 cos(pi/8) and 2 cos(3 pi/8). */
constexpr float kBiteK1 = 1.8477590f, kBiteK2 = 0.7653669f;
/** @brief Corner of the bite's high pass as a fraction of Bite Cutoff (see update()). */
constexpr float kBiteHpRatio = 0.4f;
} // namespace

void Bass::prepare(double sampleRate)
{
    sr_ = sampleRate;
    osRate_ = sr_ * os_;
    amp_.setSampleRate(sr_);
    ducker_.prepare(sr_);
    down_.setup(bassHalfband());
    reset();
}

void Bass::setOversampling(int factor)
{
    os_ = factor <= 1 ? 1 : 2;
    osRate_ = sr_ * os_;
    // The decimator's state belongs to the 2x path; at 1x it is not used, but a later switch back
    // must not start from a ringing it never heard.
    down_.reset();
}

void Bass::reset()
{
    ladder_.reset();
    down_.reset();
    hp1_.reset();
    hp2_.reset();
    hp3_.reset();
    hp4_.reset();
    bite1_.reset();
    bite2_.reset();
    biteHp_.reset();
    amp_.kill();
    ducker_.reset();
    osc_ = VaOscillator{};
    gate_ = 0;
    fenv_ = 0.0f;
    biteEnv_ = 0.0f;
    subPhase_ = 0.0;
}

void Bass::update(const float* v)
{
    wave_ = v[bass::Wave];
    pw_ = v[bass::PulseWidth];
    subLevel_ = v[bass::Sub] * kSubScale;
    octLevel_ = v[bass::SubOctave] * kSubScale;
    subMode_ = static_cast<int>(std::lround(v[bass::SubMode]));
    splitRatio_ = v[bass::SplitRatio];
    retrigger_ = v[bass::Retrigger] >= 0.5f;
    startPhase_ = v[bass::StartPhase];
    cutoff_ = v[bass::Cutoff];
    k_ = kResonanceMax * v[bass::Resonance];
    envOct_ = v[bass::EnvAmount];
    keyTrack_ = v[bass::KeyTrack];
    velCut_ = v[bass::VelToCutoff];
    fDecay_ = static_cast<float>(std::exp(std::log(1.0e-3) / (v[bass::FilterDecay] * 0.001 * sr_)));
    const float drive = v[bass::Drive];
    driveIn_ = 0.5f + 3.5f * drive;
    driveOut_ = 1.0f / (0.5f + 1.5f * drive);
    attack_ = v[bass::AmpAttack] * 0.001f;
    decay_ = v[bass::AmpDecay] * 0.001f;
    sustain_ = v[bass::AmpSustain];
    release_ = v[bass::AmpRelease] * 0.001f;
    ducker_.set(v[bass::DuckDepth], 1.0f, v[bass::DuckHold], v[bass::DuckRelease]);
    level_ = dbToGain(v[bass::Level]);
    biteLevel_ = v[bass::Bite] * kBiteScale;
    biteCut_ = v[bass::BiteCutoff];
    biteEnvOct_ = v[bass::BiteEnv];
    // Per sample at the rate the voice runs at: the envelope is advanced once per oscillator step.
    biteDecay_ = static_cast<float>(std::exp(std::log(1.0e-3) / (v[bass::BiteDecay] * 0.001 * osRate_)));
    biteGain_ = 1.0f + 7.0f * v[bass::BiteDrive];
    biteNorm_ = 1.0f / std::tanh(biteGain_);
    // Resonance 0..1 narrows the second section from Butterworth towards a Q of about 9.
    biteK2_ = kBiteK2 * (1.0f - 0.85f * clampv(v[bass::BiteResonance], 0.0f, 1.0f));
    // The bite's floor: a Butterworth high pass at kBiteHpRatio of the resting cutoff. The bite is the
    // band above the low mids; without the floor its 3rd to 6th harmonics (140 .. 280 Hz on F#1) came
    // out as strong as its 300 Hz .. 2 kHz band and thickened the low mids, which the mix already
    // carries 4 dB over the references (docs/rounds/2026-09.md, 18.09.2026).
    biteHp_.setQ(std::max(40.0f, kBiteHpRatio * biteCut_), 0.70710678f, static_cast<float>(osRate_));
    applyNoteSettings();
}

void Bass::applyNoteSettings()
{
    const double f0 = midiToHz(pitch_);
    // Release floor: half a period of the fundamental.
    releaseUsed_ = std::max(release_, static_cast<float>(0.5 / f0));
    amp_.setTimes(attack_, decay_, sustain_, releaseUsed_);
    // The Split high pass: a fourth-order Butterworth squared (Linkwitz-Riley, 8th order, 48 dB per
    // octave), the same two Butterworth dampings as the bite's low pass, each section twice.
    const float fc = static_cast<float>(std::min(f0 * splitRatio_, 0.45 * sr_));
    hp1_.setK(fc, kBiteK1, static_cast<float>(sr_));
    hp2_.setK(fc, kBiteK2, static_cast<float>(sr_));
    hp3_.copyCoefficients(hp1_);
    hp4_.copyCoefficients(hp2_);
}

void Bass::noteOn(int pitch, float velocity, int gateSamples, double late, double fundamentalPhase)
{
    pitch_ = pitch;
    velocity_ = clampv(velocity, 0.0f, 1.0f);
    gate_ = std::max(1, gateSamples);
    const double f0 = midiToHz(pitch_);
    osc_.set(f0, osRate_, wave_, pw_);
    if (retrigger_) {
        // When the previous note has died away (-60 dB), the filters start from rest as well: the
        // oscillator keeps running between notes, and its ringing in the ladder, the decimator and
        // the 48 dB high pass would otherwise make every note begin a little differently. Under
        // -60 dB the reset cannot be heard; a note that is still sounding keeps its filter states.
        if (amp_.level() < 1.0e-3f) {
            ladder_.reset();
            down_.reset();
            hp1_.reset();
            hp2_.reset();
            hp3_.reset();
            hp4_.reset();
            bite1_.reset();
            bite2_.reset();
            biteHp_.reset();
        }
        osc_.restart(fundamentalPhase, static_cast<double>(os_) * late);
        const double p = fundamentalPhase + f0 * late / sr_;
        subPhase_ = p - std::floor(p);
    }
    applyNoteSettings();
    // Sub-sample onset for the envelopes too: the filter envelope and the attack start `late`
    // samples into their curves, like the oscillators.
    fenv_ = static_cast<float>(std::pow(static_cast<double>(fDecay_), late));
    // The bite envelope steps once per oscillator sample, so `late` counts at the voice's rate.
    biteEnv_ = static_cast<float>(std::pow(static_cast<double>(biteDecay_), static_cast<double>(os_) * late));
    amp_.noteOn();
    amp_.advanceAttack(late);
}

void Bass::process(float* out, int n)
{
    const float nyq = static_cast<float>(0.45 * osRate_);
    const double f0 = midiToHz(pitch_);
    osc_.set(f0, osRate_, wave_, pw_);
    const double subInc = f0 / sr_;
    // Key tracking relative to E1 (MIDI 28), the bottom of the usual psytrance bass register.
    const float track = keyTrack_ * static_cast<float>(pitch_ - 28) / 12.0f;
    const float velScale = 1.0f - velCut_ + velCut_ * velocity_;
    for (int i = 0; i < n; ++i) {
        const float oct = envOct_ * fenv_ * velScale + track;
        const float fc = clampv(cutoff_ * std::pow(2.0f, oct), 20.0f, nyq);
        const float g = std::tan(kPi * fc / static_cast<float>(osRate_));
        fenv_ *= fDecay_;

        // 2x: two ladder steps, decimated back. 1x (Quality::Quest, not used for the bass today):
        // one step, no decimator -- the PolyBLEP oscillator still band-limits its own discontinuity,
        // only the ladder's distortion products are no longer kept out of the audible band.
        // The bite's cutoff, once per output sample and shared by both oversampled steps; its
        // envelope follows the same velocity scaling and key tracking as the saw path's.
        const bool bite = biteLevel_ > 0.0f;
        if (bite) {
            const float bfc = clampv(biteCut_ * std::pow(2.0f, biteEnvOct_ * biteEnv_ * velScale + track), 20.0f, nyq);
            const float bg = std::tan(kPi * bfc / static_cast<float>(osRate_));
            bite1_.setG(bg, kBiteK1);
            bite2_.setG(bg, biteK2_);
        }
        auto biteStep = [&](float s) {
            biteEnv_ *= biteDecay_;
            float lp, bp, hp;
            biteHp_.tick(std::tanh(s * biteGain_) * biteNorm_, lp, bp, hp);
            return bite2_.lp(bite1_.lp(hp)) * biteLevel_;
        };
        const float s0 = osc_.next();
        const float o0 = ladder_.tick(s0 * driveIn_, g, k_, 0.5f) * driveOut_ + (bite ? biteStep(s0) : 0.0f);
        float y;
        if (os_ == 2) {
            const float s1 = osc_.next();
            const float o1 = ladder_.tick(s1 * driveIn_, g, k_, 0.5f) * driveOut_ + (bite ? biteStep(s1) : 0.0f);
            y = down_.process(o0, o1);
        } else {
            y = o0;
        }
        if (subMode_ == static_cast<int>(SubMode::Split)) {
            float lp, bp, hp;
            hp1_.tick(y, lp, bp, hp);
            hp2_.tick(hp, lp, bp, y);
            hp3_.tick(y, lp, bp, hp);
            hp4_.tick(hp, lp, bp, y);
        }
        float sub = subLevel_ > 0.0f ? subLevel_ * static_cast<float>(std::sin(2.0 * 3.141592653589793 * subPhase_)) : 0.0f;
        if (octLevel_ > 0.0f) sub += octLevel_ * static_cast<float>(std::sin(4.0 * 3.141592653589793 * subPhase_));
        subPhase_ += subInc;
        if (subPhase_ >= 1.0) subPhase_ -= 1.0;

        if (gate_ > 0 && --gate_ == 0) amp_.noteOff();
        const float a = amp_.process();
        out[i] = (y + sub) * a * ducker_.next() * level_;
    }
}

} // namespace phos
