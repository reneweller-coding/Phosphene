/**
 * @file Source.cpp
 * @brief Phosphene Sets: the render app's source for Phosphene -- its five start styles, each played as a set of the
 *        length chosen with the dice of the moment (the energy arc as chosen), and the sets the player saved (the
 *        gallery's .phosset files, and those in Documents/Phosphene), each with its own seed, style, arc, knobs and
 *        curation. Or a set's first track alone, as the DJ export renders it.
 *
 * A set is what phos_render writes for the same seed, knobs and length: the composer's tempo map planned over the
 * length, the engine prepared at 48 kHz with blocks of 256, the conductor keeping eight bars composed ahead; a set
 * never ends by itself, so its last twenty seconds are faded out. A track alone is what phos_render --dj-export writes
 * for it: composed alone (Composer::setSoloTrack), at its own constant tempo, the limiter's lookahead cut from the
 * front so the first downbeat is the first sample, its last ten milliseconds faded.
 * @author Rene Weller
 */
#include "RenderKit.h"
#include "phos/Clock.h"
#include "phos/Composer.h"
#include "phos/Engine.h"
#include "phos/FactorySets.h"
#include "phos/FieldLibrary.h"
#include "phos/Form.h"
#include "phos/Model.h"
#include "phos/Params.h"
#include "phos/Preferences.h"
#include "phos/Quality.h"
#include "phos/SetFile.h"
#include "phos/WaveTableFile.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace
{

constexpr int    Rate = 48000;        ///< the files' rate
constexpr int    EngineBlock = 256;   ///< the engine's block (phos_render's)
constexpr double SetFade = 20.0;      ///< s faded out at a set's end (a set never ends by itself)
/** @brief The panel's set lengths, minutes. */
const double SetMinutes[] = { 10.0, 20.0, 30.0, 45.0, 60.0, 90.0, 120.0, 180.0 };

/** @brief What a row is: a start style, or a saved set. */
struct Row
{
    int        m_style = -1;      ///< the start style (StyleIdE), -1 for a set
    int        m_factory = -1;    ///< the factory set (phos/FactorySets.h), -1 for none
    juce::File m_file;            ///< the saved set's .phosset
    double     m_minutes = 0.0;   ///< the set's own length (compose.set_minutes)
};

/**
 * @brief How long a set of a row plays: a start style as the panel asks, a saved set as long as it was saved.
 * @param[in] row  the row
 * @param[in] r    the request
 * @return minutes
 */
double setMinutes( const Row &row, const renderkit::Request &r )
{
    return row.m_style >= 0 ? SetMinutes[std::clamp( r.option( "length", 4 ), 0, 7 )] : row.m_minutes;
}

/**
 * @brief The knobs and the composer of a row: a start style with the panel's arc and the dice, or a saved set's own.
 * @param[in,out] params    the knobs (fresh)
 * @param[in,out] composer  the composer (its seed and curation come from a saved set)
 * @param[in]     row       the row
 * @param[in]     r         the request
 * @param[out]    why       receives why a saved set cannot be read
 * @return false when it cannot be set up
 */
bool knobsOf( phos::ParamStore &params, phos::Composer &composer, const Row &row, const renderkit::Request &r,
              juce::String &why )
{
    using namespace phos;
    if ( row.m_style >= 0 )
    {
        const int cb = params.base( ModuleE::COMPOSE );
        params.set( cb + compose::STYLE, static_cast<float>( row.m_style ) );
        params.set( cb + compose::ARC, static_cast<float>( std::clamp( r.option( "arc", 4 ), 0, NumArcs - 1 ) ) );
        // the arc is shaped over the set's length
        params.set( cb + compose::SET_MINUTES, static_cast<float>( setMinutes( row, r ) ) );
        return true;
    }
    std::string error;
    const bool  read = row.m_factory >= 0
                           ? readSetText( factorySet( row.m_factory ).m_text, composer, params, &error )
                           : readSetFile( row.m_file.getFullPathName().toRawUTF8(), composer, params, &error );
    if ( !read )
    {
        why = error;
        return false;
    }
    return true;
}

/**
 * @brief A track as the status line names it.
 * @param[in] t  the track's plan
 * @return "track 3: Full-On, 146.0 BPM, F# Phrygian"
 */
juce::String trackText( const phos::TrackPlan &t )
{
    return "track " + juce::String( t.m_index + 1 ) + ": " +
           phos::StyleNames[std::clamp( t.m_style, 0, phos::NumStyles - 1 )] + ", " + juce::String( t.m_bpm, 1 ) +
           " BPM, " + phos::KeyNames[t.m_key] + " " + phos::ScaleNames[t.m_scale];
}

/**
 * @brief How long a track alone lasts: its bars at its own tempo.
 * @param[in] t  the track's plan
 * @return seconds
 */
double trackSeconds( const phos::TrackPlan &t )
{
    return static_cast<double>( t.m_bars ) * phos::BeatsPerBar * 60.0 / t.m_bpm;
}

/**
 * @brief A track's sections on the time line (renderkit::CueSheet): its form's sections as they are -- Phosphene's
 *        words are Kaleidoscope's own --, a drop landing as one; only those from @p fromBar to @p toBar (a set's track
 *        before its handover still belongs to the one before).
 * @param[in,out] sheet         the time line
 * @param[in]     t             the track's plan
 * @param[in]     secondsAtBar  where a bar of the set begins, s
 * @param[in]     fromBar       the first bar that is the track's
 * @param[in]     toBar         the bar it hands over on
 * @param[in]     chapters      each section a chapter as well (a track alone)
 */
void trackTimeline( renderkit::CueSheet &sheet, const phos::TrackPlan &t,
                    const std::function<double( int )> &secondsAtBar, int fromBar, int toBar, bool chapters )
{
    std::map<int, int> seen;   // a section's kind -> how often so far, for the chapters' names
    for ( int i = 0; i < t.m_form.m_count; ++i )
    {
        const phos::Section &sec = t.m_form.m_section[i];
        const int            bar = t.m_firstBar + sec.m_startBar;
        if ( bar < fromBar || bar >= toBar ) continue;
        const double       at = secondsAtBar( bar );
        const juce::String name = phos::SectionNames[static_cast<int>( sec.m_type )];
        sheet.section( at, name, sec.m_energy, sec.m_type == phos::SectionTypeE::DROP );
        const int n = ++seen[static_cast<int>( sec.m_type )];
        if ( chapters ) sheet.chapter( at, n > 1 ? name + " " + juce::String( n ) : name );
    }
}

/**
 * @brief A track's key as the cues name it.
 * @param[in] t  the track's plan
 * @return "F# Phrygian"
 */
juce::String keyName( const phos::TrackPlan &t )
{
    return juce::String( phos::KeyNames[t.m_key] ) + " " + phos::ScaleNames[t.m_scale];
}

/** @brief A set being rendered: the engine offline, the conductor composing ahead of it. */
class SetRendering : public renderkit::Rendering
{
public:
    /**
     * @brief Plans the tempo over the length and prepares the engine.
     * @param[in]  row      the row
     * @param[in]  r        the request
     * @param[in]  seed     the dice for a start style (renderkit::seedFor)
     * @param[in]  seconds  the length
     * @param[out] why      receives why it cannot be rendered
     */
    SetRendering( const Row &row, const renderkit::Request &r, std::uint64_t seed, double seconds, juce::String &why )
        : m_composer( seed ), m_seconds( seconds )
    {
        using namespace phos;
        m_engine = std::make_unique<Engine>();
        ParamStore &params = m_engine->params();
        if ( !knobsOf( params, m_composer, row, r, why ) )
        {
            m_engine.reset();
            return;
        }
        // The tempo map over the length, planned at the fastest tempo the range allows (phos_render).
        const int           cb = params.base( ModuleE::COMPOSE );
        const StyleProfile &style = styleProfile( styleOf( params ) );
        const double        planBpm = params.getBool( cb + compose::STYLE_TEMPO )
                                          ? style.m_bpmCentre + style.m_bpmRange
                                          : params.get( cb + compose::BPM ) + params.get( cb + compose::TEMPO_RANGE );
        // Planned as far as it is rendered: every track planned is measured by the probes first (Probe.h),
        // which is what makes a set's start take a while; the first seconds of a set are the same either way.
        const double   planSeconds = r.m_limit > 0.0 ? std::min( r.m_limit, seconds ) : seconds;
        const int      planBars = static_cast<int>( planSeconds * planBpm / 240.0 ) + 64;
        const TempoMap tempo = m_composer.tempoMap( params, planBars );
        m_tempo = tempo;
        m_planSeconds = planSeconds;
        m_engine->prepare( Rate, EngineBlock, Quality::desktop() );
        m_engine->setTempoMap( tempo );
        m_conductor = std::make_unique<Conductor>( *m_engine, m_composer );
    }
    /** @brief Whether it could be set up. @return true when it renders */
    bool ok() const { return m_engine != nullptr; }
    double seconds() const override { return m_seconds; }
    double fadeSeconds() const override { return SetFade; }
    void render( float *left, float *right, int frames ) override
    {
        using namespace phos;
        const ParamStore &params = m_engine->params();
        for ( int done = 0; done < frames; )
        {   // as phos_render: eight bars composed ahead, then a block of the engine
            const int n = std::min( EngineBlock, frames - done );
            m_conductor->pump( params, 8.0 * BeatsPerBar );
            m_engine->process( left + done, right + done, n );
            done += n;
        }
        const int                         bar = static_cast<int>( m_engine->beatPosition() / BeatsPerBar );
        const int                         t = m_composer.trackOfBar( params, std::max( 0, bar ) );
        juce::String                      now = trackText( m_composer.track( params, t ) );
        const std::lock_guard<std::mutex> hold( m_lock );
        m_now = now;
    }
    juce::String now() const override
    {
        const std::lock_guard<std::mutex> hold( m_lock );
        return m_now;
    }
    /**
     * @brief The set's time line: every track from its handover with its key, its sections and a chapter, and the
     *        beats -- the tracks planned as far as the rendering goes (the conductor finds them planned then).
     * @param[out] sheet  receives it
     */
    void timeline( renderkit::CueSheet &sheet ) const override
    {
        const phos::ParamStore &params = m_engine->params();
        const auto              secondsAtBar = [this]( int bar )
        { return m_tempo.secondsAt( static_cast<double>( bar ) * phos::BeatsPerBar ); };
        std::vector<phos::TrackPlan> plans;
        for ( int t = 0; t < 1000; ++t )
        {
            const phos::TrackPlan &p = m_composer.track( params, t );
            if ( t > 0 && secondsAtBar( phos::handoverBar( p ) ) >= m_planSeconds ) break;
            plans.push_back( p );
        }
        for ( size_t i = 0; i < plans.size(); ++i )
        {
            const int    from = i == 0 ? 0 : phos::handoverBar( plans[i] );
            const int    to = i + 1 < plans.size() ? phos::handoverBar( plans[i + 1] ) : 1 << 30;
            const double at = secondsAtBar( from );
            sheet.key( at, keyName( plans[i] ) );
            sheet.chapter( at, "Track " + juce::String( static_cast<int>( i + 1 ) ) + ": " +
                                   trackText( plans[i] ).fromFirstOccurrenceOf( ": ", false, false ) );
            trackTimeline( sheet, plans[i], secondsAtBar, from, to, false );
        }
        sheet.grid( [this]( double b ) { return m_tempo.secondsAt( b ); }, m_tempo.beatAt( m_planSeconds ) );
    }

private:
    phos::TempoMap                   m_tempo;               ///< the set's tempo over what is rendered
    double                           m_planSeconds = 0.0;   ///< s planned: what is rendered
    phos::Composer                   m_composer;            ///< the set's composer (before the conductor that reads it)
    std::unique_ptr<phos::Engine>    m_engine;              ///< the instrument offline
    std::unique_ptr<phos::Conductor> m_conductor;           ///< keeps the score ahead of the engine
    double                           m_seconds = 0.0;       ///< the length
    mutable std::mutex               m_lock;                ///< guards m_now
    juce::String                     m_now;                 ///< the track that plays
};

/** @brief A set's first track alone, as the DJ export renders it. */
class TrackRendering : public renderkit::Rendering
{
public:
    /**
     * @brief Plans the track and prepares an engine at its tempo.
     * @param[in]  row   the row
     * @param[in]  r     the request
     * @param[in]  seed  the dice for a start style (renderkit::seedFor)
     * @param[out] why   receives why it cannot be rendered
     */
    TrackRendering( const Row &row, const renderkit::Request &r, std::uint64_t seed, juce::String &why )
        : m_composer( seed )
    {
        using namespace phos;
        m_engine = std::make_unique<Engine>();
        ParamStore &params = m_engine->params();
        if ( !knobsOf( params, m_composer, row, r, why ) )
        {
            m_engine.reset();
            return;
        }
        m_plan = m_composer.track( params, 0 );
        TempoMap tm;
        tm.setConstant( m_plan.m_bpm );
        m_engine->prepare( Rate, EngineBlock, Quality::desktop() );
        m_engine->setTempoMap( tm );
        m_composer.setSoloTrack( 0 );
        m_offset = static_cast<double>( m_plan.m_firstBar ) * BeatsPerBar;
        m_nextBar = m_plan.m_firstBar;
        m_latency = std::max( 0, m_engine->latencySamples() );
        m_seconds = trackSeconds( m_plan );
        m_now = trackText( m_plan ) + ", alone";
        // its time line: its sections as chapters too, from the track's own start, at its tempo
        const double bpm = m_plan.m_bpm;
        const int    first = m_plan.m_firstBar;
        m_sheet.key( 0.0, keyName( m_plan ) );
        trackTimeline(
            m_sheet, m_plan, [bpm, first]( int bar ) { return ( bar - first ) * phos::BeatsPerBar * 60.0 / bpm; },
            first, 1 << 30, true );
        m_sheet.grid( [bpm]( double b ) { return b * 60.0 / bpm; }, m_seconds * bpm / 60.0 );
    }
    /** @brief Whether it could be set up. @return true when it renders */
    bool ok() const { return m_engine != nullptr; }
    double seconds() const override { return m_seconds; }
    double fadeSeconds() const override { return 0.01; }
    void render( float *left, float *right, int frames ) override
    {
        using namespace phos;
        const ParamStore &params = m_engine->params();
        const int         endBar = m_plan.m_firstBar + m_plan.m_bars;
        while ( static_cast<int>( m_left.size() ) < frames )
        {
            // Eight bars composed ahead of the engine, their beats moved to the file's own 0, fed as Conductor::pump
            // feeds it: a section's first bar dates its rides bars ahead, and the engine drains its queue in order, so
            // only what lies before the next bar still to compose may go in (phos_render's --dj-export pushed each
            // bar's events at once, and every control after a ride waited behind it).
            const double target = m_engine->beatPosition() + 8.0 * BeatsPerBar;
            for ( ;; )
            {
                const double safe = static_cast<double>( m_nextBar ) * BeatsPerBar;
                bool         full = false;
                while ( !full && m_controlPos < m_controls.size() && m_controls[m_controlPos].m_beat < safe )
                {
                    ControlEvent c = m_controls[m_controlPos];
                    c.m_beat -= m_offset;
                    full = !m_engine->pushControl( c );
                    m_controlPos += full ? 0 : 1;
                }
                while ( !full && m_notePos < m_notes.size() && m_notes[m_notePos].m_beat < safe )
                {
                    NoteEvent n = m_notes[m_notePos];
                    n.m_beat -= m_offset;
                    full = !m_engine->pushEvent( n );
                    m_notePos += full ? 0 : 1;
                }
                if ( m_controlPos > 256 )
                {   // the drained prefix let go, so a long track does not grow the buffers without bound
                    m_controls.erase( m_controls.begin(), m_controls.begin() + static_cast<long>( m_controlPos ) );
                    m_controlPos = 0;
                }
                if ( m_notePos > 256 )
                {
                    m_notes.erase( m_notes.begin(), m_notes.begin() + static_cast<long>( m_notePos ) );
                    m_notePos = 0;
                }
                if ( full || m_nextBar >= endBar || safe - m_offset >= target ) break;
                m_composer.composeBars( params, m_nextBar, 1, m_notes, &m_controls );
                ++m_nextBar;
            }
            float L[EngineBlock], R[EngineBlock];
            m_engine->process( L, R, EngineBlock );
            for ( int i = 0; i < EngineBlock; ++i )
            {   // the limiter's lookahead dropped at the front: the first downbeat is the first sample
                if ( m_dropped < m_latency )
                {
                    ++m_dropped;
                    continue;
                }
                m_left.push_back( L[i] );
                m_right.push_back( R[i] );
            }
        }
        for ( int i = 0; i < frames; ++i )
        {
            left[i] = m_left.front();
            right[i] = m_right.front();
            m_left.pop_front();
            m_right.pop_front();
        }
    }
    juce::String now() const override { return m_now; }
    void timeline( renderkit::CueSheet &sheet ) const override { sheet = m_sheet; }

private:
    renderkit::CueSheet             m_sheet;            ///< its time line
    phos::Composer                  m_composer;         ///< the set's composer
    std::unique_ptr<phos::Engine>   m_engine;           ///< the instrument offline, at the track's tempo
    phos::TrackPlan                 m_plan;             ///< the track
    double                          m_offset = 0.0;     ///< its first beat in the set
    int                             m_nextBar = 0;      ///< the next bar to compose
    int                             m_latency = 0;      ///< the limiter's lookahead, samples
    int                             m_dropped = 0;      ///< lookahead samples dropped so far
    double                          m_seconds = 0.0;    ///< the length
    std::vector<phos::NoteEvent>    m_notes;            ///< the notes composed so far (composeBars keeps them sorted)
    std::vector<phos::ControlEvent> m_controls;         ///< the controls composed so far, likewise
    size_t                          m_notePos = 0;      ///< the first note not yet handed to the engine
    size_t                          m_controlPos = 0;   ///< the first control not yet handed to the engine
    std::deque<float>               m_left;             ///< samples rendered and not yet handed out, left
    std::deque<float>               m_right;            ///< ... right
    juce::String                    m_now;              ///< what plays (set up once)
};

/** @brief Phosphene's start styles and saved sets for the render app. */
class PhospheneSource : public renderkit::Source
{
public:
    /**
     * @brief Points the core at the data files installed beside this program (the wavetable pack, the melody and bass
     *        models, the field recordings), as phos_render does (installDataSearchPath): without it an installed copy
     *        would play the six built-in wavetables and the Markov model. Waits for the field recordings as
     *        phos_render does, and plans with the player's learned preferences as the plugin does.
     */
    PhospheneSource()
    {
        const std::string home = juce::File::getSpecialLocation( juce::File::currentExecutableFile )
                                     .getParentDirectory()
                                     .getFullPathName()
                                     .toStdString();
        phos::setWaveTableSearchPath( home );
        phos::setModelSearchPath( home );
        // An offline render waits for the Field track's recordings (FieldLibrary.h): faster than real time, a clip
        // still decoding would come late or not at all -- the same seed, the same audio, as phos_render.
        phos::setFieldPreloadBlocking( true );
        // The player's learned preferences, as the plugin plans with them (process-wide, read before any plan; the
        // gallery's sets were planned with them): Phosphene's folder in the application data, preferences.txt.
        const juce::String env = juce::SystemStats::getEnvironmentVariable( "PHOS_USER_DIR", "" );
        const juce::File   prefs =
            ( env.isNotEmpty() ? juce::File( env )
                               : juce::File::getSpecialLocation( juce::File::userApplicationDataDirectory )
                                     .getChildFile( "Phosphene" ) )
                .getChildFile( "preferences.txt" );
        if ( prefs.existsAsFile() )
        {
            auto p = std::make_shared<phos::Preferences>();
            if ( p->parse( prefs.loadFileAsString().toStdString() ) ) phos::setPreferences( p );
        }
    }
    juce::String product() const override { return "Phosphene"; }
    juce::String title() const override { return "Phosphene Sets"; }
    juce::String subtitle() const override { return "Render sets and tracks into files, faster than real time"; }
    juce::String noun() const override { return "set"; }
    renderkit::Palette palette() const override
    {
        renderkit::Palette c;
        c.m_back = juce::Colour( 0xff0b0a12 );
        c.m_panel = juce::Colour( 0xff191625 );
        c.m_row = juce::Colour( 0xff12101b );
        c.m_raised = juce::Colour( 0xff221d33 );
        c.m_edge = juce::Colour( 0xff2b2641 );
        c.m_chosen = juce::Colour( 0xff3b2a63 );
        c.m_text = juce::Colour( 0xffeceaf6 );
        c.m_dim = juce::Colour( 0xffa39dc2 );
        c.m_accent = juce::Colour( 0xffa46bff );
        return c;
    }
    std::vector<renderkit::Column> columns() const override { return { { "Kind", 110, false } }; }
    std::vector<renderkit::Item> list() override
    {
        using namespace phos;
        std::vector<renderkit::Item> out;
        auto                         add = [&out, this]( renderkit::Item &&item, Row &&row )
        {
            const std::lock_guard<std::mutex> hold( m_lock );
            item.m_index = static_cast<int>( m_store.size() );
            m_store.push_back( std::make_shared<const Row>( std::move( row ) ) );
            out.push_back( std::move( item ) );
        };
        for ( int s = 0; s < NumStyles; ++s )
        {
            const StyleProfile &prof = styleProfile( static_cast<StyleIdE>( s ) );
            renderkit::Item     item;
            item.m_name = StyleNames[s];
            item.m_kind = "Start style";
            item.m_cells.add( "Start style" );
            item.m_keys = { std::nan( "" ) };
            item.m_detail = juce::String( prof.m_bpmCentre, 0 ) + " BPM";
            Row row;
            row.m_style = s;
            add( std::move( item ), std::move( row ) );
        }
        // the factory's sets (phos/FactorySets.h): played as they are, the dice their own
        for ( int i = 0; i < numFactorySets(); ++i )
        {
            const FactorySet &f = factorySet( i );
            renderkit::Item   item;
            item.m_name = f.m_name;
            item.m_kind = "Factory set";
            item.m_dice = false;
            item.m_cells.add( juce::String( f.m_section ) + ", " + f.m_group );
            item.m_keys = { std::nan( "" ) };
            item.m_detail = juce::String( f.m_note ) + " " + f.m_group + " " + f.m_section;
            Row row;
            row.m_factory = i;
            row.m_minutes = f.m_seconds / 60.0;
            add( std::move( item ), std::move( row ) );
        }
        // The gallery (the instrument's Sets folder) and the player's own folder in Documents.
        const juce::String env = juce::SystemStats::getEnvironmentVariable( "PHOS_GALLERY_DIR", "" );
        const juce::File   gallery = env.isNotEmpty()
                                         ? juce::File( env )
                                         : juce::File::getSpecialLocation( juce::File::userApplicationDataDirectory )
                                               .getChildFile( "Phosphene" )
                                               .getChildFile( "Sets" );
        const juce::File   docs =
            juce::File::getSpecialLocation( juce::File::userDocumentsDirectory ).getChildFile( "Phosphene" );
        for ( const juce::File &dir : { gallery, docs } )
        {
            juce::Array<juce::File> files = dir.findChildFiles( juce::File::findFiles, true, "*.phosset" );
            files.sort();
            for ( const juce::File &f : files )
            {
                renderkit::Item item;
                item.m_name = f.getFileNameWithoutExtension();
                item.m_kind = "Yours";
                item.m_dice = false;
                item.m_cells.add( dir == gallery ? "Gallery" : "Yours" );
                item.m_keys = { std::nan( "" ) };
                item.m_detail = f.getRelativePathFrom( dir );
                Row         row;
                ParamStore  params;
                Composer    composer;
                std::string error;
                row.m_file = f;
                if ( !readSetFile( f.getFullPathName().toRawUTF8(), composer, params, &error ) ) continue;
                row.m_minutes = params.get( params.base( ModuleE::COMPOSE ) + compose::SET_MINUTES );
                add( std::move( item ), std::move( row ) );
            }
        }
        return out;
    }
    std::vector<renderkit::Option> options() const override
    {
        renderkit::Option length{ "length",
                                  "Length",
                                  "How long a set from a start style plays, its arc shaped over it "
                                  "(compose.set_minutes; a saved set plays as long as it was saved). "
                                  "A set never ends by itself: its last twenty seconds are faded out.",
                                  {},
                                  4,
                                  true };
        for ( double m : SetMinutes )
            length.m_choices.add(
                m < 60.0 ? juce::String( m, 0 ) + " min"
                         : ( m == 60.0 ? juce::String( "1 hour" ) : juce::String( m / 60.0, 1 ) + " hours" ) );
        renderkit::Option arc{
            "arc", "Arc", "The energy arc over the set (a saved set brings its own).", {}, 4, false
        };
        for ( int a = 0; a < phos::NumArcs; ++a ) arc.m_choices.add( phos::ArcNames[a] );
        return { { "what",
                   "What",
                   "A set of the length chosen, its tracks blended; or its first track alone, at its own tempo, as the "
                   "DJ export writes it.",
                   { "A set", "Its first track, alone" },
                   0,
                   true },
                 length,
                 arc };
    }
    bool applies( const renderkit::Item &item, const juce::String &key ) const override
    {
        return ( key != "arc" && key != "length" ) || row( item )->m_style >= 0;
    }
    double length( const renderkit::Item &item, const renderkit::Request &r ) override
    {
        if ( r.option( "what" ) == 0 ) return setMinutes( *row( item ), r ) * 60.0;
        phos::ParamStore params;
        phos::Composer   composer( renderkit::seedFor( item, r ) );
        juce::String     why;
        if ( !knobsOf( params, composer, *row( item ), r, why ) ) return 0.0;
        return trackSeconds( composer.track( params, 0 ) );
    }
    juce::String describe( const renderkit::Item &item, const renderkit::Request &r ) override
    {
        const std::shared_ptr<const Row> rw = row( item );
        juce::String                     s;
        if ( rw->m_factory >= 0 )
            s << "A set of the factory: " << phos::factorySet( rw->m_factory ).m_group << ", "
              << phos::factorySet( rw->m_factory ).m_note;
        else if ( rw->m_style < 0 )
            s << "Your set (" << rw->m_file.getFileName() << "): its seed, style, arc, knobs and "
              << juce::String( rw->m_minutes, 0 ) << " minutes";
        else
            s << "From " << phos::StyleNames[rw->m_style] << ", the arc "
              << phos::ArcNames[std::clamp( r.option( "arc", 4 ), 0, phos::NumArcs - 1 )];
        s << "\n"
          << ( r.option( "what" ) == 0 ? "A set, its tracks blended, faded out at the end"
                                       : "Its first track alone, at its own tempo" );
        return s;
    }
    std::unique_ptr<renderkit::Rendering> open( const renderkit::Item &item, const renderkit::Request &r,
                                                juce::String &why ) override
    {
        const std::uint64_t seed = renderkit::seedFor( item, r );
        if ( r.option( "what" ) == 0 )
        {
            auto rendering =
                std::make_unique<SetRendering>( *row( item ), r, seed, setMinutes( *row( item ), r ) * 60.0, why );
            if ( !rendering->ok() ) return nullptr;
            return rendering;
        }
        auto rendering = std::make_unique<TrackRendering>( *row( item ), r, seed, why );
        if ( !rendering->ok() ) return nullptr;
        return rendering;
    }
    juce::String testItem() const override { return "Full-On"; }

private:
    /**
     * @brief A row's style or saved set.
     * @param[in] item  the row
     * @return it (shared: a rendering may outlive a Refresh)
     */
    std::shared_ptr<const Row> row( const renderkit::Item &item ) const
    {
        const std::lock_guard<std::mutex> hold( m_lock );
        return m_store[static_cast<size_t>( item.m_index )];
    }
    mutable std::mutex                      m_lock;    ///< guards m_store
    std::vector<std::shared_ptr<const Row>> m_store;   ///< every row listed (only ever added to)
};

}   // namespace

std::unique_ptr<renderkit::Source> renderkit::makeSource()
{
    return std::make_unique<PhospheneSource>();
}
