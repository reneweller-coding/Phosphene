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
    /** @param proc the processor whose parameters the strip edits; @param name the strip's caption;
     *  @param colour its accent colour; @param levelKey the fader's parameter; @param muteKey the mute;
     *  @param knobKeys the knobs, top to bottom */
    MixerStrip(PhospheneProcessor& proc, const juce::String& name, juce::Colour colour, const char* levelKey,
               const char* muteKey, const std::vector<std::pair<const char*, const char*>>& knobKeys);

    /** @brief A new meter reading (linear peak and RMS since the last one) and the time it covers. */
    void meter(float peak, float rms, double seconds);
    /** @brief The meter as it stands, in dB: the RMS bar and the held peak. */
    float rmsDb() const { return m_rmsDb; }
    float peakDb() const { return m_holdDb; }   ///< @copydoc rmsDb
    /** @brief The global ids of the parameters on this strip. */
    const std::vector<int>& params() const { return m_params; }
    /** @brief The strip's controls with the parameter each drives, for MIDI learn. */
    const std::vector<std::pair<juce::Component*, int>>& controls() const { return m_controls; }
    /** @brief Rows of knobs this strip's own knobs fill (two to a row). */
    int knobRows() const { return (static_cast<int>(m_knobs.size()) + 1) / 2; }
    /**
     * @brief Rows of knobs to make room for, at least knobRows() (26.09.2026, the user: "dass alle Meter die gleiche
     *        Höhe haben"). The console gives every strip the rows of its fullest, so that mute, fader and meter
     *        start at the same height in every strip; a strip with fewer knobs leaves the rest of its rows empty.
     */
    void setKnobRows(int rows) { m_knobRowsShown = rows; }
    /** @brief Where the meter is drawn, in the strip's coordinates (the host test compares them). */
    juce::Rectangle<int> meterArea() const { return m_meterArea; }

    /** @brief The name, the meter (RMS, the held peak) and its scale. */
    void paint(juce::Graphics&) override;
    /** @brief The knobs, the mute, the fader and the meter. */
    void resized() override;

private:
    juce::String m_name;   ///< the part's name
    juce::Colour m_colour;   ///< its page's colour
    std::unique_ptr<juce::Slider> m_fader;   ///< the level fader
    std::unique_ptr<juce::SliderParameterAttachment> m_faderLink;   ///< its host link
    std::unique_ptr<juce::TextButton> m_mute;   ///< the mute
    std::unique_ptr<juce::ButtonParameterAttachment> m_muteLink;   ///< its host link
    std::vector<std::unique_ptr<juce::Slider>> m_knobs;   ///< pan, sends and duck
    std::vector<std::unique_ptr<juce::Label>> m_knobNames;   ///< their names
    std::vector<std::unique_ptr<juce::SliderParameterAttachment>> m_knobLinks;   ///< their host links
    std::vector<int> m_params;   ///< params()
    std::vector<std::pair<juce::Component*, int>> m_controls;   ///< controls()
    juce::Rectangle<int> m_meterArea;   ///< where the meter is drawn
    int m_knobRowsShown = 0;   ///< setKnobRows()
    float m_rmsDb = -100.0f;   ///< the RMS bar, dB
    float m_holdDb = -100.0f;   ///< the held peak, dB
    double m_holdAge = 0.0;   ///< seconds the peak has been held
};

/** @brief The console: one MixerStrip per part, in the parts' order, fed by a 30 Hz timer. */
class MixerConsole final : public juce::Component, private juce::Timer {
public:
    /** @brief Builds a strip per part and starts the meter timer. */
    explicit MixerConsole(PhospheneProcessor& proc);
    ~MixerConsole() override;

    /** @brief The strips, phos::Part order. */
    int stripCount() const { return static_cast<int>(m_strips.size()); }
    MixerStrip& strip(int i) { return *m_strips[static_cast<size_t>(i)]; }   ///< @copydoc stripCount
    /** @brief Every parameter the console binds. */
    std::vector<int> params() const;
    /** @brief Reads the meters now instead of waiting for the timer (the host test). */
    void pollMeters() { timerCallback(); }

    /** @brief The console's background. */
    void paint(juce::Graphics&) override;
    /** @brief The strips side by side. */
    void resized() override;

private:
    /** @brief Takes the processor's readings and hands them to the strips. */
    void timerCallback() override;
    PhospheneProcessor& m_proc;   ///< the processor whose meters it reads
    std::vector<std::unique_ptr<MixerStrip>> m_strips;   ///< one strip per part
    double m_lastPoll = 0.0;   ///< when the meters were last read, seconds
};

} // namespace phosui
