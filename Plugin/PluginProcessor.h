/**
 * @file PluginProcessor.h
 * @brief The Phosphene plugin: the core's engine and composer under a JUCE processor.
 *
 * **Who owns time.** The engine's own clock always starts at beat 0 (phos::Engine has no way to be
 * placed anywhere else, and it should not have one: a deterministic render is a render from the
 * start). The plugin therefore keeps one number, #beatOffset(), the *musical* beat that the
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
 * takes the short #engineLock_, and the editor reads a published copy of the plans.
 *
 * **Tempo.** phos::Engine::setTempoMap is never used here. Building a map plans every track it
 * covers, so a half-hour set would begin with half a minute of silence; instead the conductor
 * writes each track's tempo as a control event on compose.bpm, which is how every other departure
 * from a knob already travels. Under a host, the host's tempo is written into compose.bpm directly
 * and no tempo events are produced at all.
 *
 * **Restarts are a handshake.** Everything that has to reposition the engine -- the transport
 * starting, a seek, a new seed -- goes through a three-step handshake between the audio thread and
 * the composer thread (see #genWanted_). The audio thread renders silence while it is in flight,
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
 * the knob's normalised domain and as an offset from the value it had (#macroBase_), which is the
 * language a phos::ControlEvent speaks; the composer's own offsets keep riding on top, and letting
 * go puts every knob back exactly where it was. See #Macro.
 *
 * **Environment.** `PHOS_MUTE=1` starts the output muted and the plugin never unmutes itself, so
 * tests and the screenshot export are silent. `PHOS_SHOT` is read by the editor (PluginEditor.h).
 */
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "phos/Composer.h"
#include "phos/Cue.h"
#include "phos/Engine.h"
#include "phos/Midi.h"
#include "phos/MidiMap.h"
#include "phos/Params.h"
#include "phos/Score.h"
#include <atomic>
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
 * from #macroTargets, so what the manual says is what the plugin does.
 */
enum class Macro : int {
    FilterSweep = 0,   ///< -1..+1: opens or closes the acid, lead, counter, arp and stab filters together
    GateDepth,         ///< 0..1: the trance gate of lead, counter, arp, stab and pad, on and as deep as this
    DropOut,           ///< momentary: kick and bass gone until the next bar line
    Stutter,           ///< held: lead, counter, arp, stab and pad chopped on sixteenths at full depth
    Count
};
constexpr int kNumMacros = static_cast<int>(Macro::Count);   ///< number of macros
extern const char* const kMacroNames[kNumMacros];            ///< display names
extern const char* const kMacroHelp[kNumMacros];             ///< one line each, for the manual and the tooltip
/** @brief Whether a macro is a momentary press (as opposed to a value that is held). */
bool macroIsMomentary(Macro m);

/**
 * @brief One host parameter, backed by one entry of phos::ParamStore.
 *
 * The store is the only place a value lives: getValue() reads it and setValue() writes it, both as
 * a relaxed atomic, so there is no second copy that could drift from the engine's. The normalised
 * mapping, the display text and the step count all come from the descriptor table, which is what
 * makes the whole parameter set -- roughly six hundred of them across the modules -- generated
 * rather than written out.
 */
class StoreParameter final : public juce::RangedAudioParameter {
public:
    /**
     * @brief Binds a parameter.
     * @param store the engine's parameter store (outlives this object)
     * @param id    global parameter id
     * @param name  display name shown by the host
     */
    StoreParameter(phos::ParamStore& store, int id, const juce::String& name);

    float getValue() const override;
    void  setValue(float newValue) override;
    float getDefaultValue() const override;
    juce::String getName(int maximumStringLength) const override;
    juce::String getLabel() const override;
    int  getNumSteps() const override;
    bool isDiscrete() const override;
    bool isBoolean() const override;
    juce::String getText(float normalisedValue, int maximumStringLength) const override;
    float getValueForText(const juce::String& text) const override;
    const juce::NormalisableRange<float>& getNormalisableRange() const override { return range_; }

