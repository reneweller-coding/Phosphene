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
 * @author Rene Weller
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
    std::string m_style;   ///< StyleNames
    std::string m_key;     ///< KeyNames
    std::string m_scale;   ///< ScaleNames
    double m_bpm = 0.0;   ///< tempo
    int m_bars = 0;   ///< length
    std::string m_form;    ///< section letters with lengths, space-separated (see the file comment)
};

/** @brief What the gallery shows of a saved set. */
struct GalleryEntry {
    std::string m_name;                 ///< what the user called it
    std::string m_saved;                ///< ISO 8601 local time
    uint64_t m_seed = 0;                ///< from the set's own seed line
    std::vector<GalleryTrack> m_tracks; ///< the tracks planned when it was saved
};

/**
 * @brief A track's gallery line from its plan.
 * @param[in] plan  the track's plan
 * @return its gallery line
 */
GalleryTrack galleryTrackOf(const TrackPlan& plan);

/**
 * @brief The comment lines that make a `.phosset` a gallery entry.
 * @param[in] e  the entry
 * @return the comment lines
 */
std::string galleryComment(const GalleryEntry& e);
/**
 * @brief A set's text (writeSetText) with the gallery lines after its header line -- the header has to stay first.
 * @param[in] setText  the set's text
 * @param[in] e        the entry
 * @return the text with the gallery lines
 */
std::string withGalleryComment(const std::string& setText, const GalleryEntry& e);

/**
 * @brief Reads a gallery entry out of a `.phosset`'s text; false when the text has no seed line at all.
 * @param[in]  text  the set's text
 * @param[out] out   the entry
 * @return false when there is no seed line
 */
bool readGalleryEntry(std::string_view text, GalleryEntry& out);

/** @brief The listener's verdicts per set seed (Rating.h). */
struct RatingCount {
    int m_good = 0;   ///< the good verdicts
    int m_bad = 0;   ///< the bad verdicts
    int m_notes = 0;   ///< the written notes
};
/**
 * @brief Counts the verdicts of a ratings file by seed; an unreadable file gives an empty map.
 * @param[in] path  the ratings file
 * @return the verdicts by seed
 */
std::map<uint64_t, RatingCount> ratingsBySeed(const char* path);

} // namespace phos
