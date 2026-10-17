/**
 * @file PluginProcessor.cpp
 * @brief Implementation of the Phosphene processor: parameters, threads, transport and MIDI out.
 * @author Rene Weller
 */
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "phos/FieldLibrary.h"
#include "phos/FieldPresets.h"
#include "phos/Probe.h"
#include <thread>
#include "phos/Clock.h"
#include "phos/Model.h"
#include "phos/FactorySets.h"
#include "phos/SetFile.h"
#include "phos/Rating.h"
#include "phos/WaveTableFile.h"
#include <algorithm>
#include <chrono>
#include <cmath>

using namespace phos;

namespace
{

/** @brief How far ahead of the play position the rings are kept filled (PLAN 3: never under 8 bars). */
constexpr double HorizonBeats = 8.0 * BeatsPerBar;

/** @brief The shipped wavetable pack, installed beside the artefacts by Plugin/CMakeLists.txt. */
constexpr const char *WaveTablePack = "library.phoswt";

/**
 * @brief The directory a shipped resource lies in, looked for the way a plugin has to look.
 *
 * A plugin has no working directory worth the name -- it is whatever the host was started in -- and
 * no source tree, so `PHOS_SOURCE_DATA_DIR` (Core/CMakeLists.txt) only ever helps a development
 * build. What a plugin does have is its own module: JUCE hands the loaded binary back through
 * `currentExecutableFile`, and the bundle around it through `currentApplicationFile` (on macOS
 * these differ, on Windows they are the same file). From there the candidates are the three places
 * a resource can sit in the formats this plugin is built in:
 *
 * | Format | Binary | The resources |
 * |---|---|---|
 * | Standalone (Windows, Linux) | `Phosphene.exe` | beside it |
 * | Standalone (macOS) | `Phosphene.app/Contents/MacOS/Phosphene` | `Contents/Resources` |
 * | VST3 (Windows) | `Phosphene.vst3/Contents/x86_64-win/Phosphene.vst3` | `Contents/Resources` |
 * | VST3 (macOS) | `Phosphene.vst3/Contents/MacOS/Phosphene` | `Contents/Resources` |
 *
 * `Contents/Resources` is where the VST3 specification puts a plugin's data, and it is the one
 * place a host will not strip when it copies a bundle. The directory beside the binary is kept as
 * the first candidate because it is what the standalone and a plain copy of the build tree have.
 *
 * Nothing is loaded here and nothing fails here: an absent file is not an error -- it is the
 * built-in tables (WaveTableFile.h) or the Markov composer (Model.h) -- and the editor says so.
 * @param[in] file  the bare name to look for, e.g. `library.phoswt` or `melody.phosmdl`
 * @return the directory holding it, or an empty string
 */
juce::String resolveResourceDirectory( const char *file )
{
    juce::Array<juce::File> dirs;
    const juce::File        binaries[] = { juce::File::getSpecialLocation( juce::File::currentExecutableFile ),
                                           juce::File::getSpecialLocation( juce::File::currentApplicationFile ) };
    for ( const juce::File &f : binaries )
    {
        if ( f == juce::File() ) continue;
        const juce::File dir = f.isDirectory() ? f : f.getParentDirectory();
        dirs.addIfNotAlreadyThere( dir );                                                    // beside the binary
        dirs.addIfNotAlreadyThere( dir.getParentDirectory().getChildFile( "Resources" ) );   // .../Contents/\<arch\>/..
        dirs.addIfNotAlreadyThere( dir.getChildFile( "Contents" ).getChildFile( "Resources" ) );   // the bundle itself
    }
    for ( const juce::File &d : dirs )
        if ( d.getChildFile( file ).existsAsFile() ) return d.getFullPathName();
    return {};
}

juce::String                      gWaveTablePackDirectory;   ///< where installSearchPaths() found the pack, or empty
PhospheneProcessor::LearnedModels gLearnedModels;            ///< what installSearchPaths() made of Phase 8

/**
 * @brief Points the core at the shipped resources and settles Phase 8, once per process.
 *
 * Called from the processor's constructor, before its composer thread starts: that thread prepares
 * probe engines of its own, and the first Engine::prepare() anywhere in the process is the one that
 * loads the wavetable library, while the first compose pass is the one that loads the two models.
 * Setting either path afterwards would be both a race and too late.
 *
 * **Why the models are opened here and not left to the composer.** They would load by themselves on
 * the composer's thread the first time the knob asks for them, and the Set tab could ask afterwards
 * -- but then the Set tab's answer would be "not yet" for as long as nobody had switched the models
 * on, which is precisely the state in which a user needs to be told that the files are missing. So
 * the question is asked once, eagerly, on the thread that constructs the processor. It costs about
 * 13 MB of packed weights and a tenth of a second per process, once, against Phase 8 disappearing
 * without a word. The load is idempotent (phos::sharedMelodyModel), so the composer later finds the
 * work already done rather than doing it twice.
 */
void installSearchPaths()
{
    static std::once_flag once;
    std::call_once( once,
                    []
                    {
                        gWaveTablePackDirectory = resolveResourceDirectory( WaveTablePack );
                        if ( gWaveTablePackDirectory.isNotEmpty() )
                            setWaveTableSearchPath( gWaveTablePackDirectory.toStdString() );

                        // The models are looked for under the melody file: the installer and Plugin/CMakeLists.txt
                        // put both into the same directory, and when only one of them is there the core's own
                        // message below names the one that is missing, which is more use than a second directory.
                        gLearnedModels.m_directory = resolveResourceDirectory( MelodyModelFile );
                        if ( gLearnedModels.m_directory.isNotEmpty() )
                            setModelSearchPath( gLearnedModels.m_directory.toStdString() );

                        std::string note;
                        if ( const NeuralModel *m = sharedMelodyModel( &note ) )
                        {
                            gLearnedModels.m_melody = true;
                            gLearnedModels.m_melodyNll = m->info().m_nll;
                        }
                        else
                        {
                            gLearnedModels.m_melodyNote = juce::String( note );
                        }
                        if ( const NeuralModel *m = sharedBassModel( &note ) )
                        {
                            gLearnedModels.m_bass = true;
                            gLearnedModels.m_bassNll = m->info().m_nll;
                        }
                        else
                        {
                            gLearnedModels.m_bassNote = juce::String( note );
                        }
                        // Loaded, but from nowhere this plugin pointed at: a development build's PHOS_SOURCE_DATA_DIR
                        // (Core/CMakeLists.txt) or the working directory it happened to be started in. Worth saying,
                        // because it is the one case that works on this machine and on no other.
                        gLearnedModels.m_fromSourceTree = gLearnedModels.m_directory.isEmpty() &&
                                                          ( gLearnedModels.m_melody || gLearnedModels.m_bass );
                    } );
}

/**
 * @brief "kick" -> "Kick", "perc3" -> "Perc 3", "compose" -> "Set".
 *
 * The host's parameter list is flat, so every name has to say which module it belongs to. The
 * prefix of the text key is the only source for that, which keeps the naming generated.
 * @param[in] key  the parameter's key
 * @return the module's name
 */
juce::String prettyPrefix( const std::string &key )
{
    const size_t dot = key.find( '.' );
    juce::String p = juce::String( key.substr( 0, dot == std::string::npos ? key.size() : dot ) );
    if ( p == "compose" ) return "Set";
    if ( p == "sfx" ) return "SFX";
    if ( p == "fx" ) return "FX";
    juce::String head, digits;
    for ( int i = 0; i < p.length(); ++i )
    {
        const juce::juce_wchar c = p[i];
        if ( c >= '0' && c <= '9' )
            digits += juce::String::charToString( c );
        else
            head += juce::String::charToString( c );
    }
    head = head.substring( 0, 1 ).toUpperCase() + head.substring( 1 );
    return digits.isEmpty() ? head : head + " " + digits;
}

}   // namespace

const char *const MacroNames[NumMacros] = { "Filter Sweep", "Gate Depth", "Drop-out", "Stutter" };
const char *const MacroHelp[NumMacros] = {
    "Opens or closes the filters of acid, lead, counter-lead, arp and stab together, by up to a third of their range in "
    "each direction. Centre is neutral.",
    "Switches the trance gate on for lead, counter-lead, arp, stab and pad and sets its depth; at 1.0 the gate closes "
    "completely between its steps.",
    "Takes kick and bass out until the next bar line and lets them back in on the downbeat. One "
    "press, one bar.",
    "Held: lead, counter-lead, arp, stab and pad run through the gate on sixteenths at full depth and the shortest "
    "duty. Phosphene has no buffer repeat, so this is the gate's stutter, not a tape one.",
};

bool macroIsMomentary( MacroE m )
{
    return m == MacroE::DROP_OUT || m == MacroE::STUTTER;
}

// ==================================================================== PhospheneProcessor

PhospheneProcessor::PhospheneProcessor() : juce::AudioProcessor( busLayout() )
{
    for ( auto &a : m_shownIndex )
        a.store( -1, std::memory_order_relaxed );   // no composer preset shown yet (26.09.2026)
    // Before anything prepares an engine -- this instance's, or one of the composer thread's probe
    // renders -- the core has to know where the shipped tables and the two learned models are. The
    // table load itself happens in the first Engine::prepare() and only once for the process.
    installSearchPaths();
    // 22.09.2026, the user: "der Start des Programms dauert relativ lange". Measured, from Play to
    // the first sample: 12.8 s, and almost all of it is one thing -- the first track's level match,
    // which renders the foundation, each melodic part, the two presence probes and the mix twice.
    // The core's own default is a single probe thread (Probe.cpp), and no host ever raised it, so the
    // plugin planned its first track serially on a machine with 24 of them.
    //
    // Parallel probes are not an approximation: testProbeSchedule renders the same plans with one,
    // three and eight threads and compares them bit for bit, cold library included.
    //
    // Probe.h left this off for the plugin on purpose, and the reason it gives is the right one:
    // "in the plugin the plan is made next to a running audio thread, where eight busy worker
    // threads for several seconds were a risk nobody had shown to be harmless". The host
    // test can show it -- it renders live, at a device's rate, while the composer thread plans ahead,
    // and it measures both the audio thread's own cost and the longest gap in the output. With this
    // on: 14.0 % of a core against 13.5 % before, no gap over 150 ms. But that is one machine with
    // twenty-four cores, and the concern is a small one, so half of them is what is taken and the
    // other half stays for the host. Probe.cpp clamps to MaxThreads.
    //
    // It happens here rather than in prepareToPlay because the composer thread starts with the
    // processor, and a test that wants another number sets it after construction, as they all do.
    const int cores = static_cast<int>( std::thread::hardware_concurrency() );
    phos::probe::setThreads( juce::jmax( 1, cores / 2 ) );
    m_engine = std::make_unique<Engine>();
    m_engine->setLive(
        true );   // the keyboard's Replace and the composer switch act here, never in an export (01.10.2026)
    m_composer = std::make_unique<Composer>( m_seed.load() );
    // The plugin plans without Auto Gain and measures it once the music is running (serviceComposer).
    m_composer->setDeferMasterGain( true );
    m_conductor = std::make_unique<PlugConductor>( *m_engine, *m_composer );
    m_conductor->setCueMarks( &m_cueMarks );
    buildParameters();
    // The live ring's values start at the knobs, so nothing shows a ring before the first block has played.
    m_playedCount = params().count();
    m_played = std::make_unique<std::atomic<float>[]>( static_cast<size_t>( m_playedCount ) );
    m_playedScratch.assign( static_cast<size_t>( m_playedCount ), 0.0f );
    for ( int id = 0; id < m_playedCount; ++id )
        m_played[static_cast<size_t>( id )].store( params().get( id ), std::memory_order_relaxed );
    m_planKnobIds = Composer::planKnobIds( params() );
    m_composeParams.copyValuesFrom( params() );
    // The listener's learned preferences (23.09.2026, phos/Preferences.h): process-wide, so every instance plans
    // with the same taste; loaded before the composer thread plans its first track.
    if ( preferencesFile().existsAsFile() )
    {
        auto prefs = std::make_shared<Preferences>();
        if ( prefs->parse( preferencesFile().loadFileAsString().toStdString() ) ) setPreferences( prefs );
    }

    // The cue bridge (PLAN 8.3) is off until `cue.send` is switched on. `PHOS_CUE_HOST` and
    // `PHOS_CUE_PORT` are for an automated run, which has no state file to carry a destination.
    const juce::String envHost = juce::SystemStats::getEnvironmentVariable( "PHOS_CUE_HOST", "" );
    if ( envHost.isNotEmpty() ) m_cueHost = envHost;
    const juce::String envPort = juce::SystemStats::getEnvironmentVariable( "PHOS_CUE_PORT", "" );
    if ( envPort.isNotEmpty() )
        params().set( params().base( ModuleE::CUE ) + cue::PORT, static_cast<float>( envPort.getIntValue() ) );

    // PHOS_MUTE=1: silent from the first sample and never unmuted from inside. The house rule for
    // every automated run -- tests, the screenshot export -- is that nothing makes a sound.
    m_forceMute = juce::SystemStats::getEnvironmentVariable( "PHOS_MUTE", "" ).isNotEmpty() ||
                  juce::SystemStats::getEnvironmentVariable( "PHOS_SHOT", "" ).isNotEmpty() ||
                  juce::SystemStats::getEnvironmentVariable( "PHOS_SHOT_ALL", "" ).isNotEmpty() ||
                  juce::SystemStats::getEnvironmentVariable( "PHOS_MANUAL", "" ).isNotEmpty();
    m_mute.store( m_forceMute );
    m_trace = juce::SystemStats::getEnvironmentVariable( "PHOS_TRACE", "" ).isNotEmpty();
    gPlugTrace = m_trace;

    m_followHost = wrapperType != wrapperType_Standalone;
    m_followHostAtomic.store( m_followHost );

    bindDefaultControllers();

    m_composerRun.store( true );
    m_composerThread = std::thread( [this] { composerLoop(); } );
    startTimer( 33 );
}

PhospheneProcessor::~PhospheneProcessor()
{
    stopTimer();
    stopRecording();
    // Before the composer thread ends: the sender's thread is the only other one that outlives the
    // audio thread, and it must not be running while the queue it reads is destroyed.
    m_cues.stop();
    m_composerRun.store( false, std::memory_order_release );
    if ( m_composerThread.joinable() ) m_composerThread.join();
    stopMeasureJob();
}