    /** @brief The global id in the store. */
    int paramId() const { return id_; }

private:
    phos::ParamStore& store_;
    int id_;
    juce::String name_;
    juce::NormalisableRange<float> range_;
};

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
class PlugConductor {
public:
    /** @brief Binds an engine and a composer (both outlive this object). */
    PlugConductor(phos::Engine& engine, const phos::Composer& composer) : engine_(engine), composer_(composer) {}

    /**
     * @brief Repositions: the next bar to compose and the musical beat that engine beat 0 means.
     *
     * Also replays the sound state of the track the bar belongs to: the composer writes a track's
     * recipe as control events on its first bar, and a set that is joined in the middle would
     * otherwise play the following bars with the knobs alone. Up to #kCatchUpBars bars before the
     * landing point are composed for their control events only; the last value of each parameter is
     * pushed as an immediate change. A ramp that was in flight lands at its target, which is where
     * it was heading.
     */
    void seek(const phos::ParamStore& params, int startBar, double beatOffset, bool writeTempo,
              std::mutex& engineLock);
    /**
     * @brief Composes and pushes until @p horizonBeats beyond the engine's position are covered.
     * @param params       knob values to compose with
     * @param horizonBeats how far ahead to keep the rings filled
     * @param midiOut      if not null, every pushed note is also written here (engine beats)
     * @param engineLock   held while events are pushed, released while bars are composed
     */
    void pump(const phos::ParamStore& params, double horizonBeats, phos::EventRing<phos::NoteEvent>* midiOut,
              std::mutex& engineLock);
    /**
     * @brief Binds the ring the cue tap drains (PLAN 8.3); null switches mark generation off.
     *
     * Section boundaries and keys are decisions, not arithmetic, so the audio thread cannot work
     * them out for itself -- but they must not be *sent* from here either, because this runs bars
     * ahead of the sound. The conductor therefore writes what it knows into a ring in beat order,
     * and phos::CueTap pops a mark when the play position reaches it (Cue.h).
     */
    void setCueMarks(phos::CueMarkRing* marks) { marks_ = marks; }

    /** @brief Index of the next bar to be composed. */
    int nextBar() const { return nextBar_; }
    /** @brief The musical beat that engine beat 0 stands for. */
    double beatOffset() const { return beatOffset_; }
    /** @brief Nudges the offset by a fraction of a beat (host drift correction). */
    void nudgeOffset(double delta) { beatOffset_ += delta; }

    /**
     * @brief The notes of the bars last composed, for the editor's pattern preview.
     *
     * The conductor keeps the last #kPreviewBars bars it composed, in musical beats. Reading them
     * is a copy under a short lock -- the editor never asks the composer itself, which would mean
     * waiting for a probe render.
     * @param firstBar first bar wanted
     * @param bars     how many
     * @param out      receives the notes whose bar falls in that range
     * @return false if nothing of that range has been composed yet
     */
    bool readPreview(int firstBar, int bars, std::vector<phos::NoteEvent>& out) const;

    static constexpr int kCatchUpBars = 128;   ///< how far back seek() looks for a track's sound
    static constexpr int kPreviewBars = 24;    ///< bars kept for the editor's pattern preview

private:
    bool flush(phos::EventRing<phos::NoteEvent>* midiOut);

    /** @brief The tempo of the track that @p bar belongs to, as control events on compose.bpm. */
    void tempoControls(const phos::ParamStore& params, int bar, std::vector<phos::ControlEvent>& out) const;

    /**
     * @brief Pushes the cue marks of bar @p bar, in the order phos::Composer::sections() lists them.
     *
     * The same three rules that build a SectionMark there: the cut at the head of a breakdown, the
     * section itself, and the pre-drop break in the last bar of a buildup. Matching that function
     * exactly is what the offline demo checks -- the cues a listener receives and the score the
     * composer wrote have to be the same list of boundaries, bar for bar.
     * @param params knob values to plan with
     * @param bar    musical bar
     * @param first  true: also send the key, whatever the bar is (after a seek nothing is known yet)
     */
    void cueMarks(const phos::ParamStore& params, int bar, bool first);

