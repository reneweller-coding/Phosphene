/**
 * @file selftest_harmony.cpp
 * @brief The self test's checks of the harmony audit (10.11.2026): what keeps a sound on the note the score gives it.
 *
 * The composer keeps every line in its scale (testGenreRules); the audit measured what the *sounds* do with those
 * notes (Tools/tuning_audit.py over the 8192 factory presets, Tools/field_tones.py over the recordings) and found the
 * key held in the score and lost in six places of the sound. These sections hold the corrections: held pads and drones
 * narrow against the preset that really plays, the drone's fifth out from under a pad's minor sixth, the recordings
 * that carry a tone played in the key, two oscillators in unison centred on the note, the bank's pitch sweeps short
 * enough to land, and the jaw harp resting on harmonics of the key's root. The bowl's modes are testPsychedelia's
 * (selftest_mix.cpp), the whole FM ratios testPoly's (selftest_synths.cpp).
 * @author Rene Weller
 */
#include "SelfTestHelpers.h"
#include "phos/FieldLibrary.h"
#include "phos/FieldPresets.h"
#include "phos/SoundPresets.h"
#include "phos/Texture.h"

using namespace phos;

namespace phostest
{

namespace
{

/**
 * @brief The track whose pad and drone sound in @p bar: the incoming one over a DJ overlap, else the bar's own.
 * @param[in] p    the knobs
 * @param[in] c    the composer
 * @param[in] bar  the bar of the set
 * @return the track's index
 */
int voicesTrackOfBar( const ParamStore &p, const Composer &c, int bar )
{
    const int incoming = c.incomingOfBar( p, bar );
    return incoming >= 0 ? incoming : c.trackOfBar( p, bar );
}

/**
 * @brief The section of @p t that bar @p inTrack lies in.
 * @param[in] t        the track's plan
 * @param[in] inTrack  the bar within the track
 * @return the section's index, -1 outside the track
 */
int sectionOfBar( const TrackPlan &t, int inTrack )
{
    for ( int k = 0; k < t.m_form.m_count; ++k )
        if ( inTrack >= t.m_form.m_section[k].m_startBar &&
             inTrack < t.m_form.m_section[k].m_startBar + t.m_form.m_section[k].m_bars )
            return k;
    return -1;
}

}   // namespace

/**
 * @brief Harmony audit, part `.heldUnison`: in an intro and in a breakdown the pad's and the drone's unison stays
 *        within Szabo's curve at 0.15 (about 6 cents) and their second oscillator within 3 cents -- of what plays.
 *
 * The rule is the Dark-Ambient addon's (25.09.2026) and was written against the knob. Since the presets (26.09.2026)
 * a track's sound reaches the engine as the knob's *base*, and the composer's offset rides on that: a pad preset at
 * 0.44 under a knob at 0.35 played its breakdown at 0.24. The rule was also absent from a set's first intro, and the
 * drone's slow ride (Composer::droneControls) replaced it eight bars into every held section. Replayed here from the
 * control events alone: base, then offset, per beat, for every track of ten sets.
 */
void testHarmonyAuditHeldUnison()
{
    section( "harmony audit: held pads and drones keep their unison narrow, measured against the preset that plays" );
    int    checked = 0, wide = 0, wideOsc2 = 0, firstIntros = 0;
    double worst = 0.0, worstCents = 0.0;
    std::string where;
    for ( int run = 0; run < 10; ++run )
    {
        ParamStore p;
        p.parseText( fmt( "compose.track_bars=128 compose.level_match=Off master.auto_gain=Off master.auto_tone=Off compose.presence_match=Off "
                          "compose.audibility_match=Off compose.style=%d",
                          run % 5 )
                         .c_str() );
        Composer                  c( 0x48454C44ull + static_cast<uint64_t>( run ) * 7919ull );
        std::vector<NoteEvent>    ev;
        std::vector<ControlEvent> ctl;
        const int                 bars = 4 * 128;
        c.composeBars( p, 0, bars, ev, &ctl );
        for ( PolyInstanceE v : { PolyInstanceE::PAD, PolyInstanceE::DRONE } )
        {
            const int synth = 3 + polyIndex( v );
            for ( const int local : { static_cast<int>( poly::DETUNE ), static_cast<int>( poly::OSC2_DETUNE ) } )
            {
                const int        id = p.base( v ) + local;
                const ParamDesc &d = p.desc( id );
                const double     span = static_cast<double>( d.m_maxValue ) - d.m_minValue;
                double           base = p.get( id ), offset = 0.0;
                bool             seen = false;
                for ( size_t i = 0; i < ctl.size(); ++i )
                {
                    const ControlEvent &e = ctl[i];
                    if ( e.m_param != id ) continue;
                    if ( e.m_kind == ControlEvent::KindE::BASE )
                        base = e.m_length < 0.0f ? static_cast<double>( p.get( id ) ) : static_cast<double>( e.m_value );
                    else if ( e.m_kind == ControlEvent::KindE::OFFSET )
                        offset = e.m_value, seen = true;
                    // The state counts once the beat's last event of this knob is in: a track's start clears, sets a
                    // recipe's offset and then the preset's base, in that order, at one beat.
                    bool last = true;
                    for ( size_t j = i + 1; j < ctl.size() && ctl[j].m_beat <= e.m_beat && last; ++j )
                        last = ctl[j].m_param != id;
                    if ( !last || !seen ) continue;
                    const int bar = static_cast<int>( std::floor( e.m_beat / BeatsPerBar + 1e-9 ) );
                    if ( bar < 0 || bar >= bars ) continue;
                    const int       track = voicesTrackOfBar( p, c, bar );
                    const TrackPlan t = c.track( p, track );
                    const int       s = sectionOfBar( t, bar - t.m_firstBar );
                    if ( s < 0 ) continue;
                    const SectionTypeE type = t.m_form.m_section[s].m_type;
                    if ( type != SectionTypeE::INTRO && type != SectionTypeE::BREAK ) continue;
                    // A set's first section plays the user's knobs exactly where the synth has no preset.
                    const bool first = track == 0 && s == 0;
                    if ( first && Composer::presetOf( p, t, synth ) == nullptr ) continue;
                    ++checked;
                    if ( first && local == poly::DETUNE ) ++firstIntros;
                    if ( local == poly::DETUNE )
                    {
                        const double played = base + offset;
                        if ( played > 0.15 + 1e-4 )
                        {
                            ++wide;
                            if ( played > worst )
                            {
                                worst = played;
                                where = fmt( "run %d bar %d %s", run, bar, v == PolyInstanceE::PAD ? "pad" : "drone" );
                            }
                        }
                    }
                    else
                    {
                        const double cents = std::fabs( base + offset * span );
                        if ( cents > 3.0 + 1e-3 ) ++wideOsc2;
                        worstCents = std::max( worstCents, cents );
                    }
                }
            }
        }
    }
    check( checked > 200 && firstIntros >= 5 && wide == 0 && wideOsc2 == 0,
           "held pads and drones: unison at most 0.15 and the second oscillator within 3 cents of the first, in every "
           "intro and breakdown, the first intro of a set and the drone's ride included",
           fmt( "%d states checked (%d of them a set's first intro), %d with a wider unison (widest %.3f, %s), %d with a "
                "wider second oscillator (widest %.1f cents)",
                checked, firstIntros, wide, worst, where.empty() ? "-" : where.c_str(), wideOsc2, worstCents ) );
}

/**
 * @brief Harmony audit, part `.droneFifth`: the drone never holds its fifth under a pad's minor sixth.
 *
 * A minor sixth over the key is a semitone over the drone's fifth -- across the octaves a minor ninth, the interval the
 * user ruled out of pad and drone (25.09.2026) -- and the iv, the bVI and the Phrygian pad on the tonic all hold it.
 * It met the drone's fifth where the drone has its band to itself, on the silent floor of an intro or a breakdown:
 * 10.2 % of the beats the two share, over 80 tracks. In such a run the drone holds the octave in the fifth's place
 * (Melody.h, MelodyContext::m_droneBare).
 */
void testHarmonyAuditDroneFifth()
{
    section( "harmony audit: no drone fifth under a pad's minor sixth" );
    int    fifths = 0, octaves = 0, clashes = 0;
    double clashBeats = 0.0, shared = 0.0;
    for ( int run = 0; run < 10; ++run )
    {
        ParamStore p;
        p.parseText( fmt( "compose.track_bars=128 compose.pad_amount=1 compose.track_variation=1 compose.level_match=Off "
                          "master.auto_gain=Off master.auto_tone=Off compose.presence_match=Off compose.audibility_match=Off compose.style=%d",
                          run % 5 )
                         .c_str() );
        Composer               c( 0xD605ull + static_cast<uint64_t>( run ) * 7919ull );
        std::vector<NoteEvent> ev;
        const int              bars = 6 * 128;
        c.composeBars( p, 0, bars, ev );
        std::vector<const NoteEvent *> pads, drones;
        for ( const NoteEvent &e : ev )
        {
            if ( e.m_part == PartE::PAD ) pads.push_back( &e );
            if ( e.m_part == PartE::DRONE ) drones.push_back( &e );
        }
        for ( const NoteEvent *d : drones )
        {
            const int bar = std::clamp( static_cast<int>( d->m_beat / BeatsPerBar ), 0, bars - 1 );
            const int key = c.track( p, voicesTrackOfBar( p, c, bar ) ).m_key;
            const int pc = ( ( d->m_pitch - key ) % 12 + 12 ) % 12;
            if ( pc == 7 ) ++fifths;
            if ( pc == 0 ) ++octaves;
            for ( const NoteEvent *q : pads )
            {
                const double a = std::max( d->m_beat, q->m_beat );
                const double b = std::min( d->m_beat + d->m_length, q->m_beat + q->m_length );
                if ( b <= a ) continue;
                shared += b - a;
                // The same track's pad: over a DJ overlap two keys sound, and that pairing is testGenreRules'.
                const int padBar = std::clamp( static_cast<int>( q->m_beat / BeatsPerBar ), 0, bars - 1 );
                if ( voicesTrackOfBar( p, c, padBar ) != voicesTrackOfBar( p, c, bar ) ) continue;
                if ( pc == 7 && ( ( q->m_pitch - key ) % 12 + 12 ) % 12 == 8 && b - a > 1.0 )
                {
                    ++clashes;
                    clashBeats += b - a;
                }
            }
        }
    }
    check( fifths > 20 && octaves > 20 && clashes == 0,
           "the drone: its fifth never sounds for more than a beat under a pad's minor sixth of the same track",
           fmt( "%d drone fifths and %d roots or octaves over ten sets; %d fifths under a minor sixth, %.0f beats", fifths,
                octaves, clashes, clashBeats ) );
}

/**
 * @brief Harmony audit, part `.fieldTones`: a recording that carries a tone is played in the key, and one that is
 *        music of its own is not the composer's to draw.
 *
 * `fr-rain-roof-0158` is a tone at 604.5 Hz, D5 +50 cents, with all of its power in it (Tools/field_tones.py); under an
 * F# minor intro it lay 32 dB over the pad in its band. The composer transposes such a recording so that the tone
 * falls on the key's root, fourth or fifth -- at most 2.5 semitones -- and the engine does the same for a NASA shot.
 */
void testHarmonyAuditFieldTones()
{
    section( "harmony audit: field recordings with a tone play it in the key" );
    // (a) The table and the shift.
    {
        const float rain = fieldToneOf( "fr-rain-roof-0158_loop" );
        double      reach = 0.0, off = 0.0;
        for ( int key = 0; key < 12; ++key )
        {
            const double shift = fieldToneShift( rain, key );
            const double note = 69.0 + 12.0 * std::log2( static_cast<double>( rain ) / 440.0 ) + shift;
            double       pc = std::fmod( note - key, 12.0 );
            if ( pc < 0.0 ) pc += 12.0;
            off = std::max( off, std::min( { std::fabs( pc ), std::fabs( pc - 12.0 ), std::fabs( pc - 5.0 ),
                                             std::fabs( pc - 7.0 ) } ) );
            reach = std::max( reach, std::fabs( shift ) );
        }
        check( std::fabs( rain - 604.5f ) < 1.0f && fieldToneOf( "Webb - Cosmic Cliffs" ) == FieldToneMusic &&
                   fieldToneOf( "fr-rainforest-2342_loop" ) == 0.0f && fieldToneOf( nullptr ) == 0.0f && off < 1e-6 &&
                   reach <= 2.5 + 1e-9,
               "the tone table knows the rain on the roof, a sonification as music and a forest as neither; the shift "
               "puts a tone on root, fourth or fifth of every key within 2.5 semitones",
               fmt( "rain %.1f Hz, furthest from a target %.2g semitones, widest shift %.2f semitones", rain, off, reach ) );
    }
    // (b) No style draws a preset whose recording is music of its own.
    {
        int drawn = 0, music = 0;
        for ( int style = 0; style < 5; ++style )
            for ( uint64_t seed = 1; seed <= 4000; ++seed )
            {
                const int k = fieldPresetFor( style, mixSeed( seed, 0x4649454C44ull ) );
                if ( k < 0 ) continue;
                ++drawn;
                const FieldPreset &f = fieldPresets()[static_cast<size_t>( k )];
                if ( fieldToneOf( f.m_fileA ) == FieldToneMusic ||
                     ( f.m_categoryB >= 0 && fieldToneOf( f.m_fileB ) == FieldToneMusic ) )
                    ++music;
            }
        check( drawn == 20000 && music == 0, "the composer's draw of a track's place never takes a recording that is music",
               fmt( "%d of %d draws", music, drawn ) );
    }
    // (c) In the score: the track that plays a recording with a tone transposes its Field notes and sets the layer's
    //     fine tune so that the tone is on the key's root, fourth or fifth.
    {
        int    tuned = 0, wrong = 0;
        double worst = 0.0;
        for ( int run = 0; run < 400 && tuned < 6; ++run )
        {
            ParamStore p;
            p.parseText( fmt( "compose.track_bars=128 compose.level_match=Off master.auto_gain=Off master.auto_tone=Off "
                              "compose.presence_match=Off compose.audibility_match=Off compose.style=%d",
                              run % 5 )
                             .c_str() );
            Composer        c( 0xF1E1Dull + static_cast<uint64_t>( run ) * 104729ull );
            const TrackPlan t = c.track( p, 0 );
            const int       k = fieldPresetFor( t.m_style, mixSeed( t.m_formSeed, 0x4649454C44ull ) );
            if ( k < 0 ) continue;
            const FieldPreset &f = fieldPresets()[static_cast<size_t>( k )];
            const float        hz = fieldToneOf( f.m_fileA );
            if ( hz <= 0.0f ) continue;
            std::vector<NoteEvent>    ev;
            std::vector<ControlEvent> ctl;
            c.composeBars( p, 0, t.m_bars, ev, &ctl );
            // The semitones are the Field note's distance from middle C, the cents layer A's fine tune (a base).
            const int fb = p.base( ModuleE::FIELD );
            double    pitch = 1e9, fine = 0.0;
            for ( const NoteEvent &e : ev )
                if ( e.m_part == PartE::FIELD && pitch > 1e8 ) pitch = static_cast<double>( e.m_pitch ) - 60.0;
            if ( pitch > 1e8 ) continue;   // the place does not sound in this track
            for ( const ControlEvent &e : ctl )
                if ( e.m_param == fb + field::A_FINE && e.m_kind == ControlEvent::KindE::BASE && e.m_length >= 0.0f &&
                     e.m_beat < 1.0 )
                    fine = e.m_value;
            const double note = 69.0 + 12.0 * std::log2( static_cast<double>( hz ) / 440.0 ) + pitch + 0.01 * fine;
            double       pc = std::fmod( note - t.m_key, 12.0 );
            if ( pc < 0.0 ) pc += 12.0;
            const double d =
                std::min( { std::fabs( pc ), std::fabs( pc - 12.0 ), std::fabs( pc - 5.0 ), std::fabs( pc - 7.0 ) } );
            ++tuned;
            worst = std::max( worst, d );
            if ( d > 0.02 || std::fabs( pitch + 0.01 * fine ) > 2.5 + 1e-6 ) ++wrong;
        }
        check( tuned >= 3 && wrong == 0,
               "a track whose place carries a tone transposes the recording onto its key's root, fourth or fifth",
               fmt( "%d such first tracks found, %d beside a target (furthest %.3f semitones)", tuned, wrong, worst ) );
    }
}

/**
 * @brief Harmony audit, part `.unisonCentre`: two oscillators in unison share the note -- osc2_detune is the distance
 *        between them, and their centre of power stays on the note whatever the mix.
 *
 * Until 10.11.2026 the first oscillator stood on the note and the second above it: every voice with a unison partner
 * played 4 .. 9 cents sharp of the bass and the acid (448 factory presets, a median of +4.3 cents). At any other
 * interval the detune is still the partner's alone.
 */
void testHarmonyAuditUnisonCentre()
{
    section( "harmony audit: two oscillators in unison are centred on the note" );
    const double sr = 48000.0;
    const int    pitch = 81;   // A5: 20 cents are 10 Hz
    const double f0 = midiToHz( pitch );
    // The unison spread stays on (detune 0.6): with every slot on one frequency the slots of an oscillator add by
    // their random start phases, and the share of the power each oscillator has would be a matter of luck per note.
    auto         centre = [&]( const char *osc, const char *interval, double mix, double cents )
    {
        const std::string s = fmt( "lead.osc=%s lead.wave=0 lead.osc2=%s lead.osc2_interval=%s lead.osc2_mix=%g "
                                   "lead.osc2_detune=%g lead.lfo_cutoff=0 lead.lfo_pitch=0 lead.lfo_amp=0 lead.drift=0 "
                                   "lead.glide=0 lead.env_amount=0 ",
                                   osc, osc, interval, mix, cents ) +
                              commonPoly( "lead", 0.6 );
        const size_t              n = 1u << 18;
        const std::vector<float>  y = renderPolyNote( s, PolyInstanceE::LEAD, pitch, 9600 + n, sr );
        std::vector<float>        w( n );
        for ( size_t i = 0; i < n; ++i )
            w[i] = y[9600 + i] * static_cast<float>( 0.5 - 0.5 * std::cos( 2.0 * PiD * static_cast<double>( i ) / n ) );
        const std::vector<double> pw = powerSpectrum( w.data(), n );
        double                    sum = 0.0, moment = 0.0;
        for ( size_t k = 1; k < pw.size(); ++k )
        {
            const double c = 1200.0 * std::log2( ( static_cast<double>( k ) * sr / n ) / f0 );
            if ( std::fabs( c ) > 60.0 ) continue;
            sum += pw[k];
            moment += pw[k] * c;
        }
        return sum > 0.0 ? moment / sum : 1e9;
    };
    const double half = centre( "VA", "Unison", 0.5, 20.0 ), quarter = centre( "VA", "Unison", 0.25, 20.0 );
    const double down = centre( "Supersaw", "Unison", 0.4, -30.0 );
    // An octave up the detune is the partner's alone: the first oscillator's fundamental stays on the note.
    const double octave = centre( "VA", "+1 Oct", 0.5, 20.0 );
    check( std::fabs( half ) < 2.0 && std::fabs( quarter ) < 2.0 && std::fabs( down ) < 2.0 && std::fabs( octave ) < 1.0,
           "the centre of the two oscillators' fundamentals is the note at any mix; at another interval the detune stays "
           "with the partner",
           fmt( "centre of the fundamental at mix 0.5 / 0.25 with +20 cents: %+.2f / %+.2f cents, a supersaw pair at mix "
                "0.4 with -30: %+.2f; with the partner an octave up: %+.2f",
                half, quarter, down, octave ) );
}

/**
 * @brief Harmony audit, part `.bank`: what the bank's recipes promise the note -- a pitch sweep that goes on at its
 *        own speed when the note is let go and is short enough to land inside a sixteenth, and no unison past the
 *        knee of Szabo's curve.
 *
 * The modulation envelope's release was left at its default of 400 ms: a Laser Arp's note of 20 ms let go of its
 * sweep 2.7 semitones up and carried it there through the whole tail (64 of 64 presets heard more than a semitone
 * sharp; 483 presets more than 25 cents beside their note). And between 0.9 and 1.0 the detune curve goes from
 * +-66 to +-181 cents: sixteen Hoover leads and sixteen Hoover stabs had up to 42 % of their fundamental's power more
 * than a semitone from the note.
 */
void testHarmonyAuditBank()
{
    section( "harmony audit: the bank's pitch sweeps land, and no preset's unison is wider than a semitone" );
    int    sweeps = 0, slowRelease = 0, longSweep = 0, wide = 0, presets = 0;
    double longest = 0.0, widest = 0.0;
    for ( int v = 0; v < PolyInstances; ++v )
    {
        ParamStore p;
        const int  b = p.base( ModuleE::POLY, v );
        for ( const SoundPreset &sp : factoryPresets( ModuleE::POLY, v ) )
        {
            ++presets;
            std::vector<float> val( static_cast<size_t>( poly::COUNT ) );
            for ( int k = 0; k < poly::COUNT; ++k ) val[static_cast<size_t>( k )] = p.defaultValue( b + k );
            for ( const auto &kv : sp.m_values ) val[static_cast<size_t>( kv.first )] = kv.second;
            if ( std::lround( val[poly::OSC] ) == 0 && val[poly::DETUNE] > 0.9f + 1e-4f ) ++wide;
            if ( std::lround( val[poly::OSC] ) == 0 ) widest = std::max( widest, static_cast<double>( val[poly::DETUNE] ) );
            bool onPitch = false;
            for ( int s = 0; s < 8; ++s )
            {
                const int src = poly::MX1_SRC + 3 * s;
                onPitch = onPitch ||
                          ( std::lround( val[static_cast<size_t>( src )] ) == static_cast<int>( ModSourceE::MOD_ENV ) &&
                            std::lround( val[static_cast<size_t>( src + 1 )] ) == static_cast<int>( ModDestE::PITCH ) &&
                            val[static_cast<size_t>( src + 2 )] != 0.0f );
            }
            if ( !onPitch ) continue;
            ++sweeps;
            const double decay = val[poly::MENV_DECAY], release = val[poly::MENV_RELEASE];
            if ( std::fabs( release / decay - 5.0 / 3.0 ) > 0.03 ) ++slowRelease;
            if ( decay > 140.0 + 0.5 ) ++longSweep;
            longest = std::max( longest, decay );
        }
    }
    check( presets == 6 * 1024 && sweeps >= 400 && slowRelease == 0 && longSweep == 0 && wide == 0,
           "every preset that sweeps its pitch with the modulation envelope lets the sweep run on at the same speed "
           "after the note (release = 5/3 decay) and ends it within 140 ms; no supersaw preset stands over 0.9",
           fmt( "%d sweeping presets of %d: %d with another release, %d longer (longest %.0f ms); %d supersaws over 0.9 "
                "(widest %.2f)",
                sweeps, presets, slowRelease, longSweep, longest, wide, widest ) );
}

/**
 * @brief Harmony audit, part `.jawHarp`: the jaw harp's drone is the key's root, and its mouth resonance rests on
 *        harmonics that are octaves of the root or of the fifth.
 *
 * Until 10.11.2026 the drone stood on the fifth and the resonance rested at 900 Hz and at 900 Hz times the sweep,
 * wherever those fell among the drone's harmonics: in F# on the key's major second and on a third between minor and
 * major, at Q 10 -- one harmonic at a time, held for an eighth.
 */
void testHarmonyAuditJawHarp()
{
    section( "harmony audit: the jaw harp rests on the root's octaves and fifths" );
    const double sr = 48000.0;
    ParamStore   p;
    int          keys = 0, good = 0;
    std::string  detail;
    for ( const int key : { 0, 4, 6, 9, 11 } )
    {
        Texture x;
        x.prepare( sr );
        std::vector<float> v = moduleValues( p, ModuleE::TEXTURE );
        x.update( v.data(), key );
        // At 20 BPM: an eighth is 1.5 s, so the resonance (which needs 180 ms to arrive) is heard where it rests. At a
        // set's tempo the next pluck sends it back before it has settled, and the average is the sweep.
        const size_t n = 1u << 20;
        x.trigger( SfxTypeE::JAW_HARP, static_cast<int>( n ), 1.0f, 0.0, sr * 3.0, 4711 );
        const std::vector<float>  y = renderMono( [&]( float *L, float *R, int k ) { x.process( L, R, k ); }, n );
        const std::vector<double> pw = powerSpectrum( y.data(), n );
        const double              f0 = midiToHz( ( key >= 9 ? 36 : 48 ) + key );
        // The power of each harmonic of the root between 500 Hz and 2.6 kHz (10 Hz either side: a plucked harmonic
        // has sidebands). The loudest is one rest; the loudest at least a fourth away from it is the other.
        std::vector<std::pair<double, int>> h;
        for ( int k = 2; k * f0 < 2600.0; ++k )
        {
            if ( k * f0 < 500.0 ) continue;
            double       s = 0.0;
            const size_t c = static_cast<size_t>( std::lround( k * f0 * n / sr ) );
            const size_t half = static_cast<size_t>( std::lround( 10.0 * n / sr ) );
            for ( size_t j = c - half; j <= c + half; ++j ) s += pw[j];
            h.emplace_back( s, k );
        }
        std::sort( h.rbegin(), h.rend() );
        ++keys;
        auto rest = []( int k ) { return k == 4 || k == 6 || k == 8 || k == 12 || k == 16 || k == 24 || k == 32; };
        int  second = 0;
        for ( size_t i = 1; i < h.size() && second == 0; ++i )
            if ( h[i].second >= 1.3 * h[0].second || h[0].second >= 1.3 * h[i].second ) second = h[i].second;
        // The harmonics beside the loudest, for the report: the resonance (Q 10) is 90 .. 170 Hz wide, and the fixed
        // formant at 450 Hz and the dry buzz lie under it, so they come 3 .. 6 dB under the one it rests on.
        double beside = 0.0;
        for ( const auto &e : h )
            if ( std::abs( e.second - h[0].second ) == 1 ) beside = std::max( beside, e.first );
        const bool ok = !h.empty() && rest( h[0].second ) && rest( second ) && beside < h[0].first;
        good += ok ? 1 : 0;
        detail += fmt( "key %d: harmonics %d and %d of %.0f Hz, the ones beside %.0f dB under; ", key,
                       h.empty() ? 0 : h[0].second, second, f0, h.empty() ? 0.0 : -powDb( beside / h[0].first ) );
    }
    check( good == keys,
           "the jaw harp's resonance rests on octaves of the key's root or fifth in every key, louder than the harmonics "
           "beside them",
           detail );
}

}   // namespace phostest
