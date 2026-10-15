/**
 * @file Engine.h
 * @brief The engine: plays score events sample-accurately through the generators, mixer and master.
 *
 * **Time.** Musical time advances on a fixed grid of 32-sample chunks counted from the start. At the
 * start of every chunk the tempo is read (from the tempo map, or from compose.bpm when no map is set),
 * ramps advance and the effective parameters are applied; inside the chunk the beat of sample i is
 * chunkBeat + i * beatsPerSample. Because the grid is absolute, the output does not depend on how a
 * host cuts the stream into blocks (checked bit for bit in the self test).
 *
 * **Events.** Notes and control events arrive in two lock-free rings. An event fires at the first
 * sample whose beat has reached it, and the generators are told how far past the ideal instant that
 * sample lies (0 <= late < 1 sample), so onsets are sub-sample exact. Control events fire before
 * notes at the same sample, so a sound change on a downbeat applies to the kick on that downbeat.
 *
 * **Effective parameters.** Every generator plays knob + offset (continuous parameters, in the
 * normalised domain) or the override (discrete parameters); see ControlEvent. Then two constraints
 * are applied that depend on tempo and pattern: the kick's tail limit at the first bass slot, and
 * the kick lock between the kick's phase and the bass's start phase.
 *
 * **Signal flow.** Every generator renders into its own buffer. Its channel strip applies the level, the
 * kick's sidechain duck (event-driven, Ducker.h), the trance gate for lead, arp and pad (TranceGate.h),
 * and sends to a short room and a long hall (Reverb.h), whose returns are ducked as well. A voice whose
 * own `hall_gate` is on (poly.hall_gate, acid.hall_gate; 20.09.2026, round "reverb") sends into a second,
 * dedicated hall instead: a big space that ducks by its own send's envelope and is cut hard on the
 * absolute bar line (Reverb::processDucked, Reverb::barGate) -- off by default, so it changes nothing
 * for a voice that never turns it on. The master sums everything, applies the gain (knob, the track's
 * level match and the composer's loudness offset),
 * the bus compressor, mono bass (the side signal high-passed), the soft clipper, the band limit that
 * gives the programme an upper end (Dsp.h, BandLimit), the lookahead true-peak limiter and a final
 * safety clip at the ceiling, and meters the result to BS.1770 (Dynamics.h, Loudness.h). With the
 * limiter on, the output is delayed by latencySamples().
 *
 * **Why the band limit sits between the clipper and the limiter.** After the clipper, because the
 * clipper is the last stage that makes new harmonics; before the limiter, because a true-peak ceiling
 * is a claim about the analogue waveform and a true-peak estimate is a band-limited reconstruction --
 * a limiter handed a programme that runs past its estimator's band cannot hold the ceiling it reports.
 * Measured on eight minutes of seed 7 before this was there: the meter read -0.98 dBTP, the exact peak
 * was -0.075, and the same render cut at the estimator's 0.45 fs read -0.182 exact against -0.218
 * estimated -- the whole error lived above the band.
 *
 * **The psychedelic layer (19.09.2026, round "fx-psychedelia").** Two more strips after the SFX strip:
 * the shamanic bed (Texture.h) and the voices (Vocal.h). Effect notes are routed by their type
 * (Sfx.h, sfxTypePart), so the composer still writes every effect as a Part::Sfx note. Three more
 * things happen around the strips:
 *  - the SFX strip passes its insert chain (PsyFx.h: flanger, phaser, frequency shifter) before its
 *    gain, and the texture and vocal strips *send* into a second chain whose return joins the mix --
 *    both move per section (control events on psyfx.*, Form.cpp) and per event (a riser drags the
 *    shifter up, a downlifter down, a sweep opens the flanger, a phrase detunes the send);
 *  - the vocal strip throws its last word into a tempo delay (Vocal.h, the throw weight);
 *  - a Stutter event repeats a slice of the melodic bus (acid, lead, arp) in place of the live signal;
 *    the sends keep the live signal, so the reverb tails run on underneath the glitch;
 *  - the sub drop leaves the SFX generator on its own mono output and joins kick and bass in the
 *    centre, under a ducker the kick triggers (sfx.sub_duck), so it never sits on a kick transient.
 *
 * **Wandering effects (20.09.2026, round "wandering-fx").** With `sfx.wander` on, an effect voice's pan
 * sweeps across the field instead of oscillating and its content crosses from dry to the hall's send over
 * its own length (Sfx.h); the growing wet share (`m_sfxWetL`/`m_sfxWetR`) is added straight into the plain
 * hall's send in renderSegment(), alongside the (correspondingly shrinking) dry mix's own constant
 * sfx.hall_send fraction -- not into Engine::m_hallGate, whose self-duck and bar-line cut are built for a
 * continuous voice, not a one-shot event's own growing trajectory into a tail that is meant to run past
 * the bar. Off by default, so no existing render changes.
 *
 * **Threads.** process() runs on the audio thread and never allocates. The push functions may be
 * called from one other thread. Parameters may be written from any thread.
 * @author Rene Weller
 */
#pragma once
#include "phos/Acid.h"
#include "phos/Bass.h"
#include "phos/Dynamics.h"
#include "phos/FieldPlayer.h"
#include "phos/Loudness.h"
#include "phos/Reverb.h"
#include "phos/Sfx.h"
#include "phos/TranceGate.h"
#include "phos/Clock.h"
#include "phos/Kick.h"
#include "phos/Params.h"
#include "phos/Perc.h"
#include "phos/Poly.h"
#include "phos/PsyFx.h"
#include "phos/Quality.h"
#include "phos/Score.h"
#include "phos/TempoDelay.h"
#include "phos/Texture.h"
#include "phos/Vocal.h"
#include <limits>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

