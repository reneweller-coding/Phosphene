/**
 * @file Sfx.h
 * @brief One-shot effects the composer places at points of the form: risers, downlifters, impacts,
 *        sweeps, formant shots, reverse swells and zaps.
 *
 * Everything is synthesised; there are no samples. A note of Part::Sfx names the type by its pitch
 * (SfxBaseNote + type, so the MIDI export shows each type on its own key) and its length in beats;
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
 *    The form places it only where kick and bass are silent anyway (FormSfx.cpp, makeFormSfx).
 *  - **Reverse crash**: a metallic cymbal wash -- high-passed noise ring-modulated by two inharmonic
 *    square waves -- under a rising exponential envelope that ends on the target beat.
 *  - Formant voice, alien chatter, spoken word, voice chop: the Vocal part (Vocal.h).
 *  - Singing bowl, didgeridoo, jaw harp: the Texture part (Texture.h).
 *
 * **20.09.2026, round "wandering-fx": effects that move through the room.** The user's rule of
 * 19.09.2026: a psychedelic effect event should be able to start far to one side, morph across the
 * stereo field, and dissolve into a long reverb tail while kick and bass stay dry and mono. `sfx.wander`
 * (off by default, so older sets render unchanged) replaces, for every voice but the sub drop
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
 *    adds directly into the plain hall's send (not `Engine::m_hallGate`: that hall's self-duck pulls its
 *    return down while a send is loud and its bar-line cut would truncate a tail that is meant to run on
 *    -- both fight a trajectory built to grow loud towards a long tail; docs/rounds/2026-09.md has the reasoning).
 *    The dry buffer's own share of `sfx.hall_send`/`sfx.room_send` shrinks together with it, so the two
 *    paths are complementary rather than double-counted.
 * @author Rene Weller
 */
#pragma once
#include "phos/Dsp.h"
#include "phos/Oscillator.h"
#include "phos/Params.h"
#include "phos/Score.h"

