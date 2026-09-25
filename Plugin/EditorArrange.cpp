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

constexpr int kStripH = 58;      ///< the set-wide bar at the top
constexpr int kHeaderW = 176;    ///< the part of a track row that names the track
constexpr int kLegendH = 15;     ///< the line at the bottom that says what a click does
constexpr int kGlyph = 13;       ///< side of the lock and reroll buttons
constexpr int kLaneGap = 3;      ///< between the strip's two lanes (odd and even tracks)

/** @brief The colour of a section type: warm where the energy is, cold where it is taken away. */
juce::Colour sectionColour(SectionType t)
{
    switch (t) {
    case SectionType::Intro:  return juce::Colour(0xff5d6683);
    case SectionType::Groove: return juce::Colour(0xff3f9d7a);
    case SectionType::Build:  return juce::Colour(0xffd9a441);
    case SectionType::Drop:   return juce::Colour(0xffff6b6b);
    case SectionType::Break:  return juce::Colour(0xff6a7fd6);
    case SectionType::Outro:  return juce::Colour(0xff4d5670);
    case SectionType::Pdb:    return juce::Colour(0xffb06bd8);
    case SectionType::Cut:    return juce::Colour(0xff2c3244);
    default: return faint;
    }
}

/** @brief A padlock, open or shut, in the box it is given. */
void drawLock(juce::Graphics& g, juce::Rectangle<int> r, bool locked)
{
    const juce::Rectangle<float> box = r.toFloat().reduced(1.5f);
    const float w = box.getWidth(), h = box.getHeight();
    const juce::Colour c = locked ? accent : faint;
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
    g.setColour(used ? accent : faint);
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
    snap_ = std::move(s);
    measure();
    render();
    repaint();
}

bool ArrangeDisplay::setPosition(double musicalBeat)
{
    beat_ = musicalBeat;
    const int px = strip_.isEmpty() ? -1 : juce::roundToInt(stripX(musicalBeat / kBeatsPerBar));
    if (px == headPixel_) return false;
    headPixel_ = px;
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
    // From the plans themselves rather than from kDjOverlap: the first track of a set has no overlap
    // in front of it, and the last planned one none behind it until its successor is planned.
    const auto& ts = snap_.tracks;
    if (i >= ts.size()) return { 0, 0 };
    auto shared = [&](size_t a, size_t b) {
        return juce::jmax(0, ts[a].firstBar + ts[a].bars - ts[b].firstBar);
    };
    return { i > 0 ? shared(i - 1, i) : 0, i + 1 < ts.size() ? shared(i, i + 1) : 0 };
}

float ArrangeDisplay::stripX(double bar) const
{
    const double t = juce::jlimit(0.0, 1.0, bar / juce::jmax(1, totalBars_));
    return strip_.getX() + static_cast<float>(t) * strip_.getWidth();
}

