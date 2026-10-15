/**
 * @file Rating.h
 * @brief The listener's verdicts as data (23.09.2026, round "Bewertung"): one line per "good here" or
 *        "bad here", with everything needed to find the bar again.
 *
 * Without ratings a listening session ends in prose ("the counters are hardly audible"), which is right
 * and names no bar. A rating is a line of tab-separated text -- seed, track, bar of the set, bar in the
 * track, section, style, verdict, note, where it came from -- that the plugin appends while it plays
 * (Perform tab), that the listening bench carries as two empty columns of its index (Tools/listen_bench.py),
 * and that Tools/ratings.py reads back. One format for all three, defined here, so the plugin and the tools
 * cannot drift apart. Text and not a binary: a person edits these files, too.
 * @author Rene Weller
 */
#pragma once

#include <cstdint>
#include <string>

namespace phos {

/** @brief One verdict. */
struct RatingEntry {
    uint64_t    m_seed = 0;          ///< the set seed
    int         m_track = 0;         ///< track number in the set, 1-based as printed everywhere
    int         m_bar = 0;           ///< bar of the set, 1-based
    int         m_barInTrack = 0;    ///< bar within the track, 1-based
    std::string m_section;           ///< section name (SectionNames), empty when unknown
    std::string m_style;             ///< style name (StyleNames), empty when unknown
    int         m_verdict = 0;       ///< +1 good, -1 bad, 0 a note without a verdict
    std::string m_note;              ///< free text; tabs and line breaks become spaces
    std::string m_source;            ///< "plugin", "bench", ...
    std::string m_time;              ///< when, ISO 8601 local time; empty when the writer has no clock
    /** @brief The composer's decisions at that bar, `name=value;...` (Preferences.h, decisionFeatures; 23.09.2026).
     *         An eleventh column; a file of ten columns reads with it empty. */
    std::string m_features;
};

/**
 * @brief The header line of a ratings file, without the line break.
 * @return the header line
 */
const char* ratingHeader();

/**
 * @brief One entry as a line of the ratings file, without the line break.
 * @param[in] e  the entry
 * @return the line
 */
std::string formatRating(const RatingEntry& e);

/**
 * @brief Reads one line of a ratings file.
 * @param[in]  line  the line
 * @param[out] out   the entry
 * @return false for the header, an empty line or a line with too few fields
 */
bool parseRating(const std::string& line, RatingEntry& out);

/**
 * @brief Appends @p e to the ratings file at @p path, writing the header first when the file is new.
 * @param[in] path  the file's path
 * @param[in] e     the entry
 * @return false if the file cannot be opened
 */
bool appendRating(const char* path, const RatingEntry& e);

} // namespace phos