    phos::Engine& engine_;
    const phos::Composer& composer_;
    int nextBar_ = 0;
    double beatOffset_ = 0.0;
    bool writeTempo_ = true;
    std::vector<phos::NoteEvent> notes_;
    std::vector<phos::ControlEvent> controls_;
    size_t notePos_ = 0, controlPos_ = 0;
    mutable std::mutex previewLock_;               ///< guards #preview_ (composer writes, editor reads)
    std::vector<phos::NoteEvent> preview_;         ///< the last kPreviewBars bars, in musical beats
    phos::CueMarkRing* marks_ = nullptr;           ///< where the cue marks go, null = the bridge is off
    int cueKey_ = -1;                              ///< the key the last mark carried, -1 = none sent yet
    int cueLandingBar_ = -1;                       ///< seek() already described this bar; pump must not repeat it
};

/** @brief What the editor needs to know about the transport, in one lump. */
struct TransportView {
    bool   playing = false;      ///< the engine is being rendered
    bool   hostSync = false;     ///< the clock is the host's playhead
    double musicalBeat = 0.0;    ///< beat from the start of the set
    double bpm = 145.0;          ///< tempo now
    int    bar = 0;              ///< bar from the start of the set
    int    track = 0;            ///< index of the track that bar belongs to
    int    barInTrack = 0;       ///< bar within that track
    bool   restarting = false;   ///< a handshake is in flight (silent)
};

/** @brief The Phosphene plugin processor. */
class PhospheneProcessor final : public juce::AudioProcessor, private juce::Timer {
public:
    PhospheneProcessor();
    ~PhospheneProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    /** @brief Offline: the audio thread takes the composer's work over, so a render is reproducible. */
    void setNonRealtime(bool offline) noexcept override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    /** @brief Since 23.09.2026 the plugin reads MIDI controllers (MIDI learn, phos/MidiMap.h); notes it ignores. */
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Set"; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;
    /**
     * @brief The version of the state this build writes.
     *
     * 2 since 19.09.2026 (the reordered voices, the counter-lead, the stab and the drone; docs/PLAN.md,
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
    static constexpr int kStateVersion = 3;
    /** @brief The version of the last state read by setStateInformation (1 for a state older than the voices round). */
    int lastStateVersion() const { return lastStateVersion_; }
    /**
     * @brief The knob text of a state older than kStateVersion that was **not** applied, or empty.
     *
     * A state of version 1 or 2 holds every value of its session and cannot say which of them the user
     * chose, so loading it would do exactly the damage this round is about. Such a state therefore does
     * not set a single knob: the engine starts at today's factory defaults and the text is kept here so
     * that the editor can offer it (PhospheneEditor's banner, adoptLegacyState()).
     */
    const juce::String& pendingLegacyState() const { return legacyKnobs_; }
    /** @brief Applies the knobs of the held older state after all, at the user's word. */
    void adoptLegacyState();
    /** @brief Forgets the held older state (the user kept the new defaults). */
    void dismissLegacyState() { legacyKnobs_.clear(); }
    /**
     * @brief Every knob back to its factory default -- the "reset to factory defaults" of the rule.
     *
     * The set itself (seed, locks, rerolls) is not a knob and is left alone: the user asked for the
     * *sound* to come back to the shipped calibration, not for their set to be thrown away.
     */
    void resetToFactoryDefaults();

    // ------------------------------------------------------------------ the set
    /** @brief The engine's parameters -- the single copy of every value. */
    phos::ParamStore& params() { return engine_->params(); }
    const phos::ParamStore& params() const { return engine_->params(); }
    /** @brief The engine, for the editor's meters. */
    const phos::Engine& engine() const { return *engine_; }
    /** @brief The host parameter for a global id. */
    StoreParameter* parameterFor(int id) const { return id >= 0 && id < static_cast<int>(byId_.size()) ? byId_[static_cast<size_t>(id)] : nullptr; }

