/**
 * @file EditorMixer.h
 * @brief The mixer page's console (24.09.2026): a channel strip per part, with a meter, a fader and the part's sends.
 *
 * The user: "Koennen wir im Mixer-Tab Meter fuer das Level fuer die einzelnen Kanalzuege anzeigen anstatt einfacher
 * Drehknoepfe? Vielleicht sogar ganze Channel-Strips?" Until then the Mix tab was a table slice of mutes and level
 * knobs, and the sends of a part stood on its own synth page. A strip here holds, top to bottom, what a console
 * strip holds: the part's pan (where it has one), its sends to the room, the hall and its delay or effects bus, its
 * duck under the kick, the mute, and the fader beside the meter. Every control is the same host parameter its synth
 * page shows -- two views of one value, no copy.
 *
 * The meter reads what the part puts into the mix (phos::Engine::takeMeters: the stem tap's signal, after fader,
 * trance gate and duck, before the master), as an RMS bar with a 300 ms release and a peak line that holds for a
 * second and a half and then falls at 20 dB a second: the body of the level and its peaks, as a console shows them.
 */
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include <array>
#include <memory>
#include <vector>

namespace phosui {

/** @brief One strip: name, knobs, mute, fader and meter of one part. */
class MixerStrip final : public juce::Component {
public:
    /** @param levelKey the fader's parameter; @param muteKey the mute; @param knobKeys the knobs, top to bottom */
    MixerStrip(PhospheneProcessor& proc, const juce::String& name, juce::Colour colour, const char* levelKey,
               const char* muteKey, const std::vector<std::pair<const char*, const char*>>& knobKeys);

    /** @brief A new meter reading (linear peak and RMS since the last one) and the time it covers. */
    void meter(float peak, float rms, double seconds);
    /** @brief The meter as it stands, in dB: the RMS bar and the held peak. */
    float rmsDb() const { return rmsDb_; }
    float peakDb() const { return holdDb_; }   ///< @copydoc rmsDb
    /** @brief The global ids of the parameters on this strip. */
    const std::vector<int>& params() const { return params_; }
    /** @brief The strip's controls with the parameter each drives, for MIDI learn. */
    const std::vector<std::pair<juce::Component*, int>>& controls() const { return controls_; }

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::String name_;
    juce::Colour colour_;
    std::unique_ptr<juce::Slider> fader_;
    std::unique_ptr<juce::SliderParameterAttachment> faderLink_;
    std::unique_ptr<juce::TextButton> mute_;
    std::unique_ptr<juce::ButtonParameterAttachment> muteLink_;
    std::vector<std::unique_ptr<juce::Slider>> knobs_;
    std::vector<std::unique_ptr<juce::Label>> knobNames_;
    std::vector<std::unique_ptr<juce::SliderParameterAttachment>> knobLinks_;
    std::vector<int> params_;
    std::vector<std::pair<juce::Component*, int>> controls_;
    juce::Rectangle<int> meterArea_;
    float rmsDb_ = -100.0f, holdDb_ = -100.0f, lastPeakDb_ = -100.0f;
    double holdAge_ = 0.0;
};

/** @brief The console: one MixerStrip per part, in the parts' order, fed by a 30 Hz timer. */
class MixerConsole final : public juce::Component, private juce::Timer {
public:
    explicit MixerConsole(PhospheneProcessor& proc);
    ~MixerConsole() override;

    /** @brief The strips, phos::Part order. */
    int stripCount() const { return static_cast<int>(strips_.size()); }
    MixerStrip& strip(int i) { return *strips_[static_cast<size_t>(i)]; }   ///< @copydoc stripCount
    /** @brief Every parameter the console binds. */
    std::vector<int> params() const;
    /** @brief Reads the meters now instead of waiting for the timer (the host test). */
    void pollMeters() { timerCallback(); }

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    /** @brief Takes the processor's readings and hands them to the strips. */
    void timerCallback() override;
    PhospheneProcessor& proc_;
    std::vector<std::unique_ptr<MixerStrip>> strips_;
    double lastPoll_ = 0.0;
};

} // namespace phosui
