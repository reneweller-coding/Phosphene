/**
 * @file Salts.h
 * @brief Every seed salt of the core in one place (23.09.2026, round "Salze").
 *
 * A salt is XOR-ed into a seed so that one decision draws from a stream of its own: then a new draw can be
 * added without moving any older one, which is how every round since Phase 5 has kept the music of the
 * rounds before it ("its own stream, so no older draw moves"). Until 22.09.2026 the salts lay in four files,
 * and three names existed twice with different values -- SaltPhrase (the bass figures, the percussion
 * phrase), SaltMode (a section's borrowed mode, the material of that mode) and SaltVoice (the voice
 * recipes, the voices' placement). Copying one of them into another file would have looked right and drawn
 * another stream. Now each file's salts live in a namespace named after the file, and the file says
 * `using namespace phos::salts::<file>`; the values are the ones they always had, so no note moves, and the
 * check at the end of this header refuses to compile if two of them are ever equal.
 *
 * **A new salt goes here**, in its file's namespace, with a value no other salt has (the check says so),
 * and into All below.
 * @author Rene Weller
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace phos::salts
{

/** @brief Salts of the set walk, the recipes, the voices, the bass (Composer.cpp). */
namespace composer
{
constexpr uint64_t SaltTrack = 0x545241434B000001ull;   ///< a track's seed from the set's
constexpr uint64_t SaltMotif =
    0x4D4F544946000017ull;   ///< the set's motif (23.09.2026, round "Set-Kurve"): its own stream, so no older draw of the walk moves
constexpr uint64_t SaltRecipe =
    0x5245434950450002ull;   ///< the tracks' sound recipes (the best of the candidates against the last four)
constexpr uint64_t SaltBlock =
    0x424C4F434B000003ull;   ///< the bass's pattern of a 16-bar block (the secondary one or not)
constexpr uint64_t SaltPhrase = 0x5048524153450004ull;   ///< the bass's phrase figures of a four-bar phrase
constexpr uint64_t SaltArc = 0x4152430000000005ull;      ///< a section's arc of the controls
constexpr uint64_t SaltPerc = 0x5045524300000006ull;     ///< a track's percussion seed
constexpr uint64_t SaltMelody = 0x4D454C4F44590007ull;   ///< a track's melody seed
constexpr uint64_t SaltWalk = 0x57414C4B00000008ull;     ///< the set's walk through keys and tempi
constexpr uint64_t SaltStyle =
    0x5354594C45000016ull;   ///< the style journey (22.09.2026): its own stream, so no older draw of the walk moves
constexpr uint64_t SaltFormU = 0x464F524D55000009ull;    ///< a track's form seed (the form unit)
constexpr uint64_t SaltSectU = 0x5345435455000010ull;    ///< a section's seed (the section unit)
constexpr uint64_t SaltLaneU = 0x4C414E4555000011ull;    ///< a percussion lane's seed (the lane unit)
constexpr uint64_t SaltReroll = 0x5245524F4C4C0012ull;   ///< a unit's seed after a reroll
constexpr uint64_t SaltAcidVoice =
    0x4143494456434500ull;   ///< Salt of the acid voicing draw: its own, so no other draw of the walk moves.
constexpr uint64_t SaltSoundPreset =
    0x534E445052535401ull;   ///< Salt of the track's sound presets (26.09.2026): their own stream, so no other draw of the walk moves.
constexpr uint64_t SaltVoice =
    0x564F494345520014ull;   ///< Salt of the voice recipes (19.09.2026): their own generator, so no other draw of the walk moves.
constexpr uint64_t SaltPadGate =
    0x504144474154ull;   ///< Salt of the section's choice between the track's two pad gate patterns (22.09.2026).
constexpr uint64_t SaltDroneRide = 0x44524944450015ull;   ///< Salt of the drone's slow evolution (19.09.2026).
constexpr uint64_t SaltBassLine =
    0x424153534C4E0013ull;   ///< Salt of the learned bass phrase: its own, so it does not move any other draw.
constexpr uint64_t SaltBassRhythm =
    0x424153535248546Dull;   ///< Salt of the drawn bass rhythm: its own, for the same reason.
}   // namespace composer