    // ------------------------------------------------------------------ MIDI learn (23.09.2026, phos/MidiMap.h)
    /**
     * @name MIDI learn
     * A *target* is a host parameter: the store's ids first (0 .. params().count() - 1), then the four macros
     * (params().count() + Macro). Every one of them can take a controller, and every one of them is a host
     * parameter a DAW can automate.
     * @{ */
    /** @brief The controller table (arm/bind/unbind on the message thread). */
    phos::MidiMap& midiMap() { return midiMap_; }
    const phos::MidiMap& midiMap() const { return midiMap_; }
    /** @brief How many targets there are. */
    int midiTargetCount() const { return params().count() + kNumMacros; }
    /** @brief The host parameter behind a target, or null. */
    juce::RangedAudioParameter* hostParameter(int target) const;
    /** @brief A target's display name ("Lead Cutoff", "Macro Filter Sweep"). */
    juce::String midiTargetName(int target) const;
    /** @brief A target's stable key for the saved map ("lead.cutoff", "macro.filter_sweep"). */
    std::string midiTargetKey(int target) const;
    /** @brief The target with key @p key, or -1. */
    int midiTargetFind(const std::string& key) const;
    /** @} */

    /**
     * @brief What became of the shipped wavetable pack in this process.
     *
     * The library is loaded once for the whole process, by whichever Engine::prepare() runs first,
     * so this says nothing about *this* instance -- it says what every instance has. The editor
     * draws it (a library table whose file is missing is marked in the chooser) and the host test
     * reads it, because "it fell back silently" is exactly the failure that has to be visible.
     */
    struct WaveTableLibrary {
        juce::String directory;   ///< where the plugin pointed the core; empty when it found no pack
        int shipped = 0;          ///< library tables this build knows about
        int loaded = 0;           ///< of those, the ones the pack really holds (indexed at load)
        /**
         * @brief Of those, the ones expanded into mip levels right now.
         *
         * Since 22.09.2026 a table is expanded when a track that uses it is planned, not at load
         * (WaveTableFile.h): 464 tables would be 935 MB. So this is a number about what has been
         * *played*, and it is small and rises; `loaded` is the one that says the installation is
         * complete.
         */
        int built = 0;
    };
    /** @brief The state of the library now; counted freshly, so it is valid after prepareToPlay(). */
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
    struct LearnedModels {
        juce::String directory;    ///< where the plugin found the two files; empty when it found none
        bool melody = false;       ///< the learned melody model really loaded (else: the Markov model)
        bool bass = false;         ///< the learned bass role really loaded (else: the pattern families)
        double melodyNll = 0.0;    ///< held-out nats per token the melody file declares
        double bassNll = 0.0;      ///< the same, out of the bass file
        juce::String melodyNote;   ///< the core's own message when the melody model did not load
        juce::String bassNote;     ///< the same for the bass model
        /**
         * @brief A model loaded although nothing was installed beside the binary.
         *
         * Only a development build can reach this: there PhospheneCore still carries
         * PHOS_SOURCE_DATA_DIR and therefore finds Core/data on the machine that compiled it
         * (Core/CMakeLists.txt). It is shown, because a build that works only here looks from the
         * outside exactly like one that works everywhere.
         */
        bool fromSourceTree = false;
    };
    /** @brief What the two models are in this process; settled once, before the first engine. */
    LearnedModels learnedModels() const;

    /** @brief The set seed. */
    uint64_t seed() const { return seed_.load(std::memory_order_relaxed); }
    /** @brief Chooses another set (message thread); restarts playback from the top. */
    void setSeed(uint64_t s);
    /** @brief A new seed from the clock. */
    void randomiseSeed();

    /**
     * @brief Copies out the plan of track @p index as the composer thread last published it.
     * @return false if that track has not been planned yet (the editor then shows what it has)
     */
    bool tryReadTrack(int index, phos::TrackPlan& out) const;
    /**
     * @brief The notes of @p bars bars from @p firstBar, for the editor's pattern preview.
     * @return false if the conductor has not composed that range (the display then keeps its own)
     */
    bool readPattern(int firstBar, int bars, std::vector<phos::NoteEvent>& out) const
    { return conductor_->readPreview(firstBar, bars, out); }
    /** @brief Asks the composer to re-plan: the knobs that shape the set have changed. */
    void requestRestart() { plansStale_.store(true, std::memory_order_release); restartRequest_.store(true, std::memory_order_release); }