void ArrangeDisplay::measure()
{
    hits_.clear();
    rowArea_.clear();
    blocks_.clear();
    juce::Rectangle<int> r = getLocalBounds().reduced(6, 4);
    if (r.getWidth() < 40 || r.getHeight() < 40) { strip_ = {}; return; }
    r.removeFromBottom(kLegendH);
    strip_ = r.removeFromTop(kStripH).withTrimmedTop(13);
    r.removeFromTop(6);

    totalBars_ = juce::jmax(1, snap_.bars);
    const int n = static_cast<int>(snap_.tracks.size());
    if (n == 0) return;

    // The set strip: every track a block in its lane, clickable at its first bar. Two lanes, because
    // neighbouring tracks share kDjOverlap bars: in one lane the later block hid the end of the earlier.
    blocks_.clear();
    const int laneH = (strip_.getHeight() - kLaneGap) / 2;
    for (size_t i = 0; i < snap_.tracks.size(); ++i) {
        const Trk& t = snap_.tracks[i];
        const int x0 = juce::roundToInt(stripX(t.firstBar)), x1 = juce::roundToInt(stripX(t.firstBar + t.bars));
        const int y = strip_.getY() + (i % 2 == 0 ? 0 : laneH + kLaneGap);
        blocks_.emplace_back(x0, y, juce::jmax(2, x1 - x0), laneH);
        Hit h;
        h.area = blocks_.back();
        h.bar = t.firstBar;
        h.action = 0;
        hits_.push_back(h);
    }

    // The rows. They share what is left; a row never grows past a comfortable height, so a short
    // set does not draw nine fat bars, and never shrinks below what a section label needs.
    const int rowH = juce::jlimit(20, 44, r.getHeight() / n);
    for (int i = 0; i < n && r.getHeight() >= rowH; ++i) {
        const Trk& t = snap_.tracks[static_cast<size_t>(i)];
        juce::Rectangle<int> row = r.removeFromTop(rowH);
        juce::Rectangle<int> head = row.removeFromLeft(kHeaderW);
        // The track's own lock and reroll sit at the end of its name.
        juce::Rectangle<int> buttons = head.removeFromRight(2 * kGlyph + 8).reduced(2, (rowH - kGlyph) / 2);
        Hit lock;
        lock.area = buttons.removeFromLeft(kGlyph);
        lock.unit = LockUnit::Track;
        lock.index = t.index;
        lock.action = 1;
        lock.locked = t.locked;
        hits_.push_back(lock);
        buttons.removeFromLeft(4);
        Hit die = lock;
        die.area = buttons.removeFromLeft(kGlyph);
        die.action = 2;
        hits_.push_back(die);

        rowArea_.push_back(row);
        const double barW = static_cast<double>(row.getWidth()) / juce::jmax(1, t.bars);
        for (const Sec& s : t.sections) {
            const int x0 = row.getX() + static_cast<int>((s.bar - t.firstBar) * barW);
            const int x1 = row.getX() + static_cast<int>((s.bar - t.firstBar + s.bars) * barW);
            const juce::Rectangle<int> box(x0, row.getY(), juce::jmax(2, x1 - x0), row.getHeight());
            Hit seek;
            seek.area = box;
            seek.bar = s.bar;
            seek.action = 0;
            hits_.push_back(seek);
            if (box.getWidth() < 3 * kGlyph) continue;   // no room for the two buttons: seeking only
            Hit sl;
            sl.area = juce::Rectangle<int>(box.getX() + 2, box.getY() + 1, kGlyph, juce::jmin(kGlyph, box.getHeight() - 2));
            sl.unit = LockUnit::Section;
            sl.index = sectionUnitIndex(t.index, s.index);
            sl.action = 1;
            sl.locked = s.locked;
            hits_.push_back(sl);
            Hit sd = sl;
            sd.area = sl.area.withX(box.getRight() - kGlyph - 2);
            sd.action = 2;
            hits_.push_back(sd);
        }
    }
}