void PhospheneProcessor::buildParameters()
{
    ParamStore &p = params();
    m_byId.assign( static_cast<size_t>( p.count() ), nullptr );
    for ( int i = 0; i < p.count(); ++i )
    {
        const juce::String name = prettyPrefix( p.key( i ) ) + " " + p.desc( i ).m_name;
        auto              *param = new StoreParameter( p, i, name );
        param->addListener( this );   // undo: the editor's gestures (parameterGestureChanged)
        m_byId[static_cast<size_t>( i )] = param;
        addParameter( param );
    }
    // The four macros as host parameters (23.09.2026), after every store parameter so that no id before them
    // moves. A DAW automates them like any knob and a controller can be learned onto them; serviceMacros()
    // carries a change of the host value into the macro and a macro that lets go by itself (a drop-out, one
    // bar) back into the host value.
    static const char *const MacroIds[NumMacros] = { "macro_filter_sweep", "macro_gate_depth", "macro_drop_out",
                                                     "macro_stutter" };
    for ( int i = 0; i < NumMacros; ++i )
    {
        const MacroE                m = static_cast<MacroE>( i );
        const juce::ParameterID     pid( MacroIds[i], 1 );
        const juce::String          name = juce::String( "Macro " ) + MacroNames[i];
        juce::RangedAudioParameter *hp = nullptr;
        if ( macroIsMomentary( m ) )
            hp = new juce::AudioParameterBool( pid, name, false );
        else
            hp = new juce::AudioParameterFloat(
                pid, name, juce::NormalisableRange<float>( m == MacroE::FILTER_SWEEP ? -1.0f : 0.0f, 1.0f ), 0.0f );
        m_macroParam[i] = hp;
        addParameter( hp );
    }
}

juce::RangedAudioParameter *PhospheneProcessor::hostParameter( int target ) const
{
    const int n = static_cast<int>( m_byId.size() );
    if ( target >= 0 && target < n ) return m_byId[static_cast<size_t>( target )];
    if ( target >= n && target < n + NumMacros ) return m_macroParam[target - n];
    return nullptr;
}

juce::String PhospheneProcessor::midiTargetName( int target ) const
{
    if ( auto *hp = hostParameter( target ) ) return hp->getName( 64 );
    return {};
}

std::string PhospheneProcessor::midiTargetKey( int target ) const
{
    static const char *const MacroKeys[NumMacros] = { "macro.filter_sweep", "macro.gate_depth", "macro.drop_out",
                                                      "macro.stutter" };
    const int                n = params().count();
    if ( target >= 0 && target < n ) return params().key( target );
    if ( target >= n && target < n + NumMacros ) return MacroKeys[target - n];
    return {};
}

int PhospheneProcessor::midiTargetFind( const std::string &key ) const
{
    for ( int i = 0; i < NumMacros; ++i )
        if ( midiTargetKey( params().count() + i ) == key ) return params().count() + i;
    return params().find( key.c_str() );
}

PhospheneProcessor::WaveTableLibrary PhospheneProcessor::waveTableLibrary() const
{
    WaveTableLibrary s;
    s.m_directory = gWaveTablePackDirectory;
    s.m_shipped = NumLibraryWaveTables;
    s.m_loaded = waveTablesIndexed();
    s.m_built = waveTablesBuilt();
    return s;
}

PhospheneProcessor::LearnedModels PhospheneProcessor::learnedModels() const
{
    // Settled by installSearchPaths() in the constructor, so this is a copy and never a load.
    return gLearnedModels;
}

juce::AudioProcessor::BusesProperties PhospheneProcessor::busLayout()
{
    BusesProperties b = BusesProperties().withOutput( "Output", juce::AudioChannelSet::stereo(), true );
    for ( int s = 0; s < phos::NumStems; ++s )
        b = b.withOutput( phos::StemNames[s], juce::AudioChannelSet::stereo(), false );
    return b;
}

bool PhospheneProcessor::isBusesLayoutSupported( const BusesLayout &layouts ) const
{
    if ( layouts.getMainInputChannels() != 0 ) return false;
    const auto &out = layouts.getMainOutputChannelSet();
    if ( out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono() ) return false;
    for ( int b = 1; b < layouts.outputBuses.size(); ++b )
    {   // a stem's bus (02.10.2026): stereo, or off
        const juce::AudioChannelSet &set = layouts.outputBuses.getReference( b );
        if ( !set.isDisabled() && set != juce::AudioChannelSet::stereo() ) return false;
    }
    return true;
}

void PhospheneProcessor::prepareToPlay( double sampleRate, int samplesPerBlock )
{
    {
        // The engine allocates here and never again. Only the composer thread's short pushing
        // section is locked out; its planning, which can take seconds, is not -- a host must not
        // wait for a level-match probe to finish before it can open a plugin.
        const std::lock_guard<std::mutex> lock( m_engineLock );
        m_sampleRate = sampleRate;
        m_scratch.setSize( 2, juce::jmax( 64, samplesPerBlock ), false, true, true );
        // The stems' buffers for their outputs (02.10.2026); the engine writes them only while a stem's bus is on.
        m_stemBlock = juce::jmax( 64, samplesPerBlock );
        m_stemBuf.assign( static_cast<size_t>( phos::NumStems * 2 * m_stemBlock ), 0.0f );
        for ( int s = 0; s < phos::NumStems; ++s )
        {
            m_stemL[static_cast<size_t>( s )] = m_stemBuf.data() + static_cast<size_t>( 2 * s * m_stemBlock );
            m_stemR[static_cast<size_t>( s )] = m_stemBuf.data() + static_cast<size_t>( ( 2 * s + 1 ) * m_stemBlock );
        }
        m_engine->setStemTap( nullptr );
        m_stemsOn = false;
        m_engine->prepare( sampleRate, samplesPerBlock );
        m_engine->setMetering( true );   // reading only: the mix is the same to the bit (Engine.h)
        for ( HeldNote &h : m_held ) h = HeldNote{};
        NoteEvent junk;
        while ( m_midiRing.pop( junk ) )
        {
        }
    }
    // A fresh generation: the conductor repositions and the rings are filled again before the next
    // sample is rendered. Nothing here writes the composer thread's own counter.
    m_wasPlaying = false;
    m_genWanted.fetch_add( 1, std::memory_order_release );
    setLatencySamples( m_engine->latencySamples() );
}

void PhospheneProcessor::timerCallback()
{
    // Ableton Link (02.10.2026): the standalone joins the session while the setting is on (or FAMILY_LINK=1); a DAW's
    // transport rules the plugin.
    m_link.setEnabled( wrapperType == wrapperType_Standalone &&
                       ( frame::Settings::of( "Phosphene" ).link() || frame::LinkClock::forcedOn() ) );
    m_link.tick();
    // The family jam (02.10.2026, Jam.h): the role from the settings; a follower's walk holds its key and takes the
    // leader's mode with its next track (the composer thread hands it on, serviceComposer).
    m_jam.setRole( frame::Settings::of( "Phosphene" ).jamRole() );
    {
        const bool following = m_jam.role() == frame::Settings::JamRoleE::FOLLOW && m_jam.leaderRoot() >= 0;
        m_jamWalkHold.store( following, std::memory_order_relaxed );
        m_jamWalkScale.store( following ? scaleOfMode( m_jam.leaderMode() ) : -1, std::memory_order_relaxed );
    }
    if ( const char *logFile = std::getenv( "FAMILY_JAM_LOG" ) )
    {   // a test aid: what the jam does, whenever it changes
        const juce::String line = m_jam.status() + "; transposed " + juce::String( m_jamTransposeOut.load() );
        if ( line != m_jamLogged )
        {
            m_jamLogged = line;
            juce::File( logFile ).appendText( juce::Time::getCurrentTime().toString( false, true, true, true ) + "  " +
                                              line + "\n" );
        }
    }
    // The fast job is the macros: a drop-out has to let go on the bar line, and 33 ms is a
    // sixty-fourth note at 145 BPM. Everything else here is housekeeping and runs every eighth tick.
    pollHeadset();   // the headset's hands move macros too (01.10.2026)
    serviceMacros();
    if ( ++m_macroTicks % 8 != 0 ) return;
    serviceCueBridge();
    // A table the *user* picked (the Table box, a loaded set) has to be expanded by somebody, and
    // the audio thread must not do it (22.09.2026, the on-demand library). Six atomic loads when
    // nothing changed, on the message thread, a quarter of a second after the click at worst --
    // until then the voice sounds the built-in fallback, which is what a machine without the pack
    // hears anyway.
    m_engine->ensureVoiceTables();
    const int latency = m_engine->latencySamples();
    if ( latency != getLatencySamples() ) setLatencySamples( latency );
    // The composer's own knobs decide how the set is planned, and the composer throws its cached
    // plans away when they move. The published copy the editor draws has to follow, so a cheap
    // fingerprint of those parameters is watched here rather than a listener on six hundred of them.
    const ParamStore &p = params();
    const int         cb = p.base( ModuleE::COMPOSE );
    float             sum = 0.0f;
    for ( int i = 0; i < ParamStore::moduleCount( ModuleE::COMPOSE ); ++i ) sum += ( i + 1 ) * p.get( cb + i );
    if ( sum != m_composeFingerprint )
    {
        m_composeFingerprint = sum;
        m_plansStale.store( true, std::memory_order_release );
    }
    refreshPresetDisplay();
}

int PhospheneProcessor::soundSynthOfId( int id, int &local ) const
{
    const ParamStore &p = params();
    for ( int k = 0; k < SoundSynths; ++k )
    {
        const ModuleE m = k == 0 ? ModuleE::KICK : k == 1 ? ModuleE::BASS : k == 2 ? ModuleE::ACID : ModuleE::POLY;
        const int     b = p.base( m, k >= 3 ? k - 3 : 0 );
        if ( id >= b && id < b + ParamStore::moduleCount( m ) )
        {
            local = id - b;
            return k;
        }
    }
    local = -1;
    return -1;
}

void PhospheneProcessor::refreshPresetDisplay()
{
    const TransportView t = transport();
    TrackPlan           plan;
    if ( !tryReadTrack( t.m_track, plan ) ) return;
    ParamStore &p = params();
    const int   ownBase = p.base( ModuleE::MIX ) + mix::KICK_OWN;
    for ( int k = 0; k < SoundSynths; ++k )
    {
        if ( p.getBool( ownBase + k ) )
        {
            m_shownPreset[k] = -2;
            m_shownIndex[k].store( -1, std::memory_order_relaxed );
            continue;
        }
        const SoundPreset *sp = Composer::presetOf( p, plan, k );
        const int          idx = sp != nullptr ? plan.m_soundPreset[k] : -1;
        m_shownPreset[k] = idx;
        m_shownIndex[k].store( idx, std::memory_order_relaxed );
    }
}

void PhospheneProcessor::takeOverPreset( int k )
{
    // The knobs of the synth take the preset's values (26.09.2026): what the user starts turning from is what sounded.
    // Inside the gesture, so the undo step that records the turn takes this back with it. Not the plan knobs.
    const int idx = k >= 0 && k < SoundSynths ? m_shownPreset[k] : -1;
    if ( idx < 0 ) return;
    const ModuleE m = k == 0 ? ModuleE::KICK : k == 1 ? ModuleE::BASS : k == 2 ? ModuleE::ACID : ModuleE::POLY;
    const std::vector<SoundPreset> &bank = factoryPresets( m, k >= 3 ? k - 3 : 0 );
    if ( idx >= static_cast<int>( bank.size() ) ) return;
    ParamStore &p = params();
    const int   b = p.base( m, k >= 3 ? k - 3 : 0 );
    for ( const auto &kv : bank[static_cast<size_t>( idx )].m_values )
    {
        const int id = b + kv.first;
        if ( std::find( m_planKnobIds.begin(), m_planKnobIds.end(), id ) != m_planKnobIds.end() ||
             p.get( id ) == kv.second )
            continue;
        if ( StoreParameter *sp = parameterFor( id ) ) sp->setValueNotifyingHost( p.toNormalised( id, kv.second ) );
    }
}

// ------------------------------------------------------------------ the cue bridge (PLAN 8.3)

juce::String PhospheneProcessor::cueHost() const
{
    const std::lock_guard<std::mutex> lock( m_cueHostLock );
    return m_cueHost;
}

void PhospheneProcessor::setCueHost( const juce::String &host )
{
    const std::lock_guard<std::mutex> lock( m_cueHostLock );
    m_cueHost = host.trim().isEmpty() ? juce::String( CueDefaultHost ) : host.trim();
}

void PhospheneProcessor::serviceCueBridge()
{
    const ParamStore &p = params();
    const int         base = p.base( ModuleE::CUE );
    m_cueBeats.store( p.getBool( base + cue::BEATS ), std::memory_order_relaxed );
    m_cueLeadMs.store( p.get( base + cue::LEAD_MS ), std::memory_order_relaxed );

    const bool         want = p.getBool( base + cue::SEND );
    const int          port = p.getInt( base + cue::PORT );
    const juce::String host = cueHost();
    if ( want == m_cueWanted && port == m_cuePort && host == m_cuePortHost ) return;
    m_cueWanted = want;
    m_cuePort = port;
    m_cuePortHost = host;

    m_cues.stop();
    if ( !want ) return;
    // A destination that cannot be resolved is not an error and is not reported to the user: the
    // whole contract of this bridge is that a visualiser which is not there changes nothing. It is
    // traced, because a silent failure that nobody can see is the other way to get this wrong.
    // Offline (a bounce, the demo) the due times describe a timeline nobody is living through, so
    // the sender does not wait for them.
    const bool ok = m_cues.start( host.toStdString(), port, !isNonRealtime() );
    if ( ok ) m_cueAnnounce.store( true, std::memory_order_release );
    if ( m_trace )
        std::fprintf( stderr, "[phos cues] %s %s:%d\n", ok ? "sending to" : "cannot reach", host.toRawUTF8(), port );
}

// ------------------------------------------------------------------ the composer thread

void PhospheneProcessor::setNonRealtime( bool offline ) noexcept
{
    juce::AudioProcessor::setNonRealtime( offline );
    m_offline.store( offline, std::memory_order_release );
    // Offline is a bounce, and a bounce has to be the same samples phos_render writes -- the whole
    // determinism contract of this project rests on it (`the plugin equals phos_render bit for bit`,
    // Tests/hosttest.cpp). Deferring Auto Gain is a *live* convenience: it lets the music start
    // before the offset is measured and rides it in over a ramp, which is a different signal. So the
    // deferral is switched off here, and the plans made under it are thrown away so the bounce plans
    // whole. (22.09.2026.)
    const std::lock_guard<std::mutex> lock( m_composeLock );
    if ( offline ) stopMeasureJob();   // a bounce measures whole, on its own; a live measurement would only compete
    if ( m_composer->defersMasterGain() == offline )
    {
        m_composer->setDeferMasterGain( !offline );
        m_composer->setSeed( m_seed.load( std::memory_order_relaxed ) );   // clears the cached plans
        m_plansStale.store( true, std::memory_order_release );
    }
}

void PhospheneProcessor::composerLoop()
{
    unsigned turns = 0;
    while ( m_composerRun.load( std::memory_order_acquire ) )
    {
        if ( m_trace && ++turns % 500 == 0 )
            std::fprintf( stderr, "[phos composer] turn %u, generation %d, next bar %d, %d tracks planned\n", turns,
                          m_genLocal, m_conductor->nextBar(), m_publishedTracks );
        if ( !m_offline.load( std::memory_order_acquire ) ) serviceComposer( true );
        std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
    }
}

