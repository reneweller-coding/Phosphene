/**
 * @file FieldPlayer.cpp
 * @brief The Field track's sampler and the NASA shots (FieldPlayer.h).
 * @author Rene Weller
 */
#include "phos/FieldPlayer.h"
#include <algorithm>
#include <cmath>

namespace phos {

namespace {

constexpr double Pi2 = 1.5707963267948966;   ///< pi / 2

/**
 * @brief The 4-point Hermite interpolation of @p s at @p p (indices clamped to the recording).
 * @param[in] s  the samples
 * @param[in] n  their count
 * @param[in] p  the position
 * @return the sample
 */
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
constexpr float TrimA[VoiceFilterModels] = { 0.0f, 1.32f, 1.28f, 1.94f, 0.0f, 1.28f, 5.8f, 0.4f, 0.0f, 0.0f };
constexpr float TrimB[VoiceFilterModels] = { 0.0f, 3.4f, 3.6f, 6.8f, 0.0f, 3.6f, 4.0f, 0.0f, 0.0f, 0.0f };   ///< ... and b, per model

/** @brief The Butterworth low cut's two sections (24 dB/octave): their quality factors. */
constexpr float LowQ1 = 0.5412f, LowQ2 = 1.3066f;   ///< the second's

} // namespace

void FieldPlayer::prepare(double sampleRate, uint64_t seed)
{
    m_sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    for (int i = 0; i < Voices; ++i) {
        Voice& v = m_voice[i];
        v.m_amp.setSampleRate(m_sr);
        v.m_filt.setSampleRate(m_sr);
        v.m_mod.prepare(m_sr, mixSeed(seed, static_cast<uint64_t>(i)));
    }
    reset();
    update(m_v);
}

void FieldPlayer::reset()
{
    for (Voice& v : m_voice) freeVoice(v);
    m_latest = -1;
    m_at = 0;
}

void FieldPlayer::freeVoice(Voice& v)
{
    for (Layer& l : v.m_layer) {
        releaseFieldClip(l.m_clip);
        l = Layer{};
    }
    v.m_on = false;
    v.m_amp.kill();
    v.m_filt.kill();
    v.m_mod.kill();
    for (float& s : v.m_sums) s = 0.0f;
}

void FieldPlayer::update(const float* v)
{
    if (v != m_v) std::copy(v, v + field::COUNT, m_v);
    const ModSettings ms = readModBlock(m_v + field::FILT_ATTACK, FieldModDestMap, 7);
    for (Voice& vo : m_voice) {
        vo.m_amp.setTimes(m_v[field::AMP_ATTACK] * 0.001f, m_v[field::AMP_DECAY] * 0.001f, std::clamp(m_v[field::AMP_SUSTAIN], 0.0f, 1.0f),
                        m_v[field::AMP_RELEASE] * 0.001f);
        vo.m_filt.setTimes(m_v[field::FILT_ATTACK] * 0.001f, m_v[field::FILTER_DECAY] * 0.001f, std::clamp(m_v[field::FILT_SUSTAIN], 0.0f, 1.0f),
                         m_v[field::FILT_RELEASE] * 0.001f);
        vo.m_mod.set(ms);
    }
    const Modulator& m = m_voice[0].m_mod;
    const bool modFilter = m.active() && (m.targets(ModDestE::CUTOFF) || m.targets(ModDestE::RESONANCE) || m.targets(ModDestE::FILTER_MODE));
    m_filterOff = std::lround(m_v[field::FILTER_MODEL]) == 0 && std::lround(m_v[field::FILTER_TYPE]) == 0 && m_v[field::CUTOFF] >= 19000.0f
              && m_v[field::ENV_AMOUNT] == 0.0f && !modFilter;
    const float fc = std::clamp(m_v[field::LOW_CUT], 10.0f, 0.45f * static_cast<float>(m_sr));
    Svf a, b;
    a.setQ(fc, LowQ1, static_cast<float>(m_sr));
    b.setQ(fc, LowQ2, static_cast<float>(m_sr));
    for (Voice& vo : m_voice) {
        vo.m_lowL1.copyCoefficients(a); vo.m_lowR1.copyCoefficients(a);
        vo.m_lowL2.copyCoefficients(b); vo.m_lowR2.copyCoefficients(b);
    }
}

int FieldPlayer::clipFor(int categoryParam, int variation, uint64_t pick)
{
    if (categoryParam > 0) return fieldClipIndex(categoryParam - 1, variation);
    // Auto without the composer's choice (a key played by hand): a category that has recordings, drawn by the event.
    Rng r;
    r.seed(pick ^ 0x41555430ull);
    int have[field::Categories];
    int n = 0;
    for (int c = 0; c < field::Categories; ++c)
        if (fieldClipCount(c) > 0) have[n++] = c;
    if (n == 0) return -1;
    const int c = have[r.below(n)];
    return fieldClipIndex(c, r.below(fieldClipCount(c)));
}

void FieldPlayer::wantedClips(int& a, int& b) const
{
    const int ca = static_cast<int>(std::lround(m_v[field::A_CATEGORY])), cb = static_cast<int>(std::lround(m_v[field::B_CATEGORY]));
    a = ca > 0 ? fieldClipIndex(ca - 1, static_cast<int>(std::lround(m_v[field::A_VARIATION]))) : -1;
    b = cb > 0 && m_v[field::B_ON] >= 0.5f ? fieldClipIndex(cb - 1, static_cast<int>(std::lround(m_v[field::B_VARIATION]))) : -1;
}

void FieldPlayer::startLayer(Layer& l, int index, float gain, float semis, bool reverse, double start)
{
    l = Layer{};
    l.m_index = index;
    l.m_on = index >= 0;
    l.m_gain = gain;
    l.m_semis = semis;
    l.m_reverse = reverse;
    l.m_start = std::clamp(start, 0.0, 1.0);
    if (!l.m_on) return;
    l.m_clip = acquireFieldClip(index);
    if (l.m_clip == nullptr) requestFieldClip(index);
    l.m_p = -1.0;   // placed at the first control step that has the recording
}

void FieldPlayer::trigger(int samples, float velocity, double late, int semis, double beat, uint64_t pick)
{
    int slot = -1;
    for (int i = 0; i < Voices && slot < 0; ++i) if (!m_voice[i].m_on) slot = i;
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < Voices; ++i) if (m_voice[i].m_age < m_voice[slot].m_age) slot = i;
    }
    Voice& v = m_voice[slot];
    freeVoice(v);
    Rng r;
    r.seed(pick ^ 0x4649454Cull);
    v.m_on = true;
    v.m_age = ++m_counter;
    v.m_pos = 0;
    v.m_length = std::max(1, samples);
    v.m_velocity = std::clamp(velocity, 0.0f, 1.0f);
    v.m_random = r.bipolar();
    v.m_keySemis = static_cast<float>(semis);
    v.m_svfL.reset(); v.m_svfR.reset();
    v.m_lowL1.reset(); v.m_lowR1.reset(); v.m_lowL2.reset(); v.m_lowR2.reset();
    v.m_laneL.clear(); v.m_laneR.clear();
    v.m_amp.noteOn();
    v.m_amp.advanceAttack(late);
    v.m_filt.noteOn();
    v.m_mod.noteOn(m_at, beat);
    // The layers' shares (layer_mix: 0 A alone, 1 B alone, both whole at the middle).
    const bool bOn = m_v[field::B_ON] >= 0.5f;
    const float mix = std::clamp(m_v[field::LAYER_MIX], 0.0f, 1.0f);
    const float gA = bOn ? std::min(1.0f, 2.0f * (1.0f - mix)) : 1.0f, gB = std::min(1.0f, 2.0f * mix);
    auto startOf = [&](int s, int sr) {
        const double base = m_v[s];
        return base + static_cast<double>(m_v[sr]) * static_cast<double>(r.uniform()) * (1.0 - base);
    };
    const double sa = startOf(field::A_START, field::A_START_RANDOM), sb = startOf(field::B_START, field::B_START_RANDOM);
    startLayer(v.m_layer[0], clipFor(static_cast<int>(std::lround(m_v[field::A_CATEGORY])), static_cast<int>(std::lround(m_v[field::A_VARIATION])), pick),
               gA * dbToGain(m_v[field::A_LEVEL]), m_v[field::A_PITCH] + 0.01f * m_v[field::A_FINE], m_v[field::A_REVERSE] >= 0.5f, sa);
    if (bOn)
        startLayer(v.m_layer[1], clipFor(static_cast<int>(std::lround(m_v[field::B_CATEGORY])), static_cast<int>(std::lround(m_v[field::B_VARIATION])), pick ^ 0x42ull),
                   gB * dbToGain(m_v[field::B_LEVEL]), m_v[field::B_PITCH] + 0.01f * m_v[field::B_FINE], m_v[field::B_REVERSE] >= 0.5f, sb);
    m_latest = slot;
}

