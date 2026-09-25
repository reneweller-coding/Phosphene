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
    const std::lock_guard<std::mutex> g(lock_);
    result_.when = juce::Time(kv["last"].getLargeIntValue());
    result_.latest = kv["latest"];
    result_.url = kv["url"];
    result_.ok = result_.latest.isNotEmpty();
    result_.asked = kv["last"].isNotEmpty();
    result_.newer = result_.ok && isNewer(result_.latest, JucePlugin_VersionString);
    result_.message = kv["message"];
}

void UpdateCheck::startIfDue(const juce::File& stateFile, bool force)
{
    if (switchedOffForProcess()) {
        const std::lock_guard<std::mutex> g(lock_);
        result_.message = "The update check is switched off in this process (PHOS_NO_UPDATE_CHECK).";
        return;
    }
    if (isThreadRunning()) return;
    stateFile_ = stateFile;
    loadCache(stateFile);
    if (!force) {
        if (!enabled(stateFile)) return;
        const std::lock_guard<std::mutex> g(lock_);
        if (result_.asked && juce::Time::getCurrentTime() - result_.when < juce::RelativeTime::hours(24)) return;
    }
    startThread(juce::Thread::Priority::background);
}

UpdateCheck::Result UpdateCheck::result() const
{
    const std::lock_guard<std::mutex> g(lock_);
    return result_;
}

void UpdateCheck::run()
{
    Result r;
    r.asked = true;
    r.when = juce::Time::getCurrentTime();
    int status = 0;
    const juce::URL url(kLatestReleaseApi);
    auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                       .withConnectionTimeoutMs(8000)
                       .withExtraHeaders(juce::String("User-Agent: Phosphene/") + JucePlugin_VersionString + "\r\nAccept: application/vnd.github+json")
                       .withStatusCode(&status);
    std::unique_ptr<juce::InputStream> in = url.createInputStream(options);
    if (threadShouldExit()) return;
    if (in == nullptr) {
        r.message = "GitHub could not be reached.";
    } else if (status == 404) {
        r.message = "GitHub knows no public release of Phosphene yet.";
    } else if (status != 200) {
        r.message = "GitHub answered " + juce::String(status) + ".";
    } else {
        const juce::var json = juce::JSON::parse(in->readEntireStreamAsString());
        r.latest = json.getProperty("tag_name", juce::var()).toString();
        r.url = json.getProperty("html_url", juce::var()).toString();
        r.ok = r.latest.isNotEmpty();
        r.newer = r.ok && isNewer(r.latest, JucePlugin_VersionString);
        r.message = !r.ok ? juce::String("GitHub's answer named no release.")
                  : r.newer ? "Phosphene " + r.latest.trimCharactersAtStart("vV") + " is available."
                            : "This is the latest version.";
    }
    {
        const std::lock_guard<std::mutex> g(lock_);
        result_ = r;
    }
    juce::StringPairArray kv = readState(stateFile_);
    kv.set("last", juce::String(r.when.toMilliseconds()));
    kv.set("latest", r.latest);
    kv.set("url", r.url);
    kv.set("message", r.message);
    writeState(stateFile_, kv);
}

} // namespace phosui
