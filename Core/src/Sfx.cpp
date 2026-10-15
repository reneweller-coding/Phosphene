/**
 * @file Sfx.cpp
 * @brief Effect voices.
 */
#include "phos/Sfx.h"
#include "phos/Harmony.h"
#include "phos/Params.h"
#include <algorithm>
#include <cmath>

namespace phos {

const char* const SfxTypeNames[NumSfxTypes] = { "Riser", "Downlifter", "Impact", "Sweep", "Formant Shot", "Reverse Swell", "Zap",
                                                  "Squelch", "Bubble", "Stutter", "Sub Drop", "Reverse Crash",
                                                  "Formant Voice", "Alien Chatter", "Spoken Word", "Voice Chop",
                                                  "Singing Bowl", "Didgeridoo", "Jaw Harp", "Atmosphere", "Space Shot" };

namespace {
constexpr double PiD = 3.141592653589793;   ///< pi

/**
 * @brief Level of each type relative to sfx.level, in dB (18.09.2026, round "mix-foundation").
 *
 * The seven types come out of their synthesis at very different levels -- a narrow band pass over
 * noise has a fraction of the power of a broad one -- so one gain for all of them cannot be right.
 * Measured on the listening seed with the master's dynamics off, as the momentary (400 ms) loudness
 * of the SFX strip at an event's loudest moment against the mix's in the same window: impacts peaked
 * 14.9 dB under the mix, the riser 9.4 and sweeps 8.9, while a downlifter reached -4.7 -- and with
 * sfx.level at -12 dB. sfx.level is now -3 dB, and this table takes the types back apart.
 * The table evens that out towards two targets: the markers of a transition (riser, impact, formant
 * shot, downlifter) about 4 to 6 dB under the mix at their peak, which is prominent without covering
 * the drop, and the short ear candy (sweeps, swells, zaps) about 8 to 10 dB under it, heard as a
 * detail. The measurement and its targets are in docs/rounds/2026-09.md (round "mix-foundation").
 */
constexpr float TypeGainDb[] = {
    +0.0f,    // Riser (19.09.2026: -2 -> 0 dB; it now arrives on beat 4 of the pre-drop break, over the loudest bar of the roll)
    -8.0f,    // Downlifter
    +3.0f,    // Impact
    -7.0f,    // Sweep
    +1.0f,    // Formant shot
    -2.0f,    // Reverse swell
    +2.0f,    // Zap
    // 19.09.2026, round "fx-psychedelia": short ear candy, heard as a detail like the zap (docs/rounds/2026-09.md
    // has the measurement against the mix). The types of the other two generators carry 0 here.
    +0.0f,    // Squelch
    +0.0f,    // Bubble
     0.0f,    // Stutter (the engine's buffer repeat; this generator stays silent)
     0.0f,    // Sub drop (its own level, sfx.sub_level)
    -4.0f,    // Reverse crash
     0.0f, 0.0f, 0.0f, 0.0f,   // Vocal types
     0.0f, 0.0f, 0.0f,         // Texture types
    -5.0f,    // Atmosphere (23.09.2026): a background layer, under the candy
     0.0f,    // Space shot (27.09.2026): played by the Field track's FieldShot, not by this generator
};
static_assert(sizeof(TypeGainDb) / sizeof(TypeGainDb[0]) == NumSfxTypes, "one gain per effect type");
/** @brief Band-pass output of an SVF at unity gain in the centre. */
inline float bandPass(Svf& f, float x)
{
    float lp, bp, hp;
    f.tick(x, lp, bp, hp);
    return bp * f.m_k;
}
} // namespace

void Sfx::prepare(double sampleRate)
{
    m_sr = sampleRate;
    reset();
}

void Sfx::reset()
{
    for (int i = 0; i < Voices; ++i) {
        m_voice[i] = Voice{};
        m_voice[i].m_rng.seed(0x5346580000ull + static_cast<uint64_t>(i));
    }
    m_counter = 0;
}

double sfxToneInterval(int scale, double iv)
{
    if (scale < 0) return iv;
    // An interval the mode does not have goes to its neighbour that it does: up first (the major third of Phrygian
    // dominant for a minor one), then down. Never onto the b2 or the leading tone (25.09.2026): an atmosphere is a
    // long, pad-like layer, and held over the tonic either is the semitone rub the pad and the drone may not hold
    // either -- the minor seventh of harmonic minor went up to the leading tone until then. Such a tone takes the
    // fifth of its octave, the literature's "+7 st above the root" for pads and atmospheres.
    const int semis = static_cast<int>(std::lround(iv));
    double out = iv;
    if (!inScale(scale, semis)) {
        if (inScale(scale, semis + 1) && !rubsTonic(semis + 1)) out = iv + 1.0;
        else if (inScale(scale, semis - 1) && !rubsTonic(semis - 1)) out = iv - 1.0;
        else out = iv + 1.0;   // only a rub is near: the fifth below takes it
    }
    const int got = static_cast<int>(std::lround(out));
    if (rubsTonic(got)) {
        const int octave = static_cast<int>(std::floor(static_cast<double>(got) / 12.0));
        return 12.0 * octave + 7.0;
    }
    return out;
}

double Sfx::interval(const SfxPreset* P, double fallback) const
{
    return sfxToneInterval(m_scale, P != nullptr ? static_cast<double>(P->m_pitchInterval) : fallback);
}

void Sfx::update(const float* v, int keyRoot)
{
    m_keyRoot = keyRoot;
    m_level = dbToGain(v[sfx::LEVEL]);
    m_noise = v[sfx::NOISE];
    m_resonance = v[sfx::RESONANCE];
    m_brightness = v[sfx::BRIGHTNESS];
    m_impactDecay = v[sfx::IMPACT_DECAY] * 0.001f;
    m_vowel = v[sfx::VOWEL];
    m_swellDecay = v[sfx::SWELL_DECAY] * 0.001f;
    m_width = v[sfx::WIDTH];
    m_subLevel = dbToGain(v[sfx::SUB_LEVEL]);
    m_wander = v[sfx::WANDER] >= 0.5f;
    m_wanderSend = v[sfx::WANDER_SEND];
    for (int i = 0; i < sfx::NumPresetChoices; ++i) m_fixed[i] = static_cast<int>(std::lround(v[sfx::FirstPreset + i]));
}

int Sfx::active() const
{
    int n = 0;
    for (const Voice& v : m_voice) n += v.m_on ? 1 : 0;
    return n;
}

void Sfx::trigger(SfxTypeE type, int samples, float velocity, double late, int preset)
{
    // The stutter is the engine's, and the voices and the bed have generators of their own.
    if (type == SfxTypeE::STUTTER || type == SfxTypeE::SPACE_SHOT || sfxTypePart(type) != PartE::SFX || type >= SfxTypeE::COUNT) return;
    // The page's choice for the family, where it is not Auto (24.09.2026, sfx.preset_*).
    const int choice = sfxPresetChoice(type);
    if (choice >= 0 && m_fixed[choice] > 0) preset = m_fixed[choice];
    int slot = 0;
    for (int i = 0; i < Voices; ++i) {
        if (!m_voice[i].m_on) { slot = i; break; }
        if (m_voice[i].m_age < m_voice[slot].m_age) slot = i;
    }
    Voice& v = m_voice[slot];
    const Rng keep = v.m_rng;
    v = Voice{};
    v.m_rng = keep;
    v.m_type = type;
    v.m_on = true;
    v.m_velocity = clampv(velocity, 0.0f, 1.0f);
    v.m_late = late;
    v.m_age = ++m_counter;
    v.m_preset = sfxPreset(type, preset);
    double seconds = samples / m_sr;
    // The preset's length scale: one-shots only -- everything that swells or rises ends on its target beat.
    if (v.m_preset != nullptr && (type == SfxTypeE::IMPACT || type == SfxTypeE::ZAP || type == SfxTypeE::SQUELCH || type == SfxTypeE::BUBBLE
                                || type == SfxTypeE::FORMANT_SHOT || type == SfxTypeE::SWEEP))
        seconds *= v.m_preset->m_lengthScale;
    if (type == SfxTypeE::IMPACT) seconds = std::max(seconds, static_cast<double>(m_impactDecay));
    if (type == SfxTypeE::FORMANT_SHOT) seconds = clampv(seconds, 0.08, 0.6);
    if (type == SfxTypeE::ZAP) seconds = std::max(seconds, 0.25);
    if (type == SfxTypeE::SQUELCH) seconds = clampv(seconds, 0.06, 0.5);
    if (type == SfxTypeE::BUBBLE) seconds = clampv(seconds, 0.12, 0.8);
    if (type == SfxTypeE::SUB_DROP) seconds = std::max(seconds, 1.2);
    v.m_length = std::max<long long>(1, static_cast<long long>(seconds * m_sr));
    if (type == SfxTypeE::BUBBLE) {
        // Three to seven bubbles spread over the first two thirds of the event, each drawn once here
        // from the voice's own stream: onset, start frequency (1.2 .. 4 kHz), decay (12 .. 40 ms) and a
        // rise of 60 .. 160 % of the start frequency per second of its life -- small bubbles are high and
        // short, which the draw keeps by tying the decay to the frequency.
        v.m_bubbles = 3 + v.m_rng.below(5);
        for (int b = 0; b < v.m_bubbles; ++b) {
            const double u = v.m_rng.uniform();
            v.m_bubT[b] = b == 0 ? 0.0 : (2.0 / 3.0) * seconds * v.m_rng.uniform();
            v.m_bubF[b] = 1200.0 * std::pow(4000.0 / 1200.0, u);
            v.m_bubTau[b] = 0.040 - 0.028 * u;
            v.m_bubRise[b] = (0.6 + v.m_rng.uniform()) / (4.0 * v.m_bubTau[b]);   // over ~4 tau the pitch rises 0.6 .. 1.6 x
            v.m_bubPh[b] = 0.0;
        }
    }
    const float sr = static_cast<float>(m_sr);
    for (Svf* f : { &v.m_hpL1, &v.m_hpL2, &v.m_hpR1, &v.m_hpR2 }) f->setQ(150.0f, 0.70710678f, sr);
    if (type == SfxTypeE::FORMANT_SHOT) {
        // Peterson and Barney formants, a (0) to u (1), as in the vocal wavetable.
        static const double A[3] = { 730, 1090, 2440 }, U[3] = { 300, 870, 2240 };
        const double vowel = v.m_preset != nullptr ? clampv(static_cast<double>(v.m_preset->m_motionRate), 0.0, 1.0) : static_cast<double>(m_vowel);
        for (int i = 0; i < 3; ++i) v.m_formant[i].setQ(static_cast<float>(A[i] + vowel * (U[i] - A[i])), 6.0f, sr);
    }
    v.m_panPh = 0.25;
    v.m_typeGain = dbToGain(TypeGainDb[static_cast<int>(type)]);
    // 20.09.2026, round "wandering-fx": the directed trajectory (Sfx.h), drawn once here from the
    // voice's own seed stream so a fresh Sfx (Sfx::reset() reseeds every voice the same way) draws the
    // same shape every time it plays this slot in this position in the stream. Not the sub drop: it
    // stays centred and mono under the kick by its own design (Sfx.h), and a growing send into the hall
    // would fight the depth rule (the return's low cut never goes below 150 Hz).
    v.m_wander = m_wander && type != SfxTypeE::SUB_DROP;
    if (v.m_wander) {
        const bool leftToRight = v.m_rng.below(2) == 0;   // the seed's choice, not always left to right
        const float a = 0.75f + 0.20f * v.m_rng.uniform();   // 0.75 .. 0.95: "far back" on its start side
        const float b = 0.75f + 0.20f * v.m_rng.uniform();   // its own draw for the far side it ends on
        v.m_panFrom = leftToRight ? -a : b;
        v.m_panTo = leftToRight ? b : -a;
        v.m_panCurve = 0.6f + 1.2f * v.m_rng.uniform();   // 0.6 .. 1.8: eases in or out, never a straight ramp
        v.m_wetExpo = 0.6f + 1.4f * v.m_rng.uniform();    // 0.6 .. 2.0: some events turn wet early, some late
    }
}

float Sfx::voiceSample(Voice& v, float& pan, float& wetFrac)
{
    const float sr = static_cast<float>(m_sr);
    const double posD = static_cast<double>(v.m_pos) + v.m_late;
    const double t = posD / m_sr;
    const double x = std::min(1.0, posD / static_cast<double>(v.m_length));
    const double rootHz = midiToHz(60 + ((m_keyRoot % 12) + 12) % 12);   // the key's root in the octave from C4
    float s = 0.0f, amp = 0.0f;
    double panRate = 0.3;
    // The bank preset's hand on the type (23.09.2026, round "SFX"): every constant of the syntheses below
    // has its preset field, and without a preset (lane 0) the field's default is the synthesis's own constant.
    const SfxPreset* P = v.m_preset;
    switch (v.m_type) {
    case SfxTypeE::RISER:
    case SfxTypeE::DOWNLIFTER: {
        const double u = v.m_type == SfxTypeE::RISER ? x : 1.0 - x;
        const double octLo = P ? P->m_filterLo : 1.0, octHi = P ? P->m_filterHi : 1.0 + 3.6 + 0.8 * m_brightness;
        const float fc = static_cast<float>(200.0 * std::pow(2.0, octLo + (octHi - octLo) * u));
        const double res = P ? P->m_resonance : m_resonance;
        v.m_bp.set(fc, static_cast<float>(0.2 + 0.7 * res * u), sr);
        const float n = bandPass(v.m_bp, v.m_rng.bipolar());
        const double hz = rootHz * std::pow(2.0, interval(P, 0.0) / 12.0) * std::pow(2.0, 2.0 * u);
        const double spread = P ? 0.002 + 0.012 * P->m_detune : 0.006;
        v.m_saw1.set(hz * (1.0 + spread), m_sr, 0.0f, 0.5f);
        v.m_saw2.set(hz * (1.0 - spread), m_sr, 0.0f, 0.5f);
        v.m_lp.set(static_cast<float>(600.0 * std::pow(2.0, 4.0 * u)), 0.1f, sr);
        const float tone = v.m_lp.lp(0.5f * (v.m_saw1.next() + v.m_saw2.next()));
        const float mix = P ? 1.0f - P->m_toneMix : m_noise;
        s = mix * n + (1.0f - mix) * tone;
        amp = static_cast<float>(std::pow(u, P ? P->m_envShape : 2.0f));
        if (v.m_type == SfxTypeE::DOWNLIFTER) amp *= static_cast<float>(std::min(1.0, t / 0.005));
        panRate = P ? P->m_motionRate * (0.2 + 0.8 * u * u) : 0.5 + 7.5 * u * u;
        break;
    }
    case SfxTypeE::ATMOSPHERE: {
        // The background layer (Myloops on risers and atmospheres; the user's "flaechigere und laengere
        // Effekte"): two detuned saws on the root and one on the preset's interval through a low pass
        // whose cutoff breathes between the preset's two octaves at its motion rate, a band of noise at
        // the same place, a metallic ring where the preset asks for it -- under a raised-cosine swell
        // that rises over the preset's attack share of the event, holds, and leaves over the last
        // quarter so that the event ends on its target beat.
        const double lenS = static_cast<double>(v.m_length) / m_sr;
        const double lo = P ? P->m_filterLo : 1.5, hi = P ? P->m_filterHi : 3.0, rate = P ? P->m_motionRate : 0.1;
        v.m_lfoPh += rate / m_sr;
        if (v.m_lfoPh >= 1.0) v.m_lfoPh -= 1.0;
        const double breathe = 0.5 - 0.5 * std::cos(2.0 * PiD * v.m_lfoPh);
        const float fc = static_cast<float>(200.0 * std::pow(2.0, lo + (hi - lo) * breathe));
        const float q = static_cast<float>(0.15 + 0.6 * (P ? P->m_resonance : 0.3));
        const double iv = interval(P, 7.0);
        const double spread = 0.003 + 0.02 * (P ? P->m_detune : 0.4);
        v.m_saw1.set(rootHz * (1.0 + spread), m_sr, 0.0f, 0.5f);
        v.m_saw2.set(rootHz * std::pow(2.0, iv / 12.0) * (1.0 - spread), m_sr, 0.0f, 0.5f);
        v.m_lp.set(fc, q, sr);
        float tone = v.m_lp.lp(0.5f * (v.m_saw1.next() + v.m_saw2.next()));
        v.m_bp.set(fc * 1.5f, 0.6f, sr);
        const float n = bandPass(v.m_bp, v.m_rng.bipolar());
        const double metal = P ? P->m_metal : 0.0;
        if (metal > 0.0) {
            v.m_chordPh[0] += (300.0 + 900.0 * (P ? P->m_resonance : 0.5)) / m_sr;
            if (v.m_chordPh[0] >= 1.0) v.m_chordPh[0] -= 1.0;
            tone *= static_cast<float>(1.0 - metal + metal * (v.m_chordPh[0] < 0.5 ? 1.0 : -1.0));
        }
        const float mix = P ? P->m_toneMix : 0.6f;
        s = mix * tone + (1.0f - mix) * 0.7f * n;
        const double attack = clampv(P ? static_cast<double>(P->m_envShape) : 0.3, 0.05, 0.7) * lenS;
        const double release = 0.25 * lenS;
        double env = 1.0;
        if (t < attack) env = 0.5 - 0.5 * std::cos(PiD * t / attack);
        else if (t > lenS - release) env = 0.5 + 0.5 * std::cos(PiD * (t - (lenS - release)) / release);
        amp = static_cast<float>(std::max(0.0, env));
        panRate = 0.05 + 0.5 * (P ? P->m_panSpeed : 0.2);
        break;
    }
    case SfxTypeE::IMPACT: {
        const double decay = m_impactDecay * (P ? P->m_lengthScale : 1.0f);
        const double top = P ? 200.0 * std::pow(2.0, P->m_filterLo) * 2.0 : 6500.0;
        v.m_lp.set(static_cast<float>(500.0 + (top - 500.0) * std::exp(-t / 0.35)), 0.1f, sr);
        const float n = v.m_lp.lp(v.m_rng.bipolar()) * static_cast<float>(std::exp(-6.9 * t / decay));
        const double base = 160.0 * std::pow(2.0, interval(P, 0.0) / 12.0);
        const double f = base + 260.0 * std::exp(-t / 0.04);
        v.m_sinePh += f / m_sr;
        const float thump = static_cast<float>(std::sin(2.0 * PiD * v.m_sinePh) * 0.9 * std::exp(-t / (0.25 * (P ? P->m_envShape : 1.0f))));
        const float nMix = P ? 0.3f + 0.6f * (1.0f - P->m_toneMix) : 0.6f;
        s = nMix * n + thump;
        amp = static_cast<float>(std::min(1.0, t / 0.001));
        panRate = 0.0;
        break;
    }
    case SfxTypeE::SWEEP: {
        const double lo = P ? P->m_filterLo : 0.585, hi = P ? P->m_filterHi : 5.585;   // the defaults: 300 Hz .. 300 * 2^5
        const float fc = static_cast<float>(200.0 * std::pow(2.0, lo + (hi - lo) * std::sin(PiD * x)));
        v.m_bp.set(fc, static_cast<float>(0.6 + 0.35 * (P ? P->m_resonance : m_resonance)), sr);
        s = bandPass(v.m_bp, v.m_rng.bipolar());
        amp = static_cast<float>(std::pow(std::max(0.0, std::sin(PiD * x)), 0.5 * (P ? P->m_envShape : 1.0f)));
        panRate = P ? 0.1 + 0.8 * P->m_panSpeed : 0.25;
        break;
    }
    case SfxTypeE::FORMANT_SHOT: {
        v.m_saw1.set(2.0 * rootHz * std::pow(2.0, interval(P, 0.0) / 12.0) * std::pow(2.0, -5.0 * x / 12.0), m_sr, 0.0f, 0.5f);
        const float src = v.m_saw1.next();
        s = bandPass(v.m_formant[0], src) + 0.5f * bandPass(v.m_formant[1], src) + 0.25f * bandPass(v.m_formant[2], src);
        const double lenS = static_cast<double>(v.m_length) / m_sr;
        amp = static_cast<float>(std::min(1.0, t / 0.003) * std::exp(-t / (0.3 * lenS * (P ? P->m_envShape : 1.0f))));
        panRate = 0.0;
        break;
    }
    case SfxTypeE::REVERSE_SWELL: {
        const double lenS = static_cast<double>(v.m_length) / m_sr;
        v.m_lp.set(static_cast<float>(P ? 200.0 * std::pow(2.0, P->m_filterHi) : 2500.0 + 5000.0 * m_brightness), 0.1f, sr);
        const float n = v.m_lp.lp(v.m_rng.bipolar());
        float chord = 0.0f;
        const double Interval[3] = { 0.0, interval(P, 7.0), 12.0 };
        for (int i = 0; i < 3; ++i) {
            v.m_chordPh[i] += rootHz * std::pow(2.0, Interval[i] / 12.0) / m_sr;
            if (v.m_chordPh[i] >= 1.0) v.m_chordPh[i] -= 1.0;
            chord += static_cast<float>(std::sin(2.0 * PiD * v.m_chordPh[i]));
        }
        s = n + (P ? 0.08f + 0.3f * P->m_toneMix : 0.12f) * chord;
        amp = static_cast<float>(std::exp(-6.9 * (lenS - t) / std::max(0.05, static_cast<double>(m_swellDecay) * (P ? P->m_envShape : 1.0f))));
        panRate = P ? 0.05 + 0.5 * P->m_panSpeed : 0.15;
        break;
    }
    case SfxTypeE::ZAP: {
        const double floorHz = P ? 200.0 * std::pow(2.0, P->m_filterHi) : 200.0, topHz = P ? 200.0 * std::pow(2.0, P->m_filterLo) : 3000.0;
        const double f = floorHz + (topHz - floorHz) * std::exp(-t / (0.02 * (P ? 0.4 + 0.8 * P->m_motionRate / 1.5 : 1.0)));
        v.m_sinePh += f / m_sr;
        s = static_cast<float>(std::sin(2.0 * PiD * v.m_sinePh));
        if (P && P->m_metal > 0.0f) s *= static_cast<float>(1.0 - P->m_metal + P->m_metal * std::sin(2.0 * PiD * v.m_sinePh * 3.0));
        amp = static_cast<float>(std::exp(-t / (0.08 * (P ? P->m_envShape : 1.0f))));
        panRate = 0.0;
        break;
    }
    case SfxTypeE::SQUELCH: {
        // A saw two octaves over the key's root (a bright, harmonic-rich source) through a resonant band
        // pass that sweeps from 3 kHz up to 3 * 2^1.3 = 7.4 kHz and back on a half sine: the squelch is
        // the resonance, so the resonance is what has to sit in the 3 .. 8 kHz pocket (a sweep from
        // 1.4 kHz measured 29 % of the power there, the saw's fundamental taking the rest). A small
        // upward pitch blip in the first 30 ms makes it liquid rather than a filter sweep.
        const double hz = rootHz * std::pow(2.0, interval(P, 24.0) / 12.0) * (1.0 + 0.5 * std::exp(-t / 0.03));
        v.m_saw1.set(hz, m_sr, 0.0f, 0.5f);
        const double sweep = std::sin(PiD * x);
        const double centre = P ? 200.0 * std::pow(2.0, P->m_filterLo) : 3000.0;
        v.m_bp.setQ(static_cast<float>(centre * std::pow(2.0, (P ? 0.6 + 1.2 * P->m_filterHi / 1.8 : 1.3) * sweep)), static_cast<float>(4.0 + 6.0 * (P ? P->m_resonance : m_resonance)), sr);
        s = bandPass(v.m_bp, v.m_saw1.next());
        amp = static_cast<float>(std::min(1.0, t / 0.002) * std::exp(-2.0 * x * (P ? P->m_envShape / 2.0 : 1.0)));
        panRate = 0.0;
        break;
    }
    case SfxTypeE::BUBBLE: {
        // van den Doel's bubble: a decaying sinusoid whose frequency rises, f(t) = f0 (1 + rise t).
        s = 0.0f;
        for (int b = 0; b < v.m_bubbles; ++b) {
            const double tb = t - v.m_bubT[b];
            if (tb < 0.0 || tb > 6.0 * v.m_bubTau[b]) continue;
            v.m_bubPh[b] += v.m_bubF[b] * (1.0 + v.m_bubRise[b] * tb) / m_sr;
            if (v.m_bubPh[b] >= 1.0) v.m_bubPh[b] -= 1.0;
            s += static_cast<float>(std::sin(2.0 * PiD * v.m_bubPh[b]) * std::min(1.0, tb / 0.001) * std::exp(-tb / v.m_bubTau[b]));
        }
        amp = 0.6f;
        panRate = P ? 0.5 + 3.0 * P->m_panSpeed : 2.0;
        break;
    }
    case SfxTypeE::SUB_DROP: {
        // A sine falling from 110 Hz to 32 Hz with a time constant of 0.3 s; its phase is integrated, so
        // the glide never steps. It is the effect that lives where the kick lives -- processSplit sends
        // it to the engine on its own output, which ducks it under every kick (Engine.cpp).
        const double f = 32.0 + 78.0 * std::exp(-t / 0.3);
        v.m_sinePh += f / m_sr;
        if (v.m_sinePh >= 1.0) v.m_sinePh -= 1.0;
        s = static_cast<float>(std::sin(2.0 * PiD * v.m_sinePh));
        const double lenS = static_cast<double>(v.m_length) / m_sr;
        amp = static_cast<float>(std::min(1.0, t / 0.005) * std::exp(-2.5 * t / lenS));
        panRate = 0.0;
        break;
    }
    case SfxTypeE::REVERSE_CRASH: {
        // A cymbal wash: noise above 5 kHz, ring-modulated by two inharmonic square waves (the metallic
        // partials), under an envelope that rises exponentially -- a reversed crash ends on its beat.
        v.m_bp.setQ(static_cast<float>(P ? 200.0 * std::pow(2.0, P->m_filterLo) : 7000.0), static_cast<float>(0.5 + 0.6 * (P ? P->m_resonance : 0.5)), sr);
        const float n = bandPass(v.m_bp, v.m_rng.bipolar());
        const double m = P ? 0.6 + 0.8 * P->m_metal : 1.0;
        v.m_chordPh[0] += 540.0 * m / m_sr; if (v.m_chordPh[0] >= 1.0) v.m_chordPh[0] -= 1.0;
        v.m_chordPh[1] += 1173.0 * m / m_sr; if (v.m_chordPh[1] >= 1.0) v.m_chordPh[1] -= 1.0;
        const float sq = (v.m_chordPh[0] < 0.5 ? 1.0f : -1.0f) * (v.m_chordPh[1] < 0.5 ? 1.0f : -1.0f);
        v.m_lp.set(static_cast<float>(P ? 200.0 * std::pow(2.0, P->m_filterHi + 0.3) : 10000.0), 0.1f, sr);
        s = v.m_lp.lp(n * (0.6f + 0.4f * sq));
        amp = static_cast<float>(std::exp(-(P ? P->m_envShape : 5.0f) * (1.0 - x)));
        panRate = P ? 0.05 + 0.5 * P->m_panSpeed : 0.2;
        break;
    }
    default: break;
    }
    // Rising effects end on their beat with a short fade; the others end when they have decayed.
    if (v.m_pos >= v.m_length) {
        const bool riseToEnd = v.m_type == SfxTypeE::RISER || v.m_type == SfxTypeE::REVERSE_SWELL || v.m_type == SfxTypeE::REVERSE_CRASH || v.m_type == SfxTypeE::ATMOSPHERE;
        const double fade = riseToEnd ? 0.02 : 0.005;
        const double over = static_cast<double>(v.m_pos - v.m_length) / m_sr;
        amp *= static_cast<float>(std::max(0.0, 1.0 - over / fade));
        if (over >= fade) v.m_on = false;
    }
    if (v.m_wander) {
        // A directed sweep instead of the oscillation: pan(x) eases from panFrom to panTo with the
        // event's own curve exponent. x is the same absolute-position fraction every type's synthesis
        // above already uses (posD / length, capped at 1), so the sweep needs no state of its own beyond
        // it -- block-size independent for the same reason the rest of the voice is (Engine.h).
        const double xp = std::pow(x, static_cast<double>(v.m_panCurve));
        pan = m_width * static_cast<float>(v.m_panFrom + (v.m_panTo - v.m_panFrom) * xp);
    } else {
        v.m_panPh += panRate / m_sr;
        if (v.m_panPh >= 1.0) v.m_panPh -= 1.0;
        pan = m_width * static_cast<float>(std::sin(2.0 * PiD * v.m_panPh));
    }
    ++v.m_pos;
    const float total = s * amp * v.m_velocity * v.m_typeGain;
    if (v.m_wander) {
        // Dry at onset, wet by the tail: a smoothstep of x^wetExpo, so wetFrac(0) = 0 and wetFrac(1) =
        // wander_send exactly, whatever wetExpo this event's seed drew (0^k = 0, 1^k = 1 for any k > 0)
        // -- the closed form Tests/selftest.cpp checks against (testWanderingFx).
        const double xe = std::pow(x, static_cast<double>(v.m_wetExpo));
        const double sm = xe * xe * (3.0 - 2.0 * xe);
        wetFrac = static_cast<float>(clampv(sm, 0.0, 1.0)) * m_wanderSend;
    } else {
        wetFrac = 0.0f;
    }
    return total;
}

void Sfx::process(float* L, float* R, int n)
{
    float sub[64], wetL[64], wetR[64];
    for (int done = 0; done < n; done += 64) {
        const int m = std::min(64, n - done);
        processSplit(L + done, R + done, sub, wetL, wetR, m);
        // This call has no reverb to hand the wandering trajectory's wet share to (that is Engine.cpp's
        // job, Engine.h), so it folds it straight back into the dry output alongside the sub drop --
        // energy-preserving and silent when sfx.wander is off, since wetL/wetR are then exactly zero.
        for (int i = 0; i < m; ++i) { L[done + i] += sub[i] + wetL[i]; R[done + i] += sub[i] + wetR[i]; }
    }
}

void Sfx::processSplit(float* L, float* R, float* sub, float* wetL, float* wetR, int n)
{
    for (int i = 0; i < n; ++i) { L[i] = 0.0f; R[i] = 0.0f; sub[i] = 0.0f; wetL[i] = 0.0f; wetR[i] = 0.0f; }
    for (Voice& v : m_voice) {
        if (!v.m_on) continue;
        if (v.m_type == SfxTypeE::SUB_DROP) {
            // Mono and unfiltered: the engine puts it in the centre with kick and bass.
            for (int i = 0; i < n && v.m_on; ++i) {
                float pan = 0.0f, wetFrac = 0.0f;
                sub[i] += voiceSample(v, pan, wetFrac) * m_level * m_subLevel;
            }
            continue;
        }
        for (int i = 0; i < n && v.m_on; ++i) {
            float pan = 0.0f, wetFrac = 0.0f;
            const float s = voiceSample(v, pan, wetFrac) * m_level;
            const double theta = (static_cast<double>(clampv(pan, -1.0f, 1.0f)) + 1.0) * PiD / 4.0;
            float l = s * static_cast<float>(std::cos(theta) * 1.41421356);
            float r = s * static_cast<float>(std::sin(theta) * 1.41421356);
            float lp, bp, hp;
            v.m_hpL1.tick(l, lp, bp, hp); v.m_hpL2.tick(hp, lp, bp, l);
            v.m_hpR1.tick(r, lp, bp, hp); v.m_hpR2.tick(hp, lp, bp, r);
            // The crossfade: the same panned, filtered sample split into a shrinking dry share and a
            // growing wet share, so L + wetL (and R + wetR) always equals the sample before this split --
            // no energy is created or lost, only rerouted towards the hall as the event nears its tail.
            const float wl = l * wetFrac, wr = r * wetFrac;
            L[i] += l - wl;
            R[i] += r - wr;
            wetL[i] += wl;
            wetR[i] += wr;
        }
    }
}

} // namespace phos