bool PhospheneProcessor::drainCuration()
{
    CurationCommand c;
    bool            any = false;
    while ( m_curation.pop( c ) )
    {
        const LockUnitE unit =
            static_cast<LockUnitE>( juce::jlimit( 0, NumLockUnits - 1, static_cast<int>( c.m_unit ) ) );
        switch ( c.m_op )
        {
        case 0: m_composer->setLock( unit, c.m_index, false ); break;
        case 1: m_composer->setLock( unit, c.m_index, true ); break;
        case 2: m_composer->reroll( unit, c.m_index ); break;
        case 4: m_composer->setVariation( unit, c.m_index, c.m_value ); break;
        default: m_composer->clearLocks(); break;
        }
        any = true;
    }
    if ( !any ) return false;
    // Every cached plan is gone (Composer::setLock clears them), so the editor's published copy has
    // to be built again, and the bars already in the rings were composed under the old decisions:
    // the transport restarts at the bar the editor marked when the button was pressed.
    m_plansStale.store( true, std::memory_order_release );
    m_restartRequest.store( true, std::memory_order_release );
    m_restarts.fetch_add( 1, std::memory_order_relaxed );
    return true;
}

void PhospheneProcessor::refreshComposeParams( bool restart )
{
    // What the plans were made with, for the plan knobs whose change has to wait.
    const size_t       n = m_planKnobIds.size();
    std::vector<float> held( n ), now( n );
    for ( size_t k = 0; k < n; ++k ) held[k] = m_composeParams.get( m_planKnobIds[k] );
    m_composeParams.copyValuesFrom( params() );
    for ( size_t k = 0; k < n; ++k ) now[k] = m_composeParams.get( m_planKnobIds[k] );
    // The clock of the rule is the music's, so that a bounce takes a change over on the same bar every time.
    const double beat = m_musicalBeat.load( std::memory_order_relaxed );
    if ( now != m_planKnobsSeen || beat < m_planKnobsSeenBeat )
    {
        m_planKnobsSeen = now;
        m_planKnobsSeenBeat = beat;
    }
    if ( now == held ) return;
    const bool running =
        m_playRequest.load( std::memory_order_relaxed ) || m_hostSyncNow.load( std::memory_order_relaxed );
    if ( restart || !running || beat - m_planKnobsSeenBeat >= PlanKnobSettleBeats )
    {
        m_plansStale.store(
            true, std::memory_order_release );   // taken over: the composer plans again, the editor's list follows
        if ( m_trace ) std::fprintf( stderr, "[phos composer] plan knobs taken over at beat %.1f\n", beat );
        return;
    }
    for ( size_t k = 0; k < n; ++k ) m_composeParams.set( m_planKnobIds[k], held[k] );   // still moving: the plans stay
}

void PhospheneProcessor::serviceComposer( bool warmUp )
{
    const std::lock_guard<std::mutex> lock( m_composeLock );
    drainCuration();
    m_composer->setJam( m_jamWalkHold.load( std::memory_order_relaxed ),
                        m_jamWalkScale.load( std::memory_order_relaxed ) );   // the family jam
    const int want = m_genWanted.load( std::memory_order_acquire );
    // One set of knobs for the whole turn (m_composeParams, PluginProcessor.h); a restart takes every knob over.
    refreshComposeParams( want != m_genLocal );
    const ParamStore &knobs = m_composeParams;
    if ( m_trace && want != m_genLocal )
        std::fprintf( stderr, "[phos composer] generation %d wanted, was %d, engine reset for %d\n", want, m_genLocal,
                      m_genReset.load() );
    if ( want != m_genLocal )
    {
        // Step 2 of the handshake: position the conductor. The engine's own tempo map is never
        // used -- the tempo arrives as control events (PlugConductor::tempoControls) -- so nothing
        // here allocates inside the engine, and a seek costs one composed bar.
        const int    bar = m_pendingBar.load( std::memory_order_relaxed );
        const double offset = m_pendingOffset.load( std::memory_order_relaxed );
        const bool   ownClock = !( m_followHostAtomic.load( std::memory_order_relaxed ) &&
                                   m_hostSyncNow.load( std::memory_order_relaxed ) );
        {
            const std::lock_guard<std::mutex> eng( m_engineLock );
            m_engine->clearTempoMap();
        }
        m_conductor->seek( knobs, bar, offset, ownClock, m_engineLock );
        m_genLocal = want;
        m_genAck.store( want, std::memory_order_release );
        return;
    }
    if ( m_genReset.load( std::memory_order_acquire ) != m_genLocal ) return;   // waiting for step 3
    m_conductor->pump( knobs, HorizonBeats, &m_midiRing, m_engineLock );
    m_genPrimed.store( m_genLocal, std::memory_order_release );

    // ---------------------------------------------------------------- measuring behind the music
    //
    // 22.09.2026, at the user's decision: Auto Gain after the start -- two whole-mix renders that used to put nine
    // seconds of silence between the button and the first sound, measured while the music runs and ridden in over
    // a phrase (`ControlEvent::Kind::Offset` ramps with a raised cosine over `length` beats).
    //
    // 23.09.2026, round "Planung": the first track's level, presence and audibility probes too (the composer plans
    // it without any, Composer::completeMeasurement), and all of it on a thread of its own, on a copy of the
    // composer. Done here, on this thread, the measurement held m_composeLock for 8 to 11 seconds, and a second
    // restart right after Play -- a click into the Arrange tab, a host's locate -- waited for all of them; the host
    // test measured 9.4 s. Now this thread is never busy for longer than a pump, and the job's result is taken over
    // when it arrives (Composer::adoptMeasurement, refused if the plans were thrown away meanwhile).
    //
    // A track opens with sixteen bars without a kick and without the lines -- some twenty-six seconds at 145 BPM --
    // and the job takes about fifteen, so the levels settle long before anyone hears a line or the first drop.
    const bool running =
        m_playRequest.load( std::memory_order_relaxed ) || m_hostSyncNow.load( std::memory_order_relaxed );
    if ( m_composer->defersMasterGain() )
    {
        const int playingTrack = m_composer->trackOfBar(
            knobs, juce::jmax( 0, static_cast<int>( m_musicalBeat.load( std::memory_order_relaxed ) / BeatsPerBar ) ) );
        // The corrections of a plan as the engine should now have them: every melodic part's level, and the
        // master offset, both over a phrase. Sent again when unchanged, which a ramp to the value it holds is.
        auto sendCorrections = [&]( int index )
        {
            const phos::TrackPlan    &now = m_composer->track( knobs, index );
            std::vector<ControlEvent> events;
            const double at = m_engine->beatPosition() + 1.0;   // a beat ahead of the play head, never behind it
            const float  phrase = 4.0f * static_cast<float>( BeatsPerBar );
            m_composer->levelControls( knobs, now, at, phrase, events );
            if ( !now.m_masterDeferred )
            {
                const ParamStore &ps = knobs;
                const ParamDesc  &mg = ps.desc( ps.base( ModuleE::MASTER ) + master::GAIN );
                ControlEvent      c;
                c.m_beat = at;
                c.m_param = static_cast<int16_t>( ps.base( ModuleE::MASTER ) + master::GAIN );
                c.m_kind = ControlEvent::KindE::OFFSET;
                c.m_value = now.m_masterGainDb / ( mg.m_maxValue - mg.m_minValue );
                c.m_length = phrase;
                events.push_back( c );
            }
            {
                const std::lock_guard<std::mutex> eng( m_engineLock );
                for ( const ControlEvent &c : events ) m_engine->pushImmediate( c );   // not behind eight queued bars
            }
            m_composer->clearCorrectionsPending( index );
            const std::lock_guard<std::mutex> pl( m_plansLock );
            if ( index < static_cast<int>( m_plans.size() ) ) m_plans[static_cast<size_t>( index )] = now;
        };
        // A finished job: take it over and send what it measured.
        if ( m_measureJob != nullptr && m_measureJob->m_done.load( std::memory_order_acquire ) )
        {
            if ( m_measureThread.joinable() ) m_measureThread.join();
            const bool adopted = m_composer->adoptMeasurement( m_measureJob->m_index, m_measureJob->m_result,
                                                               m_measureJob->m_generation );
            if ( m_trace )
                std::fprintf( stderr, "[phos composer] background measurement of track %d %s\n",
                              m_measureJob->m_index + 1,
                              adopted ? "taken over" : "dropped (the plans changed meanwhile)" );
            const int index = m_measureJob->m_index;
            m_measureJob.reset();
            if ( adopted && index == playingTrack )
            {
                sendCorrections( index );
            }
            else if ( adopted )
            {
                // Measured ahead of its turn: its first bars will be composed with it, and the editor's list shows it.
                const phos::TrackPlan            &now = m_composer->track( knobs, index );
                const std::lock_guard<std::mutex> pl( m_plansLock );
                if ( index < static_cast<int>( m_plans.size() ) ) m_plans[static_cast<size_t>( index )] = now;
            }
        }
        // Measured already, by the planning of a later track that needed the first one's numbers.
        if ( m_composer->correctionsPending( playingTrack ) ) sendCorrections( playingTrack );
        // Start one for the track that plays, while the set plays and no restart waits; that one measured, for the
        // next track, so that it arrives with its levels (26.09.2026: every live plan is made without its probes,
        // Composer::measureTrack). Stopped, nothing is measured at all: a probe render's CPU is only worth spending
        // on what is about to be heard.
        if ( m_measureJob == nullptr && running && m_genWanted.load( std::memory_order_acquire ) == m_genLocal )
        {
            for ( const int index : { playingTrack, playingTrack + 1 } )
            {
                const phos::TrackPlan &plan = m_composer->track( knobs, index );
                if ( plan.m_measureDeferred || plan.m_masterDeferred )
                {
                    startMeasureJob( index );
                    break;
                }
            }
        }
    }

    // With the rings filled, the spare time goes into planning the tracks ahead: the editor's list
    // of tracks comes from here, and a seek into a track that is already planned is instant. One
    // track per turn, because the first plan of a track measures its level by rendering it.
    // Only the composer thread does this. Offline, serviceComposer runs on the audio thread, and
    // planning a track there would stall the render for seconds per track for no gain.
    if ( !warmUp ) return;
    // Not while a restart is waiting: planning a track takes seconds, and the transport would
    // stand silent for all of them. The warm-up is what there is spare time for, nothing more.
    if ( m_genWanted.load( std::memory_order_acquire ) != m_genLocal ) return;
    if ( m_plansStale.exchange( false, std::memory_order_acq_rel ) ) m_publishedTracks = 0;
    // Live, a plan renders no probes (Composer::measureTrack, 26.09.2026) and takes milliseconds, so the list is
    // planned whether the set plays or not; until then this stopped at the first track while stopped or measuring,
    // because every later plan measured the first one again, here, for seconds.
    if ( m_publishedTracks < PublishedTracks )
    {
        const TrackPlan                   plan = m_composer->track( knobs, m_publishedTracks );
        const std::lock_guard<std::mutex> pl( m_plansLock );
        if ( m_publishedTracks == 0 ) m_plans.clear();
        m_plans.push_back( plan );
        ++m_publishedTracks;
    }
}

void PhospheneProcessor::startMeasureJob( int index )
{
    auto job = std::make_unique<MeasureJob>();
    job->m_composer = std::make_unique<phos::Composer>( *m_composer );
    job->m_composer->setAbortFlag( &job->m_abort );
    job->m_params.copyValuesFrom( m_composeParams );   // the knobs the plan was made with
    job->m_index = index;
    job->m_generation = m_composer->planGeneration();
    MeasureJob *j = job.get();
    m_measureJob = std::move( job );
    m_measureThread = std::thread(
        [j]
        {
            j->m_composer->completeMeasurement( j->m_params,
                                                j->m_index );   // level, presence, audibility (the first track)
            if ( !j->m_abort.load( std::memory_order_relaxed ) )
                j->m_composer->completeMasterGain( j->m_params, j->m_index );   // Auto Gain
            j->m_result = j->m_composer->track( j->m_params, j->m_index );
            j->m_done.store( true, std::memory_order_release );
        } );
    if ( m_trace ) std::fprintf( stderr, "[phos composer] measuring track %d behind the music\n", index + 1 );
}

void PhospheneProcessor::stopMeasureJob()
{
    if ( m_measureJob != nullptr ) m_measureJob->m_abort.store( true, std::memory_order_relaxed );
    if ( m_measureThread.joinable() ) m_measureThread.join();
    m_measureJob.reset();
}

// ------------------------------------------------------------------ the family jam (02.10.2026, Jam.h)

void PhospheneProcessor::jamFollow( double musicalBefore, double shared )
{
    if ( m_jam.role() != frame::Settings::JamRoleE::FOLLOW )
    {
        if ( m_jamFollowing ) m_engine->setJam( 0, false );
        m_jamFollowing = false;
        m_jamTranspose = 0;
        m_jamTransposeOut = 0;
        m_jamEnergyOut = -1.0f;
        m_jamLastBefore = musicalBefore;
        return;
    }
    const double step =
        ( m_jamLastBefore >= 0.0 && musicalBefore > m_jamLastBefore && musicalBefore - m_jamLastBefore < 1.0 )
            ? musicalBefore - m_jamLastBefore
            : 0.0;
    m_jamLastBefore = musicalBefore;
    const frame::JamState js = m_jam.stateAt( shared >= 0.0 ? shared + step : -1.0 );
    int                   want = 0;
    if ( js.m_root >= 0 )
    {
        // From the key of the track playing: the set walks its keys, the jam moves the whole to the leader's root.
        const int d = ( ( js.m_root - m_engine->keyRoot() ) % 12 + 12 ) % 12;
        want = d > 6 ? d - 12 : d;
    }
    const bool barLine =
        step <= 0.0 || std::floor( ( musicalBefore + step ) / 4.0 ) > std::floor( musicalBefore / 4.0 );
    if ( want != m_jamTranspose && barLine ) m_jamTranspose = want;
    m_jamTransposeOut = m_jamTranspose;
    m_jamEnergyOut = js.m_root >= 0 ? js.m_energy : -1.0f;
    m_engine->setJam( m_jamTranspose, js.m_root >= 0 && js.m_rhythmOut );
    m_jamFollowing = true;
}

void PhospheneProcessor::jamLead( double musicalBefore, double shared )
{
    if ( m_jam.role() != frame::Settings::JamRoleE::LEAD )
    {
        m_jamLeadStarted = false;
        return;
    }
    const phos::CueMark *m = m_cueTap.lastMark();
    if ( m == nullptr ) return;
    if ( m_jamLeadStarted && m_cueTap.marksPassed() == m_jamMarksSeen ) return;
    const bool first = !m_jamLeadStarted;
    m_jamLeadStarted = true;
    m_jamMarksSeen = m_cueTap.marksPassed();
    if ( first || m->m_keyChanged )
        m_jam.postKey( m->m_key,
                       phos::ScaleNames[juce::jlimit( 0, phos::NumScales - 1, static_cast<int>( m->m_scale ) )] );
    float                    e = 0.5f;
    bool                     drop = false;
    const frame::JamSectionE s =
        frame::jamSectionOf( phos::SectionNames[juce::jlimit( 0, static_cast<int>( phos::SectionTypeE::COUNT ) - 1,
                                                              static_cast<int>( m->m_type ) )],
                             e, drop );
    const int64_t bar = ( shared >= 0.0 && !first )
                            ? static_cast<int64_t>( std::llround( ( shared + m->m_beat - musicalBefore ) / 4.0 ) )
                            : int64_t( -1 );
    m_jam.postSection( s, m->m_energy, !first && drop, bar );
}