namespace phos {

/** @brief Effect types, in the order of their MIDI notes. Appended only (the notes sit in MIDI files). */
enum class SfxTypeE : int { RISER = 0, DOWNLIFTER, IMPACT, SWEEP, FORMANT_SHOT, REVERSE_SWELL, ZAP,
                           // 19.09.2026, round "fx-psychedelia"
                           SQUELCH, BUBBLE, STUTTER, SUB_DROP, REVERSE_CRASH,
                           FORMANT_VOICE, ALIEN_CHATTER, SPOKEN_WORD, VOICE_CHOP,
                           BOWL, DIDGERIDOO, JAW_HARP,
                           // 23.09.2026, round "SFX": the long, pad-like layer the user asked for ("flaechigere und
                           // laengere Effekte"): layered noise and detuned saws under a slow swell, two to eight bars.
                           ATMOSPHERE,
                           // 27.09.2026: a NASA recording up to 25 s, played whole (FieldPlayer.h, FieldShot).
                           SPACE_SHOT,
                           COUNT };
constexpr int NumSfxTypes = static_cast<int>(SfxTypeE::COUNT);   ///< number of types
constexpr int SfxBaseNote = 48;                                   ///< MIDI note of the riser
extern const char* const SfxTypeNames[NumSfxTypes];             ///< display names

/**
 * @brief One preset of the effect bank (23.09.2026, round "SFX"; Tools/sfx_bank.py).
 *
 * Twelve numbers that steer a type's synthesis (Sfx.cpp, voiceSample): the user heard the effects repeat
 * -- "die kurzen Zips und Zaps wiederholen sich viel zu oft" -- because every type was one fixed sound.
 * The bank holds 2048 presets in eleven families; the composer picks one per event and never the same
 * twice in a track (FormSfx.cpp, makeFormSfx), and the pick rides in the event's lane (SfxEvent::variant).
 * A lane of 0 plays the type as it always did.
 */
struct SfxPreset {
    float m_lengthScale;    ///< multiplier on the written length (one-shots only; risers and swells end on their beat)
    float m_toneMix;        ///< 0..1: tonal layer against noise
    float m_filterLo;       ///< octaves above 200 Hz where the filter starts (or sits low)
    float m_filterHi;       ///< octaves above 200 Hz where it ends (or sits high)
    float m_resonance;      ///< 0..1
    float m_envShape;       ///< exponent or attack share of the envelope, by type
    float m_pitchInterval;  ///< semitones of the tonal layer over the key's root
    float m_detune;         ///< 0..1: spread of the detuned saws
    float m_motionRate;     ///< Hz (or a type's own unit) of the slow modulation
    float m_panSpeed;       ///< 0..1: how fast the event travels
    float m_wet;            ///< 0..1: extra share into the hall (with sfx.wander)
    float m_metal;          ///< 0..1: ring-modulated, metallic partials
};
extern const SfxPreset SfxBank[];                 ///< every preset, family by family (SfxBankTables.cpp)
extern const int SfxBankSize;                     ///< 2048
extern const int SfxBankOffset[NumSfxTypes];     ///< first preset of each type's family in SfxBank
extern const int SfxBankCount[NumSfxTypes];      ///< presets per type (0 for the types without a family)
/**
 * @brief The preset @p index (1-based, as the event's lane carries it) of @p type, or null for 0 / out of range.
 * @param[in] type   the effect type
 * @param[in] index  the preset, 1-based
 * @return the preset, or null
 */
inline const SfxPreset* sfxPreset(SfxTypeE type, int index)
{
    const int t = static_cast<int>(type);
    if (t < 0 || t >= NumSfxTypes || index <= 0 || index > SfxBankCount[t]) return nullptr;
    return &SfxBank[SfxBankOffset[t] + index - 1];
}
static_assert(SfxBaseNote + NumSfxTypes <= 128, "every effect type needs a MIDI note");

/**
 * @brief The effect family each preset choice of the effects page names (sfx::PresetRiser ..), in their order.
 *
 * The eleven types with a family in the bank (SfxBankCount > 0). sfxPresetChoice() is the other direction.
 */
inline constexpr SfxTypeE PresetChoiceType[sfx::NumPresetChoices] = {
    SfxTypeE::RISER, SfxTypeE::DOWNLIFTER, SfxTypeE::IMPACT, SfxTypeE::SWEEP, SfxTypeE::FORMANT_SHOT, SfxTypeE::REVERSE_SWELL,
    SfxTypeE::ZAP, SfxTypeE::SQUELCH, SfxTypeE::BUBBLE, SfxTypeE::REVERSE_CRASH, SfxTypeE::ATMOSPHERE,
};
/**
 * @brief The preset choice (0 .. sfx::NumPresetChoices - 1) of a type, or -1 for a type without a family.
 * @param[in] t  the effect type
 * @return the preset choice, or -1
 */
constexpr int sfxPresetChoice(SfxTypeE t)
{
    for (int i = 0; i < sfx::NumPresetChoices; ++i) if (PresetChoiceType[i] == t) return i;
    return -1;
}
/**
 * @brief How long an audition of a family plays, in beats (Engine::previewSfx): about what the composer
 *        writes for it -- a riser's four bars, a zap's eighth.
 */
inline constexpr float PresetPreviewBeats[sfx::NumPresetChoices] = { 16.0f, 8.0f, 4.0f, 4.0f, 1.0f, 4.0f, 0.5f, 0.5f, 1.0f, 2.0f, 16.0f };

/**
 * @brief Which generator, and so which part and mix strip, plays an effect type.
 *
 * The composer writes every effect as a Part::Sfx note (Melody.cpp, composeSfxBar); the engine and the
 * MIDI export route it by its type, so the shamanic bed and the voices get their own strips and their
 * own MIDI tracks without the composer knowing either exists.
 * @param[in] t  the effect type
 * @return the part that plays it
 */
constexpr PartE sfxTypePart(SfxTypeE t)
{
    return (t >= SfxTypeE::FORMANT_VOICE && t <= SfxTypeE::VOICE_CHOP) ? PartE::VOCAL
         : (t >= SfxTypeE::BOWL && t <= SfxTypeE::JAW_HARP) ? PartE::TEXTURE
         : PartE::SFX;
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
 * that is about the candy. FormSfx.cpp's density floor keeps it.
 * @param[in] t  the effect type
 * @return true when it counts towards the density rule
 */
constexpr bool isDensityEvent(SfxTypeE t) { return sfxTypePart(t) == PartE::SFX && t != SfxTypeE::SPACE_SHOT; }

/**
 * @brief The part a note is played by: its own, or for an effect note the part of its type.
 * @param[in] e  the note
 * @return the part that plays it
 */
inline PartE routedPart(const NoteEvent& e)
{
    if (e.m_part != PartE::SFX && e.m_part != PartE::TEXTURE && e.m_part != PartE::VOCAL) return e.m_part;
    const int type = static_cast<int>(e.m_pitch) - SfxBaseNote;
    return type >= 0 && type < NumSfxTypes ? sfxTypePart(static_cast<SfxTypeE>(type)) : e.m_part;
}

/** @brief The effect generator. */
/**
 * @brief The tonal layer's interval over the key's root in mode @p scale, for a preset that asks for @p iv semitones.
 * @param[in] scale  index into ScaleSteps, or -1 for no mapping
 * @param[in] iv     the preset's interval in semitones
 * @return the interval the effect plays
 */
double sfxToneInterval(int scale, double iv);

/** @brief The effects (SfxType: risers, downlifters, impacts, sweeps, voices, sub drops ...), a few at once (see the file comment). */
class Sfx {
public:
    static constexpr int Voices = 4;   ///< simultaneous effects

    /**
     * @brief Prepares for a sample rate.
     * @param[in] sampleRate  the sample rate
     */
    void prepare(double sampleRate);
    /** @brief Silences every voice. */
    void reset();
    /**
     * @brief Reads the effective parameters (indexed by sfx::) and the key.
     * @param[in] v        the effective parameter values
     * @param[in] keyRoot  the key's root
     */
    void update(const float* v, int keyRoot);
    /**
     * @brief Starts an effect.
     * @param[in] type      the effect
     * @param[in] samples   its length
     * @param[in] velocity  0..1
     * @param[in] late      how many samples ago it ideally started (0 <= late < 1)
     * @param[in] preset    1-based index into the type's family of the bank (SfxPreset), 0 = the type as before; the
     *                      family's sfx.preset_* choice replaces it when that is not Auto
     */
    void trigger(SfxTypeE type, int samples, float velocity, double late, int preset = 0);
    /**
     * @brief Renders @p n stereo samples, replacing @p L and @p R; a sub drop is added to both, centred.
     *        A wandering event's growing reverb-send trajectory (sfx.wander) is folded back in here too
     *        -- this call has no reverb to hand it to -- so it stays audible for direct callers/tests.
     * @param[out] L  the left channel
     * @param[out] R  the right channel
     * @param[in]  n  the block's samples
     */
    void process(float* L, float* R, int n);
    /**
     * @brief Renders @p n samples: everything but the sub drop into @p L and @p R, the sub drop alone
     *        (mono, unfiltered) into @p sub, and a wandering event's reverb-send trajectory (Sfx.h,
     *        sfx.wander) -- energy the dry @p L/@p R lose as it grows towards the event's tail -- into
     *        @p wetL/@p wetR. All five are replaced. @p wetL/@p wetR are silent whenever sfx.wander is
     *        off (every existing caller is unaffected).
     * @param[out] L     the left channel
     * @param[out] R     the right channel
     * @param[out] sub   the sub drop, mono
     * @param[out] wetL  the reverb send's left channel
     * @param[out] wetR  the reverb send's right channel
     * @param[in]  n     the block's samples
     */
    void processSplit(float* L, float* R, float* sub, float* wetL, float* wetR, int n);
    /**
     * @brief Voices sounding.
     * @return the voices sounding
     */
    int active() const;
    /**
     * @brief The scale the tonal layers keep to (index into ScaleSteps), -1 for none (24.09.2026).
     *
     * A bank preset gives its tonal layer an interval over the key's root -- a minor third or a minor seventh among
     * them -- and a long atmosphere held the minor seventh over harmonic minor's leading tone for eight bars. With a
     * scale, an interval the mode does not have takes its neighbour that it does (interval()). The engine sets it;
     * a direct caller that does not keeps the intervals as the bank has them.
     * @param[in] scale  the scale's index, -1 for none
     */
    void setScale(int scale) { m_scale = scale; }

private:
    static constexpr int Bubbles = 8;   ///< bubbles a Bubble event can hold
    /** @brief One sounding effect event. */
    struct Voice {
        SfxTypeE m_type = SfxTypeE::RISER;   ///< which effect
        bool m_on = false;   ///< sounding
        long long m_pos = 0;   ///< samples played
        long long m_length = 1;   ///< the event's length, samples
        double m_late = 0.0;   ///< sub-sample onset
        float m_velocity = 1.0f;   ///< 0..1
        float m_typeGain = 1.0f;   ///< the type's level against sfx.level (Sfx.cpp, TypeGainDb)
        Rng m_rng;   ///< the event's own random stream
        VaOscillator m_saw1;   ///< the tonal layer's first oscillator
        VaOscillator m_saw2;   ///< ... and its second
        Svf m_bp;   ///< the effect's band pass
        Svf m_lp;   ///< the effect's low pass
        Svf m_formant[3];   ///< the effect's three formants
        Svf m_hpL1;   ///< the stereo high pass: left, first stage
        Svf m_hpL2;   ///< ... left, second stage
        Svf m_hpR1;   ///< ... right, first stage
        Svf m_hpR2;   ///< ... right, second stage
        double m_sinePh = 0.0;   ///< the sine's phase
        double m_panPh = 0.0;   ///< the auto-pan's phase
        double m_chordPh[3] = {};   ///< the chord's three phases
        /// Bubble burst: onset (s), start frequency (Hz), decay time constant (s), rise per second, phase.
        double m_bubT[Bubbles] = {};   ///< per bubble of the burst: its onset, s
        double m_bubF[Bubbles] = {};   ///< its start frequency, Hz
        double m_bubTau[Bubbles] = {};   ///< its decay time constant, s
        double m_bubRise[Bubbles] = {};   ///< its rise per second
        double m_bubPh[Bubbles] = {};   ///< its phase
        int m_bubbles = 0;   ///< bubbles in the burst
        uint64_t m_age = 0;   ///< trigger order, for voice stealing
        /// @name The wandering trajectory (20.09.2026, round "wandering-fx"), drawn once at trigger()
        /// from this voice's own seed stream; unused while `wander` is false (the SubDrop type and every
        /// voice while sfx.wander is off).
        /// @{
        bool m_wander = false;
        float m_panFrom = -1.0f, m_panTo = 1.0f;   ///< pan at x = 0 and x = 1 (before the sfx.width scale)
        float m_panCurve = 1.0f;                 ///< pan(x) eases with x^panCurve
        float m_wetExpo = 1.0f;                  ///< the dry/wet crossfade eases with x^wetExpo
        /// @}
        const SfxPreset* m_preset = nullptr;    ///< the bank preset this event plays, or null (23.09.2026)
        double m_lfoPh = 0.0;                    ///< the atmosphere's slow motion
    };
    /**
     * @brief One sample of voice @p v; its pan and its share for the reverb go to @p pan and @p wetFrac.
     * @param[in,out] v        the voice
     * @param[out]    pan      its pan
     * @param[out]    wetFrac  its share for the reverb
     * @return the sample
     */
    float voiceSample(Voice& v, float& pan, float& wetFrac);

    double m_sr = 48000.0;   ///< sample rate
    Voice m_voice[Voices];   ///< the sounding events
    uint64_t m_counter = 0;   ///< triggers so far (the voices' age)
    int m_keyRoot = 6;   ///< the key's pitch class, for the tonal layers
    float m_level = 0.5f;   ///< sfx.level, linear
    float m_noise = 0.6f;   ///< the noise's share
    float m_resonance = 0.5f;   ///< the filters' resonance
    float m_brightness = 0.5f;   ///< the brightness
    float m_impactDecay = 1.2f;   ///< an impact's decay, s
    float m_vowel = 0.0f;   ///< the formants' vowel
    float m_swellDecay = 1.5f;   ///< a swell's decay, s
    float m_width = 0.7f;   ///< the width
    float m_subLevel = 0.5f;   ///< sfx.sub_level, linear
    bool m_wander = false;          ///< sfx.wander
    float m_wanderSend = 0.85f;     ///< sfx.wander_send
    int m_scale = -1;                           ///< setScale()
    /**
     * @brief The preset's tonal interval (or @p fallback without one), kept to the scale.
     * @param[in] P         the preset
     * @param[in] fallback  the interval without one
     * @return the interval in semitones
     */
    double interval(const SfxPreset* P, double fallback) const;
    int m_fixed[sfx::NumPresetChoices] = {};   ///< sfx.preset_*: the bank preset a family always plays, 0 = the event's own
};

} // namespace phos