namespace phos
{

/**
 * @name Stems (23.09.2026, round "Stems")
 * One stem per part (Part order: kick, bass, percussion, acid, the six polyphonic voices, effects, bed,
 * voices) and one for the returns -- room, hall, gated hall, the modulation send and the voices' delay
 * throw. Each part's stem is what that part puts into the mix: after its strip gain, its trance gate and
 * its duck, times the master gain, *before* the master (bus compressor, mono bass, clipper, band limit,
 * limiter). The sub drop belongs to the effects stem. A stutter is not in the stems: they carry the live
 * melodic bus the stutter replaces. So the stems sum to the mix as it enters the master, and a stem is what
 * a mixing engineer would get from a bounce of that channel.
 * @{ */
constexpr int            NumStems = NumParts + 1;   ///< the parts and the returns
extern const char *const StemNames[NumStems];       ///< "Kick" ... "Vocal", "Returns"
/**
 * @brief Where the engine writes the stems while it renders (Engine::setStemTap).
 *
 * Each pointer is a buffer of at least as many samples as the largest process() call; the engine writes
 * sample i of a call at index i, exactly as it writes the mix.
 */
struct StemTap
{
    float *m_L[NumStems] = {};   ///< left channel of each stem
    float *m_R[NumStems] = {};   ///< right channel of each stem
};
/** @} */

/** @brief The Phosphene engine. */
class Engine
{
public:
    static constexpr int Chunk = 32;   ///< samples per time/parameter chunk

    /**
     * @brief Writes the stems into @p tap on every following process() call; nullptr stops it (StemTap).
     *
     * Costs a store per part and sample while it is set and nothing when it is not; the mix is the same
     * bit for bit either way (the self test measures that). Not for the audio thread of a host: set it
     * before rendering, from the thread that renders.
     * @param[in,out] tap  the tap, or null to stop
     */
    void setStemTap( StemTap *tap ) { m_tap = tap; }
    /**
     * @brief Live play (01.10.2026; the plugin's engine): the keyboard's Replace and the composer switch (mix.composer)
     *        act. Off (phos_render, the exports, the probes, the default): the set plays as it was composed, whatever its
     *        keyboard knobs say.
     * @param[in] on  true for live play
     */
    void setLive( bool on ) { m_live = on; }

    /**
     * @name A MIDI keyboard on one voice (23.09.2026, round "Keyboard")
     * mix.keyboard_part says which voice the keyboard plays -- the acid, one of the six polyphonic voices, the bass,
     * the kit (C1 the kick, C#1 .. C2 the twelve lanes; 01.10.2026), or "By channel" (channel 1 the acid, 2 the lead ...
     * 7 the drone, 8 the bass, 10 the kit) -- with that voice's sound as its page has it. mix.composer off leaves every
     * generated note out (01.10.2026). Replace and the composer switch act in live play only (setLive).
     * mix.keyboard_mode Replace silences the voice's generated notes (in "By channel" from a voice's first played
     * note on), Layer plays over them. A played note starts with a gate that never runs out; its key's release
     * ends it. Called on the rendering thread between process() calls -- the plugin splits a block at every MIDI
     * event, so a note starts on its sample.
     * @{ */
    /**
     * @brief Starts a note played live.
     * @param[in] pitch     MIDI note;
     * @param[in] velocity  1..127;
     * @param[in] channel   0..15;
     * @param[in] part      the voice it names (mix.keyboard_part's values; the keyboard split, 02.10.2026), -1 for the
     *                      keyboard's own
     */
    void liveNoteOn( int pitch, int velocity, int channel, int part = -1 );
    /**
     * @brief Releases the note @p pitch on whichever voice its note-on went to.
     * @param[in] pitch    the MIDI note
     * @param[in] channel  the MIDI channel
     */
    void liveNoteOff( int pitch, int channel );
    /** @brief Releases every played note (transport stop, a changed keyboard part). */
    void liveAllOff();
    /**
     * @brief Whether the composer's notes of @p part are replaced by the keyboard (the plugin's MIDI out skips them
     *        too).
     * @param[in] part  the part
     * @return true when the keyboard replaces its notes
     */
    bool generatedSilenced( PartE part ) const;
    /**
     * @brief The family jam (02.10.2026, the plugin's Jam.h), live only: the melodic voices' notes (the acid, the bass,
     *        the lead and the others) @p transpose semitones from where they were written, and with @p rhythmOut the
     *        kick and the bass out for the leader's break.
     * @param[in] transpose  the transposition in semitones
     * @param[in] rhythmOut  true for the kick and the bass out
     */
    void setJam( int transpose, bool rhythmOut )
    {
        m_jamTranspose = transpose;
        m_jamRhythmOut = rhythmOut;
    }
    /**
     * @brief Whether the family jam leaves @p part's notes out now (the kick and the bass in the leader's break).
     * @param[in] part  the part
     * @return true when its notes are left out
     */
    bool jamSilenced( PartE part ) const { return m_jamRhythmOut && ( part == PartE::KICK || part == PartE::BASS ); }
    /**
     * @brief @p pitch as the family jam plays @p part's notes now (moved by its transposition, or as it is).
     * @param[in] part   the part
     * @param[in] pitch  the MIDI note
     * @return the note played
     */
    int jamPitch( PartE part, int pitch ) const;
    /** @} */

