/**
 * @file Acid.cpp
 * @brief Acid voice implementation.
 */
#include "phos/Acid.h"
#include "phos/Params.h"
#include <algorithm>
#include <cmath>

namespace phos {

namespace {
constexpr double kPiD = 3.141592653589793;
constexpr float kResonanceMax = 16.0f;       ///< feedback at Resonance 1: below the self-oscillation at 17, as on the TB-303
constexpr float kLadderComp = 0.3f;          ///< input gain (1 + 0.3 k) against the passband loss 1/(1 + k)
constexpr double kAccentDecayMax = 0.2;      ///< an accented note's filter envelope lasts at most 200 ms
constexpr double kSweepPulseTau = 0.06;      ///< accent pulse
constexpr double kSweepTau = 0.15;           ///< accent sweep capacitor
constexpr float kSweepOctaves = 2.0f;        ///< sweep depth at full accent, resonance and charge
/**
 * @brief Input gain of the drive stage at Drive = 1, in dB (18.09.2026, round "mix-foundation").
 *
 * Until 18.09.2026 the drive ran 1 + 5 d into the tanh (at most 15.6 dB) and 1 / (1 + 1.5 d) out.
 * Measured on the listening seed, four bars of the first drop, acid alone and dry: crest 25.6 dB at
 * drive 0, 22.2 at the default 0.45 and 19.4 at 1, and the band 3 .. 8 kHz moved by 0.7 dB between
 * 0.45 and 1. The body of a line with a resonant diode ladder sits 20 dB and more under the ladder's
 * resonant peaks, so a stage that only saturates at the peaks never touches the body: the knob did
 * almost nothing to the tone. The psytrance acid is a 303 into a distortion pedal, whose point is
 * exactly that the peaks clip and the body comes up. 30 dB in, sqrt of it out: a quiet body gains
 * 15 dB at full drive while a saturating peak loses 15, so the crest collapses the way a pedal's does.
 * At the old default of 0.45 the small-signal gain is +6.8 dB against the old +5.8 dB, so the default
 * line keeps its level.
 */
constexpr float kDriveMaxDb = 30.0f;

/** @brief The half-band decimator of the acid's 2x oversampling (96 dB stop band, 0.1 transition), designed once. */
const HalfbandDesign& acidHalfband()
{
    static const HalfbandDesign d = designHalfband(96.0, 0.1);
    return d;
}
} // namespace

void Acid::setOversampling(int factor)
{
    os_ = factor <= 1 ? 1 : 2;
    osRate_ = sr_ * os_;
    // At 2x the cutoff must stay inside the decimator's passband (0.2 of the high rate); at 1x there
    // is no decimator and the only limit is the ladder's stability at 0.45 fs. At 48 kHz both land
    // above the 18 kHz cap below, so the two levels reach the same highest cutoff.
    nyqFactor_ = os_ == 2 ? 0.2 : 0.45;
    down_.reset();
}

void Acid::prepare(double sampleRate)
{
    sr_ = sampleRate;
    osRate_ = sr_ * os_;
    amp_.setSampleRate(sr_);
    mod_.prepare(sr_, 0x41434944'4D4F4430ull);   // "ACIDMOD0": one voice, one fixed stream (26.09.2026)
    modRng_.seed(0x41434944'524E4430ull);         // "ACIDRND0"
    down_.setup(acidHalfband());
    delay_.prepare(sr_);
    size_t n = 1;
    while (static_cast<double>(n) < sr_ / 40.0 + 4.0) n <<= 1;
    comb_.assign(n, 0.0f);
    mono_.assign(256, 0.0f);
    send_.assign(256, 0.0f);
    reset();
}

void Acid::reset()
{
    mod_.kill();
    for (float& m : modSum_) m = 0.0f;
    osc_ = VaOscillator{};
    ladder_.reset();
    down_.reset();
    amp_.kill();
    shaper_.reset();
    lc1_.reset();
    lc2_.reset();
    dispState_.reset();
    delay_.reset();
    std::fill(comb_.begin(), comb_.end(), 0.0f);
    combPos_ = 0;
    pitchNow_ = pitchTarget_ = 57.0;
    hz_ = static_cast<float>(midiToHz(57.0));
    gate_ = 0;
    fenv_ = sq_ = pulse_ = sweep_ = 0.0f;
    accentGain_ = accentGainTarget_ = 1.0f;
    slidePending_ = legato_ = accent_ = false;
}

void Acid::update(const float* v, double bpm)
{
    const double sr2 = osRate_;
    wave_ = v[acid::Wave];
    cutoff_ = v[acid::Cutoff];
    resonance_ = v[acid::Resonance];
    k_ = kResonanceMax * resonance_;
    envOct_ = v[acid::EnvAmount];
    fDecay_ = static_cast<float>(std::exp(std::log(1.0e-3) / (v[acid::Decay] * 0.001 * sr2)));
    fDecayAccent_ = static_cast<float>(std::exp(std::log(1.0e-3) / (std::min(static_cast<double>(v[acid::Decay]) * 0.001, kAccentDecayMax) * sr2)));
    accentAmt_ = v[acid::Accent];
    glide_ = static_cast<float>(1.0 - std::exp(-1.0 / (v[acid::SlideTime] * 0.001 * sr_)));
    ampDecay_ = v[acid::AmpDecay] * 0.001f;
    keyTrack_ = v[acid::KeyTrack];
    // Drive: up to kDriveMaxDb into the saturator, and back out by half of that in dB, so a quiet
    // body gains half the drive and a peak that saturates loses the other half (see kDriveMaxDb).
    const float drive = v[acid::Drive];
    driveIn_ = dbToGain(kDriveMaxDb * drive);
    driveOut_ = 1.0f / std::sqrt(driveIn_);
    const bool sq = v[acid::Squelch] >= 0.5f;
    if (sq && !squelch_) std::fill(comb_.begin(), comb_.end(), 0.0f);   // no stale ring from long ago
    squelch_ = sq;
    sqOct_ = static_cast<float>(std::log2(v[acid::SquelchStart]));
    sqDecay_ = static_cast<float>(std::exp(-1.0 / (v[acid::SquelchTime] * 0.001 * sr2)));
    combMix_ = v[acid::CombMix];
    combFb_ = v[acid::CombFeedback];
    lc1_.setQ(std::max(150.0f, v[acid::LowCut]), 0.70710678f, static_cast<float>(sr_));
    lc2_.copyCoefficients(lc1_);
    disperse_.set(static_cast<int>(std::lround(v[acid::Disperse])), v[acid::DisperseFreq], sr_);
    pulseDecay_ = static_cast<float>(std::exp(-1.0 / (kSweepPulseTau * sr2)));
    sweepCharge_ = static_cast<float>(1.0 - std::exp(-1.0 / (kSweepTau * sr2)));
    accentSmooth_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.004 * sr_)));
    level_ = dbToGain(v[acid::Level]);
    // The modulation (26.09.2026): the block from its envelope on (the acid has no filter envelope of the block's).
    static_assert(acid::Mx8Amount - acid::MenvAttack + 1 == kModBlockSize - kModBlockMenv, "the block's layout (Modulation.h)");
    mod_.set(readModBlock(v + acid::MenvAttack - kModBlockMenv, kAcidModDestMap, kAcidModDests));
    modOn_ = mod_.active();
    kMod_ = k_;
    cutMod_ = pitchMod_ = 0.0f;
    gainL_ = gainR_ = 1.0f;
    sendAmt_ = v[acid::DelaySend];
    const int dl = std::clamp(static_cast<int>(std::lround(v[acid::DelayLeft])), 0, kNumDelayTimes - 1);
    const int dr = std::clamp(static_cast<int>(std::lround(v[acid::DelayRight])), 0, kNumDelayTimes - 1);
    delay_.set(kDelayBeats[dl], kDelayBeats[dr], bpm, v[acid::DelayFeedback], v[acid::DelayHighPass], v[acid::DelayLowPass]);
}