int FieldPlayer::active() const
{
    int n = 0;
    for (const Voice& v : m_voice) n += v.m_on ? 1 : 0;
    return n;
}

float FieldPlayer::displayModulation(ModDestE d) const
{
    if (m_latest < 0 || !m_voice[m_latest].m_on) return 0.0f;
    return m_voice[m_latest].m_mod.offset(m_voice[m_latest].m_sums, d);
}

void FieldPlayer::control(Voice& v, int64_t at, double beat)
{
    // The modulation's sources and sums.
    if (v.m_mod.active()) {
        float ext[ModSources] = {};
        ext[static_cast<int>(ModSourceE::FILTER_ENV)] = v.m_filt.level();
        ext[static_cast<int>(ModSourceE::VELOCITY)] = v.m_velocity;
        ext[static_cast<int>(ModSourceE::KEY)] = std::clamp(v.m_keySemis / 24.0f, -1.0f, 1.0f);
        ext[static_cast<int>(ModSourceE::RANDOM)] = v.m_random;
        v.m_mod.evaluate(at, beat, ext, v.m_sums);
    }
    v.m_pitchMod = v.m_mod.offset(v.m_sums, ModDestE::PITCH);
    v.m_gainNow = std::clamp(1.0f + v.m_mod.offset(v.m_sums, ModDestE::LEVEL), 0.0f, 2.0f);
    const float pan = std::clamp(m_v[field::PAN] + v.m_mod.offset(v.m_sums, ModDestE::PAN), -1.0f, 1.0f);
    const double a = (static_cast<double>(pan) + 1.0) * 0.5 * Pi2;
    v.m_panL = static_cast<float>(std::cos(a) * 1.4142135623730951);
    v.m_panR = static_cast<float>(std::sin(a) * 1.4142135623730951);
    // The filter.
    if (!m_filterOff) {
        const double oct = static_cast<double>(m_v[field::ENV_AMOUNT]) * static_cast<double>(v.m_filt.level())
                         + static_cast<double>(v.m_mod.offset(v.m_sums, ModDestE::CUTOFF));
        const int model = std::clamp(static_cast<int>(std::lround(m_v[field::FILTER_MODEL])), 0, VoiceFilterModels - 1);
        const float res = std::clamp(m_v[field::RESONANCE] + v.m_mod.offset(v.m_sums, ModDestE::RESONANCE), 0.0f, 1.0f);
        v.m_mode = std::clamp(m_v[field::FILTER_MODE] + v.m_mod.offset(v.m_sums, ModDestE::FILTER_MODE), 0.0f, 1.0f);
        if (model == 0) {
            const double fc = std::clamp(static_cast<double>(m_v[field::CUTOFF]) * std::exp2(oct), 20.0, 0.45 * m_sr);
            v.m_svfL.set(static_cast<float>(fc), res, static_cast<float>(m_sr));
            v.m_svfR.copyCoefficients(v.m_svfL);
        } else {
            // The circuit models run at the sample rate here (the synths run them at twice): their cutoff stays under
            // a third of it, where their nonlinearity has room above.
            const double fc = std::clamp(static_cast<double>(m_v[field::CUTOFF]) * std::exp2(oct), 20.0, 0.3 * m_sr);
            const FilterModelE fm = static_cast<FilterModelE>(model - 1);
            v.m_g = static_cast<float>(std::tan(3.141592653589793 * fc / m_sr));
            v.m_k = FilterVoicing::feedback(fm, res);
            v.m_trim = dbToGain(TrimA[model] + TrimB[model] * res);
        }
    }
    // The layers: a recording that has arrived since the last step, the loop's frames, the playback ratio.
    for (Layer& l : v.m_layer) {
        if (!l.m_on || l.m_done) continue;
        if (l.m_clip == nullptr) {
            l.m_clip = acquireFieldClip(l.m_index);
            if (l.m_clip == nullptr) continue;
        }
        const int n = l.m_clip->frames();
        if (n < 4) { l.m_done = true; continue; }
        const bool loop = m_v[field::LOOP] >= 0.5f;
        double ls = std::clamp(static_cast<double>(m_v[field::LOOP_START]), 0.0, 1.0) * (n - 1);
        double le = std::clamp(static_cast<double>(m_v[field::LOOP_END]), 0.0, 1.0) * (n - 1);
        if (le < ls) std::swap(ls, le);
        const double minLen = std::min(0.05 * l.m_clip->m_sampleRate, static_cast<double>(n - 1));
        if (le - ls < minLen) { le = std::min(ls + minLen, static_cast<double>(n - 1)); ls = le - minLen; }
        l.m_ls = ls;
        l.m_le = le;
        l.m_x = loop ? std::min(static_cast<double>(m_v[field::LOOP_XFADE]) * 0.001 * l.m_clip->m_sampleRate, 0.5 * (le - ls)) : 0.0;
        l.m_loop = loop;
        if (l.m_p < 0.0) {
            // The start: a share of the loop's playable stretch, or of the recording without a loop.
            if (loop) l.m_p = l.m_reverse ? le - l.m_start * (le - l.m_x - ls) : ls + l.m_start * (le - l.m_x - ls);
            else l.m_p = (l.m_reverse ? 1.0 - l.m_start : l.m_start) * (n - 1);
        }
        l.m_ratio = static_cast<double>(l.m_clip->m_sampleRate) / m_sr * std::exp2(static_cast<double>(l.m_semis + v.m_keySemis + v.m_pitchMod) / 12.0);
    }
}