    /**
     * @brief Plays bank preset @p preset of the family behind preset choice @p choice now, for the effects page's
     *        audition (24.09.2026; Sfx.h, PresetChoiceType). Rendering thread, between process() calls. The
     *        family's sfx.preset_* choice still wins inside Sfx::trigger, so the page passes the preset it shows.
     * @param[in] choice  the preset choice
     * @param[in] preset  the bank preset
     */
    void previewSfx( int choice, int preset );

    /**
     * @name Channel meters (24.09.2026, the mixer's strips)
     * What each part puts into the mix -- the stem tap's signal (StemTap): after its strip gain, trance gate and
     * duck, times the master gain, before the master -- as a peak and a sum of squares (the mean of the two
     * channels) per part since the last takeMeters(). Off unless switched on, and reading only: the mix is the
     * same to the bit either way.
     * @{ */
    /**
     * @brief Switches the gathering on or off (rendering thread).
     * @param[in] on  true to gather the meters
     */
    void setMetering( bool on ) { m_metering = on; }
    /**
     * @brief Hands over what was gathered since the last call and starts again (rendering thread).
     * @param[in,out] peak   NumParts peaks (absolute sample values)
     * @param[in,out] sumSq  NumParts sums of squares
     * @return the number of samples they cover
     */
    int takeMeters( float *peak, double *sumSq );
    /** @} */

    Engine();
    /** @brief Lets go of the field library's loader thread (FieldLibrary.h). */
    ~Engine();
    Engine( const Engine & ) = delete;              ///< one engine holds one share of the loader thread
    Engine &operator=( const Engine & ) = delete;   ///< likewise

    /**
     * @brief Prepares for playback.
     * @param[in] sampleRate    output rate
     * @param[in] maxBlockSize  largest block the host will use (any size works; larger ones are split)
     * @param[in] quality       what the engine may spend per part (Quality.h); the default is the desktop level, which
     *                          is what the engine did before quality levels existed
     */
    void prepare( double sampleRate, int maxBlockSize, const Quality &quality = Quality::desktop() );

    /**
     * @brief Expands the wavetables the parameters currently name, so the voices really sound them.
     *
     * Since 22.09.2026 the library is only indexed at load and a table is expanded when somebody
     * asks for it (WaveTableFile.h, ensureWaveTables) -- 464 tables cannot all live in memory. The
     * composer asks for a track's tables while it plans it, which covers everything the generator
     * itself decides. This covers the other way a table gets chosen: a human turning the Table knob,
     * a `--set pad.table=...`, a loaded `.phosset`. Without it those play the built-in fallback and
     * nothing says why.
     *
     * Cheap when nothing changed (six atomic loads, no lock), so the plugin can call it off its
     * 30 Hz timer. **Never from the audio thread:** it decodes and allocates. `prepare()` calls it
     * once for whatever the parameters hold at that point.
     */
    void ensureVoiceTables();
    /**
     * @brief The level prepare() was called with.
     * @return the quality
     */
    const Quality &quality() const { return m_quality; }
    /** @brief Back to beat 0; clears events, offsets, overrides and all sound. */
    void reset();

    /**
     * @brief The parameters (the knobs).
     * @return the parameters
     */
    ParamStore &params() { return m_params; }
    /**
     * @brief The parameters, read-only.
     * @return the parameters
     */
    const ParamStore &params() const { return m_params; }

    /**
     * @brief Uses a tempo map instead of compose.bpm (call while stopped).
     * @param[in] map  the tempo map
     */
    void setTempoMap( const TempoMap &map )
    {
        m_tempo = map;
        m_useTempoMap = true;
    }
    /** @brief Goes back to compose.bpm. */
    void clearTempoMap() { m_useTempoMap = false; }

    /**
     * @brief Queues a note (producer thread); false if the ring is full.
     * @param[in] e  the note
     * @return false if the ring is full
     */
    bool pushEvent( const NoteEvent &e ) { return m_notes.push( e ); }
    /**
     * @brief Queues a control event (producer thread); false if the ring is full.
     * @param[in] e  the control event
     * @return false if the ring is full
     */
    bool pushControl( const ControlEvent &e ) { return m_controls.push( e ); }
    /**
     * @brief Queues a control event that takes effect at the next chunk, ahead of everything queued (producer
     *        thread, the same one that pushes the others); false if its ring is full.
     *
     * The control ring is first in, first out, and the composer keeps it filled eight bars ahead, so an event
     * pushed "now" into it waits behind every event of those eight bars -- some thirteen seconds at 145 BPM
     * (23.09.2026: the measured line levels and Auto Gain's offset arrived that late, and the host test's ramp
     * was still short of its target when it looked). This ring is read at every chunk start before anything
     * else; an event whose beat has passed starts there, so a ramp runs its full length from now.
     * @param[in] e  the control event
     * @return false if the ring is full
     */
    bool pushImmediate( const ControlEvent &e ) { return m_immediate.push( e ); }
    /**
     * @brief Beat position of the next sample to be rendered (any thread).
     * @return the beat position
     */
    double beatPosition() const { return m_beatNow.load( std::memory_order_relaxed ); }
    /**
     * @brief Samples rendered since reset.
     * @return the current position in samples
     */
    uint64_t samplePosition() const { return m_samples; }

