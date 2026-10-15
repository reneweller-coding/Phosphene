/**
 * @file UpdateCheck.cpp
 * @brief The update check (UpdateCheck.h).
 */
#include "UpdateCheck.h"

namespace phosui {

namespace {
/** @brief The `key=value` lines of the state file. */
juce::StringPairArray readState(const juce::File& f)
{
    juce::StringPairArray kv;
    juce::StringArray lines;
    f.readLines(lines);
    for (const juce::String& l : lines)
        if (l.containsChar('=')) kv.set(l.upToFirstOccurrenceOf("=", false, false).trim(), l.fromFirstOccurrenceOf("=", false, false).trim());
    return kv;
}

/** @brief Writes the check's state as key=value lines. */
void writeState(const juce::File& f, const juce::StringPairArray& kv)
{
    juce::String s;
    for (int i = 0; i < kv.size(); ++i) s << kv.getAllKeys()[i] << "=" << kv.getAllValues()[i] << "\n";
    f.getParentDirectory().createDirectory();
    f.replaceWithText(s, false, false, "\n");
}

/** @brief Whether PHOS_NO_UPDATE_CHECK switches the check off for this process (the tests). */
bool switchedOffForProcess()
{
    return juce::SystemStats::getEnvironmentVariable("PHOS_NO_UPDATE_CHECK", "").isNotEmpty();
}
} // namespace

UpdateCheck::UpdateCheck() : juce::Thread("Phosphene update check") {}

UpdateCheck::~UpdateCheck() { stopThread(4000); }

bool UpdateCheck::isNewer(const juce::String& candidate, const juce::String& current)
{
    auto numbers = [](juce::String v) {
        v = v.trim().trimCharactersAtStart("vV");
        juce::StringArray parts = juce::StringArray::fromTokens(v.upToFirstOccurrenceOf("-", false, false), ".", "");
        juce::Array<int> n;
        for (const juce::String& p : parts) n.add(p.getIntValue());
        while (n.size() < 3) n.add(0);
        return n;
    };
    const juce::Array<int> a = numbers(candidate), b = numbers(current);
    for (int i = 0; i < juce::jmax(a.size(), b.size()); ++i) {
        const int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

bool UpdateCheck::enabled(const juce::File& stateFile)
{
    const juce::StringPairArray kv = readState(stateFile);
    return kv["enabled"] != "0";
}

void UpdateCheck::setEnabled(const juce::File& stateFile, bool on)
{
    juce::StringPairArray kv = readState(stateFile);
    kv.set("enabled", on ? "1" : "0");
    writeState(stateFile, kv);
}

void UpdateCheck::loadCache(const juce::File& stateFile)
{
    const juce::StringPairArray kv = readState(stateFile);
    const std::lock_guard<std::mutex> g(m_lock);
    m_result.m_when = juce::Time(kv["last"].getLargeIntValue());
    m_result.m_latest = kv["latest"];
    m_result.m_url = kv["url"];
    m_result.m_ok = m_result.m_latest.isNotEmpty();
    m_result.m_asked = kv["last"].isNotEmpty();
    m_result.m_newer = m_result.m_ok && isNewer(m_result.m_latest, JucePlugin_VersionString);
    m_result.m_message = kv["message"];
}

void UpdateCheck::startIfDue(const juce::File& stateFile, bool force)
{
    if (switchedOffForProcess()) {
        const std::lock_guard<std::mutex> g(m_lock);
        m_result.m_message = "The update check is switched off in this process (PHOS_NO_UPDATE_CHECK).";
        return;
    }
    if (isThreadRunning()) return;
    m_stateFile = stateFile;
    loadCache(stateFile);
    if (!force) {
        if (!enabled(stateFile)) return;
        const std::lock_guard<std::mutex> g(m_lock);
        if (m_result.m_asked && juce::Time::getCurrentTime() - m_result.m_when < juce::RelativeTime::hours(24)) return;
    }
    startThread(juce::Thread::Priority::background);
}

UpdateCheck::Result UpdateCheck::result() const
{
    const std::lock_guard<std::mutex> g(m_lock);
    return m_result;
}

void UpdateCheck::run()
{
    Result r;
    r.m_asked = true;
    r.m_when = juce::Time::getCurrentTime();
    int status = 0;
    const juce::URL url(LatestReleaseApi);
    auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                       .withConnectionTimeoutMs(8000)
                       .withExtraHeaders(juce::String("User-Agent: Phosphene/") + JucePlugin_VersionString + "\r\nAccept: application/vnd.github+json")
                       .withStatusCode(&status);
    std::unique_ptr<juce::InputStream> in = url.createInputStream(options);
    if (threadShouldExit()) return;
    if (in == nullptr) {
        r.m_message = "GitHub could not be reached.";
    } else if (status == 404) {
        r.m_message = "GitHub knows no public release of Phosphene yet.";
    } else if (status != 200) {
        r.m_message = "GitHub answered " + juce::String(status) + ".";
    } else {
        const juce::var json = juce::JSON::parse(in->readEntireStreamAsString());
        r.m_latest = json.getProperty("tag_name", juce::var()).toString();
        r.m_url = json.getProperty("html_url", juce::var()).toString();
        r.m_ok = r.m_latest.isNotEmpty();
        r.m_newer = r.m_ok && isNewer(r.m_latest, JucePlugin_VersionString);
        r.m_message = !r.m_ok ? juce::String("GitHub's answer named no release.")
                  : r.m_newer ? "Phosphene " + r.m_latest.trimCharactersAtStart("vV") + " is available."
                            : "This is the latest version.";
    }
    {
        const std::lock_guard<std::mutex> g(m_lock);
        m_result = r;
    }
    juce::StringPairArray kv = readState(m_stateFile);
    kv.set("last", juce::String(r.m_when.toMilliseconds()));
    kv.set("latest", r.m_latest);
    kv.set("url", r.m_url);
    kv.set("message", r.m_message);
    writeState(m_stateFile, kv);
}

} // namespace phosui