void Acid::noteOn(int pitch, float velocity, bool accent, bool slide, int gateSamples, double late)
{
    legato_ = slidePending_ && gate_ > 0 && amp_.isActive();
    pitchTarget_ = pitch;
    velocity_ = clampv(velocity, 0.0f, 1.0f);
    accent_ = accent;
    accentGainTarget_ = accent ? 1.0f + accentAmt_ : 1.0f;
    fDecayNote_ = accent ? fDecayAccent_ : fDecay_;
    gate_ = std::max(1, gateSamples);
    slidePending_ = slide;
    const double f0 = midiToHz(pitch);
    noteRand_ = modRng_.bipolar();
    if (!legato_) mod_.noteOn(static_cast<int64_t>(modPos_), beat_);   // a slide is one note to the modulation, as to the envelopes
    // Release floor: half a period (as the bass), and never under 8 ms.
    amp_.setTimes(0.0004f, ampDecay_, 0.0f, static_cast<float>(std::max(0.008, 0.5 / f0)));
    if (!legato_) {
        pitchNow_ = pitch;
        hz_ = static_cast<float>(f0);
        // The envelopes step once per oversampled sample, so `late` counts in those steps too.
        fenv_ = static_cast<float>(std::pow(static_cast<double>(fDecayNote_), static_cast<double>(os_) * late));
        sq_ = static_cast<float>(std::pow(static_cast<double>(sqDecay_), static_cast<double>(os_) * late));
        if (accent) pulse_ = 1.0f;
        amp_.noteOn();
        amp_.advanceAttack(late);
    } else if (accent) {
        pulse_ = 1.0f;
    }
}