void FieldPlayer::readLayer(Layer& l, float& outL, float& outR)
{
    const FieldClip& c = *l.m_clip;
    const int n = c.frames();
    const float* sl = c.dataL();
    const float* sr = c.dataR();
    float a = 1.0f, b = 0.0f;
    double q = 0.0;
    if (l.m_loop && l.m_x > 0.0) {
        if (!l.m_reverse && l.m_p >= l.m_le - l.m_x) {
            const double t = (l.m_p - (l.m_le - l.m_x)) / l.m_x;
            q = l.m_ls + (l.m_p - (l.m_le - l.m_x));
            a = static_cast<float>(std::cos(t * Pi2));
            b = static_cast<float>(std::sin(t * Pi2));
        } else if (l.m_reverse && l.m_p <= l.m_ls + l.m_x) {
            const double t = ((l.m_ls + l.m_x) - l.m_p) / l.m_x;
            q = l.m_le - ((l.m_ls + l.m_x) - l.m_p);
            a = static_cast<float>(std::cos(t * Pi2));
            b = static_cast<float>(std::sin(t * Pi2));
        }
    }
    outL = a * hermite(sl, n, l.m_p);
    outR = a * hermite(sr, n, l.m_p);
    if (b > 0.0f) {
        outL += b * hermite(sl, n, q);
        outR += b * hermite(sr, n, q);
    }
    // Advance; at the loop's end the playhead jumps to where the second head was.
    if (!l.m_reverse) {
        l.m_p += l.m_ratio;
        if (l.m_loop) { if (l.m_p >= l.m_le) l.m_p = l.m_ls + l.m_x + (l.m_p - l.m_le); }
        else if (l.m_p >= n - 1) l.m_done = true;
    } else {
        l.m_p -= l.m_ratio;
        if (l.m_loop) { if (l.m_p <= l.m_ls) l.m_p = l.m_le - l.m_x - (l.m_ls - l.m_p); }
        else if (l.m_p <= 0.0) l.m_done = true;
    }
}

