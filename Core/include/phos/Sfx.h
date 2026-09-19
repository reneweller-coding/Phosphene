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
 *
 * **19.09.2026, round "fx-psychedelia": the psychedelic layer.** Appended types, so that no MIDI note
 * of an older type moved:
 *  - **Squelch**: a bright saw through a resonant band pass that sweeps up into 3 .. 8 kHz and back
 *    within a fraction of a second -- the liquid "blip" of a 303 caught at full resonance. Its power
 *    sits where the user's pocket rule puts squelch resonance (3 .. 8 kHz).
 *  - **Bubble**: a burst of three to seven bubbles, each a sinusoid that decays exponentially while its
 *    frequency *rises* -- the shape of the physically based bubble model of van den Doel ("Physically
 *    based models for liquid sounds", ACM Transactions on Applied Perception 2(4), 2005), with our own
 *    constants (1.2 .. 4 kHz, 12 .. 40 ms).
 *  - **Stutter**: not a sound of this generator. The engine repeats a slice of the melodic bus for the
 *    event's length (PsyFx.h, Stutter); here the type only reserves its note.
 *  - **Sub drop**: a sine falling from about 110 Hz to 32 Hz. It is the one effect allowed under
 *    140 Hz, so it skips the low cut, leaves on its own mono output (processSplit) and the engine ducks
 *    it under every kick with a depth of its own (sfx.sub_duck): it never sits on a kick transient.
 *    The form places it only where kick and bass are silent anyway (Form.cpp, makeFormSfx).
 *  - **Reverse crash**: a metallic cymbal wash -- high-passed noise ring-modulated by two inharmonic
 *    square waves -- under a rising exponential envelope that ends on the target beat.
 *  - Formant voice, alien chatter, spoken word, voice chop: the Vocal part (Vocal.h).
 *  - Singing bowl, didgeridoo, jaw harp: the Texture part (Texture.h).
 */
#pragma once
#include "phos/Dsp.h"
#include "phos/Oscillator.h"
#include "phos/Score.h"

namespace phos {

/** @brief Effect types, in the order of their MIDI notes. Appended only (the notes sit in MIDI files). */
enum class SfxType : int { Riser = 0, Downlifter, Impact, Sweep, FormantShot, ReverseSwell, Zap,
                           // 19.09.2026, round "fx-psychedelia"
                           Squelch, Bubble, Stutter, SubDrop, ReverseCrash,
                           FormantVoice, AlienChatter, SpokenWord, VoiceChop,
                           Bowl, Didgeridoo, JawHarp,
                           Count };
constexpr int kNumSfxTypes = static_cast<int>(SfxType::Count);   ///< number of types
constexpr int kSfxBaseNote = 48;                                   ///< MIDI note of the riser
extern const char* const kSfxTypeNames[kNumSfxTypes];             ///< display names
static_assert(kSfxBaseNote + kNumSfxTypes <= 128, "every effect type needs a MIDI note");

/**
 * @brief Which generator, and so which part and mix strip, plays an effect type.
 *
 * The composer writes every effect as a Part::Sfx note (Melody.cpp, composeSfxBar); the engine and the
 * MIDI export route it by its type, so the shamanic bed and the voices get their own strips and their
 * own MIDI tracks without the composer knowing either exists.
 */
constexpr Part sfxTypePart(SfxType t)
{
    return (t >= SfxType::FormantVoice && t <= SfxType::VoiceChop) ? Part::Vocal
         : (t >= SfxType::Bowl && t <= SfxType::JawHarp) ? Part::Texture
         : Part::Sfx;
}
/** @brief The part a note is played by: its own, or for an effect note the part of its type. */
inline Part routedPart(const NoteEvent& e)
{
    if (e.part != Part::Sfx && e.part != Part::Texture && e.part != Part::Vocal) return e.part;
    const int type = static_cast<int>(e.pitch) - kSfxBaseNote;
    return type >= 0 && type < kNumSfxTypes ? sfxTypePart(static_cast<SfxType>(type)) : e.part;
}

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
    /** @brief Renders @p n stereo samples, replacing @p L and @p R; a sub drop is added to both, centred. */
    void process(float* L, float* R, int n);
    /**
     * @brief Renders @p n samples: everything but the sub drop into @p L and @p R, the sub drop alone
     *        (mono, unfiltered) into @p sub. All three are replaced.
     */
    void processSplit(float* L, float* R, float* sub, int n);
    /** @brief Voices sounding. */
    int active() const;

private:
    static constexpr int kBubbles = 8;   ///< bubbles a Bubble event can hold
    struct Voice {
        SfxType type = SfxType::Riser;
        bool on = false;
        long long pos = 0, length = 1;
        double late = 0.0;
        float velocity = 1.0f;
        float typeGain = 1.0f;   ///< the type's level against sfx.level (Sfx.cpp, kTypeGainDb)
        Rng rng;
        VaOscillator saw1, saw2;
        Svf bp, lp, formant[3], hpL1, hpL2, hpR1, hpR2;
        double sinePh = 0.0, panPh = 0.0, chordPh[3] = {};
        /// Bubble burst: onset (s), start frequency (Hz), decay time constant (s), rise per second, phase.
        double bubT[kBubbles] = {}, bubF[kBubbles] = {}, bubTau[kBubbles] = {}, bubRise[kBubbles] = {}, bubPh[kBubbles] = {};
        int bubbles = 0;
        uint64_t age = 0;
    };
    float voiceSample(Voice& v, float& pan);

    double sr_ = 48000.0;
    Voice voice_[kVoices];
    uint64_t counter_ = 0;
    int keyRoot_ = 6;
    float level_ = 0.5f, noise_ = 0.6f, resonance_ = 0.5f, brightness_ = 0.5f, impactDecay_ = 1.2f, vowel_ = 0.0f, swellDecay_ = 1.5f, width_ = 0.7f;
    float subLevel_ = 0.5f;
};

} // namespace phos
