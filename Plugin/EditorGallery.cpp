/**
 * @file EditorGallery.cpp
 * @brief The Gallery tab (23.09.2026): saved sets, each with its name, seed, tracks, the listener's verdicts and
 *        a strip of its form; a click loads one.
 *
 * Sets are ordinary `.phosset` files in the gallery folder (PhospheneProcessor::galleryFolder) with the gallery
 * lines of phos/Gallery.h after their header. "Save to gallery..." asks for a name and writes the set as it
 * stands -- seed, knobs, locks and rerolls -- together with the tracks the editor has seen planned, which is what
 * the strip draws. Loading goes through the processor's undo (a loaded set is one step back), and the verdicts
 * next to each set are counted from the ratings file of the Perform tab, by seed.
 */
#include "PluginEditor.h"
#include <algorithm>

using namespace phos;
using namespace phosui;

namespace {

/** @brief The colour of a section letter in the form strip. */
juce::Colour sectionColour(char c)
{
    switch (c) {
    case 'I': return faint;
    case 'G': return juce::Colour::fromHSV(0.33f, 0.45f, 0.75f, 1.0f);
    case 'B': return juce::Colour::fromHSV(0.14f, 0.60f, 0.90f, 1.0f);
    case 'D': return accent;
    case 'C': return warm;
    case 'K': return juce::Colour::fromHSV(0.76f, 0.45f, 0.85f, 1.0f);
    case 'O': return faint.darker(0.3f);
    default: return edge;
    }
}

} // namespace

// ==================================================================== GalleryDisplay

void GalleryDisplay::setRows(std::vector<Row> rows)
{
    rows_ = std::move(rows);
    hover_ = -1;
    setSize(getWidth(), juce::jmax(kRowH, static_cast<int>(rows_.size()) * kRowH));
    repaint();
}

void GalleryDisplay::paint(juce::Graphics& g)
{
    if (rows_.empty()) {
        g.setColour(dim);
        g.setFont(body(13.0f));
        g.drawText("No sets saved yet. \"Save to gallery...\" keeps the set that is playing, with its seed, knobs, locks and rerolls.",
                   getLocalBounds().reduced(12, 0), juce::Justification::centredLeft, true);
        return;
    }
    for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& r = rows_[i];
        juce::Rectangle<int> row(0, static_cast<int>(i) * kRowH, getWidth(), kRowH);
        if (static_cast<int>(i) == hover_) { g.setColour(group.brighter(0.08f)); g.fillRect(row); }
        g.setColour(edge);
        g.drawHorizontalLine(row.getBottom() - 1, 0.0f, static_cast<float>(getWidth()));
        juce::Rectangle<int> left = row.reduced(12, 6).removeFromLeft(380);
        const juce::String name = r.entry.name.empty() ? r.file.getFileNameWithoutExtension() : juce::String(r.entry.name);
        g.setColour(text);
        g.setFont(title(13.5f));
        g.drawText(name, left.removeFromTop(18), juce::Justification::centredLeft, true);
        double minutes = 0.0;
        juce::StringArray styles;
        for (const GalleryTrack& t : r.entry.tracks) {
            if (t.bpm > 0.0) minutes += t.bars * 4.0 / t.bpm;
            styles.addIfNotAlreadyThere(juce::String(t.style));
        }
        juce::String info;
        info << "seed " << juce::String(static_cast<juce::int64>(r.entry.seed));
        if (!r.entry.tracks.empty()) info << "  ·  " << static_cast<int>(r.entry.tracks.size()) << " tracks, " << juce::roundToInt(minutes) << " min  ·  " << styles.joinIntoString(", ");
        g.setColour(dim);
        g.setFont(body(11.5f));
        g.drawText(info, left.removeFromTop(16), juce::Justification::centredLeft, true);
        juce::String second = juce::String(r.entry.saved).replaceCharacter('T', ' ');
        if (r.rating.good + r.rating.bad + r.rating.notes > 0)
            second << "   ·   good " << r.rating.good << ", bad " << r.rating.bad << (r.rating.notes > 0 ? ", notes " + juce::String(r.rating.notes) : juce::String());
        g.drawText(second, left.removeFromTop(16), juce::Justification::centredLeft, true);

        // The strip: every track's sections in order, as wide as their bars, over the width that is left.
        juce::Rectangle<int> strip = row.reduced(12, 16).withTrimmedLeft(400);
        int total = 0;
        for (const GalleryTrack& t : r.entry.tracks) total += t.bars;
        if (total <= 0 || strip.getWidth() < 20) {
            g.setColour(faint);
            g.drawText("(no form saved)", strip, juce::Justification::centredLeft, false);
            continue;
        }
        const float scale = (strip.getWidth() - 3.0f * static_cast<float>(r.entry.tracks.size())) / static_cast<float>(total);
        float x = static_cast<float>(strip.getX());
        for (const GalleryTrack& t : r.entry.tracks) {
            const juce::StringArray parts = juce::StringArray::fromTokens(juce::String(t.form), " ", "");
            for (const juce::String& p : parts) {
                if (p.isEmpty()) continue;
                const float w = scale * static_cast<float>(p.substring(1).getIntValue());
                g.setColour(sectionColour(static_cast<char>(p[0])));
                g.fillRect(juce::Rectangle<float>(x, static_cast<float>(strip.getY()), juce::jmax(1.0f, w - 0.5f), static_cast<float>(strip.getHeight())));
                x += w;
            }
            x += 3.0f;   // a gap between tracks
        }
    }
}

