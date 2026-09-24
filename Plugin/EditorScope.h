/**
 * @file EditorScope.h
 * @brief The kick's and the bass's scope (24.09.2026): what one hit or one note of the synth sounds like, drawn.
 *
 * The user: "Koennten wir bei der Kick und beim Bass noch Anzeigen einbauen, wie in (Kick 3 von Sonic Academy)."
 * Kick 3 draws the kick it will play -- its waveform, its pitch sweep and the note it lands on -- next to the knobs
 * that shape it. This does the same for both low-end synths: it renders one hit (the kick) or one note (the bass, on
 * the key's root) offline through the synth's own code, with the values the engine plays right now (the knobs plus
 * the track's recipe, Engine::effective), and draws
 *
 *  - the waveform, filled, over its first 600 ms (the kick) or one eighth note at the set's tempo (the bass);
 *  - the kick's pitch curve (Kick::frequencyAt) or the bass's filter cutoff (Bass.cpp's own formula), on a log axis;
 *  - the spectrum of the rendered sound, 20 Hz to 20 kHz;
 *  - the figures that matter on a floor: start and end pitch with the note the kick lands on, its length to -60 dB,
 *    and for the bass the note and the filter's travel.
 *
 * It renders only when a value changed (a comparison of the synth's effective values every tick), which is a few
 * hundred microseconds for a kick; nothing of it touches the audio thread.
 */
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include <vector>

namespace phosui {

/** @brief One synth's scope. */
class SynthScope final : public juce::Component, private juce::Timer {
public:
    enum class Kind { Kick, Bass };
    SynthScope(PhospheneProcessor& proc, Kind kind);
    ~SynthScope() override;

    /** @brief Renders again now if anything changed (the timer does this ten times a second). */
    void refresh();
    /** @brief The peak of the rendered sound, and the frequency the kick ends on / the bass note's (tests). */
    float renderedPeak() const { return peak_; }
    double landingHz() const { return landHz_; }   ///< @copydoc renderedPeak

    void paint(juce::Graphics&) override;

private:
    void timerCallback() override { if (isShowing()) refresh(); }
    void render(const std::vector<float>& v, int key, double bpm);

    PhospheneProcessor& proc_;
    Kind kind_;
    std::vector<float> seen_;          ///< the values (and key, tempo) the picture was made from
    std::vector<float> wave_;          ///< the rendered sound
    std::vector<float> curveHz_;       ///< pitch (kick) or cutoff (bass) per display column's time
    std::vector<float> spectrumDb_;    ///< per log-frequency column
    double sr_ = 48000.0, seconds_ = 0.6;
    float peak_ = 0.0f;
    double startHz_ = 0.0, landHz_ = 0.0, lengthMs_ = 0.0;
    int note_ = -1;
};

} // namespace phosui
