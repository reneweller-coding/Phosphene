/**
 * @file Preferences.h
 * @brief What the listener liked, learned from the ratings (23.09.2026, round "Präferenzen").
 *
 * A rating (Rating.h) carries the composer's decisions at the rated bar as *features* -- `style=Goa`,
 * `section=Drop`, `lead.archetype=Surge`, `counter.mode=Answer`, `harmony=loop` and a few more (decisionFeatures).
 * fitPreferences() turns a pile of good and bad verdicts into one weight per feature value: the log-odds of "good"
 * against "bad" among the ratings that carry it, smoothed (one pseudo-verdict each way) and shrunk towards zero by
 * how few ratings there are (n / (n + 4)), so three verdicts move a draw a little and thirty move it clearly. It is
 * counting, not a black box: the file it writes is one line per feature value and can be read and edited by hand.
 *
 * **Where it acts.** Only on draws that are already weighted choices between alternatives the rules allow: the form
 * template, the lead archetype, the arp style, the counter mode, and the chance of a loop against the pendulum. Each
 * alternative's weight is multiplied by exp(weight) -- the rules decide what is allowed, the listener's taste shifts
 * how often each allowed thing is chosen ([[rules-over-corpus]] with the listener in the corpus's place). With no
 * preferences set, every factor is exactly 1 and every plan is what it was, bit for bit.
 *
 * **Scope.** The preferences are process-wide (setPreferences): the plugin sets them from its data folder, phos_render
 * from `--preferences`. Changing them moves a revision number that Composer::validate() watches, so the plans are
 * made again.
 * @author Rene Weller
 */
#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace phos
{

struct RatingEntry;
struct TrackPlan;

/** @brief The learned weights: one per "name=value" feature. */
class Preferences
{
public:
    /**
     * @brief Reads `name=value weight` lines ('#' starts a comment); false on a malformed line.
     * @param[in]  text   the lines
     * @param[out] error  receives the failure, or null
     * @return false on a malformed line
     */
    bool parse( std::string_view text, std::string *error = nullptr );
    /**
     * @brief The lines parse() reads, sorted by feature.
     * @return the lines
     */
    std::string toText() const;
    /**
     * @brief exp(weight) of @p name = @p value, 1 when there is none.
     * @param[in] name   the feature's name
     * @param[in] value  its value
     * @return the factor
     */
    double factor( const std::string &name, const std::string &value ) const;
    /**
     * @brief The weight itself (0 when absent).
     * @param[in] feature  the feature
     * @return its weight
     */
    double weight( const std::string &feature ) const;
    /**
     * @brief Whether no weight is set.
     * @return true when no weight is set
     */
    bool empty() const { return m_w.empty(); }
    /**
     * @brief How many weights are set.
     * @return the number of weights
     */
    size_t size() const { return m_w.size(); }
    /**
     * @brief Sets one weight (the fitter and the tests).
     * @param[in] feature  the feature
     * @param[in] weight   its weight
     */
    void set( const std::string &feature, double weight ) { m_w[feature] = weight; }

private:
    std::map<std::string, double> m_w;   ///< "name=value" -> weight
};

/**
 * @brief Fits the weights from verdicts that carry features: per feature value, (good, bad) counts, the smoothed
 *        log-odds ln((good + 1) / (bad + 1)) times n / (n + 4); values rated fewer than twice are left out.
 * @param[in] ratings  the verdicts
 * @return the weights
 */
Preferences fitPreferences( const std::vector<RatingEntry> &ratings );

/**
 * @brief The composer's decisions at bar @p barInTrack of a track, as `name=value` pairs joined by ';'.
 * @param[in] plan        the track's plan
 * @param[in] barInTrack  the bar within the track
 * @return the name=value pairs
 */
std::string decisionFeatures( const TrackPlan &plan, int barInTrack );

/**
 * @brief Sets the process-wide preferences (nullptr: none); moves preferencesRevision().
 * @param[in] prefs  the preferences, or null
 */
void setPreferences( std::shared_ptr<const Preferences> prefs );
/**
 * @brief The process-wide preferences, or nullptr.
 * @return the preferences, or null
 */
std::shared_ptr<const Preferences> preferences();
/**
 * @brief Moves every time setPreferences() is called (Composer::validate() watches it).
 * @return the revision
 */
unsigned preferencesRevision();
/**
 * @brief preferences()->factor(name, value), or 1 without preferences.
 * @param[in] name   the feature's name
 * @param[in] value  its value
 * @return the factor
 */
double preferenceFactor( const char *name, const std::string &value );

/** @name The feature values' names at the draw sites (so the features and the factors spell them alike)
 *  @{ */
extern const char *const FormTemplateFeatureNames[4];   ///< "Full-On", "Progressive", "Goa", "Dark Forest"
extern const char *const ArpStyleFeatureNames[6];       ///< "corpus", "up", "down", "up-down", "Euclid", "polymeter"
extern const char *const CounterModeFeatureNames[4];    ///< "Echo", "Answer", "Timbral", "Hocket"
/** @} */

}   // namespace phos