/** @brief Salts of the form, its sections and its effects (Form.cpp, FormSfx.cpp). */
namespace form
{
constexpr uint64_t SaltForm = 0x464F524D00000001ull;   ///< the form
constexpr uint64_t SaltKickRoll =
    0x4B49434B524F4C0Eull;   ///< the kick's roll before the pre-drop break (23.09.2026, round "Set-Kurve")
constexpr uint64_t SaltAbriss =
    0x41425249535300F0ull;   ///< the kick's Abriss before a sixteen-bar line of a drop (23.09.2026)
constexpr uint64_t SaltSection = 0x5345435449000002ull;   ///< the sections' details
constexpr uint64_t SaltGroup = 0x47524F5550000003ull;     ///< a group of sections' draw
constexpr uint64_t SaltSfx = 0x5346580000000004ull;       ///< the form's effects
constexpr uint64_t SaltFuzz = 0x46555A5A0000000Dull;      ///< the form's fuzziness (23.09.2026, round "Form")
constexpr uint64_t SaltMode = 0x4D4F44450000005ull;       ///< the section's borrowed mode (16.09.2026)
constexpr uint64_t SaltRide = 0x5249444500000006ull;      ///< the section's macro ride (16.09.2026)
constexpr uint64_t SaltPsy = 0x5053594300000007ull;       ///< the psychedelic ear candy (19.09.2026)
constexpr uint64_t SaltVoice = 0x564F494300000008ull;     ///< the voices' placement (19.09.2026)
constexpr uint64_t SaltBed = 0x4245440000000009ull;       ///< the shamanic bed's placement (19.09.2026)
constexpr uint64_t SaltFxRide = 0x46585244000000Aull;     ///< the modulation effects' section ride (19.09.2026)
constexpr uint64_t SaltVariant =
    0x564152490000000Bull;   ///< the voices' and the bed's variants (19.09.2026, round "voices")
constexpr uint64_t SaltClimax = 0x434C494D0000000Cull;   ///< drop 2's squelches (19.09.2026, round "arrangement")
}   // namespace form

/** @brief Salts of the melodic material (Melody.cpp). */
namespace melody
{
constexpr uint64_t SaltChords = 0x43484F5244000001ull;    ///< the chords
constexpr uint64_t SaltAcid = 0x4143494400000002ull;      ///< the acid's line
constexpr uint64_t SaltLead = 0x4C45414400000003ull;      ///< the lead's line
constexpr uint64_t SaltArp = 0x4152500000000004ull;       ///< the arp's line
constexpr uint64_t SaltSound = 0x534F554E44000006ull;     ///< the parts' sounds
constexpr uint64_t SaltPad = 0x5041440000000007ull;       ///< the pad's voicing
constexpr uint64_t SaltMode = 0x4D4F44450000008ull;       ///< the material of a borrowed mode (16.09.2026)
constexpr uint64_t SaltCounter = 0x434F554E5445000Aull;   ///< the counter-lead's material (19.09.2026)
constexpr uint64_t SaltStab = 0x535441420000000Bull;      ///< the stab's rhythm and material (19.09.2026)
constexpr uint64_t SaltDrone = 0x44524F4E4500000Cull;     ///< the drone's octave and evolution (19.09.2026)
constexpr uint64_t SaltColour = 0x434F4C4F55520009ull;    ///< the colour slots of a line (18.09.2026)
}   // namespace melody

/** @brief Salts of the percussion (Rhythm.cpp). */
namespace rhythm
{
constexpr uint64_t SaltPlan = 0x5045524350000001ull;     ///< the percussion's plan
constexpr uint64_t SaltPhrase = 0x5045524350000002ull;   ///< the percussion's phrase of four bars
constexpr uint64_t SaltFill = 0x5045524350000004ull;     ///< a bar's fill
constexpr uint64_t SaltLane = 0x5045524350000005ull;     ///< a lane's own stream
}   // namespace rhythm

/** @brief Every salt above, for the uniqueness check. */
inline constexpr uint64_t All[] = {
    composer::SaltTrack,
    composer::SaltMotif,
    composer::SaltRecipe,
    composer::SaltBlock,
    composer::SaltPhrase,
    composer::SaltArc,
    composer::SaltPerc,
    composer::SaltMelody,
    composer::SaltWalk,
    composer::SaltStyle,
    composer::SaltFormU,
    composer::SaltSectU,
    composer::SaltLaneU,
    composer::SaltReroll,
    composer::SaltAcidVoice,
    composer::SaltSoundPreset,
    composer::SaltVoice,
    composer::SaltPadGate,
    composer::SaltDroneRide,
    composer::SaltBassLine,
    composer::SaltBassRhythm,
    form::SaltForm,
    form::SaltKickRoll,
    form::SaltAbriss,
    form::SaltSection,
    form::SaltGroup,
    form::SaltSfx,
    form::SaltFuzz,
    form::SaltMode,
    form::SaltRide,
    form::SaltPsy,
    form::SaltVoice,
    form::SaltBed,
    form::SaltFxRide,
    form::SaltVariant,
    form::SaltClimax,
    melody::SaltChords,
    melody::SaltAcid,
    melody::SaltLead,
    melody::SaltArp,
    melody::SaltSound,
    melody::SaltPad,
    melody::SaltMode,
    melody::SaltCounter,
    melody::SaltStab,
    melody::SaltDrone,
    melody::SaltColour,
    rhythm::SaltPlan,
    rhythm::SaltPhrase,
    rhythm::SaltFill,
    rhythm::SaltLane,
};

/**
 * @brief True when no two entries of All are equal.
 * @return true when every salt differs
 */
constexpr bool allDistinct()
{
    constexpr std::size_t n = sizeof( All ) / sizeof( All[0] );
    for ( std::size_t i = 0; i < n; ++i )
        for ( std::size_t j = i + 1; j < n; ++j )
            if ( All[i] == All[j] ) return false;
    return true;
}
static_assert( allDistinct(), "two seed salts are equal: two decisions would draw the same stream" );

}   // namespace phos::salts