    // ------------------------------------------------------------------ curation (PLAN 6.8)
    /**
     * @brief Locks or unlocks a unit (message thread).
     *
     * The command travels to the composer thread through #curation_; what the editor draws comes
     * from the mirror this keeps, so a lock toggles under the mouse even while a track is being
     * planned. A locked unit is frozen on the seed it had, so rerolling anything around it leaves
     * it bit-identical.
     */
    void setLock(phos::LockUnit unit, int index, bool locked);
    /** @brief Whether a unit is locked, from the editor's mirror. */
    bool isLocked(phos::LockUnit unit, int index) const;
    /** @brief Draws that unit again from a fresh seed; a locked unit does not move. */
    void reroll(phos::LockUnit unit, int index);
    /** @brief How often a unit has been rerolled (0 = never), from the mirror. */
    uint32_t variation(phos::LockUnit unit, int index) const;
    /** @brief Forgets every lock and every reroll. */
    void clearCuration();

    // ------------------------------------------------------------------ perform macros
    /**
     * @brief Moves a macro (message thread).
     * @param m     which macro
     * @param value 0 is neutral for all of them; #Macro::FilterSweep runs -1..+1, the rest 0..1
     */
    void setMacro(Macro m, float value);
    /** @brief Where a macro stands. */
    float macroValue(Macro m) const { return macro_[static_cast<size_t>(m)].value; }
    /**
     * @brief The macro tick (message thread): applies what is held and releases what has run out.
     *
     * Called by #timerCallback at about 30 Hz. It is public because a test has no message loop and
     * has to turn the crank itself.
     */
    void serviceMacros();
    /** @brief The parameters one macro moves and how far, for the editor and the manual generator. */
    struct MacroTarget {
        int   param = -1;      ///< global parameter id
        float value = 0.0f;    ///< normalised delta, or the absolute normalised value
        bool  absolute = false;   ///< true: set the knob to @ref value instead of adding it
    };
    /**
     * @brief What macro @p m at @p value wants of the parameters.
     * @param m     the macro
     * @param value its position
     * @param out   receives at most #kMaxMacroTargets entries
     * @return how many were written
     */
    int macroTargets(Macro m, float value, MacroTarget* out) const;
    static constexpr int kMaxMacroTargets = 24;   ///< most parameters one macro touches (Stutter: four each on five voices)

    // ------------------------------------------------------------------ transport
    /** @brief Standalone: starts at the top (or resumes after a pause). */
    void play();
    /** @brief Standalone: stops and rewinds to bar 0. */
    void stop();
    /** @brief Standalone: jumps to a bar. */
    void seekToBar(int bar);
    /** @brief Whether the standalone clock is running. */
    bool isPlaying() const { return playRequest_.load(std::memory_order_relaxed); }
    /** @brief A snapshot of where we are, for the editor. */
    TransportView transport() const;
    /** @brief Whether the plugin takes tempo and position from the host. */
    bool followsHost() const { return followHost_; }
    /** @brief Turns host sync off (then the standalone clock runs even inside a host). */
    void setFollowHost(bool on);

    // ------------------------------------------------------------------ the cue bridge (PLAN 8.3)
    /**
     * @brief Where the cues go: a host name or address (the port is the parameter `cue.port`).
     *
     * Not a parameter, because a parameter is a float and an address is not. It is saved with the
     * state, and `PHOS_CUE_HOST` overrides it for an automated run.
     */
    juce::String cueHost() const;
    /** @brief Sets the destination; takes effect at the next timer tick. @param host name or address */
    void setCueHost(const juce::String& host);
    /** @brief Whether the sender thread is running (the switch is `cue.send`). */
    bool cueRunning() const { return cues_.running(); }
    /** @brief Datagrams sent since the bridge was switched on, for the editor and the tests. */
    juce::uint64 cuesSent() const { return static_cast<juce::uint64>(cues_.sent()); }
    /** @brief Cues lost to a full queue, which is the only way one can be lost. */
    juce::uint64 cuesDropped() const { return static_cast<juce::uint64>(cues_.dropped()); }
    /** @brief The sender, for the offline demo and the tests. */
    phos::CueSender& cueSender() { return cues_; }
    /**
     * @brief Opens or closes the bridge to match `cue.send` and `cue.port` (message thread).
     *
     * Called from the timer; public because a test has no message loop and has to turn the crank
     * itself, the same reason #serviceMacros is public.
     */
    void serviceCueBridge();

