/**
 * @file Preferences.cpp
 * @brief Learned preferences (Preferences.h).
 */
#include "phos/Preferences.h"
#include "phos/Composer.h"
#include "phos/Form.h"
#include "phos/Melody.h"
#include "phos/Params.h"
#include "phos/Rating.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>

namespace phos {

const char* const kFormTemplateFeatureNames[4] = { "Full-On", "Progressive", "Goa", "Dark Forest" };
const char* const kArpStyleFeatureNames[6] = { "corpus", "up", "down", "up-down", "Euclid", "polymeter" };
const char* const kCounterModeFeatureNames[4] = { "Echo", "Answer", "Timbral", "Hocket" };

namespace {
std::mutex gLock;
std::shared_ptr<const Preferences> gPrefs;
std::atomic<unsigned> gRevision{ 0 };
} // namespace

bool Preferences::parse(std::string_view text, std::string* error)
{
    w_.clear();
    std::istringstream in{ std::string(text) };
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        // "name=value weight": the weight is the last field, everything before it the feature (values may hold spaces).
        const size_t end = line.find_last_not_of(" \t");
        const size_t space = line.find_last_of(" \t", end);
        if (space == std::string::npos || line.find('=') == std::string::npos) {
            if (error != nullptr) *error = "line " + std::to_string(n) + ": want 'name=value weight'";
            return false;
        }
        std::string feature = line.substr(0, space);
        feature.erase(feature.find_last_not_of(" \t") + 1);
        feature.erase(0, feature.find_first_not_of(" \t"));
        char* stop = nullptr;
        const std::string num = line.substr(space + 1, end - space);
        const double w = std::strtod(num.c_str(), &stop);
        if (stop == num.c_str() || !std::isfinite(w)) {
            if (error != nullptr) *error = "line " + std::to_string(n) + ": the weight is not a number";
            return false;
        }
        w_[feature] = std::clamp(w, -3.0, 3.0);
    }
    return true;
}

std::string Preferences::toText() const
{
    std::string s = "# Phosphene preferences: one 'name=value weight' per line, the draw's weight times exp(weight).\n";
    for (const auto& kv : w_) {
        char num[32];
        std::snprintf(num, sizeof(num), " %.3f\n", kv.second);
        s += kv.first + num;
    }
    return s;
}

double Preferences::weight(const std::string& feature) const
{
    const auto it = w_.find(feature);
    return it == w_.end() ? 0.0 : it->second;
}

double Preferences::factor(const std::string& name, const std::string& value) const
{
    const auto it = w_.find(name + "=" + value);
    return it == w_.end() ? 1.0 : std::exp(it->second);
}

Preferences fitPreferences(const std::vector<RatingEntry>& ratings)
{
    std::map<std::string, std::pair<int, int>> counts;   // feature -> good, bad
    for (const RatingEntry& r : ratings) {
        if (r.verdict == 0 || r.features.empty()) continue;
        std::istringstream in(r.features);
        std::string f;
        while (std::getline(in, f, ';')) {
            if (f.find('=') == std::string::npos) continue;
            auto& c = counts[f];
            (r.verdict > 0 ? c.first : c.second) += 1;
        }
    }
    Preferences p;
    for (const auto& kv : counts) {
        const int g = kv.second.first, b = kv.second.second, n = g + b;
        if (n < 2) continue;
        const double w = std::log((g + 1.0) / (b + 1.0)) * static_cast<double>(n) / (n + 4.0);
        if (std::fabs(w) >= 0.02) p.set(kv.first, w);
    }
    return p;
}

std::string decisionFeatures(const TrackPlan& t, int barInTrack)
{
    std::string s;
    auto add = [&](const std::string& name, const std::string& value) { if (!s.empty()) s += ';'; s += name + "=" + value; };
    add("style", kStyleNames[std::clamp(t.style, 0, kNumStyles - 1)]);
    if (t.form.count > 0) {
        const Section& sec = t.form.section[sectionOfBar(t.form, std::clamp(barInTrack, 0, std::max(0, t.bars - 1)))];
        add("section", kSectionNames[static_cast<int>(sec.type)]);
        add("climax", sec.climax ? "1" : "0");
    }
    add("form.template", kFormTemplateFeatureNames[std::clamp(t.form.body, 0, 3)]);
    const MelodyPlan& m = t.melody;
    if (m.present[mpIndex(MelodyPart::Lead)]) {
        add("lead.archetype", kLeadArchetypeNames[std::clamp(m.leadArchetype[0], 0, kNumLeadArchetypes - 1)]);
        if (m.leadArchetype[1] != m.leadArchetype[0]) add("lead.archetype", kLeadArchetypeNames[std::clamp(m.leadArchetype[1], 0, kNumLeadArchetypes - 1)]);
        add("lead.band", std::to_string(m.leadDensityBand));
    }
    if (m.present[mpIndex(MelodyPart::Counter)]) add("counter.mode", kCounterModeFeatureNames[std::clamp(m.counterMode, 0, 3)]);
    if (m.present[mpIndex(MelodyPart::Arp)]) add("arp.style", kArpStyleFeatureNames[std::clamp(m.arpStyle, 0, 5)]);
    add("harmony", m.progression == 1 ? "loop" : "pendulum");
    return s;
}

void setPreferences(std::shared_ptr<const Preferences> prefs)
{
    {
        const std::lock_guard<std::mutex> lock(gLock);
        gPrefs = std::move(prefs);
    }
    gRevision.fetch_add(1, std::memory_order_acq_rel);
}

std::shared_ptr<const Preferences> preferences()
{
    const std::lock_guard<std::mutex> lock(gLock);
    return gPrefs;
}

unsigned preferencesRevision() { return gRevision.load(std::memory_order_acquire); }

double preferenceFactor(const char* name, const std::string& value)
{
    const std::shared_ptr<const Preferences> p = preferences();
    return p != nullptr ? p->factor(name, value) : 1.0;
}

} // namespace phos