int PhospheneProcessor::scaleOfMode( const juce::String &mode )
{
    const juce::String m = mode.trim();
    for ( int i = 0; i < phos::NumScales; ++i )
        if ( m.equalsIgnoreCase( phos::ScaleNames[i] ) ) return i;
    if ( m.containsIgnoreCase( "major" ) || m.containsIgnoreCase( "ionian" ) || m.containsIgnoreCase( "lydian" ) ||
         m.containsIgnoreCase( "locrian" ) )
        return -1;   // the family's major modes (and the Locrian): psytrance's palette has none
    if ( m.containsIgnoreCase( "minor" ) || m.containsIgnoreCase( "penta" ) || m.containsIgnoreCase( "hexachord" ) )
        return 0;   // a minor, its pentatonic, Totality's hexachord: an Aeolian
    return -1;
}

juce::String PhospheneProcessor::linkStatus() const
{
    if ( !m_link.enabled() ) return "off";
    const int n = m_link.peers();
    return n == 0 ? "alone in the session: it takes this tempo"
                  : juce::String( n ) + ( n == 1 ? " other app" : " other apps" );
}

void PhospheneProcessor::requestSeek( int bar, double beatOffset )
{
    m_pendingBar.store( bar, std::memory_order_relaxed );
    m_pendingOffset.store( beatOffset, std::memory_order_relaxed );
    m_genWanted.fetch_add( 1, std::memory_order_release );
}

// ------------------------------------------------------------------ transport

void PhospheneProcessor::play()
{
    m_playRequest.store( true, std::memory_order_release );
}

void PhospheneProcessor::bindDefaultControllers()
{
    // The controllers every generator of the family has by default (01.10.2026): controller 74 (brightness) the filter
    // sweep, the expression pedal (11) the gate depth, the sustain pedal (64) the stutter while it is held. The mod
    // wheel (1) is left free.
    m_midiMap.bind( 0, 74, params().count() + static_cast<int>( MacroE::FILTER_SWEEP ) );
    m_midiMap.bind( 0, 11, params().count() + static_cast<int>( MacroE::GATE_DEPTH ) );
    m_midiMap.bind( 0, 64, params().count() + static_cast<int>( MacroE::STUTTER ) );
}

void PhospheneProcessor::composeSet()
{
    // Every knob taken over at once (refreshComposeParams: a restart takes them all), the plans made again, the set
    // from bar 1.
    m_plansStale.store( true, std::memory_order_release );
    seekToBar( 0 );
}

void PhospheneProcessor::resetToDefault( int id )
{
    if ( id < 0 || id >= params().count() ) return;
    StoreParameter *p = parameterFor( id );
    if ( p == nullptr ) return;
    undoable( juce::String( params().desc( id ).m_name ) + " to its default",
              [p]
              {
                  p->beginChangeGesture();
                  p->setValueNotifyingHost( p->getDefaultValue() );
                  p->endChangeGesture();
              } );
}

void PhospheneProcessor::pollHeadset()
{
    frame::Settings &st = frame::Settings::of( "Phosphene" );
    m_headset.listen( st.headset() == frame::Settings::HeadsetModeE::OFF ? 0 : st.headsetPort() );
    const frame::HeadsetEvents e = m_headset.poll();
    if ( e.m_playStop )
    {
        if ( isPlaying() )
            stop();
        else
            play();
    }
    if ( e.m_next )
    {
        // The next track, as the Quest app has it: the conductor goes to its first bar.
        phos::TrackPlan plan;
        if ( tryReadTrack( transport().m_track + 1, plan ) ) seekToBar( plan.m_firstBar );
    }
    if ( e.m_action ) setMacro( MacroE::DROP_OUT, 1.0f );
    if ( e.m_hold ) setMacro( MacroE::STUTTER, 1.0f );
    if ( e.m_holdEnded ) setMacro( MacroE::STUTTER, 0.0f );
    if ( e.m_filterMoved ) setMacro( MacroE::FILTER_SWEEP, e.m_filter );
    if ( e.m_throwMoved ) setMacro( MacroE::GATE_DEPTH, e.m_throwAmount );
}

void PhospheneProcessor::stop()
{
    m_playRequest.store( false, std::memory_order_release );
    seekToBar( 0 );
}

void PhospheneProcessor::seekToBar( int bar )
{
    m_pendingBar.store( juce::jmax( 0, bar ), std::memory_order_relaxed );
    m_pendingOffset.store( static_cast<double>( juce::jmax( 0, bar ) ) * BeatsPerBar, std::memory_order_relaxed );
    m_restartRequest.store( true, std::memory_order_release );
    m_restarts.fetch_add( 1, std::memory_order_relaxed );
}

void PhospheneProcessor::setFollowHost( bool on )
{
    m_followHost = on;
    m_followHostAtomic.store( on, std::memory_order_release );
    requestRestart();
}

// ------------------------------------------------------------------ undo (23.09.2026)

namespace
{

/** @brief One undo step: the state before and after an edit. perform() is a no-op the first time -- the edit has
 *         already happened when the step is recorded. */
class UndoStep final : public juce::UndoableAction
{
public:
    /**
     * @brief A step of @p p from @p before to @p after.
     * @param[in,out] p       the processor
     * @param[in]     before  the state before
     * @param[in]     after   the state after
     */
    UndoStep( PhospheneProcessor &p, PhospheneProcessor::UndoState before, PhospheneProcessor::UndoState after )
        : m_p( p ), m_before( std::move( before ) ), m_after( std::move( after ) )
    {
    }
    /**
     * @brief Applies after (the first time: it is applied already, so nothing happens).
     * @return true
     */
    bool perform() override
    {
        if ( m_first )
        {
            m_first = false;
            return true;
        }
        m_p.applyUndoState( m_after );
        return true;
    }
    /**
     * @brief Puts before back.
     * @return true
     */
    bool undo() override
    {
        m_p.applyUndoState( m_before );
        return true;
    }
    /**
     * @brief The memory the step takes, roughly (the history's limit).
     * @return the size in bytes
     */
    int getSizeInUnits() override { return static_cast<int>( m_before.m_knobs.size() + m_after.m_knobs.size() ) + 64; }

private:
    PhospheneProcessor           &m_p;              ///< the processor
    PhospheneProcessor::UndoState m_before;         ///< the state before the step
    PhospheneProcessor::UndoState m_after;          ///< the state after it
    bool                          m_first = true;   ///< perform() has not run yet (the change is made already)
};

}   // namespace

bool PhospheneProcessor::UndoState::operator==( const UndoState &o ) const
{
    if ( m_knobs != o.m_knobs || m_seed != o.m_seed ) return false;
    for ( int u = 0; u < NumLockUnits; ++u )
        if ( m_locks[u] != o.m_locks[u] || m_variations[u] != o.m_variations[u] ) return false;
    return true;
}

PhospheneProcessor::UndoState PhospheneProcessor::captureUndoState() const
{
    UndoState s;
    s.m_knobs = params().toText( false );
    s.m_seed = m_seed.load( std::memory_order_relaxed );
    const std::lock_guard<std::mutex> lock( m_curationLock );
    for ( int u = 0; u < NumLockUnits; ++u )
    {
        s.m_locks[u] = m_lockMirror[u];
        s.m_variations[u] = m_variationMirror[u];
    }
    return s;
}

void PhospheneProcessor::applyUndoState( const UndoState &s )
{
    const bool was = m_restoringUndo;
    m_restoringUndo = true;
    params().parseText( s.m_knobs );
    if ( s.m_seed != m_seed.load( std::memory_order_relaxed ) ) setSeed( s.m_seed );
    // The curation as a difference against the mirror, sent to the composer like any lock or reroll; a reroll
    // counter that moves moves notes, so the transport restarts at the bar that is playing, as a reroll does.
    std::vector<CurationCommand> cmds;
    bool                         moved = false;
    {
        const std::lock_guard<std::mutex> lock( m_curationLock );
        for ( int u = 0; u < NumLockUnits; ++u )
        {
            for ( const auto &kv : m_lockMirror[u] )
                if ( s.m_locks[u].count( kv.first ) == 0 )
                    cmds.push_back( { static_cast<uint8_t>( u ), 0, kv.first, 0 } );
            for ( const auto &kv : s.m_locks[u] )
                if ( m_lockMirror[u].count( kv.first ) == 0 )
                    cmds.push_back( { static_cast<uint8_t>( u ), 1, kv.first, 0 } );
            std::map<int, uint32_t> keys = m_variationMirror[u];
            for ( const auto &kv : s.m_variations[u] ) keys[kv.first] = 0;
            for ( const auto &kv : keys )
            {
                const auto     a = m_variationMirror[u].find( kv.first );
                const auto     b = s.m_variations[u].find( kv.first );
                const uint32_t now = a == m_variationMirror[u].end() ? 0u : a->second,
                               want = b == s.m_variations[u].end() ? 0u : b->second;
                if ( now != want )
                {
                    cmds.push_back( { static_cast<uint8_t>( u ), 4, kv.first, want } );
                    moved = true;
                }
            }
            m_lockMirror[u] = s.m_locks[u];
            m_variationMirror[u] = s.m_variations[u];
        }
    }
    if ( moved ) markCurrentPosition();
    for ( const CurationCommand &c : cmds ) m_curation.push( c );
    m_restoringUndo = was;
}

void PhospheneProcessor::recordUndo( const juce::String &name, const UndoState &before )
{
    if ( m_restoringUndo ) return;
    UndoState after = captureUndoState();
    if ( after == before ) return;
    m_undo.beginNewTransaction( name );
    m_undo.perform( new UndoStep( *this, before, std::move( after ) ), name );
}

void PhospheneProcessor::undoable( const juce::String &name, const std::function<void()> &action )
{
    const UndoState before = captureUndoState();
    action();
    recordUndo( name, before );
}

bool PhospheneProcessor::undo()
{
    return m_undo.undo();
}

bool PhospheneProcessor::redo()
{
    return m_undo.redo();
}

void PhospheneProcessor::parameterGestureChanged( int parameterIndex, bool gestureIsStarting )
{
    // Only the editor's gestures, which come on the message thread; a host touching an automation lane may call
    // this from its own threads, and automation is the host's history, not this one.
    if ( m_restoringUndo || !juce::MessageManager::existsAndIsCurrentThread() ) return;
    if ( gestureIsStarting )
    {
        if ( m_gestureDepth++ == 0 )
        {
            m_gestureBefore = captureUndoState();
            const auto &all = getParameters();
            m_gestureName = parameterIndex >= 0 && parameterIndex < all.size() ? all[parameterIndex]->getName( 64 )
                                                                               : juce::String( "knob" );
            // A synth playing the composer's preset (26.09.2026): the knob the user takes becomes the sound's owner --
            // Own Sound on, the knobs standing at the preset's values -- in the same undo step as the turn.
            if ( parameterIndex >= 0 && parameterIndex < all.size() )
                if ( auto *spar = dynamic_cast<StoreParameter *>( all[parameterIndex] ) )
                {
                    int           local = -1;
                    const int     k = soundSynthOfId( spar->storeId(), local );
                    const ModuleE m = k == 0   ? ModuleE::KICK
                                      : k == 1 ? ModuleE::BASS
                                      : k == 2 ? ModuleE::ACID
                                               : ModuleE::POLY;
                    const int     own = params().base( ModuleE::MIX ) + mix::KICK_OWN + k;
                    if ( k >= 0 && !presetLeaves( m, local ) && !params().getBool( own ) && m_shownPreset[k] >= 0 )
                    {
                        takeOverPreset( k );
                        if ( StoreParameter *o = parameterFor( own ) ) o->setValueNotifyingHost( 1.0f );
                    }
                }
        }
    }
    else if ( m_gestureDepth > 0 && --m_gestureDepth == 0 )
    {
        recordUndo( m_gestureName, m_gestureBefore );
    }
}

void PhospheneProcessor::setSeed( uint64_t s )
{
    {
        const std::lock_guard<std::mutex> lock( m_composeLock );
        m_seed.store( s, std::memory_order_relaxed );
        m_composer->setSeed( s );
    }
    m_plansStale.store( true, std::memory_order_release );
    seekToBar( 0 );
}

void PhospheneProcessor::randomiseSeed()
{
    // The clock and the address of this object: two things that differ between runs, so "another
    // set" never gives the same one twice in a session.
    const uint64_t t = static_cast<uint64_t>( juce::Time::getHighResolutionTicks() );
    uint64_t       x = t ^ ( reinterpret_cast<uintptr_t>( this ) * 0x9E3779B97F4A7C15ull );
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    setSeed( 1 + ( x % 1000000000ull ) );
}

bool PhospheneProcessor::tryReadTrack( int index, TrackPlan &out ) const
{
    // The published copy, not the composer itself: the editor must never wait on a probe render.
    const std::lock_guard<std::mutex> lock( m_plansLock );
    if ( index < 0 || index >= static_cast<int>( m_plans.size() ) ) return false;
    out = m_plans[static_cast<size_t>( index )];
    return true;
}

// ------------------------------------------------------------------ curation (PLAN 6.8)

void PhospheneProcessor::markCurrentPosition()
{
    // Where the music is now, so that the restart a curation command triggers lands here again
    // rather than at the top of the set. Under a host the drift check corrects it a block later.
    const int bar = juce::jmax( 0, static_cast<int>( m_musicalBeat.load( std::memory_order_relaxed ) / BeatsPerBar ) );
    m_pendingBar.store( bar, std::memory_order_relaxed );
    m_pendingOffset.store( static_cast<double>( bar ) * BeatsPerBar, std::memory_order_relaxed );
}

void PhospheneProcessor::setLock( LockUnitE unit, int index, bool locked )
{
    {
        const std::lock_guard<std::mutex> lock( m_curationLock );
        auto                             &m = m_lockMirror[static_cast<int>( unit )];
        if ( locked )
            m[index] = 1;
        else
            m.erase( index );
    }
    // A lock changes no note by itself -- it only decides what a later reroll may move -- so it
    // does not restart the transport; the mirror is enough for the editor, and the composer picks
    // the command up on its next turn.
    m_curation.push( CurationCommand{ static_cast<uint8_t>( unit ), static_cast<uint8_t>( locked ? 1 : 0 ), index } );
}

bool PhospheneProcessor::isLocked( LockUnitE unit, int index ) const
{
    const std::lock_guard<std::mutex> lock( m_curationLock );
    const auto                       &m = m_lockMirror[static_cast<int>( unit )];
    return m.find( index ) != m.end();
}