    /**
     * @brief Renders @p n stereo samples.
     * @param[out] L  receives the left channel (overwritten)
     * @param[out] R  receives the right channel (overwritten)
     * @param[in]  n  how many samples
     */
    void process( float *L, float *R, int n );

    /**
     * @brief The kick, for tests and displays.
     * @return deck A's kick
     */
    const Kick &kick() const { return m_kick; }
    /**
     * @brief The bass, for tests and displays.
     * @return the bass
     */
    const Bass &bass() const { return m_bass; }
    /**
     * @brief The percussion kit, for tests and displays.
     * @return the percussion kit
     */
    const PercKit &percKit() const { return m_perc; }
    /**
     * @brief The acid voice, for tests and displays.
     * @return the acid voice
     */
    const Acid &acid() const { return m_acid; }
    /**
     * @brief A polyphonic engine (lead or arp), for tests and displays.
     * @param[in] i  the instance
     * @return the engine
     */
    const Poly &poly( PolyInstanceE i ) const { return m_poly[static_cast<int>( i )]; }
    /**
     * @brief The effect generator, for tests and displays.
     * @return the effect generator
     */
    const Sfx &sfx() const { return m_sfx; }
    /**
     * @brief The shamanic bed, for tests and displays.
     * @return the shamanic bed
     */
    const Texture &texture() const { return m_texture; }
    /**
     * @brief The voices, for tests and displays.
     * @return the voices
     */
    const Vocal &vocal() const { return m_vocal; }
    /**
     * @brief True while a stutter replaces the melodic bus (tests).
     * @return true while a stutter plays
     */
    bool stuttering() const { return m_stutter.active(); }
    /**
     * @brief The SFX strip's modulation chain (tests: the event motion in force).
     * @return the chain
     */
    const PsyFxChain &sfxChain() const { return m_sfxFx; }
    /**
     * @brief Samples by which the output lags the events (the limiter's lookahead; 0 when it is off).
     * @return the latency in samples
     */
    int latencySamples() const { return m_limiterOn ? m_limiter.latency() : 0; }
    /**
     * @brief The meter on the master output (audio thread; read it when not processing).
     * @return the reading
     */
    LoudnessReading meter() const { return m_meter.read(); }
    /** @brief Restarts the meter. */
    void resetMeter() { m_meter.reset(); }
    /**
     * @brief Gain reduction of the bus compressor and the limiter at the end of the last block, dB.
     * @return the compressor's reduction in dB
     */
    float compReduction() const { return m_comp.reduction(); }
    /**
     * @copybrief compReduction
     * @return the limiter's reduction in dB
     */
    float limiterReduction() const { return m_limiter.reduction(); }
    /**
     * @brief Effective value of a parameter as last applied (audio thread view).
     * @param[in] id  the parameter
     * @return its effective value
     */
    float effective( int id ) const;
    /**
     * @brief Every parameter's effective value, with the modulation of the voice a player hears on the knobs it moves
     *        (26.09.2026, the live ring): cutoff, resonance, filter mode, pulse width, FM index, table position, level
     *        and pan of the six voices, the bass and the acid, the way each synth applies its sums. Rendering thread.
     * @param[in,out] out  params().count() values
     */
    void playedValues( float *out ) const;
    /**
     * @brief The key's pitch class the engine plays in (0 = C), as last applied (audio thread view; the scopes).
     * @return the key's pitch class
     */
    int keyRoot() const { return m_keyRoot; }
    /**
     * @brief The scale the engine plays in (index into ScaleSteps / ScaleNames), as last applied.
     * @return the scale's index
     */
    int scale() const { return m_scale; }

private:
    int  m_jamTranspose = 0;       ///< the family jam's transposition of the melodic voices (setJam)
    bool m_jamRhythmOut = false;   ///< the family jam: kick and bass out for the leader's break (setJam)
    /** @brief Moves every running offset ramp to the current chunk's beat. */
    void advanceRamps();
    /** @brief Recomputes the effective values that moved and hands them to every generator (Engine.h, m_raw). */
    void applyParams();
    /**
     * @brief Renders @p count samples at @p offset: every generator, the strips, the sends and the master.
     * @param[out] L       the left channel
     * @param[out] R       the right channel
     * @param[in]  offset  the first sample in the block
     * @param[in]  count   the samples
     */
    void renderSegment( float *L, float *R, int offset, int count );
    /**
     * @brief Starts note @p e on its generator, @p late samples after its ideal onset.
     * @param[in] e     the note
     * @param[in] late  samples after its ideal onset
     */
    void dispatch( const NoteEvent &e, double late );
    /**
     * @brief Applies control event @p e: a parameter's offset ramp or override, or the bass slot of the beat.
     * @param[in] e  the control event
     */
    void dispatchControl( const ControlEvent &e );
    /**
     * @brief Seconds from the kick to the bass's first slot of the beat (the phase lock's target).
     * @return the seconds
     */
    double firstSlotSeconds() const;

    ParamStore m_params;         ///< the knobs (the host's and the command line's)
    double     m_sr = 48000.0;   ///< sample rate
    Quality    m_quality;        ///< what each part may spend (Quality.h)

    TempoMap m_tempo;                 ///< the tempo ramps, when a render has them
    bool     m_useTempoMap = false;   ///< true: beats per sample follow m_tempo, not compose.bpm

