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
 */
#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace phos {

struct RatingEntry;
struct TrackPlan;

/** @brief The learned weights: one per "name=value" feature. */
class Preferences {
public:
    /** @brief Reads `name=value weight` lines ('#' starts a comment); false on a malformed line. */
    bool parse(std::string_view text, std::string* error = nullptr);
    /** @brief The lines parse() reads, sorted by feature. */
    std::string toText() const;
    /** @brief exp(weight) of @p name = @p value, 1 when there is none. */
    double factor(const std::string& name, const std::string& value) const;
    /** @brief The weight itself (0 when absent). */
    double weight(const std::string& feature) const;
    /** @brief Whether no weight is set. */
    bool empty() const { return w_.empty(); }
    /** @brief How many weights are set. */
    size_t size() const { return w_.size(); }
    /** @brief Sets one weight (the fitter and the tests). */
    void set(const std::string& feature, double weight) { w_[feature] = weight; }

private:
    std::map<std::string, double> w_;   ///< "name=value" -> weight
};

/**
 * @brief Fits the weights from verdicts that carry features: per feature value, (good, bad) counts, the smoothed
 *        log-odds ln((good + 1) / (bad + 1)) times n / (n + 4); values rated fewer than twice are left out.
 */
Preferences fitPreferences(const std::vector<RatingEntry>& ratings);

/** @brief The composer's decisions at bar @p barInTrack of a track, as `name=value` pairs joined by ';'. */
std::string decisionFeatures(const TrackPlan& plan, int barInTrack);

/** @brief Sets the process-wide preferences (nullptr: none); moves preferencesRevision(). */
void setPreferences(std::shared_ptr<const Preferences> prefs);
/** @brief The process-wide preferences, or nullptr. */
std::shared_ptr<const Preferences> preferences();
/** @brief Moves every time setPreferences() is called (Composer::validate() watches it). */
unsigned preferencesRevision();
/** @brief preferences()->factor(name, value), or 1 without preferences. */
double preferenceFactor(const char* name, const std::string& value);

/** @name The feature values' names at the draw sites (so the features and the factors spell them alike)
 *  @{ */
extern const char* const kFormTemplateFeatureNames[4];   ///< "Full-On", "Progressive", "Goa", "Dark Forest"
extern const char* const kArpStyleFeatureNames[6];       ///< "corpus", "up", "down", "up-down", "Euclid", "polymeter"
extern const char* const kCounterModeFeatureNames[4];    ///< "Echo", "Answer", "Timbral", "Hocket"
/** @} */

} // namespace phos
