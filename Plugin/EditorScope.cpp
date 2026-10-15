/**
 * @file EditorScope.cpp
 * @brief The kick's and the bass's scope (EditorScope.h).
 * @author Rene Weller
 */
#include "EditorScope.h"
#include "PhospheneLookAndFeel.h"
#include "phos/Bass.h"
#include "phos/Fft.h"
#include "phos/Harmony.h"
#include "phos/Kick.h"
#include <cmath>
#include <complex>

using namespace phos;

namespace phosui {

namespace {

constexpr double Sr = 48000.0;   ///< the rate the scope renders at, Hz
constexpr int Columns = 400;       ///< time and frequency resolution of the curves
constexpr double LoHz = 20.0;   ///< the spectrum's lowest frequency, Hz
constexpr double HiHz = 20000.0;   ///< ... its highest

/**
 * @brief A whole number: juce::String(x, 0) prints every digit, not none.
 * @param[in] x  the number
 * @return it as text without decimals
 */
juce::String whole(double x) { return juce::String(juce::roundToInt(x)); }

/**
 * @brief The name of MIDI note @p midi ("A3").
 * @param[in] midi  the MIDI note
 * @return its name
 */
juce::String noteName(int midi) { return juce::String(KeyNames[((midi % 12) + 12) % 12]) + juce::String(midi / 12 - 1); }

/**
 * @brief The nearest MIDI note of @p hz and how many cents it lies off it.
 * @param[in]  hz     the frequency
 * @param[out] cents  how far it lies off the note
 * @return the MIDI note
 */
int nearestNote(double hz, double& cents)
{
    const double m = 69.0 + 12.0 * std::log2(std::max(hz, 1.0) / 440.0);
    const int n = static_cast<int>(std::lround(m));
    cents = (m - n) * 100.0;
    return n;
}

} // namespace

SynthScope::SynthScope(PhospheneProcessor& proc, KindE kind) : m_proc(proc), m_kind(kind)
{
    setOpaque(false);
    startTimerHz(10);
}

SynthScope::~SynthScope() { stopTimer(); }

void SynthScope::refresh()
{
    const ParamStore& ps = m_proc.params();
    const ModuleE m = m_kind == KindE::KICK ? ModuleE::KICK : ModuleE::BASS;
    const int base = ps.base(m), n = ParamStore::moduleCount(m);
    // What plays: the engine's effective values while the set runs (the knobs plus the track's recipe); stopped, the
    // knobs as they stand.
    const TransportView t = m_proc.transport();
    const bool live = t.m_playing && !t.m_restarting;
    std::vector<float> v(static_cast<size_t>(n));
    for (int k = 0; k < n; ++k) v[static_cast<size_t>(k)] = live ? m_proc.engine().effective(base + k) : ps.get(base + k);
    const int key = live ? m_proc.engine().keyRoot()
                         : static_cast<int>(std::lround(ps.get(ps.base(ModuleE::COMPOSE) + compose::KEY)));
    const double bpm = t.m_bpm > 1.0 ? t.m_bpm : 145.0;
    std::vector<float> sig = v;
    sig.push_back(static_cast<float>(key));
    sig.push_back(static_cast<float>(bpm));
    if (sig == m_seen) return;
    m_seen = sig;
    render(v, key, bpm);
    repaint();
}

void SynthScope::render(const std::vector<float>& v, int key, double bpm)
{
    const double eighth = 30.0 / bpm;   // one eighth note, seconds
    m_seconds = m_kind == KindE::KICK ? 0.6 : 2.0 * eighth;
    const int len = static_cast<int>(m_seconds * Sr);
    m_wave.assign(static_cast<size_t>(len), 0.0f);
    m_curveHz.assign(Columns, 0.0f);
    if (m_kind == KindE::KICK) {
        Kick k;
        k.prepare(Sr);
        k.update(v.data(), key);
        k.trigger(1.0f);
        k.process(m_wave.data(), len);
        for (int c = 0; c < Columns; ++c) m_curveHz[static_cast<size_t>(c)] = static_cast<float>(k.frequencyAt(m_seconds * c / (Columns - 1)));
        m_startHz = k.frequencyAt(0.0);
        m_landHz = k.tunedEndHz();
        m_note = -1;
    } else {
        Bass b;
        b.prepare(Sr);
        b.update(v.data());
        const int pitch = bassRootNote(key, 0);
        // One eighth note, released at its end, as a rolling bassline plays it; the second eighth is its tail.
        b.noteOn(pitch, 1.0f, static_cast<int>(eighth * Sr), 0.0, 0.0);
        b.process(m_wave.data(), len);
        // The cutoff as Bass::process computes it (Bass.cpp): cutoff x 2^(env amount x envelope + key tracking),
        // the envelope falling to 1/1000 over the filter decay; velocity 1.
        const double cutoff = v[bass::CUTOFF], envOct = v[bass::ENV_AMOUNT], track = v[bass::KEY_TRACK] * (pitch - 28) / 12.0;
        const double decay = std::max(1.0e-3, static_cast<double>(v[bass::FILTER_DECAY]) * 0.001);
        for (int c = 0; c < Columns; ++c) {
            const double tt = m_seconds * c / (Columns - 1);
            const double env = std::exp(std::log(1.0e-3) * tt / decay);
            m_curveHz[static_cast<size_t>(c)] = static_cast<float>(std::clamp(cutoff * std::pow(2.0, envOct * env + track), 20.0, 0.45 * Sr));
        }
        m_startHz = m_curveHz.front();
        m_landHz = midiToHz(pitch);
        m_note = pitch;
    }
    m_peak = 0.0f;
    int last = 0;
    for (int i = 0; i < len; ++i) {
        const float a = std::fabs(m_wave[static_cast<size_t>(i)]);
        m_peak = std::max(m_peak, a);
    }
    for (int i = 0; i < len; ++i) if (std::fabs(m_wave[static_cast<size_t>(i)]) > m_peak * 1.0e-3f) last = i;   // -60 dB
    m_lengthMs = 1000.0 * last / Sr;

    // The spectrum: one Hann-windowed FFT over the rendered sound, read at log-spaced frequencies.
    const int nfft = 32768;
    std::vector<std::complex<double>> x(static_cast<size_t>(nfft));
    for (int i = 0; i < nfft && i < len; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * 3.141592653589793 * i / std::max(1, len - 1));
        x[static_cast<size_t>(i)] = std::complex<double>(m_wave[static_cast<size_t>(i)] * w, 0.0);
    }
    Fft(nfft).transform(x, false);
    m_spectrumDb.assign(Columns, -120.0f);
    double top = 1.0e-30;
    std::vector<double> mag(static_cast<size_t>(nfft / 2));
    for (int i = 0; i < nfft / 2; ++i) { mag[static_cast<size_t>(i)] = std::norm(x[static_cast<size_t>(i)]); top = std::max(top, mag[static_cast<size_t>(i)]); }
    for (int c = 0; c < Columns; ++c) {
        const double f0 = LoHz * std::pow(HiHz / LoHz, static_cast<double>(c) / Columns);
        const double f1 = LoHz * std::pow(HiHz / LoHz, static_cast<double>(c + 1) / Columns);
        const int b0 = std::clamp(static_cast<int>(f0 * nfft / Sr), 0, nfft / 2 - 1);
        const int b1 = std::clamp(static_cast<int>(f1 * nfft / Sr), b0, nfft / 2 - 1);
        double mx = 0.0;
        for (int b = b0; b <= b1; ++b) mx = std::max(mx, mag[static_cast<size_t>(b)]);
        m_spectrumDb[static_cast<size_t>(c)] = static_cast<float>(10.0 * std::log10(mx / top + 1.0e-12));
    }
}

