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
constexpr double PiD = 3.141592653589793;   ///< pi
constexpr float ResonanceMax = 16.0f;       ///< feedback at Resonance 1: below the self-oscillation at 17, as on the TB-303
constexpr float LadderComp = 0.3f;          ///< input gain (1 + 0.3 k) against the passband loss 1/(1 + k)
constexpr double AccentDecayMax = 0.2;      ///< an accented note's filter envelope lasts at most 200 ms
constexpr double SweepPulseTau = 0.06;      ///< accent pulse
constexpr double SweepTau = 0.15;           ///< accent sweep capacitor
constexpr float SweepOctaves = 2.0f;        ///< sweep depth at full accent, resonance and charge
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
constexpr float DriveMaxDb = 30.0f;

/** @brief The half-band decimator of the acid's 2x oversampling (96 dB stop band, 0.1 transition), designed once. */
const HalfbandDesign& acidHalfband()
{
    static const HalfbandDesign d = designHalfband(96.0, 0.1);
    return d;
}
} // namespace

void Acid::setOversampling(int factor)
{
    m_os = factor <= 1 ? 1 : 2;
    m_osRate = m_sr * m_os;
    // At 2x the cutoff must stay inside the decimator's passband (0.2 of the high rate); at 1x there
    // is no decimator and the only limit is the ladder's stability at 0.45 fs. At 48 kHz both land
    // above the 18 kHz cap below, so the two levels reach the same highest cutoff.
    m_nyqFactor = m_os == 2 ? 0.2 : 0.45;
    m_down.reset();
}