    EventRing<NoteEvent>    m_notes;                  ///< notes from the composer, in beat order
    EventRing<ControlEvent> m_controls;               ///< control events from the composer, in beat order
    EventRing<ControlEvent> m_immediate;              ///< pushImmediate()
    uint64_t                m_samples = 0;            ///< samples rendered since the last reset
    double                  m_chunkBeat = 0.0;        ///< the beat at the start of the current 32-sample chunk
    double                  m_beatsPerSample = 0.0;   ///< the tempo of the current chunk
    int                     m_chunkPos = 0;           ///< samples of the current chunk already rendered
    std::atomic<double>     m_beatNow{ 0.0 };         ///< the beat reached, for other threads (beatPosition)

    /** @brief State of a parameter's variation. */
    struct Variation
    {
        float  m_offset = 0.0f;      ///< current normalised offset
        float  m_from = 0.0f;        ///< the ramp's start
        float  m_to = 0.0f;          ///< the ramp's end
        double m_start = 0.0;        ///< where the ramp begins, beats
        double m_length = 0.0;       ///< how long it takes, beats; 0: not ramping
        float  m_override = -1.0f;   ///< discrete override, < 0 = none
        float  m_base = std::numeric_limits<
            float>::quiet_NaN();   ///< ControlEvent::Kind::Base: the value in place of the knob, NaN = none
    };
    std::unique_ptr<Variation[]> m_var;   ///< every parameter's offset and override
    std::vector<float>           m_eff;   ///< effective values, indexed by global id
    /**
     * @name What applyParams() can skip (23.09.2026, round "Planung + applyParams")
     * VTune put applyParams at 6 % of a render: every 32-sample chunk and every control event recomputed all ~3000
     * effective values, each continuous one with an offset through fromNormalised(toNormalised()) -- powf and log
     * for every logarithmic knob -- and a track start dispatches hundreds of events at one instant, each of them a
     * full pass. Now a value is recomputed only when its knob, its offset or override, or its own-sound switch
     * moved (m_raw keeps the result before Kick::constrain, which edits m_eff in place), and events at one sample
     * are applied once, before anything reads them. The output is the same to the bit (checked on three sets).
     * @{ */
    std::vector<float>   m_raw;                     ///< effective values before the kick's constraints, per id
    std::vector<float>   m_rawKnob;                 ///< the knob value m_raw was computed from
    std::vector<uint8_t> m_rawDirty;                ///< 1: the offset or override moved since m_raw was computed
    bool                 m_ownSeen[9] = {};         ///< the own-sound switches as the last pass saw them
    bool                 m_rawValid = false;        ///< false: recompute everything (construction, reset)
    bool                 m_paramsPending = false;   ///< control events dispatched, applyParams() not yet run for them
    double               m_polyBpm[PolyInstances] = {};   ///< the tempo each voice's last Poly::update() was given
    /** @brief Runs applyParams() if control events are waiting for it (before a note, a render or the next chunk). */
    void flushParams()
    {
        if ( m_paramsPending ) applyParams();
    }
    /** @} */

    Kick               m_kick;               ///< the kick
    Bass               m_bass;               ///< the bass
    PercKit            m_perc;               ///< the twelve percussion lanes
    int                m_keyRoot = 6;        ///< the key's pitch class (0 = C)
    int                m_scale = 1;          ///< the scale (index into ScaleSteps)
    bool               m_percMute = false;   ///< mix.perc_mute
    float              m_percGain = 1.0f;    ///< the percussion strip's gain (mute and level)
    std::vector<float> m_percL;              ///< the percussion's output of a segment, left
    std::vector<float> m_percR;              ///< ... right
    int                m_pattern = 0;        ///< compose.bass_pattern, for the bass's first slot (firstSlotSeconds)
    /**
     * @brief The first sounding bass slot of the current beat in beats, or <= 0 for "derive it".
     *
     * Set by a `ControlEvent::Kind::BassSlot` and read by firstSlotSeconds(). While it is <= 0 --
     * which it is on every bar the composer plays from a pattern family, because such a bar sends a
     * clearing event of -1 -- the engine derives the slot from `firstBassSlot(m_pattern)` exactly as
     * it did before 16.09.2026, so a Pattern render is bit-identical to every earlier one. That
     * clearing event is what lets `compose.bass_rhythm` be switched back *during* playback: without
     * it the engine could not tell "nothing sent yet" from "nothing sent any more".
     */
    double   m_slotBeats = -1.0;
    int      m_lockMode = 2;               ///< bass.kick_lock (KickLock): who follows whom in the phase lock
    double   m_bassPhase = 0.0;            ///< fundamental phase for the next bass note
    bool     m_kickMute = false;           ///< mix.kick_mute
    bool     m_bassMute = false;           ///< mix.bass_mute
    float    m_masterGain = 1.0f;          ///< master.level, linear
    float    m_ceiling = 1.0f;             ///< master.gain (with Auto Gain's offset) and master.ceiling, linear
    StemTap *m_tap = nullptr;              ///< where the stems go (setStemTap), null = nowhere
    bool     m_metering = false;           ///< setMetering()
    float    m_meterPeak[NumParts] = {};   ///< takeMeters(): peak per part
    double   m_meterSum[NumParts] = {};    ///< takeMeters(): sum of squares per part
    int      m_meterCount = 0;             ///< takeMeters(): samples gathered
    /**
     * @brief Adds one stereo sample of part @p part to the meters.
     * @param[in] part  the part
     * @param[in] l     the left sample
     * @param[in] r     the right sample
     */
    void meterAdd( int part, float l, float r )
    {
        const float a = std::max( std::fabs( l ), std::fabs( r ) );
        if ( a > m_meterPeak[part] ) m_meterPeak[part] = a;
        m_meterSum[part] += 0.5 * ( static_cast<double>( l ) * l + static_cast<double>( r ) * r );
    }
    std::vector<int8_t> m_ownOf;   ///< per parameter: which own-sound switch governs it (0 kick .. 8 drone), -1 none
    int      m_liveTarget[128];    ///< per pitch: the voice a played note went to (0 acid, 1.. poly, 7 bass), -1 none
    bool     m_live = false;       ///< setLive: the keyboard's Replace and the composer switch act
    unsigned m_livePlayed = 0;     ///< voices the keyboard has played ("By channel" replaces from the first note)
    /**
     * @brief The voice the keyboard plays on @p channel (0 acid, 1 .. 6 poly, 7 bass, 8 kit), -1 none.
     * @param[in] channel  the MIDI channel
     * @return the target a key there plays (perform::keys), Off for none
     */
    int keyboardTarget( int channel ) const;
    bool     m_clip = true;   ///< master.clip: the safety clip after the limiter
    TanhAdaa m_clipL;         ///< the safety clip, left (antiderivative anti-aliased)
    TanhAdaa m_clipR;         ///< ... right

