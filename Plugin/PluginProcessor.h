/**
 * @file PluginProcessor.h
 * @brief The Phosphene plugin: the core's engine and composer under a JUCE processor.
 *
 * **Who owns time.** The engine's own clock always starts at beat 0 (phos::Engine has no way to be
 * placed anywhere else, and it should not have one: a deterministic render is a render from the
 * start). The plugin therefore keeps one number, PhospheneProcessor::beatOffset(), the *musical* beat that the
 * engine's beat 0 stands for. Host beat = engine beat + offset. In the standalone the offset is 0
 * unless the player jumps to a track; under a host it is the playhead's position at the moment
 * playback started or jumped, so a seek costs one engine reset and nothing else. Because
 * phos::Composer::composeBars is deterministic per bar, the composer simply carries on at the bar
 * the host landed on.
 *
 * **Threads.** processBlock() runs the engine and never allocates. A thread called "composer"
 * (PhospheneProcessor::composerLoop) calls PlugConductor::pump ahead of the play position, pushes
 * notes and control events into the engine's two lock-free rings, and in its spare time plans the
 * tracks ahead. It is the only thread that ever touches phos::Composer, whose plans are cached
 * lazily and whose first plan of a track *renders* that track to measure its level -- two seconds
 * of work, which is why nothing the user or the host waits on shares a lock with it: prepareToPlay
 * takes the short `m_engineLock`, and the editor reads a published copy of the plans.
 *
 * **Tempo.** phos::Engine::setTempoMap is never used here. Building a map plans every track it
 * covers, so a half-hour set would begin with half a minute of silence; instead the conductor
 * writes each track's tempo as a control event on compose.bpm, which is how every other departure
 * from a knob already travels. Under a host, the host's tempo is written into compose.bpm directly
 * and no tempo events are produced at all.
 *
 * **Restarts are a handshake.** Everything that has to reposition the engine -- the transport
 * starting, a seek, a new seed -- goes through a three-step handshake between the audio thread and
 * the composer thread (see `m_genWanted`). The audio thread renders silence while it is in flight,
 * which is the price for never allocating and never locking on the audio thread. In a non-realtime
 * run (offline bounce, the host test) the audio thread performs the composer's steps itself, so a
 * block is self-contained and the output is bit-identical to phos_render.
 *
 * **Curation.** Locking and rerolling (PLAN 6.8) belong to phos::Composer, which only the composer
 * thread may touch, and a reroll may cost a probe render. The editor therefore does not call the
 * composer: it pushes a CurationCommand into a lock-free ring and keeps a mirror of the locks for
 * its own drawing, and the composer thread applies the command the next time it comes round. Nothing
 * on the message thread waits for a plan.
 *
 * **Perform macros.** They are immediate parameter moves, not score events, and that is a measured
 * decision rather than a shortcut: phos::Engine's control ring is strictly first in, first out and
 * the conductor keeps it filled eight bars ahead, so an event pushed now would fire only after every
 * event already queued -- about thirteen seconds at 145 BPM. A macro instead writes the *knob*, in
 * the knob's normalised domain and as an offset from the value it had (`m_macroBase`), which is the
 * language a phos::ControlEvent speaks; the composer's own offsets keep riding on top, and letting
 * go puts every knob back exactly where it was. See #MacroE.
 *
 * **Environment.** `PHOS_MUTE=1` starts the output muted and the plugin never unmutes itself, so
 * tests and the screenshot export are silent. `PHOS_SHOT` is read by the editor (PluginEditor.h).
 * @author Rene Weller
 */
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Frame.h"
#include "LinkClock.h"
#include "Jam.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include "phos/Composer.h"
#include "phos/Cue.h"
#include "phos/Engine.h"
#include "phos/Midi.h"
#include "phos/Gallery.h"
#include "phos/MidiMap.h"
#include "phos/Preferences.h"
#include "phos/Params.h"
#include "phos/Score.h"
#include "phos/SoundPresets.h"
#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

/**
 * @brief The live macros of the Perform tab (PLAN 8.1).
 *
 * Four, because a macro that nobody can describe in one line is a knob with a nickname. Each one
 * names a handful of parameters and the size of its move; the manual generator prints that table
 * from `macroTargets`, so what the manual says is what the plugin does.
 */
enum class MacroE : int
{
    FILTER_SWEEP = 0,   ///< -1..+1: opens or closes the acid, lead, counter, arp and stab filters together
    GATE_DEPTH,         ///< 0..1: the trance gate of lead, counter, arp, stab and pad, on and as deep as this
    DROP_OUT,           ///< momentary: kick and bass gone until the next bar line
    STUTTER,            ///< held: lead, counter, arp, stab and pad chopped on sixteenths at full depth
    COUNT
};
constexpr int            NumMacros = static_cast<int>( MacroE::COUNT );   ///< number of macros
extern const char *const MacroNames[NumMacros];                           ///< display names
extern const char *const MacroHelp[NumMacros];   ///< one line each, for the manual and the tooltip
/**
 * @brief Whether a macro is a momentary press (as opposed to a value that is held).
 * @param[in] m  the macro
 * @return true for a momentary press
 */
bool macroIsMomentary( MacroE m );

/**
 * @brief One host parameter, backed by one entry of phos::ParamStore.
 *
 * The store is the only place a value lives: getValue() reads it and setValue() writes it, both as
 * a relaxed atomic, so there is no second copy that could drift from the engine's. The normalised
 * mapping, the display text and the step count all come from the descriptor table, which is what
 * makes the whole parameter set -- roughly six hundred of them across the modules -- generated
 * rather than written out.
 */
class StoreParameter final : public juce::RangedAudioParameter
{
public:
    /**
     * @brief Binds a parameter.
     * @param[in,out] store  the engine's parameter store (outlives this object)
     * @param[in]     id     global parameter id
     * @param[in]     name   display name shown by the host
     */
    StoreParameter( phos::ParamStore &store, int id, const juce::String &name );
    /**
     * @brief The global id of the store's parameter this one is.
     * @return the store id
     */
    int storeId() const { return m_id; }

    /**
     * the store's value, normalised
     * @return the store's value, normalised
     */
    float getValue() const override;
    /**
     * writes the store (relaxed atomic)
     * @param[in] newValue  the value, normalised
     */
    void setValue( float newValue ) override;
    /**
     * the descriptor's default, normalised
     * @return the default, normalised
     */
    float getDefaultValue() const override;
    /**
     * the display name (0: no limit)
     * @param[in] maximumStringLength  the longest name the host takes, 0 for no limit
     * @return the display name
     */
    juce::String getName( int maximumStringLength ) const override;
    /**
     * the unit
     * @return the unit
     */
    juce::String getLabel() const override;
    /**
     * steps of a discrete parameter
     * @return the steps of a discrete parameter
     */
    int getNumSteps() const override;
    /**
     * Int, Choice and Toggle are discrete
     * @return true for Int, Choice and Toggle
     */
    bool isDiscrete() const override;
    /**
     * a Toggle is boolean
     * @return true for a Toggle
     */
    bool isBoolean() const override;
    /**
     * the value as the panel shows it
     * @param[in] normalisedValue      the value, normalised
     * @param[in] maximumStringLength  the longest text the host takes
     * @return the value as the panel shows it
     */
    juce::String getText( float normalisedValue, int maximumStringLength ) const override;
    /**
     * parses a choice name, On/Off or a number
     * @param[in] text  a choice name, On/Off or a number
     * @return the value, normalised
     */
    float getValueForText( const juce::String &text ) const override;
    /**
     * the store's own mapping
     * @return the store's mapping
     */
    const juce::NormalisableRange<float> &getNormalisableRange() const override { return m_range; }

    /**
     * @brief The global id in the store.
     * @return the id in the store
     */
    int paramId() const { return m_id; }

private:
    phos::ParamStore              &m_store;   ///< the store the value lives in
    int                            m_id;      ///< its global id
    juce::String                   m_name;    ///< the name the host shows
    juce::NormalisableRange<float> m_range;   ///< the store's own mapping, for the host
};

/** @brief `PHOS_TRACE=1`: the conductor's bar-by-bar work on stderr (set by the processor, PlugConductor.cpp). */
extern bool gPlugTrace;

/**
 * @brief Keeps the engine's rings filled, at an offset, from any bar.
 *
 * phos::Conductor always starts at bar 0 and pushes the composer's absolute beats unchanged, which
 * is right for the offline render and not enough for a host that seeks. This is the same loop --
 * whole bars, in order, controls before notes, stop when a ring is full -- with two additions: a
 * beat offset (engine beat = musical beat - offset) and a start bar. With offset 0 and start bar 0
 * it pushes exactly what phos::Conductor pushes, in the same order, which is what lets the host
 * test compare the plugin with phos_render sample for sample.
 */
class PlugConductor
{
public:
    /**
     * @brief Binds an engine and a composer (both outlive this object).
     * @param[in,out] engine    the engine
     * @param[in]     composer  the composer
     */
    PlugConductor( phos::Engine &engine, const phos::Composer &composer ) : m_engine( engine ), m_composer( composer )
    {
    }

