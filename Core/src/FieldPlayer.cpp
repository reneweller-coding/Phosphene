/**
 * @file FieldPlayer.cpp
 * @brief The Field track's sampler and the NASA shots (FieldPlayer.h).
 */
#include "phos/FieldPlayer.h"
#include <algorithm>
#include <cmath>

namespace phos {

namespace {

constexpr double kPi2 = 1.5707963267948966;   ///< pi / 2

/** @brief The 4-point Hermite interpolation of @p s at @p p (indices clamped to the recording). */
inline float hermite(const float* s, int n, double p)
{
    const int i = static_cast<int>(std::floor(p));
    const float t = static_cast<float>(p - static_cast<double>(i));
    auto at = [&](int k) { return s[std::clamp(k, 0, n - 1)]; };
    const float x0 = at(i - 1), x1 = at(i), x2 = at(i + 1), x3 = at(i + 2);
    const float c1 = 0.5f * (x2 - x0);
    const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * t + c2) * t + c1) * t + x1;
}

/** @brief The circuit models' level against the state-variable filter (Poly.cpp, 26.09.2026): a + b res in dB; a per model. */
constexpr float kTrimA[kVoiceFilterModels] = { 0.0f, 1.32f, 1.28f, 1.94f, 0.0f, 1.28f, 5.8f, 0.4f, 0.0f, 0.0f };
constexpr float kTrimB[kVoiceFilterModels] = { 0.0f, 3.4f, 3.6f, 6.8f, 0.0f, 3.6f, 4.0f, 0.0f, 0.0f, 0.0f };   ///< ... and b, per model

/** @brief The Butterworth low cut's two sections (24 dB/octave): their quality factors. */
constexpr float kLowQ1 = 0.5412f, kLowQ2 = 1.3066f;   ///< the second's

} // namespace

void FieldPlayer::prepare(double sampleRate, uint64_t seed)
{
    sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    for (int i = 0; i < kVoices; ++i) {
        Voice& v = voice_[i];
        v.amp.setSampleRate(sr_);
        v.filt.setSampleRate(sr_);
        v.mod.prepare(sr_, mixSeed(seed, static_cast<uint64_t>(i)));
    }
    reset();
    update(v_);
}

void FieldPlayer::reset()
{
    for (Voice& v : voice_) freeVoice(v);
    latest_ = -1;
    at_ = 0;
}

void FieldPlayer::freeVoice(Voice& v)
{
    for (Layer& l : v.layer) {
        releaseFieldClip(l.clip);
        l = Layer{};
    }
    v.on = false;
    v.amp.kill();
    v.filt.kill();
    v.mod.kill();
    for (float& s : v.sums) s = 0.0f;
}

void FieldPlayer::update(const float* v)
{
    if (v != v_) std::copy(v, v + field::Count, v_);
    const ModSettings ms = readModBlock(v_ + field::FiltAttack, kFieldModDestMap, 7);
    for (Voice& vo : voice_) {
        vo.amp.setTimes(v_[field::AmpAttack] * 0.001f, v_[field::AmpDecay] * 0.001f, std::clamp(v_[field::AmpSustain], 0.0f, 1.0f),
                        v_[field::AmpRelease] * 0.001f);
        vo.filt.setTimes(v_[field::FiltAttack] * 0.001f, v_[field::FilterDecay] * 0.001f, std::clamp(v_[field::FiltSustain], 0.0f, 1.0f),
                         v_[field::FiltRelease] * 0.001f);
        vo.mod.set(ms);
    }
    const Modulator& m = voice_[0].mod;
    const bool modFilter = m.active() && (m.targets(ModDest::Cutoff) || m.targets(ModDest::Resonance) || m.targets(ModDest::FilterMode));
    filterOff_ = std::lround(v_[field::FilterModel]) == 0 && std::lround(v_[field::FilterType]) == 0 && v_[field::Cutoff] >= 19000.0f
              && v_[field::EnvAmount] == 0.0f && !modFilter;
    const float fc = std::clamp(v_[field::LowCut], 10.0f, 0.45f * static_cast<float>(sr_));
    Svf a, b;
    a.setQ(fc, kLowQ1, static_cast<float>(sr_));
    b.setQ(fc, kLowQ2, static_cast<float>(sr_));
    for (Voice& vo : voice_) {
        vo.lowL1.copyCoefficients(a); vo.lowR1.copyCoefficients(a);
        vo.lowL2.copyCoefficients(b); vo.lowR2.copyCoefficients(b);
    }
}

