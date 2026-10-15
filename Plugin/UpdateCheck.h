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
 * @author Rene Weller
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
        bool m_asked = false;        ///< a check has run (now or from the cache)
        bool m_ok = false;           ///< the answer arrived and was understood
        bool m_newer = false;        ///< the latest release is newer than this build
        juce::String m_latest;       ///< the latest release's tag ("v1.1.0")
        juce::String m_url;          ///< its page
        juce::String m_message;      ///< one line for the help page: what happened
        juce::Time m_when;           ///< when GitHub was last asked
    };

    /** @brief Creates the (not yet running) check thread; nothing is fetched until startIfDue(). */
    UpdateCheck();
    ~UpdateCheck() override;

    /**
     * @brief Asks GitHub unless it was asked less than a day ago (@p force asks anyway) or the check is off.
     * @param[in] stateFile  `update.txt` in the user folder
     * @param[in] force      check now, even if the last check was less than a day ago
     */
    void startIfDue(const juce::File& stateFile, bool force = false);
    /**
     * @brief The last result (thread-safe).
     * @return the result
     */
    Result result() const;
    /**
     * @brief Whether the automatic check is on (the file's `enabled` line; on when there is none).
     * @param[in] stateFile  the state file
     * @return true when the check is on
     */
    static bool enabled(const juce::File& stateFile);
    /**
     * @brief Switches the automatic check on or off.
     * @param[in] stateFile  the state file
     * @param[in] on         true to switch it on
     */
    static void setEnabled(const juce::File& stateFile, bool on);
    /**
     * @brief Whether @p candidate ("v1.2.0", "1.10") is a later version than @p current, number by number.
     * @param[in] candidate  the candidate version
     * @param[in] current    the current version
     * @return true when the candidate is later
     */
    static bool isNewer(const juce::String& candidate, const juce::String& current);
    /** @brief The address asked. */
    static constexpr const char* LatestReleaseApi = "https://api.github.com/repos/reneweller-coding/Phosphene/releases/latest";

private:
    /** @brief The thread: asks GitHub for the latest release and keeps what it says. */
    void run() override;
    /**
     * @brief Reads the cached answer out of the state file.
     * @param[in] stateFile  the state file
     */
    void loadCache(const juce::File& stateFile);
    mutable std::mutex m_lock;   ///< guards m_result
    Result m_result;   ///< what the last check found
    juce::File m_stateFile;   ///< update.txt in the user folder
};

} // namespace phosui
