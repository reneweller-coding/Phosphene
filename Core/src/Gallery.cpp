/**
 * @file Gallery.cpp
 * @brief The set gallery's comment lines (Gallery.h).
 */
#include "phos/Gallery.h"
#include "phos/Composer.h"
#include "phos/Form.h"
#include "phos/Params.h"
#include "phos/Rating.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace phos {

namespace {

/** @brief The letter of a section in the form string. */
char sectionLetter(const Section& s)
{
    switch (s.type) {
    case SectionType::Intro: return 'I';
    case SectionType::Groove: return 'G';
    case SectionType::Build: return 'B';
    case SectionType::Drop: return s.climax ? 'C' : 'D';
    case SectionType::Break: return 'K';
    case SectionType::Outro: return 'O';
    default: return 'X';
    }
}

/** @brief Tabs and line breaks out of a field, and the separator of the track line. */
std::string clean(const std::string& s)
{
    std::string out = s;
    for (char& c : out) if (c == '\n' || c == '\r' || c == '|') c = ' ';
    return out;
}

} // namespace

GalleryTrack galleryTrackOf(const TrackPlan& p)
{
    GalleryTrack t;
    t.style = kStyleNames[std::clamp(p.style, 0, kNumStyles - 1)];
    t.key = kKeyNames[((p.key % 12) + 12) % 12];
    t.scale = kScaleNames[std::clamp(p.scale, 0, kNumScales - 1)];
    t.bpm = p.bpm;
    t.bars = p.bars;
    for (int s = 0; s < p.form.count; ++s) {
        if (!t.form.empty()) t.form += ' ';
        t.form += sectionLetter(p.form.section[s]);
        t.form += std::to_string(p.form.section[s].bars);
    }
    return t;
}

std::string galleryComment(const GalleryEntry& e)
{
    std::string s = "# gallery.name=" + clean(e.name) + "\n# gallery.saved=" + clean(e.saved) + "\n";
    for (const GalleryTrack& t : e.tracks) {
        char bpm[32];
        std::snprintf(bpm, sizeof(bpm), "%.1f", t.bpm);
        s += "# gallery.track=" + clean(t.style) + "|" + clean(t.key) + "|" + clean(t.scale) + "|" + bpm + "|" + std::to_string(t.bars) + "|" + clean(t.form) + "\n";
    }
    return s;
}

std::string withGalleryComment(const std::string& setText, const GalleryEntry& e)
{
    const size_t nl = setText.find('\n');
    if (nl == std::string::npos) return setText + "\n" + galleryComment(e);
    return setText.substr(0, nl + 1) + galleryComment(e) + setText.substr(nl + 1);
}

bool readGalleryEntry(std::string_view text, GalleryEntry& out)
{
    GalleryEntry e;
    bool haveSeed = false;
    std::istringstream in{ std::string(text) };
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("seed=", 0) == 0) { e.seed = std::strtoull(line.c_str() + 5, nullptr, 10); haveSeed = true; continue; }
        if (line.rfind("# gallery.name=", 0) == 0) { e.name = line.substr(15); continue; }
        if (line.rfind("# gallery.saved=", 0) == 0) { e.saved = line.substr(16); continue; }
        if (line.rfind("# gallery.track=", 0) == 0) {
            std::vector<std::string> f;
            std::string cell;
            std::istringstream cells(line.substr(16));
            while (std::getline(cells, cell, '|')) f.push_back(cell);
            if (f.size() < 6) continue;
            GalleryTrack t;
            t.style = f[0];
            t.key = f[1];
            t.scale = f[2];
            t.bpm = std::atof(f[3].c_str());
            t.bars = std::atoi(f[4].c_str());
            t.form = f[5];
            e.tracks.push_back(t);
        }
    }
    if (!haveSeed) return false;
    out = e;
    return true;
}

std::map<uint64_t, RatingCount> ratingsBySeed(const char* path)
{
    std::map<uint64_t, RatingCount> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        RatingEntry r;
        if (!parseRating(line, r)) continue;
        RatingCount& c = out[r.seed];
        if (r.verdict > 0) ++c.good;
        else if (r.verdict < 0) ++c.bad;
        else ++c.notes;
    }
    return out;
}

} // namespace phos