int FieldPlayer::clipFor(int categoryParam, int variation, uint64_t pick)
{
    if (categoryParam > 0) return fieldClipIndex(categoryParam - 1, variation);
    // Auto without the composer's choice (a key played by hand): a category that has recordings, drawn by the event.
    Rng r;
    r.seed(pick ^ 0x41555430ull);
    int have[field::kCategories];
    int n = 0;
    for (int c = 0; c < field::kCategories; ++c)
        if (fieldClipCount(c) > 0) have[n++] = c;
    if (n == 0) return -1;
    const int c = have[r.below(n)];
    return fieldClipIndex(c, r.below(fieldClipCount(c)));
}

void FieldPlayer::wantedClips(int& a, int& b) const
{
    const int ca = static_cast<int>(std::lround(v_[field::ACategory])), cb = static_cast<int>(std::lround(v_[field::BCategory]));
    a = ca > 0 ? fieldClipIndex(ca - 1, static_cast<int>(std::lround(v_[field::AVariation]))) : -1;
    b = cb > 0 && v_[field::BOn] >= 0.5f ? fieldClipIndex(cb - 1, static_cast<int>(std::lround(v_[field::BVariation]))) : -1;
}

void FieldPlayer::startLayer(Layer& l, int index, float gain, float semis, bool reverse, double start)
{
    l = Layer{};
    l.index = index;
    l.on = index >= 0;
    l.gain = gain;
    l.semis = semis;
    l.reverse = reverse;
    l.start = std::clamp(start, 0.0, 1.0);
    if (!l.on) return;
    l.clip = acquireFieldClip(index);
    if (l.clip == nullptr) requestFieldClip(index);
    l.p = -1.0;   // placed at the first control step that has the recording
}

void FieldPlayer::trigger(int samples, float velocity, double late, int semis, double beat, uint64_t pick)
{
    int slot = -1;
    for (int i = 0; i < kVoices && slot < 0; ++i) if (!voice_[i].on) slot = i;
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < kVoices; ++i) if (voice_[i].age < voice_[slot].age) slot = i;
    }
    Voice& v = voice_[slot];
    freeVoice(v);
    Rng r;
    r.seed(pick ^ 0x4649454Cull);
    v.on = true;
    v.age = ++counter_;
    v.pos = 0;
    v.length = std::max(1, samples);
    v.velocity = std::clamp(velocity, 0.0f, 1.0f);
    v.random = r.bipolar();
    v.keySemis = static_cast<float>(semis);
    v.svfL.reset(); v.svfR.reset();
    v.lowL1.reset(); v.lowR1.reset(); v.lowL2.reset(); v.lowR2.reset();
    v.laneL.clear(); v.laneR.clear();
    v.amp.noteOn();
    v.amp.advanceAttack(late);
    v.filt.noteOn();
    v.mod.noteOn(at_, beat);
    // The layers' shares (layer_mix: 0 A alone, 1 B alone, both whole at the middle).
    const bool bOn = v_[field::BOn] >= 0.5f;
    const float mix = std::clamp(v_[field::LayerMix], 0.0f, 1.0f);
    const float gA = bOn ? std::min(1.0f, 2.0f * (1.0f - mix)) : 1.0f, gB = std::min(1.0f, 2.0f * mix);
    auto startOf = [&](int s, int sr) {
        const double base = v_[s];
        return base + static_cast<double>(v_[sr]) * static_cast<double>(r.uniform()) * (1.0 - base);
    };
    const double sa = startOf(field::AStart, field::AStartRandom), sb = startOf(field::BStart, field::BStartRandom);
    startLayer(v.layer[0], clipFor(static_cast<int>(std::lround(v_[field::ACategory])), static_cast<int>(std::lround(v_[field::AVariation])), pick),
               gA * dbToGain(v_[field::ALevel]), v_[field::APitch] + 0.01f * v_[field::AFine], v_[field::AReverse] >= 0.5f, sa);
    if (bOn)
        startLayer(v.layer[1], clipFor(static_cast<int>(std::lround(v_[field::BCategory])), static_cast<int>(std::lround(v_[field::BVariation])), pick ^ 0x42ull),
                   gB * dbToGain(v_[field::BLevel]), v_[field::BPitch] + 0.01f * v_[field::BFine], v_[field::BReverse] >= 0.5f, sb);
    latest_ = slot;
}

