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
constexpr float kSubScale = 0.7f;       ///< sub knob to amplitude: at 0.6 the sine matches the filtered voice's old fundamental
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
    amp_.kill();
    ducker_.reset();
    osc_ = VaOscillator{};
    gate_ = 0;
    fenv_ = 0.0f;
    subPhase_ = 0.0;
}

void Bass::update(const float* v)
{
    wave_ = v[bass::Wave];
    pw_ = v[bass::PulseWidth];
    subLevel_ = v[bass::Sub] * kSubScale;
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
    applyNoteSettings();
}

void Bass::applyNoteSettings()
{
    const double f0 = midiToHz(pitch_);
    // Release floor: half a period of the fundamental.
    releaseUsed_ = std::max(release_, static_cast<float>(0.5 / f0));
    amp_.setTimes(attack_, decay_, sustain_, releaseUsed_);
    const float fc = static_cast<float>(std::min(f0 * splitRatio_, 0.45 * sr_));
    hp1_.setQ(fc, 0.70710678f, static_cast<float>(sr_));
    hp2_.copyCoefficients(hp1_);
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
        // the 24 dB high pass would otherwise make every note begin a little differently. Under
        // -60 dB the reset cannot be heard; a note that is still sounding keeps its filter states.
        if (amp_.level() < 1.0e-3f) {
            ladder_.reset();
            down_.reset();
            hp1_.reset();
            hp2_.reset();
        }
        osc_.restart(fundamentalPhase, static_cast<double>(os_) * late);
        const double p = fundamentalPhase + f0 * late / sr_;
        subPhase_ = p - std::floor(p);
    }
    applyNoteSettings();
    // Sub-sample onset for the envelopes too: the filter envelope and the attack start `late`
    // samples into their curves, like the oscillators.
    fenv_ = static_cast<float>(std::pow(static_cast<double>(fDecay_), late));
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
        const float x0 = osc_.next() * driveIn_;
        const float o0 = ladder_.tick(x0, g, k_, 0.5f);
        float y;
        if (os_ == 2) {
            const float x1 = osc_.next() * driveIn_;
            const float o1 = ladder_.tick(x1, g, k_, 0.5f);
            y = down_.process(o0, o1) * driveOut_;
        } else {
            y = o0 * driveOut_;
        }
        if (subMode_ == static_cast<int>(SubMode::Split)) {
            float lp, bp, hp;
            hp1_.tick(y, lp, bp, hp);
            hp2_.tick(hp, lp, bp, y);
        }
        const float sub = subLevel_ > 0.0f ? subLevel_ * static_cast<float>(std::sin(2.0 * 3.141592653589793 * subPhase_)) : 0.0f;
        subPhase_ += subInc;
        if (subPhase_ >= 1.0) subPhase_ -= 1.0;

        if (gate_ > 0 && --gate_ == 0) amp_.noteOff();
        const float a = amp_.process();
        out[i] = (y + sub) * a * ducker_.next() * level_;
    }
}

} // namespace phos