void ArrangeDisplay::render()
{
    const int w = getWidth(), h = getHeight();
    if (w <= 0 || h <= 0) return;
    cache_ = juce::Image(juce::Image::ARGB, w, h, true);
    juce::Graphics g(cache_);
    g.setColour(bg0.withAlpha(0.6f));
    g.fillRoundedRectangle(getLocalBounds().reduced(3, 2).toFloat(), 5.0f);

    juce::Rectangle<int> caption = getLocalBounds().reduced(6, 4).withHeight(13);
    g.setColour(dim);
    g.setFont(title(10.0f));
    juce::String head = "ARRANGE";
    if (!snap_.tracks.empty())
        head << "   " << juce::String(static_cast<int>(snap_.tracks.size())) << " tracks   "
             << juce::String(snap_.bars) << " bars planned";
    if (snap_.minutes > 0) head << "   set " << juce::String(snap_.minutes) << " min";
    g.drawText(head, caption, juce::Justification::topLeft, false);

    if (snap_.tracks.empty()) {
        g.setColour(faint);
        g.setFont(body(11.5f));
        g.drawText("planning...", getLocalBounds(), juce::Justification::centred, false);
        return;
    }

    // ---------------------------------------------------------------- the set strip
    g.setColour(edge.withAlpha(0.5f));
    g.fillRoundedRectangle(strip_.toFloat(), 3.0f);
    // The overlaps first, under the blocks: a band over both lanes with a crossfade (the outgoing track
    // falling, the incoming one rising), so the eye reads the two lanes as one mix there.
    for (size_t i = 0; i + 1 < snap_.tracks.size() && i + 1 < blocks_.size(); ++i) {
        const int shared = overlapOf(i).second;
        if (shared <= 0) continue;
        const Trk& nx = snap_.tracks[i + 1];
        const float x0 = stripX(nx.firstBar), x1 = stripX(nx.firstBar + shared);
        const juce::Rectangle<float> band(x0, static_cast<float>(strip_.getY()), juce::jmax(1.0f, x1 - x0),
                                          static_cast<float>(strip_.getHeight()));
        g.setColour(text.withAlpha(0.07f));
        g.fillRect(band);
        const juce::Rectangle<float> out = blocks_[i].toFloat(), in = blocks_[i + 1].toFloat();
        g.setColour(text.withAlpha(0.35f));
        g.drawLine(band.getX(), out.getCentreY(), band.getRight(), in.getCentreY(), 1.0f);
        g.drawLine(band.getX(), in.getCentreY(), band.getRight(), out.getCentreY(), 1.0f);
    }
    for (size_t i = 0; i < snap_.tracks.size() && i < blocks_.size(); ++i) {
        const Trk& t = snap_.tracks[i];
        const juce::Rectangle<float> lane = blocks_[i].toFloat();
        const juce::Rectangle<float> box = lane.withWidth(juce::jmax(2.0f, lane.getWidth() - 1.0f));
        g.setColour(partColour(static_cast<int>(i) + 1).withAlpha(t.locked ? 0.42f : 0.26f));
        g.fillRoundedRectangle(box, 2.5f);
        // Every section of that track as a hairline of its own colour along the bottom: at this
        // scale a section is a few pixels, so it is a texture rather than a block.
        for (const Sec& s : t.sections) {
            const float sx0 = stripX(s.bar), sx1 = stripX(s.bar + s.bars);
            g.setColour(sectionColour(s.type).withAlpha(0.85f));
            g.fillRect(sx0, box.getBottom() - 4.0f, juce::jmax(1.0f, sx1 - sx0 - 0.5f), 3.0f);
        }
        if (box.getWidth() > 46.0f) {
            g.setColour(text);
            g.setFont(body(9.5f));
            g.drawText(juce::String(t.index + 1) + " " + kKeyNames[t.key], box.reduced(3.0f, 1.0f).removeFromTop(11.0f),
                       juce::Justification::topLeft, false);
        }
        if (t.locked) drawLock(g, juce::Rectangle<int>(juce::roundToInt(box.getRight()) - kGlyph - 1,
                                                       juce::roundToInt(box.getY()), kGlyph,
                                                       juce::jmin(kGlyph, juce::roundToInt(box.getHeight()))), true);
    }
    // The energy arc of the night over it (Form.h): the line the sections were drawn against. One line
    // per track: over the overlap the outgoing outro and the incoming intro both run, and a single path
    // through the tracks in order would jump back sixteen bars at every hand-over.
    juce::Path arc;
    for (const Trk& t : snap_.tracks) {
        bool started = false;
        for (const Sec& s : t.sections) {
            const float y0 = strip_.getBottom() - 6.0f - s.energy * (strip_.getHeight() - 12.0f);
            const float y1 = strip_.getBottom() - 6.0f - s.energyTo * (strip_.getHeight() - 12.0f);
            if (!started) { arc.startNewSubPath(stripX(s.bar), y0); started = true; }
            else arc.lineTo(stripX(s.bar), y0);
            arc.lineTo(stripX(s.bar + s.bars), y1);
        }
    }
    if (!arc.isEmpty()) {
        g.setColour(green.withAlpha(0.8f));
        g.strokePath(arc, juce::PathStrokeType(1.4f));
    }

    // ---------------------------------------------------------------- the track rows
    for (size_t i = 0; i < rowArea_.size(); ++i) {
        const Trk& t = snap_.tracks[i];
        const juce::Rectangle<int> row = rowArea_[i];
        const juce::Rectangle<int> head2(row.getX() - kHeaderW, row.getY(), kHeaderW, row.getHeight());
        g.setColour(t.locked ? accent.withAlpha(0.10f) : juce::Colours::transparentBlack);
        g.fillRect(head2);
        g.setColour(t.locked ? text : dim);
        g.setFont(body(juce::jlimit(8.5f, 10.5f, row.getHeight() * 0.45f)));
        juce::String name;
        name << (t.index + 1) << "  " << kKeyNames[t.key] << " " << kScaleNames[t.scale] << "  "
             << juce::String(t.bpm, 1) << " BPM";
        if (t.variation > 0) name << "  v" << juce::String(static_cast<int>(t.variation));
        g.drawText(name, head2.reduced(4, 0).withTrimmedRight(2 * kGlyph + 8), juce::Justification::centredLeft, false);

        const double barW = static_cast<double>(row.getWidth()) / juce::jmax(1, t.bars);
        for (const Sec& s : t.sections) {
            const int x0 = row.getX() + static_cast<int>((s.bar - t.firstBar) * barW);
            const int x1 = row.getX() + static_cast<int>((s.bar - t.firstBar + s.bars) * barW);
            juce::Rectangle<float> box(static_cast<float>(x0), static_cast<float>(row.getY() + 1),
                                       juce::jmax(2.0f, static_cast<float>(x1 - x0) - 1.5f),
                                       static_cast<float>(row.getHeight() - 2));
            // The fill says what kind of section it is, its brightness how much energy it carries.
            const juce::Colour c = sectionColour(s.type);
            g.setColour(c.withAlpha(0.30f + 0.45f * juce::jlimit(0.0f, 1.0f, s.energy)));
            g.fillRoundedRectangle(box, 2.5f);
            if (s.locked) {
                g.setColour(accent.withAlpha(0.85f));
                g.drawRoundedRectangle(box.reduced(0.5f), 2.5f, 1.2f);
            }
            if (box.getWidth() >= 3 * kGlyph) {
                const juce::Rectangle<int> lockBox(x0 + 2, row.getY() + 1, kGlyph, juce::jmin(kGlyph, row.getHeight() - 2));
                drawLock(g, lockBox, s.locked);
                drawDie(g, lockBox.withX(juce::roundToInt(box.getRight()) - kGlyph - 2), s.variation > 0);
            }
            if (box.getWidth() > 3.4f * kGlyph) {
                g.setColour(text.withAlpha(0.92f));
                g.setFont(body(juce::jlimit(8.0f, 10.0f, row.getHeight() * 0.42f)));
                g.drawText(kSectionNames[static_cast<int>(s.type)],
                           box.toNearestInt().reduced(kGlyph + 3, 0), juce::Justification::centred, false);
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
            g.setColour(text.withAlpha(0.22f));
            for (float x = zone.getX() - zone.getHeight(); x < zone.getRight(); x += 5.0f)
                g.drawLine(x, zone.getBottom(), x + zone.getHeight(), zone.getY(), 0.8f);
            g.restoreState();
            g.setColour(text.withAlpha(0.55f));
            g.drawRect(zone, 0.8f);
            if (zone.getWidth() > 30.0f && row.getHeight() >= 24) {
                g.setFont(body(8.0f));
                g.drawText(label, zone.toNearestInt().reduced(2, 1).removeFromBottom(10),
                           leftAligned ? juce::Justification::bottomLeft : juce::Justification::bottomRight, false);
            }
        };
        hatch(0, in, "mix " + juce::String(t.index), true);
        hatch(t.bars - out, out, "mix " + juce::String(t.index + 2), false);
    }

    // ---------------------------------------------------------------- the legend
    g.setColour(faint);
    g.setFont(body(9.5f));
    g.drawText("the strip is the night to scale, every row is one track filling the width   -   "
               "hatched: bars two tracks share (DJ mix)   -   click a block to jump   -   "
               "padlock freezes a track or a section on its seed   -   die draws it again",
               getLocalBounds().reduced(6, 3).removeFromBottom(kLegendH), juce::Justification::centredLeft, false);
}

void ArrangeDisplay::paint(juce::Graphics& g)
{
    if (cache_.isValid()) g.drawImageAt(cache_, 0, 0);
    if (snap_.tracks.empty() || strip_.isEmpty()) return;
    // The play head, and only the play head, moves between repaints.
    const double bar = beat_ / kBeatsPerBar;
    g.setColour(accent);
    g.drawVerticalLine(juce::roundToInt(stripX(bar)), static_cast<float>(strip_.getY()),
                       static_cast<float>(strip_.getBottom()));
    for (size_t i = 0; i < rowArea_.size(); ++i) {
        const Trk& t = snap_.tracks[i];
        if (bar < t.firstBar || bar >= t.firstBar + t.bars) continue;
        const juce::Rectangle<int> row = rowArea_[i];
        const float x = row.getX() + static_cast<float>((bar - t.firstBar) / juce::jmax(1, t.bars)) * row.getWidth();
        g.setColour(accent);
        g.drawVerticalLine(juce::roundToInt(x), static_cast<float>(row.getY()), static_cast<float>(row.getBottom()));
        g.setColour(accent.withAlpha(0.10f));
        g.fillRect(row.withTrimmedLeft(-kHeaderW));
    }
}

void ArrangeDisplay::mouseDown(const juce::MouseEvent& e)
{
    // Later hits were added on top of earlier ones (the buttons after the block they sit on), so
    // the scan runs backwards and the small things win.
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (!it->area.contains(e.x, e.y)) continue;
        if (it->action == 1) { if (onLock) onLock(it->unit, it->index, !it->locked); return; }
        if (it->action == 2) { if (onReroll) onReroll(it->unit, it->index); return; }
        if (it->bar >= 0 && onSeek) onSeek(it->bar);
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
        view->onSeek = [this](int bar) { proc_.seekToBar(bar); };
        view->onLock = [this](LockUnit unit, int index, bool locked) {
            proc_.undoable(locked ? "Lock" : "Unlock", [this, unit, index, locked] { proc_.setLock(unit, index, locked); });
            arrangeDirty_ = true;
        };
        view->onReroll = [this](LockUnit unit, int index) {
            proc_.undoable("Reroll", [this, unit, index] { proc_.reroll(unit, index); });
            arrangeDirty_ = true;
        };
        arrange_ = view.get();
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
        reroll->onClick = [this] { proc_.undoable("Reroll the set", [this] { proc_.reroll(LockUnit::Set, 0); }); arrangeDirty_ = true; };
        page->addControl(gc, std::move(reroll), "", 4, true);

        auto here = std::make_unique<juce::TextButton>("Reroll this track");
        // "This track" is the one that owns the bar: over the DJ overlap the outgoing one, whose kick and
        // bass still play; the incoming one is rerolled from its own row on the timeline.
        here->setTooltip("Draws the playing track again. Over the sixteen bars two tracks share, that is the "
                         "outgoing one; the incoming track has its own die on its row.");
        here->onClick = [this] {
            proc_.undoable("Reroll this track", [this] { proc_.reroll(LockUnit::Track, proc_.transport().track); });
            arrangeDirty_ = true;
        };
        page->addControl(gc, std::move(here), "", 4, true);

        auto lockHere = std::make_unique<juce::TextButton>("Lock this track");
        lockHere->setTooltip("Locks the playing track (over the DJ overlap the outgoing one).");
        lockHere->onClick = [this] {
            const int t = proc_.transport().track;
            proc_.undoable("Lock this track", [this, t] { proc_.setLock(LockUnit::Track, t, !proc_.isLocked(LockUnit::Track, t)); });
            arrangeDirty_ = true;
        };
        page->addControl(gc, std::move(lockHere), "", 4, true);

        auto clear = std::make_unique<juce::TextButton>("Clear all locks");
        clear->onClick = [this] { proc_.undoable("Clear all locks", [this] { proc_.clearCuration(); }); arrangeDirty_ = true; };
        page->addControl(gc, std::move(clear), "", 4, true);

        auto note = std::make_unique<juce::Label>(juce::String(), juce::String());
        note->setJustificationType(juce::Justification::centredLeft);
        note->setColour(juce::Label::textColourId, dim);
        arrangeNote_ = note.get();
        page->addControl(gc, std::move(note), "", 16, true);
    }
    pages_[static_cast<size_t>(TabArrange)] = std::move(page);
}