int FieldPlayer::active() const
{
    int n = 0;
    for (const Voice& v : voice_) n += v.on ? 1 : 0;
    return n;
}

float FieldPlayer::displayModulation(ModDest d) const
{
    if (latest_ < 0 || !voice_[latest_].on) return 0.0f;
    return voice_[latest_].mod.offset(voice_[latest_].sums, d);
}

void FieldPlayer::control(Voice& v, int64_t at, double beat)
{
    // The modulation's sources and sums.
    if (v.mod.active()) {
        float ext[kModSources] = {};
        ext[static_cast<int>(ModSource::FilterEnv)] = v.filt.level();
        ext[static_cast<int>(ModSource::Velocity)] = v.velocity;
        ext[static_cast<int>(ModSource::Key)] = std::clamp(v.keySemis / 24.0f, -1.0f, 1.0f);
        ext[static_cast<int>(ModSource::Random)] = v.random;
        v.mod.evaluate(at, beat, ext, v.sums);
    }
    v.pitchMod = v.mod.offset(v.sums, ModDest::Pitch);
    v.gainNow = std::clamp(1.0f + v.mod.offset(v.sums, ModDest::Level), 0.0f, 2.0f);
    const float pan = std::clamp(v_[field::Pan] + v.mod.offset(v.sums, ModDest::Pan), -1.0f, 1.0f);
    const double a = (static_cast<double>(pan) + 1.0) * 0.5 * kPi2;
    v.panL = static_cast<float>(std::cos(a) * 1.4142135623730951);
    v.panR = static_cast<float>(std::sin(a) * 1.4142135623730951);
    // The filter.
    if (!filterOff_) {
        const double oct = static_cast<double>(v_[field::EnvAmount]) * static_cast<double>(v.filt.level())
                         + static_cast<double>(v.mod.offset(v.sums, ModDest::Cutoff));
        const int model = std::clamp(static_cast<int>(std::lround(v_[field::FilterModel])), 0, kVoiceFilterModels - 1);
        const float res = std::clamp(v_[field::Resonance] + v.mod.offset(v.sums, ModDest::Resonance), 0.0f, 1.0f);
        v.mode = std::clamp(v_[field::FilterMode] + v.mod.offset(v.sums, ModDest::FilterMode), 0.0f, 1.0f);
        if (model == 0) {
            const double fc = std::clamp(static_cast<double>(v_[field::Cutoff]) * std::exp2(oct), 20.0, 0.45 * sr_);
            v.svfL.set(static_cast<float>(fc), res, static_cast<float>(sr_));
            v.svfR.copyCoefficients(v.svfL);
        } else {
            // The circuit models run at the sample rate here (the synths run them at twice): their cutoff stays under
            // a third of it, where their nonlinearity has room above.
            const double fc = std::clamp(static_cast<double>(v_[field::Cutoff]) * std::exp2(oct), 20.0, 0.3 * sr_);
            const FilterModel fm = static_cast<FilterModel>(model - 1);
            v.g = static_cast<float>(std::tan(3.141592653589793 * fc / sr_));
            v.k = FilterVoicing::feedback(fm, res);
            v.trim = dbToGain(kTrimA[model] + kTrimB[model] * res);
        }
    }
    // The layers: a recording that has arrived since the last step, the loop's frames, the playback ratio.
    for (Layer& l : v.layer) {
        if (!l.on || l.done) continue;
        if (l.clip == nullptr) {
            l.clip = acquireFieldClip(l.index);
            if (l.clip == nullptr) continue;
        }
        const int n = l.clip->frames();
        if (n < 4) { l.done = true; continue; }
        const bool loop = v_[field::Loop] >= 0.5f;
        double ls = std::clamp(static_cast<double>(v_[field::LoopStart]), 0.0, 1.0) * (n - 1);
        double le = std::clamp(static_cast<double>(v_[field::LoopEnd]), 0.0, 1.0) * (n - 1);
        if (le < ls) std::swap(ls, le);
        const double minLen = std::min(0.05 * l.clip->sampleRate, static_cast<double>(n - 1));
        if (le - ls < minLen) { le = std::min(ls + minLen, static_cast<double>(n - 1)); ls = le - minLen; }
        l.ls = ls;
        l.le = le;
        l.x = loop ? std::min(static_cast<double>(v_[field::LoopXfade]) * 0.001 * l.clip->sampleRate, 0.5 * (le - ls)) : 0.0;
        l.loop = loop;
        if (l.p < 0.0) {
            // The start: a share of the loop's playable stretch, or of the recording without a loop.
            if (loop) l.p = l.reverse ? le - l.start * (le - l.x - ls) : ls + l.start * (le - l.x - ls);
            else l.p = (l.reverse ? 1.0 - l.start : l.start) * (n - 1);
        }
        l.ratio = static_cast<double>(l.clip->sampleRate) / sr_ * std::exp2(static_cast<double>(l.semis + v.keySemis + v.pitchMod) / 12.0);
    }
}