    /** @brief Output muted; forced on by `PHOS_MUTE=1`. */
    bool muted() const { return mute_.load(std::memory_order_relaxed); }
    /** @brief Mutes or unmutes; does nothing while `PHOS_MUTE` forces it. */
    void setMuted(bool on) { if (!forceMute_) mute_.store(on, std::memory_order_relaxed); }
    /** @brief True when `PHOS_MUTE=1` was set, so the editor can say so and disable the switch. */
    bool muteForced() const { return forceMute_; }

    // ------------------------------------------------------------------ export
    /**
     * @brief Writes the score of the first @p bars bars as a Standard MIDI File (message thread).
     * @return false if the file cannot be written
     */
    bool exportMidi(const juce::File& file, int bars);
    /** @brief Writes every parameter and the seed as text (`.phosset`, message thread). */
    bool exportSet(const juce::File& file);
    /** @brief Reads such a file back. */
    bool importSet(const juce::File& file);

    /** @brief Standalone: records the output to a 32-bit float WAV. */
    bool startRecording(const juce::File& file);
    void stopRecording();                                         ///< @copydoc startRecording
    bool isRecording() const { return recording_.load(std::memory_order_relaxed); }   ///< @copydoc startRecording
    /** @brief Seconds recorded so far. */
    double recordedSeconds() const { return static_cast<double>(recordedSamples_.load(std::memory_order_relaxed)) / juce::jmax(1.0, sampleRate_); }

private:
    /** @brief Makes one host parameter per entry of the store, named from its module and descriptor. */
    void buildParameters();
    /** @brief The composer thread's body: service the handshake, pump, plan ahead, sleep 2 ms. */
    void composerLoop();
    /**
     * @brief One step of the composer's side of the handshake.
     * @param warmUp also plan a track ahead for the editor's list; only the composer thread does that
     *               (the audio thread calls this offline, where a probe render would stall the block)
     */
    void serviceComposer(bool warmUp);
    /** @brief Asks for a restart at @p bar with engine beat 0 meaning bar @p bar (audio thread). */
    void requestSeek(int bar, double beatOffset);
    /**
      * @brief Writes the notes of this block into the host's MIDI buffer, with their note-offs.
      * @param midi          the host's buffer
      * @param beatAtStart   engine beat at sample 0 of the block
      * @param beatsPerSample how fast the block runs
      * @param n             samples in the block
      */
    void emitMidi(juce::MidiBuffer& midi, double beatAtStart, double beatsPerSample, int n);
    /** @brief The seed, the host-sync switch and every parameter value, as XML. */
    void writeStateTo(juce::MemoryBlock& dest) const;
    /** @brief Composer thread: applies whatever the editor has asked for since the last turn. */
    bool drainCuration();
    /** @brief Message thread: copies the composer's locks into the mirror (after reading a file). */
    void syncCurationMirror();
    /** @brief Message thread: remembers where the play head is, so a restart lands there again. */
    void markCurrentPosition();
    /** @brief Message thread: the limiter's lookahead is a switchable latency; tell the host when it moves. */
    void timerCallback() override;