    std::vector<float> m_kickBuf;   ///< the kick's output of a segment
    std::vector<float> m_bassBuf;   ///< the bass's output of a segment

    Acid               m_acid;                   ///< the acid line
    Poly               m_poly[PolyInstances];    ///< the six polyphonic voices, PolyInstance order
    Sfx                m_sfx;                    ///< the effect generator
    std::vector<float> m_acidL;                  ///< the acid's output of a segment, left
    std::vector<float> m_acidR;                  ///< ... right
    std::vector<float> m_polyL[PolyInstances];   ///< each polyphonic voice's output of a segment, left
    std::vector<float> m_polyR[PolyInstances];   ///< ... right
    std::vector<float> m_sfxL;                   ///< the effects' output of a segment, left
    std::vector<float> m_sfxR;                   ///< ... right
    /// The wandering trajectory's reverb-send share (20.09.2026, round "wandering-fx"; Sfx.h,
    /// sfx.wander): energy the dry m_sfxL/m_sfxR above lose as an event nears its tail, added straight into
    /// the plain hall's send in renderSegment() -- silent whenever sfx.wander is off.
    std::vector<float> m_sfxWetL, m_sfxWetR;   ///< ... right

    Texture            m_texture;    ///< the shamanic bed
    Vocal              m_vocal;      ///< the voices
    std::vector<float> m_texL;       ///< the bed's output of a segment, left
    std::vector<float> m_texR;       ///< ... right
    std::vector<float> m_vocL;       ///< the voices' output of a segment, left
    std::vector<float> m_vocR;       ///< ... right
    std::vector<float> m_vocThrow;   ///< the voices' send into the delay throw
    std::vector<float> m_subBuf;     ///< the effects' sub layer (the sub drop), mono
    std::vector<float> m_throwIn;    ///< the delay throw's input
    std::vector<float> m_sendL;      ///< the send into the mixer's effects, left
    std::vector<float> m_sendR;      ///< the send into the mixer's effects, right
    std::vector<float> m_bedSendL;   ///< the bed's share of the modulation send, left
    std::vector<float> m_bedSendR;   ///< ... right
    /** @name The Field track (27.09.2026; FieldPlayer.h): the sampler on its own strip, and the NASA shots on the SFX strip's
     *  @{ */
    FieldPlayer        m_field;              ///< the field recordings' sampler
    FieldShot          m_shot;               ///< the NASA shots (SfxType::SpaceShot)
    std::vector<float> m_fieldL, m_fieldR;   ///< the sampler's output of a segment
    /** @} */

    /**
     * @brief Channel strips of the parts after kick and bass, in this order. The six polyphonic strips
     *        run from StripLead in the order of PolyInstance (19.09.2026), so StripLead + k is instance k.
     *        StripAcid .. StripStab is the melodic bus a stutter repeats: acid, the two leads, arp and stab;
     *        pad and drone keep holding underneath the glitch.
     */
    enum StripE : int
    {
        STRIP_PERC = 0,
        STRIP_ACID,
        STRIP_LEAD,
        STRIP_COUNTER,
        STRIP_ARP,
        STRIP_STAB,
        STRIP_PAD,
        STRIP_DRONE,
        STRIP_SFX,
        STRIP_TEXTURE,
        STRIP_VOCAL,
        STRIP_FIELD,
        STRIP_COUNT
    };
    static_assert( STRIP_DRONE - STRIP_LEAD + 1 == PolyInstances &&
                       STRIP_STAB - STRIP_LEAD == static_cast<int>( PolyInstanceE::STAB ),
                   "the polyphonic strips must follow PolyInstance" );
    float m_stripGain[STRIP_COUNT] = {};    ///< each strip's gain (mute and level)
    float m_stripRoom[STRIP_COUNT] = {};    ///< each strip's room send
    float m_stripHall[STRIP_COUNT] = {};    ///< each strip's hall send
    float m_stripPlate[STRIP_COUNT] = {};   ///< each strip's plate send (the middle plane, 25.09.2026)
    float m_stripFx[STRIP_COUNT] = {};      ///< send into the modulation chain (texture and vocal only)

