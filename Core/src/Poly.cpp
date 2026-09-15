/**
 * @file Poly.cpp
 * @brief Polyphonic engine: voice allocation, supersaw coefficients, rendering.
 */
#include "phos/Poly.h"
#include "phos/Params.h"
#include <algorithm>
#include <cmath>

namespace phos {

namespace {

constexpr double kPiD = 3.141592653589793;

/** @brief Pan position of each unison oscillator: low and high partners on opposite sides. */
constexpr double kUnisonPan[kPolyUnison] = { -1.0, 0.67, -0.33, 0.0, 0.33, -0.67, 1.0 };

void svfCoefs(double fc, double damping, double sr, float& a1, float& a2, float& a3)
{
    const double g = std::tan(kPiD * std::clamp(fc, 10.0, 0.45 * sr) / sr);
    const double d1 = 1.0 / (1.0 + g * (g + damping));
    a1 = static_cast<float>(d1);
    a2 = static_cast<float>(g * d1);
    a3 = static_cast<float>(g * g * d1);
}

} // namespace

double Poly::detuneCurve(double x)
{
    // Szabo (2010), the eleventh-degree fit to the JP-8000's detune, highest power first.
    static constexpr double c[12] = { 10028.7312891634, -50818.8652045924, 111363.4808729368, -138150.6761080548,
                                      106649.6679158292, -53046.9642751875, 17019.9518580080, -3425.0836591318,
                                      404.2703938388, -24.1878824391, 0.6717417634, 0.0030115596 };
    const double t = std::clamp(x, 0.0, 1.0);
    double y = 0.0;
    for (double k : c) y = y * t + k;
    return std::max(0.0, y);
}

void Poly::mixGains(double x, double& center, double& side)
{
    const double t = std::clamp(x, 0.0, 1.0);
    center = -0.55366 * t + 0.99785;
    side = -0.73764 * t * t + 1.2841 * t + 0.044372;
}

double Poly::dynamicDetune(double knob, double amount, double lengthBeats)
{
    // 0 at a sixteenth or shorter, 1 from a beat on, linear in log2 of the length in between.
    const double s = std::clamp(std::log2(std::max(lengthBeats, 1e-6) / 0.25) / 2.0, 0.0, 1.0);
    return knob * (1.0 - std::clamp(amount, 0.0, 1.0) * (1.0 - s));
}

void Poly::prepare(double sampleRate)
{
    sr_ = sampleRate;
    for (Envelope& e : amp_) e.setSampleRate(sr_);
    delay_.prepare(sr_);
    slotL_.assign(static_cast<size_t>(kPolyBlock * kPolySlots), 0.0f);
    slotR_.assign(static_cast<size_t>(kPolyBlock * kPolySlots), 0.0f);
    chanIn_.assign(static_cast<size_t>(kPolyBlock * kPolyLanes), 0.0f);
    chanAmp_.assign(static_cast<size_t>(kPolyBlock * kPolyLanes), 0.0f);
    chanOut_.assign(static_cast<size_t>(kPolyBlock * kPolyLanes), 0.0f);
    sendBuf_.assign(static_cast<size_t>(kPolyBlock), 0.0f);
    reset();
}

void Poly::reset()
{
    slots_ = PolySlots{};
    ch_ = PolyChannels{};
    for (int s = 0; s < kPolySlots; ++s) { slots_.dt[s] = 0.001f; slots_.inv[s] = 1000.0f; slots_.idxDecay[s] = 1.0f; slots_.pw[s] = 0.5f; }
    for (int v = 0; v < kPolyVoices; ++v) {
        amp_[v].kill();
        fenv_[v] = 0.0f;
        gate_[v] = 0;
        age_[v] = 0;
        pitch_[v] = 60;
        hpHz_[v] = 220.0f;
        voiceCoefs(v);
    }
    counter_ = 0;
    pos_ = 0;
    phaseRng_.seed(0x504F4C59ull);
    delay_.reset();
}

void Poly::update(const float* v, double bpm)
{
    std::copy(v, v + poly::Count, values_);
    fDecay_ = static_cast<float>(std::exp(std::log(1.0e-3) / (v[poly::FilterDecay] * 0.001 * sr_)));
    for (Envelope& e : amp_)
        e.setTimes(v[poly::AmpAttack] * 0.001f, v[poly::AmpDecay] * 0.001f, v[poly::AmpSustain], std::max(0.005f, v[poly::AmpRelease] * 0.001f));
    send_ = v[poly::DelaySend];
    level_ = dbToGain(v[poly::Level]);
    const int dl = std::clamp(static_cast<int>(std::lround(v[poly::DelayLeft])), 0, kNumDelayTimes - 1);
    const int dr = std::clamp(static_cast<int>(std::lround(v[poly::DelayRight])), 0, kNumDelayTimes - 1);
    delay_.set(kDelayBeats[dl], kDelayBeats[dr], bpm, v[poly::DelayFeedback], v[poly::DelayHighPass], v[poly::DelayLowPass]);
}

void Poly::voiceCoefs(int voice)
{
    // High pass: fixed for the note. The low pass follows its envelope in renderSegment().
    for (int c = 0; c < 2; ++c)
        svfCoefs(hpHz_[voice], std::sqrt(2.0), sr_, ch_.c1[voice * 2 + c], ch_.c2[voice * 2 + c], ch_.c3[voice * 2 + c]);
}

int Poly::activeVoices() const
{
    int n = 0;
    for (const Envelope& e : amp_) n += e.isActive() ? 1 : 0;
    return n;
}

void Poly::noteOn(int pitch, float velocity, double lengthBeats, int gateSamples, double late)
{
    const float* v = values_;
    // A free voice, else the one that started longest ago.
    int voice = -1;
    for (int i = 0; i < kPolyVoices && voice < 0; ++i) if (!amp_[i].isActive()) voice = i;
    if (voice < 0) {
        voice = 0;
        for (int i = 1; i < kPolyVoices; ++i) if (age_[i] < age_[voice]) voice = i;
    }
    const bool wasIdle = !amp_[voice].isActive();
    age_[voice] = ++counter_;
    pitch_[voice] = pitch;
    vel_[voice] = clampv(velocity, 0.0f, 1.0f);
    gate_[voice] = std::max(1, gateSamples);

    const int osc = std::clamp(static_cast<int>(std::lround(v[poly::Osc])), 0, static_cast<int>(PolyOsc::Count) - 1);
    const double x = dynamicDetune(v[poly::Detune], v[poly::DynamicDetune], lengthBeats);
    const double y = detuneCurve(x);
    double center = 1.0, side = 0.0;
    mixGains(v[poly::Mix], center, side);
    const double f0 = midiToHz(pitch);
    const double velGain = 1.0 - v[poly::VelSens] + v[poly::VelSens] * vel_[voice];
    const double width = v[poly::Width];
    const double wave = v[poly::Wave];
    const double fmI = v[poly::FmIndex];
    const float idxDecay = static_cast<float>(std::exp(-3.0 / (std::max(1.0, v[poly::FmDecay] * 0.001 * sr_))));

    double gains[kPolyUnison] = {}, detuneScale = 1.0;
    switch (static_cast<PolyOsc>(osc)) {
    case PolyOsc::Supersaw:
        for (int u = 0; u < kPolyUnison; ++u) gains[u] = u == 3 ? center : side;
        break;
    case PolyOsc::Va:
        gains[2] = gains[4] = 0.5 * side;
        gains[3] = center;
        break;
    case PolyOsc::Fm:
        gains[2] = gains[4] = 0.35 * side;
        gains[3] = center;
        detuneScale = 0.5;
        break;
    default: break;
    }
    double power = 0.0;
    for (double g : gains) power += g * g;
    const double norm = power > 0.0 ? 1.0 / std::sqrt(power) : 0.0;

    for (int u = 0; u < kPolyUnison; ++u) {
        const int s = voice * kPolyUnison + u;
        const double hz = f0 * (1.0 + kSupersawOffsets[u] * y * detuneScale);
        const double dt = std::min(hz / sr_, 0.45);
        const double mdt = std::min(hz * v[poly::FmRatio] / sr_, 0.45);
        slots_.dt[s] = static_cast<float>(dt);
        slots_.inv[s] = static_cast<float>(1.0 / dt);
        slots_.mdt[s] = static_cast<float>(mdt);
        slots_.pw[s] = v[poly::PulseWidth];
        const bool fm = osc == static_cast<int>(PolyOsc::Fm);
        slots_.wSaw[s] = fm ? 0.0f : static_cast<float>(osc == static_cast<int>(PolyOsc::Va) ? 1.0 - wave : 1.0);
        slots_.wPulse[s] = osc == static_cast<int>(PolyOsc::Va) ? static_cast<float>(wave) : 0.0f;
        slots_.wFm[s] = fm ? 1.0f : 0.0f;
        slots_.idxFloor[s] = static_cast<float>(0.3 * fmI);
        slots_.idxDecay[s] = idxDecay;
        const double theta = (std::clamp(width * kUnisonPan[u], -1.0, 1.0) + 1.0) * kPiD / 4.0;
        const double g = gains[u] * norm * velGain;
        slots_.gL[s] = static_cast<float>(g * std::cos(theta) * std::sqrt(2.0));
        slots_.gR[s] = static_cast<float>(g * std::sin(theta) * std::sqrt(2.0));
        // Szabo: a new random phase for every oscillator at every note. `late` samples already passed.
        double ph = static_cast<double>(phaseRng_.uniform()) + dt * late;
        slots_.ph[s] = static_cast<float>(ph - std::floor(ph));
        const double mp = mdt * late;
        slots_.mph[s] = static_cast<float>(mp - std::floor(mp));
        slots_.idx[s] = static_cast<float>(0.7 * fmI * std::pow(static_cast<double>(idxDecay), late));
    }
    hpHz_[voice] = std::max(v[poly::HpFloor], static_cast<float>(v[poly::HpTrack] * f0));
    if (wasIdle) {
        for (int c = 0; c < 2; ++c) {
            const int l = voice * 2 + c;
            ch_.ic1[l] = ch_.ic2[l] = ch_.ha1[l] = ch_.ha2[l] = ch_.hb1[l] = ch_.hb2[l] = 0.0f;
        }
    }
    voiceCoefs(voice);
    fenv_[voice] = static_cast<float>(std::pow(static_cast<double>(fDecay_), late));
    lowPassCoefs(voice, 2.0 - 1.9 * std::clamp(static_cast<double>(v[poly::Resonance]), 0.0, 1.0));
    amp_[voice].noteOn();
    amp_[voice].advanceAttack(late);
}

template <class V>
void Poly::renderSegment(float* L, float* R, int n)
{
    const int width = laneWidth<V>();
    const float* v = values_;
    const double damping = 2.0 - 1.9 * std::clamp(static_cast<double>(v[poly::Resonance]), 0.0, 1.0);
    bool voiceOn[kPolyVoices] = {};
    for (int voice = 0; voice < kPolyVoices; ++voice) {
        voiceOn[voice] = amp_[voice].isActive();
        if (!voiceOn[voice]) {
            for (int i = 0; i < n; ++i) chanAmp_[static_cast<size_t>(i * kPolyLanes + voice * 2)] = chanAmp_[static_cast<size_t>(i * kPolyLanes + voice * 2 + 1)] = 0.0f;
            continue;
        }
        // Low pass from the envelope, renewed on the absolute 16-sample grid only: where the host cuts
        // its blocks must not decide when a coefficient changes.
        if (pos_ % kPolyBlock == 0) lowPassCoefs(voice, damping);
        for (int i = 0; i < n; ++i) {
            if (gate_[voice] > 0 && --gate_[voice] == 0) amp_[voice].noteOff();
            const float a = amp_[voice].process();
            chanAmp_[static_cast<size_t>(i * kPolyLanes + voice * 2)] = a;
            chanAmp_[static_cast<size_t>(i * kPolyLanes + voice * 2 + 1)] = a;
            fenv_[voice] *= fDecay_;
        }
    }
    // Groups of eight slots and eight channels run when any of their voices sounds: the same decision
    // on every vector path.
    for (int g = 0; g < kPolySlots / 8; ++g) {
        bool on = false;
        for (int s = g * 8; s < g * 8 + 8; ++s) on = on || voiceOn[s / kPolyUnison];
        if (on) {
            for (int s = g * 8; s < g * 8 + 8; s += width) polySlotKernel<V>(slots_, s, n, slotL_.data(), slotR_.data());
        } else {
            for (int i = 0; i < n; ++i)
                for (int s = g * 8; s < g * 8 + 8; ++s) slotL_[static_cast<size_t>(i * kPolySlots + s)] = slotR_[static_cast<size_t>(i * kPolySlots + s)] = 0.0f;
        }
    }
    for (int voice = 0; voice < kPolyVoices; ++voice) {
        for (int i = 0; i < n; ++i) {
            float sl = 0.0f, sr = 0.0f;
            for (int u = 0; u < kPolyUnison; ++u) {
                sl += slotL_[static_cast<size_t>(i * kPolySlots + voice * kPolyUnison + u)];
                sr += slotR_[static_cast<size_t>(i * kPolySlots + voice * kPolyUnison + u)];
            }
            chanIn_[static_cast<size_t>(i * kPolyLanes + voice * 2)] = sl;
            chanIn_[static_cast<size_t>(i * kPolyLanes + voice * 2 + 1)] = sr;
        }
    }
    for (int g = 0; g < kPolyLanes / 8; ++g) {
        bool on = false;
        for (int l = g * 8; l < g * 8 + 8; ++l) on = on || voiceOn[l / 2];
        if (on) {
            for (int l = g * 8; l < g * 8 + 8; l += width) polyChannelKernel<V>(ch_, l, n, chanIn_.data(), chanAmp_.data(), chanOut_.data());
        } else {
            for (int i = 0; i < n; ++i)
                for (int l = g * 8; l < g * 8 + 8; ++l) chanOut_[static_cast<size_t>(i * kPolyLanes + l)] = 0.0f;
        }
    }
    for (int i = 0; i < n; ++i) {
        float l = 0.0f, r = 0.0f;
        for (int voice = 0; voice < kPolyVoices; ++voice) {
            l += chanOut_[static_cast<size_t>(i * kPolyLanes + voice * 2)];
            r += chanOut_[static_cast<size_t>(i * kPolyLanes + voice * 2 + 1)];
        }
        L[i] = l * level_;
        R[i] = r * level_;
        sendBuf_[static_cast<size_t>(i)] = 0.5f * (L[i] + R[i]) * send_;
    }
    delay_.process(sendBuf_.data(), L, R, n);
    pos_ += static_cast<uint64_t>(n);
}

void Poly::lowPassCoefs(int voice, double damping)
{
    const float* v = values_;
    const double oct = v[poly::EnvAmount] * fenv_[voice] + v[poly::KeyTrack] * (pitch_[voice] - 60) / 12.0;
    const double fc = std::min(static_cast<double>(v[poly::Cutoff]) * std::pow(2.0, oct), 0.45 * sr_);
    svfCoefs(fc, damping, sr_, ch_.a1[voice * 2], ch_.a2[voice * 2], ch_.a3[voice * 2]);
    ch_.a1[voice * 2 + 1] = ch_.a1[voice * 2];
    ch_.a2[voice * 2 + 1] = ch_.a2[voice * 2];
    ch_.a3[voice * 2 + 1] = ch_.a3[voice * 2];
}

template <class V>
void Poly::processWith(float* L, float* R, int n)
{
    int done = 0;
    while (done < n) {
        const int m = std::min(static_cast<int>(kPolyBlock - pos_ % kPolyBlock), n - done);
        renderSegment<V>(L + done, R + done, m);
        done += m;
    }
}

template void Poly::processWith<float>(float*, float*, int);
#if PHOS_VEC_PATH != 0
template void Poly::processWith<VecF>(float*, float*, int);
#endif

void Poly::process(float* L, float* R, int n)
{
    processWith<VecF>(L, R, n);
}

} // namespace phos