    std::unique_ptr<phos::Engine> engine_;
    std::unique_ptr<phos::Composer> composer_;
    std::unique_ptr<PlugConductor> conductor_;
    /**
     * @brief Guards the composer and the conductor.
     *
     * The composer thread holds it while it plans and composes -- which takes seconds the first time
     * a track is planned, because a track's level match is measured by rendering it. Nothing the
     * host or the user waits on ever takes this lock: prepareToPlay takes #engineLock_ instead, and
     * the editor reads the published copy of the plans (#plans_). Only the exports, which are user
     * actions with a file dialog in front of them, block on it.
     */
    mutable std::mutex composeLock_;
    /** @brief Guards the engine's structure: prepareToPlay against the composer's pushes. Held briefly. */
    mutable std::mutex engineLock_;
    /** @brief The track plans the composer has published, for the editor. */
    mutable std::mutex plansLock_;
    std::vector<phos::TrackPlan> plans_;   ///< the published copy; the editor reads only this
    /**
     * @brief A seek was asked for while the music was sounding: fade this block out, then seek.
     *
     * Audio thread only (22.09.2026). See the comment at the restart branch in processBlock: the
     * request has to be made after the block is rendered, or the block the click lands in comes out
     * silent from whatever level the waveform was at.
     */
    bool fadeOutThenSeek_ = false;

    static constexpr int kPublishedTracks = 24;   ///< how many tracks the warm-up plans ahead
    int publishedTracks_ = 0;                  ///< composer thread: how far the warm-up has got
    std::atomic<bool> plansStale_{ true };     ///< the seed or the knobs changed; plan again
    float composeFingerprint_ = 0.0f;          ///< message thread: watches the composer's knobs for a change
    std::vector<StoreParameter*> byId_;   ///< host parameters by global id, for the editor

    double sampleRate_ = 48000.0;
    std::atomic<uint64_t> seed_{ 1 };
    /** @brief Stereo scratch for hosts (or devices) that hand over fewer than two channels. */
    juce::AudioBuffer<float> scratch_;

    // ---- the restart handshake (see the file comment)
    std::atomic<int> genWanted_{ 0 };   ///< audio thread: bumped to ask for a restart
    std::atomic<int> genAck_{ 0 };      ///< composer: positioned and no longer pushing
    std::atomic<int> genReset_{ 0 };    ///< audio thread: the engine has been reset for that generation
    std::atomic<int> genPrimed_{ 0 };   ///< composer: the rings hold the first bars of that generation
    std::atomic<int> pendingBar_{ 0 };      ///< bar the restart lands on
    std::atomic<double> pendingOffset_{ 0.0 };   ///< musical beat of engine beat 0 after the restart
    int genLocal_ = 0;                  ///< the composer thread's view of the generation
    std::atomic<bool> restartRequest_{ false };  ///< message thread asks for a restart at the next block
    std::atomic<bool> hostSyncNow_{ false };     ///< the last block followed a host playhead
    std::atomic<double> hostBpm_{ 0.0 };         ///< tempo the host reported, 0 = none

    std::thread composerThread_;
    std::atomic<bool> composerRun_{ false };   ///< false asks the composer thread to end
    std::atomic<bool> offline_{ false };   ///< non-realtime: the composer thread idles

    // ---- transport
    std::atomic<bool> playRequest_{ false };     ///< the standalone's play button
    bool  wasPlaying_ = false;                   ///< audio thread: whether the last block played
    bool  followHost_ = true;                    ///< message thread's copy, for the state and the editor
    std::atomic<bool> followHostAtomic_{ true }; ///< the same, as the audio thread reads it
    std::atomic<double> musicalBeat_{ 0.0 };     ///< where we are in the set, for the editor
    std::atomic<double> bpmNow_{ 145.0 };        ///< the tempo actually being played

    // ---- MIDI out
    phos::EventRing<phos::NoteEvent> midiRing_{ 4096 };   ///< composer to audio thread, for the MIDI out
    /** @brief A note waiting for its note-off, in engine beats. */
    struct HeldNote { double endBeat = 0.0; int channel = 0; int pitch = 0; bool active = false; };
    static constexpr int kMaxHeld = 128;   ///< notes that may sound at once on the MIDI output
    HeldNote held_[kMaxHeld];              ///< audio thread only

    // ---- curation: the editor asks, the composer thread does it
    /** @brief One thing the editor wants done to a lockable unit. */
    struct CurationCommand {
        uint8_t unit = 0;    ///< phos::LockUnit
        uint8_t op = 0;      ///< 0 unlock, 1 lock, 2 reroll, 3 clear everything
        int32_t index = 0;   ///< unit index (Composer::sectionUnitIndex and friends)
    };
    phos::EventRing<CurationCommand> curation_{ 64 };   ///< message thread -> composer thread
    mutable std::mutex curationLock_;                   ///< guards the two mirrors below
    std::map<int, uint8_t> lockMirror_[phos::kNumLockUnits];      ///< what the editor draws
    std::map<int, uint32_t> variationMirror_[phos::kNumLockUnits];   ///< @copydoc lockMirror_

