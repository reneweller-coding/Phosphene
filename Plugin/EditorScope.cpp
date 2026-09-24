/**
 * @file EditorScope.cpp
 * @brief The kick's and the bass's scope (EditorScope.h).
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

constexpr double kSr = 48000.0;
constexpr int kColumns = 400;       ///< time and frequency resolution of the curves
constexpr double kLoHz = 20.0, kHiHz = 20000.0;

/** @brief A whole number: juce::String(x, 0) prints every digit, not none. */
juce::String whole(double x) { return juce::String(juce::roundToInt(x)); }

juce::String noteName(int midi) { return juce::String(kKeyNames[((midi % 12) + 12) % 12]) + juce::String(midi / 12 - 1); }

/** @brief The nearest MIDI note of @p hz and how many cents it lies off it. */
int nearestNote(double hz, double& cents)
{
    const double m = 69.0 + 12.0 * std::log2(std::max(hz, 1.0) / 440.0);
    const int n = static_cast<int>(std::lround(m));
    cents = (m - n) * 100.0;
    return n;
}

} // namespace

SynthScope::SynthScope(PhospheneProcessor& proc, Kind kind) : proc_(proc), kind_(kind)
{
    setOpaque(false);
    startTimerHz(10);
}

SynthScope::~SynthScope() { stopTimer(); }

void SynthScope::refresh()
{
    const ParamStore& ps = proc_.params();
    const Module m = kind_ == Kind::Kick ? Module::Kick : Module::Bass;
    const int base = ps.base(m), n = ParamStore::moduleCount(m);
    // What plays: the engine's effective values while the set runs (the knobs plus the track's recipe); stopped, the
    // knobs as they stand.
    const TransportView t = proc_.transport();
    const bool live = t.playing && !t.restarting;
    std::vector<float> v(static_cast<size_t>(n));
    for (int k = 0; k < n; ++k) v[static_cast<size_t>(k)] = live ? proc_.engine().effective(base + k) : ps.get(base + k);
    const int key = live ? proc_.engine().keyRoot()
                         : static_cast<int>(std::lround(ps.get(ps.base(Module::Compose) + compose::Key)));
    const double bpm = t.bpm > 1.0 ? t.bpm : 145.0;
    std::vector<float> sig = v;
    sig.push_back(static_cast<float>(key));
    sig.push_back(static_cast<float>(bpm));
    if (sig == seen_) return;
    seen_ = sig;
    render(v, key, bpm);
    repaint();
}