void PhospheneProcessor::reroll( LockUnitE unit, int index )
{
    {
        const std::lock_guard<std::mutex> lock( m_curationLock );
        if ( m_lockMirror[static_cast<int>( unit )].count( index ) != 0 ) return;   // a locked unit does not move
        ++m_variationMirror[static_cast<int>( unit )][index];
    }
    markCurrentPosition();
    m_curation.push( CurationCommand{ static_cast<uint8_t>( unit ), 2, index } );
}

uint32_t PhospheneProcessor::variation( LockUnitE unit, int index ) const
{
    const std::lock_guard<std::mutex> lock( m_curationLock );
    const auto                       &m = m_variationMirror[static_cast<int>( unit )];
    const auto                        it = m.find( index );
    return it == m.end() ? 0u : it->second;
}

void PhospheneProcessor::clearCuration()
{
    {
        const std::lock_guard<std::mutex> lock( m_curationLock );
        for ( int u = 0; u < NumLockUnits; ++u )
        {
            m_lockMirror[u].clear();
            m_variationMirror[u].clear();
        }
    }
    markCurrentPosition();
    m_curation.push( CurationCommand{ 0, 3, 0 } );
}

void PhospheneProcessor::syncCurationMirror()
{
    // After a .phosset has been read the composer holds locks the editor has never seen.
    const std::lock_guard<std::mutex> lock( m_curationLock );
    for ( int u = 0; u < NumLockUnits; ++u )
    {
        m_lockMirror[u] = m_composer->locks( static_cast<LockUnitE>( u ) );
        m_variationMirror[u] = m_composer->variations( static_cast<LockUnitE>( u ) );
    }
}

// ------------------------------------------------------------------ perform macros

int PhospheneProcessor::macroTargets( MacroE m, float value, MacroTarget *out ) const
{
    const ParamStore &p = params();
    const int         acidBase = p.base( ModuleE::ACID );
    const int         lead = p.base( PolyInstanceE::LEAD ), counter = p.base( PolyInstanceE::COUNTER );
    const int         arp = p.base( PolyInstanceE::ARP ), stab = p.base( PolyInstanceE::STAB );
    const int         pad = p.base( PolyInstanceE::PAD );
    const int         mixBase = p.base( ModuleE::MIX );
    int               n = 0;
    auto              add = [&]( int id, float v, bool absolute )
    {
        if ( id >= 0 && n < MaxMacroTargets ) out[n++] = MacroTarget{ id, v, absolute };
    };
    if ( value == 0.0f ) return 0;   // neutral: the macro wants nothing, and its knobs go back
    switch ( m )
    {
    case MacroE::FILTER_SWEEP:
    {
        // A third of the normalised range each way. The three melodic filters move together,
        // which is what makes it one gesture instead of three knobs.
        const float d = juce::jlimit( -1.0f, 1.0f, value ) * 0.35f;
        add( acidBase + acid::CUTOFF, d, false );
        add( lead + poly::CUTOFF, d, false );
        add( arp + poly::CUTOFF, d, false );
        add( counter + poly::CUTOFF, d, false );   // 19.09.2026: the counter-lead and the stab move with them
        add( stab + poly::CUTOFF, d, false );
        break;
    }
    case MacroE::GATE_DEPTH:
    {
        const float v = juce::jlimit( 0.0f, 1.0f, value );
        for ( int base : { lead, counter, arp, stab, pad } )
        {
            add( base + poly::GATE, 1.0f, true );
            add( base + poly::GATE_DEPTH, v, true );
        }
        break;
    }
    case MacroE::DROP_OUT:
        add( mixBase + mix::KICK_MUTE, 1.0f, true );
        add( mixBase + mix::BASS_MUTE, 1.0f, true );
        break;
    case MacroE::STUTTER:
        for ( int base : { lead, counter, arp, stab, pad } )
        {
            add( base + poly::GATE, 1.0f, true );
            add( base + poly::GATE_PATTERN, 0.0f, true );   // Sixteenths
            add( base + poly::GATE_DEPTH, 1.0f, true );
            add( base + poly::GATE_DUTY, 0.0f, true );   // the shortest opening the knob allows
        }
        break;
    default: break;
    }
    return n;
}

void PhospheneProcessor::setMacro( MacroE m, float value )
{
    const int i = static_cast<int>( m );
    if ( i < 0 || i >= NumMacros ) return;
    m_macro[i].m_value = juce::jlimit( m == MacroE::FILTER_SWEEP ? -1.0f : 0.0f, 1.0f, value );
    // A drop-out is one press and one bar: it lets go on the next bar line, wherever the press
    // fell. The release is a message-thread job, so it lands within a tick of the downbeat.
    if ( m == MacroE::DROP_OUT && m_macro[i].m_value > 0.0f )
    {
        const double beat = m_musicalBeat.load( std::memory_order_relaxed );
        m_macro[i].m_releaseBeat = ( std::floor( beat / BeatsPerBar ) + 1.0 ) * BeatsPerBar;
    }
    else if ( m_macro[i].m_value == 0.0f )
    {
        m_macro[i].m_releaseBeat = -1.0;
    }
    // The host parameter follows (MIDI learn round): the editor's knob and the DAW's lane show the same thing.
    if ( auto *hp = m_macroParam[i] )
    {
        m_macroHostSeen[i] = m_macro[i].m_value;
        const float norm = hp->convertTo0to1( macroIsMomentary( m ) ? ( m_macro[i].m_value > 0.0f ? 1.0f : 0.0f )
                                                                    : m_macro[i].m_value );
        if ( std::fabs( hp->getValue() - norm ) > 1.0e-6f ) hp->setValueNotifyingHost( norm );
    }
    serviceMacros();
}

void PhospheneProcessor::serviceMacros()
{
    const std::lock_guard<std::mutex> lock( m_macroBaseLock );
    // A macro's host parameter moved (DAW automation, a learned controller): the macro follows. Compared with
    // what was seen last, not with the macro's own value, so a drop-out that let go by itself is not pressed
    // again by a host value that still says 1.
    for ( int i = 0; i < NumMacros; ++i )
    {
        auto *hp = m_macroParam[i];
        if ( hp == nullptr ) continue;
        const MacroE m = static_cast<MacroE>( i );
        float        v = hp->convertFrom0to1( hp->getValue() );
        if ( macroIsMomentary( m ) ) v = v > 0.5f ? 1.0f : 0.0f;
        if ( std::fabs( v - m_macroHostSeen[i] ) > 1.0e-6f )
        {
            m_macroHostSeen[i] = v;
            m_macro[static_cast<size_t>( i )].m_value =
                juce::jlimit( m == MacroE::FILTER_SWEEP ? -1.0f : 0.0f, 1.0f, v );
            if ( m == MacroE::DROP_OUT && v > 0.0f )
            {
                const double b = m_musicalBeat.load( std::memory_order_relaxed );
                m_macro[static_cast<size_t>( i )].m_releaseBeat = ( std::floor( b / BeatsPerBar ) + 1.0 ) * BeatsPerBar;
            }
            else if ( v == 0.0f )
            {
                m_macro[static_cast<size_t>( i )].m_releaseBeat = -1.0;
            }
        }
    }
    const double beat = m_musicalBeat.load( std::memory_order_relaxed );
    const bool   playing =
        m_playRequest.load( std::memory_order_relaxed ) || m_hostSyncNow.load( std::memory_order_relaxed );
    for ( MacroState &s : m_macro )
    {
        if ( s.m_releaseBeat < 0.0 ) continue;
        // A transport that is not running has no bar line to wait for.
        if ( beat >= s.m_releaseBeat || !playing )
        {
            s.m_value = 0.0f;
            s.m_releaseBeat = -1.0;
        }
    }
    // A macro that let go by itself takes its host parameter with it.
    for ( int i = 0; i < NumMacros; ++i )
    {
        auto *hp = m_macroParam[i];
        if ( hp == nullptr || m_macro[static_cast<size_t>( i )].m_value != 0.0f || m_macroHostSeen[i] == 0.0f )
            continue;
        m_macroHostSeen[i] = 0.0f;
        hp->setValueNotifyingHost( hp->convertTo0to1( 0.0f ) );
    }

    // What the macros want, summed: two macros may ask for the same knob (gate depth and stutter
    // both do), and the stronger wish wins rather than the later one.
    MacroTarget want[NumMacros * MaxMacroTargets];
    int         n = 0;
    for ( int i = 0; i < NumMacros; ++i )
    {
        MacroTarget t[MaxMacroTargets];
        const int   c = macroTargets( static_cast<MacroE>( i ), m_macro[static_cast<size_t>( i )].m_value, t );
        for ( int k = 0; k < c; ++k )
        {
            int found = -1;
            for ( int j = 0; j < n; ++j )
                if ( want[j].m_param == t[k].m_param )
                {
                    found = j;
                    break;
                }
            if ( found < 0 )
            {
                want[n++] = t[k];
                continue;
            }
            if ( t[k].m_absolute && want[found].m_absolute )
                want[found].m_value = juce::jmax( want[found].m_value, t[k].m_value );
            else if ( t[k].m_absolute )
                want[found] = t[k];
            else
                want[found].m_value += t[k].m_value;
        }
    }

    // The family jam (02.10.2026): the leader's energy as a filter sweep of the jam's own -- closed by up to 0.6 at low
    // energy, gliding at the timer's pace --, wanted only while the Filter Sweep macro rests (the hand comes first).
    {
        const float e = m_jamEnergyOut.load( std::memory_order_relaxed );
        const float target = e >= 0.0f ? ( juce::jlimit( 0.0f, 1.0f, e ) - 1.0f ) * 0.6f : 0.0f;
        m_jamSweep += ( target - m_jamSweep ) * 0.15f;
        if ( std::fabs( m_jamSweep ) > 0.01f && m_macro[static_cast<size_t>( MacroE::FILTER_SWEEP )].m_value == 0.0f )
        {
            MacroTarget t[MaxMacroTargets];
            const int   c = macroTargets( MacroE::FILTER_SWEEP, m_jamSweep, t );
            for ( int k = 0; k < c && n < NumMacros * MaxMacroTargets; ++k )
            {
                int found = -1;
                for ( int j = 0; j < n; ++j )
                    if ( want[j].m_param == t[k].m_param )
                    {
                        found = j;
                        break;
                    }
                if ( found < 0 )
                    want[n++] = t[k];
                else if ( !t[k].m_absolute && !want[found].m_absolute )
                    want[found].m_value += t[k].m_value;
            }
        }
    }
    ParamStore &p = params();
    for ( int i = 0; i < n; ++i )
    {
        const int id = want[i].m_param;
        // The value is remembered as the store keeps it, not as a normalised position: putting a
        // knob back must be exact, and a round trip through a logarithmic mapping is not.
        const auto  it = m_macroBase.find( id );
        const float base = it != m_macroBase.end() ? it->second : ( m_macroBase[id] = p.get( id ) );
        const float target = juce::jlimit(
            0.0f, 1.0f, want[i].m_absolute ? want[i].m_value : p.toNormalised( id, base ) + want[i].m_value );
        p.setNormalised( id, target );
    }
    // Everything no macro wants any more goes back to the value it had before any macro touched it.
    for ( auto it = m_macroBase.begin(); it != m_macroBase.end(); )
    {
        bool wanted = false;
        for ( int i = 0; i < n; ++i )
            if ( want[i].m_param == it->first )
            {
                wanted = true;
                break;
            }
        if ( wanted )
        {
            ++it;
            continue;
        }
        p.set( it->first, it->second );
        it = m_macroBase.erase( it );
    }
}

TransportView PhospheneProcessor::transport() const
{
    TransportView v;
    v.m_playing = m_playRequest.load( std::memory_order_relaxed ) || m_hostSyncNow.load( std::memory_order_relaxed );
    v.m_hostSync = m_hostSyncNow.load( std::memory_order_relaxed );
    v.m_musicalBeat = m_musicalBeat.load( std::memory_order_relaxed );
    v.m_bpm = m_bpmNow.load( std::memory_order_relaxed );
    v.m_bar = static_cast<int>( v.m_musicalBeat / BeatsPerBar );
    v.m_restarting = m_genPrimed.load( std::memory_order_relaxed ) != m_genWanted.load( std::memory_order_relaxed );
    // Which track that bar belongs to, from the published plans rather than from the composer: the
    // editor asks for this at every tick and must never wait behind a probe render.
    {
        const std::lock_guard<std::mutex> lock( m_plansLock );
        for ( const TrackPlan &p : m_plans )
            if ( v.m_bar >= p.m_firstBar && v.m_bar < p.m_firstBar + p.m_bars )
            {
                v.m_track = p.m_index;
                v.m_barInTrack = v.m_bar - p.m_firstBar;
                break;
            }
    }
    return v;
}

// ------------------------------------------------------------------ MIDI out

void PhospheneProcessor::emitMidi( juce::MidiBuffer &midi, double beatAtStart, double beatsPerSample, int n )
{
    const double endBeat = beatAtStart + beatsPerSample * n;
    auto         sampleOf = [&]( double beat )
    {
        if ( beatsPerSample <= 0.0 ) return 0;
        return juce::jlimit( 0, n - 1, static_cast<int>( ( beat - beatAtStart ) / beatsPerSample ) );
    };
    // Note-offs that fall in this block, first: a re-used slot must be free before a note-on takes it.
    for ( HeldNote &h : m_held )
    {
        if ( !h.m_active || h.m_endBeat >= endBeat ) continue;
        midi.addEvent( juce::MidiMessage::noteOff( h.m_channel + 1, h.m_pitch ), sampleOf( h.m_endBeat ) );
        h.m_active = false;
    }
    NoteEvent e;
    while ( m_midiRing.peek( e ) )
    {
        if ( e.m_beat >= endBeat ) break;
        m_midiRing.pop( e );
        if ( m_engine->generatedSilenced( e.m_part ) ) continue;   // the keyboard plays this voice (Replace)
        if ( m_engine->jamSilenced( e.m_part ) ) continue;         // the family jam: the leader's break
        e.m_pitch = static_cast<decltype( e.m_pitch )>( m_engine->jamPitch( e.m_part, e.m_pitch ) );   // and its root
        const int channel = midiChannelOf( e.m_part );
        const int at = sampleOf( e.m_beat );
        midi.addEvent( juce::MidiMessage::noteOn(
                           channel + 1, e.m_pitch,
                           static_cast<juce::uint8>( juce::jlimit( 1, 127, static_cast<int>( e.m_velocity ) ) ) ),
                       at );
        const double off = e.m_beat + juce::jmax( 0.02, static_cast<double>( e.m_length ) );
        if ( off < endBeat )
        {
            midi.addEvent( juce::MidiMessage::noteOff( channel + 1, e.m_pitch ), juce::jmax( at, sampleOf( off ) ) );
            continue;
        }
        HeldNote *slot = nullptr;
        for ( HeldNote &h : m_held )
            if ( !h.m_active )
            {
                slot = &h;
                break;
            }
        if ( slot == nullptr ) continue;   // more than 128 notes sounding at once: the off would be lost anyway
        slot->m_active = true;
        slot->m_endBeat = off;
        slot->m_channel = channel;
        slot->m_pitch = e.m_pitch;
    }
}

