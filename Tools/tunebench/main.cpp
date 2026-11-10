/**
 * @file main.cpp
 * @brief phos_tunebench: every factory preset of one synth playing one note, short and long, as raw audio.
 *
 * The harmony audit of 10.11.2026 measured where a preset's energy lies against the pitch of the note it plays
 * (Tools/tuning_audit.py reads what this writes): a pitch envelope that has not settled when a sixteenth ends, an FM
 * ratio that puts partials beside the note's harmonics, a unison spread wider than a semitone. The composer keeps every
 * line in its scale; this bench is the check that the sounds keep the notes.
 *
 *     phos_tunebench <synth 1..8> <out.f32> <out.tsv> [first] [count] [extra "key=value;..."]
 *
 * synth: 1 bass, 2 acid, 3 lead, 4 counter, 5 arp, 6 stab, 7 pad, 8 drone. Per preset the part's stem (stereo float32,
 * 48 kHz): 0.75 s with a note of the part's usual length, then 3 s with a note held for 2.9 s, both at the part's median
 * pitch in a set. The table names the presets in the order of the audio.
 * @author Rene Weller
 */
#include "phos/Engine.h"
#include "phos/FieldLibrary.h"
#include "phos/SoundPresets.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace phos;

/**
 * @brief Renders the presets.
 * @param[in] argc  the number of arguments
 * @param[in] argv  synth, audio file, table, optionally the first preset, how many, and knobs set after each preset
 * @return 0, 2 on a missing argument, 3 when a file cannot be written
 */
int main( int argc, char **argv )
{
    if ( argc < 4 ) return 2;
    const int synth = std::atoi( argv[1] );
    const int first = argc > 4 ? std::atoi( argv[4] ) : 0;
    const int count = argc > 5 ? std::atoi( argv[5] ) : 1 << 20;
    static const char *const names[9] = { "", "bass", "acid", "lead", "counter", "arp", "stab", "pad", "drone" };
    static const PartE       parts[9] = { PartE::KICK, PartE::BASS,  PartE::ACID, PartE::LEAD, PartE::COUNTER,
                                          PartE::ARP,  PartE::STAB,  PartE::PAD,  PartE::DRONE };
    // The part's median pitch and note length in a set (measured from a 30-minute score, seed 1).
    static const int   pitch[9] = { 0, 32, 57, 69, 82, 73, 63, 61, 54 };
    static const float shortBeats[9] = { 0, 0.17f, 0.14f, 0.25f, 0.30f, 0.05f, 0.50f, 1.50f, 1.50f };
    const ModuleE      module = synth == 1 ? ModuleE::BASS : synth == 2 ? ModuleE::ACID : ModuleE::POLY;
    const int          instance = synth >= 3 ? synth - 3 : 0;
    const double       sr = 48000.0;
    const int          shortN = 36000, longN = 144000;   // 0.75 s and 3 s
    const float        longBeats = static_cast<float>( 2.9 * 145.0 / 60.0 );

    setFieldPreloadBlocking( true );
    auto engine = std::make_unique<Engine>();
    engine->prepare( sr, 512 );
    ParamStore &p = engine->params();
    p.parseText( "compose.bpm=145" );
    const std::vector<SoundPreset> &bank = factoryPresets( module, instance );
    std::FILE                      *fa = std::fopen( argv[2], "wb" );
    std::FILE                      *ft = std::fopen( argv[3], "w" );
    if ( !fa || !ft ) return 3;
    std::vector<std::vector<float>> sl( NumStems, std::vector<float>( 512 ) ), sr2( NumStems, std::vector<float>( 512 ) );
    StemTap                         tap;
    for ( int k = 0; k < NumStems; ++k )
    {
        tap.m_L[k] = sl[static_cast<size_t>( k )].data();
        tap.m_R[k] = sr2[static_cast<size_t>( k )].data();
    }
    std::vector<float> L( 512 ), R( 512 ), out;
    const int          stem = static_cast<int>( parts[synth] );
    for ( int i = first; i < static_cast<int>( bank.size() ) && i < first + count; ++i )
    {
        const SoundPreset &sp = bank[static_cast<size_t>( i )];
        applySoundPreset( p, module, instance, sp.m_text );
        if ( synth >= 2 ) p.parseText( ( std::string( names[synth] ) + ".delay_send=0" ).c_str() );
        if ( argc > 6 ) p.parseText( argv[6] );
        std::string text = sp.m_text;
        for ( char &c : text )
            if ( c == '\n' || c == '\t' ) c = ';';
        std::fprintf( ft, "%s\t%d\t%s\t%s\t%s\n", names[synth], i, sp.m_group.c_str(), sp.m_name.c_str(), text.c_str() );
        for ( int pass = 0; pass < 2; ++pass )
        {
            const int n = pass == 0 ? shortN : longN;
            engine->reset();
            engine->setStemTap( &tap );
            NoteEvent e;
            e.m_part = parts[synth];
            e.m_beat = 0.0;
            e.m_length = pass == 0 ? shortBeats[synth] : longBeats;
            e.m_pitch = static_cast<uint8_t>( pitch[synth] );
            e.m_velocity = 100;
            engine->pushEvent( e );
            out.assign( static_cast<size_t>( 2 * n ), 0.0f );
            for ( int done = 0; done < n; done += 512 )
            {
                const int m = std::min( 512, n - done );
                engine->process( L.data(), R.data(), m );
                for ( int k = 0; k < m; ++k )
                {
                    out[static_cast<size_t>( 2 * ( done + k ) )] = sl[static_cast<size_t>( stem )][static_cast<size_t>( k )];
                    out[static_cast<size_t>( 2 * ( done + k ) + 1 )] = sr2[static_cast<size_t>( stem )][static_cast<size_t>( k )];
                }
            }
            std::fwrite( out.data(), sizeof( float ), out.size(), fa );
        }
    }
    std::fclose( fa );
    std::fclose( ft );
    return 0;
}
