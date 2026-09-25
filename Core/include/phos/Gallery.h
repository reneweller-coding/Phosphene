/**
 * @file Gallery.h
 * @brief The set gallery (23.09.2026, round "Galerie"): saved sets with what they sound like at a glance.
 *
 * A gallery entry is an ordinary `.phosset` (SetFile.h) with a few comment lines after its header, which the set
 * reader skips and the gallery reads: a name, when it was saved, and per track its style, key, mode, tempo, length and
 * form. The form is a string of section letters with their lengths ("I32 G32 B16 D32 K32 B24 C56 O32": intro,
 * groove, build, drop, breakdown -- K for "Kahlschlag", the floor gone --, climax, outro), which is enough to
 * draw a thumbnail of the whole set without planning it. Because the lines are comments, every gallery file
 * loads anywhere a set loads (`phos_render --set-file`), and a set saved by hand can be put into the gallery
 * folder and simply has no thumbnail.
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace phos {

struct TrackPlan;

/** @brief One track of a gallery entry. */
struct GalleryTrack {
    std::string style;   ///< kStyleNames
    std::string key;     ///< kKeyNames
    std::string scale;   ///< kScaleNames
    double bpm = 0.0;   ///< tempo
    int bars = 0;   ///< length
    std::string form;    ///< section letters with lengths, space-separated (see the file comment)
};

/** @brief What the gallery shows of a saved set. */
struct GalleryEntry {
    std::string name;                 ///< what the user called it
    std::string saved;                ///< ISO 8601 local time
    uint64_t seed = 0;                ///< from the set's own seed line
    std::vector<GalleryTrack> tracks; ///< the tracks planned when it was saved
};

/** @brief A track's gallery line from its plan. */
GalleryTrack galleryTrackOf(const TrackPlan& plan);

/** @brief The comment lines that make a `.phosset` a gallery entry. */
std::string galleryComment(const GalleryEntry& e);
/** @brief A set's text (writeSetText) with the gallery lines after its header line -- the header has to stay first. */
std::string withGalleryComment(const std::string& setText, const GalleryEntry& e);

/** @brief Reads a gallery entry out of a `.phosset`'s text; false when the text has no seed line at all. */
bool readGalleryEntry(std::string_view text, GalleryEntry& out);

/** @brief The listener's verdicts per set seed (Rating.h). */
struct RatingCount {
    int good = 0, bad = 0, notes = 0;   ///< verdicts and written notes
};
/** @brief Counts the verdicts of a ratings file by seed; an unreadable file gives an empty map. */
std::map<uint64_t, RatingCount> ratingsBySeed(const char* path);

} // namespace phos