void SynthScope::render(const std::vector<float>& v, int key, double bpm)
{
    const double eighth = 30.0 / bpm;   // one eighth note, seconds
    seconds_ = kind_ == Kind::Kick ? 0.6 : 2.0 * eighth;
    const int len = static_cast<int>(seconds_ * kSr);
    wave_.assign(static_cast<size_t>(len), 0.0f);
    curveHz_.assign(kColumns, 0.0f);
    if (kind_ == Kind::Kick) {
        Kick k;
        k.prepare(kSr);
        k.update(v.data(), key);
        k.trigger(1.0f);
        k.process(wave_.data(), len);
        for (int c = 0; c < kColumns; ++c) curveHz_[static_cast<size_t>(c)] = static_cast<float>(k.frequencyAt(seconds_ * c / (kColumns - 1)));
        startHz_ = k.frequencyAt(0.0);
        landHz_ = k.tunedEndHz();
        note_ = -1;
    } else {
        Bass b;
        b.prepare(kSr);
        b.update(v.data());
        const int pitch = bassRootNote(key, 0);
        // One eighth note, released at its end, as a rolling bassline plays it; the second eighth is its tail.
        b.noteOn(pitch, 1.0f, static_cast<int>(eighth * kSr), 0.0, 0.0);
        b.process(wave_.data(), len);
        // The cutoff as Bass::process computes it (Bass.cpp): cutoff x 2^(env amount x envelope + key tracking),
        // the envelope falling to 1/1000 over the filter decay; velocity 1.
        const double cutoff = v[bass::Cutoff], envOct = v[bass::EnvAmount], track = v[bass::KeyTrack] * (pitch - 28) / 12.0;
        const double decay = std::max(1.0e-3, static_cast<double>(v[bass::FilterDecay]) * 0.001);
        for (int c = 0; c < kColumns; ++c) {
            const double tt = seconds_ * c / (kColumns - 1);
            const double env = std::exp(std::log(1.0e-3) * tt / decay);
            curveHz_[static_cast<size_t>(c)] = static_cast<float>(std::clamp(cutoff * std::pow(2.0, envOct * env + track), 20.0, 0.45 * kSr));
        }
        startHz_ = curveHz_.front();
        landHz_ = midiToHz(pitch);
        note_ = pitch;
    }
    peak_ = 0.0f;
    int last = 0;
    for (int i = 0; i < len; ++i) {
        const float a = std::fabs(wave_[static_cast<size_t>(i)]);
        peak_ = std::max(peak_, a);
    }
    for (int i = 0; i < len; ++i) if (std::fabs(wave_[static_cast<size_t>(i)]) > peak_ * 1.0e-3f) last = i;   // -60 dB
    lengthMs_ = 1000.0 * last / kSr;

    // The spectrum: one Hann-windowed FFT over the rendered sound, read at log-spaced frequencies.
    const int nfft = 32768;
    std::vector<std::complex<double>> x(static_cast<size_t>(nfft));
    for (int i = 0; i < nfft && i < len; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * 3.141592653589793 * i / std::max(1, len - 1));
        x[static_cast<size_t>(i)] = std::complex<double>(wave_[static_cast<size_t>(i)] * w, 0.0);
    }
    Fft(nfft).transform(x, false);
    spectrumDb_.assign(kColumns, -120.0f);
    double top = 1.0e-30;
    std::vector<double> mag(static_cast<size_t>(nfft / 2));
    for (int i = 0; i < nfft / 2; ++i) { mag[static_cast<size_t>(i)] = std::norm(x[static_cast<size_t>(i)]); top = std::max(top, mag[static_cast<size_t>(i)]); }
    for (int c = 0; c < kColumns; ++c) {
        const double f0 = kLoHz * std::pow(kHiHz / kLoHz, static_cast<double>(c) / kColumns);
        const double f1 = kLoHz * std::pow(kHiHz / kLoHz, static_cast<double>(c + 1) / kColumns);
        const int b0 = std::clamp(static_cast<int>(f0 * nfft / kSr), 0, nfft / 2 - 1);
        const int b1 = std::clamp(static_cast<int>(f1 * nfft / kSr), b0, nfft / 2 - 1);
        double mx = 0.0;
        for (int b = b0; b <= b1; ++b) mx = std::max(mx, mag[static_cast<size_t>(b)]);
        spectrumDb_[static_cast<size_t>(c)] = static_cast<float>(10.0 * std::log10(mx / top + 1.0e-12));
    }
}

