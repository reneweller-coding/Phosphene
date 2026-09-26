/**
 * @file Salts.h
 * @brief Every seed salt of the core in one place (23.09.2026, round "Salze").
 *
 * A salt is XOR-ed into a seed so that one decision draws from a stream of its own: then a new draw can be
 * added without moving any older one, which is how every round since Phase 5 has kept the music of the
 * rounds before it ("its own stream, so no older draw moves"). Until 22.09.2026 the salts lay in four files,
 * and three names existed twice with different values -- kSaltPhrase (the bass figures, the percussion
 * phrase), kSaltMode (a section's borrowed mode, the material of that mode) and kSaltVoice (the voice
 * recipes, the voices' placement). Copying one of them into another file would have looked right and drawn
 * another stream. Now each file's salts live in a namespace named after the file, and the file says
 * `using namespace phos::salts::<file>`; the values are the ones they always had, so no note moves, and the
 * check at the end of this header refuses to compile if two of them are ever equal.
 *
 * **A new salt goes here**, in its file's namespace, with a value no other salt has (the check says so),
 * and into kAll below.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace phos::salts {

/** @brief Salts of the set walk, the recipes, the voices, the bass (Composer.cpp). */
namespace composer {
constexpr uint64_t kSaltTrack      = 0x545241434B000001ull;
constexpr uint64_t kSaltMotif      = 0x4D4F544946000017ull;   ///< the set's motif (23.09.2026, round "Set-Kurve"): its own stream, so no older draw of the walk moves
constexpr uint64_t kSaltRecipe     = 0x5245434950450002ull;
constexpr uint64_t kSaltBlock      = 0x424C4F434B000003ull;
constexpr uint64_t kSaltPhrase     = 0x5048524153450004ull;
constexpr uint64_t kSaltArc        = 0x4152430000000005ull;
constexpr uint64_t kSaltPerc       = 0x5045524300000006ull;
constexpr uint64_t kSaltMelody     = 0x4D454C4F44590007ull;
constexpr uint64_t kSaltWalk       = 0x57414C4B00000008ull;
constexpr uint64_t kSaltStyle      = 0x5354594C45000016ull;   ///< the style journey (22.09.2026): its own stream, so no older draw of the walk moves
constexpr uint64_t kSaltFormU      = 0x464F524D55000009ull;
constexpr uint64_t kSaltSectU      = 0x5345435455000010ull;
constexpr uint64_t kSaltLaneU      = 0x4C414E4555000011ull;
constexpr uint64_t kSaltReroll     = 0x5245524F4C4C0012ull;
constexpr uint64_t kSaltAcidVoice  = 0x4143494456434500ull;   ///< Salt of the acid voicing draw: its own, so no other draw of the walk moves.
constexpr uint64_t kSaltSoundPreset = 0x534E445052535401ull;   ///< Salt of the track's sound presets (26.09.2026): their own stream, so no other draw of the walk moves.
constexpr uint64_t kSaltVoice      = 0x564F494345520014ull;   ///< Salt of the voice recipes (19.09.2026): their own generator, so no other draw of the walk moves.
constexpr uint64_t kSaltPadGate    = 0x504144474154ull;   ///< Salt of the section's choice between the track's two pad gate patterns (22.09.2026).
constexpr uint64_t kSaltDroneRide  = 0x44524944450015ull;   ///< Salt of the drone's slow evolution (19.09.2026).
constexpr uint64_t kSaltBassLine   = 0x424153534C4E0013ull;   ///< Salt of the learned bass phrase: its own, so it does not move any other draw.
constexpr uint64_t kSaltBassRhythm = 0x424153535248546Dull;   ///< Salt of the drawn bass rhythm: its own, for the same reason.
} // namespace composer

/** @brief Salts of the form, its sections and its effects (Form.cpp, FormSfx.cpp). */
namespace form {
constexpr uint64_t kSaltForm     = 0x464F524D00000001ull;
constexpr uint64_t kSaltKickRoll = 0x4B49434B524F4C0Eull;   ///< the kick's roll before the pre-drop break (23.09.2026, round "Set-Kurve")
constexpr uint64_t kSaltAbriss   = 0x41425249535300F0ull;   ///< the kick's Abriss before a sixteen-bar line of a drop (23.09.2026)
constexpr uint64_t kSaltSection  = 0x5345435449000002ull;
constexpr uint64_t kSaltGroup    = 0x47524F5550000003ull;
constexpr uint64_t kSaltSfx      = 0x5346580000000004ull;
constexpr uint64_t kSaltFuzz     = 0x46555A5A0000000Dull;   ///< the form's fuzziness (23.09.2026, round "Form")
constexpr uint64_t kSaltMode     = 0x4D4F44450000005ull;   ///< the section's borrowed mode (16.09.2026)
constexpr uint64_t kSaltRide     = 0x5249444500000006ull;   ///< the section's macro ride (16.09.2026)
constexpr uint64_t kSaltPsy      = 0x5053594300000007ull;   ///< the psychedelic ear candy (19.09.2026)
constexpr uint64_t kSaltVoice    = 0x564F494300000008ull;   ///< the voices' placement (19.09.2026)
constexpr uint64_t kSaltBed      = 0x4245440000000009ull;   ///< the shamanic bed's placement (19.09.2026)
constexpr uint64_t kSaltFxRide   = 0x46585244000000Aull;   ///< the modulation effects' section ride (19.09.2026)
constexpr uint64_t kSaltVariant  = 0x564152490000000Bull;   ///< the voices' and the bed's variants (19.09.2026, round "voices")
constexpr uint64_t kSaltClimax   = 0x434C494D0000000Cull;   ///< drop 2's squelches (19.09.2026, round "arrangement")
} // namespace form