void SynthScope::paint(juce::Graphics& g)
{
    if (m_wave.empty()) refresh();
    const juce::Colour tint = m_kind == KindE::KICK ? partColour(2) : partColour(3);
    auto area = getLocalBounds().toFloat().reduced(4.0f);
    g.setColour(Bg0);
    g.fillRoundedRectangle(area, 6.0f);
    auto wavePanel = area.removeFromLeft(area.getWidth() * 0.62f).reduced(8.0f, 8.0f);
    auto specPanel = area.reduced(8.0f, 8.0f);

    // ---- the waveform, and the pitch or cutoff curve over it
    auto header = wavePanel.removeFromTop(16.0f);
    g.setFont(body(11.5f));
    g.setColour(Dim);
    double cents = 0.0;
    juce::String info;
    if (m_kind == KindE::KICK) {
        const int nn = nearestNote(m_landHz, cents);
        info << "pitch " << whole(m_startHz) << " > " << juce::String(m_landHz, 1) << " Hz  ("
             << noteName(nn) << (std::fabs(cents) >= 1.0 ? juce::String(" ") + (cents > 0 ? "+" : "") + juce::String(juce::roundToInt(cents)) + " ct" : juce::String())
             << ")   length " << whole(m_lengthMs) << " ms   peak " << juce::String(20.0 * std::log10(std::max(1.0e-6f, m_peak)), 1) << " dB";
    } else {
        info << "note " << noteName(m_note) << " (" << juce::String(m_landHz, 1) << " Hz)   filter " << whole(m_startHz) << " > "
             << whole(m_curveHz.empty() ? 0.0f : m_curveHz.back()) << " Hz   peak "
             << juce::String(20.0 * std::log10(std::max(1.0e-6f, m_peak)), 1) << " dB";
    }
    g.drawText(info, header, juce::Justification::centredLeft, true);
    g.setColour(Edge);
    g.drawRect(wavePanel, 1.0f);
    if (!m_wave.empty()) {
        const float mid = wavePanel.getCentreY(), half = wavePanel.getHeight() * 0.46f;
        const float scale = m_peak > 1.0e-6f ? half / m_peak : 0.0f;
        juce::Path fill;
        const int cols = juce::jmax(2, static_cast<int>(wavePanel.getWidth()));
        const size_t per = juce::jmax<size_t>(1, m_wave.size() / static_cast<size_t>(cols));
        fill.startNewSubPath(wavePanel.getX(), mid);
        std::vector<float> lo(static_cast<size_t>(cols)), hi(static_cast<size_t>(cols));
        for (int c = 0; c < cols; ++c) {
            float a = 0.0f, b = 0.0f;
            for (size_t i = static_cast<size_t>(c) * per; i < std::min(m_wave.size(), (static_cast<size_t>(c) + 1) * per); ++i) {
                a = std::min(a, m_wave[i]);
                b = std::max(b, m_wave[i]);
            }
            lo[static_cast<size_t>(c)] = a;
            hi[static_cast<size_t>(c)] = b;
            fill.lineTo(wavePanel.getX() + c, mid - b * scale);
        }
        for (int c = cols - 1; c >= 0; --c) fill.lineTo(wavePanel.getX() + c, mid - lo[static_cast<size_t>(c)] * scale);
        fill.closeSubPath();
        g.setColour(tint.withAlpha(0.55f));
        g.fillPath(fill);
        g.setColour(tint);
        g.strokePath(fill, juce::PathStrokeType(0.8f));
        // The curve on a log axis from 20 Hz to 2 kHz (the kick's sweep) or 20 Hz to 20 kHz (the cutoff).
        const double top = m_kind == KindE::KICK ? 2000.0 : 20000.0;
        auto yOf = [&](double hz) {
            const double t = std::log(std::clamp(hz, LoHz, top) / LoHz) / std::log(top / LoHz);
            return wavePanel.getBottom() - static_cast<float>(t) * wavePanel.getHeight();
        };
        juce::Path curve;
        for (int c = 0; c < Columns; ++c) {
            const float x = wavePanel.getX() + wavePanel.getWidth() * c / (Columns - 1);
            const float y = yOf(m_curveHz[static_cast<size_t>(c)]);
            if (c == 0) curve.startNewSubPath(x, y); else curve.lineTo(x, y);
        }
        g.setColour(Text.withAlpha(0.9f));
        g.strokePath(curve, juce::PathStrokeType(1.6f));
        g.setColour(Faint);
        g.setFont(body(10.0f));
        for (double hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 5000.0 }) {
            if (hz > top) continue;
            g.drawText(hz >= 1000.0 ? whole(hz / 1000.0) + "k" : whole(hz), juce::Rectangle<float>(wavePanel.getRight() - 30.0f, yOf(hz) - 6.0f, 28.0f, 12.0f),
                       juce::Justification::centredRight, false);
        }
        g.drawText(m_kind == KindE::KICK ? "pitch" : "cutoff", juce::Rectangle<float>(wavePanel.getX() + 4.0f, wavePanel.getY() + 2.0f, 60.0f, 12.0f), juce::Justification::centredLeft, false);
        g.drawText(whole(m_seconds * 1000.0) + " ms", juce::Rectangle<float>(wavePanel.getRight() - 70.0f, wavePanel.getBottom() - 14.0f, 66.0f, 12.0f),
                   juce::Justification::centredRight, false);
    }

    // ---- the spectrum
    auto specHead = specPanel.removeFromTop(16.0f);
    g.setColour(Dim);
    g.setFont(body(11.5f));
    g.drawText("spectrum", specHead, juce::Justification::centredLeft, false);
    g.setColour(Edge);
    g.drawRect(specPanel, 1.0f);
    if (!m_spectrumDb.empty()) {
        juce::Path sp;
        auto yDb = [&](float db) { return specPanel.getY() + juce::jlimit(0.0f, 1.0f, -db / 72.0f) * specPanel.getHeight(); };
        sp.startNewSubPath(specPanel.getX(), specPanel.getBottom());
        for (int c = 0; c < Columns; ++c) sp.lineTo(specPanel.getX() + specPanel.getWidth() * c / (Columns - 1), yDb(m_spectrumDb[static_cast<size_t>(c)]));
        sp.lineTo(specPanel.getRight(), specPanel.getBottom());
        sp.closeSubPath();
        g.setColour(tint.withAlpha(0.45f));
        g.fillPath(sp);
        g.setColour(Faint);
        g.setFont(body(10.0f));
        for (double hz : { 50.0, 100.0, 500.0, 1000.0, 5000.0 }) {
            const float x = specPanel.getX() + specPanel.getWidth() * static_cast<float>(std::log(hz / LoHz) / std::log(HiHz / LoHz));
            g.drawVerticalLine(juce::roundToInt(x), specPanel.getY(), specPanel.getBottom());
            g.drawText(hz >= 1000.0 ? whole(hz / 1000.0) + "k" : whole(hz), juce::Rectangle<float>(x + 2.0f, specPanel.getBottom() - 13.0f, 30.0f, 12.0f),
                       juce::Justification::centredLeft, false);
        }
    }
}

} // namespace phosui