void FieldPlayer::readLayer(Layer& l, float& outL, float& outR)
{
    const FieldClip& c = *l.clip;
    const int n = c.frames();
    const float* sl = c.dataL();
    const float* sr = c.dataR();
    float a = 1.0f, b = 0.0f;
    double q = 0.0;
    if (l.loop && l.x > 0.0) {
        if (!l.reverse && l.p >= l.le - l.x) {
            const double t = (l.p - (l.le - l.x)) / l.x;
            q = l.ls + (l.p - (l.le - l.x));
            a = static_cast<float>(std::cos(t * kPi2));
            b = static_cast<float>(std::sin(t * kPi2));
        } else if (l.reverse && l.p <= l.ls + l.x) {
            const double t = ((l.ls + l.x) - l.p) / l.x;
            q = l.le - ((l.ls + l.x) - l.p);
            a = static_cast<float>(std::cos(t * kPi2));
            b = static_cast<float>(std::sin(t * kPi2));
        }
    }
    outL = a * hermite(sl, n, l.p);
    outR = a * hermite(sr, n, l.p);
    if (b > 0.0f) {
        outL += b * hermite(sl, n, q);
        outR += b * hermite(sr, n, q);
    }
    // Advance; at the loop's end the playhead jumps to where the second head was.
    if (!l.reverse) {
        l.p += l.ratio;
        if (l.loop) { if (l.p >= l.le) l.p = l.ls + l.x + (l.p - l.le); }
        else if (l.p >= n - 1) l.done = true;
    } else {
        l.p -= l.ratio;
        if (l.loop) { if (l.p <= l.ls) l.p = l.le - l.x - (l.ls - l.p); }
        else if (l.p <= 0.0) l.done = true;
    }
}