    PsyFxChain m_sfxFx;    ///< the SFX strip's insert chain
    PsyFxChain m_sendFx;   ///< the send chain (the voices')
    /**
     * @brief The bed's modulation send (24.09.2026): the same chain and knobs as m_sendFx, without the spoken
     *        phrases' frequency shift.
     *
     * A frequency shifter moves every partial by the same number of hertz, so it turns any pitched sound
     * inharmonic. Every spoken phrase shifts the send chain by 8 to 25 Hz (dispatch, m_sendMotion) -- meant for
     * the voice, which is speech and has no pitch to lose -- and the bed shared that chain: under the intro's
     * phrases the didgeridoo's tonic drone got a copy of itself a few hertz off every harmonic. The user,
     * asked to have every voice checked for such detunings after the pad's: "Bitte untersuche, ob es zu solchen
     * Verstimmungen auch in den anderen Stimmen kommen kann". Flanger and phaser, which only colour, stay.
     */
    PsyFxChain m_bedFx;
    /** @name The per-voice modulation insert (20.09.2026, round "dialogue"; Params.h, PolyMod)
     *  One flanger and one phaser per polyphonic instance, behind the voice and before its strip gain,
     *  so that the comb / flanger / phaser colour the user's effect list asks for is a property of the
     *  *voice* and not of the effects bus alone. At poly.mod = Off neither is ticked and the strip is
     *  sample for sample the strip without inserts.
     *  @{ */
    Flanger m_polyFlanger[PolyInstances];    ///< each voice's flanger (and comb)
    Phaser  m_polyPhaser[PolyInstances];     ///< each voice's phaser
    int     m_polyMod[PolyInstances] = {};   ///< each voice's poly.mod (PolyMod)
    /** @} */
    float m_sendReturn = 0.7f;   ///< psyfx.return
    float m_motion = 0.7f;       ///< psyfx.motion: how far events move the chains
    /**
     * @brief The event motion of one chain: which event last moved it, from when and for how long
     *        (in beats), and how far. Evaluated at every chunk start from the chunk's beat, so the motion
     *        is a function of the score alone.
     */
    struct Motion
    {
        double m_start = 0.0;      ///< the beat the event began
        double m_length = 0.0;     ///< how long it moves the chain, beats
        float  m_shiftHz = 0.0f;   ///< how far it shifts, Hz
        float  m_flange = 0.0f;    ///< how far it moves the flanger
    };
    Motion m_sfxShift;     ///< the SFX chain's shift strand
    Motion m_sfxFlange;    ///< the SFX chain's flanger strand
    Motion m_sendMotion;   ///< the send chain's motion
    /** @brief Evaluates the effect events' motion (shift and flanger) for the current chunk and hands it to the chains. */
    void applyMotion();

    TempoDelay m_throw;               ///< the vocal's delay throw
    float      m_throwSend = 0.6f;    ///< vocal.throw_send
    Stutter    m_stutter;             ///< buffer repeat of the melodic bus
    Ducker     m_subDuck;             ///< the kick's hold on the sub drop
    Ducker     m_duck[STRIP_COUNT];   ///< each strip's duck under the kick
    Ducker     m_returnDuck;          ///< the returns' duck under the kick
    /** @name The mix guide's two ducks that are not the kick's (25.09.2026)
     *  The counter gives way to the lead (mix.counter_duck, 1 .. 3 dB, back in 50 ms), and the pad's presence band
     *  -- a bell at 1.2 kHz, an octave and a half wide, over 500 Hz .. 3 kHz -- drops by mix.pad_lead_duck while a
     *  lead note sounds (its hold is the note's length, its release 80 ms): the guide's dynamic EQ, the pad
     *  stepping back exactly where and while the lead speaks. Both are driven by the lead's notes, as the kick's
     *  ducks are by the kick's: deterministic, no detector.
     *  @{ */
    Ducker m_leadDuck;               ///< the counter's gain under the lead
    Ducker m_padBandDuck;            ///< the pad's presence band under the lead
    Svf    m_padBandL, m_padBandR;   ///< the band's filters (a bandpass whose share is subtracted)
    float  m_counterDuck = 0.0f;     ///< mix.counter_duck
    float  m_padBandCut = 1.0f;      ///< linear gain of the band at full depth (mix.pad_lead_duck)
    /** @} */
    TranceGate m_gate[PolyInstances];               ///< each voice's trance gate
    bool       m_gateOn[PolyInstances] = {};        ///< poly.gate per voice
    int        m_gatePattern[PolyInstances] = {};   ///< poly.gate_pattern per voice
    float      m_gateDepth[PolyInstances] = {};     ///< the gate's depth per voice
    float      m_gateDuty[PolyInstances] = {};      ///< the gate's duty per voice
    float      m_gateTone[PolyInstances] = {};      ///< the gate's tone per voice
    double     m_gateAttack[PolyInstances] = {};    ///< the gate's attack per voice, beats
    double     m_gateRelease[PolyInstances] = {};   ///< the gate's release per voice, beats