void FieldPlayer::process(float* L, float* R, int n, double beat, double beatsPerSample)
{
    std::fill(L, L + n, 0.0f);
    std::fill(R, R + n, 0.0f);
    const int model = std::clamp(static_cast<int>(std::lround(m_v[field::FILTER_MODEL])), 0, VoiceFilterModels - 1);
    const int type = std::clamp(static_cast<int>(std::lround(m_v[field::FILTER_TYPE])), 0, 3);
    const float width = std::clamp(m_v[field::WIDTH], 0.0f, 2.0f);
    for (Voice& v : m_voice) {
        if (!v.m_on) continue;
        for (int i = 0; i < n; ++i) {
            if (v.m_pos % Control == 0) control(v, m_at + i, beat + static_cast<double>(i) * beatsPerSample);
            const float env = v.m_amp.process();
            v.m_filt.process();
            v.m_mod.tick();
            float xl = 0.0f, xr = 0.0f;
            for (Layer& l : v.m_layer) {
                if (!l.m_on || l.m_done || l.m_clip == nullptr || l.m_p < 0.0) continue;
                float a, b;
                readLayer(l, a, b);
                xl += l.m_gain * l.m_clip->m_norm * a;
                xr += l.m_gain * l.m_clip->m_norm * b;
            }
            if (!m_filterOff) {
                if (model == 0) {
                    float lp, bp, hp;
                    v.m_svfL.tick(xl, lp, bp, hp);
                    xl = type == 0 ? lp : type == 1 ? bp * v.m_svfL.m_k : type == 2 ? hp : lp + hp;
                    v.m_svfR.tick(xr, lp, bp, hp);
                    xr = type == 0 ? lp : type == 1 ? bp * v.m_svfR.m_k : type == 2 ? hp : lp + hp;
                } else {
                    const FilterModelE fm = static_cast<FilterModelE>(model - 1);
                    xl = v.m_laneL.tick(fm, xl, v.m_g, v.m_k, v.m_mode) * v.m_trim;
                    xr = v.m_laneR.tick(fm, xr, v.m_g, v.m_k, v.m_mode) * v.m_trim;
                }
            }
            float lp, bp, hp;
            v.m_lowL1.tick(xl, lp, bp, hp); v.m_lowL2.tick(hp, lp, bp, xl);
            v.m_lowR1.tick(xr, lp, bp, hp); v.m_lowR2.tick(hp, lp, bp, xr);
            const float g = env * v.m_velocity * v.m_gainNow;
            const float m = 0.5f * (xl + xr), s = 0.5f * (xl - xr) * width;
            L[i] += g * v.m_panL * (m + s);
            R[i] += g * v.m_panR * (m - s);
            if (++v.m_pos == v.m_length) { v.m_amp.noteOff(); v.m_filt.noteOff(); v.m_mod.noteOff(); }
            if (!v.m_amp.isActive()) break;
        }
        bool sounding = v.m_amp.isActive();
        bool any = false;
        for (const Layer& l : v.m_layer) any = any || (l.m_on && !l.m_done);
        if (!sounding || !any) freeVoice(v);
    }
    m_at += n;
}