void FieldPlayer::process(float* L, float* R, int n, double beat, double beatsPerSample)
{
    std::fill(L, L + n, 0.0f);
    std::fill(R, R + n, 0.0f);
    const int model = std::clamp(static_cast<int>(std::lround(v_[field::FilterModel])), 0, kVoiceFilterModels - 1);
    const int type = std::clamp(static_cast<int>(std::lround(v_[field::FilterType])), 0, 3);
    const float width = std::clamp(v_[field::Width], 0.0f, 2.0f);
    for (Voice& v : voice_) {
        if (!v.on) continue;
        for (int i = 0; i < n; ++i) {
            if (v.pos % kControl == 0) control(v, at_ + i, beat + static_cast<double>(i) * beatsPerSample);
            const float env = v.amp.process();
            v.filt.process();
            v.mod.tick();
            float xl = 0.0f, xr = 0.0f;
            for (Layer& l : v.layer) {
                if (!l.on || l.done || l.clip == nullptr || l.p < 0.0) continue;
                float a, b;
                readLayer(l, a, b);
                xl += l.gain * l.clip->norm * a;
                xr += l.gain * l.clip->norm * b;
            }
            if (!filterOff_) {
                if (model == 0) {
                    float lp, bp, hp;
                    v.svfL.tick(xl, lp, bp, hp);
                    xl = type == 0 ? lp : type == 1 ? bp * v.svfL.k : type == 2 ? hp : lp + hp;
                    v.svfR.tick(xr, lp, bp, hp);
                    xr = type == 0 ? lp : type == 1 ? bp * v.svfR.k : type == 2 ? hp : lp + hp;
                } else {
                    const FilterModel fm = static_cast<FilterModel>(model - 1);
                    xl = v.laneL.tick(fm, xl, v.g, v.k, v.mode) * v.trim;
                    xr = v.laneR.tick(fm, xr, v.g, v.k, v.mode) * v.trim;
                }
            }
            float lp, bp, hp;
            v.lowL1.tick(xl, lp, bp, hp); v.lowL2.tick(hp, lp, bp, xl);
            v.lowR1.tick(xr, lp, bp, hp); v.lowR2.tick(hp, lp, bp, xr);
            const float g = env * v.velocity * v.gainNow;
            const float m = 0.5f * (xl + xr), s = 0.5f * (xl - xr) * width;
            L[i] += g * v.panL * (m + s);
            R[i] += g * v.panR * (m - s);
            if (++v.pos == v.length) { v.amp.noteOff(); v.filt.noteOff(); v.mod.noteOff(); }
            if (!v.amp.isActive()) break;
        }
        bool sounding = v.amp.isActive();
        bool any = false;
        for (const Layer& l : v.layer) any = any || (l.on && !l.done);
        if (!sounding || !any) freeVoice(v);
    }
    at_ += n;
}

// ---- The NASA shots ----------------------------------------------------------------------------------------------

void FieldShot::reset()
{
    for (Voice& v : voice_) {
        releaseFieldClip(v.clip);
        v = Voice{};
    }
}

void FieldShot::trigger(float velocity, double late, uint64_t pick)
{
    (void)late;
    const int count = fieldShotCount();
    if (count == 0) return;
    Rng r;
    r.seed(pick ^ 0x53484F54ull);
    const int index = fieldShotIndex(r.below(count));
    FieldClip* c = acquireFieldClip(index);
    if (c == nullptr) {
        // Not in memory: a shot that sounds seconds late is not the shot the score asked for, so this one is left out
        // and the recording fetched for the next. The engine asks for every shot when it is prepared.
        requestFieldClip(index);
        return;
    }
    Voice& v = voice_[next_];
    next_ = (next_ + 1) % kVoices;
    releaseFieldClip(v.clip);
    v.clip = c;
    v.index = index;
    v.p = 0.0;
    v.gain = std::clamp(velocity, 0.0f, 1.0f) * c->norm;
    v.pan = 0.35f * r.bipolar();
    v.on = true;
}

void FieldShot::process(float* L, float* R, int n)
{
    for (Voice& v : voice_) {
        if (!v.on) continue;
        const FieldClip& c = *v.clip;
        const int frames = c.frames();
        const double ratio = static_cast<double>(c.sampleRate) / sr_;
        const double a = (static_cast<double>(v.pan) + 1.0) * 0.5 * kPi2;
        const float gl = v.gain * static_cast<float>(std::cos(a) * 1.4142135623730951);
        const float gr = v.gain * static_cast<float>(std::sin(a) * 1.4142135623730951);
        for (int i = 0; i < n; ++i) {
            if (v.p >= frames - 1) { v.on = false; break; }
            L[i] += gl * hermite(c.dataL(), frames, v.p);
            R[i] += gr * hermite(c.dataR(), frames, v.p);
            v.p += ratio;
        }
        if (!v.on) { releaseFieldClip(v.clip); v.clip = nullptr; }
    }
}

} // namespace phos
