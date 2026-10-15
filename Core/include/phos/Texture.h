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
 * @author Rene Weller
 */
#pragma once
#include "phos/Dsp.h"
#include "phos/Sfx.h"
#include <cstdint>

namespace phos {

/** @brief The shamanic bed's generator (part Texture). */
class Texture {
public:
    static constexpr int Voices = 3;   ///< simultaneous instruments
    static constexpr int Modes = 4;    ///< bowl modes (each a doublet)

    /**
     * @brief Prepares for @p sampleRate.
     * @param[in] sampleRate  the sample rate in Hz
     */
    void prepare(double sampleRate);
    /** @brief Silences every instrument. */
    void reset();
    /**
     * @brief Reads the effective parameters (indexed by texture::) and the key.
     * @param[in] v        the effective parameter values
     * @param[in] keyRoot  the key's root
     */
    void update(const float* v, int keyRoot);
    /**
     * @brief Starts an instrument.
     * @param[in] type            SfxType::Bowl, Didgeridoo or JawHarp (anything else is ignored)
     * @param[in] samples         the event's length (a drone plays that long; a bowl rings out its decay)
     * @param[in] velocity        0..1
     * @param[in] late            sub-sample onset (0 <= late < 1)
     * @param[in] samplesPerBeat  the tempo, for the rhythm of breath, accents and plucks
     * @param[in] pick            the event's own seed (Engine.cpp derives it from the event's beat)
     */
    void trigger(SfxTypeE type, int samples, float velocity, double late, double samplesPerBeat, uint64_t pick);
    /**
     * @brief Renders @p n stereo samples, replacing @p L and @p R.
     * @param[out] L  receives the left channel (replaced)
     * @param[out] R  receives the right channel (replaced)
     * @param[in]  n  how many samples
     */
    void process(float* L, float* R, int n);
    /**
     * @brief Instruments sounding.
     * @return the instruments sounding
     */
    int active() const;
    /**
     * @brief The bowl's mode ratios (Rayleigh's ring), for the self test.
     * @param[in] mode  the mode
     * @return its frequency ratio
     */
    static double bowlRatio(int mode);

private:
    /** @brief One sounding instrument of the bed. */
    struct Voice {
        SfxTypeE m_type = SfxTypeE::BOWL;   ///< which instrument
        bool m_on = false;   ///< sounding
        long long m_pos = 0;   ///< samples played
        long long m_length = 1;   ///< the event's length, samples
        double m_late = 0.0;   ///< the sub-sample onset
        double m_spb = 20000.0;   ///< samples per beat
        float m_velocity = 1.0f;   ///< 0..1
        uint64_t m_age = 0;   ///< trigger order, for voice stealing
        Rng m_rng;   ///< the event's own random stream
        // Bowl: two partials per mode as rotating phasors (re, im) with their per-sample rotation.
        double m_re[2 * Modes] = {};   ///< the partials' phasors: real part
        double m_im[2 * Modes] = {};   ///< ... imaginary part
        double m_c[2 * Modes] = {};   ///< their rotation per sample: cosine
        double m_s[2 * Modes] = {};   ///< ... sine
        double m_amp[2 * Modes] = {};   ///< each partial's amplitude
        // Didgeridoo and jaw harp.
        double m_f0 = 100.0;   ///< the drone's fundamental, Hz
        double m_ph = 0.0;   ///< ... and its phase, cycles
        static constexpr int MaxHarmonics = 96;   ///< 6 kHz over the lowest root (65 Hz)
        float m_hw[MaxHarmonics] = {};               ///< harmonic weights, computed at the trigger
        int m_harmonics = 0;   ///< harmonics in use
        uint8_t m_pattern = 0;   ///< eighth notes of a bar that carry an accent or a pluck
        int m_lastEighth = -1;   ///< the eighth note last looked at
        float m_accent = 0.0f;   ///< the accent's level
        float m_pluck = 0.0f;   ///< the pluck's envelope
        float m_formant = 0.0f;   ///< the formant's position
        float m_target = 0.0f;   ///< where it goes
        bool m_up = false;   ///< the jaw harp's sweep goes up on this pluck (it alternates)
        Svf m_f1;   ///< the first formant
        Svf m_f2;   ///< the second formant
        Svf m_hp1;   ///< the high pass: first section
        Svf m_hp2;   ///< ... second
        Svf m_hp3;   ///< ... third
        Svf m_breathBp;   ///< the breath's band pass
        double m_panPh = 0.0;   ///< the slow pan's phase
    };
    /**
     * @brief One sample of a voice, already placed in the stereo field.
     * @param[in,out] v  the voice
     * @param[out]    l  the left sample
     * @param[out]    r  the right sample
     */
    void voiceSample(Voice& v, float& l, float& r);

    double m_sr = 48000.0;   ///< sample rate
    Voice m_voice[Voices];   ///< the instruments
    uint64_t m_counter = 0;   ///< triggers so far (the voices' age)
    int m_keyRoot = 6;   ///< the key's pitch class, for the tuning
    float m_width = 0.8f;   ///< the width
    float m_bowlDecay = 7.0f;   ///< the bowl's decay, s
    float m_bowlBright = 0.4f;   ///< the bowl's brightness
    float m_didgeFormant = 0.5f;   ///< the didgeridoo's formant
    float m_didgeBreath = 0.5f;   ///< the didgeridoo's breath
    float m_jawSweep = 0.6f;   ///< the jaw harp's sweep
};

} // namespace phos
