/**
 * @file Bass.cpp
 * @brief Bass implementation.
 * @author Rene Weller
 */
#include "phos/Bass.h"
#include "phos/Params.h"
#include <cmath>

namespace phos {

namespace {
/**
 * @brief The decimator for every 2x stage in the core: 96 dB stopband, passband to 0.2 of the high rate.
 * @return the decimator's design
 */
const HalfbandDesign& bassHalfband()
{
    static const HalfbandDesign d = designHalfband(96.0, 0.1);
    return d;
}
constexpr float ResonanceMax = 3.9f;   ///< feedback at Resonance 1: just short of self-oscillation
constexpr float SubScale = 0.7f;       ///< sub (and sub octave) knob to amplitude: at 0.6 the sine matches the filtered voice's old fundamental
/**
 * @brief Bite knob to amplitude.
 *
 * The layer is a band of a full-scale saw, so its raw level is already comparable to the saw path's;
 * the scale only sets where the knob's middle lands. Calibrated on 19.09.2026 so that the default
 * sound meets the references' bite band: 300 Hz .. 2 kHz at -9.8 dB against 20 .. 120 Hz between the
 * kicks, the median of Tools/ref_bass.py over 24 recordings (docs/rounds/2026-09.md, 19.09.2026).
 */
constexpr float BiteScale = 1.344f;
/** @brief Butterworth dampings of the two sections: 2 cos(pi/8) and 2 cos(3 pi/8). */
constexpr float BiteK1 = 1.8477590f, BiteK2 = 0.7653669f;   ///< 2 cos(3 pi/8)
/** @brief Corner of the bite's high pass as a fraction of Bite Cutoff (see update()). */
constexpr float BiteHpRatio = 0.4f;
} // namespace

void Bass::prepare(double sampleRate)
{
    m_sr = sampleRate;
    m_osRate = m_sr * m_os;
    m_amp.setSampleRate(m_sr);
    m_fAdsr.setSampleRate(m_sr);
    m_mod.prepare(m_sr, 0x42415353'4D4F4430ull);   // "BASSMOD0": one voice, one fixed stream (26.09.2026)
    m_modRng.seed(0x42415353'524E4430ull);         // "BASSRND0"
    m_ducker.prepare(m_sr);
    m_down.setup(bassHalfband());
    reset();
}

void Bass::setOversampling(int factor)
{
    m_os = factor <= 1 ? 1 : 2;
    m_osRate = m_sr * m_os;
    // The decimator's state belongs to the 2x path; at 1x it is not used, but a later switch back
    // must not start from a ringing it never heard.
    m_down.reset();
}

void Bass::reset()
{
    m_mod.kill();
    m_fAdsr.kill();
    for (float& m : m_modSum) m = 0.0f;
    m_ladder.reset();
    m_model.clear();
    m_down.reset();
    m_hp1.reset();
    m_hp2.reset();
    m_hp3.reset();
    m_hp4.reset();
    m_bite1.reset();
    m_bite2.reset();
    m_biteHp.reset();
    m_amp.kill();
    m_ducker.reset();
    m_osc = VaOscillator{};
    m_gate = 0;
    m_fenv = 0.0f;
    m_biteEnv = 0.0f;
    m_subPhase = 0.0;
}

void Bass::update(const float* v)
{
    m_wave = v[bass::WAVE];
    m_pw = v[bass::PULSE_WIDTH];
    m_subLevel = v[bass::SUB] * SubScale;
    m_octLevel = v[bass::SUB_OCTAVE] * SubScale;
    m_subMode = static_cast<int>(std::lround(v[bass::SUB_MODE]));
    m_splitRatio = v[bass::SPLIT_RATIO];
    m_retrigger = v[bass::RETRIGGER] >= 0.5f;
    m_startPhase = v[bass::START_PHASE];
    m_cutoff = v[bass::CUTOFF];
    m_k = ResonanceMax * v[bass::RESONANCE];
    m_modelIndex = std::clamp(static_cast<int>(std::lround(v[bass::FILTER_MODEL])), 0, VoiceFilterModels - 1);
    m_modelMode = std::clamp(v[bass::FILTER_MODE], 0.0f, 1.0f);
    if (m_modelIndex != 0)
        m_modelK = FilterVoicing::feedback(static_cast<FilterModelE>(m_modelIndex - 1), std::clamp(v[bass::RESONANCE], 0.0f, 1.0f));
    // The level against the bass ladder, made good (26.09.2026, testFilterModels.levels at resonance 0.45): the drive
    // stage evens out most of it, but the SEM, the Korg35, the Polivoks and the Wasp stood 1.6 .. 3.6 dB over it.
    // The trim reaches only the filtered voice, not the sub under it, so it moves the bass's level by about half its
    // value (measured: -2.94 dB on the SEM took it from +2.94 to +1.46): the table is twice the differences.
    static const float ModelTrimDb[VoiceFilterModels] = { 0.0f, -0.5f, -2.9f, -1.2f, -5.9f, -2.9f, 1.0f, -3.3f, -4.7f, -7.1f };
    m_modelTrim = dbToGain(ModelTrimDb[m_modelIndex]);
    // The modulation (26.09.2026): the block with the bass's own targets, and the filter envelope as an ADSR once it
    // asks for more than a decay.
    static_assert(bass::MX8_AMOUNT - bass::FILT_ATTACK + 1 == ModBlockSize, "the block's layout (Modulation.h)");
    m_mod.set(readModBlock(v + bass::FILT_ATTACK, BassModDestMap, BassModDests));
    m_modOn = m_mod.active();
    m_fAdsrOn = v[bass::FILT_ATTACK] > 0.1001f || v[bass::FILT_SUSTAIN] > 0.0f;
    m_fAdsr.setTimes(v[bass::FILT_ATTACK] * 0.001f, v[bass::FILTER_DECAY] * 0.001f, v[bass::FILT_SUSTAIN], std::max(0.005f, v[bass::FILT_RELEASE] * 0.001f));
    m_resBase = std::clamp(v[bass::RESONANCE], 0.0f, 1.0f);
    m_kMod = m_k;
    m_modelKMod = m_modelK;
    m_modeMod = m_modelMode;
    m_cutMod = 0.0f;
    m_levelMod = 1.0f;
    m_envOct = v[bass::ENV_AMOUNT];
    m_keyTrack = v[bass::KEY_TRACK];
    m_velCut = v[bass::VEL_TO_CUTOFF];
    m_fDecay = static_cast<float>(std::exp(std::log(1.0e-3) / (v[bass::FILTER_DECAY] * 0.001 * m_sr)));
    const float drive = v[bass::DRIVE];
    m_driveIn = 0.5f + 3.5f * drive;
    m_driveOut = 1.0f / (0.5f + 1.5f * drive);
    m_attack = v[bass::AMP_ATTACK] * 0.001f;
    m_decay = v[bass::AMP_DECAY] * 0.001f;
    m_sustain = v[bass::AMP_SUSTAIN];
    m_release = v[bass::AMP_RELEASE] * 0.001f;
    m_ducker.set(v[bass::DUCK_DEPTH], 1.0f, v[bass::DUCK_HOLD], v[bass::DUCK_RELEASE]);
    m_level = dbToGain(v[bass::LEVEL]);
    m_biteLevel = v[bass::BITE] * BiteScale;
    m_biteCut = v[bass::BITE_CUTOFF];
    m_biteEnvOct = v[bass::BITE_ENV];
    // Per sample at the rate the voice runs at: the envelope is advanced once per oscillator step.
    m_biteDecay = static_cast<float>(std::exp(std::log(1.0e-3) / (v[bass::BITE_DECAY] * 0.001 * m_osRate)));
    m_biteGain = 1.0f + 7.0f * v[bass::BITE_DRIVE];
    m_biteNorm = 1.0f / std::tanh(m_biteGain);
    // Resonance 0..1 narrows the second section from Butterworth towards a Q of about 9.
    m_biteK2 = BiteK2 * (1.0f - 0.85f * clampv(v[bass::BITE_RESONANCE], 0.0f, 1.0f));
    // The bite's floor: a Butterworth high pass at BiteHpRatio of the resting cutoff. The bite is the
    // band above the low mids; without the floor its 3rd to 6th harmonics (140 .. 280 Hz on F#1) came
    // out as strong as its 300 Hz .. 2 kHz band and thickened the low mids, which the mix already
    // carries 4 dB over the references (docs/rounds/2026-09.md, 18.09.2026).
    m_biteHp.setQ(std::max(40.0f, BiteHpRatio * m_biteCut), 0.70710678f, static_cast<float>(m_osRate));
    applyNoteSettings();
}

void Bass::applyNoteSettings()
{
    const double f0 = midiToHz(m_pitch);
    // Release floor: half a period of the fundamental.
    m_releaseUsed = std::max(m_release, static_cast<float>(0.5 / f0));
    m_amp.setTimes(m_attack, m_decay, m_sustain, m_releaseUsed);
    // The Split high pass: a fourth-order Butterworth squared (Linkwitz-Riley, 8th order, 48 dB per
    // octave), the same two Butterworth dampings as the bite's low pass, each section twice.
    const float fc = static_cast<float>(std::min(f0 * m_splitRatio, 0.45 * m_sr));
    m_hp1.setK(fc, BiteK1, static_cast<float>(m_sr));
    m_hp2.setK(fc, BiteK2, static_cast<float>(m_sr));
    m_hp3.copyCoefficients(m_hp1);
    m_hp4.copyCoefficients(m_hp2);
}

void Bass::noteOn(int pitch, float velocity, int gateSamples, double late, double fundamentalPhase)
{
    m_pitch = pitch;
    m_velocity = clampv(velocity, 0.0f, 1.0f);
    m_gate = std::max(1, gateSamples);
    const double f0 = midiToHz(m_pitch);
    m_osc.set(f0, m_osRate, m_wave, m_pw);
    if (m_retrigger) {
        // When the previous note has died away (-60 dB), the filters start from rest as well: the
        // oscillator keeps running between notes, and its ringing in the ladder, the decimator and
        // the 48 dB high pass would otherwise make every note begin a little differently. Under
        // -60 dB the reset cannot be heard; a note that is still sounding keeps its filter states.
        if (m_amp.level() < 1.0e-3f) {
            m_ladder.reset();
            m_model.clear();
            m_down.reset();
            m_hp1.reset();
            m_hp2.reset();
            m_hp3.reset();
            m_hp4.reset();
            m_bite1.reset();
            m_bite2.reset();
            m_biteHp.reset();
        }
        m_osc.restart(fundamentalPhase, static_cast<double>(m_os) * late);
        const double p = fundamentalPhase + f0 * late / m_sr;
        m_subPhase = p - std::floor(p);
    }
    applyNoteSettings();
    // Sub-sample onset for the envelopes too: the filter envelope and the attack start `late`
    // samples into their curves, like the oscillators.
    m_fenv = static_cast<float>(std::pow(static_cast<double>(m_fDecay), late));
    // The modulation (26.09.2026): the note's random value, the envelope and the retriggered LFOs, the ADSR.
    m_noteRand = m_modRng.bipolar();
    m_mod.noteOn(static_cast<int64_t>(m_modPos), m_beat);
    if (m_fAdsrOn) {
        m_fAdsr.noteOn();
        m_fAdsr.advanceAttack(late);
        m_fenv = m_fAdsr.level();
    }
    // The bite envelope steps once per oscillator sample, so `late` counts at the voice's rate.
    m_biteEnv = static_cast<float>(std::pow(static_cast<double>(m_biteDecay), static_cast<double>(m_os) * late));
    m_amp.noteOn();
    m_amp.advanceAttack(late);
}

void Bass::process(float* out, int n)
{
    const float nyq = static_cast<float>(0.45 * m_osRate);
    const double f0 = midiToHz(m_pitch);
    m_osc.set(f0, m_osRate, m_wave, m_pw);
    const double subInc = f0 / m_sr;
    // Key tracking relative to E1 (MIDI 28), the bottom of the usual psytrance bass register.
    const float track = m_keyTrack * static_cast<float>(m_pitch - 28) / 12.0f;
    const float velScale = 1.0f - m_velCut + m_velCut * m_velocity;
    for (int i = 0; i < n; ++i) {
        // The modulation, every 16 samples on the absolute count (26.09.2026): where the host cuts its blocks must
        // not decide when a destination moves.
        if (m_modOn && m_modPos % 16 == 0) {
            float ext[ModSources] = {};
            ext[static_cast<int>(ModSourceE::FILTER_ENV)] = m_fenv;
            ext[static_cast<int>(ModSourceE::VELOCITY)] = m_velocity;
            ext[static_cast<int>(ModSourceE::KEY)] = std::clamp(static_cast<float>(m_pitch - 40) / 24.0f, -1.0f, 1.0f);
            ext[static_cast<int>(ModSourceE::RANDOM)] = m_noteRand;
            m_mod.evaluate(static_cast<int64_t>(m_modPos), m_beat, ext, m_modSum);
            m_cutMod = m_mod.offset(m_modSum, ModDestE::CUTOFF);
            m_levelMod = m_mod.targets(ModDestE::LEVEL) ? std::clamp(1.0f + m_modSum[static_cast<int>(ModDestE::LEVEL)], 0.0f, 2.0f) : 1.0f;
            if (m_mod.targets(ModDestE::RESONANCE)) {
                const float res = std::clamp(m_resBase + m_modSum[static_cast<int>(ModDestE::RESONANCE)], 0.0f, 1.0f);
                m_kMod = ResonanceMax * res;
                if (m_modelIndex != 0) m_modelKMod = FilterVoicing::feedback(static_cast<FilterModelE>(m_modelIndex - 1), res);
            }
            m_modeMod = std::clamp(m_modelMode + m_mod.offset(m_modSum, ModDestE::FILTER_MODE), 0.0f, 1.0f);
            if (m_mod.targets(ModDestE::PULSE_WIDTH))
                m_osc.set(f0, m_osRate, m_wave, std::clamp(m_pw + m_modSum[static_cast<int>(ModDestE::PULSE_WIDTH)], 0.05f, 0.95f));
        }
        const float oct = m_envOct * m_fenv * velScale + track + m_cutMod;
        const float fc = clampv(m_cutoff * std::pow(2.0f, oct), 20.0f, nyq);
        const float g = std::tan(Pi * fc / static_cast<float>(m_osRate));
        if (m_fAdsrOn) m_fenv = m_fAdsr.process();
        else m_fenv *= m_fDecay;
        m_mod.tick();

        // 2x: two ladder steps, decimated back. 1x (Quality::Quest, not used for the bass today):
        // one step, no decimator -- the PolyBLEP oscillator still band-limits its own discontinuity,
        // only the ladder's distortion products are no longer kept out of the audible band.
        // The bite's cutoff, once per output sample and shared by both oversampled steps; its
        // envelope follows the same velocity scaling and key tracking as the saw path's.
        const bool bite = m_biteLevel > 0.0f;
        if (bite) {
            const float bfc = clampv(m_biteCut * std::pow(2.0f, m_biteEnvOct * m_biteEnv * velScale + track), 20.0f, nyq);
            const float bg = std::tan(Pi * bfc / static_cast<float>(m_osRate));
            m_bite1.setG(bg, BiteK1);
            m_bite2.setG(bg, m_biteK2);
        }
        auto biteStep = [&](float s) {
            m_biteEnv *= m_biteDecay;
            float lp, bp, hp;
            m_biteHp.tick(std::tanh(s * m_biteGain) * m_biteNorm, lp, bp, hp);
            return m_bite2.lp(m_bite1.lp(hp)) * m_biteLevel;
        };
        // The filter: the bass's own ladder, or since 26.09.2026 one of the models (Filters.h) at the same rate.
        auto filt = [&](float x) {
            return m_modelIndex == 0 ? m_ladder.tick(x, g, m_kMod, 0.5f)
                                    : m_model.tick(static_cast<FilterModelE>(m_modelIndex - 1), x, g, m_modelKMod, m_modeMod) * m_modelTrim;
        };
        const float s0 = m_osc.next();
        const float o0 = filt(s0 * m_driveIn) * m_driveOut + (bite ? biteStep(s0) : 0.0f);
        float y;
        if (m_os == 2) {
            const float s1 = m_osc.next();
            const float o1 = filt(s1 * m_driveIn) * m_driveOut + (bite ? biteStep(s1) : 0.0f);
            y = m_down.process(o0, o1);
        } else {
            y = o0;
        }
        if (m_subMode == static_cast<int>(SubModeE::SPLIT)) {
            float lp, bp, hp;
            m_hp1.tick(y, lp, bp, hp);
            m_hp2.tick(hp, lp, bp, y);
            m_hp3.tick(y, lp, bp, hp);
            m_hp4.tick(hp, lp, bp, y);
        }
        float sub = m_subLevel > 0.0f ? m_subLevel * static_cast<float>(std::sin(2.0 * 3.141592653589793 * m_subPhase)) : 0.0f;
        if (m_octLevel > 0.0f) sub += m_octLevel * static_cast<float>(std::sin(4.0 * 3.141592653589793 * m_subPhase));
        m_subPhase += subInc;
        if (m_subPhase >= 1.0) m_subPhase -= 1.0;

        if (m_gate > 0 && --m_gate == 0) {
            m_amp.noteOff();
            m_mod.noteOff();
            m_fAdsr.noteOff();
        }
        const float a = m_amp.process();
        out[i] = (y + sub) * a * m_ducker.next() * m_level * m_levelMod;
        ++m_modPos;
        m_beat += m_beatsPerSample;
    }
}

} // namespace phos