void SynthScope::paint(juce::Graphics& g)
{
    if (wave_.empty()) refresh();
    const juce::Colour tint = kind_ == Kind::Kick ? partColour(2) : partColour(3);
    auto area = getLocalBounds().toFloat().reduced(4.0f);
    g.setColour(bg0);
    g.fillRoundedRectangle(area, 6.0f);
    auto wavePanel = area.removeFromLeft(area.getWidth() * 0.62f).reduced(8.0f, 8.0f);
    auto specPanel = area.reduced(8.0f, 8.0f);

    // ---- the waveform, and the pitch or cutoff curve over it
    auto header = wavePanel.removeFromTop(16.0f);
    g.setFont(body(11.5f));
    g.setColour(dim);
    double cents = 0.0;
    juce::String info;
    if (kind_ == Kind::Kick) {
        const int nn = nearestNote(landHz_, cents);
        info << "pitch " << whole(startHz_) << " > " << juce::String(landHz_, 1) << " Hz  ("
             << noteName(nn) << (std::fabs(cents) >= 1.0 ? juce::String(" ") + (cents > 0 ? "+" : "") + juce::String(juce::roundToInt(cents)) + " ct" : juce::String())
             << ")   length " << whole(lengthMs_) << " ms   peak " << juce::String(20.0 * std::log10(std::max(1.0e-6f, peak_)), 1) << " dB";
    } else {
        info << "note " << noteName(note_) << " (" << juce::String(landHz_, 1) << " Hz)   filter " << whole(startHz_) << " > "
             << whole(curveHz_.empty() ? 0.0f : curveHz_.back()) << " Hz   peak "
             << juce::String(20.0 * std::log10(std::max(1.0e-6f, peak_)), 1) << " dB";
    }
    g.drawText(info, header, juce::Justification::centredLeft, true);
    g.setColour(edge);
    g.drawRect(wavePanel, 1.0f);
    if (!wave_.empty()) {
        const float mid = wavePanel.getCentreY(), half = wavePanel.getHeight() * 0.46f;
        const float scale = peak_ > 1.0e-6f ? half / peak_ : 0.0f;
        juce::Path fill;
        const int cols = juce::jmax(2, static_cast<int>(wavePanel.getWidth()));
        const size_t per = juce::jmax<size_t>(1, wave_.size() / static_cast<size_t>(cols));
        fill.startNewSubPath(wavePanel.getX(), mid);
        std::vector<float> lo(static_cast<size_t>(cols)), hi(static_cast<size_t>(cols));
        for (int c = 0; c < cols; ++c) {
            float a = 0.0f, b = 0.0f;
            for (size_t i = static_cast<size_t>(c) * per; i < std::min(wave_.size(), (static_cast<size_t>(c) + 1) * per); ++i) {
                a = std::min(a, wave_[i]);
                b = std::max(b, wave_[i]);
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
        const double top = kind_ == Kind::Kick ? 2000.0 : 20000.0;
        auto yOf = [&](double hz) {
            const double t = std::log(std::clamp(hz, kLoHz, top) / kLoHz) / std::log(top / kLoHz);
            return wavePanel.getBottom() - static_cast<float>(t) * wavePanel.getHeight();
        };
        juce::Path curve;
        for (int c = 0; c < kColumns; ++c) {
            const float x = wavePanel.getX() + wavePanel.getWidth() * c / (kColumns - 1);
            const float y = yOf(curveHz_[static_cast<size_t>(c)]);
            if (c == 0) curve.startNewSubPath(x, y); else curve.lineTo(x, y);
        }
        g.setColour(text.withAlpha(0.9f));
        g.strokePath(curve, juce::PathStrokeType(1.6f));
        g.setColour(faint);
        g.setFont(body(10.0f));
        for (double hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 5000.0 }) {
            if (hz > top) continue;
            g.drawText(hz >= 1000.0 ? whole(hz / 1000.0) + "k" : whole(hz), juce::Rectangle<float>(wavePanel.getRight() - 30.0f, yOf(hz) - 6.0f, 28.0f, 12.0f),
                       juce::Justification::centredRight, false);
        }
        g.drawText(kind_ == Kind::Kick ? "pitch" : "cutoff", juce::Rectangle<float>(wavePanel.getX() + 4.0f, wavePanel.getY() + 2.0f, 60.0f, 12.0f), juce::Justification::centredLeft, false);
        g.drawText(whole(seconds_ * 1000.0) + " ms", juce::Rectangle<float>(wavePanel.getRight() - 70.0f, wavePanel.getBottom() - 14.0f, 66.0f, 12.0f),
                   juce::Justification::centredRight, false);
    }

    // ---- the spectrum
    auto specHead = specPanel.removeFromTop(16.0f);
    g.setColour(dim);
    g.setFont(body(11.5f));
    g.drawText("spectrum", specHead, juce::Justification::centredLeft, false);
    g.setColour(edge);
    g.drawRect(specPanel, 1.0f);
    if (!spectrumDb_.empty()) {
        juce::Path sp;
        auto yDb = [&](float db) { return specPanel.getY() + juce::jlimit(0.0f, 1.0f, -db / 72.0f) * specPanel.getHeight(); };
        sp.startNewSubPath(specPanel.getX(), specPanel.getBottom());
        for (int c = 0; c < kColumns; ++c) sp.lineTo(specPanel.getX() + specPanel.getWidth() * c / (kColumns - 1), yDb(spectrumDb_[static_cast<size_t>(c)]));
        sp.lineTo(specPanel.getRight(), specPanel.getBottom());
        sp.closeSubPath();
        g.setColour(tint.withAlpha(0.45f));
        g.fillPath(sp);
        g.setColour(faint);
        g.setFont(body(10.0f));
        for (double hz : { 50.0, 100.0, 500.0, 1000.0, 5000.0 }) {
            const float x = specPanel.getX() + specPanel.getWidth() * static_cast<float>(std::log(hz / kLoHz) / std::log(kHiHz / kLoHz));
            g.drawVerticalLine(juce::roundToInt(x), specPanel.getY(), specPanel.getBottom());
            g.drawText(hz >= 1000.0 ? whole(hz / 1000.0) + "k" : whole(hz), juce::Rectangle<float>(x + 2.0f, specPanel.getBottom() - 13.0f, 30.0f, 12.0f),
                       juce::Justification::centredLeft, false);
        }
    }
}

} // namespace phosui
