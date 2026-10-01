/**
 * @file Texture.h
 * @brief The shamanic bed: singing bowls, a didgeridoo and a jaw harp, synthesised (19.09.2026, round
 *        "fx-psychedelia").
 *
 * The user's inventory of a psytrance track names an organic, shamanic layer -- didgeridoo, jaw harp,
 * singing bowls -- mostly in intros, breakdowns and outros and low in the mix. None of it is sampled;
 * each instrument is the smallest model that has the property the ear identifies it by:
 *
 *  - **Singing bowl: inharmonic modes that beat.** A bowl rings in its bending modes, whose frequencies
 *    for a thin ring go as n (n^2 - 1) / sqrt(n^2 + 1) for n = 2, 3, 4, ... (Rayleigh, "The Theory of
 *    Sound", 2nd ed. 1894, vol. 1, ch. X on the flexural vibrations of rings), which gives the ratios
 *    1 : 2.83 : 5.42 : 8.77. A real bowl is never perfectly round, so each mode splits into two close
 *    frequencies, and the two beat -- the slow "wah-wah" that makes a bowl sing. Four modes, each a
 *    doublet split by 0.2 to 0.4 %, the higher ones dying faster. Struck, not rubbed.
 *  - **Didgeridoo: a buzzing drone under a moving formant.** Lips buzzing into a tube give a harmonic
 *    source with the odd harmonics strong (a tube closed at the lips); the player's vocal tract moves a
 *    formant over it -- the "wow" -- in a rhythm, and circular breathing leaves a short dip and an
 *    inhale every couple of bars. Additive harmonics up to 5 kHz, a resonant band pass whose centre
 *    follows an eighth-note accent pattern drawn per event, and a breath every two bars.
 *  - **Jaw harp: a buzzing reed through a sweeping mouth resonance.** A plucked lamella rich in
 *    harmonics, the mouth a narrow resonance that picks out one harmonic after another as it moves --
 *    the "boing". Plucked on an eighth-note pattern; every pluck swings the resonance the other way.
 *
 * **The depth rule.** Below 100 Hz only kick and sub may play, and 140 .. 350 Hz belongs to acid and
 * snare body. The didgeridoo's and jaw harp's fundamentals sit at 65 .. 185 Hz, so both end in a
 * 36 dB/octave high pass, the didgeridoo at 200 Hz and the jaw harp at 300 Hz: what remains is their
 * harmonics and formants, and the ear supplies the missing fundamental. (A 24 dB/octave slope at
 * 150 Hz left 0.5 % of their power under 140 Hz -- -23 dB, where the depth rule asks for -30.) The
 * bowl starts at C4 (262 Hz) and needs no filter.
 *
 * Everything that is drawn (bowl octave, doublet splits, accent and pluck patterns) comes from the
 * @c pick the engine derives from the event's own position, so a bar rendered alone sounds like the
 * same bar inside the set.
 */
#pragma once
#include "phos/Dsp.h"
#include "phos/Sfx.h"
#include <cstdint>

namespace phos {

/** @brief The shamanic bed's generator (part Texture). */
class Texture {
public:
    static constexpr int kVoices = 3;   ///< simultaneous instruments
    static constexpr int kModes = 4;    ///< bowl modes (each a doublet)

    /** @brief Prepares for @p sampleRate. */
    void prepare(double sampleRate);
    /** @brief Silences every instrument. */
    void reset();
    /** @brief Reads the effective parameters (indexed by texture::) and the key. */
    void update(const float* v, int keyRoot);
    /**
     * @brief Starts an instrument.
     * @param type          SfxType::Bowl, Didgeridoo or JawHarp (anything else is ignored)
     * @param samples       the event's length (a drone plays that long; a bowl rings out its decay)
     * @param velocity      0..1
     * @param late          sub-sample onset (0 <= late < 1)
     * @param samplesPerBeat the tempo, for the rhythm of breath, accents and plucks
     * @param pick          the event's own seed (Engine.cpp derives it from the event's beat)
     */
    void trigger(SfxType type, int samples, float velocity, double late, double samplesPerBeat, uint64_t pick);
    /** @brief Renders @p n stereo samples, replacing @p L and @p R. */
    void process(float* L, float* R, int n);
    /** @brief Instruments sounding. */
    int active() const;
    /** @brief The bowl's mode ratios (Rayleigh's ring), for the self test. */
    static double bowlRatio(int mode);

private:
    /** @brief One sounding instrument of the bed. */
    struct Voice {
        SfxType type = SfxType::Bowl;   ///< which instrument
        bool on = false;   ///< sounding
        long long pos = 0;   ///< samples played
        long long length = 1;   ///< the event's length, samples
        double late = 0.0;   ///< the sub-sample onset
        double spb = 20000.0;   ///< samples per beat
        float velocity = 1.0f;   ///< 0..1
        uint64_t age = 0;   ///< trigger order, for voice stealing
        Rng rng;   ///< the event's own random stream
        // Bowl: two partials per mode as rotating phasors (re, im) with their per-sample rotation.
        double re[2 * kModes] = {};   ///< the partials' phasors: real part
        double im[2 * kModes] = {};   ///< ... imaginary part
        double c[2 * kModes] = {};   ///< their rotation per sample: cosine
        double s[2 * kModes] = {};   ///< ... sine
        double amp[2 * kModes] = {};   ///< each partial's amplitude
        // Didgeridoo and jaw harp.
        double f0 = 100.0;   ///< the drone's fundamental, Hz
        double ph = 0.0;   ///< ... and its phase, cycles
        static constexpr int kMaxHarmonics = 96;   ///< 6 kHz over the lowest root (65 Hz)
        float hw[kMaxHarmonics] = {};               ///< harmonic weights, computed at the trigger
        int harmonics = 0;   ///< harmonics in use
        uint8_t pattern = 0;   ///< eighth notes of a bar that carry an accent or a pluck
        int lastEighth = -1;   ///< the eighth note last looked at
        float accent = 0.0f;   ///< the accent's level
        float pluck = 0.0f;   ///< the pluck's envelope
        float formant = 0.0f;   ///< the formant's position
        float target = 0.0f;   ///< where it goes
        bool up = false;   ///< the jaw harp's sweep goes up on this pluck (it alternates)
        Svf f1;   ///< the first formant
        Svf f2;   ///< the second formant
        Svf hp1;   ///< the high pass: first section
        Svf hp2;   ///< ... second
        Svf hp3;   ///< ... third
        Svf breathBp;   ///< the breath's band pass
        double panPh = 0.0;   ///< the slow pan's phase
    };
    /** @brief One sample of a voice, already placed in the stereo field. */
    void voiceSample(Voice& v, float& l, float& r);

    double sr_ = 48000.0;   ///< sample rate
    Voice voice_[kVoices];   ///< the instruments
    uint64_t counter_ = 0;   ///< triggers so far (the voices' age)
    int keyRoot_ = 6;   ///< the key's pitch class, for the tuning
    float width_ = 0.8f;   ///< the width
    float bowlDecay_ = 7.0f;   ///< the bowl's decay, s
    float bowlBright_ = 0.4f;   ///< the bowl's brightness
    float didgeFormant_ = 0.5f;   ///< the didgeridoo's formant
    float didgeBreath_ = 0.5f;   ///< the didgeridoo's breath
    float jawSweep_ = 0.6f;   ///< the jaw harp's sweep
};

} // namespace phos
