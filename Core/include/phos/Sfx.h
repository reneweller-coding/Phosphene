/**
 * @file Sfx.h
 * @brief One-shot effects the composer places at points of the form: risers, downlifters, impacts,
 *        sweeps, formant shots, reverse swells and zaps.
 *
 * Everything is synthesised; there are no samples. A note of Part::Sfx names the type by its pitch
 * (kSfxBaseNote + type, so the MIDI export shows each type on its own key) and its length in beats;
 * tonal parts are tuned to the key the engine plays in.
 *
 *  - **Riser**: noise through a band pass whose centre climbs from 400 Hz to 8 kHz with rising
 *    resonance, and two slightly detuned saws gliding up two octaves; the level rises as x^2 and the
 *    stereo motion speeds up towards the end, which arrives exactly on the target beat.
 *  - **Downlifter**: the mirror image, from full level down.
 *  - **Impact**: a noise burst whose low pass closes as it decays, with a tonal thump that falls from
 *    420 Hz to 160 Hz. No sub: under 140 Hz only kick and bass may play (the plan's "Sub-Sinus" is
 *    therefore left to the kick on the same downbeat).
 *  - **Sweep**: noise through a resonant band pass that travels up and back down.
 *  - **Formant shot** (the pre-drop "Abriss" of PLAN 6.2): a saw an octave above the root through
 *    three formant band passes of a vowel, falling a fourth, a fraction of a second long.
 *  - **Reverse swell**: the reversed tail of a reverb, ending on the target beat. A reverb's late tail
 *    is noise with an exponentially decaying envelope, and time-reversed filtered noise has the same
 *    power spectrum as the forward one; the swell is therefore synthesised forward as band-limited noise
 *    under a rising exponential envelope, without rendering and reversing a buffer on the audio thread.
 *    A chord of the key sounds inside it at a lower level.
 *  - **Zap**: a sine falling from 3 kHz within tens of milliseconds.
 *
 * Every voice ends in a 24 dB/octave low cut at 150 Hz. Four voices sound at once; a fifth takes the
 * oldest.
 */
#pragma once
#include "phos/Dsp.h"
#include "phos/Oscillator.h"

namespace phos {

/** @brief Effect types, in the order of their MIDI notes. */
enum class SfxType : int { Riser = 0, Downlifter, Impact, Sweep, FormantShot, ReverseSwell, Zap, Count };
constexpr int kNumSfxTypes = static_cast<int>(SfxType::Count);   ///< number of types
constexpr int kSfxBaseNote = 48;                                   ///< MIDI note of the riser
extern const char* const kSfxTypeNames[kNumSfxTypes];             ///< display names

/** @brief The effect generator. */
class Sfx {
public:
    static constexpr int kVoices = 4;   ///< simultaneous effects

    /** @brief Prepares for a sample rate. */
    void prepare(double sampleRate);
    /** @brief Silences every voice. */
    void reset();
    /** @brief Reads the effective parameters (indexed by sfx::) and the key. */
    void update(const float* v, int keyRoot);
    /**
     * @brief Starts an effect.
     * @param type     the effect
     * @param samples  its length
     * @param velocity 0..1
     * @param late     how many samples ago it ideally started (0 <= late < 1)
     */
    void trigger(SfxType type, int samples, float velocity, double late);
    /** @brief Renders @p n stereo samples, replacing @p L and @p R. */
    void process(float* L, float* R, int n);
    /** @brief Voices sounding. */
    int active() const;

private:
    struct Voice {
        SfxType type = SfxType::Riser;
        bool on = false;
        long long pos = 0, length = 1;
        double late = 0.0;
        float velocity = 1.0f;
        Rng rng;
        VaOscillator saw1, saw2;
        Svf bp, lp, formant[3], hpL1, hpL2, hpR1, hpR2;
        double sinePh = 0.0, panPh = 0.0, chordPh[3] = {};
        uint64_t age = 0;
    };
    float voiceSample(Voice& v, float& pan);

    double sr_ = 48000.0;
    Voice voice_[kVoices];
    uint64_t counter_ = 0;
    int keyRoot_ = 6;
    float level_ = 0.5f, noise_ = 0.6f, resonance_ = 0.5f, brightness_ = 0.5f, impactDecay_ = 1.2f, vowel_ = 0.0f, swellDecay_ = 1.5f, width_ = 0.7f;
};

} // namespace phos