void Acid::process(float* L, float* R, int total)
{
    const double sr2 = osRate_;
    const float nyq = static_cast<float>(std::min(18000.0, nyqFactor_ * sr2));
    const float sweepDepth = kSweepOctaves * accentAmt_ * resonance_;
    const float accentOct = accent_ ? 1.0f + accentAmt_ : 1.0f;
    const float velGain = 0.7f + 0.3f * velocity_;
    const size_t mask = comb_.size() - 1;
    int done = 0;
    while (done < total) {
        const int n = std::min(total - done, static_cast<int>(mono_.size()));
        for (int i = 0; i < n; ++i) {
            // The modulation, every 16 samples on the absolute count (26.09.2026).
            if (modOn_ && modPos_ % 16 == 0) {
                float ext[kModSources] = {};
                ext[static_cast<int>(ModSource::FilterEnv)] = fenv_;
                ext[static_cast<int>(ModSource::Velocity)] = velocity_;
                ext[static_cast<int>(ModSource::Key)] = std::clamp(static_cast<float>(pitchNow_ - 57.0) / 24.0f, -1.0f, 1.0f);
                ext[static_cast<int>(ModSource::Random)] = noteRand_;
                mod_.evaluate(static_cast<int64_t>(modPos_), beat_, ext, modSum_);
                cutMod_ = mod_.offset(modSum_, ModDest::Cutoff);
                pitchMod_ = mod_.offset(modSum_, ModDest::Pitch);
                kMod_ = mod_.targets(ModDest::Resonance)
                    ? kResonanceMax * std::clamp(resonance_ + modSum_[static_cast<int>(ModDest::Resonance)], 0.0f, 1.0f) : k_;
                const float gain = mod_.targets(ModDest::Level) ? std::clamp(1.0f + modSum_[static_cast<int>(ModDest::Level)], 0.0f, 2.0f) : 1.0f;
                if (mod_.targets(ModDest::Pan)) {
                    const double theta = (std::clamp(static_cast<double>(modSum_[static_cast<int>(ModDest::Pan)]), -1.0, 1.0) + 1.0) * kPiD / 4.0;
                    gainL_ = gain * static_cast<float>(std::cos(theta) * std::sqrt(2.0));
                    gainR_ = gain * static_cast<float>(std::sin(theta) * std::sqrt(2.0));
                } else {
                    gainL_ = gainR_ = gain;
                }
            }
            if (std::fabs(pitchTarget_ - pitchNow_) > 1.0e-6) {
                pitchNow_ += (pitchTarget_ - pitchNow_) * glide_;
                hz_ = static_cast<float>(midiToHz(pitchNow_));
            }
            osc_.set(pitchMod_ != 0.0f ? hz_ * std::pow(2.0f, pitchMod_ / 12.0f) : hz_, sr2, wave_, 0.5f);
            accentGain_ += (accentGainTarget_ - accentGain_) * accentSmooth_;
            // 2x: two ladder steps, decimated back. 1x (Quality::Quest): one step, no decimator.
            float o[2] = { 0.0f, 0.0f };
            for (int j = 0; j < os_; ++j) {
                float oct = envOct_ * fenv_ * accentOct + keyTrack_ * static_cast<float>(pitchNow_ - 57.0) / 12.0f + sweepDepth * sweep_ + cutMod_;
                if (squelch_) oct += sqOct_ * sq_;
                const float fc = clampv(cutoff_ * std::pow(2.0f, oct), 20.0f, nyq);
                const float g = 1.41421356f * static_cast<float>(std::tan(kPiD * fc / sr2));
                fenv_ *= fDecayNote_;
                sq_ *= sqDecay_;
                pulse_ *= pulseDecay_;
                sweep_ += (pulse_ - sweep_) * sweepCharge_;
                o[j] = ladder_.tick(osc_.next(), g, kMod_, kLadderComp);
            }
            float y = os_ == 2 ? down_.process(o[0], o[1]) : o[0];
            if (squelch_) {
                // Feedback comb tuned to the period, read with a third-order Lagrange interpolator
                // (Acid.h, combTaps()). Its four taps sit at -1 .. +2 around the integer part, so the
                // delay is clamped to at least 3 samples: the tap at +2 must still lie behind the
                // write position, otherwise it would read what the previous lap of the buffer left.
                const double d = std::clamp(sr_ / static_cast<double>(hz_), 3.0, static_cast<double>(mask - 3));
                const double pos = static_cast<double>(combPos_) - d;
                const double fl = std::floor(pos);
                const long long i0 = static_cast<long long>(fl);
                float wm1, w0, w1, w2;
                combTaps(static_cast<float>(pos - fl), wm1, w0, w1, w2);
                const float delayed = wm1 * comb_[static_cast<size_t>(i0 - 1) & mask]
                                    + w0 * comb_[static_cast<size_t>(i0) & mask]
                                    + w1 * comb_[static_cast<size_t>(i0 + 1) & mask]
                                    + w2 * comb_[static_cast<size_t>(i0 + 2) & mask];
                const float yc = y + combFb_ * delayed;
                comb_[combPos_] = yc;
                combPos_ = (combPos_ + 1) & mask;
                y += combMix_ * ((1.0f - combFb_) * yc - y);
            }
            if (gate_ > 0 && --gate_ == 0) { amp_.noteOff(); mod_.noteOff(); }
            const float a = amp_.process();
            mod_.tick();
            ++modPos_;
            beat_ += beatsPerSample_;
            float v = shaper_(y * a * accentGain_ * velGain * driveIn_) * driveOut_;
            float lp, bp, hp;
            lc1_.tick(v, lp, bp, hp);
            lc2_.tick(hp, lp, bp, v);
            // Dispersion after the low cut: an all-pass changes no band's power, so the order cannot
            // matter for the depth rule, but running it on the cut signal keeps the chain from
            // spending sections on a band that is already gone.
            if (disperse_.stages > 0) v = dispState_.tick(v, disperse_.c, disperse_.d, disperse_.stages);
            v *= level_;
            L[done + i] = v * gainL_;
            R[done + i] = v * gainR_;
            send_[static_cast<size_t>(i)] = v * sendAmt_;
        }
        delay_.process(send_.data(), L + done, R + done, n);
        done += n;
    }
}

} // namespace phos