    // ---- perform macros (message thread only)
    /** @brief One macro's position and, for a momentary one, when it ends. */
    struct MacroState {
        float  value = 0.0f;         ///< where it stands; 0 is neutral
        double releaseBeat = -1.0;   ///< musical beat at which it lets go, < 0 = not timed
    };
    MacroState macro_[kNumMacros];
    /**
     * @brief The knob values the macros found, by parameter id, in their real units.
     *
     * A macro writes knobs, so it has to be able to put them back: the first macro to touch a
     * parameter records what was there, the last one to let go writes it back. Without this a
     * filter sweep would leave the cutoff wherever the hand stopped. The real value is kept rather
     * than a normalised position, because a round trip through a logarithmic mapping is not exact.
     */
    std::map<int, float> macroBase_;
    unsigned macroTicks_ = 0;   ///< message thread: how many ticks since the slow jobs last ran
    /** @brief The macros as host parameters (23.09.2026): automatable and MIDI-learnable like every knob. */
    juce::RangedAudioParameter* macroParam_[kNumMacros] = {};
    /** @brief The value each macro's host parameter had when serviceMacros() last looked (message thread). */
    float macroHostSeen_[kNumMacros] = {};
    /** @brief Controller -> target (MIDI learn). */
    phos::MidiMap midiMap_;

    // ---- the cue bridge (PLAN 8.3, Cue.h)
    /**
     * @brief The cues of a set, sent from the play position rather than from the composer.
     *
     * The chain is: the composer thread writes marks into #cueMarks_ bars ahead; processBlock hands
     * #cueTap_ the beat range the block covers and it pushes cues into the sender's queue with the
     * instant the listener will hear them; the sender's own thread waits for that instant and calls
     * sendto(). Nothing on the audio thread allocates, locks or touches a socket.
     */
    phos::CueSender cues_;
    phos::CueTap cueTap_;                       ///< @copydoc cues_
    phos::CueMarkRing cueMarks_{ 256 };         ///< @copydoc cues_
    juce::String cueHost_{ phos::kCueDefaultHost };   ///< message thread's copy of the destination
    mutable std::mutex cueHostLock_;            ///< guards #cueHost_ (the editor writes, the timer reads)
    bool cueWanted_ = false;                    ///< message thread: what `cue.send` said at the last tick
    int  cuePort_ = phos::kCueDefaultPort;      ///< message thread: what `cue.port` said at the last tick
    juce::String cuePortHost_;                  ///< message thread: the host the socket was opened with
    std::atomic<float> cueLeadMs_{ 0.0f };      ///< `cue.lead_ms`, as the audio thread reads it
    int lastStateVersion_ = kStateVersion;      ///< the version attribute of the last state that was read
    juce::String legacyKnobs_;                  ///< an older state's knob text, held and not applied (20.09.2026)
    std::atomic<bool> cueBeats_{ true };        ///< `cue.beats`, as the audio thread reads it
    std::atomic<bool> cueAnnounce_{ false };    ///< the bridge just opened: say which section is playing

    // ---- mute and recording
    std::atomic<bool> mute_{ false };   ///< the output is silenced at the very end of processBlock
    bool forceMute_ = false;            ///< `PHOS_MUTE=1`: the switch is stuck on
    /** @brief `PHOS_TRACE=1`: processBlock reports the transport and the handshake on stderr. */
    bool trace_ = false;
    unsigned traceCount_ = 0;   ///< @copydoc trace_
    juce::TimeSliceThread recordThread_{ "Phosphene recorder" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> recordWriter_;
    juce::CriticalSection recordLock_;
    std::atomic<bool> recording_{ false };
    std::atomic<juce::int64> recordedSamples_{ 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhospheneProcessor)
};