// ------------------------------------------------------------------ the audio thread

void PhospheneProcessor::processBlock( juce::AudioBuffer<float> &buffer, juce::MidiBuffer &midiMessages )
{
    const juce::ScopedNoDenormals noDenormals;
    const int                     n = buffer.getNumSamples();
    buffer.clear();
    // MIDI in (23.09.2026): controllers drive whatever was learned onto them (phos/MidiMap.h); notes go to the
    // voice mix.keyboard_part names (Engine::liveNoteOn) at their sample, below. The buffer is then cleared,
    // because it is also the generator's MIDI out.
    int liveCount = 0;
    for ( const juce::MidiMessageMetadata meta : midiMessages )
    {
        const juce::MidiMessage msg = meta.getMessage();
        if ( ( msg.isNoteOn() || msg.isNoteOff() ) && liveCount < MaxLive )
        {
            // The keyboard's options (02.10.2026): the keys under the split play the lower voice, Scale Lock moves a key
            // to the nearest note of the track's key and scale (not on the kit, whose keys are its instruments), the
            // velocity goes through its curve. Where a press went, its release goes too.
            const int ch = msg.getChannel() - 1, key = msg.getNoteNumber();
            LiveNote  ln{ juce::jlimit( 0, juce::jmax( 0, n - 1 ), meta.samplePosition ), key, msg.getVelocity(), ch,
                          msg.isNoteOn() };
            if ( ln.m_on )
            {
                const int mb = params().base( ModuleE::MIX );
                const int lower = static_cast<int>( std::lround( params().get( mb + phos::mix::KEYBOARD_LOWER ) ) );
                const int split =
                    36 + 12 * static_cast<int>( std::lround( params().get( mb + phos::mix::KEYBOARD_SPLIT ) ) );
                ln.m_part = lower > 0 && key < split ? ( lower <= 7 ? lower : lower + 1 )
                                                     : -1;   // Bass and Kit sit past By channel
                const int part = ln.m_part > 0
                                     ? ln.m_part
                                     : static_cast<int>( std::lround( params().get( mb + phos::mix::KEYBOARD_PART ) ) );
                const bool kit = part == 10 || ( part == 8 && ch == 9 );
                if ( params().get( mb + phos::mix::KEYBOARD_SCALE ) >= 0.5f && !kit )
                    ln.m_pitch = frame::snapToScale(
                        key, m_engine->keyRoot(),
                        frame::scaleMask( phos::ScaleNames[juce::jlimit( 0, 5, m_engine->scale() )] ) );
                ln.m_velocity = frame::shapeVelocity(
                    ln.m_velocity,
                    static_cast<int>( std::lround( params().get( mb + phos::mix::KEYBOARD_VELOCITY ) ) ) );
                m_keyMemory.press( ch, key, ln.m_part, ln.m_pitch );
            }
            else
            {
                m_keyMemory.release( ch, key, ln.m_part, ln.m_pitch );
            }
            m_live[liveCount++] = ln;
            continue;
        }
        if ( msg.isAllNotesOff() || msg.isAllSoundOff() )
        {
            m_engine->liveAllOff();
            m_keyMemory.clear();
            continue;
        }
        if ( !msg.isController() ) continue;
        float     norm = 0.0f;
        const int target =
            m_midiMap.handleCc( msg.getChannel() - 1, msg.getControllerNumber(), msg.getControllerValue(), norm );
        if ( auto *hp = hostParameter( target ) ) hp->setValueNotifyingHost( norm );
    }
    midiMessages.clear();
    if ( n <= 0 ) return;

    // ---------------------------------------------------------------- where the clock comes from
    bool   playing = m_playRequest.load( std::memory_order_relaxed );
    bool   hostSync = false;
    double hostBeat = 0.0;
    if ( m_followHostAtomic.load( std::memory_order_relaxed ) )
    {
        if ( auto *ph = getPlayHead() )
        {
            if ( const auto pos = ph->getPosition() )
            {
                if ( const auto ppq = pos->getPpqPosition() )
                {
                    hostSync = true;
                    hostBeat = *ppq;
                    playing = pos->getIsPlaying();
                    if ( const auto bpm = pos->getBpm() )
                        if ( *bpm > 1.0 ) m_hostBpm.store( *bpm, std::memory_order_relaxed );
                }
            }
        }
    }
    // Ableton Link (02.10.2026, LinkClock.h), the standalone only: with other apps in the session it rules as a host
    // does -- its tempo (compose.bpm below), its bar phase (the conductor's offset, after the restarts), its start and
    // stop; alone in it, the standalone leads (its tempo goes out after the render).
    bool linkFollow = false;
    if ( !hostSync && m_link.enabled() )
    {
        const double                latency = static_cast<double>( getLatencySamples() + n ) / m_sampleRate;
        const frame::LinkClock::Now ln = m_link.capture( latency, 4.0 );
        linkFollow = ln.m_peers > 0;
        if ( linkFollow )
        {
            if ( playing != m_linkPlayed )
            {
                m_link.setPlaying( playing, latency, 4.0 );   // Play or Stop pressed here: to the session
            }
            else if ( ln.m_playing != playing )
            {
                playing = ln.m_playing;   // pressed elsewhere: here too
                m_playRequest.store( playing, std::memory_order_relaxed );
            }
            m_hostBpm.store( ln.m_bpm, std::memory_order_relaxed );
            m_linkBeat = ln.m_beat;
        }
        m_linkPlayed = playing;
    }
    m_linkFollowing.store( linkFollow, std::memory_order_relaxed );
    m_hostSyncNow.store( hostSync, std::memory_order_relaxed );
    // PHOS_TRACE=1: a line every hundred blocks on stderr saying what the transport and the
    // handshake are doing. The only way to see inside a plugin that a host has loaded and that is
    // not making a sound.
    if ( m_trace && ++m_traceCount % 100 == 0 )
        std::fprintf(
            stderr, "[phos] play=%d sync=%d beat=%.2f want=%d ack=%d reset=%d primed=%d mute=%d follow=%d wrapper=%d\n",
            static_cast<int>( playing ), static_cast<int>( hostSync ), hostBeat, m_genWanted.load(), m_genAck.load(),
            m_genReset.load(), m_genPrimed.load(), static_cast<int>( m_mute.load() ),
            static_cast<int>( m_followHostAtomic.load() ), static_cast<int>( wrapperType ) );
    // Under a host the tempo is the host's. compose.bpm is written directly rather than through the
    // host parameter, so the value the user sees follows the transport without an automation fight;
    // "Follow host" in the Set tab switches the whole arrangement off.
    if ( hostSync || linkFollow )
    {
        const double bpm = m_hostBpm.load( std::memory_order_relaxed );
        const int    id = params().base( ModuleE::COMPOSE ) + compose::BPM;
        if ( bpm > 1.0 && std::fabs( static_cast<double>( params().get( id ) ) - bpm ) > 0.005 )
            params().set( id, static_cast<float>( bpm ) );
    }

    // ---------------------------------------------------------------- restarts
    //
    // Never ask for a second restart while one is in flight. The composer can need seconds for the
    // first bar of a track it has not planned yet, and the host's play head keeps running through
    // all of them -- so the distance between the two clocks grows while we wait, and asking again
    // for every block of that wait would cancel the answer just before it arrived, over and over.
    // That livelock is what the VST3 test caught: the plugin planned forever and never played.
    const bool restarting =
        m_genPrimed.load( std::memory_order_acquire ) != m_genWanted.load( std::memory_order_acquire );
    const bool readyNow =
        m_genPrimed.load( std::memory_order_acquire ) == m_genWanted.load( std::memory_order_acquire ) &&
        m_genReset.load( std::memory_order_relaxed ) == m_genWanted.load( std::memory_order_acquire );
    if ( m_restartRequest.exchange( false, std::memory_order_acq_rel ) )
    {
        // 22.09.2026, the user: "Klicken direkt im Arrangement-View fuehrt zu schlimmsten
        // Stoergeraeuschen". A click there is a seek, and a seek used to take effect inside this very
        // block: m_genWanted moved, the `ready` test below failed, and the block came out silent --
        // from whatever level the waveform happened to be at. The host test measured a step of
        // 0.0235 on a quiet passage, and the size of that step is simply the instantaneous signal, so
        // on a kick it is most of full scale. A step is broadband; once per mouse click it is exactly
        // what the user described.
        //
        // So the seek waits for the end of this block: the block renders as it would have, a fade to
        // zero is laid over it, and only then is the request made. The composer thread cannot be
        // repositioning while we render, because it waits for m_genWanted -- which is the reason the
        // request has to go *after* the render and not before it. When nothing is sounding (stopped,
        // or a restart already in flight) there is nothing to fade and the seek goes at once.
        if ( playing && readyNow )
        {
            m_fadeOutThenSeek = true;
        }
        else
        {
            requestSeek( m_pendingBar.load( std::memory_order_relaxed ),
                         m_pendingOffset.load( std::memory_order_relaxed ) );
            m_wasPlaying = false;
        }
    }
    else if ( hostSync && !restarting )
    {
        const double expected = m_engine->beatPosition() + m_conductor->beatOffset();
        const double drift = hostBeat - expected;
        if ( playing && ( !m_wasPlaying || std::fabs( drift ) > 0.25 ) )
        {
            // The transport started, or the host moved the play head. Everything below a bar is
            // cheap to redo, because composeBars is deterministic per bar. If the plan for that bar
            // is not cached yet this takes a moment, and the next block's drift check picks up
            // whatever the host moved on by in the meantime.
            requestSeek( static_cast<int>( std::floor( hostBeat / BeatsPerBar ) ), hostBeat );
        }
        else if ( playing && std::fabs( drift ) > 1.0e-9 )
        {
            // Rounding between two clocks, not a jump: slide the offset instead of resetting, so
            // the events stay where the host expects them and nothing clicks.
            m_conductor->nudgeOffset( drift );
        }
    }
    else if ( linkFollow && playing && m_wasPlaying && !restarting )
    {
        // The Link session's bar phase: the bars line up, the place in the set stays. Drift is nudged into the
        // conductor's offset as a host's is; a jump (another app started its bar elsewhere) is a seek.
        const double expected = m_engine->beatPosition() + m_conductor->beatOffset();
        double       d = std::fmod( m_linkBeat - expected, 4.0 );
        if ( d > 2.0 )
            d -= 4.0;
        else if ( d <= -2.0 )
            d += 4.0;
        if ( std::fabs( d ) > 0.25 )
            requestSeek( static_cast<int>( std::floor( ( expected + d ) / BeatsPerBar ) ), expected + d );
        else if ( std::fabs( d ) > 1.0e-9 )
            m_conductor->nudgeOffset( d );
    }
    else if ( !hostSync && playing && !m_wasPlaying && !restarting )
    {
        requestSeek( m_pendingBar.load( std::memory_order_relaxed ),
                     m_pendingOffset.load( std::memory_order_relaxed ) );
    }
    const bool justStopped = m_wasPlaying && !playing;
    m_wasPlaying = playing;
    // A played note belongs to the voice it went to; a transport that stops, or a keyboard moved to another voice,
    // must not leave it hanging there.
    const int keyboardPart =
        static_cast<int>( std::lround( params().get( params().base( ModuleE::MIX ) + phos::mix::KEYBOARD_PART ) ) );
    if ( justStopped || keyboardPart != m_keyboardPartSeen ) m_engine->liveAllOff();
    m_keyboardPartSeen = keyboardPart;
    if ( justStopped )
    {
        // A transport that stops in the middle of a note leaves it sounding in whatever the MIDI
        // output is wired to; the notes we started are the notes we end.
        for ( HeldNote &h : m_held )
        {
            if ( !h.m_active ) continue;
            midiMessages.addEvent( juce::MidiMessage::noteOff( h.m_channel + 1, h.m_pitch ), 0 );
            h.m_active = false;
        }
    }

    // Step 3: the composer has positioned itself, so the engine may be cleared for the new start.
    const int want = m_genWanted.load( std::memory_order_acquire );
    auto      resetForNewStart = [this, want]
    {
        if ( m_genAck.load( std::memory_order_acquire ) != want ||
             m_genReset.load( std::memory_order_relaxed ) == want )
            return;
        m_engine->reset();
        NoteEvent junk;
        while ( m_midiRing.pop( junk ) )
        {
        }
        for ( HeldNote &h : m_held ) h = HeldNote{};
        m_genReset.store( want, std::memory_order_release );
    };
    resetForNewStart();
    // Offline (a bounce, or the host test): this thread does the composer's work itself, once per
    // block, exactly where phos_render does it -- so the block is self-contained and the render is
    // bit-identical. The loop is for the handshake, which needs up to three steps on a restart.
    if ( isNonRealtime() )
    {
        for ( int i = 0; i < 3; ++i )
        {
            serviceComposer( false );
            resetForNewStart();
            if ( m_genPrimed.load( std::memory_order_relaxed ) == want ) break;
        }
    }

    const bool ready =
        m_genPrimed.load( std::memory_order_acquire ) == want && m_genReset.load( std::memory_order_relaxed ) == want;
    if ( !playing || !ready )
    {
        m_musicalBeat.store( m_engine->beatPosition() + m_conductor->beatOffset(), std::memory_order_relaxed );
        return;   // silence: either stopped, or a restart is in flight
    }

    // ---------------------------------------------------------------- render
    const double beatAtStart = m_engine->beatPosition();
    // The family jam's shared timeline (Jam.h): the host's beat in a DAW, Link's with others in the session; none alone.
    const double jamShared = hostSync ? hostBeat : linkFollow ? m_linkBeat : -1.0;
    jamFollow( beatAtStart + m_conductor->beatOffset(), jamShared );
    float *L = buffer.getWritePointer( 0 );
    // The main bus only: with the stems' outputs on, the channels after it are theirs (02.10.2026).
    const auto *mainBus = getBus( false, 0 );
    float      *R = ( mainBus != nullptr ? mainBus->getNumberOfChannels() : buffer.getNumChannels() ) > 1
                        ? buffer.getWritePointer( 1 )
                        : nullptr;
    {   // The stems' outputs (02.10.2026): the engine writes its StemTap while the host has one of their buses on.
        bool want = false;
        for ( int b = 1; b < getBusCount( false ) && !want; ++b )
            if ( const auto *bus = getBus( false, b ) ) want = bus->isEnabled();
        want = want && n <= m_stemBlock;   // the stems' buffers hold the prepared block
        if ( want != m_stemsOn )
        {
            m_engine->setStemTap( want ? &m_stemTap : nullptr );
            m_stemsOn = want;
        }
    }
    auto render = [&]( int from, int count )
    {
        if ( count <= 0 ) return;
        if ( m_stemsOn )   // every render of a split block writes its stems from its own start
            for ( int s = 0; s < phos::NumStems; ++s )
            {
                m_stemTap.m_L[s] = m_stemL[static_cast<size_t>( s )] + from;
                m_stemTap.m_R[s] = m_stemR[static_cast<size_t>( s )] + from;
            }
        if ( R == nullptr )
        {
            // Mono device: render the stereo pair into the scratch and fold it down.
            const int m = juce::jmin( count, m_scratch.getNumSamples() - from );
            if ( m <= 0 ) return;
            m_engine->process( m_scratch.getWritePointer( 0, from ), m_scratch.getWritePointer( 1, from ), m );
            for ( int i = from; i < from + m; ++i )
                L[i] = 0.5f * ( m_scratch.getReadPointer( 0 )[i] + m_scratch.getReadPointer( 1 )[i] );
        }
        else
        {
            m_engine->process( L + from, R + from, count );
        }
    };
    // The effects page's audition (previewSfx): a live action like a played note, at the block's start.
    if ( const int pv = m_sfxPreview.exchange( -1, std::memory_order_acq_rel ); pv >= 0 )
        m_engine->previewSfx( pv >> 12, pv & 0xFFF );
    // Played notes split the block at their samples; a block without one is a single process() call, exactly as
    // before, so a set nobody plays along to stays bit-identical to phos_render.
    int done = 0;
    for ( int k = 0; k < liveCount; ++k )
    {
        const LiveNote &ln = m_live[k];
        render( done, ln.m_at - done );
        done = juce::jmax( done, ln.m_at );
        if ( ln.m_on )
            m_engine->liveNoteOn( ln.m_pitch, ln.m_velocity, ln.m_channel, ln.m_part );
        else
            m_engine->liveNoteOff( ln.m_pitch, ln.m_channel );
    }
    render( done, n - done );
    if ( m_stemsOn )
        for ( int b = 1; b < getBusCount( false ) && b - 1 < phos::NumStems; ++b )
        {
            const auto *bus = getBus( false, b );
            if ( bus == nullptr || !bus->isEnabled() ) continue;
            auto out = getBusBuffer( buffer, false, b );
            if ( out.getNumChannels() < 2 ) continue;
            out.copyFrom( 0, 0, m_stemL[static_cast<size_t>( b - 1 )], n );
            out.copyFrom( 1, 0, m_stemR[static_cast<size_t>( b - 1 )], n );
        }
    if ( m_link.enabled() && !linkFollow && !hostSync )   // alone in the Link session: it takes this set's tempo
        m_link.proposeTempo(
            60.0 * ( m_engine->beatPosition() - beatAtStart ) * m_sampleRate / static_cast<double>( n ), 0.0 );
    {
        // The channel meters: raised here, taken by the mixer page (takeChannelMeters). One writer, so a load and a
        // store will do; a block the editor takes in between is at worst counted in the next reading.
        float     pk[phos::NumParts];
        double    ss[phos::NumParts];
        const int got = m_engine->takeMeters( pk, ss );
        // What every parameter played at, for the editor's live ring (26.09.2026): one relaxed store each, a few
        // microseconds a block.
        m_engine->playedValues( m_playedScratch.data() );   // with the modulation of the voice one hears (Engine.h)
        for ( int id = 0; id < m_playedCount; ++id )
            m_played[static_cast<size_t>( id )].store( m_playedScratch[static_cast<size_t>( id )],
                                                       std::memory_order_relaxed );
        if ( got > 0 )
        {
            for ( int k = 0; k < phos::NumParts; ++k )
            {
                if ( pk[k] > m_meterPeak[static_cast<size_t>( k )].load( std::memory_order_relaxed ) )
                    m_meterPeak[static_cast<size_t>( k )].store( pk[k], std::memory_order_relaxed );
                m_meterSum[static_cast<size_t>( k )].store(
                    m_meterSum[static_cast<size_t>( k )].load( std::memory_order_relaxed ) + ss[k],
                    std::memory_order_relaxed );
            }
            m_meterCount.fetch_add( got, std::memory_order_release );
        }
    }
    if ( m_fadeOutThenSeek )
    {
        // A raised cosine rather than a straight line: its first derivative is zero at both ends, so
        // neither the start of the fade nor the arrival at zero is itself a corner.
        for ( int c = 0; c < buffer.getNumChannels(); ++c )
        {
            float *d = buffer.getWritePointer( c );
            for ( int i = 0; i < n; ++i )
                d[i] *=
                    static_cast<float>( 0.5 * ( 1.0 + std::cos( juce::MathConstants<double>::pi * ( i + 1 ) / n ) ) );
        }
        m_fadeOutThenSeek = false;
        requestSeek( m_pendingBar.load( std::memory_order_relaxed ),
                     m_pendingOffset.load( std::memory_order_relaxed ) );
        m_wasPlaying = false;
    }
    const double beatAfter = m_engine->beatPosition();
    const double beatsPerSample = n > 0 ? ( beatAfter - beatAtStart ) / n : 0.0;
    m_musicalBeat.store( beatAfter + m_conductor->beatOffset(), std::memory_order_relaxed );
    m_bpmNow.store( beatsPerSample * m_sampleRate * 60.0, std::memory_order_relaxed );

    emitMidi( midiMessages, beatAtStart, beatsPerSample, n );

    // ---------------------------------------------------------------- the cue bridge (PLAN 8.3)
    //
    // Here, and nowhere earlier. The composer knows every boundary bars in advance -- which is why
    // the marks travel ahead in a ring -- but a cue is an instant, and the only instant a beat has
    // is the one at which its samples are rendered. What is rendered now is not what is heard now,
    // by two known amounts, and both make the cue early rather than late: the limiter's lookahead
    // (engine->latencySamples()) and the buffers between this block and the device. The portable
    // part of the second is the block itself -- on the usual double-buffered stream the block being
    // filled is the one after the block being played -- and `cue.lead_ms` trims whatever the
    // driver's own queue adds, the one number a plugin is never told. The tap stamps every cue with
    // that instant; the sender's thread waits for it.
    {
        m_cueTap.setSendBeats( m_cueBeats.load( std::memory_order_relaxed ) );
        const double  offset = m_conductor->beatOffset();
        const int64_t now = phos::cueNowNanos();
        const double  leadSeconds = static_cast<double>( n + m_engine->latencySamples() ) / m_sampleRate +
                                    0.001 * static_cast<double>( m_cueLeadMs.load( std::memory_order_relaxed ) );
        const int64_t leadNanos = static_cast<int64_t>( leadSeconds * 1.0e9 );
        const int64_t blockNanos = static_cast<int64_t>( static_cast<double>( n ) / m_sampleRate * 1.0e9 );
        if ( m_cueAnnounce.exchange( false, std::memory_order_acq_rel ) )
            m_cueTap.announce( now + leadNanos, m_cues.queue() );
        m_cueTap.scan( beatAtStart + offset, beatAfter + offset, now, leadNanos, blockNanos, m_cueMarks,
                       m_cues.queue() );
        jamLead( beatAtStart + offset, hostSync ? hostBeat : linkFollow ? m_linkBeat : -1.0 );
        // With the bridge off the tap still runs -- it costs a handful of comparisons, it keeps the
        // mark ring empty, and it remembers the section that is playing, so switching the bridge on
        // says where we are at once instead of waiting for the next boundary.
        if ( !m_cues.running() ) m_cues.queue().clear();
    }

    {
        const juce::ScopedTryLock lock( m_recordLock );
        if ( lock.isLocked() && m_recordWriter != nullptr )
        {
            m_recordWriter->write( buffer.getArrayOfReadPointers(), n );
            m_recordedSamples.fetch_add( n, std::memory_order_relaxed );
        }
    }
    if ( m_mute.load( std::memory_order_relaxed ) ) buffer.clear();
}

