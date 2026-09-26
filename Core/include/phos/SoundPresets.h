/**
 * @file SoundPresets.h
 * @brief Sound presets for every synth, in groups (23.09.2026, round "Presets"; the bank since 26.09.2026).
 *
 * Since 26.09.2026 the factory presets are a bank (PresetBank.cpp, from Tools/presets/bank_spec.py): sixteen groups
 * of sixty-four for each of nine synths -- kick, bass, acid and the six voices -- on the BerlinSchoolGenerator's
 * scheme, a group an eight by eight grid of the synth's adjectives (dark to bright) and the group's nouns, each
 * preset with its filter model and its modulation drawn from the group's recipes. The composer plays them: a track
 * chooses one per synth by its style (Composer::presetOf) and sends it absolute (ControlEvent::Kind::Base). Until
 * then the presets were the composer's own recipes laid out on purpose (24.09.2026).
 *
 * A preset is the text of the module's knobs that differ from their defaults (ParamStore::parseText form), so a
 * user preset is the same thing saved from the knobs as they stand (moduleText).
 */
#pragma once

#include "phos/Params.h"

#include <string>
#include <utility>
#include <vector>

namespace phos {

/** @brief One preset: where it is listed and what it sets. */
struct SoundPreset {
    std::string group;   ///< the submenu ("FM", "Wavetables: Ambient", "Resonant", "Liquid", ...)
    std::string name;    ///< the entry
    std::string text;    ///< `key=value` lines of the module's knobs that differ from their defaults
    /** @name A bank preset's place and sound (26.09.2026; empty for a user preset)
     *  @{ */
    int groupIndex = -1;                             ///< its group within the synth's sixteen
    float style[5] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };   ///< its group's weight in each style (StyleId order)
    std::vector<std::pair<int, float>> values;       ///< every knob the preset sets (all but presetLeaves), by index in the module
    float trimDb = 0.0f;   ///< what brings it to the loudness of the synth's default knobs (PresetTrims.inl), the composer's offset on Level
    /** @} */
};

/**
 * @brief The factory presets of a synth: Module::Kick, Module::Bass, Module::Acid, or Module::Poly with the
 *        instance (PolyInstance order). Built once per synth, thread-safe; empty for any other module.
 */
const std::vector<SoundPreset>& factoryPresets(Module module, int instance = 0);

/**
 * @brief The bank's presets of a synth, built afresh (PresetBank.cpp; 26.09.2026): 16 groups x 64, the names adjective +
 *        noun. factoryPresets() caches them; empty for a module without a bank.
 */
std::vector<SoundPreset> bankPresets(Module module, int instance = 0);
/** @brief How many keys the bank's table names that no module has (tests); @p first gets the first. */
int bankUnknownKeys(std::string* first = nullptr);

/**
 * @brief Whether a preset leaves knob @p k of @p module alone: the level, the ducking and, on a polyphonic voice,
 *        the high pass and the trance gate -- where the mix and the form put the synth, not what it sounds like.
 */
bool presetLeaves(Module module, int k);

/** @brief Puts the module's knobs to their defaults and applies @p text (a preset's); presetLeaves() knobs stay. */
bool applySoundPreset(ParamStore& params, Module module, int instance, const std::string& text);

/** @brief The module's knobs that differ from their defaults, as `key=value` lines (a user preset); presetLeaves() knobs are left out. */
std::string moduleText(const ParamStore& params, Module module, int instance = 0);

} // namespace phos
