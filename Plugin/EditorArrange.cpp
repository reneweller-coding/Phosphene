/**
 * @file EditorArrange.cpp
 * @brief The arrange timeline (EditorArrange.h) and the Arrange tab that carries it.
 */
#include "EditorArrange.h"
#include "PhospheneLookAndFeel.h"
#include "PluginEditor.h"
#include <cmath>

using namespace phos;
using namespace phosui;

namespace {

constexpr int StripH = 58;      ///< the set-wide bar at the top
constexpr int HeaderW = 176;    ///< the part of a track row that names the track
constexpr int LegendH = 15;     ///< the line at the bottom that says what a click does
constexpr int Glyph = 13;       ///< side of the lock and reroll buttons
constexpr int LaneGap = 3;      ///< between the strip's two lanes (odd and even tracks)

/** @brief The colour of a section type: warm where the energy is, cold where it is taken away. */
juce::Colour sectionColour(SectionTypeE t)
{
    switch (t) {
    case SectionTypeE::INTRO:  return juce::Colour(0xff5d6683);
    case SectionTypeE::GROOVE: return juce::Colour(0xff3f9d7a);
    case SectionTypeE::BUILD:  return juce::Colour(0xffd9a441);
    case SectionTypeE::DROP:   return juce::Colour(0xffff6b6b);
    case SectionTypeE::BREAK:  return juce::Colour(0xff6a7fd6);
    case SectionTypeE::OUTRO:  return juce::Colour(0xff4d5670);
    case SectionTypeE::PDB:    return juce::Colour(0xffb06bd8);
    case SectionTypeE::CUT:    return juce::Colour(0xff2c3244);
    default: return Faint;
    }
}

/** @brief A padlock, open or shut, in the box it is given. */
void drawLock(juce::Graphics& g, juce::Rectangle<int> r, bool locked)
{
    const juce::Rectangle<float> box = r.toFloat().reduced(1.5f);
    const float w = box.getWidth(), h = box.getHeight();
    const juce::Colour c = locked ? Accent : Faint;
    g.setColour(c);
    // The shackle: a half ring over the body, leaning open when the lock is not set.
    juce::Path shackle;
    const float sx = box.getX() + w * (locked ? 0.26f : 0.40f);
    shackle.addCentredArc(sx + w * 0.24f, box.getY() + h * 0.36f, w * 0.24f, h * 0.26f, 0.0f,
                          -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
    g.strokePath(shackle, juce::PathStrokeType(1.3f));
    const juce::Rectangle<float> body(box.getX() + w * 0.16f, box.getY() + h * 0.46f, w * 0.68f, h * 0.44f);
    if (locked) g.fillRoundedRectangle(body, 1.6f);
    else g.drawRoundedRectangle(body, 1.6f, 1.1f);
}

/** @brief A die: the reroll button. */
void drawDie(juce::Graphics& g, juce::Rectangle<int> r, bool used)
{
    const juce::Rectangle<float> box = r.toFloat().reduced(1.5f);
    g.setColour(used ? Accent : Faint);
    g.drawRoundedRectangle(box, 2.0f, 1.1f);
    const float d = juce::jmax(1.4f, box.getWidth() * 0.16f);
    g.fillEllipse(box.getX() + box.getWidth() * 0.28f - d * 0.5f, box.getY() + box.getHeight() * 0.30f - d * 0.5f, d, d);
    g.fillEllipse(box.getCentreX() - d * 0.5f, box.getCentreY() - d * 0.5f, d, d);
    g.fillEllipse(box.getX() + box.getWidth() * 0.72f - d * 0.5f, box.getY() + box.getHeight() * 0.70f - d * 0.5f, d, d);
}

} // namespace

// ==================================================================== ArrangeDisplay

ArrangeDisplay::ArrangeDisplay() { setInterceptsMouseClicks(true, false); }

void ArrangeDisplay::setSnapshot(Snapshot s)
{
    m_snap = std::move(s);
    measure();
    render();
    repaint();
}

bool ArrangeDisplay::setPosition(double musicalBeat)
{
    m_beat = musicalBeat;
    const int px = m_strip.isEmpty() ? -1 : juce::roundToInt(stripX(musicalBeat / BeatsPerBar));
    if (px == m_headPixel) return false;
    m_headPixel = px;
    repaint();
    return true;
}

void ArrangeDisplay::resized()
{
    measure();
    render();
}

std::pair<int, int> ArrangeDisplay::overlapOf(size_t i) const
{
    // From the plans themselves rather than from DjOverlap: the first track of a set has no overlap
    // in front of it, and the last planned one none behind it until its successor is planned.
    const auto& ts = m_snap.m_tracks;
    if (i >= ts.size()) return { 0, 0 };
    auto shared = [&](size_t a, size_t b) {
        return juce::jmax(0, ts[a].m_firstBar + ts[a].m_bars - ts[b].m_firstBar);
    };
    return { i > 0 ? shared(i - 1, i) : 0, i + 1 < ts.size() ? shared(i, i + 1) : 0 };
}

float ArrangeDisplay::stripX(double bar) const
{
    const double t = juce::jlimit(0.0, 1.0, bar / juce::jmax(1, m_totalBars));
    return m_strip.getX() + static_cast<float>(t) * m_strip.getWidth();
}

void ArrangeDisplay::measure()
{
    m_hits.clear();
    m_rowArea.clear();
    m_blocks.clear();
    juce::Rectangle<int> r = getLocalBounds().reduced(6, 4);
    if (r.getWidth() < 40 || r.getHeight() < 40) { m_strip = {}; return; }
    if (!m_stripOnly) r.removeFromBottom(LegendH);
    m_strip = m_stripOnly ? r.withTrimmedTop(13) : r.removeFromTop(StripH).withTrimmedTop(13);
    r.removeFromTop(6);

    m_totalBars = juce::jmax(1, m_snap.m_bars);
    const int n = static_cast<int>(m_snap.m_tracks.size());
    if (n == 0) return;

    // The set strip: every track a block in its lane, clickable at its first bar. Two lanes, because
    // neighbouring tracks share DjOverlap bars: in one lane the later block hid the end of the earlier.
    m_blocks.clear();
    const int laneH = (m_strip.getHeight() - LaneGap) / 2;
    for (size_t i = 0; i < m_snap.m_tracks.size(); ++i) {
        const Trk& t = m_snap.m_tracks[i];
        const int x0 = juce::roundToInt(stripX(t.m_firstBar)), x1 = juce::roundToInt(stripX(t.m_firstBar + t.m_bars));
        const int y = m_strip.getY() + (i % 2 == 0 ? 0 : laneH + LaneGap);
        m_blocks.emplace_back(x0, y, juce::jmax(2, x1 - x0), laneH);
        Hit h;
        h.m_area = m_blocks.back();
        h.m_bar = t.m_firstBar;
        h.m_action = 0;
        m_hits.push_back(h);
    }

    if (m_stripOnly) return;   // the overview: the strip is all
    // The rows. They share what is left; a row never grows past a comfortable height, so a short
    // set does not draw nine fat bars, and never shrinks below what a section label needs.
    const int rowH = juce::jlimit(20, 44, r.getHeight() / n);
    for (int i = 0; i < n && r.getHeight() >= rowH; ++i) {
        const Trk& t = m_snap.m_tracks[static_cast<size_t>(i)];
        juce::Rectangle<int> row = r.removeFromTop(rowH);
        juce::Rectangle<int> head = row.removeFromLeft(HeaderW);
        // The track's own lock and reroll sit at the end of its name.
        juce::Rectangle<int> buttons = head.removeFromRight(2 * Glyph + 8).reduced(2, (rowH - Glyph) / 2);
        Hit lock;
        lock.m_area = buttons.removeFromLeft(Glyph);
        lock.m_unit = LockUnitE::TRACK;
        lock.m_index = t.m_index;
        lock.m_action = 1;
        lock.m_locked = t.m_locked;
        m_hits.push_back(lock);
        buttons.removeFromLeft(4);
        Hit die = lock;
        die.m_area = buttons.removeFromLeft(Glyph);
        die.m_action = 2;
        m_hits.push_back(die);

        m_rowArea.push_back(row);
        const double barW = static_cast<double>(row.getWidth()) / juce::jmax(1, t.m_bars);
        for (const Sec& s : t.m_sections) {
            const int x0 = row.getX() + static_cast<int>((s.m_bar - t.m_firstBar) * barW);
            const int x1 = row.getX() + static_cast<int>((s.m_bar - t.m_firstBar + s.m_bars) * barW);
            const juce::Rectangle<int> box(x0, row.getY(), juce::jmax(2, x1 - x0), row.getHeight());
            Hit seek;
            seek.m_area = box;
            seek.m_bar = s.m_bar;
            seek.m_action = 0;
            m_hits.push_back(seek);
            if (box.getWidth() < 3 * Glyph) continue;   // no room for the two buttons: seeking only
            Hit sl;
            sl.m_area = juce::Rectangle<int>(box.getX() + 2, box.getY() + 1, Glyph, juce::jmin(Glyph, box.getHeight() - 2));
            sl.m_unit = LockUnitE::SECTION;
            sl.m_index = sectionUnitIndex(t.m_index, s.m_index);
            sl.m_action = 1;
            sl.m_locked = s.m_locked;
            m_hits.push_back(sl);
            Hit sd = sl;
            sd.m_area = sl.m_area.withX(box.getRight() - Glyph - 2);
            sd.m_action = 2;
            m_hits.push_back(sd);
        }
    }
}

void ArrangeDisplay::render()
{
    const int w = getWidth(), h = getHeight();
    if (w <= 0 || h <= 0) return;
    m_cache = juce::Image(juce::Image::ARGB, w, h, true);
    juce::Graphics g(m_cache);
    g.setColour(Bg0.withAlpha(0.6f));
    g.fillRoundedRectangle(getLocalBounds().reduced(3, 2).toFloat(), 5.0f);

    juce::Rectangle<int> caption = getLocalBounds().reduced(6, 4).withHeight(13);
    g.setColour(Dim);
    g.setFont(title(10.0f));
    juce::String head = "ARRANGE";
    if (!m_snap.m_tracks.empty())
        head << "   " << juce::String(static_cast<int>(m_snap.m_tracks.size())) << " tracks   "
             << juce::String(m_snap.m_bars) << " bars planned";
    if (m_snap.m_minutes > 0) head << "   set " << juce::String(m_snap.m_minutes) << " min";
    g.drawText(head, caption, juce::Justification::topLeft, false);

    if (m_snap.m_tracks.empty()) {
        g.setColour(Faint);
        g.setFont(body(11.5f));
        g.drawText("planning...", getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    // ---------------------------------------------------------------- the set strip
    g.setColour(Edge.withAlpha(0.5f));
    g.fillRoundedRectangle(m_strip.toFloat(), 3.0f);
    // The overlaps first, under the blocks: a band over both lanes with a crossfade (the outgoing track
    // falling, the incoming one rising), so the eye reads the two lanes as one mix there.
    for (size_t i = 0; i + 1 < m_snap.m_tracks.size() && i + 1 < m_blocks.size(); ++i) {
        const int shared = overlapOf(i).second;
        if (shared <= 0) continue;
        const Trk& nx = m_snap.m_tracks[i + 1];
        const float x0 = stripX(nx.m_firstBar), x1 = stripX(nx.m_firstBar + shared);
        const juce::Rectangle<float> band(x0, static_cast<float>(m_strip.getY()), juce::jmax(1.0f, x1 - x0),
                                          static_cast<float>(m_strip.getHeight()));
        g.setColour(Text.withAlpha(0.07f));
        g.fillRect(band);
        const juce::Rectangle<float> out = m_blocks[i].toFloat(), in = m_blocks[i + 1].toFloat();
        g.setColour(Text.withAlpha(0.35f));
        g.drawLine(band.getX(), out.getCentreY(), band.getRight(), in.getCentreY(), 1.0f);
        g.drawLine(band.getX(), in.getCentreY(), band.getRight(), out.getCentreY(), 1.0f);
    }
    for (size_t i = 0; i < m_snap.m_tracks.size() && i < m_blocks.size(); ++i) {
        const Trk& t = m_snap.m_tracks[i];
        const juce::Rectangle<float> lane = m_blocks[i].toFloat();
        const juce::Rectangle<float> box = lane.withWidth(juce::jmax(2.0f, lane.getWidth() - 1.0f));
        g.setColour(cycleColour(static_cast<int>(i)).withAlpha(t.m_locked ? 0.42f : 0.26f));
        g.fillRoundedRectangle(box, 2.5f);
        // Every section of that track as a hairline of its own colour along the bottom: at this
        // scale a section is a few pixels, so it is a texture rather than a block.
        for (const Sec& s : t.m_sections) {
            const float sx0 = stripX(s.m_bar), sx1 = stripX(s.m_bar + s.m_bars);
            g.setColour(sectionColour(s.m_type).withAlpha(0.85f));
            g.fillRect(sx0, box.getBottom() - 4.0f, juce::jmax(1.0f, sx1 - sx0 - 0.5f), 3.0f);
        }
        if (box.getWidth() > 46.0f) {
            g.setColour(Text);
            g.setFont(body(9.5f));
            g.drawText(juce::String(t.m_index + 1) + " " + KeyNames[t.m_key], box.reduced(3.0f, 1.0f).removeFromTop(11.0f),
                       juce::Justification::topLeft, false);
        }
        if (t.m_locked) drawLock(g, juce::Rectangle<int>(juce::roundToInt(box.getRight()) - Glyph - 1,
                                                       juce::roundToInt(box.getY()), Glyph,
                                                       juce::jmin(Glyph, juce::roundToInt(box.getHeight()))), true);
    }
    // The energy arc of the night over it (Form.h): the line the sections were drawn against. One line
    // per track: over the overlap the outgoing outro and the incoming intro both run, and a single path
    // through the tracks in order would jump back sixteen bars at every hand-over.
    juce::Path arc;
    for (const Trk& t : m_snap.m_tracks) {
        bool started = false;
        for (const Sec& s : t.m_sections) {
            const float y0 = m_strip.getBottom() - 6.0f - s.m_energy * (m_strip.getHeight() - 12.0f);
            const float y1 = m_strip.getBottom() - 6.0f - s.m_energyTo * (m_strip.getHeight() - 12.0f);
            if (!started) { arc.startNewSubPath(stripX(s.m_bar), y0); started = true; }
            else arc.lineTo(stripX(s.m_bar), y0);
            arc.lineTo(stripX(s.m_bar + s.m_bars), y1);
        }
    }
    if (!arc.isEmpty()) {
        g.setColour(Green.withAlpha(0.8f));
        g.strokePath(arc, juce::PathStrokeType(1.4f));
    }

    // ---------------------------------------------------------------- the track rows
    for (size_t i = 0; i < m_rowArea.size(); ++i) {
        const Trk& t = m_snap.m_tracks[i];
        const juce::Rectangle<int> row = m_rowArea[i];
        const juce::Rectangle<int> head2(row.getX() - HeaderW, row.getY(), HeaderW, row.getHeight());
        g.setColour(t.m_locked ? Accent.withAlpha(0.10f) : juce::Colours::transparentBlack);
        g.fillRect(head2);
        g.setColour(t.m_locked ? Text : Dim);
        g.setFont(body(juce::jlimit(8.5f, 10.5f, row.getHeight() * 0.45f)));
        juce::String name;
        name << (t.m_index + 1) << "  " << KeyNames[t.m_key] << " " << ScaleNames[t.m_scale] << "  "
             << juce::String(t.m_bpm, 1) << " BPM";
        if (t.m_variation > 0) name << "  v" << juce::String(static_cast<int>(t.m_variation));
        g.drawText(name, head2.reduced(4, 0).withTrimmedRight(2 * Glyph + 8), juce::Justification::centredLeft, false);

        const double barW = static_cast<double>(row.getWidth()) / juce::jmax(1, t.m_bars);
        for (const Sec& s : t.m_sections) {
            const int x0 = row.getX() + static_cast<int>((s.m_bar - t.m_firstBar) * barW);
            const int x1 = row.getX() + static_cast<int>((s.m_bar - t.m_firstBar + s.m_bars) * barW);
            juce::Rectangle<float> box(static_cast<float>(x0), static_cast<float>(row.getY() + 1),
                                       juce::jmax(2.0f, static_cast<float>(x1 - x0) - 1.5f),
                                       static_cast<float>(row.getHeight() - 2));
            // The fill says what kind of section it is, its brightness how much energy it carries.
            const juce::Colour c = sectionColour(s.m_type);
            g.setColour(c.withAlpha(0.30f + 0.45f * juce::jlimit(0.0f, 1.0f, s.m_energy)));
            g.fillRoundedRectangle(box, 2.5f);
            if (s.m_locked) {
                g.setColour(Accent.withAlpha(0.85f));
                g.drawRoundedRectangle(box.reduced(0.5f), 2.5f, 1.2f);
            }
            if (box.getWidth() >= 3 * Glyph) {
                const juce::Rectangle<int> lockBox(x0 + 2, row.getY() + 1, Glyph, juce::jmin(Glyph, row.getHeight() - 2));
                drawLock(g, lockBox, s.m_locked);
                drawDie(g, lockBox.withX(juce::roundToInt(box.getRight()) - Glyph - 2), s.m_variation > 0);
            }
            if (box.getWidth() > 3.4f * Glyph) {
                g.setColour(Text.withAlpha(0.92f));
                g.setFont(body(juce::jlimit(8.0f, 10.0f, row.getHeight() * 0.42f)));
                g.drawText(SectionNames[static_cast<int>(s.m_type)],
                           box.toNearestInt().reduced(Glyph + 3, 0), juce::Justification::centred, false);
            }
        }
        // The bars this track shares with its neighbours (the DJ overlap): hatched, with the track they
        // mix with, drawn at the bottom edge so the section's lock, name and die stay readable.
        const auto [in, out] = overlapOf(i);
        auto hatch = [&](int fromBar, int bars, const juce::String& label, bool leftAligned) {
            if (bars <= 0) return;
            const float x0 = row.getX() + static_cast<float>(fromBar * barW);
            const float x1 = row.getX() + static_cast<float>((fromBar + bars) * barW);
            const juce::Rectangle<float> zone(x0, static_cast<float>(row.getY() + 1), juce::jmax(1.0f, x1 - x0),
                                              static_cast<float>(row.getHeight() - 2));
            g.saveState();
            g.reduceClipRegion(zone.toNearestInt());
            g.setColour(Text.withAlpha(0.22f));
            for (float x = zone.getX() - zone.getHeight(); x < zone.getRight(); x += 5.0f)
                g.drawLine(x, zone.getBottom(), x + zone.getHeight(), zone.getY(), 0.8f);
            g.restoreState();
            g.setColour(Text.withAlpha(0.55f));
            g.drawRect(zone, 0.8f);
            if (zone.getWidth() > 30.0f && row.getHeight() >= 24) {
                g.setFont(body(8.0f));
                g.drawText(label, zone.toNearestInt().reduced(2, 1).removeFromBottom(10),
                           leftAligned ? juce::Justification::bottomLeft : juce::Justification::bottomRight, false);
            }
        };
        hatch(0, in, "mix " + juce::String(t.m_index), true);
        hatch(t.m_bars - out, out, "mix " + juce::String(t.m_index + 2), false);
    }

    if (!m_stripOnly) {
        // ---------------------------------------------------------------- the legend
        g.setColour(Faint);
        g.setFont(body(9.5f));
        g.drawText("the strip is the night to scale, every row is one track filling the width   -   "
                   "hatched: bars two tracks share (DJ mix)   -   click a block to jump   -   "
                   "padlock freezes a track or a section on its seed   -   die draws it again",
                   getLocalBounds().reduced(6, 3).removeFromBottom(LegendH), juce::Justification::centredLeft, false);
    }
}

void ArrangeDisplay::paint(juce::Graphics& g)
{
    if (m_cache.isValid()) g.drawImageAt(m_cache, 0, 0);
    if (m_snap.m_tracks.empty() || m_strip.isEmpty()) return;
    // The play head, and only the play head, moves between repaints.
    const double bar = m_beat / BeatsPerBar;
    g.setColour(Accent);
    g.drawVerticalLine(juce::roundToInt(stripX(bar)), static_cast<float>(m_strip.getY()),
                       static_cast<float>(m_strip.getBottom()));
    for (size_t i = 0; i < m_rowArea.size(); ++i) {
        const Trk& t = m_snap.m_tracks[i];
        if (bar < t.m_firstBar || bar >= t.m_firstBar + t.m_bars) continue;
        const juce::Rectangle<int> row = m_rowArea[i];
        const float x = row.getX() + static_cast<float>((bar - t.m_firstBar) / juce::jmax(1, t.m_bars)) * row.getWidth();
        g.setColour(Accent);
        g.drawVerticalLine(juce::roundToInt(x), static_cast<float>(row.getY()), static_cast<float>(row.getBottom()));
        g.setColour(Accent.withAlpha(0.10f));
        g.fillRect(row.withTrimmedLeft(-HeaderW));
    }
}

void ArrangeDisplay::mouseDown(const juce::MouseEvent& e)
{
    // Later hits were added on top of earlier ones (the buttons after the block they sit on), so
    // the scan runs backwards and the small things win.
    for (auto it = m_hits.rbegin(); it != m_hits.rend(); ++it) {
        if (!it->m_area.contains(e.x, e.y)) continue;
        if (it->m_action == 1) { if (m_onLock) m_onLock(it->m_unit, it->m_index, !it->m_locked); return; }
        if (it->m_action == 2) { if (m_onReroll) m_onReroll(it->m_unit, it->m_index); return; }
        if (it->m_bar >= 0 && m_onSeek) m_onSeek(it->m_bar);
        return;
    }
}

// ==================================================================== the Arrange tab

void PhospheneEditor::buildArrangePage()
{
    auto page = std::make_unique<phosui::ControlPage>();
    const juce::Colour tint = partColour(1);

    const int ga = page->addGroup("Set timeline", tint, 16);
    {
        auto view = std::make_unique<ArrangeDisplay>();
        view->m_onSeek = [this](int bar) { m_proc.seekToBar(bar); };
        view->m_onLock = [this](LockUnitE unit, int index, bool locked) {
            m_proc.undoable(locked ? "Lock" : "Unlock", [this, unit, index, locked] { m_proc.setLock(unit, index, locked); });
            m_arrangeDirty = true;
        };
        view->m_onReroll = [this](LockUnitE unit, int index) {
            m_proc.undoable("Reroll", [this, unit, index] { m_proc.reroll(unit, index); });
            m_arrangeDirty = true;
        };
        m_arrange = view.get();
        page->addControl(ga, std::move(view), "", 16, true, 5);
    }

    // The set-wide half of the curation loop: the moves that have no block to sit on. Tracks and
    // sections are locked and rerolled on the timeline itself. (The fourth lockable unit, a
    // percussion lane, has no place here either -- it belongs on the Percussion tab and is not
    // built yet; see docs/rounds/2026-09.md, Phase 6 second round.)
    const int gc = page->addGroup("Curation", tint, 16);
    {
        auto reroll = std::make_unique<juce::TextButton>("Reroll the set");
        reroll->setTooltip("Draws the whole journey again: lengths, keys, tempi and sound recipes. "
                           "Locked tracks keep their seed and stay exactly as they are.");
        reroll->onClick = [this] { m_proc.undoable("Reroll the set", [this] { m_proc.reroll(LockUnitE::SET, 0); }); m_arrangeDirty = true; };
        page->addControl(gc, std::move(reroll), "", 4, true);

        auto here = std::make_unique<juce::TextButton>("Reroll this track");
        // "This track" is the one that owns the bar: over the DJ overlap the outgoing one, whose kick and
        // bass still play; the incoming one is rerolled from its own row on the timeline.
        here->setTooltip("Draws the playing track again. Over the sixteen bars two tracks share, that is the "
                         "outgoing one; the incoming track has its own die on its row.");
        here->onClick = [this] {
            m_proc.undoable("Reroll this track", [this] { m_proc.reroll(LockUnitE::TRACK, m_proc.transport().m_track); });
            m_arrangeDirty = true;
        };
        page->addControl(gc, std::move(here), "", 4, true);

        auto lockHere = std::make_unique<juce::TextButton>("Lock this track");
        lockHere->setTooltip("Locks the playing track (over the DJ overlap the outgoing one).");
        lockHere->onClick = [this] {
            const int t = m_proc.transport().m_track;
            m_proc.undoable("Lock this track", [this, t] { m_proc.setLock(LockUnitE::TRACK, t, !m_proc.isLocked(LockUnitE::TRACK, t)); });
            m_arrangeDirty = true;
        };
        page->addControl(gc, std::move(lockHere), "", 4, true);

        auto clear = std::make_unique<juce::TextButton>("Clear all locks");
        clear->onClick = [this] { m_proc.undoable("Clear all locks", [this] { m_proc.clearCuration(); }); m_arrangeDirty = true; };
        page->addControl(gc, std::move(clear), "", 4, true);

        auto note = std::make_unique<juce::Label>(juce::String(), juce::String());
        note->setJustificationType(juce::Justification::centredLeft);
        note->setColour(juce::Label::textColourId, Dim);
        m_arrangeNote = note.get();
        page->addControl(gc, std::move(note), "", 16, true);
    }
    m_pages[static_cast<size_t>(TAB_ARRANGE)] = std::move(page);
}

ArrangeDisplay::Snapshot PhospheneEditor::planSnapshot() const
{
    ArrangeDisplay::Snapshot s;
    const ParamStore& p = m_proc.params();
    s.m_minutes = static_cast<int>(p.get(p.base(ModuleE::COMPOSE) + compose::SET_MINUTES));
    for (int i = 0; i < 64; ++i) {
        TrackPlan plan;
        if (!m_proc.tryReadTrack(i, plan)) break;
        ArrangeDisplay::Trk trk;
        trk.m_index = plan.m_index;
        trk.m_firstBar = plan.m_firstBar;
        trk.m_bars = plan.m_bars;
        trk.m_key = plan.m_key;
        trk.m_scale = plan.m_scale;
        trk.m_bpm = plan.m_bpm;
        trk.m_gainDb = plan.m_gainDb;
        trk.m_locked = m_proc.isLocked(LockUnitE::TRACK, plan.m_index);
        trk.m_variation = m_proc.variation(LockUnitE::TRACK, plan.m_index);
        for (int si = 0; si < plan.m_form.m_count; ++si) {
            const Section& sec = plan.m_form.m_section[si];
            ArrangeDisplay::Sec out;
            out.m_index = si;
            out.m_bar = plan.m_firstBar + sec.m_startBar;
            out.m_bars = sec.m_bars;
            out.m_type = sec.m_type;
            out.m_energy = sec.m_energy;
            out.m_energyTo = sec.m_energyTo;
            out.m_locked = m_proc.isLocked(LockUnitE::SECTION, sectionUnitIndex(plan.m_index, si));
            out.m_variation = m_proc.variation(LockUnitE::SECTION, sectionUnitIndex(plan.m_index, si));
            trk.m_sections.push_back(out);
        }
        s.m_bars = plan.m_firstBar + plan.m_bars;
        s.m_tracks.push_back(std::move(trk));
    }
    return s;
}

namespace {
/** @brief A hash over everything the timeline's picture shows. */
uint64_t snapshotHash(const ArrangeDisplay::Snapshot& s)
{
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](uint64_t v) { hash = (hash ^ v) * 1099511628211ull; };
    mix(static_cast<uint64_t>(s.m_tracks.size()));
    mix(static_cast<uint64_t>(s.m_bars));
    for (const ArrangeDisplay::Trk& trk : s.m_tracks) {
        mix(static_cast<uint64_t>(trk.m_firstBar) * 131 + static_cast<uint64_t>(trk.m_bars));
        mix(static_cast<uint64_t>(trk.m_key) * 17 + static_cast<uint64_t>(trk.m_scale));
        mix(static_cast<uint64_t>(trk.m_bpm * 100.0));
        mix(static_cast<uint64_t>(trk.m_locked) * 7 + trk.m_variation * 1000003ull);
        for (const ArrangeDisplay::Sec& sec : trk.m_sections) {
            mix(static_cast<uint64_t>(sec.m_bar) * 131 + static_cast<uint64_t>(sec.m_bars) * 13 + static_cast<uint64_t>(sec.m_type));
            mix(static_cast<uint64_t>(sec.m_energy * 1000.0f) * 3 + static_cast<uint64_t>(sec.m_locked) + sec.m_variation * 1000003ull);
        }
    }
    return hash;
}
} // namespace

void PhospheneEditor::refreshOverview()
{
    // The strip under the header (01.10.2026): the plans read once a second, or at once after a reroll or a lock; the
    // play head every tick.
    if (m_arrangeDirty || m_overview.snapshot().m_tracks.empty() || ++m_overviewTicks % 12 == 0) {
        ArrangeDisplay::Snapshot s = planSnapshot();
        const uint64_t hash = snapshotHash(s);
        if (hash != m_overviewHash) {
            m_overviewHash = hash;
            m_overview.setSnapshot(std::move(s));
        }
    }
    m_overview.setPosition(m_proc.transport().m_musicalBeat);
}

void PhospheneEditor::refreshArrangePage()
{
    if (m_arrange == nullptr) return;
    const TransportView t = m_proc.transport();
    // Only the play head between snapshots: a set of a hundred sections is drawn once into an
    // image, and a tick moves a line over it.
    if (m_arrangeDirty || m_arrange->snapshot().m_tracks.empty() || ++m_arrangeTicks % 12 == 0) {
        ArrangeDisplay::Snapshot s;
        const ParamStore& p = m_proc.params();
        s.m_minutes = static_cast<int>(p.get(p.base(ModuleE::COMPOSE) + compose::SET_MINUTES));
        for (int i = 0; i < 64; ++i) {
            TrackPlan plan;
            if (!m_proc.tryReadTrack(i, plan)) break;
            ArrangeDisplay::Trk trk;
            trk.m_index = plan.m_index;
            trk.m_firstBar = plan.m_firstBar;
            trk.m_bars = plan.m_bars;
            trk.m_key = plan.m_key;
            trk.m_scale = plan.m_scale;
            trk.m_bpm = plan.m_bpm;
            trk.m_gainDb = plan.m_gainDb;
            trk.m_locked = m_proc.isLocked(LockUnitE::TRACK, plan.m_index);
            trk.m_variation = m_proc.variation(LockUnitE::TRACK, plan.m_index);
            for (int si = 0; si < plan.m_form.m_count; ++si) {
                const Section& sec = plan.m_form.m_section[si];
                ArrangeDisplay::Sec out;
                out.m_index = si;
                out.m_bar = plan.m_firstBar + sec.m_startBar;
                out.m_bars = sec.m_bars;
                out.m_type = sec.m_type;
                out.m_energy = sec.m_energy;
                out.m_energyTo = sec.m_energyTo;
                out.m_locked = m_proc.isLocked(LockUnitE::SECTION, sectionUnitIndex(plan.m_index, si));
                out.m_variation = m_proc.variation(LockUnitE::SECTION, sectionUnitIndex(plan.m_index, si));
                trk.m_sections.push_back(out);
            }
            s.m_bars = plan.m_firstBar + plan.m_bars;
            s.m_tracks.push_back(std::move(trk));
        }
        // Drawing the set is the expensive part (eight milliseconds for a sixty-minute night), so it
        // happens when the set has changed and not once a second on principle. The hash is over
        // everything the picture shows.
        uint64_t hash = 1469598103934665603ull;
        auto mix = [&hash](uint64_t v) { hash = (hash ^ v) * 1099511628211ull; };
        mix(static_cast<uint64_t>(s.m_tracks.size()));
        mix(static_cast<uint64_t>(s.m_bars));
        for (const ArrangeDisplay::Trk& trk : s.m_tracks) {
            mix(static_cast<uint64_t>(trk.m_firstBar) * 131 + static_cast<uint64_t>(trk.m_bars));
            mix(static_cast<uint64_t>(trk.m_key) * 17 + static_cast<uint64_t>(trk.m_scale));
            mix(static_cast<uint64_t>(trk.m_bpm * 100.0));
            mix(static_cast<uint64_t>(trk.m_locked) * 7 + trk.m_variation * 1000003ull);
            for (const ArrangeDisplay::Sec& sec : trk.m_sections) {
                mix(static_cast<uint64_t>(sec.m_bar) * 131 + static_cast<uint64_t>(sec.m_bars) * 13
                    + static_cast<uint64_t>(sec.m_type));
                mix(static_cast<uint64_t>(sec.m_energy * 1000.0f) * 3 + static_cast<uint64_t>(sec.m_locked)
                    + sec.m_variation * 1000003ull);
            }
        }
        if (hash != m_arrangeHash) {
            m_arrangeHash = hash;
            m_arrange->setSnapshot(std::move(s));
        }
        m_arrangeDirty = false;
    }
    m_arrange->setPosition(t.m_musicalBeat);
    if (m_arrangeNote != nullptr) {
        juce::String s;
        s << "bar " << juce::String(t.m_bar + 1) << " of the set";
        const int track = t.m_track;
        s << "   -   track " << juce::String(track + 1) << (m_proc.isLocked(LockUnitE::TRACK, track) ? " (locked)" : "");
        // The DJ overlap: the next track's intro already sounds under this one's outro.
        for (const ArrangeDisplay::Trk& trk : m_arrange->snapshot().m_tracks)
            if (trk.m_index > track && t.m_bar >= trk.m_firstBar && t.m_bar < trk.m_firstBar + trk.m_bars)
                s << ", mixing into track " << juce::String(trk.m_index + 1);
        if (t.m_restarting) s << "   -   planning...";
        m_arrangeNote->setText(s, juce::dontSendNotification);
    }
}