// ------------------------------------------------------------------ state

void PhospheneProcessor::writeStateTo( juce::MemoryBlock &dest ) const
{
    juce::XmlElement xml( "PHOSPHENE" );
    // The values travel as "key=value" text, so the order of the parameters never reaches a state; the
    // version says what the *content* means (PluginProcessor.h, StateVersion).
    xml.setAttribute( "version", StateVersion );
    xml.setAttribute( "seed", juce::String( m_seed.load() ) );
    xml.setAttribute( "followHost", m_followHost );
    // The cue destination is not a parameter (a parameter is a float); the port and the switch are.
    xml.setAttribute( "cueHost", cueHost() );
    xml.setAttribute( "fieldFolder", m_fieldFolder );   // 27.09.2026: the user's own field recordings
    // 20.09.2026, round "dialogue": only the knobs that differ from *this* build's defaults. A state
    // that held every value silently undid every default a later round recalibrated -- see StateVersion
    // for the whole reasoning and for what it costs. "%.9g" still round-trips a float exactly, so a
    // knob that is in the file comes back bit for bit.
    //
    // A knob a macro is holding is saved at the value it goes back to, not where the macro has it (23.09.2026: the
    // VST3 test randomised the four macro host parameters, a filter sweep moved the cutoffs, and the state no longer
    // came back byte for byte). The macros themselves are performance, not part of the set.
    std::string knobs;
    {
        const std::lock_guard<std::mutex> lock( m_macroBaseLock );
        if ( m_macroBase.empty() )
        {
            knobs = params().toText( true );
        }
        else
        {
            ParamStore base;
            base.copyValuesFrom( params() );
            for ( const auto &[id, value] : m_macroBase ) base.set( id, value );
            knobs = base.toText( true );
        }
    }
    xml.createNewChildElement( "params" )->addTextElement( juce::String( knobs ) );
    // The learned controllers, by parameter key (23.09.2026): a map outlives parameters being added.
    xml.createNewChildElement( "midimap" )
        ->addTextElement( juce::String( m_midiMap.toText( [this]( int t ) { return midiTargetKey( t ); } ) ) );
    juce::AudioProcessor::copyXmlToBinary( xml, dest );
}

void PhospheneProcessor::getStateInformation( juce::MemoryBlock &destData )
{
    writeStateTo( destData );
}

void PhospheneProcessor::setStateInformation( const void *data, int sizeInBytes )
{
    std::unique_ptr<juce::XmlElement> xml( getXmlFromBinary( data, sizeInBytes ) );
    if ( xml == nullptr || !xml->hasTagName( "PHOSPHENE" ) ) return;
    // Every knob back to its default first, then the state's own values on top. With version 3 the
    // state names only the knobs the user moved, so this is what makes a recalibrated default reach an
    // old session at all (PluginProcessor.h, StateVersion).
    params().resetDefaults();
    m_legacyKnobs.clear();
    m_lastStateVersion = xml->getIntAttribute( "version", 1 );
    const juce::String knobs =
        xml->getChildByName( "params" ) != nullptr ? xml->getChildByName( "params" )->getAllSubText() : juce::String();
    if ( m_lastStateVersion >= 3 )
    {
        params().parseText( knobs.toStdString() );
        // A macro still holding a knob now holds it from the loaded value: that is where it goes back to.
        const std::lock_guard<std::mutex> lock( m_macroBaseLock );
        for ( auto &[id, value] : m_macroBase ) value = params().get( id );
    }
    else
    {
        // A version-1 or version-2 state holds *every* value of its session and cannot say which of them
        // the user chose. Applying it is exactly the failure the state version 3 exists to stop: the user's own
        // standalone state carried sfx.level -12, bass.cutoff 140, mix.lead_level 0 and kick.level -2
        // from an older calibration and silently undid four rounds of work.
        //
        // The decision, and it is a decision: such a state sets **no knob at all**. The engine comes up
        // on today's factory defaults, which is the safe half of the choice and the one a listener
        // wants by default, and the old text is kept so the editor can offer it ("Load the saved knobs
        // anyway", adoptLegacyState). What is *not* a calibrated value is restored as always -- the
        // seed, the host-sync switch and the cue destination are the user's set, not our tuning.
        //
        // The cost is real and is stated rather than hidden: a DAW project saved with an older build
        // opens with its sound on the defaults until the user takes the offer.
        m_legacyKnobs = knobs;
    }
    // A loaded set lets go of every macro (01.10.2026). The macros are performance, not part of the set
    // (writeStateTo), so a state cannot say where they stand -- and leaving them where they were did not bring them
    // back either: pluginval's state test pressed the drop-out, the timer let it go by itself (no transport, no bar
    // line to wait for), and the next state handed the host a value it had not had before. Neutral is the one value
    // a state can promise; the knobs a macro held go back to what was just loaded.
    for ( int i = 0; i < NumMacros; ++i ) setMacro( static_cast<MacroE>( i ), 0.0f );
    if ( auto *mm = xml->getChildByName( "midimap" ) )
        m_midiMap.fromText( mm->getAllSubText().toStdString(),
                            [this]( const std::string &k ) { return midiTargetFind( k ); } );
    if ( m_midiMap.bindings().empty() ) bindDefaultControllers();   // a state from before 01.10.2026 had none at all
    m_followHost = xml->getBoolAttribute( "followHost", m_followHost );
    setCueHost( xml->getStringAttribute( "cueHost", cueHost() ) );
    if ( xml->getStringAttribute( "fieldFolder" ) != m_fieldFolder )
        setFieldFolder( xml->getStringAttribute( "fieldFolder" ) );
    m_followHostAtomic.store( m_followHost, std::memory_order_release );
    const uint64_t s = static_cast<uint64_t>( xml->getStringAttribute( "seed", "1" ).getLargeIntValue() );
    setSeed( juce::jmax<uint64_t>( 1, s ) );
}

void PhospheneProcessor::adoptLegacyState()
{
    if ( m_legacyKnobs.isEmpty() ) return;
    params().parseText( m_legacyKnobs.toStdString() );
    m_legacyKnobs.clear();
}

void PhospheneProcessor::resetToFactoryDefaults()
{
    // The host's own parameter objects read the store, so putting the store back is enough for the
    // sound; the editor refreshes from the same place on its timer.
    params().resetDefaults();
    m_legacyKnobs.clear();
    m_lastStateVersion = StateVersion;
}

// ------------------------------------------------------------------ export

bool PhospheneProcessor::exportMidi( const juce::File &file, int bars )
{
    // Blocks on the composer, which is what a user action with a file dialog in front of it may do.
    const std::lock_guard<std::mutex> lock( m_composeLock );
    // The knobs as they stand, every one of them: the file is the set the user sees (m_composeParams, 25.09.2026).
    refreshComposeParams( true );
    const ParamStore &knobs = m_composeParams;
    const int         total = juce::jlimit( 1, 4096, bars );
    Score             score;
    score.m_tempo = m_composer->tempoMap( knobs, total );
    score.m_keyRoot = m_composer->track( knobs, 0 ).m_key;
    score.m_scale = m_composer->track( knobs, 0 ).m_scale;
    for ( int t = 0; m_composer->track( knobs, t ).m_firstBar < total; ++t )
    {
        const TrackPlan p = m_composer->track( knobs, t );
        const double    beat = static_cast<double>( p.m_firstBar ) * BeatsPerBar;
        // The key signature changes where the bass does: at the hand-over of the DJ overlap (19.09.2026).
        const double keyBeat = static_cast<double>( handoverBar( p ) ) * BeatsPerBar;
        if ( t > 0 && p.m_key != m_composer->track( knobs, t - 1 ).m_key )
            score.m_keyChanges.push_back( KeyChange{ keyBeat, p.m_key } );
        score.m_sections.push_back( SectionMark{ beat, SectionTypeE::GROOVE, 0.5f, t } );
    }
    std::vector<NoteEvent> notes;
    m_composer->composeBars( knobs, 0, total, notes, nullptr );
    score.m_notes = std::move( notes );
    score.sort();
    return writeMidiFile( score, file.getFullPathName().toRawUTF8() );
}

