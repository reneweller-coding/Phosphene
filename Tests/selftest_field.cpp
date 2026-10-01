/**
 * @file selftest_field.cpp
 * @brief The self test's checks of the Field track (27.09.2026; FieldLibrary.h, FieldPlayer.h, FieldPresets.h): the
 *        crossfade loop keeps its level over the seam forwards and backwards, a recording without a loop ends, the
 *        engine routes a Field note to the Field strip alone and a Space Shot to the effects, the form puts the places
 *        where the floor leaves room, and every preset names only knobs that exist.
 *
 * The recordings here are made in memory (addFieldClip), so the checks run on a machine without the field folder.
 */
#include "SelfTestHelpers.h"
#include "phos/FieldLibrary.h"
#include "phos/FieldPlayer.h"
#include "phos/FieldPresets.h"
#include "phos/Form.h"
#include "phos/Composer.h"
#include <algorithm>
#include <cmath>

using namespace phos;

namespace phostest {

namespace {

constexpr double kSr = 48000.0;   ///< the sample rate, Hz

/** @brief A second of stereo white noise at 44.1 kHz whose end does NOT meet its start: only the crossfade joins it. */
void addNoiseClip(int category, const char* name)
{
    Rng r;
    r.seed(0x4E4F495345ull);
    std::vector<float> L(44100), R(44100);
    for (size_t i = 0; i < L.size(); ++i) { L[i] = 0.3f * r.bipolar(); R[i] = 0.3f * r.bipolar(); }
    addFieldClip(category, name, std::move(L), std::move(R), 44100);
    scanFieldLibrary();
}

/** @brief The Field module's defaults with @p changes (`key=value`, keys without "field."). */
std::vector<float> fieldValues(const ParamStore& p, std::initializer_list<std::pair<const char*, float>> changes)
{
    const int b = p.base(Module::Field);
    std::vector<float> v(static_cast<size_t>(field::Count));
    for (int i = 0; i < field::Count; ++i) v[static_cast<size_t>(i)] = p.defaultValue(b + i);
    for (const auto& c : changes) v[static_cast<size_t>(p.find(std::string("field.") + c.first) - b)] = c.second;
    return v;
}

/** @brief The largest and smallest 10 ms RMS (dB) of @p y from sample @p from on. */
void rmsRange(const std::vector<float>& y, size_t from, double& lo, double& hi)
{
    lo = 1e9;
    hi = -1e9;
    const size_t w = 480;
    for (size_t a = from; a + w <= y.size(); a += w) {
        double s = 0.0;
        for (size_t i = a; i < a + w; ++i) s += static_cast<double>(y[i]) * y[i];
        const double db = 10.0 * std::log10(s / static_cast<double>(w) + 1e-30);
        lo = std::min(lo, db);
        hi = std::max(hi, db);
    }
}

} // namespace

void testFieldLoop()
{
    section("field track: the crossfade loop keeps its level over the seam, forwards and backwards; no loop, the end");
    addNoiseClip(0, "zz-test-noise");
    // The shipped library may be there too: the test's recording by its name, not by the first variation.
    const float var = static_cast<float>(fieldVariationOf(0, "zz-test-noise"));
    ParamStore p;
    for (const bool reverse : { false, true }) {
        // Four seconds of a one-second recording: three seams, each a 200 ms equal-power crossfade.
        FieldPlayer fp;
        fp.prepare(kSr);
        const std::vector<float> v = fieldValues(p, { { "a_category", 1.0f }, { "a_variation", var }, { "a_start_random", 0.0f },
                                                      { "a_reverse", reverse ? 1.0f : 0.0f }, { "loop_xfade", 200.0f },
                                                      { "amp_attack", 1.0f }, { "low_cut", 20.0f } });
        fp.update(v.data());
        fp.trigger(static_cast<int>(5.0 * kSr), 1.0f, 0.0, 0, 0.0, 7);
        std::vector<float> L(static_cast<size_t>(4.0 * kSr)), R(L.size());
        for (size_t done = 0; done < L.size(); done += 512)
            fp.process(L.data() + done, R.data() + done, static_cast<int>(std::min<size_t>(512, L.size() - done)), 0.0, 0.0);
        double lo, hi;
        rmsRange(L, static_cast<size_t>(0.05 * kSr), lo, hi);
        // Uncorrelated noise under an equal-power fade keeps its power; 10 ms windows of noise scatter about 1 dB.
        check(hi - lo < 2.0 && lo > -40.0, reverse ? "the reversed loop holds its level over every seam" : "the loop holds its level over every seam",
              fmt("10 ms RMS from %.1f to %.1f dB", lo, hi));
    }
    {
        FieldPlayer fp;
        fp.prepare(kSr);
        const std::vector<float> v = fieldValues(p, { { "a_category", 1.0f }, { "a_variation", var }, { "a_start_random", 0.0f }, { "loop", 0.0f },
                                                      { "amp_attack", 1.0f } });
        fp.update(v.data());
        fp.trigger(static_cast<int>(5.0 * kSr), 1.0f, 0.0, 0, 0.0, 7);
        std::vector<float> L(static_cast<size_t>(1.5 * kSr)), R(L.size());
        for (size_t done = 0; done < L.size(); done += 512)
            fp.process(L.data() + done, R.data() + done, static_cast<int>(std::min<size_t>(512, L.size() - done)), 0.0, 0.0);
        double tail = 0.0;
        for (size_t i = static_cast<size_t>(1.1 * kSr); i < L.size(); ++i) tail = std::max(tail, static_cast<double>(std::fabs(L[i])));
        check(fp.active() == 0 && tail == 0.0, "without the loop a layer ends with its recording and frees its voice",
              fmt("%d voices, tail peak %.3g", fp.active(), tail));
    }
}

void testFieldEngine()
{
    section("field track: a Field note plays on the Field strip alone, a Space Shot on the effects' strip");
    addNoiseClip(0, "zz-test-noise");
    addNoiseClip(field::kCategories, "zz-test-shot");   // the hidden shots category: FieldLibrary.cpp, kShotCategory
    auto e = std::make_unique<Engine>();
    e->prepare(kSr, 512);
    e->params().parseText(("mix.kick_mute=1 mix.perc_mute=1 field.a_category=1 field.a_start_random=0 field.amp_attack=5 compose.bpm=145 "
                           "field.a_variation=" + std::to_string(fieldVariationOf(0, "zz-test-noise"))).c_str());
    NoteEvent n;
    n.part = Part::Field; n.beat = 0.0; n.length = 8.0f; n.pitch = 60; n.velocity = 100;
    e->pushEvent(n);
    NoteEvent s;
    s.part = Part::Sfx; s.beat = 2.0; s.length = 4.0f; s.pitch = static_cast<uint8_t>(kSfxBaseNote + static_cast<int>(SfxType::SpaceShot)); s.velocity = 110;
    s.lane = 3;
    e->pushEvent(s);
    const size_t len = static_cast<size_t>(2.0 * kSr);
    std::vector<float> L(len), R(len);
    std::vector<std::vector<float>> stemL(kNumStems, std::vector<float>(len)), stemR(kNumStems, std::vector<float>(len));
    StemTap tap;
    for (int k = 0; k < kNumStems; ++k) { tap.L[k] = stemL[static_cast<size_t>(k)].data(); tap.R[k] = stemR[static_cast<size_t>(k)].data(); }
    e->setStemTap(&tap);
    e->process(L.data(), R.data(), static_cast<int>(len));
    e->setStemTap(nullptr);
    auto energy = [&](int stem, size_t a, size_t b) {
        double sum = 0.0;
        for (size_t i = a; i < b; ++i) sum += static_cast<double>(stemL[static_cast<size_t>(stem)][i]) * stemL[static_cast<size_t>(stem)][i];
        return sum;
    };
    const size_t shotAt = static_cast<size_t>(2.0 * 60.0 / 145.0 * kSr);
    double others = 0.0;
    for (int k = 0; k < kNumParts; ++k)
        if (k != static_cast<int>(Part::Field) && k != static_cast<int>(Part::Sfx)) others += energy(k, 0, len);
    check(energy(static_cast<int>(Part::Field), 0, len) > 1.0 && others == 0.0, "the Field note sounds on the Field stem and nowhere else",
          fmt("field %.3g, other parts %.3g", energy(static_cast<int>(Part::Field), 0, len), others));
    check(energy(static_cast<int>(Part::Sfx), 0, shotAt) == 0.0 && energy(static_cast<int>(Part::Sfx), shotAt, len) > 1.0,
          "the Space Shot sounds on the effects' stem from its beat on",
          fmt("before %.3g, after %.3g", energy(static_cast<int>(Part::Sfx), 0, shotAt), energy(static_cast<int>(Part::Sfx), shotAt, len)));
}

void testFieldForm()
{
    section("field track: places under intros, breakdowns and outros, never in a build-up or a drop; shots away from voices");
    int places = 0, wrong = 0, shots = 0, nearVoice = 0, forestTracks = 0, forestWith = 0;
    for (int style = 0; style < kNumStyles; ++style) {
        for (uint64_t seed = 1; seed <= 12; ++seed) {
            FormPlan f = makeFormPlan(styleProfile(static_cast<StyleId>(style)), seed * 7919u, 224, 0.5, 0.6);
            makeFormSfx(f, seed, 0.6f);
            placeField(f, seed, style, 1.0f, 0.6f);
            for (const SfxEvent& e : f.field) {
                ++places;
                const int bar = static_cast<int>(e.beat / kBeatsPerBar);
                const int endBar = static_cast<int>((e.beat + e.length) / kBeatsPerBar) - 1;
                for (int b = bar; b <= endBar; ++b) {
                    const SectionType t = f.section[sectionOfBar(f, b)].type;
                    if (t == SectionType::Build || t == SectionType::Drop) { ++wrong; break; }
                }
            }
            for (const SfxEvent& e : f.sfx) {
                if (e.type != static_cast<int>(SfxType::SpaceShot)) continue;
                ++shots;
                for (const SfxEvent& v : f.sfx)
                    if (sfxTypePart(static_cast<SfxType>(std::clamp(v.type, 0, kNumSfxTypes - 1))) == Part::Vocal
                        && std::fabs(v.beat - e.beat) < 2.0 * kBeatsPerBar) ++nearVoice;
            }
            if (style == static_cast<int>(StyleId::DarkForest)) { ++forestTracks; forestWith += f.field.empty() ? 0 : 1; }
        }
    }
    check(places > 0 && wrong == 0, "no place reaches into a build-up or a drop", fmt("%d places, %d wrong", places, wrong));
    check(shots > 0 && nearVoice == 0, "the NASA shots keep two bars from every voice", fmt("%d shots, %d near a voice", shots, nearVoice));
    check(forestWith == forestTracks, "every Dark Forest track has its place", fmt("%d of %d", forestWith, forestTracks));
}

void testFieldPresets()
{
    section("field track: every preset names only knobs that exist, and puts its recordings' categories in");
    ParamStore p;
    int unknown = 0, bad = 0;
    std::string first;
    for (const FieldPreset& fp : fieldPresets()) {
        std::string text = fp.text;
        size_t at = 0;
        while (at < text.size()) {
            const size_t nl = text.find('\n', at);
            const std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            at = nl == std::string::npos ? text.size() : nl + 1;
            const size_t eq = line.find('=');
            if (eq != std::string::npos && p.find("field." + line.substr(0, eq)) < 0) {
                ++unknown;
                if (first.empty()) first = std::string(fp.name) + ": " + line;
            }
        }
        const auto values = fieldPresetValues(p, fp);
        float a = -1.0f, bOn = -1.0f;
        for (const auto& kv : values) { if (kv.first == field::ACategory) a = kv.second; if (kv.first == field::BOn) bOn = kv.second; }
        if (a != static_cast<float>(fp.categoryA + 1) || bOn != (fp.categoryB >= 0 ? 1.0f : 0.0f)) ++bad;
    }
    check(fieldPresets().size() >= 100 && unknown == 0 && bad == 0, "the presets are well formed",
          fmt("%zu presets, %d unknown keys (%s), %d with the wrong layers", fieldPresets().size(), unknown, first.c_str(), bad));
}

void testFieldNoLibrary()
{
    section("field track: without the recordings the score is the same, the Field track is silent and the rest plays");
    // The user: "Bitte achte darauf, dass das Programm auch dann funktioniert, wenn die Samples nicht heruntergeladen
    // wurden!" -- a Dark Forest track, whose intro has its place for certain, composed with the shipped folder (where
    // this machine has it) and without; then its intro rendered without.
    ParamStore q;
    // Without the level, presence and audibility matches: they plan by probe renders of the track (45 s here), and
    // what this test compares -- the notes and the controls with and without the library -- does not need them.
    q.parseText("compose.style=3 compose.style_mix=Off compose.level_match=Off compose.presence_match=Off "
                "compose.audibility_match=Off master.auto_gain=Off");
    auto compose = [&](std::vector<NoteEvent>& notes, std::vector<ControlEvent>& ctl) {
        Composer c(4711);
        const TrackPlan t = c.track(q, 0);
        c.composeBars(q, 0, 32, notes, &ctl);
        return t.form.field.size();
    };
    setFieldShippedFolder(true);
    scanFieldLibrary();
    std::vector<NoteEvent> n1, n2;
    std::vector<ControlEvent> c1, c2;
    compose(n1, c1);
    setFieldShippedFolder(false);
    const int left = scanFieldLibrary();
    const size_t places = compose(n2, c2);
    bool same = n1.size() == n2.size() && c1.size() == c2.size();
    for (size_t i = 0; same && i < n1.size(); ++i)
        same = n1[i].beat == n2[i].beat && n1[i].part == n2[i].part && n1[i].pitch == n2[i].pitch && n1[i].velocity == n2[i].velocity
            && n1[i].length == n2[i].length;
    for (size_t i = 0; same && i < c1.size(); ++i)
        same = c1[i].beat == c2[i].beat && c1[i].param == c2[i].param && c1[i].kind == c2[i].kind && c1[i].value == c2[i].value
            && c1[i].length == c2[i].length;
    int fieldNotes = 0;
    for (const NoteEvent& n : n2) fieldNotes += n.part == Part::Field ? 1 : 0;
    check(same && places > 0 && fieldNotes > 0, "the score does not depend on whether the recordings are there",
          fmt("%zu/%zu notes, %zu/%zu controls, %zu places, %d Field notes, %d recordings left", n1.size(), n2.size(), c1.size(),
              c2.size(), places, fieldNotes, left));

    auto e = std::make_unique<Engine>();
    e->prepare(kSr, 512);
    e->params().copyValuesFrom(q);
    for (const ControlEvent& c : c2) e->pushControl(c);
    for (const NoteEvent& n : n2) e->pushEvent(n);
    const size_t len = static_cast<size_t>(16.0 * kBeatsPerBar * 60.0 / 138.0 * kSr);   // the intro's first sixteen bars
    std::vector<float> L(len), R(len);
    std::vector<std::vector<float>> stemL(kNumStems, std::vector<float>(len)), stemR(kNumStems, std::vector<float>(len));
    StemTap tap;
    for (int k = 0; k < kNumStems; ++k) { tap.L[k] = stemL[static_cast<size_t>(k)].data(); tap.R[k] = stemR[static_cast<size_t>(k)].data(); }
    e->setStemTap(&tap);
    for (size_t done = 0; done < len; done += 512) e->process(L.data() + done, R.data() + done, static_cast<int>(std::min<size_t>(512, len - done)));
    e->setStemTap(nullptr);
    double field = 0.0, mix = 0.0;
    bool finite = true;
    for (size_t i = 0; i < len; ++i) {
        field += static_cast<double>(stemL[static_cast<size_t>(Part::Field)][i]) * stemL[static_cast<size_t>(Part::Field)][i];
        mix += static_cast<double>(L[i]) * L[i];
        finite = finite && std::isfinite(L[i]) && std::isfinite(R[i]);
    }
    check(field == 0.0 && mix > 1.0 && finite, "without the recordings the Field track is silent and the rest of the mix plays",
          fmt("field %.3g, mix %.3g, %s", field, mix, finite ? "finite" : "NOT FINITE"));
    setFieldShippedFolder(true);
}

} // namespace phostest
