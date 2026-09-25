/**
 * @file UpdateCheck.h
 * @brief Is there a newer Phosphene? One question to GitHub, at most once a day (23.09.2026).
 *
 * The check asks the GitHub API for the project's latest release and compares its tag with this build's
 * version. Nothing is sent but the request itself, nothing is downloaded or installed: a newer release is
 * shown in the editor's header, and a click opens its page. The answer and the time of the last question
 * are kept in `update.txt` in the user folder (PhospheneProcessor::userFolder), together with the switch
 * that turns the check off, so a second window, a second instance in a DAW or the next start within a day
 * asks nobody.
 *
 * It never runs in a test: PHOS_NO_UPDATE_CHECK=1 switches it off for the process (the host test, the VST3
 * test and the screenshot mode set it), and a test never talks to the network.
 *
 * While the repository is private the API answers 404 for anybody without a token, and the check reports
 * "no public release" -- which is the truth, and turns into a real answer the day the project is public.
 */
#pragma once
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <mutex>

namespace phosui {

/** @brief The update check: one per process (juce::SharedResourcePointer), its own thread for the request. */
class UpdateCheck : private juce::Thread {
public:
    /** @brief What the last check found. */
    struct Result {
        bool asked = false;        ///< a check has run (now or from the cache)
        bool ok = false;           ///< the answer arrived and was understood
        bool newer = false;        ///< the latest release is newer than this build
        juce::String latest;       ///< the latest release's tag ("v1.1.0")
        juce::String url;          ///< its page
        juce::String message;      ///< one line for the help page: what happened
        juce::Time when;           ///< when GitHub was last asked
    };

    /** @brief Creates the (not yet running) check thread; nothing is fetched until startIfDue(). */
    UpdateCheck();
    ~UpdateCheck() override;

    /**
     * @brief Asks GitHub unless it was asked less than a day ago (@p force asks anyway) or the check is off.
     * @param stateFile `update.txt` in the user folder
     * @param force check now, even if the last check was less than a day ago
     */
    void startIfDue(const juce::File& stateFile, bool force = false);
    /** @brief The last result (thread-safe). */
    Result result() const;
    /** @brief Whether the automatic check is on (the file's `enabled` line; on when there is none). */
    static bool enabled(const juce::File& stateFile);
    /** @brief Switches the automatic check on or off. */
    static void setEnabled(const juce::File& stateFile, bool on);
    /** @brief Whether @p candidate ("v1.2.0", "1.10") is a later version than @p current, number by number. */
    static bool isNewer(const juce::String& candidate, const juce::String& current);
    /** @brief The address asked. */
    static constexpr const char* kLatestReleaseApi = "https://api.github.com/repos/reneweller-coding/Phosphene/releases/latest";

private:
    void run() override;
    /** @brief Reads the cached answer out of the state file. */
    void loadCache(const juce::File& stateFile);
    mutable std::mutex lock_;   ///< guards result_
    Result result_;   ///< what the last check found
    juce::File stateFile_;   ///< update.txt in the user folder
};

} // namespace phosui