juce::File PhospheneProcessor::galleryFolder() const
{
    const juce::String env = juce::SystemStats::getEnvironmentVariable( "PHOS_GALLERY_DIR", "" );
    const juce::File dir = env.isNotEmpty() ? juce::File( env )
                                            : juce::File::getSpecialLocation( juce::File::userApplicationDataDirectory )
                                                  .getChildFile( "Phosphene" )
                                                  .getChildFile( "Sets" );
    // Not created here: the gallery page asks for the path just by opening (23.09.2026 -- the VST3 test, which opens
    // the editor, left an empty Sets folder in the user's real data folder). Saving creates it.
    return dir;
}

juce::File PhospheneProcessor::userFolder() const
{
    // PHOS_USER_DIR (tests): a folder of their own for the ratings, the preferences and the user presets. The host
    // test presses every button of the editor, "Good here" and "Learn from my ratings..." among them, and must never
    // write into the user's real files -- its first runs of 23.09.2026 left sixteen verdicts in them before this existed.
    const juce::String env = juce::SystemStats::getEnvironmentVariable( "PHOS_USER_DIR", "" );
    const juce::File   dir =
        env.isNotEmpty()
            ? juce::File( env )
            : juce::File::getSpecialLocation( juce::File::userApplicationDataDirectory ).getChildFile( "Phosphene" );
    dir.createDirectory();
    return dir;
}

juce::File PhospheneProcessor::ratingsFile() const
{
    return userFolder().getChildFile( "ratings.tsv" );
}

// ------------------------------------------------------------------ sound presets

juce::File PhospheneProcessor::userPresetFolder( ModuleE module, int instance ) const
{
    const std::string key = params().key( params().base( module, instance ) );
    return userFolder().getChildFile( "Presets" ).getChildFile( juce::String( key.substr( 0, key.find( '.' ) ) ) );
}

std::vector<SoundPreset> PhospheneProcessor::userPresets( ModuleE module, int instance ) const
{
    std::vector<SoundPreset> out;
    const juce::File         dir = userPresetFolder( module, instance );
    if ( !dir.isDirectory() ) return out;
    for ( const juce::File &f : dir.findChildFiles( juce::File::findFiles, false, "*.txt" ) )
        out.push_back( { "User", f.getFileNameWithoutExtension().toStdString(),
                         f.loadFileAsString().removeCharacters( "\r" ).toStdString() } );
    std::sort( out.begin(), out.end(),
               []( const SoundPreset &a, const SoundPreset &b ) { return a.m_name < b.m_name; } );
    return out;
}

void PhospheneProcessor::previewSfx( int choice, int preset )
{
    if ( choice < 0 || choice >= phos::sfx::NumPresetChoices ) return;
    m_sfxPreview.store( ( choice << 12 ) | juce::jlimit( 0, 0xFFF, preset ), std::memory_order_release );
}

void PhospheneProcessor::takeChannelMeters( float *peak, float *rms )
{
    const int n = m_meterCount.exchange( 0, std::memory_order_acquire );
    for ( int k = 0; k < phos::NumParts; ++k )
    {
        peak[k] = m_meterPeak[static_cast<size_t>( k )].exchange( 0.0f, std::memory_order_relaxed );
        const double s = m_meterSum[static_cast<size_t>( k )].exchange( 0.0, std::memory_order_relaxed );
        rms[k] = n > 0 ? static_cast<float>( std::sqrt( s / n ) ) : 0.0f;
    }
}

void PhospheneProcessor::applyPreset( ModuleE module, int instance, const SoundPreset &preset )
{
    // The synth's owner number is the order of the own-sound switches (Params.h, mix::KickOwn): kick, bass, acid, the voices.
    const int owner = module == ModuleE::KICK   ? 0
                      : module == ModuleE::BASS ? 1
                      : module == ModuleE::ACID ? 2
                                                : 3 + instance;
    undoable( "Preset " + juce::String( preset.m_name ),
              [&]
              {
                  applySoundPreset( params(), module, instance, preset.m_text );
                  params().set( params().base( ModuleE::MIX ) + mix::KICK_OWN + owner, 1.0f );
              } );
}

void PhospheneProcessor::beginStep( const juce::String &what )
{
    if ( m_gestureDepth++ == 0 )
    {
        m_gestureBefore = captureUndoState();
        m_gestureName = what;
    }
}

void PhospheneProcessor::endStep()
{
    if ( m_gestureDepth > 0 && --m_gestureDepth == 0 ) recordUndo( m_gestureName, m_gestureBefore );
}

void PhospheneProcessor::takeOver( int id )
{
    int       local = -1;
    const int k = soundSynthOfId( id, local );
    if ( k < 0 ) return;
    const ModuleE m = k == 0 ? ModuleE::KICK : k == 1 ? ModuleE::BASS : k == 2 ? ModuleE::ACID : ModuleE::POLY;
    const int     own = params().base( ModuleE::MIX ) + mix::KICK_OWN + k;
    if ( !presetLeaves( m, local ) && !params().getBool( own ) && m_shownPreset[k] >= 0 )
    {
        takeOverPreset( k );
        if ( StoreParameter *o = parameterFor( own ) ) o->setValueNotifyingHost( 1.0f );
    }
}

void PhospheneProcessor::applyKnobs( const juce::String &what, const std::vector<std::pair<int, float>> &values )
{
    endPreview();
    beginStep( what );
    for ( const auto &[id, v] : values )
    {
        takeOver( id );
        if ( StoreParameter *p = parameterFor( id ) ) p->setValueNotifyingHost( params().toNormalised( id, v ) );
    }
    endStep();
}

void PhospheneProcessor::previewKnobs( const std::vector<std::pair<int, float>> &values )
{
    endPreview();
    for ( const auto &[id, v] : values )
    {
        if ( id < 0 || id >= params().count() ) continue;
        m_previewSaved.emplace_back( id, params().get( id ) );
        params().set( id, v );
    }
}

void PhospheneProcessor::endPreview()
{
    for ( const auto &[id, v] : m_previewSaved ) params().set( id, v );
    m_previewSaved.clear();
}

void PhospheneProcessor::applyFieldPreset( int index )
{
    const std::vector<FieldPreset> &all = fieldPresets();
    if ( index < 0 || index >= static_cast<int>( all.size() ) ) return;
    const FieldPreset &fp = all[static_cast<size_t>( index )];
    // A preset loaded by hand is the user's choice: its category is no longer Auto, so the composer leaves it alone.
    undoable( "Field preset " + juce::String( fp.m_name ),
              [&]
              {
                  const int b = params().base( ModuleE::FIELD );
                  for ( const auto &kv : fieldPresetValues( params(), fp ) ) params().set( b + kv.first, kv.second );
              } );
}

void PhospheneProcessor::setFieldFolder( const juce::String &folder )
{
    m_fieldFolder = folder.trim();
    setFieldUserFolder( m_fieldFolder.toStdString() );
    scanFieldLibrary();
}

juce::File PhospheneProcessor::saveUserPreset( ModuleE module, int instance, const juce::String &name )
{
    const juce::String clean = juce::File::createLegalFileName( name.trim() );
    if ( clean.isEmpty() ) return {};
    const juce::File dir = userPresetFolder( module, instance );
    dir.createDirectory();
    const juce::File f = dir.getChildFile( clean + ".txt" );
    // "\n" line ends: JUCE's default is "\r\n", and the preset must read back as the text it was.
    return f.replaceWithText( juce::String( moduleText( params(), module, instance ) ), false, false, "\n" )
               ? f
               : juce::File();
}

juce::File PhospheneProcessor::saveToGallery( const juce::String &name )
{
    GalleryEntry e;
    e.m_name = name.trim().toStdString();
    e.m_saved = juce::Time::getCurrentTime().formatted( "%Y-%m-%dT%H:%M:%S" ).toStdString();
    {
        // The published plans: the tracks the editor has seen planned, which is what the thumbnail should show.
        const std::lock_guard<std::mutex> lock( m_plansLock );
        for ( const TrackPlan &p : m_plans ) e.m_tracks.push_back( galleryTrackOf( p ) );
    }
    std::string text;
    {
        const std::lock_guard<std::mutex> lock( m_composeLock );
        text = writeSetText( *m_composer, params() );
    }
    text = withGalleryComment( text, e );
    juce::String base = juce::Time::getCurrentTime().formatted( "%Y-%m-%d_%H%M" ) + "_";
    base += name.trim().isNotEmpty() ? juce::File::createLegalFileName( name.trim() )
                                     : juce::String( "seed" ) + juce::String( m_seed.load() );
    galleryFolder().createDirectory();
    const juce::File f = galleryFolder().getNonexistentChildFile( base, ".phosset", false );
    return f.replaceWithText( juce::String( text ) ) ? f : juce::File();
}

juce::File PhospheneProcessor::preferencesFile() const
{
    return ratingsFile().getSiblingFile( "preferences.txt" );
}

Preferences PhospheneProcessor::fitFromRatings( int *verdicts ) const
{
    std::vector<RatingEntry> ratings;
    juce::StringArray        lines;
    ratingsFile().readLines( lines );
    for ( const juce::String &l : lines )
    {
        RatingEntry r;
        if ( parseRating( l.toStdString(), r ) && r.m_verdict != 0 && !r.m_features.empty() ) ratings.push_back( r );
    }
    if ( verdicts != nullptr ) *verdicts = static_cast<int>( ratings.size() );
    return fitPreferences( ratings );
}

bool PhospheneProcessor::applyPreferences( const Preferences &prefs )
{
    const bool written = preferencesFile().replaceWithText( juce::String( prefs.toText() ) );
    setPreferences( std::make_shared<Preferences>( prefs ) );
    // The plans are made again (Composer::validate watches the revision); the bars in the rings were composed under
    // the old taste, so the transport restarts at the bar that is playing, as a reroll does.
    markCurrentPosition();
    m_plansStale.store( true, std::memory_order_release );
    m_restartRequest.store( true, std::memory_order_release );
    m_restarts.fetch_add( 1, std::memory_order_relaxed );
    return written;
}

void PhospheneProcessor::forgetPreferences()
{
    const juce::File f = preferencesFile();
    if ( f.existsAsFile() ) f.moveFileTo( f.getSiblingFile( "preferences.txt.old" ) );
    setPreferences( nullptr );
    markCurrentPosition();
    m_plansStale.store( true, std::memory_order_release );
    m_restartRequest.store( true, std::memory_order_release );
    m_restarts.fetch_add( 1, std::memory_order_relaxed );
}

juce::String PhospheneProcessor::preferencesSummary() const
{
    const std::shared_ptr<const Preferences> p = preferences();
    if ( p == nullptr || p->empty() ) return "No learned preferences: every choice is the style's own.";
    // The strongest three, read back from the text form (one line per weight).
    juce::StringArray                            lines = juce::StringArray::fromLines( juce::String( p->toText() ) );
    std::vector<std::pair<double, juce::String>> w;
    for ( const juce::String &l : lines )
    {
        if ( l.startsWithChar( '#' ) || l.trim().isEmpty() ) continue;
        w.emplace_back( std::fabs( l.fromLastOccurrenceOf( " ", false, false ).getDoubleValue() ), l.trim() );
    }
    std::sort( w.begin(), w.end(), []( const auto &a, const auto &b ) { return a.first > b.first; } );
    juce::String s;
    s << static_cast<int>( p->size() ) << " learned weights in force, strongest: ";
    for ( size_t i = 0; i < std::min<size_t>( 3, w.size() ); ++i ) s << ( i > 0 ? ",  " : "" ) << w[i].second;
    return s;
}

bool PhospheneProcessor::exportSet( const juce::File &file )
{
    // The core's own `.phosset` writer (SetFile.h), not a private format: seed, style, arc, the
    // knobs that differ from their defaults and -- since the curation loop exists in the editor --
    // every lock and every reroll counter. What this writes, `phos_render --set-file` plays.
    const std::lock_guard<std::mutex> lock( m_composeLock );
    return writeSetFile( file.getFullPathName().toRawUTF8(), *m_composer, params() );
}

bool PhospheneProcessor::importSet( const juce::File &file )
{
    if ( !file.existsAsFile() ) return false;
    return importSetText( file.loadFileAsString().toStdString() );
}

bool PhospheneProcessor::loadFactorySet( int index )
{
    if ( index < 0 || index >= numFactorySets() ) return false;
    return importSetText( factorySet( index ).m_text );
}

bool PhospheneProcessor::importSetText( const std::string &text )
{
    bool ok = false;
    {
        // Blocks on the composer, which a user action with a file dialog in front of it may do.
        const std::lock_guard<std::mutex> lock( m_composeLock );
        std::string                       error;
        ok = readSetText( text, *m_composer, params(), &error );
        if ( !ok && !error.empty() ) juce::Logger::writeToLog( "phosset: " + juce::String( error ) );
        m_seed.store( m_composer->seed(), std::memory_order_relaxed );
    }
    syncCurationMirror();
    m_plansStale.store( true, std::memory_order_release );
    seekToBar( 0 );
    return ok;
}

// ------------------------------------------------------------------ recording

bool PhospheneProcessor::startRecording( const juce::File &file )
{
    stopRecording();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream( file.createOutputStream() );
    if ( stream == nullptr ) return false;
    juce::WavAudioFormat wav;
    // 32-bit float, as the offline renderer writes: a recording of a set is a master, not a mixdown.
    auto writer = wav.createWriterFor(
        stream, juce::AudioFormatWriter::Options{}
                    .withSampleRate( m_sampleRate )
                    .withNumChannels( 2 )
                    .withBitsPerSample( 32 )
                    .withSampleFormat( juce::AudioFormatWriterOptions::SampleFormat::floatingPoint ) );
    if ( writer == nullptr ) return false;
    if ( !m_recordThread.isThreadRunning() ) m_recordThread.startThread();
    auto threaded =
        std::make_unique<juce::AudioFormatWriter::ThreadedWriter>( writer.release(), m_recordThread, 1 << 16 );
    m_recordedSamples.store( 0 );
    {
        const juce::ScopedLock lock( m_recordLock );
        m_recordWriter = std::move( threaded );
    }
    m_recording.store( true );
    return true;
}

void PhospheneProcessor::stopRecording()
{
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> gone;
    {
        const juce::ScopedLock lock( m_recordLock );
        gone = std::move( m_recordWriter );
    }
    gone.reset();
    m_recording.store( false );
}

// ------------------------------------------------------------------ editor and entry point

juce::AudioProcessorEditor *PhospheneProcessor::createEditor()
{
    return new PhospheneEditor( *this );
}

/**
 * @brief JUCE's entry point: one processor per instance.
 * @return a new processor
 */
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter()
{
    return new PhospheneProcessor();
}