// ---- The NASA shots ----------------------------------------------------------------------------------------------

void FieldShot::reset()
{
    for (Voice& v : m_voice) {
        releaseFieldClip(v.m_clip);
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
    Voice& v = m_voice[m_next];
    m_next = (m_next + 1) % Voices;
    releaseFieldClip(v.m_clip);
    v.m_clip = c;
    v.m_index = index;
    v.m_p = 0.0;
    v.m_gain = std::clamp(velocity, 0.0f, 1.0f) * c->m_norm;
    v.m_pan = 0.35f * r.bipolar();
    v.m_on = true;
}

void FieldShot::process(float* L, float* R, int n)
{
    for (Voice& v : m_voice) {
        if (!v.m_on) continue;
        const FieldClip& c = *v.m_clip;
        const int frames = c.frames();
        const double ratio = static_cast<double>(c.m_sampleRate) / m_sr;
        const double a = (static_cast<double>(v.m_pan) + 1.0) * 0.5 * Pi2;
        const float gl = v.m_gain * static_cast<float>(std::cos(a) * 1.4142135623730951);
        const float gr = v.m_gain * static_cast<float>(std::sin(a) * 1.4142135623730951);
        for (int i = 0; i < n; ++i) {
            if (v.m_p >= frames - 1) { v.m_on = false; break; }
            L[i] += gl * hermite(c.dataL(), frames, v.m_p);
            R[i] += gr * hermite(c.dataR(), frames, v.m_p);
            v.m_p += ratio;
        }
        if (!v.m_on) { releaseFieldClip(v.m_clip); v.m_clip = nullptr; }
    }
}

} // namespace phos
