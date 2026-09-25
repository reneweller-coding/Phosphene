/**
 * @file MidiMap.h
 * @brief MIDI controllers on any parameter (23.09.2026, round "MIDI"): learn, bind, forget, save.
 *
 * Every parameter of Phosphene is a host parameter, so a DAW can automate all of them; what a player on
 * stage needs besides is a hardware knob on a parameter, without a DAW in between. A MidiMap is that table:
 * (channel, controller number) -> target. A *target* is an index the owner defines -- the plugin uses the
 * store's parameter ids and, above them, its own extra host parameters (the four performance macros) --
 * so the map knows nothing of what it drives.
 *
 * **Learning.** arm(target) marks a target; the next controller message that arrives binds its channel and
 * number to it (replacing whatever that controller drove before, and whatever controller drove that
 * target before: one knob, one parameter, both ways) and disarms. That message is applied at once, so the
 * knob takes over from where it stands.
 *
 * **Threads.** arm(), bind(), unbind(), clear() and the text form are for the message thread; handleCc() is
 * for the audio thread. The table is atomics throughout, so neither side ever locks or allocates on the
 * audio thread.
 *
 * **The text form** names targets through callbacks (the plugin passes parameter keys), so a saved map
 * survives parameters being added to the store: `cc <channel 1..16> <number 0..127> <key>` per line.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace phos {

/** @brief One binding, as the message thread lists them. */
struct MidiBinding {
    int channel = 0;   ///< 0..15
    int cc = 0;        ///< 0..127
    int target = -1;   ///< the owner's target index
};

/** @brief Controller-to-target table with learning (see the file comment). */
class MidiMap {
public:
    static constexpr int kChannels = 16;   ///< MIDI channels
    static constexpr int kControllers = 128;   ///< controllers per channel

    /** @brief An empty map: no controller bound. */
    MidiMap();

    /** @brief The next controller message binds to @p target (message thread); -1 disarms. */
    void arm(int target) { armed_.store(target, std::memory_order_release); }
    /** @brief The target waiting for a controller, or -1. */
    int armed() const { return armed_.load(std::memory_order_acquire); }

    /**
     * @brief A controller message arrived (audio thread).
     * @param channel 0..15
     * @param cc      0..127
     * @param value   0..127
     * @param normalised receives value / 127
     * @return the target it drives (just learned, or bound before), or -1 for an unbound controller
     */
    int handleCc(int channel, int cc, int value, float& normalised);

    /** @brief Binds a controller to a target, releasing both from any earlier binding (message thread). */
    void bind(int channel, int cc, int target);
    /** @brief Releases whatever controller drives @p target. */
    void unbind(int target);
    /** @brief Forgets every binding and disarms. */
    void clear();
    /** @brief The controller driving @p target, or false. */
    bool controllerOf(int target, int& channel, int& cc) const;
    /** @brief Every binding, by channel and number. */
    std::vector<MidiBinding> bindings() const;
    /** @brief A counter that moves on every change of the table (so a display knows when to redraw). */
    uint32_t revision() const { return revision_.load(std::memory_order_acquire); }

    /** @brief The bindings as text, one `cc <channel> <number> <name>` line each; @p name gives a target's key. */
    std::string toText(const std::function<std::string(int)>& name) const;
    /**
     * @brief Replaces the bindings with the ones in @p text; lines whose key @p find does not know (returns -1)
     *        are skipped. @return how many bindings were read
     */
    int fromText(const std::string& text, const std::function<int(const std::string&)>& find);

private:
    /** @brief Binds a controller to a target, releasing both from any other binding. */
    void bindLocked(int channel, int cc, int target);
    std::atomic<int32_t> table_[kChannels * kControllers];   ///< target + 1, 0 = unbound
    std::atomic<int32_t> armed_{ -1 };   ///< the target MIDI learn waits for, -1 none
    std::atomic<uint32_t> revision_{ 0 };   ///< bumped on every change (the editor redraws)
};

} // namespace phos