    /**
     * @brief Repositions: the next bar to compose and the musical beat that engine beat 0 means.
     *
     * Also replays the sound state of the track the bar belongs to: the composer writes a track's
     * recipe as control events on its first bar, and a set that is joined in the middle would
     * otherwise play the following bars with the knobs alone. Up to #CatchUpBars bars before the
     * landing point are composed for their control events only; the last value of each parameter is
     * pushed as an immediate change. A ramp that was in flight lands at its target, which is where
     * it was heading.
     * @param[in]     params      the knobs
     * @param[in]     startBar    the next bar to compose
     * @param[in]     beatOffset  the musical beat of engine beat 0
     * @param[in]     writeTempo  true to write the tempo too
     * @param[in,out] engineLock  the engine's lock
     */
    void seek( const phos::ParamStore &params, int startBar, double beatOffset, bool writeTempo,
               std::mutex &engineLock );
    /**
     * @brief Composes and pushes until @p horizonBeats beyond the engine's position are covered.
     * @param[in]     params        knob values to compose with
     * @param[in]     horizonBeats  how far ahead to keep the rings filled
     * @param[in,out] midiOut       if not null, every pushed note is also written here (engine beats)
     * @param[in,out] engineLock    held while events are pushed, released while bars are composed
     */
    void pump( const phos::ParamStore &params, double horizonBeats, phos::EventRing<phos::NoteEvent> *midiOut,
               std::mutex &engineLock );
    /**
     * @brief Binds the ring the cue tap drains (PLAN 8.3); null switches mark generation off.
     *
     * Section boundaries and keys are decisions, not arithmetic, so the audio thread cannot work
     * them out for itself -- but they must not be *sent* from here either, because this runs bars
     * ahead of the sound. The conductor therefore writes what it knows into a ring in beat order,
     * and phos::CueTap pops a mark when the play position reaches it (Cue.h).
     * @param[in,out] marks  the ring, or null
     */
    void setCueMarks( phos::CueMarkRingT *marks ) { m_marks = marks; }

    /**
     * @brief Index of the next bar to be composed.
     * @return the bar's index
     */
    int nextBar() const { return m_nextBar; }
    /**
     * @brief The musical beat that engine beat 0 stands for.
     * @return the musical beat
     */
    double beatOffset() const { return m_beatOffset; }
    /**
     * @brief Nudges the offset by a fraction of a beat (host drift correction).
     * @param[in] delta  the fraction of a beat
     */
    void nudgeOffset( double delta ) { m_beatOffset += delta; }

    /**
     * @brief The notes of the bars last composed, for the editor's pattern preview.
     *
     * The conductor keeps the notes of the last #PreviewBars bars it composed, and every older one still sounding
     * there, in musical beats. Reading them
     * is a copy under a short lock -- the editor never asks the composer itself, which would mean
     * waiting for a probe render.
     * @param[in]  firstBar  first bar wanted
     * @param[in]  bars      how many
     * @param[out] out       receives the notes that sound in that range, held ones struck before it included
     * @return false if nothing of that range has been composed yet
     */
    bool readPreview( int firstBar, int bars, std::vector<phos::NoteEvent> &out ) const;

    static constexpr int CatchUpBars = 128;   ///< how far back seek() looks for a track's sound
    static constexpr int PreviewBars = 24;    ///< bars kept for the editor's pattern preview

private:
    /**
     * @brief Pushes what the last composed bar left over into the engine's rings; false while they are full.
     * @param[in,out] midiOut  the MIDI out's ring, or null
     * @return false while the rings are full
     */
    bool flush( phos::EventRing<phos::NoteEvent> *midiOut );
    /**
     * @brief The sound state seek() replayed, waiting for the first pump() after the engine's reset.
     *
     * 23.09.2026: seek() used to push these straight into the engine -- and the seek is step 2 of the restart
     * handshake, after which the audio thread resets the engine (step 3), which empties the control ring. So a
     * set joined in the middle (Play from a position, a click in the Arrange tab) never got the replayed state:
     * the track's recipe, its part levels and its track gain stayed at the knobs until the next track started.
     * Found when the first track's measured line levels, sent while a seek was pending, did not arrive.
     */
    std::vector<phos::ControlEvent> m_landing;

    /**
     * @brief The tempo of the track that @p bar belongs to, as control events on compose.bpm.
     * @param[in]     params  the knobs
     * @param[in]     bar     the bar
     * @param[in,out] out     the events, added to
     */
    void tempoControls( const phos::ParamStore &params, int bar, std::vector<phos::ControlEvent> &out ) const;

    /**
     * @brief Pushes the cue marks of bar @p bar, in the order phos::Composer::sections() lists them.
     *
     * The same three rules that build a SectionMark there: the cut at the head of a breakdown, the
     * section itself, and the pre-drop break in the last bar of a buildup. Matching that function
     * exactly is what the offline demo checks -- the cues a listener receives and the score the
     * composer wrote have to be the same list of boundaries, bar for bar.
     * @param[in] params  knob values to plan with
     * @param[in] bar     musical bar
     * @param[in] first   true: also send the key, whatever the bar is (after a seek nothing is known yet)
     */
    void cueMarks( const phos::ParamStore &params, int bar, bool first );

    phos::Engine         &m_engine;             ///< where the events go
    const phos::Composer &m_composer;           ///< where they come from
    int                   m_nextBar = 0;        ///< the next bar to compose
    double                m_beatOffset = 0.0;   ///< musical beat of engine beat 0
    bool m_writeTempo = true;   ///< send the tempo walk's control events (not when the host sets the tempo)
    std::vector<phos::NoteEvent>    m_notes;             ///< the last composed bar's notes
    std::vector<phos::ControlEvent> m_controls;          ///< and its control events
    size_t                          m_notePos = 0;       ///< how many of the notes are in the engine's rings
    size_t                          m_controlPos = 0;    ///< ... and of the control events
    mutable std::mutex              m_previewLock;       ///< guards #m_preview (composer writes, editor reads)
    std::vector<phos::NoteEvent>    m_preview;           ///< the last PreviewBars bars, in musical beats
    phos::CueMarkRingT             *m_marks = nullptr;   ///< where the cue marks go, null = the bridge is off
    int                             m_cueKey = -1;       ///< the key the last mark carried, -1 = none sent yet
    int m_cueLandingBar = -1;                            ///< seek() already described this bar; pump must not repeat it
};

/** @brief What the editor needs to know about the transport, in one lump. */
struct TransportView
{
    bool   m_playing = false;      ///< the engine is being rendered
    bool   m_hostSync = false;     ///< the clock is the host's playhead
    double m_musicalBeat = 0.0;    ///< beat from the start of the set
    double m_bpm = 145.0;          ///< tempo now
    int    m_bar = 0;              ///< bar from the start of the set
    int    m_track = 0;            ///< index of the track that bar belongs to
    int    m_barInTrack = 0;       ///< bar within that track
    bool   m_restarting = false;   ///< a handshake is in flight (silent)
};

