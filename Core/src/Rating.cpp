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
    return "time\tseed\ttrack\tbar\tbar_in_track\tsection\tstyle\tverdict\tnote\tsource";
}

std::string formatRating(const RatingEntry& e)
{
    char nums[160];
    std::snprintf(nums, sizeof(nums), "%llu\t%d\t%d\t%d", static_cast<unsigned long long>(e.seed), e.track, e.bar, e.barInTrack);
    const char* verdict = e.verdict > 0 ? "good" : (e.verdict < 0 ? "bad" : "note");
    return clean(e.time) + "\t" + nums + "\t" + clean(e.section) + "\t" + clean(e.style) + "\t" + verdict + "\t" + clean(e.note) + "\t" + clean(e.source);
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
    if (!f[9].empty() && f[9].back() == '\r') f[9].pop_back();
    RatingEntry e;
    e.time = f[0];
    e.seed = std::strtoull(f[1].c_str(), nullptr, 10);
    e.track = std::atoi(f[2].c_str());
    e.bar = std::atoi(f[3].c_str());
    e.barInTrack = std::atoi(f[4].c_str());
    e.section = f[5];
    e.style = f[6];
    e.verdict = f[7] == "good" ? 1 : (f[7] == "bad" ? -1 : 0);
    e.note = f[8];
    e.source = f[9];
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
