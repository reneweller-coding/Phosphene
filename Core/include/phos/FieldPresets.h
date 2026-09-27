/**
 * @file FieldPresets.h
 * @brief The Field track's factory presets (27.09.2026): one per recording of the shipped library and a group of
 *        scenes of two, from Tools/field_presets.py (FieldPresetData.inl).
 *
 * The user: "Sollten wir für die vorhandenen Samples dann Presets schreiben, die einfach in den Sampler geladen werden
 * können?" A preset names its recordings by file and by their place in the shipped library: the file where the
 * library has it (a user folder beside the shipped one moves the numbers), the shipped place where it cannot say --
 * so the composer's score is the same on a machine without the recordings, and the plan snapshots with it.
 * Loaded by hand (the Field tab's preset menu) it sets the knobs; the composer plays one per track when layer A's
 * category is Auto, drawn by the track's style (fieldPresetFor), as bases and overrides that leave the knobs alone.
 */
#pragma once
#include "phos/Params.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace phos {

/** @brief One Field preset. */
struct FieldPreset {
    const char* group;      ///< the menu's group (the category, or "Scenes")
    const char* name;       ///< the entry
    int categoryA;          ///< layer A's category (kFieldCategorySlugs index)
    const char* fileA;      ///< layer A's recording (the file's stem)
    int variationA;         ///< its variation in the shipped library
    int categoryB;          ///< layer B's category, -1 for no layer B
    const char* fileB;      ///< layer B's recording
    int variationB;         ///< its variation in the shipped library
    const char* text;       ///< `key=value` lines of the Field module's knobs that differ from their defaults
    float style[5];         ///< its weight in each style (StyleId order)
};

/** @brief The factory presets, in menu order. */
const std::vector<FieldPreset>& fieldPresets();
/** @brief The variation of @p category whose file is @p stem in the current library, -1 if it has none. */
int fieldVariationOf(int category, const std::string& stem);
/** @brief Whether every recording of @p preset is in the current library. */
bool fieldPresetAvailable(const FieldPreset& preset);
/**
 * @brief Every Field knob the preset puts where: the defaults, its text, its recordings' categories and variations.
 * @return (index in the Field module, value) for every knob of the module but the strip's own (none today)
 */
std::vector<std::pair<int, float>> fieldPresetValues(const ParamStore& p, const FieldPreset& preset);
/** @brief The variation a preset's layer plays: the file's in the library if it is there, else the shipped one. */
int fieldPresetVariation(int category, const char* file, int shipped);
/**
 * @brief A preset for a track in style @p style (StyleId), drawn by @p seed with the presets' style weights -- over all
 *        of them, whether their files are there or not, so the score does not depend on the machine.
 */
int fieldPresetFor(int style, uint64_t seed);

} // namespace phos
