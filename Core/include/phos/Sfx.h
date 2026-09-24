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
 *
 * **20.09.2026, round "wandering-fx": effects that move through the room.** The user's rule of
 * 19.09.2026: a psychedelic effect event should be able to start far to one side, morph across the
 * stereo field, and dissolve into a long reverb tail while kick and bass stay dry and mono. `sfx.wander`
 * (off by default, so no render before this round changes) replaces, for every voice but the sub drop
 * (which stays centred under the kick by design), two things at once:
 *  - the oscillating auto-pan (`panPh`/`panRate`) with a directed trajectory: `pan(x) = panFrom +
 *    (panTo - panFrom) * x^panCurve`, `x` the event's own elapsed fraction (0 at onset, 1 at its target
 *    beat) already used by every type's synthesis above, so the sweep is exactly as block-size
 *    independent as the rest of the voice. Direction, "how far back" and the ease of the curve are drawn
 *    once at trigger() from the event's own seed (`Voice::rng`), not fixed to always run left to right.
 *  - the constant `sfx.hall_send` fraction of the (whole) dry signal with a trajectory that crosses from
 *    dry to wet over the same `x`: `wetFrac(x) = smoothstep(x^wetExpo) * sfx.wander_send`, 0 at onset and
 *    exactly `sfx.wander_send` at the target beat, whatever `wetExpo` the seed drew (0^k = 0 and 1^k = 1
 *    for any k > 0). `Sfx::processSplit()` splits each voice's already-panned, already-filtered sample
 *    into `(1 - wetFrac)` for the ordinary dry buffer and `wetFrac` for a second, `wet` buffer Engine.cpp
 *    adds directly into the plain hall's send (not `Engine::hallGate_`: that hall's self-duck pulls its
 *    return down while a send is loud and its bar-line cut would truncate a tail that is meant to run on
 *    -- both fight a trajectory built to grow loud towards a long tail; docs/PLAN.md has the reasoning).
 *    The dry buffer's own share of `sfx.hall_send`/`sfx.room_send` shrinks together with it, so the two
 *    paths are complementary rather than double-counted.
 */
#pragma once
#include "phos/Dsp.h"
#include "phos/Oscillator.h"
#include "phos/Params.h"
#include "phos/Score.h"