void Acid::prepare(double sampleRate)
{
    m_sr = sampleRate;
    m_osRate = m_sr * m_os;
    m_amp.setSampleRate(m_sr);
    m_mod.prepare(m_sr, 0x41434944'4D4F4430ull);   // "ACIDMOD0": one voice, one fixed stream (26.09.2026)
    m_modRng.seed(0x41434944'524E4430ull);         // "ACIDRND0"
    m_down.setup(acidHalfband());
    m_delay.prepare(m_sr);
    size_t n = 1;
    while (static_cast<double>(n) < m_sr / 40.0 + 4.0) n <<= 1;
    m_comb.assign(n, 0.0f);
    m_mono.assign(256, 0.0f);
    m_send.assign(256, 0.0f);
    reset();
}

void Acid::reset()
{
    m_mod.kill();
    for (float& m : m_modSum) m = 0.0f;
    m_osc = VaOscillator{};
    m_ladder.reset();
    m_down.reset();
    m_amp.kill();
    m_shaper.reset();
    m_lc1.reset();
    m_lc2.reset();
    m_dispState.reset();
    m_delay.reset();
    std::fill(m_comb.begin(), m_comb.end(), 0.0f);
    m_combPos = 0;
    m_pitchNow = m_pitchTarget = 57.0;
    m_hz = static_cast<float>(midiToHz(57.0));
    m_gate = 0;
    m_fenv = m_sq = m_pulse = m_sweep = 0.0f;
    m_accentGain = m_accentGainTarget = 1.0f;
    m_slidePending = m_legato = m_accent = false;
}

void Acid::update(const float* v, double bpm)
{
    const double sr2 = m_osRate;
    m_wave = v[acid::WAVE];
    m_cutoff = v[acid::CUTOFF];
    m_resonance = v[acid::RESONANCE];
    m_k = ResonanceMax * m_resonance;
    m_envOct = v[acid::ENV_AMOUNT];
    m_fDecay = static_cast<float>(std::exp(std::log(1.0e-3) / (v[acid::DECAY] * 0.001 * sr2)));
    m_fDecayAccent = static_cast<float>(std::exp(std::log(1.0e-3) / (std::min(static_cast<double>(v[acid::DECAY]) * 0.001, AccentDecayMax) * sr2)));
    m_accentAmt = v[acid::ACCENT];
    m_glide = static_cast<float>(1.0 - std::exp(-1.0 / (v[acid::SLIDE_TIME] * 0.001 * m_sr)));
    m_ampDecay = v[acid::AMP_DECAY] * 0.001f;
    m_keyTrack = v[acid::KEY_TRACK];
    // Drive: up to DriveMaxDb into the saturator, and back out by half of that in dB, so a quiet
    // body gains half the drive and a peak that saturates loses the other half (see DriveMaxDb).
    const float drive = v[acid::DRIVE];
    m_driveIn = dbToGain(DriveMaxDb * drive);
    m_driveOut = 1.0f / std::sqrt(m_driveIn);
    const bool sq = v[acid::SQUELCH] >= 0.5f;
    if (sq && !m_squelch) std::fill(m_comb.begin(), m_comb.end(), 0.0f);   // no stale ring from long ago
    m_squelch = sq;
    m_sqOct = static_cast<float>(std::log2(v[acid::SQUELCH_START]));
    m_sqDecay = static_cast<float>(std::exp(-1.0 / (v[acid::SQUELCH_TIME] * 0.001 * sr2)));
    m_combMix = v[acid::COMB_MIX];
    m_combFb = v[acid::COMB_FEEDBACK];
    m_lc1.setQ(std::max(150.0f, v[acid::LOW_CUT]), 0.70710678f, static_cast<float>(m_sr));
    m_lc2.copyCoefficients(m_lc1);
    m_disperse.set(static_cast<int>(std::lround(v[acid::DISPERSE])), v[acid::DISPERSE_FREQ], m_sr);
    m_pulseDecay = static_cast<float>(std::exp(-1.0 / (SweepPulseTau * sr2)));
    m_sweepCharge = static_cast<float>(1.0 - std::exp(-1.0 / (SweepTau * sr2)));
    m_accentSmooth = static_cast<float>(1.0 - std::exp(-1.0 / (0.004 * m_sr)));
    m_level = dbToGain(v[acid::LEVEL]);
    // The modulation (26.09.2026): the block from its envelope on (the acid has no filter envelope of the block's).
    static_assert(acid::MX8_AMOUNT - acid::MENV_ATTACK + 1 == ModBlockSize - ModBlockMenv, "the block's layout (Modulation.h)");
    m_mod.set(readModBlock(v + acid::MENV_ATTACK - ModBlockMenv, AcidModDestMap, AcidModDests));
    m_modOn = m_mod.active();
    m_kMod = m_k;
    m_cutMod = m_pitchMod = 0.0f;
    m_gainL = m_gainR = 1.0f;
    m_sendAmt = v[acid::DELAY_SEND];
    const int dl = std::clamp(static_cast<int>(std::lround(v[acid::DELAY_LEFT])), 0, NumDelayTimes - 1);
    const int dr = std::clamp(static_cast<int>(std::lround(v[acid::DELAY_RIGHT])), 0, NumDelayTimes - 1);
    m_delay.set(DelayBeats[dl], DelayBeats[dr], bpm, v[acid::DELAY_FEEDBACK], v[acid::DELAY_HIGH_PASS], v[acid::DELAY_LOW_PASS]);
}

void Acid::noteOn(int pitch, float velocity, bool accent, bool slide, int gateSamples, double late)
{
    m_legato = m_slidePending && m_gate > 0 && m_amp.isActive();
    m_pitchTarget = pitch;
    m_velocity = clampv(velocity, 0.0f, 1.0f);
    m_accent = accent;
    m_accentGainTarget = accent ? 1.0f + m_accentAmt : 1.0f;
    m_fDecayNote = accent ? m_fDecayAccent : m_fDecay;
    m_gate = std::max(1, gateSamples);
    m_slidePending = slide;
    const double f0 = midiToHz(pitch);
    m_noteRand = m_modRng.bipolar();
    if (!m_legato) m_mod.noteOn(static_cast<int64_t>(m_modPos), m_beat);   // a slide is one note to the modulation, as to the envelopes
    // Release floor: half a period (as the bass), and never under 8 ms.
    m_amp.setTimes(0.0004f, m_ampDecay, 0.0f, static_cast<float>(std::max(0.008, 0.5 / f0)));
    if (!m_legato) {
        m_pitchNow = pitch;
        m_hz = static_cast<float>(f0);
        // The envelopes step once per oversampled sample, so `late` counts in those steps too.
        m_fenv = static_cast<float>(std::pow(static_cast<double>(m_fDecayNote), static_cast<double>(m_os) * late));
        m_sq = static_cast<float>(std::pow(static_cast<double>(m_sqDecay), static_cast<double>(m_os) * late));
        if (accent) m_pulse = 1.0f;
        m_amp.noteOn();
        m_amp.advanceAttack(late);
    } else if (accent) {
        m_pulse = 1.0f;
    }
}

void Acid::process(float* L, float* R, int total)
{
    const double sr2 = m_osRate;
    const float nyq = static_cast<float>(std::min(18000.0, m_nyqFactor * sr2));
    const float sweepDepth = SweepOctaves * m_accentAmt * m_resonance;
    const float accentOct = m_accent ? 1.0f + m_accentAmt : 1.0f;
    const float velGain = 0.7f + 0.3f * m_velocity;
    const size_t mask = m_comb.size() - 1;
    int done = 0;
    while (done < total) {
        const int n = std::min(total - done, static_cast<int>(m_mono.size()));
        for (int i = 0; i < n; ++i) {
            // The modulation, every 16 samples on the absolute count (26.09.2026).
            if (m_modOn && m_modPos % 16 == 0) {
                float ext[ModSources] = {};
                ext[static_cast<int>(ModSourceE::FILTER_ENV)] = m_fenv;
                ext[static_cast<int>(ModSourceE::VELOCITY)] = m_velocity;
                ext[static_cast<int>(ModSourceE::KEY)] = std::clamp(static_cast<float>(m_pitchNow - 57.0) / 24.0f, -1.0f, 1.0f);
                ext[static_cast<int>(ModSourceE::RANDOM)] = m_noteRand;
                m_mod.evaluate(static_cast<int64_t>(m_modPos), m_beat, ext, m_modSum);
                m_cutMod = m_mod.offset(m_modSum, ModDestE::CUTOFF);
                m_pitchMod = m_mod.offset(m_modSum, ModDestE::PITCH);
                m_kMod = m_mod.targets(ModDestE::RESONANCE)
                    ? ResonanceMax * std::clamp(m_resonance + m_modSum[static_cast<int>(ModDestE::RESONANCE)], 0.0f, 1.0f) : m_k;
                const float gain = m_mod.targets(ModDestE::LEVEL) ? std::clamp(1.0f + m_modSum[static_cast<int>(ModDestE::LEVEL)], 0.0f, 2.0f) : 1.0f;
                if (m_mod.targets(ModDestE::PAN)) {
                    const double theta = (std::clamp(static_cast<double>(m_modSum[static_cast<int>(ModDestE::PAN)]), -1.0, 1.0) + 1.0) * PiD / 4.0;
                    m_gainL = gain * static_cast<float>(std::cos(theta) * std::sqrt(2.0));
                    m_gainR = gain * static_cast<float>(std::sin(theta) * std::sqrt(2.0));
                } else {
                    m_gainL = m_gainR = gain;
                }
            }
            if (std::fabs(m_pitchTarget - m_pitchNow) > 1.0e-6) {
                m_pitchNow += (m_pitchTarget - m_pitchNow) * m_glide;
                m_hz = static_cast<float>(midiToHz(m_pitchNow));
            }
            m_osc.set(m_pitchMod != 0.0f ? m_hz * std::pow(2.0f, m_pitchMod / 12.0f) : m_hz, sr2, m_wave, 0.5f);
            m_accentGain += (m_accentGainTarget - m_accentGain) * m_accentSmooth;
            // 2x: two ladder steps, decimated back. 1x (Quality::Quest): one step, no decimator.
            float o[2] = { 0.0f, 0.0f };
            for (int j = 0; j < m_os; ++j) {
                float oct = m_envOct * m_fenv * accentOct + m_keyTrack * static_cast<float>(m_pitchNow - 57.0) / 12.0f + sweepDepth * m_sweep + m_cutMod;
                if (m_squelch) oct += m_sqOct * m_sq;
                const float fc = clampv(m_cutoff * std::pow(2.0f, oct), 20.0f, nyq);
                const float g = 1.41421356f * static_cast<float>(std::tan(PiD * fc / sr2));
                m_fenv *= m_fDecayNote;
                m_sq *= m_sqDecay;
                m_pulse *= m_pulseDecay;
                m_sweep += (m_pulse - m_sweep) * m_sweepCharge;
                o[j] = m_ladder.tick(m_osc.next(), g, m_kMod, LadderComp);
            }
            float y = m_os == 2 ? m_down.process(o[0], o[1]) : o[0];
            if (m_squelch) {
                // Feedback comb tuned to the period, read with a third-order Lagrange interpolator
                // (Acid.h, combTaps()). Its four taps sit at -1 .. +2 around the integer part, so the
                // delay is clamped to at least 3 samples: the tap at +2 must still lie behind the
                // write position, otherwise it would read what the previous lap of the buffer left.
                const double d = std::clamp(m_sr / static_cast<double>(m_hz), 3.0, static_cast<double>(mask - 3));
                const double pos = static_cast<double>(m_combPos) - d;
                const double fl = std::floor(pos);
                const long long i0 = static_cast<long long>(fl);
                float wm1, w0, w1, w2;
                combTaps(static_cast<float>(pos - fl), wm1, w0, w1, w2);
                const float delayed = wm1 * m_comb[static_cast<size_t>(i0 - 1) & mask]
                                    + w0 * m_comb[static_cast<size_t>(i0) & mask]
                                    + w1 * m_comb[static_cast<size_t>(i0 + 1) & mask]
                                    + w2 * m_comb[static_cast<size_t>(i0 + 2) & mask];
                const float yc = y + m_combFb * delayed;
                m_comb[m_combPos] = yc;
                m_combPos = (m_combPos + 1) & mask;
                y += m_combMix * ((1.0f - m_combFb) * yc - y);
            }
            if (m_gate > 0 && --m_gate == 0) { m_amp.noteOff(); m_mod.noteOff(); }
            const float a = m_amp.process();
            m_mod.tick();
            ++m_modPos;
            m_beat += m_beatsPerSample;
            float v = m_shaper(y * a * m_accentGain * velGain * m_driveIn) * m_driveOut;
            float lp, bp, hp;
            m_lc1.tick(v, lp, bp, hp);
            m_lc2.tick(hp, lp, bp, v);
            // Dispersion after the low cut: an all-pass changes no band's power, so the order cannot
            // matter for the depth rule, but running it on the cut signal keeps the chain from
            // spending sections on a band that is already gone.
            if (m_disperse.m_stages > 0) v = m_dispState.tick(v, m_disperse.m_c, m_disperse.m_d, m_disperse.m_stages);
            v *= m_level;
            L[done + i] = v * m_gainL;
            R[done + i] = v * m_gainR;
            m_send[static_cast<size_t>(i)] = v * m_sendAmt;
        }
        m_delay.process(m_send.data(), L + done, R + done, n);
        done += n;
    }
}

} // namespace phos