/** @brief The Phosphene plugin processor. */
class PhospheneProcessor final : public juce::AudioProcessor,
                                 private juce::Timer,
                                 private juce::AudioProcessorParameter::Listener
{
public:
    // ------------------------------------------------------------------ undo (23.09.2026)
    /**
     * @brief What an undo step puts back: every knob, the seed, every lock and every reroll counter.
     *
     * A step is two of these, before and after. Whole states rather than deltas: a knob gesture, a reroll and
     * a loaded set are then one kind of step, and a step can never be applied to a state it was not made in.
     * The macros are not in it -- they are performance, not editing, and put their knobs back themselves.
     */
    struct UndoState
    {
        std::string             m_knobs;                            ///< params().toText(false)
        uint64_t                m_seed = 1;                         ///< the set seed
        std::map<int, uint8_t>  m_locks[phos::NumLockUnits];        ///< the lock mirror
        std::map<int, uint32_t> m_variations[phos::NumLockUnits];   ///< the reroll counters
        /**
         * @brief Whether two states are the same (a step that changes nothing is not recorded).
         * @param[in] o  the other state
         * @return true when they are the same
         */
        bool operator==( const UndoState &o ) const;
    };
    /**
     * @brief The state an undo step records (message thread).
     * @return the state
     * @pre called on the message thread
     */
    UndoState captureUndoState() const;
    /**
     * @brief Puts a recorded state back (message thread); records nothing itself.
     * @param[in] s  the state
     * @pre called on the message thread
     */
    void applyUndoState( const UndoState &s );
    /**
     * @brief Runs an editing action from the editor as one undo step named @p name: seed, locks, rerolls, a
     *        loaded set, the factory reset. Knob gestures are recorded without this (parameterGestureChanged).
     * @param[in] name    the step's name
     * @param[in] action  the action
     */
    void undoable( const juce::String &name, const std::function<void()> &action );
    // ------------------------------------------------------------------ gallery and ratings (23.09.2026)
    /**
     * @brief Where saved sets live: `PHOS_GALLERY_DIR`, else `<application data>/Phosphene/Sets` (created).
     * @return the folder
     */
    juce::File galleryFolder() const;
    /**
     * @brief The listener's verdicts (phos/Rating.h): `<application data>/Phosphene/ratings.tsv`.
     * @return the ratings file
     */
    juce::File ratingsFile() const;
    /**
     * @brief The user folder: %APPDATA%/Phosphene, or PHOS_USER_DIR when that is set (the tests). Created on demand.
     * @return the folder
     */
    juce::File userFolder() const;
    /**
     * @brief Saves the set as it stands into the gallery: the `.phosset` with the gallery lines (phos/Gallery.h)
     *        -- name, time, and every published track's style, tempo and form.
     * @param[in] name  the set's name
     * @return the file written, or a non-existent File when it could not be written
     */
    juce::File saveToGallery( const juce::String &name );

    // ------------------------------------------------------------------ learned preferences (23.09.2026)
    /**
     * @brief `<application data>/Phosphene/preferences.txt`, loaded at start when it exists (phos/Preferences.h).
     * @return the file
     */
    juce::File preferencesFile() const;
    /**
     * @brief The preferences fitted from ratingsFile(), and how many verdicts with features went in.
     * @param[out] verdicts  receives the verdicts used, or null
     * @return the preferences
     */
    phos::Preferences fitFromRatings( int *verdicts = nullptr ) const;
    /**
     * @brief Writes @p prefs to preferencesFile(), makes them the process's, and plans the set again from here.
     * @param[in] prefs  the preferences
     * @return false if the file cannot be written
     */
    bool applyPreferences( const phos::Preferences &prefs );
    /** @brief Renames preferencesFile() to preferences.txt.old, drops the preferences, plans again from here. */
    void forgetPreferences();
    /**
     * @brief One line on what is in force: "none", or the number of weights and the strongest of them.
     * @return the line
     */
    juce::String preferencesSummary() const;

    /**
     * one step back; false when there is none
     * @return false if there was no step
     */
    bool undo();
    /**
     * one step forward
     * @return false if there was no step
     */
    bool redo();
    /**
     * for the editor's buttons
     * @return true when there is a step to undo
     */
    bool canUndo() const { return m_undo.canUndo(); }
    /**
     * @copybrief canUndo
     * @return true when there is a step to redo
     */
    bool canRedo() const { return m_undo.canRedo(); }
    /**
     * what undo() would take back
     * @return the step's name, empty for none
     */
    juce::String undoName() const { return m_undo.getUndoDescription(); }
    /**
     * what redo() would do again
     * @return the step's name, empty for none
     */
    juce::String redoName() const { return m_undo.getRedoDescription(); }

    /** @brief Creates the engine, the composer and its thread, and binds every parameter. */
    PhospheneProcessor();
    ~PhospheneProcessor() override;

    /**
     * prepares the engine and reloads the score
     * @param[in] sampleRate       the sample rate
     * @param[in] samplesPerBlock  the largest block
     */
    void prepareToPlay( double sampleRate, int samplesPerBlock ) override;
    void releaseResources() override {}   ///< nothing to release
    /**
     * @brief Offline: the audio thread takes the composer's work over, so a render is reproducible.
     * @param[in] offline  true for an offline render
     */
    void setNonRealtime( bool offline ) noexcept override;
    /**
     * stereo out only
     * @param[in] layouts  the layout asked for
     * @return true for a stereo output
     */
    bool isBusesLayoutSupported( const BusesLayout &layouts ) const override;
    /**
     * plays, following the host's playhead
     * @param[out]    buffer        the block's audio
     * @param[in,out] midiMessages  the block's MIDI in, then the parts' notes out
     */
    void processBlock( juce::AudioBuffer<float> &, juce::MidiBuffer & ) override;

    /**
     * the panel
     * @return a new panel
     */
    juce::AudioProcessorEditor *createEditor() override;
    /**
     * it has one
     * @return true
     */
    bool hasEditor() const override { return true; }

    /**
     * "Phosphene"
     * @return the plugin's name
     */
    const juce::String getName() const override { return JucePlugin_Name; }
    /**
     * @brief Since 23.09.2026 the plugin reads MIDI controllers (MIDI learn, phos/MidiMap.h); notes it ignores.
     * @return true
     */
    bool acceptsMidi() const override { return true; }
    /**
     * @brief Yes: the parts go out as MIDI.
     * @return true
     */
    bool producesMidi() const override { return true; }
    /**
     * @brief No: it makes sound.
     * @return false
     */
    bool isMidiEffect() const override { return false; }
    /**
     * @brief Four seconds: the rooms ring on.
     * @return the tail in seconds
     */
    double getTailLengthSeconds() const override { return 4.0; }

    /**
     * one program
     * @return 1
     */
    int getNumPrograms() override { return 1; }
    /**
     * always the one
     * @return 0
     */
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram( int ) override {}   ///< nothing to switch
    /**
     * "Set"
     * @return the program's name
     */
    const juce::String getProgramName( int ) override { return "Set"; }
    void changeProgramName( int, const juce::String & ) override {}   ///< not renameable

    /**
     * seed, rerolls, parameters, controllers as XML
     * @param[out] destData  the state
     */
    void getStateInformation( juce::MemoryBlock &destData ) override;
    /**
     * restores them, lets go of the macros, composes
     * @param[in] data         the state
     * @param[in] sizeInBytes  its size in bytes
     */
    void setStateInformation( const void *data, int sizeInBytes ) override;
    /**
     * @brief The version of the state this build writes.
     *
     * 2 since 19.09.2026 (the reordered voices, the counter-lead, the stab and the drone; docs/rounds/2026-09.md,
     * "Stimmen"). **3 since 20.09.2026** (round "dialogue"), and it is a change of meaning, not only of
     * content: a version-3 state stores **only the knobs that differ from the defaults of the build that
     * saved it**. Every knob a state does not name follows the current defaults when it is loaded.
     *
     * Why. The user's standalone state carried every parameter of an older session, including all the
     * values later rounds recalibrated -- sfx.level -12 against today's -3, bass.cutoff 140 against 240,
     * mix.lead_level 0 against -6, kick.level -2 against -6 -- and loading it silently undid every one of
     * them. It cost an evening of listening to an old mix. A state that names only what the user touched
     * cannot do that: a recalibrated default reaches every session that never touched that knob.
     *
     * The price, written down rather than hidden: a knob the user deliberately set *to* the value that
     * was the default at the time is not stored, so if that default later moves, the deliberate choice
     * moves with it. There is no way to tell the two apart from a value alone -- that is exactly the
     * information a full state does not carry either, only with the opposite failure.
     *
     * What happens to older states is in setStateInformation().
     */
    static constexpr int StateVersion = 3;
    /**
     * @brief The version of the last state read by setStateInformation (1 for a state older than the voices round).
     * @return the version
     */
    int lastStateVersion() const { return m_lastStateVersion; }
    /**
     * @brief The knob text of a state older than StateVersion that was **not** applied, or empty.
     *
     * A state of version 1 or 2 holds every value of its session and cannot say which of them the user
     * chose, so loading it would silently undo the defaults of later calibrations. Such a state therefore does
     * not set a single knob: the engine starts at today's factory defaults and the text is kept here so
     * that the editor can offer it (PhospheneEditor's banner, adoptLegacyState()).
     * @return the knob text, or empty
     */
    const juce::String &pendingLegacyState() const { return m_legacyKnobs; }
    /** @brief Applies the knobs of the held older state after all, at the user's word. */
    void adoptLegacyState();
    /** @brief Forgets the held older state (the user kept the new defaults). */
    void dismissLegacyState() { m_legacyKnobs.clear(); }
    /**
     * @brief Every knob back to its factory default -- the "reset to factory defaults" of the rule.
     *
     * The set itself (seed, locks, rerolls) is not a knob and is left alone: the user asked for the
     * *sound* to come back to the shipped calibration, not for their set to be thrown away.
     */
    void resetToFactoryDefaults();

    // ------------------------------------------------------------------ sound presets (23.09.2026, phos/SoundPresets.h)
    /**
     * @name Sound presets
     * A synth is Module::Kick, Module::Bass, Module::Acid or Module::Poly with its instance. Its presets are the
     * factory ones (phos::factoryPresets) and the user's, which live as `<name>.txt` in a folder per synth under the
     * user folder (PHOS_USER_DIR in the tests, so a test never writes into the real one).
     * @{ */
    /**
     * @brief The folder of the user's presets for one synth ("<user folder>/Presets/lead").
     * @param[in] module    the synth
     * @param[in] instance  its instance
     * @return the folder
     */
    juce::File userPresetFolder( phos::ModuleE module, int instance ) const;
    /**
     * @brief The user's presets of one synth, in the group "User", sorted by name.
     * @param[in] module    the synth
     * @param[in] instance  its instance
     * @return the presets
     */
    std::vector<phos::SoundPreset> userPresets( phos::ModuleE module, int instance ) const;
    /**
     * @brief Applies a preset as one undo step and switches the synth's own sound on (mix.*_own), so the composer's
     *        per-track recipes leave the sound as the preset has it.
     * @param[in] module    the synth
     * @param[in] instance  its instance
     * @param[in] preset    the preset
     */
    void applyPreset( phos::ModuleE module, int instance, const phos::SoundPreset &preset );
    /**
     * @brief Opens one undo step of several changes (15.10.2026: a modulation added, a depth dragged, a value typed into
     *        the command palette); endStep() records it. Steps nest, and a knob's gesture inside one joins it.
     * @param[in] what  the step's name
     */
    void beginStep( const juce::String &what );
    void endStep();   ///< closes beginStep's step
    /**
     * @brief The synth knob @p id belongs to takes the composer's preset over (Own Sound on) where it plays one --
     *        before a knob of it is set by hand.
     * @param[in] id  the knob's store id
     */
    void takeOver( int id );
    /**
     * @brief Sets the knobs @p values (store id, real value) through the host as one undo step named @p what.
     * @param[in] what    the undo step's name
     * @param[in] values  (store id, real value) of the knobs
     */
    void applyKnobs( const juce::String &what, const std::vector<std::pair<int, float>> &values );
    /**
     * @brief Plays @p values (store id, real value) while a preset is listened to (the browser, A|B held) -- in the store
     *        only: no undo step, nothing to the host --; endPreview() takes them back.
     * @param[in] values  (store id, real value) of the knobs
     */
    void previewKnobs( const std::vector<std::pair<int, float>> &values );
    /** @brief Back to the knobs as they were before previewKnobs (nothing when no preview plays). */
    void endPreview();

    /** @name The Field track's presets and library (27.09.2026; phos/FieldPresets.h, phos/FieldLibrary.h)
     *  @{ */
    /**
     * @brief Puts the Field knobs where factory preset @p index has them, as one undo step (message thread).
     * @param[in] index  the factory preset
     * @pre called on the message thread
     */
    void applyFieldPreset( int index );
    /**
     * @brief The user's own folder of field recordings, scanned beside the shipped one ("" none).
     * @return the folder, empty for none
     */
    juce::String fieldFolder() const { return m_fieldFolder; }
    /**
     * @brief Sets that folder and scans the library again; it is saved with the plugin's state (message thread).
     * @param[in] folder  the folder, empty for none
     * @pre called on the message thread
     */
    void setFieldFolder( const juce::String &folder );
    /** @} */

    /**
     * @brief Auditions bank preset @p preset of effect family @p choice (sfx::PresetRiser order) in the running set:
     *        the audio thread plays it at its next block (phos::Engine::previewSfx). Nothing sounds while the
     *        transport stands -- the engine renders silence then. Message thread.
     * @param[in] choice  the effect family
     * @param[in] preset  the bank preset
     * @pre called on the message thread
     */
    void previewSfx( int choice, int preset );
    /**
     * @brief The audition waiting for the audio thread (choice << 12 | preset), -1 none -- for the tests.
     * @return choice << 12 | preset, or -1
     */
    int pendingSfxPreview() const { return m_sfxPreview.load( std::memory_order_acquire ); }

    /**
     * @brief The channel meters (24.09.2026): per part (phos::Part order), the peak and the RMS of what it put into
     *        the mix since the last call, as linear amplitudes; both 0 where nothing was rendered. Message thread
     *        (the mixer page's timer).
     * @param[out] peak  the peak per part
     * @param[out] rms   the RMS per part
     * @pre called on the message thread
     */
    void takeChannelMeters( float *peak, float *rms );
    /**
     * @brief The value parameter @p id plays at (26.09.2026, the live ring): the knob with the composer's preset base,
     *        its rides and the level match's corrections on it, as the engine applied them in the last block. Any thread.
     * @param[in] id  the parameter
     * @return the value played
     */
    float playedValue( int id ) const
    {
        return id >= 0 && id < m_playedCount ? m_played[static_cast<size_t>( id )].load( std::memory_order_relaxed )
                                             : 0.0f;
    }
    /**
     * @brief Saves the synth's knobs as the user preset @p name; returns the file, or File() when it failed.
     * @param[in] module    the synth
     * @param[in] instance  its instance
     * @param[in] name      the preset's name
     * @return the file, or File() when it failed
     */
    juce::File saveUserPreset( phos::ModuleE module, int instance, const juce::String &name );
    /** @} */

    // ------------------------------------------------------------------ the set
    /**
     * @brief The engine's parameters -- the single copy of every value.
     * @return the parameters
     */
    phos::ParamStore &params() { return m_engine->params(); }
    /**
     * @brief The engine's parameters (read only).
     * @return the parameters
     */
    const phos::ParamStore &params() const { return m_engine->params(); }
    /**
     * @brief The engine, for the editor's meters.
     * @return the engine
     */
    const phos::Engine &engine() const { return *m_engine; }
    /**
     * @brief The host parameter for a global id.
     * @param[in] id  the global id
     * @return the host parameter, or null
     */
    StoreParameter *parameterFor( int id ) const
    {
        return id >= 0 && id < static_cast<int>( m_byId.size() ) ? m_byId[static_cast<size_t>( id )] : nullptr;
    }

    // ------------------------------------------------------------------ MIDI learn (23.09.2026, phos/MidiMap.h)
    /**
     * @name MIDI learn
     * A *target* is a host parameter: the store's ids first (0 .. params().count() - 1), then the four macros
     * (params().count() + Macro). Every one of them can take a controller, and every one of them is a host
     * parameter a DAW can automate.
     * @{ */
    /**
     * @brief The controller table (arm/bind/unbind on the message thread).
     * @return the controller table
     * @pre called on the message thread
     */
    phos::MidiMap &midiMap() { return m_midiMap; }
    const phos::MidiMap &midiMap() const { return m_midiMap; }
    /**
     * @brief How many targets there are.
     * @return the number of targets
     */
    int midiTargetCount() const { return params().count() + NumMacros; }
    /**
     * @brief The host parameter behind a target, or null.
     * @param[in] target  the target
     * @return the host parameter, or null
     */
    juce::RangedAudioParameter *hostParameter( int target ) const;
    /**
     * @brief A target's display name ("Lead Cutoff", "Macro Filter Sweep").
     * @param[in] target  the target
     * @return its display name
     */
    juce::String midiTargetName( int target ) const;
    /**
     * @brief A target's stable key for the saved map ("lead.cutoff", "macro.filter_sweep").
     * @param[in] target  the target
     * @return its key
     */
    std::string midiTargetKey( int target ) const;
    /**
     * @brief The target with key @p key, or -1.
     * @param[in] key  the key
     * @return the target, or -1
     */
    int midiTargetFind( const std::string &key ) const;
    /** @} */

    /**
     * @brief What became of the shipped wavetable pack in this process.
     *
     * The library is loaded once for the whole process, by whichever Engine::prepare() runs first,
     * so this says nothing about *this* instance -- it says what every instance has. The editor
     * draws it (a library table whose file is missing is marked in the chooser) and the host test
     * reads it, because "it fell back silently" is exactly the failure that has to be visible.
     */
    struct WaveTableLibrary
    {
        juce::String m_directory;     ///< where the plugin pointed the core; empty when it found no pack
        int          m_shipped = 0;   ///< library tables this build knows about
        int          m_loaded = 0;    ///< of those, the ones the pack really holds (indexed at load)
        /**
         * @brief Of those, the ones expanded into mip levels right now.
         *
         * Since 22.09.2026 a table is expanded when a track that uses it is planned, not at load
         * (WaveTableFile.h): 464 tables would be 935 MB. So this is a number about what has been
         * *played*, and it is small and rises; `loaded` is the one that says the installation is
         * complete.
         */
        int m_built = 0;
    };
    /**
     * @brief The state of the library now; counted freshly, so it is valid after prepareToPlay().
     * @return the library's state
     */
    WaveTableLibrary waveTableLibrary() const;

    /**
     * @brief What became of the two learned models of Phase 8 in this process.
     *
     * The same shape of answer as WaveTableLibrary and for the same reason: `melody.phosmdl` and
     * `bass.phosmdl` are opened by bare name, a file that is not there is not an error but a quiet
     * fall back to the Markov model and the pattern families, and the one line the core prints about
     * it goes to stderr -- which inside a DAW is nowhere. So the plugin asks the question once, at
     * start-up, and the Set tab shows the answer (EditorSetTab.cpp).
     *
     * Process-wide and not per instance: phos::sharedMelodyModel() and phos::sharedBassModel() load
     * once and are never unloaded, so every instance has exactly this.
     */
    struct LearnedModels
    {
        juce::String m_directory;         ///< where the plugin found the two files; empty when it found none
        bool         m_melody = false;    ///< the learned melody model really loaded (else: the Markov model)
        bool         m_bass = false;      ///< the learned bass role really loaded (else: the pattern families)
        double       m_melodyNll = 0.0;   ///< held-out nats per token the melody file declares
        double       m_bassNll = 0.0;     ///< the same, out of the bass file
        juce::String m_melodyNote;        ///< the core's own message when the melody model did not load
        juce::String m_bassNote;          ///< the same for the bass model
        /**
         * @brief A model loaded although nothing was installed beside the binary.
         *
         * Only a development build can reach this: there PhospheneCore still carries
         * PHOS_SOURCE_DATA_DIR and therefore finds Core/data on the machine that compiled it
         * (Core/CMakeLists.txt). It is shown, because a build that works only here looks from the
         * outside exactly like one that works everywhere.
         */
        bool m_fromSourceTree = false;
    };
    /**
     * @brief What the two models are in this process; settled once, before the first engine.
     * @return the models' state
     */
    LearnedModels learnedModels() const;

    /**
     * @brief The set seed.
     * @return the set's seed
     */
    uint64_t seed() const { return m_seed.load( std::memory_order_relaxed ); }
    /**
     * @brief Chooses another set (message thread); restarts playback from the top.
     * @param[in] s  the set's seed
     * @pre called on the message thread
     */
    void setSeed( uint64_t s );
    /** @brief A new seed from the clock. */
    void randomiseSeed();

    /**
     * @brief Copies out the plan of track @p index as the composer thread last published it.
     * @param[in]  index  the track
     * @param[out] out    its plan
     * @return false if that track has not been planned yet (the editor then shows what it has)
     */
    bool tryReadTrack( int index, phos::TrackPlan &out ) const;
    /**
     * @brief The notes of @p bars bars from @p firstBar, for the editor's pattern preview.
     * @param[in]  firstBar  the first bar
     * @param[in]  bars      the bars
     * @param[out] out       the notes
     * @return false if the conductor has not composed that range (the display then keeps its own)
     */
    bool readPattern( int firstBar, int bars, std::vector<phos::NoteEvent> &out ) const
    {
        return m_conductor->readPreview( firstBar, bars, out );
    }
    /** @brief Asks the composer to re-plan: the knobs that shape the set have changed. */
    void requestRestart()
    {
        m_plansStale.store( true, std::memory_order_release );
        m_restartRequest.store( true, std::memory_order_release );
    }

    // ------------------------------------------------------------------ curation (PLAN 6.8)
    /**
     * @brief Locks or unlocks a unit (message thread).
     *
     * The command travels to the composer thread through `m_curation`; what the editor draws comes
     * from the mirror this keeps, so a lock toggles under the mouse even while a track is being
     * planned. A locked unit is frozen on the seed it had, so rerolling anything around it leaves
     * it bit-identical.
     * @param[in] unit    the kind of unit
     * @param[in] index   the unit
     * @param[in] locked  true to lock
     * @pre called on the message thread
     */
    void setLock( phos::LockUnitE unit, int index, bool locked );
    /**
     * @brief Whether a unit is locked, from the editor's mirror.
     * @param[in] unit   the kind of unit
     * @param[in] index  the unit
     * @return true when locked
     */
    bool isLocked( phos::LockUnitE unit, int index ) const;
    /**
     * @brief Draws that unit again from a fresh seed; a locked unit does not move.
     * @param[in] unit   the kind of unit
     * @param[in] index  the unit
     */
    void reroll( phos::LockUnitE unit, int index );
    /**
     * @brief How often a unit has been rerolled (0 = never), from the mirror.
     * @param[in] unit   the kind of unit
     * @param[in] index  the unit
     * @return the rerolls
     */
    uint32_t variation( phos::LockUnitE unit, int index ) const;
    /** @brief Forgets every lock and every reroll. */
    void clearCuration();

    // ------------------------------------------------------------------ perform macros
    /**
     * @brief Moves a macro (message thread).
     * @param[in] m      which macro
     * @param[in] value  0 is neutral for all of them; #MacroE::FILTER_SWEEP runs -1..+1, the rest 0..1
     * @pre called on the message thread
     */
    void setMacro( MacroE m, float value );
    /**
     * @brief Where a macro stands.
     * @param[in] m  the macro
     * @return its value
     */
    float macroValue( MacroE m ) const { return m_macro[static_cast<size_t>( m )].m_value; }
    /**
     * @brief The macro tick (message thread): applies what is held and releases what has run out.
     *
     * Called by `timerCallback` at about 30 Hz. It is public because a test has no message loop and
     * has to turn the crank itself.
     * @pre called on the message thread
     */
    void serviceMacros();
    /** @brief The parameters one macro moves and how far, for the editor and the manual generator. */
    struct MacroTarget
    {
        int   m_param = -1;         ///< global parameter id
        float m_value = 0.0f;       ///< normalised delta, or the absolute normalised value
        bool  m_absolute = false;   ///< true: set the knob to @ref m_value instead of adding it
    };
    /**
     * @brief What macro @p m at @p value wants of the parameters.
     * @param[in]  m      the macro
     * @param[in]  value  its position
     * @param[out] out    receives at most #MaxMacroTargets entries
     * @return how many were written
     */
    int macroTargets( MacroE m, float value, MacroTarget *out ) const;
    static constexpr int MaxMacroTargets =
        24;   ///< most parameters one macro touches (Stutter: four each on five voices)

    // ------------------------------------------------------------------ transport
    /** @brief Standalone: starts at the top (or resumes after a pause). */
    void play();
    /**
     * @brief Compose set (01.10.2026, the frame's header): the plans made again with every knob as it stands -- the seed,
     *        the locks and the rerolls kept -- and the set started from its beginning.
     */
    void composeSet();
    /**
     * @brief How often the transport was asked to start again (the host test sees Compose set by it).
     * @return the restarts
     */
    int restartCount() const { return m_restarts.load( std::memory_order_relaxed ); }
    /**
     * @brief Parameter @p id back to its default, an undo step (the controls' right-click menu).
     * @param[in] id  the parameter
     */
    void resetToDefault( int id );
    /**
     * @brief The headset (01.10.2026, the frame): the hands of the Quest app in bridge mode, as OSC, while the settings do
     *        not say Off -- left pinch play and stop, both hands the next track, right pinch the drop-out, held the
     *        stutter, the left hand's height the filter sweep, the right hand's the gate depth.
     * @return the headset
     */
    frame::Headset &headset() { return m_headset; }
    /** @brief Standalone: stops and rewinds to bar 0. */
    void stop();
    /**
     * @brief Standalone: jumps to a bar.
     * @param[in] bar  the bar
     */
    void seekToBar( int bar );
    /**
     * @brief Whether the standalone clock is running.
     * @return true while the clock runs
     */
    bool isPlaying() const { return m_playRequest.load( std::memory_order_relaxed ); }
    /**
     * @brief The standalone's Ableton Link, as the settings menu says it: off, alone, or how many apps are with it.
     * @return the Link's state as text
     */
    juce::String linkStatus() const;
    /**
     * @brief The family jam, as the settings menu says it (Jam.h): off, leading, or whom it follows.
     * @return the jam's state as text
     */
    juce::String jamStatus() const { return m_jam.status(); }
    /**
     * @brief A snapshot of where we are, for the editor.
     * @return the snapshot
     */
    TransportView transport() const;
    /**
     * @brief Whether the plugin takes tempo and position from the host.
     * @return true when the host rules
     */
    bool followsHost() const { return m_followHost; }
    /**
     * @brief Turns host sync off (then the standalone clock runs even inside a host).
     * @param[in] on  true to follow the host
     */
    void setFollowHost( bool on );

    // ------------------------------------------------------------------ the cue bridge (PLAN 8.3)
    /**
     * @brief Where the cues go: a host name or address (the port is the parameter `cue.port`).
     *
     * Not a parameter, because a parameter is a float and an address is not. It is saved with the
     * state, and `PHOS_CUE_HOST` overrides it for an automated run.
     * @return the host name or address
     */
    juce::String cueHost() const;
    /**
     * @brief Sets the destination; takes effect at the next timer tick.
     * @param[in] host  name or address
     */
    void setCueHost( const juce::String &host );
    /**
     * @brief Whether the sender thread is running (the switch is `cue.send`).
     * @return true while it runs
     */
    bool cueRunning() const { return m_cues.running(); }
    /**
     * @brief Datagrams sent since the bridge was switched on, for the editor and the tests.
     * @return the datagrams sent
     */
    juce::uint64 cuesSent() const { return static_cast<juce::uint64>( m_cues.sent() ); }
    /**
     * @brief Cues lost to a full queue, which is the only way one can be lost.
     * @return the cues lost
     */
    juce::uint64 cuesDropped() const { return static_cast<juce::uint64>( m_cues.dropped() ); }
    /**
     * @brief The sender, for the offline demo and the tests.
     * @return the sender
     */
    phos::CueSender &cueSender() { return m_cues; }
    /**
     * @brief Opens or closes the bridge to match `cue.send` and `cue.port` (message thread).
     *
     * Called from the timer; public because a test has no message loop and has to turn the crank
     * itself, the same reason #serviceMacros is public.
     * @pre called on the message thread
     */
    void serviceCueBridge();

    /**
     * @brief Output muted; forced on by `PHOS_MUTE=1`.
     * @return true while muted
     */
    bool muted() const { return m_mute.load( std::memory_order_relaxed ); }
    /**
     * @brief Mutes or unmutes; does nothing while `PHOS_MUTE` forces it.
     * @param[in] on  true to mute
     */
    void setMuted( bool on )
    {
        if ( !m_forceMute ) m_mute.store( on, std::memory_order_relaxed );
    }
    /**
     * @brief True when `PHOS_MUTE=1` was set, so the editor can say so and disable the switch.
     * @return true when the environment forces the mute
     */
    bool muteForced() const { return m_forceMute; }

    // ------------------------------------------------------------------ export
    /**
     * @brief Writes the score of the first @p bars bars as a Standard MIDI File (message thread).
     * @param[in] file  the MIDI file
     * @param[in] bars  the bars
     * @return false if the file cannot be written
     * @pre called on the message thread
     */
    bool exportMidi( const juce::File &file, int bars );
    /**
     * @brief Writes every parameter and the seed as text (`.phosset`, message thread).
     * @param[in] file  the file
     * @return false if it cannot be written
     */
    bool exportSet( const juce::File &file );
    /**
     * @brief Reads such a file back.
     * @param[in] file  the file
     * @return false if it cannot be read
     */
    bool importSet( const juce::File &file );
    /**
     * @brief Reads a set from its text (a file's, a factory set's), as importSet does.
     * @param[in] text  the set's text
     * @return false if it is no set
     */
    bool importSetText( const std::string &text );
    /**
     * @brief Loads one of the factory's sets (phos/FactorySets.h), as importSet does.
     * @param[in] index  the set
     * @return false when there is none of that index
     */
    bool loadFactorySet( int index );

    /**
     * @brief Standalone: records the output to a 32-bit float WAV.
     * @param[in] file  the WAV file
     * @return false if it cannot be opened
     */
    bool startRecording( const juce::File &file );
    /** @brief Standalone: stops the recording and closes the file. */
    void stopRecording();
    /**
     * @brief Standalone: whether the output is being recorded.
     * @return true while recording
     */
    bool isRecording() const { return m_recording.load( std::memory_order_relaxed ); }
    /**
     * @brief Seconds recorded so far.
     * @return the seconds
     */
    double recordedSeconds() const
    {
        return static_cast<double>( m_recordedSamples.load( std::memory_order_relaxed ) ) /
               juce::jmax( 1.0, m_sampleRate );
    }

private:
    /** @brief Makes one host parameter per entry of the store, named from its module and descriptor. */
    void buildParameters();
    /** @brief The composer thread's body: service the handshake, pump, plan ahead, sleep 2 ms. */
    void composerLoop();
    /**
     * @brief One step of the composer's side of the handshake.
     * @param[in] warmUp  also plan a track ahead for the editor's list; only the composer thread does that (the audio
     *                    thread calls this offline, where a probe render would stall the block)
     */
    void serviceComposer( bool warmUp );
    /**
     * @brief Takes the live knobs over into m_composeParams, the plan knobs by their own rule (under
     *        m_composeLock: the composer thread and the MIDI export; 25.09.2026).
     * @param[in] restart  a restart is being positioned, or a user action wants the knobs as they stand: every knob is
     *                     taken over at once
     */
    void refreshComposeParams( bool restart );
    /**
     * @brief Asks for a restart at @p bar with engine beat 0 meaning bar @p bar (audio thread).
     * @param[in] bar         the bar
     * @param[in] beatOffset  the musical beat of engine beat 0
     * @pre called on the audio thread
     */
    void requestSeek( int bar, double beatOffset );
    /**
     * @brief Writes the notes of this block into the host's MIDI buffer, with their note-offs.
     * @param[in,out] midi            the host's buffer
     * @param[in]     beatAtStart     engine beat at sample 0 of the block
     * @param[in]     beatsPerSample  how fast the block runs
     * @param[in]     n               samples in the block
     */
    void emitMidi( juce::MidiBuffer &midi, double beatAtStart, double beatsPerSample, int n );
    /**
     * @brief The seed, the host-sync switch and every parameter value, as XML.
     * @param[out] dest  the state
     */
    void writeStateTo( juce::MemoryBlock &dest ) const;
    /**
     * @brief Composer thread: applies whatever the editor has asked for since the last turn.
     * @return true when anything was applied
     * @pre called on the composer thread
     */
    bool drainCuration();
    /**
     * @brief Message thread: copies the composer's locks into the mirror (after reading a file).
     * @pre called on the message thread
     */
    void syncCurationMirror();
    /**
     * @brief Message thread: remembers where the play head is, so a restart lands there again.
     * @pre called on the message thread
     */
    void markCurrentPosition();
    /**
     * @brief Message thread: the limiter's lookahead is a switchable latency; tell the host when it moves.
     * @pre called on the message thread
     */
    void timerCallback() override;
    /**
     * the hands' events (message thread, the timer)
     * @pre called on the message thread
     */
    void pollHeadset();
    void bindDefaultControllers();      ///< CC 74, 11 and 64 on the macros, as every generator has them
    frame::Headset   m_headset;         ///< the Quest's hands
    std::atomic<int> m_restarts{ 0 };   ///< restartCount

    std::unique_ptr<phos::Engine>   m_engine;      ///< the engine the audio thread renders
    std::unique_ptr<phos::Composer> m_composer;    ///< the composer (its thread plans, the conductor reads)
    std::unique_ptr<PlugConductor>  m_conductor;   ///< keeps the engine's rings filled from the composer
    /**
     * @brief Guards the composer and the conductor.
     *
     * The composer thread holds it while it plans and composes -- which takes seconds the first time
     * a track is planned, because a track's level match is measured by rendering it. Nothing the
     * host or the user waits on ever takes this lock: prepareToPlay takes `m_engineLock` instead, and
     * the editor reads the published copy of the plans (#m_plans). Only the exports, which are user
     * actions with a file dialog in front of them, block on it.
     */
    mutable std::mutex m_composeLock;
    /** @brief Guards the engine's structure: prepareToPlay against the composer's pushes. Held briefly. */
    mutable std::mutex m_engineLock;
    /** @brief The track plans the composer has published, for the editor. */
    mutable std::mutex           m_plansLock;
    std::vector<phos::TrackPlan> m_plans;   ///< the published copy; the editor reads only this
    /**
     * @brief A seek was asked for while the music was sounding: fade this block out, then seek.
     *
     * Audio thread only (22.09.2026). See the comment at the restart branch in processBlock: the
     * request has to be made after the block is rendered, or the block the click lands in comes out
     * silent from whatever level the waveform was at.
     */
    bool m_fadeOutThenSeek = false;

    static constexpr int PublishedTracks = 24;          ///< how many tracks the warm-up plans ahead
    int                  m_publishedTracks = 0;         ///< composer thread: how far the warm-up has got
    std::atomic<bool>    m_plansStale{ true };          ///< the seed or the knobs changed; plan again
    float                m_composeFingerprint = 0.0f;   ///< message thread: watches the composer's knobs for a change
    /**
     * @brief The knobs as the composer sees them (25.09.2026): a copy of the live store, taken at the start of each
     *        composer turn under m_composeLock, so that one turn plans and composes from one set of knobs while the
     *        host's automation and the editor go on writing the live store.
     *
     * The knobs the plans depend on (phos::Composer::planKnobIds) follow a rule of their own, because a change of
     * one throws every plan away and planning a track renders probes for seconds. They are taken over at a restart
     * (a seek plans anyway), at once while the transport stands, and while it runs only once they have held still
     * for PlanKnobSettleBeats beats of the music; the bars already composed keep the plans they were written
     * with, so a change is heard a few bars later, on a bar line. Until this date a knob automated on every block
     * threw the plans away on every bar -- in the host test, a seek re-planned the set from its first track for
     * 1 h 46 min and then crashed (docs/rounds/2026-09.md, 25.09.2026). Now the set plays on under the plans it
     * has and takes the value over when the automation rests. The same rule offline, in musical time, so a
     * bounce comes out the same every time.
     */
    phos::ParamStore   m_composeParams;
    std::vector<int>   m_planKnobIds;     ///< phos::Composer::planKnobIds of the store
    std::vector<float> m_planKnobsSeen;   ///< composer thread: the plan knobs as the live store last held them
    double             m_planKnobsSeenBeat = 0.0;   ///< under m_composeLock: the musical beat at which they last moved
    static constexpr double PlanKnobSettleBeats =
        4.0;                                ///< how long a plan knob holds still before it is taken over: a bar
    std::vector<StoreParameter *> m_byId;   ///< host parameters by global id, for the editor

    double                m_sampleRate = 48000.0;   ///< the host's rate
    std::atomic<uint64_t> m_seed{ 1 };              ///< the set seed
    /** @brief Stereo scratch for hosts (or devices) that hand over fewer than two channels. */
    juce::AudioBuffer<float> m_scratch;

    // ---- the restart handshake (see the file comment)
    std::atomic<int>    m_genWanted{ 0 };            ///< audio thread: bumped to ask for a restart
    std::atomic<int>    m_genAck{ 0 };               ///< composer: positioned and no longer pushing
    std::atomic<int>    m_genReset{ 0 };             ///< audio thread: the engine has been reset for that generation
    std::atomic<int>    m_genPrimed{ 0 };            ///< composer: the rings hold the first bars of that generation
    std::atomic<int>    m_pendingBar{ 0 };           ///< bar the restart lands on
    std::atomic<double> m_pendingOffset{ 0.0 };      ///< musical beat of engine beat 0 after the restart
    int                 m_genLocal = 0;              ///< the composer thread's view of the generation
    std::atomic<bool>   m_restartRequest{ false };   ///< message thread asks for a restart at the next block
    std::atomic<bool>   m_hostSyncNow{ false };      ///< the last block followed a host playhead
    std::atomic<double> m_hostBpm{ 0.0 };            ///< tempo the host reported, 0 = none
    // Ableton Link in the standalone (02.10.2026, LinkClock.h; Settings > Ableton Link).
    frame::LinkClock  m_link;                     ///< the session (joined on the timer when the setting is on)
    std::atomic<bool> m_linkFollowing{ false };   ///< other apps are in the session: its tempo and bar phase rule
    bool              m_linkPlayed = false;   ///< audio thread: the transport last told to or taken from the session
    double            m_linkBeat = 0.0;       ///< audio thread: the session's beat at this block (its bar phase)
    // The family jam (02.10.2026, Jam.h; Settings > Family jam).
    /**
     * @brief Audio thread: as a follower, the engine takes the leader's root (on a bar line) and its breaks.
     * @param[in] musicalBefore  the block's first musical beat
     * @param[in] shared         the shared clock's beat
     * @pre called on the audio thread
     */
    void jamFollow( double musicalBefore, double shared );
    /**
     * @brief Audio thread: as the leader, the section marks the cue tap has passed go out, stamped with their bar.
     * @param[in] musicalBefore  the block's first musical beat
     * @param[in] shared         the shared clock's beat
     * @pre called on the audio thread
     */
    void jamLead( double musicalBefore, double shared );
    /**
     * @brief The scale of compose.scale a leader's mode names (Jam.h): its own name, or the family's other names for
     *        it (a minor is an Aeolian, so is a minor pentatonic or a hexachord); -1 for a mode Phosphene has not (a
     *        major one) -- the walk keeps its own.
     * @param[in] mode  the mode's name
     * @return the scale's value, -1 for none
     */
    static int scaleOfMode( const juce::String &mode );
    frame::JamBus      m_jam{ "Phosphene" };       ///< the family jam's bus
    bool               m_jamLeadStarted = false;   ///< audio thread: the leader has said where it stands
    uint32_t           m_jamMarksSeen = 0;         ///< audio thread: the cue tap's marks already sent
    double             m_jamLastBefore = -1.0;     ///< audio thread: the previous block's musical beat (its length)
    int                m_jamTranspose = 0;         ///< audio thread: the transposition the engine plays now
    std::atomic<int>   m_jamTransposeOut{ 0 };     ///< the same, for the message thread
    std::atomic<float> m_jamEnergyOut{ -1.0f };    ///< the leader's energy for the message thread (-1: none)
    bool               m_jamFollowing = false;     ///< audio thread: the engine follows the jam (setJam was called)
    std::atomic<bool>  m_jamWalkHold{ false };     ///< message thread to composer thread: the set's walk holds its key
    std::atomic<int>   m_jamWalkScale{
        -1
    };   ///< message thread to composer thread: the leader's mode as a scale, -1 none
    float        m_jamSweep = 0.0f;   ///< message thread: the filter sweep the jam's energy asks for, glided
    juce::String m_jamLogged;         ///< the last line written to FAMILY_JAM_LOG

    std::thread m_composerThread;   ///< plans, composes and measures behind the audio thread
    /**
     * @brief A measurement running behind the music (23.09.2026, round "Planung"): a copy of the composer measures
     *        the playing track's deferred probes on a thread of its own, so the composer thread stays free for
     *        seeks and pumps. Started and taken over by serviceComposer(); ended at once by stopMeasureJob().
     */
    struct MeasureJob
    {
        std::unique_ptr<phos::Composer> m_composer;         ///< the copy, with the abort flag set
        phos::ParamStore                m_params;           ///< the knobs as they stood when it started
        int                             m_index = -1;       ///< the track it measures
        uint64_t                        m_generation = 0;   ///< Composer::planGeneration() when it started
        phos::TrackPlan                 m_result;           ///< the measured plan, once done
        std::atomic<bool>               m_done{ false };    ///< result is complete
        std::atomic<bool>               m_abort{ false };   ///< stop the probes (Composer::setAbortFlag)
    };
    std::unique_ptr<MeasureJob> m_measureJob;      ///< composer thread only
    std::thread                 m_measureThread;   ///< the job's thread
    /**
     * @brief Starts the job for track @p index (composer thread, under m_composeLock).
     * @param[in] index  the track
     */
    void startMeasureJob( int index );
    /** @brief Aborts a running job and waits for it (a few milliseconds: the probes stop at their next block). */
    void stopMeasureJob();
    std::atomic<bool> m_composerRun{ false };   ///< false asks the composer thread to end
    std::atomic<bool> m_offline{ false };       ///< non-realtime: the composer thread idles

    // ---- transport
    std::atomic<bool>   m_playRequest{ false };       ///< the standalone's play button
    bool                m_wasPlaying = false;         ///< audio thread: whether the last block played
    bool                m_followHost = true;          ///< message thread's copy, for the state and the editor
    std::atomic<bool>   m_followHostAtomic{ true };   ///< the same, as the audio thread reads it
    std::atomic<double> m_musicalBeat{ 0.0 };         ///< where we are in the set, for the editor
    std::atomic<double> m_bpmNow{ 145.0 };            ///< the tempo actually being played

    // ---- MIDI out
    phos::EventRing<phos::NoteEvent> m_midiRing{ 4096 };   ///< composer to audio thread, for the MIDI out
    /** @brief A note waiting for its note-off, in engine beats. */
    struct HeldNote
    {
        double m_endBeat = 0.0;    ///< when its note-off is due, engine beats
        int    m_channel = 0;      ///< its MIDI channel
        int    m_pitch = 0;        ///< its MIDI note
        bool   m_active = false;   ///< it sounds
    };
    static constexpr int MaxHeld = 128;     ///< notes that may sound at once on the MIDI output
    HeldNote             m_held[MaxHeld];   ///< audio thread only

    // ---- MIDI in: a keyboard on one voice (23.09.2026, phos::Engine::liveNoteOn)
    /** @brief A note message of the incoming block, kept past the buffer's clear(). */
    struct LiveNote
    {
        int  m_at = 0;         ///< its sample in the block
        int  m_pitch = 0;      ///< MIDI note
        int  m_velocity = 0;   ///< 1..127
        int  m_channel = 0;    ///< 0..15
        bool m_on = false;     ///< a note-on (else a note-off)
        int  m_part = -1;   ///< the voice it names (mix.keyboard_part's values; the split), -1 for the keyboard's own
    };
    static constexpr int MaxLive =
        256;   ///< note messages per block; more are dropped (a controller flood, not a player)
    LiveNote         m_live[MaxLive];          ///< audio thread only
    int              m_keyboardPartSeen = 0;   ///< audio thread: mix.keyboard_part as the last block had it
    frame::KeyMemory m_keyMemory;   ///< audio thread: where each held key went (the split, Scale Lock; 02.10.2026)
    // The stems as outputs of their own (02.10.2026): a stereo bus per part and one for the returns (phos::StemNames),
    // off until the host switches one on; then the engine writes its StemTap, offset to every render of a split block.
    std::vector<float>                  m_stemBuf;         ///< NumStems x 2 x block (prepareToPlay)
    std::array<float *, phos::NumStems> m_stemL{};         ///< per stem: its left channel in m_stemBuf
    std::array<float *, phos::NumStems> m_stemR{};         ///< per stem: its right channel in m_stemBuf
    phos::StemTap                       m_stemTap;         ///< the engine's view of them, moved to each render's start
    int                                 m_stemBlock = 0;   ///< the block the stems' buffers hold
    bool                                m_stemsOn = false;   ///< audio thread: the engine writes the stems now
    /**
     * @brief The buses: the main output, then one stereo output per part and one for the returns, those off.
     * @return the buses
     */
    static BusesProperties busLayout();

    // ---- the effects page's audition and the mixer's meters (24.09.2026)
    std::atomic<int> m_sfxPreview{ -1 };   ///< choice << 12 | preset, -1 none (previewSfx)
    std::array<std::atomic<float>, phos::NumParts>
        m_meterPeak{};   ///< audio thread raises, the editor takes (exchange 0)
    std::array<std::atomic<double>, phos::NumParts> m_meterSum{};   ///< sums of squares since the editor last took them
    std::atomic<int>                                m_meterCount{ 0 };   ///< the samples m_meterSum covers
    std::unique_ptr<std::atomic<float>[]> m_played;   ///< playedValue(): the audio thread stores after every block
    int                                   m_playedCount = 0;   ///< its length (the store's parameter count)
    std::vector<float>                    m_playedScratch;     ///< the audio thread's copy before the stores

    // ---- curation: the editor asks, the composer thread does it
    /** @brief One thing the editor wants done to a lockable unit. */
    struct CurationCommand
    {
        uint8_t m_unit = 0;   ///< phos::LockUnit
        uint8_t m_op =
            0;   ///< 0 unlock, 1 lock, 2 reroll, 3 clear everything, 4 set the reroll counter to value (undo)
        int32_t  m_index = 0;   ///< unit index (Composer::sectionUnitIndex and friends)
        uint32_t m_value = 0;   ///< op 4: the counter
    };
    phos::EventRing<CurationCommand> m_curation{ 64 };                        ///< message thread -> composer thread
    mutable std::mutex               m_curationLock;                          ///< guards the two mirrors below
    std::map<int, uint8_t>           m_lockMirror[phos::NumLockUnits];        ///< what the editor draws
    std::map<int, uint32_t>          m_variationMirror[phos::NumLockUnits];   ///< @copydoc m_lockMirror

    // ---- perform macros (message thread only)
    /** @brief One macro's position and, for a momentary one, when it ends. */
    struct MacroState
    {
        float  m_value = 0.0f;         ///< where it stands; 0 is neutral
        double m_releaseBeat = -1.0;   ///< musical beat at which it lets go, < 0 = not timed
    };
    MacroState m_macro[NumMacros];   ///< the perform macros
    /**
     * @brief The knob values the macros found, by parameter id, in their real units.
     *
     * A macro writes knobs, so it has to be able to put them back: the first macro to touch a
     * parameter records what was there, the last one to let go writes it back. Without this a
     * filter sweep would leave the cutoff wherever the hand stopped. The real value is kept rather
     * than a normalised position, because a round trip through a logarithmic mapping is not exact.
     */
    std::map<int, float> m_macroBase;
    /**
     * @brief Guards `m_macroBase`: serviceMacros() runs on the message thread, a host may ask for the state from
     *        another (23.09.2026: the state stores a macro-held knob at its base, writeStateTo).
     */
    mutable std::mutex m_macroBaseLock;
    unsigned           m_macroTicks = 0;   ///< message thread: how many ticks since the slow jobs last ran
    /** @brief The macros as host parameters (23.09.2026): automatable and MIDI-learnable like every knob. */
    juce::RangedAudioParameter *m_macroParam[NumMacros] = {};
    /** @brief The value each macro's host parameter had when serviceMacros() last looked (message thread). */
    float m_macroHostSeen[NumMacros] = {};
    /** @brief Controller -> target (MIDI learn). */
    phos::MidiMap m_midiMap;

    // ---- undo (message thread)
    /** @brief Nothing: a value alone is no step (the gestures are). */
    void parameterValueChanged( int, float ) override {}
    /**
     * @brief A knob gesture from the editor: its start takes the before-state, its end records the step.
     * @param[in] parameterIndex     the host parameter's index
     * @param[in] gestureIsStarting  true when the gesture begins
     */
    void parameterGestureChanged( int parameterIndex, bool gestureIsStarting ) override;
    /**
     * @name The composer's presets on the knobs (26.09.2026)
     * The user: "Stellt der Composer auch die Preset-Werte auf Absolutwerte ... ein?" The engine plays a track's preset
     * absolute (ControlEvent::Kind::Base); refreshPresetDisplay() tells the editor which preset each synth whose Own
     * Sound is off plays, and the editor shows its values on the knobs without writing them (the state stays the
     * user's: vst3test reads it back byte for byte). A gesture on such a synth's knob takes the sound over: the knobs
     * get the preset's values (takeOverPreset; not the plan knobs, Composer::planKnobIds, whose change would throw the
     * plans away) and Own Sound goes on, all in the gesture's undo step (parameterGestureChanged).
     * @{ */
    void refreshPresetDisplay();
    /**
     * @brief Writes the shown preset of synth @p k into its knobs (a take-over; message thread).
     * @param[in] k  the synth (SoundSynths order)
     */
    void takeOverPreset( int k );
    int m_shownPreset[phos::SoundSynths] = {
        -2, -2, -2, -2, -2, -2, -2, -2, -2
    };   ///< the preset each synth's knobs show, -1 none, -2 not yet
    std::atomic<int> m_shownIndex[phos::SoundSynths];   ///< the same, for the editor (any thread)
    /**
     * @brief The synth (SoundSynths order) a global parameter id belongs to, and its index in the module; -1 for none.
     * @param[in]  id     the global id
     * @param[out] local  its index in the module
     * @return the synth, or -1
     */
    int soundSynthOfId( int id, int &local ) const;
    /** @} */
public:
    /**
     * @brief The preset the composer plays on synth @p synth (SoundSynths order) and its knobs show; -1 none (the
     *        editor).
     * @param[in] synth  the synth (SoundSynths order)
     * @return the preset, or -1
     */
    int composerPreset( int synth ) const
    {
        return synth >= 0 && synth < phos::SoundSynths ? m_shownIndex[synth].load( std::memory_order_relaxed ) : -1;
    }

private:
    /**
     * @brief Records the step from @p before to now, unless nothing changed.
     * @param[in] name    the step's name
     * @param[in] before  the state before
     */
    void recordUndo( const juce::String &name, const UndoState &before );
    mutable juce::UndoManager          m_undo{ 32 * 1024 * 1024, 100 };   ///< undo and redo of whole states (UndoState)
    bool                               m_restoringUndo = false;           ///< applyUndoState is running: record nothing
    std::vector<std::pair<int, float>> m_previewSaved;       ///< the knobs a preview replaced (store id, real value)
    int                                m_gestureDepth = 0;   ///< gestures open at once (a two-knob drag is one step)
    UndoState                          m_gestureBefore;      ///< the state when the first open gesture began
    juce::String                       m_gestureName;        ///< the knob it began on

    // ---- the cue bridge (PLAN 8.3, Cue.h)
    /**
     * @brief The cues of a set, sent from the play position rather than from the composer.
     *
     * The chain is: the composer thread writes marks into #m_cueMarks bars ahead; processBlock hands
     * #m_cueTap the beat range the block covers and it pushes cues into the sender's queue with the
     * instant the listener will hear them; the sender's own thread waits for that instant and calls
     * sendto(). Nothing on the audio thread allocates, locks or touches a socket.
     */
    phos::CueSender    m_cues;
    phos::CueTap       m_cueTap;                            ///< @copydoc m_cues
    phos::CueMarkRingT m_cueMarks{ 256 };                   ///< @copydoc m_cues
    juce::String       m_cueHost{ phos::CueDefaultHost };   ///< message thread's copy of the destination
    juce::String       m_fieldFolder;         ///< setFieldFolder (message thread; saved in the state as "fieldFolder")
    mutable std::mutex m_cueHostLock;         ///< guards #m_cueHost (the editor writes, the timer reads)
    bool               m_cueWanted = false;   ///< message thread: what `cue.send` said at the last tick
    int                m_cuePort = phos::CueDefaultPort;    ///< message thread: what `cue.port` said at the last tick
    juce::String       m_cuePortHost;                       ///< message thread: the host the socket was opened with
    std::atomic<float> m_cueLeadMs{ 0.0f };                 ///< `cue.lead_ms`, as the audio thread reads it
    int                m_lastStateVersion = StateVersion;   ///< the version attribute of the last state that was read
    juce::String       m_legacyKnobs;            ///< an older state's knob text, held and not applied (20.09.2026)
    std::atomic<bool>  m_cueBeats{ true };       ///< `cue.beats`, as the audio thread reads it
    std::atomic<bool>  m_cueAnnounce{ false };   ///< the bridge just opened: say which section is playing

    // ---- mute and recording
    std::atomic<bool> m_mute{ false };       ///< the output is silenced at the very end of processBlock
    bool              m_forceMute = false;   ///< `PHOS_MUTE=1`: the switch is stuck on
    /** @brief `PHOS_TRACE=1`: processBlock reports the transport and the handshake on stderr. */
    bool                  m_trace = false;
    unsigned              m_traceCount = 0;                         ///< @copydoc m_trace
    juce::TimeSliceThread m_recordThread{ "Phosphene recorder" };   ///< writes the recording to disk
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter>
                             m_recordWriter;         ///< the recording's writer, null when none runs
    juce::CriticalSection    m_recordLock;           ///< guards m_recordWriter between the audio and the message thread
    std::atomic<bool>        m_recording{ false };   ///< a recording runs
    std::atomic<juce::int64> m_recordedSamples{ 0 };   ///< samples recorded so far

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR( PhospheneProcessor )
};