    Reverb m_room;   ///< the room (send)
    Reverb m_hall;   ///< the hall (send)
    /** @brief The plate (25.09.2026): the middle plane of depth -- acid, counter, arp and stab -- between the dry front
     *         and the hall; short, bright, its pre-delay on the tempo grid (fx.plate_*). */
    Reverb m_plate;
    float  m_plateReturn = 0.0f;   ///< fx.plate_return, linear
    float  m_plateToHall = 0.0f;   ///< fx.plate_to_hall: the plate's output share that feeds the far room (the addon)
    /** @name Distance (25.09.2026, the Dark-Ambient addon)
     *  One cue for the five of distance, per polyphonic voice: poly.distance against the voice's own plane
     *  (DistancePlane) moves its level, a low pass, its plate send and its width together (distanceCue), so a
     *  voice is never near by one cue and far by another -- the addon's "half assignment" that sticks to the
     *  speakers. At the default the deltas are nil and nothing is processed.
     *  @{ */
    float m_distGain[PolyInstances] = {};    ///< linear gain against the plane's
    float m_distWet[PolyInstances] = {};     ///< plate send added (may take away)
    float m_distWidth[PolyInstances] = {};   ///< side gain against the plane's
    bool  m_distLpOn[PolyInstances] = {};    ///< the low pass is engaged (the voice is further than its plane)
    Svf   m_distLpL[PolyInstances], m_distLpR[PolyInstances];   ///< the low passes
    /** @} */
    float              m_slowWidth[PolyInstances] = {};   ///< poly.slow_mod, for the width's free movement (Engine.cpp)
    int                m_monitor = 0;                     ///< master.monitor
    float              m_slowSide = 0.0f;                 ///< the free width movement's sine, renewed every 32 samples
    Svf                m_monLp1;                          ///< the sub monitor's low pass at 80 Hz: first stage
    Svf                m_monLp2;                          ///< ... second stage
    std::vector<float> m_plateInL;                        ///< the plate's send, left
    std::vector<float> m_plateInR;                        ///< ... right
    std::vector<float> m_plateOutL;                       ///< the plate's return, left
    std::vector<float> m_plateOutR;                       ///< ... right
    float              m_roomReturn = 0.5f;               ///< the room's return, linear
    float              m_hallReturn = 0.5f;               ///< the hall's return, linear
    std::vector<float> m_roomInL;                         ///< the room's input of a segment, left
    std::vector<float> m_roomInR;                         ///< ... right
    std::vector<float> m_hallInL;                         ///< the hall's input, left
    std::vector<float> m_hallInR;                         ///< ... right
    std::vector<float> m_roomOutL;                        ///< the room's output, left
    std::vector<float> m_roomOutR;                        ///< ... right
    std::vector<float> m_hallOutL;                        ///< the hall's output, left
    std::vector<float> m_hallOutR;                        ///< ... right
    /**
     * @name The gated hall (20.09.2026, round "reverb"; A2-gated-reverb.md)
     * A second, dedicated hall a voice's hall_send is routed into instead of the plain one when its own
     * `hall_gate` is on (poly.hall_gate, acid.hall_gate) -- so gating is an independent per-voice choice
     * and never touches the plain hall's tail for the voices that leave it alone. Recipe is fixed
     * (Engine.cpp, applyParams()): the brief names a result ("a big hall ... ducked ... cut hard"), not
     * a set of knobs. m_hallGateOn is indexed like Strip (Reverb.h `Strip` below), so an off entry for a
     * strip that never sets it (perc, sfx, texture, vocal) reads false, its default.
     * @{
     */
    Reverb             m_hallGate;
    std::vector<float> m_hallGateInL, m_hallGateInR, m_hallGateOutL,
        m_hallGateOutR;                    ///< the gated hall's input and output of a segment
    bool m_hallGateOn[STRIP_COUNT] = {};   ///< per strip: its hall send goes to the gated hall
    /// closeBeats_/holdBeats_/openBeats_ converted each chunk from fixed ms constants at the current
    /// tempo (Reverb::barGate() takes beats, like TranceGate::open() -- the same reason: tempo ramps
    /// between chunks, beats do not drift with it).
    double m_hallGateCloseBeats = 0.001, m_hallGateHoldBeats = 0.0, m_hallGateOpenBeats = 0.001;
    /** @} */

    BusCompressor       m_comp;               ///< the master's bus compressor
    Svf                 m_sideHp1;            ///< mono below (master.mono_below): the side's high pass, first stage
    Svf                 m_sideHp2;            ///< ... and the second (fourth-order Butterworth together)
    HalfbandUp<float>   m_clipUpL;            ///< the clipper's 2x upsampling, left
    HalfbandUp<float>   m_clipUpR;            ///< ... right
    HalfbandDown<float> m_clipDownL;          ///< its way back down, left
    HalfbandDown<float> m_clipDownR;          ///< ... right
    bool                m_clipperOn = true;   ///< master.clipper
    float               m_clipperT = 1.0f;    ///< the clipper's threshold, linear
    /// The upper end of the programme (Dsp.h, BandLimit). It sits after the clipper and before the
    /// limiter on purpose: the limiter's ceiling is a true-peak claim, and a true-peak estimate is a
    /// band-limited reconstruction, so the limiter has to be handed a signal that lives inside its
    /// own band -- otherwise it reads 0.9 dB low and lets the ceiling through (measured 16.09.2026).
    BandLimit       m_bandLimitL, m_bandLimitR;   ///< ... right
    TruePeakLimiter m_limiter;                    ///< the lookahead true-peak limiter
    bool            m_limiterOn = true;           ///< master.limiter
    LoudnessMeter   m_meter;                      ///< the output meter (BS.1770)
};

}   // namespace phos
