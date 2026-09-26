/**
 * @file EditorFlow.cpp
 * @brief The signal flow as a picture (26.09.2026): the help page's first topic and the manual's figure, drawn by the
 *        plugin itself as Noctuary draws its own -- every unit a box in its family's colour, the buses as arrows.
 *
 * The boxes say what the engine does, in the order it does it (Engine.h, the file comment: "Signal flow"; Composer.h
 * for the plans). A fixed canvas, scaled to fit: the manual renders it at twice its size (writeManual, flow.png).
 */
#include "PluginEditor.h"

using namespace phosui;

void SignalFlow::paint(juce::Graphics& g)
{
    const float sc = juce::jmin(static_cast<float>(getWidth()) / kCanvasW, static_cast<float>(getHeight()) / kCanvasH);
    if (sc <= 0.05f) return;
    g.fillAll(bg0);
    g.addTransform(juce::AffineTransform::scale(sc));
    auto node = [&](float x, float y, float w, float h, const juce::String& t, juce::Colour c, float fs = 11.0f) {
        const juce::Rectangle<float> r(x, y, w, h);
        g.setColour(group);
        g.fillRoundedRectangle(r, 6.0f);
        g.setColour(c.withAlpha(0.9f));
        g.drawRoundedRectangle(r.reduced(0.5f), 6.0f, 1.2f);
        g.setColour(text);
        g.setFont(body(fs));
        g.drawFittedText(t, r.reduced(6.0f, 2.0f).toNearestInt(), juce::Justification::centred, 3, 0.85f);
        return r;
    };
    auto frame = [&](float x, float y, float w, float h, const juce::String& t, juce::Colour c) {
        g.setColour(c.withAlpha(0.35f));
        g.drawRoundedRectangle(x, y, w, h, 8.0f, 1.0f);
        g.setColour(c);
        g.setFont(title(10.0f));
        g.drawText(t, juce::roundToInt(x + 10), juce::roundToInt(y + 3), juce::roundToInt(w - 20), 14, juce::Justification::centredLeft, false);
    };
    auto arrow = [&](juce::Point<float> a, juce::Point<float> b, juce::Colour c, bool dashed = false) {
        g.setColour(c.withAlpha(0.85f));
        const juce::Line<float> l(a, b);
        if (dashed) {
            const float d[] = { 5.0f, 4.0f };
            juce::Path p;
            p.startNewSubPath(a);
            p.lineTo(b);
            juce::PathStrokeType(1.4f).createDashedStroke(p, p, d, 2);
            g.fillPath(p);
        } else {
            g.drawLine(l, 1.4f);
        }
        juce::Path head;
        const juce::Point<float> u = (b - a) / juce::jmax(1.0f, l.getLength());
        const juce::Point<float> n(-u.y, u.x);
        head.startNewSubPath(b);
        head.lineTo(b - u * 7.0f + n * 3.5f);
        head.lineTo(b - u * 7.0f - n * 3.5f);
        head.closeSubPath();
        g.fillPath(head);
    };
    auto label = [&](float x, float y, const juce::String& t, juce::Colour c) {
        g.setColour(c);
        g.setFont(body(10.0f));
        g.drawText(t, juce::roundToInt(x), juce::roundToInt(y), 700, 14, juce::Justification::centredLeft, false);
    };

    const juce::Colour setC = partColour(0), lowC = partColour(2), percC = partColour(4), acidC = partColour(5),
                       lineC = partColour(6), spaceC = partColour(10), mixC = partColour(13);

    // ---- the composer: what a set is before it sounds
    node(20, 14, 960, 30, "COMPOSER   seed  ->  set arc (energy, style morph)  ->  tracks: key, tempo, style, form "
                          "(intro, grooves, buildups, breakdowns, drops, outro), DJ overlap", setC, 10.5f);
    const auto score = node(20, 56, 310, 42, "Score\nmelody (genre rules + learned model), bass rhythm,\npercussion grooves and fills, effect events", setC, 9.5f);
    const auto sound = node(345, 56, 310, 42, "Sound\na preset per synth and track (9216 of them),\nsection rides, drop 2's lift", setC, 9.5f);
    const auto mix = node(670, 56, 310, 42, "Mix\nlevel, presence and audibility match\n(probe renders), Auto Gain", setC, 9.5f);
    for (const auto* r : { &score, &sound, &mix }) arrow({ r->getCentreX(), 44.0f }, { r->getCentreX(), 56.0f }, setC);
    node(20, 110, 960, 26, "Conductor: notes and control events into the engine, sample-accurate on an absolute 32-sample grid", setC, 10.0f);
    for (const auto* r : { &score, &sound, &mix }) arrow({ r->getCentreX(), r->getBottom() }, { r->getCentreX(), 110.0f }, setC);

    // ---- the generators
    frame(14, 146, 464, 280, "GENERATORS", lineC);
    const auto kick = node(22, 166, 140, 38, "Kick\nsweep or resonator, tuned", lowC, 9.5f);
    const auto bass = node(170, 166, 140, 38, "Bass\noscillator + sub, kick lock", lowC, 9.5f);
    const auto perc = node(318, 166, 152, 38, "Percussion\n12 lanes, 5 engines", percC, 9.5f);
    const auto acid = node(22, 212, 448, 30, "Acid: 303 diode ladder, accent sweep, squelch comb, disperser, delay", acidC, 9.5f);
    frame(22, 250, 448, 118, "SIX VOICES   lead - counter - arp - stab - pad - drone", lineC);
    node(30, 270, 432, 26, "Supersaw, VA, FM or wavetable  +  a second oscillator", lineC, 9.5f);
    node(30, 302, 432, 26, "State-variable filter or nine circuit models (Moog ... Wasp), at twice the rate", lineC, 9.5f);
    const auto voiceEnd = node(30, 334, 432, 26, "Amp, filter and mod envelopes  -  voice FX  -  delay", lineC, 9.5f);
    const auto sfx = node(22, 376, 448, 40, "SFX, bed and voices\nrisers, zips, impacts, the sub drop  -  a shamanic texture  -  spoken phrases", spaceC, 9.5f);
    arrow({ 500.0f, 136.0f }, { 500.0f, 146.0f }, setC);

    // ---- the strips and the space
    const auto strip = node(500, 166, 480, 38, "Channel strip per part: level  -  the kick's sidechain duck  -\n"
                                               "trance gate  -  distance (level, low pass, width)", mixC, 9.5f);
    arrow({ kick.getRight(), 185.0f }, { 170.0f, 185.0f }, lowC);   // the kick lock ties kick and bass
    arrow({ perc.getRight(), 185.0f }, { 500.0f, 185.0f }, percC);
    arrow({ acid.getRight(), acid.getCentreY() }, { 500.0f, 198.0f }, acidC);
    arrow({ voiceEnd.getRight(), voiceEnd.getCentreY() }, { 500.0f, 200.0f }, lineC);
    arrow({ sfx.getRight(), sfx.getCentreY() }, { 500.0f, 202.0f }, spaceC);
    // Sends and effects in one frame: every strip sends into it, and its returns join the master.
    frame(494, 212, 492, 98, "SPACE AND EFFECTS   sends and returns", spaceC);
    const auto room = node(500, 232, 110, 32, "Room\nthe near plane", spaceC, 9.0f);
    const auto plate = node(620, 232, 110, 32, "Plate\nthe middle plane", spaceC, 9.0f);
    const auto hall = node(740, 232, 110, 32, "Hall\nthe far plane", spaceC, 9.0f);
    const auto gated = node(860, 232, 120, 32, "Gated hall\nper voice (hall gate)", spaceC, 9.0f);
    arrow({ plate.getRight(), 248.0f }, { hall.getX(), 248.0f }, spaceC);
    const auto fx = node(500, 272, 230, 32, "PsyFx: flanger, phaser, shifter\n(the SFX insert; bed and voices send)", spaceC, 9.0f);
    const auto stut = node(740, 272, 240, 32, "Stutter: a slice of the melodic bus\nin place of the live signal", accent, 9.0f);
    arrow({ 740.0f, strip.getBottom() }, { 740.0f, 212.0f }, spaceC);

    // ---- the master and what leaves the plugin
    const auto master = node(500, 318, 480, 50, "MASTER   sum  ->  gain (knob + level match + loudness)  ->  bus compressor  ->\n"
                                                "mono bass  ->  soft clipper  ->  band limit  ->  true-peak limiter  ->  safety clip", mixC, 9.5f);
    arrow({ 740.0f, 310.0f }, { 740.0f, master.getY() }, spaceC);
    const auto meter = node(500, 378, 235, 36, "BS.1770 meter\nLUFS, loudness range, true peak", mixC, 9.0f);
    const auto out = node(745, 378, 235, 36, "Out: audio  -  stems  -  MIDI (the score)\nOSC cues (bars, sections, drops)", mixC, 9.0f);
    arrow({ meter.getCentreX(), master.getBottom() }, { meter.getCentreX(), meter.getY() }, mixC);
    arrow({ out.getCentreX(), master.getBottom() }, { out.getCentreX(), out.getY() }, mixC);

    // ---- modulation, and the live ring that shows it
    const auto mod = node(20, 440, 960, 34, "Modulation per voice: mod and filter envelopes, 4 LFOs (free or synced), velocity, key, random  ->  "
                                            "8-slot matrix  ->  pitch, cutoff, resonance, table position, FM index, pulse width, filter mode, level, pan",
                          lineC, 9.5f);
    // Up the left margin into the voices, clear of every box.
    g.setColour(lineC.withAlpha(0.85f));
    {
        const float d[] = { 5.0f, 4.0f };
        juce::Path p;
        p.startNewSubPath(18.0f, mod.getY());
        p.lineTo(18.0f, 309.0f);
        juce::PathStrokeType(1.4f).createDashedStroke(p, p, d, 2);
        g.fillPath(p);
    }
    arrow({ 18.0f, 309.0f }, { 30.0f, 309.0f }, lineC);
    label(500, 420, "the editor's live ring shows every value as it plays: the composer's rides, corrections and the modulation", dim);
    juce::ignoreUnused(bass, stut, fx, room, hall, gated);
}

juce::Rectangle<int> SignalFlow::drawn() const
{
    const float sc = juce::jmin(static_cast<float>(getWidth()) / kCanvasW, static_cast<float>(getHeight()) / kCanvasH);
    return { 0, 0, juce::roundToInt(kCanvasW * sc), juce::roundToInt(kCanvasH * sc) };
}