namespace phos {

/** @brief Effect types, in the order of their MIDI notes. Appended only (the notes sit in MIDI files). */
enum class SfxType : int { Riser = 0, Downlifter, Impact, Sweep, FormantShot, ReverseSwell, Zap,
                           // 19.09.2026, round "fx-psychedelia"
                           Squelch, Bubble, Stutter, SubDrop, ReverseCrash,
                           FormantVoice, AlienChatter, SpokenWord, VoiceChop,
                           Bowl, Didgeridoo, JawHarp,
                           // 23.09.2026, round "SFX": the long, pad-like layer the user asked for ("flaechigere und
                           // laengere Effekte"): layered noise and detuned saws under a slow swell, two to eight bars.
                           Atmosphere,
                           Count };
constexpr int kNumSfxTypes = static_cast<int>(SfxType::Count);   ///< number of types
constexpr int kSfxBaseNote = 48;                                   ///< MIDI note of the riser
extern const char* const kSfxTypeNames[kNumSfxTypes];             ///< display names

/**
 * @brief One preset of the effect bank (23.09.2026, round "SFX"; Tools/sfx_bank.py).
 *
 * Twelve numbers that steer a type's synthesis (Sfx.cpp, voiceSample): the user heard the effects repeat
 * -- "die kurzen Zips und Zaps wiederholen sich viel zu oft" -- because every type was one fixed sound.
 * The bank holds 2048 presets in eleven families; the composer picks one per event and never the same
 * twice in a track (Form.cpp, makeFormSfx), and the pick rides in the event's lane (SfxEvent::variant).
 * A lane of 0 plays the type as it always did.
 */
struct SfxPreset {
    float lengthScale;    ///< multiplier on the written length (one-shots only; risers and swells end on their beat)
    float toneMix;        ///< 0..1: tonal layer against noise
    float filterLo;       ///< octaves above 200 Hz where the filter starts (or sits low)
    float filterHi;       ///< octaves above 200 Hz where it ends (or sits high)
    float resonance;      ///< 0..1
    float envShape;       ///< exponent or attack share of the envelope, by type
    float pitchInterval;  ///< semitones of the tonal layer over the key's root
    float detune;         ///< 0..1: spread of the detuned saws
    float motionRate;     ///< Hz (or a type's own unit) of the slow modulation
    float panSpeed;       ///< 0..1: how fast the event travels
    float wet;            ///< 0..1: extra share into the hall (with sfx.wander)
    float metal;          ///< 0..1: ring-modulated, metallic partials
};
extern const SfxPreset kSfxBank[];                 ///< every preset, family by family (SfxBankTables.cpp)
extern const int kSfxBankSize;                     ///< 2048
extern const int kSfxBankOffset[kNumSfxTypes];     ///< first preset of each type's family in kSfxBank
extern const int kSfxBankCount[kNumSfxTypes];      ///< presets per type (0 for the types without a family)
/** @brief The preset @p index (1-based, as the event's lane carries it) of @p type, or null for 0 / out of range. */
inline const SfxPreset* sfxPreset(SfxType type, int index)
{
    const int t = static_cast<int>(type);
    if (t < 0 || t >= kNumSfxTypes || index <= 0 || index > kSfxBankCount[t]) return nullptr;
    return &kSfxBank[kSfxBankOffset[t] + index - 1];
}
static_assert(kSfxBaseNote + kNumSfxTypes <= 128, "every effect type needs a MIDI note");

/**
 * @brief The effect family each preset choice of the effects page names (sfx::PresetRiser ..), in their order.
 *
 * The eleven types with a family in the bank (kSfxBankCount > 0). sfxPresetChoice() is the other direction.
 */
inline constexpr SfxType kPresetChoiceType[sfx::kNumPresetChoices] = {
    SfxType::Riser, SfxType::Downlifter, SfxType::Impact, SfxType::Sweep, SfxType::FormantShot, SfxType::ReverseSwell,
    SfxType::Zap, SfxType::Squelch, SfxType::Bubble, SfxType::ReverseCrash, SfxType::Atmosphere,
};
/** @brief The preset choice (0 .. sfx::kNumPresetChoices - 1) of a type, or -1 for a type without a family. */
constexpr int sfxPresetChoice(SfxType t)
{
    for (int i = 0; i < sfx::kNumPresetChoices; ++i) if (kPresetChoiceType[i] == t) return i;
    return -1;
}
/**
 * @brief How long an audition of a family plays, in beats (Engine::previewSfx): about what the composer
 *        writes for it -- a riser's four bars, a zap's eighth.
 */
inline constexpr float kPresetPreviewBeats[sfx::kNumPresetChoices] = { 16.0f, 8.0f, 4.0f, 4.0f, 1.0f, 4.0f, 0.5f, 0.5f, 1.0f, 2.0f, 16.0f };

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
/**
 * @brief Whether an effect type counts towards the user's density rule (20.09.2026, round "dialogue").
 *
 * The rule: "im Groove gibt es nie zwei Takte hintereinander ohne mindestens einen Zap, Glitch oder
 * Swell". The three words name the three families the effect strip has -- the short bright one-shots
 * (zap, squelch, bubble), the glitch (the stutter of the melodic bus) and everything that swells in or
 * out (riser, sweep, reverse swell, reverse crash, downlifter, impact, sub drop) -- so the rule reads
 * exactly as "an event of the effects strip". The voices and the shamanic bed do not count: they are
 * another layer with another job, and a groove carried by chant alone would otherwise satisfy a rule
 * that is about the candy. Form.cpp's density floor keeps it.
 */
constexpr bool isDensityEvent(SfxType t) { return sfxTypePart(t) == Part::Sfx; }

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
     * @param preset   1-based index into the type's family of the bank (SfxPreset), 0 = the type as before;
     *                 the family's sfx.preset_* choice replaces it when that is not Auto
     */
    void trigger(SfxType type, int samples, float velocity, double late, int preset = 0);
    /**
     * @brief Renders @p n stereo samples, replacing @p L and @p R; a sub drop is added to both, centred.
     *        A wandering event's growing reverb-send trajectory (sfx.wander) is folded back in here too
     *        -- this call has no reverb to hand it to -- so it stays audible for direct callers/tests.
     */
    void process(float* L, float* R, int n);
    /**
     * @brief Renders @p n samples: everything but the sub drop into @p L and @p R, the sub drop alone
     *        (mono, unfiltered) into @p sub, and a wandering event's reverb-send trajectory (Sfx.h,
     *        sfx.wander) -- energy the dry @p L/@p R lose as it grows towards the event's tail -- into
     *        @p wetL/@p wetR. All five are replaced. @p wetL/@p wetR are silent whenever sfx.wander is
     *        off (every existing caller is unaffected).
     */
    void processSplit(float* L, float* R, float* sub, float* wetL, float* wetR, int n);
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
        /// @name The wandering trajectory (20.09.2026, round "wandering-fx"), drawn once at trigger()
        /// from this voice's own seed stream; unused while `wander` is false (the SubDrop type and every
        /// voice while sfx.wander is off).
        /// @{
        bool wander = false;
        float panFrom = -1.0f, panTo = 1.0f;   ///< pan at x = 0 and x = 1 (before the sfx.width scale)
        float panCurve = 1.0f;                 ///< pan(x) eases with x^panCurve
        float wetExpo = 1.0f;                  ///< the dry/wet crossfade eases with x^wetExpo
        /// @}
        const SfxPreset* preset = nullptr;    ///< the bank preset this event plays, or null (23.09.2026)
        double lfoPh = 0.0;                    ///< the atmosphere's slow motion
    };
    float voiceSample(Voice& v, float& pan, float& wetFrac);

    double sr_ = 48000.0;
    Voice voice_[kVoices];
    uint64_t counter_ = 0;
    int keyRoot_ = 6;
    float level_ = 0.5f, noise_ = 0.6f, resonance_ = 0.5f, brightness_ = 0.5f, impactDecay_ = 1.2f, vowel_ = 0.0f, swellDecay_ = 1.5f, width_ = 0.7f;
    float subLevel_ = 0.5f;
    bool wander_ = false;          ///< sfx.wander
    float wanderSend_ = 0.85f;     ///< sfx.wander_send
    int fixed_[sfx::kNumPresetChoices] = {};   ///< sfx.preset_*: the bank preset a family always plays, 0 = the event's own
};

} // namespace phos
