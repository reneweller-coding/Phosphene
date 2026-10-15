/**
 * @file Rating.cpp
 * @brief The ratings file (Rating.h).
 */
#include "phos/Rating.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace phos {

namespace {

/** @brief A field as the file holds it: tabs and line breaks would break the columns, so they become spaces. */
std::string clean(const std::string& s)
{
    std::string out = s;
    for (char& c : out) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return out;
}

} // namespace

const char* ratingHeader()
{
    return "time\tseed\ttrack\tbar\tbar_in_track\tsection\tstyle\tverdict\tnote\tsource\tfeatures";
}

std::string formatRating(const RatingEntry& e)
{
    char nums[160];
    std::snprintf(nums, sizeof(nums), "%llu\t%d\t%d\t%d", static_cast<unsigned long long>(e.m_seed), e.m_track, e.m_bar, e.m_barInTrack);
    const char* verdict = e.m_verdict > 0 ? "good" : (e.m_verdict < 0 ? "bad" : "note");
    return clean(e.m_time) + "\t" + nums + "\t" + clean(e.m_section) + "\t" + clean(e.m_style) + "\t" + verdict + "\t" + clean(e.m_note) + "\t" + clean(e.m_source) + "\t" + clean(e.m_features);
}

bool parseRating(const std::string& line, RatingEntry& out)
{
    std::vector<std::string> f;
    size_t start = 0;
    for (;;) {
        const size_t tab = line.find('\t', start);
        f.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) break;
        start = tab + 1;
    }
    if (f.size() < 10 || f[1] == "seed") return false;
    if (!f.back().empty() && f.back().back() == '\r') f.back().pop_back();
    RatingEntry e;
    e.m_time = f[0];
    e.m_seed = std::strtoull(f[1].c_str(), nullptr, 10);
    e.m_track = std::atoi(f[2].c_str());
    e.m_bar = std::atoi(f[3].c_str());
    e.m_barInTrack = std::atoi(f[4].c_str());
    e.m_section = f[5];
    e.m_style = f[6];
    e.m_verdict = f[7] == "good" ? 1 : (f[7] == "bad" ? -1 : 0);
    e.m_note = f[8];
    e.m_source = f[9];
    if (f.size() > 10) e.m_features = f[10];   // since 23.09.2026; older files have ten columns
    out = e;
    return true;
}

bool appendRating(const char* path, const RatingEntry& e)
{
    bool isNew = true;
    if (FILE* probe = std::fopen(path, "rb")) {
        std::fseek(probe, 0, SEEK_END);
        isNew = std::ftell(probe) <= 0;
        std::fclose(probe);
    }
    FILE* f = std::fopen(path, "ab");
    if (f == nullptr) return false;
    if (isNew) std::fprintf(f, "%s\n", ratingHeader());
    std::fprintf(f, "%s\n", formatRating(e).c_str());
    std::fclose(f);
    return true;
}

} // namespace phos