/** @brief Salts of the melodic material (Melody.cpp). */
namespace melody {
constexpr uint64_t kSaltChords  = 0x43484F5244000001ull;
constexpr uint64_t kSaltAcid    = 0x4143494400000002ull;
constexpr uint64_t kSaltLead    = 0x4C45414400000003ull;
constexpr uint64_t kSaltArp     = 0x4152500000000004ull;
constexpr uint64_t kSaltSound   = 0x534F554E44000006ull;
constexpr uint64_t kSaltPad     = 0x5041440000000007ull;
constexpr uint64_t kSaltMode    = 0x4D4F44450000008ull;   ///< the material of a borrowed mode (16.09.2026)
constexpr uint64_t kSaltCounter = 0x434F554E5445000Aull;   ///< the counter-lead's material (19.09.2026)
constexpr uint64_t kSaltStab    = 0x535441420000000Bull;   ///< the stab's rhythm and material (19.09.2026)
constexpr uint64_t kSaltDrone   = 0x44524F4E4500000Cull;   ///< the drone's octave and evolution (19.09.2026)
constexpr uint64_t kSaltColour  = 0x434F4C4F55520009ull;   ///< the colour slots of a line (18.09.2026)
} // namespace melody

/** @brief Salts of the percussion (Rhythm.cpp). */
namespace rhythm {
constexpr uint64_t kSaltPlan   = 0x5045524350000001ull;
constexpr uint64_t kSaltPhrase = 0x5045524350000002ull;
constexpr uint64_t kSaltFill   = 0x5045524350000004ull;
constexpr uint64_t kSaltLane   = 0x5045524350000005ull;
} // namespace rhythm

/** @brief Every salt above, for the uniqueness check. */
inline constexpr uint64_t kAll[] = {
    composer::kSaltTrack,
    composer::kSaltMotif,
    composer::kSaltRecipe,
    composer::kSaltBlock,
    composer::kSaltPhrase,
    composer::kSaltArc,
    composer::kSaltPerc,
    composer::kSaltMelody,
    composer::kSaltWalk,
    composer::kSaltStyle,
    composer::kSaltFormU,
    composer::kSaltSectU,
    composer::kSaltLaneU,
    composer::kSaltReroll,
    composer::kSaltAcidVoice,
    composer::kSaltSoundPreset,
    composer::kSaltVoice,
    composer::kSaltPadGate,
    composer::kSaltDroneRide,
    composer::kSaltBassLine,
    composer::kSaltBassRhythm,
    form::kSaltForm,
    form::kSaltKickRoll,
    form::kSaltAbriss,
    form::kSaltSection,
    form::kSaltGroup,
    form::kSaltSfx,
    form::kSaltFuzz,
    form::kSaltMode,
    form::kSaltRide,
    form::kSaltPsy,
    form::kSaltVoice,
    form::kSaltBed,
    form::kSaltFxRide,
    form::kSaltVariant,
    form::kSaltClimax,
    melody::kSaltChords,
    melody::kSaltAcid,
    melody::kSaltLead,
    melody::kSaltArp,
    melody::kSaltSound,
    melody::kSaltPad,
    melody::kSaltMode,
    melody::kSaltCounter,
    melody::kSaltStab,
    melody::kSaltDrone,
    melody::kSaltColour,
    rhythm::kSaltPlan,
    rhythm::kSaltPhrase,
    rhythm::kSaltFill,
    rhythm::kSaltLane,
};

/** @brief True when no two entries of kAll are equal. */
constexpr bool allDistinct()
{
    constexpr std::size_t n = sizeof(kAll) / sizeof(kAll[0]);
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = i + 1; j < n; ++j)
            if (kAll[i] == kAll[j]) return false;
    return true;
}
static_assert(allDistinct(), "two seed salts are equal: two decisions would draw the same stream");

} // namespace phos::salts