void PhospheneEditor::refreshArrangePage()
{
    if (arrange_ == nullptr) return;
    const TransportView t = proc_.transport();
    // Only the play head between snapshots: a set of a hundred sections is drawn once into an
    // image, and a tick moves a line over it.
    if (arrangeDirty_ || arrange_->snapshot().tracks.empty() || ++arrangeTicks_ % 12 == 0) {
        ArrangeDisplay::Snapshot s;
        const ParamStore& p = proc_.params();
        s.minutes = static_cast<int>(p.get(p.base(Module::Compose) + compose::SetMinutes));
        for (int i = 0; i < 64; ++i) {
            TrackPlan plan;
            if (!proc_.tryReadTrack(i, plan)) break;
            ArrangeDisplay::Trk trk;
            trk.index = plan.index;
            trk.firstBar = plan.firstBar;
            trk.bars = plan.bars;
            trk.key = plan.key;
            trk.scale = plan.scale;
            trk.bpm = plan.bpm;
            trk.gainDb = plan.gainDb;
            trk.locked = proc_.isLocked(LockUnit::Track, plan.index);
            trk.variation = proc_.variation(LockUnit::Track, plan.index);
            for (int si = 0; si < plan.form.count; ++si) {
                const Section& sec = plan.form.section[si];
                ArrangeDisplay::Sec out;
                out.index = si;
                out.bar = plan.firstBar + sec.startBar;
                out.bars = sec.bars;
                out.type = sec.type;
                out.energy = sec.energy;
                out.energyTo = sec.energyTo;
                out.locked = proc_.isLocked(LockUnit::Section, sectionUnitIndex(plan.index, si));
                out.variation = proc_.variation(LockUnit::Section, sectionUnitIndex(plan.index, si));
                trk.sections.push_back(out);
            }
            s.bars = plan.firstBar + plan.bars;
            s.tracks.push_back(std::move(trk));
        }
        // Drawing the set is the expensive part (eight milliseconds for a sixty-minute night), so it
        // happens when the set has changed and not once a second on principle. The hash is over
        // everything the picture shows.
        uint64_t hash = 1469598103934665603ull;
        auto mix = [&hash](uint64_t v) { hash = (hash ^ v) * 1099511628211ull; };
        mix(static_cast<uint64_t>(s.tracks.size()));
        mix(static_cast<uint64_t>(s.bars));
        for (const ArrangeDisplay::Trk& trk : s.tracks) {
            mix(static_cast<uint64_t>(trk.firstBar) * 131 + static_cast<uint64_t>(trk.bars));
            mix(static_cast<uint64_t>(trk.key) * 17 + static_cast<uint64_t>(trk.scale));
            mix(static_cast<uint64_t>(trk.bpm * 100.0));
            mix(static_cast<uint64_t>(trk.locked) * 7 + trk.variation * 1000003ull);
            for (const ArrangeDisplay::Sec& sec : trk.sections) {
                mix(static_cast<uint64_t>(sec.bar) * 131 + static_cast<uint64_t>(sec.bars) * 13
                    + static_cast<uint64_t>(sec.type));
                mix(static_cast<uint64_t>(sec.energy * 1000.0f) * 3 + static_cast<uint64_t>(sec.locked)
                    + sec.variation * 1000003ull);
            }
        }
        if (hash != arrangeHash_) {
            arrangeHash_ = hash;
            arrange_->setSnapshot(std::move(s));
        }
        arrangeDirty_ = false;
    }
    arrange_->setPosition(t.musicalBeat);
    if (arrangeNote_ != nullptr) {
        juce::String s;
        s << "bar " << juce::String(t.bar + 1) << " of the set";
        const int track = t.track;
        s << "   -   track " << juce::String(track + 1) << (proc_.isLocked(LockUnit::Track, track) ? " (locked)" : "");
        // The DJ overlap: the next track's intro already sounds under this one's outro.
        for (const ArrangeDisplay::Trk& trk : arrange_->snapshot().tracks)
            if (trk.index > track && t.bar >= trk.firstBar && t.bar < trk.firstBar + trk.bars)
                s << ", mixing into track " << juce::String(trk.index + 1);
        if (t.restarting) s << "   -   planning...";
        arrangeNote_->setText(s, juce::dontSendNotification);
    }
}