void GalleryDisplay::mouseDown(const juce::MouseEvent& e)
{
    const int i = e.y / kRowH;
    if (i >= 0 && i < static_cast<int>(rows_.size()) && onOpen) onOpen(rows_[static_cast<size_t>(i)].file);
}

void GalleryDisplay::mouseMove(const juce::MouseEvent& e)
{
    const int i = e.y / kRowH;
    if (i != hover_) { hover_ = i; repaint(); }
}

void GalleryDisplay::mouseExit(const juce::MouseEvent&)
{
    hover_ = -1;
    repaint();
}

// ==================================================================== the page

void PhospheneEditor::buildGalleryPage()
{
    auto page = std::make_unique<ControlPage>();
    const juce::Colour tint = partColour(TabGallery);

    const int ga = page->addGroup("Gallery", tint, 16);
    {
        auto save = std::make_unique<juce::TextButton>("Save to gallery...");
        save->setTooltip("Keeps the set as it is now: seed, knobs, locks, rerolls, and the form of its tracks");
        save->onClick = [this] {
            galleryAsk_ = std::make_unique<juce::AlertWindow>("Save to gallery", "A name for this set", juce::MessageBoxIconType::NoIcon);
            galleryAsk_->addTextEditor("name", "seed " + juce::String(static_cast<juce::int64>(proc_.seed())));
            galleryAsk_->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
            galleryAsk_->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            galleryAsk_->enterModalState(true, juce::ModalCallbackFunction::create([this](int result) {
                if (result == 1 && galleryAsk_ != nullptr) {
                    const juce::File f = proc_.saveToGallery(galleryAsk_->getTextEditorContents("name"));
                    if (galleryNote_ != nullptr)
                        galleryNote_->setText(f.existsAsFile() ? "Saved: " + f.getFullPathName() : juce::String("Could not write into ") + proc_.galleryFolder().getFullPathName(),
                                              juce::dontSendNotification);
                    refreshGalleryPage();
                }
                // Not deleted inside its own callback: the modal manager is still holding it.
                juce::MessageManager::callAsync([safe = juce::Component::SafePointer<PhospheneEditor>(this)] { if (safe != nullptr) safe->galleryAsk_.reset(); });
            }), false);
        };
        page->addControl(ga, std::move(save), "", 4, true);

        auto open = std::make_unique<juce::TextButton>("Open folder...");
        open->setTooltip("Shows the gallery folder: sets can be copied in and out as files");
        open->onClick = [this] { proc_.galleryFolder().startAsProcess(); };
        page->addControl(ga, std::move(open), "", 3, true);

        auto note = std::make_unique<juce::Label>(juce::String(), proc_.galleryFolder().getFullPathName());
        note->setJustificationType(juce::Justification::centredLeft);
        note->setColour(juce::Label::textColourId, dim);
        note->setMinimumHorizontalScale(0.6f);
        galleryNote_ = note.get();
        page->addControl(ga, std::move(note), "", 9, true);
    }

    const int gl = page->addGroup("Saved sets (click to load)", tint, 16);
    {
        auto view = std::make_unique<juce::Viewport>();
        view->setScrollBarsShown(true, false);
        auto* list = new GalleryDisplay();
        list->onOpen = [this](const juce::File& f) {
            bool ok = false;
            proc_.undoable("Load " + f.getFileNameWithoutExtension(), [this, f, &ok] { ok = proc_.importSet(f); });
            if (galleryNote_ != nullptr)
                galleryNote_->setText((ok ? "Loaded: " : "Could not read: ") + f.getFileName(), juce::dontSendNotification);
            trackRows_.clear();
            rowsSeed_ = 0;
        };
        view->setViewedComponent(list, true);
        gallery_ = list;
        galleryView_ = view.get();
        page->addControl(gl, std::move(view), "", 16, true, 8);
    }
    pages_[static_cast<size_t>(TabGallery)] = std::move(page);
}

void PhospheneEditor::refreshGalleryPage()
{
    if (gallery_ == nullptr) return;
    const std::map<uint64_t, RatingCount> ratings = ratingsBySeed(proc_.ratingsFile().getFullPathName().toRawUTF8());
    juce::Array<juce::File> files = proc_.galleryFolder().findChildFiles(juce::File::findFiles, false, "*.phosset");
    std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b) { return a.getLastModificationTime() > b.getLastModificationTime(); });
    std::vector<GalleryDisplay::Row> rows;
    for (const juce::File& f : files) {
        GalleryDisplay::Row r;
        r.file = f;
        if (!readGalleryEntry(f.loadFileAsString().toStdString(), r.entry)) continue;
        const auto it = ratings.find(r.entry.seed);
        if (it != ratings.end()) r.rating = it->second;
        rows.push_back(std::move(r));
    }
    const int w = galleryView_ != nullptr ? juce::jmax(200, galleryView_->getMaximumVisibleWidth()) : 800;
    gallery_->setSize(w, gallery_->getHeight());
    gallery_->setRows(std::move(rows));
}
