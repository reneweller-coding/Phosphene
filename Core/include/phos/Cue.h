/**
 * @file Cue.h
 * @brief Score cues for a visualiser (PLAN 8.3): the OSC encoder, the tap on the play position, and
 *        the sender thread.
 *
 * Kaleidoscope guesses the beat, the bar and the section from the audio it is fed. Phosphene does not
 * have to guess any of it -- it *wrote* the score -- so it says so: `/phos/beat i`, `/phos/bar i`,
 * `/phos/section s f`, `/phos/key s` and `/phos/drop` over UDP.
 *
 * **Where a cue is emitted, and why that is the only place it can be.** Three positions in the chain
 * are candidates, and two of them are wrong:
 *  - *The composer* knows every section boundary long before it is heard, and that is exactly the
 *    problem: phos::Conductor keeps the engine's rings filled about eight bars ahead of the play
 *    position, which at 145 BPM is thirteen seconds. A cue sent where a section is *planned* would
 *    reach the visualiser a quarter of a minute before the drop.
 *  - *The score* has no clock at all. A SectionMark is a beat number, not an instant.
 *  - *The play position* is the one place where a beat is an instant: phos::Engine::beatPosition()
 *    says which beat the next sample to be rendered carries. phos::CueTap therefore takes the beat range a
 *    block covers -- the value before and after Engine::process() -- and emits the boundaries that
 *    fall inside it.
 *
 * That still is not what the listener hears, for two reasons that both point the same way, and both
 * are known numbers rather than guesses:
 *  1. The engine's true-peak limiter looks ahead, so the sound of an event dispatched at engine
 *     sample @e s leaves the engine at output sample @e s + Engine::latencySamples() (83 samples at
 *     the default settings, 1.7 ms at 48 kHz).
 *  2. The block being rendered now is not the block being played now. It is handed to the device and
 *     played once the buffers in front of it have drained.
 * Both are a *lead*: the cue would be early, never late. phos::CueTap::scan() therefore stamps every cue with
 * the instant the listener will hear it -- `now + (outputLatency + limiterLookahead + sampleOffset) /
 * sampleRate` -- and the sender thread holds it back until then. Being a little early is the safer
 * error of the two (a visual can be scheduled, it cannot be un-drawn), so the lead is only ever
 * corrected by a latency that is *reported*, plus a trim the user can dial in.
 *
 * **The audio thread sends nothing.** phos::CueTap::scan() does arithmetic and one wait-free push per cue
 * into a phos::EventRing; phos::CueSender's own thread pops, waits for the due instant and calls sendto().
 * No socket, no allocation and no lock ever touches the audio thread.
 *
 * **Failure is silence.** UDP to a port nobody listens on is not an error anywhere in the stack: the
 * datagram is dropped and the generator plays on. A host that cannot be resolved, a socket that
 * cannot be opened and a full queue are all counted (phos::CueSender::dropped()) and otherwise ignored.
 *
 * @note The OSC encoding is written out here rather than taken from a library: five message types,
 *       three argument types, forty lines. Tests/selftest.cpp checks it against the byte layout of
 *       the OSC 1.0 specification, and Tools/cue_check.py decodes the same bytes with an independent
 *       decoder.
 * @author Rene Weller
 */
#pragma once
#include "phos/Form.h"
#include "phos/Params.h"
#include "phos/Score.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace phos {

/** @brief Default destination of the cue bridge (PLAN 8.3). */
constexpr const char* CueDefaultHost = "127.0.0.1";
constexpr int CueDefaultPort = 9000;   ///< default destination port of the cue bridge

// ---------------------------------------------------------------------------- OSC encoding

/**
 * @brief Builds one OSC 1.0 message into a fixed buffer.
 *
 * The type tag string has to be written before the arguments, so it is given at the start rather
 * than accumulated: every message Phosphene sends has a fixed shape. Everything is big-endian and
 * padded to four bytes, which is the whole of the specification that five messages need. An
 * overlong message sets #ok to false instead of writing past the buffer.
 */
class OscWriter {
public:
    static constexpr int Capacity = 128;   ///< longest message this can build

    /**
     * @brief Starts a message.
     * @param[in] address  the address pattern, e.g. "/phos/section"
     * @param[in] tags     the type tags without the leading comma, e.g. "sf"; "" for a message with no arguments (which
     *                     still carries the tag string "," -- receivers rely on it)
     */
    OscWriter(const char* address, const char* tags)
    {
        putString(address);
        char t[16] = { ',' };
        const size_t n = std::strlen(tags);
        if (n + 2 > sizeof(t)) { m_ok = false; return; }
        std::memcpy(t + 1, tags, n + 1);
        putString(t);
    }

    /**
     * @brief Appends a 32-bit integer (tag 'i').
     * @param[in] v  the integer
     */
    void putInt(int32_t v)
    {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        putWord(u);
    }
    /**
     * @brief Appends a 32-bit float (tag 'f').
     * @param[in] v  the float
     */
    void putFloat(float v)
    {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        putWord(u);
    }
    /**
     * @brief Appends an OSC string (tag 's'): terminated and padded to a multiple of four.
     * @param[in] s  the string
     */
    void putString(const char* s)
    {
        const size_t n = std::strlen(s) + 1;
        if (!m_ok || m_pos + static_cast<int>(n) + 3 > Capacity) { m_ok = false; return; }
        std::memcpy(m_buf + m_pos, s, n);
        m_pos += static_cast<int>(n);
        while (m_pos & 3) m_buf[m_pos++] = 0;
    }

    /**
     * @brief Whether everything fitted.
     * @return true when everything fitted
     */
    bool ok() const { return m_ok; }
    /**
     * @brief The bytes of the message.
     * @return the bytes
     */
    const char* data() const { return m_buf; }
    /**
     * @brief Their number (always a multiple of four).
     * @return the number of bytes
     */
    int size() const { return m_ok ? m_pos : 0; }

private:
    /**
     * @brief Appends @p u big-endian, as OSC wants every word.
     * @param[in] u  the word
     */
    void putWord(uint32_t u)
    {
        if (!m_ok || m_pos + 4 > Capacity) { m_ok = false; return; }
        m_buf[m_pos++] = static_cast<char>((u >> 24) & 0xFF);
        m_buf[m_pos++] = static_cast<char>((u >> 16) & 0xFF);
        m_buf[m_pos++] = static_cast<char>((u >> 8) & 0xFF);
        m_buf[m_pos++] = static_cast<char>(u & 0xFF);
    }
    char m_buf[Capacity] = {};   ///< the message being built
    int  m_pos = 0;   ///< bytes written
    bool m_ok = true;   ///< false once the message would have overflowed
};

// ---------------------------------------------------------------------------- the cues

/**
 * @brief One cue on its way from the audio thread to the sender thread.
 *
 * Trivially copyable on purpose: it travels through a phos::EventRing, which copies rather than
 * allocates. Everything a message needs is a number here; the strings are looked up from the tables
 * (phos::SectionNames, phos::KeyNames, phos::ScaleNames) on the sender thread.
 */
struct Cue {
    /** @brief Which of the five messages of PLAN 8.3 this is. */
    enum class KindE : uint8_t { BEAT = 0, BAR, SECTION, KEY, DROP, COUNT };

    KindE    m_kind = KindE::BEAT;   ///< what to send
    uint8_t m_section = 0;         ///< SectionType, for Kind::Section
    uint8_t m_key = 0;             ///< pitch class 0..11, for Kind::Key
    uint8_t m_scale = 0;           ///< index into ScaleNames, for Kind::Key
    int32_t m_index = 0;           ///< beat or bar number from the start of the set
    float   m_energy = 0.0f;       ///< 0..1 on the energy arc, for Kind::Section
    int64_t m_dueNanos = 0;        ///< steady_clock nanoseconds at which the listener hears it; 0 = at once
};

/**
 * @brief steady_clock nanoseconds; the one clock every due time is measured on.
 * @return the nanoseconds
 */
inline int64_t cueNowNanos()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

/**
 * @brief Writes the key of a cue as the string `/phos/key` carries, e.g. "F# Phrygian".
 * @param[in]  c    the cue
 * @param[out] out  receives the text, at least 32 bytes
 * @param[in]  cap  size of @p out in bytes, the terminating zero included
 */
inline void cueKeyText(const Cue& c, char* out, size_t cap)
{
    const char* root = KeyNames[c.m_key % 12];
    const char* mode = ScaleNames[c.m_scale < 6 ? c.m_scale : 0];
    std::snprintf(out, cap, "%s %s", root, mode);
}

/**
 * @brief Encodes a cue as its OSC message.
 * @param[in]  c    the cue
 * @param[out] buf  receives the bytes
 * @param[in]  cap  size of @p buf
 * @return the number of bytes written, or 0 if the cue does not fit or its kind is unknown
 */
inline int cueToOsc(const Cue& c, char* buf, int cap)
{
    switch (c.m_kind) {
    case Cue::KindE::BEAT: {
        OscWriter w("/phos/beat", "i");
        w.putInt(c.m_index);
        if (!w.ok() || w.size() > cap) return 0;
        std::memcpy(buf, w.data(), static_cast<size_t>(w.size()));
        return w.size();
    }
    case Cue::KindE::BAR: {
        OscWriter w("/phos/bar", "i");
        w.putInt(c.m_index);
        if (!w.ok() || w.size() > cap) return 0;
        std::memcpy(buf, w.data(), static_cast<size_t>(w.size()));
        return w.size();
    }
    case Cue::KindE::SECTION: {
        OscWriter w("/phos/section", "sf");
        const int t = c.m_section < static_cast<int>(SectionTypeE::COUNT) ? c.m_section : 0;
        w.putString(SectionNames[t]);
        w.putFloat(c.m_energy);
        if (!w.ok() || w.size() > cap) return 0;
        std::memcpy(buf, w.data(), static_cast<size_t>(w.size()));
        return w.size();
    }
    case Cue::KindE::KEY: {
        char text[32];
        cueKeyText(c, text, sizeof(text));
        OscWriter w("/phos/key", "s");
        w.putString(text);
        if (!w.ok() || w.size() > cap) return 0;
        std::memcpy(buf, w.data(), static_cast<size_t>(w.size()));
        return w.size();
    }
    case Cue::KindE::DROP: {
        OscWriter w("/phos/drop", "");
        if (!w.ok() || w.size() > cap) return 0;
        std::memcpy(buf, w.data(), static_cast<size_t>(w.size()));
        return w.size();
    }
    default: return 0;
    }
}

// ---------------------------------------------------------------------------- the tap

/**
 * @brief What the composer knows in advance and the audio thread cannot work out for itself.
 *
 * Beats and bars are arithmetic on the beat position; a section boundary and a key are decisions.
 * They are pushed into a phos::EventRing by whichever thread composes, in beat order, and the tap
 * pops them as the play position reaches them -- so the *knowledge* travels ahead of the sound, as
 * it must, and the *cue* does not.
 */
struct CueMark {
    double  m_beat = 0.0;      ///< musical beat from the start of the set on which the section starts
    uint8_t m_type = 0;        ///< SectionType
    float   m_energy = 0.5f;   ///< 0..1 on the energy arc
    uint8_t m_key = 0;         ///< pitch class of the key from here
    uint8_t m_scale = 0;       ///< index into ScaleNames
    uint8_t m_keyChanged = 0;  ///< 1: the key changes here, so `/phos/key` is sent as well
};

/** @brief The ring the composing thread fills and phos::CueTap drains. */
using CueMarkRingT = EventRing<CueMark>;

/**
 * @brief Pushes the marks of one bar, in the order phos::Composer::sections() lists them.
 *
 * The same three rules that build a SectionMark there: the cut at the head of a breakdown, the
 * section itself, and the pre-drop break in the last bar of a buildup. Matching that function
 * exactly is what the offline demo checks -- what a listener receives and what the composer wrote
 * have to be the same list of boundaries, bar for bar. Whoever composes calls this, bars ahead of
 * the sound; only the *sending* waits for the play position.
 * @param[in]     form      the track's form
 * @param[in]     firstBar  the bar the track starts on
 * @param[in]     key       pitch class of the track's key
 * @param[in]     scale     index into phos::ScaleNames
 * @param[in]     bar       the bar being composed
 * @param[in,out] lastKey   the key the caller last announced (-1 = none); updated when a mark goes out
 * @param[out]    out       receives the marks
 */
inline void cueMarksForBar(const FormPlan& form, int firstBar, int key, int scale, int bar,
                           int& lastKey, CueMarkRingT& out)
{
    const int barInTrack = bar - firstBar;
    CueMark base;
    base.m_beat = static_cast<double>(bar) * BeatsPerBar;
    base.m_key = static_cast<uint8_t>(((key % 12) + 12) % 12);
    base.m_scale = static_cast<uint8_t>(scale);
    base.m_keyChanged = key != lastKey ? 1 : 0;

    for (int s = 0; s < form.m_count; ++s) {
        const Section& sec = form.m_section[s];
        if (sec.m_startBar == barInTrack) {
            base.m_energy = sec.m_energy;
            if (sec.m_type == SectionTypeE::BREAK && sec.m_cutBeats > 0.0f) {
                CueMark c = base;
                c.m_type = static_cast<uint8_t>(SectionTypeE::CUT);
                out.push(c);
                base.m_keyChanged = 0;   // the key travels with the first mark of the bar, once
            }
            CueMark m = base;
            m.m_type = static_cast<uint8_t>(sec.m_type);
            out.push(m);
            base.m_keyChanged = 0;
            lastKey = key;
        }
        if (sec.m_type == SectionTypeE::BUILD && sec.m_startBar + sec.m_bars - 1 == barInTrack) {
            CueMark d = base;
            d.m_type = static_cast<uint8_t>(SectionTypeE::PDB);
            // The buildup's own energy, not the energy it climbs to: phos::Composer::sections()
            // gives the pre-drop break the mark of the buildup it belongs to, and the two lists have
            // to be the same list.
            d.m_energy = sec.m_energy;
            out.push(d);
            base.m_keyChanged = 0;
            lastKey = key;
        }
    }
}

/**
 * @brief Pushes one mark describing where a jump landed: the section that contains @p bar.
 *
 * A listener that joins in the middle of a track would otherwise know nothing until the next
 * boundary. The key always travels with it, because after a jump nothing is known.
 * @param[in]     form      the track's form
 * @param[in]     firstBar  the track's first bar
 * @param[in]     key       the key's root
 * @param[in]     scale     the scale
 * @param[in]     bar       the bar landed on
 * @param[in,out] lastKey   the key sent last, updated
 * @param[in,out] out       the marks, pushed to
 */
inline void cueLandingMark(const FormPlan& form, int firstBar, int key, int scale, int bar,
                           int& lastKey, CueMarkRingT& out)
{
    const int si = sectionOfBar(form, bar - firstBar);
    CueMark m;
    m.m_beat = static_cast<double>(bar) * BeatsPerBar;
    m.m_type = static_cast<uint8_t>(form.m_section[si].m_type);
    m.m_energy = form.m_section[si].m_energy;
    m.m_key = static_cast<uint8_t>(((key % 12) + 12) % 12);
    m.m_scale = static_cast<uint8_t>(scale);
    m.m_keyChanged = 1;
    out.push(m);
    lastKey = key;
}
/** @brief The ring phos::CueTap fills and phos::CueSender drains. */
using CueRingT = EventRing<Cue>;

/**
 * @brief Turns the beat range of one audio block into cues, on the audio thread.
 *
 * Stateless apart from the mark ring: every block covers `[beatFrom, beatTo)` and the ranges of
 * consecutive blocks join exactly, so no integer beat is emitted twice and none is skipped. After a
 * seek the range jumps, which costs at most the cues of the bar that was jumped over -- and never a
 * duplicate, because the boundaries are read off the beat numbers themselves rather than counted.
 */
class CueTap {
public:
    /**
     * @brief Whether `/phos/beat` is sent at all (the busiest message; a visualiser may not want it).
     * @param[in] on  true to send the beats
     */
    void setSendBeats(bool on) { m_sendBeats = on; }

    /**
     * @brief Emits every cue whose musical instant falls in `[beatFrom, beatTo)`.
     * @param[in]     beatFrom    musical beat of sample 0 of the block
     * @param[in]     beatTo      musical beat just past its last sample
     * @param[in]     nowNanos    steady_clock time at which the block was handed to the engine
     * @param[in]     leadNanos   how long after @p nowNanos sample 0 of this block reaches the listener (output
     *                            buffering + the limiter's lookahead + the user's trim)
     * @param[in]     blockNanos  how long the block lasts, so a cue inside it can be placed within it
     * @param[in,out] marks       the section and key marks, in beat order
     * @param[out]    out         receives the cues
     * @return how many cues were dropped because @p out was full
     */
    int scan(double beatFrom, double beatTo, int64_t nowNanos, int64_t leadNanos, int64_t blockNanos,
             CueMarkRingT& marks, CueRingT& out)
    {
        int lost = 0;
        if (!(beatTo > beatFrom)) return lost;
        const double span = beatTo - beatFrom;
        auto dueOf = [&](double beat) {
            const double frac = (beat - beatFrom) / span;
            return nowNanos + leadNanos + static_cast<int64_t>(frac * static_cast<double>(blockNanos));
        };
        auto emit = [&](const Cue& c) { if (!out.push(c)) ++lost; };

        // A mark that is already behind the block start is one the tap could not reach -- a seek
        // landed past it. It is not sent late; it is sent now, because a visualiser that never hears
        // "Drop" is worse off than one that hears it a block late.
        CueMark m;
        while (marks.peek(m) && m.m_beat < beatFrom) {
            marks.pop(m);
            emitMark(m, nowNanos + leadNanos, emit);
        }

        const double eps = 1.0e-9;
        for (int64_t b = static_cast<int64_t>(std::ceil(beatFrom - eps)); static_cast<double>(b) < beatTo - eps; ++b) {
            const double beat = static_cast<double>(b);
            const int64_t due = dueOf(beat);
            // The bar goes first, then what that bar *is*, then the beat. A receiver that keeps a
            // bar counter can then say which bar a section started on without having to look ahead,
            // which is exactly what the offline demo checks.
            if (b % BeatsPerBar == 0) {
                Cue c;
                c.m_kind = Cue::KindE::BAR;
                c.m_index = static_cast<int32_t>(b / BeatsPerBar);
                c.m_dueNanos = due;
                emit(c);
            }
            while (marks.peek(m) && m.m_beat <= beat + eps) {
                marks.pop(m);
                emitMark(m, due, emit);
            }
            if (m_sendBeats) {
                Cue c;
                c.m_kind = Cue::KindE::BEAT;
                c.m_index = static_cast<int32_t>(b);
                c.m_dueNanos = due;
                emit(c);
            }
        }
        return lost;
    }

    /**
     * @brief Sends the section and the key again, without a drop.
     *
     * A listener that starts in the middle of a track would otherwise know nothing about where it is
     * until the next boundary, which can be a minute away. The drop is deliberately left out: it is
     * an event, not a state, and re-announcing one would cut the picture for a drop that happened
     * bars ago.
     * @param[in]  due  what to stamp the cues with
     * @param[out] out  receives them
     * @return false if nothing has been seen yet
     */
    bool announce(int64_t due, CueRingT& out) const
    {
        if (!m_haveLast) return false;
        CueMark m = m_last;
        m.m_keyChanged = 1;
        if (m.m_keyChanged) {
            Cue c;
            c.m_kind = Cue::KindE::KEY;
            c.m_key = m.m_key;
            c.m_scale = m.m_scale;
            c.m_dueNanos = due;
            out.push(c);
        }
        Cue s;
        s.m_kind = Cue::KindE::SECTION;
        s.m_section = m.m_type;
        s.m_energy = m.m_energy;
        s.m_dueNanos = due;
        out.push(s);
        return true;
    }

    /** @brief Forgets the last mark (a new set, a new seed). */
    void forget() { m_haveLast = false; }
    /**
     * @brief The last section mark that went out, or null before the first (the family jam's leader reads it).
     * @return the mark, or null
     */
    const CueMark* lastMark() const { return m_haveLast ? &m_last : nullptr; }
    /**
     * @brief How many section marks have gone out so far (the family jam's leader sends each once).
     * @return the number of marks
     */
    uint32_t marksPassed() const { return m_marksPassed; }

private:
    /**
     * @brief A section mark becomes a key cue (if the key moved), a section cue and, on a core, a drop.
     * @param[in] m     the mark
     * @param[in] due   its due time in nanoseconds
     * @param[in] emit  called with every cue
     */
    template <class EmitT>
    void emitMark(const CueMark& m, int64_t due, EmitT&& emit)
    {
        m_last = m;
        m_haveLast = true;
        ++m_marksPassed;
        if (m.m_keyChanged) {
            Cue c;
            c.m_kind = Cue::KindE::KEY;
            c.m_key = m.m_key;
            c.m_scale = m.m_scale;
            c.m_dueNanos = due;
            emit(c);
        }
        Cue s;
        s.m_kind = Cue::KindE::SECTION;
        s.m_section = m.m_type;
        s.m_energy = m.m_energy;
        s.m_dueNanos = due;
        emit(s);
        if (static_cast<SectionTypeE>(m.m_type) == SectionTypeE::DROP) {
            Cue d;
            d.m_kind = Cue::KindE::DROP;
            d.m_dueNanos = due;
            emit(d);
        }
    }

    bool m_sendBeats = true;   ///< cue.beats: a beat cue on every beat, not only the marks
    CueMark m_last;              ///< the last section mark that went out, for #announce
    bool m_haveLast = false;     ///< @copydoc m_last
    uint32_t m_marksPassed = 0;  ///< marksPassed()
};

// ---------------------------------------------------------------------------- the sender

/**
 * @brief Sleeps until an instant with sub-millisecond accuracy.
 *
 * `std::this_thread::sleep_for` on Windows is `Sleep()`, whose granularity is the system timer --
 * 15.6 ms unless some other process has raised it, which is a quarter of a bar at 145 BPM and would
 * make the due times meaningless. A high-resolution waitable timer (Windows 10 1803 and later) gives
 * the accuracy without raising the global timer resolution, which is a system-wide change and not
 * a generator's business. Elsewhere `nanosleep` is already accurate enough.
 * @param[in] nanos  how long to wait; <= 0 returns at once
 */
inline void cueSleepNanos(int64_t nanos)
{
    if (nanos <= 0) return;
#if defined(_WIN32)
    static thread_local HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer != nullptr) {
        LARGE_INTEGER due;
        due.QuadPart = -(nanos / 100);   // negative = relative, in 100 ns units
        if (due.QuadPart == 0) due.QuadPart = -1;
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
#endif
    std::this_thread::sleep_for(std::chrono::nanoseconds(nanos));
}

/**
 * @brief The UDP socket and the thread that empties the cue queue into it.
 *
 * `push()` is called from the audio thread and does nothing but a wait-free ring push. The thread
 * wakes every #PollNanos, sends everything that is due, and waits for the next due instant when
 * there is one -- so a cue is never sent more than a fraction of a millisecond after its time and
 * the thread costs nothing when there is nothing to send.
 *
 * Nothing here ever fails loudly. An unresolvable host or a socket that cannot be opened makes
 * start() return false and the generator plays on; a datagram to a port with no listener is dropped
 * by the operating system without an error, which is the whole reason UDP was chosen.
 */
class CueSender {
public:
    static constexpr int64_t PollNanos = 1000000;   ///< 1 ms: the longest the thread sleeps when idle

    /** @brief A sender with no destination; start() opens it. */
    CueSender() = default;
    CueSender(const CueSender&) = delete;
    CueSender& operator=(const CueSender&) = delete;
    ~CueSender() { stop(); }

    /**
     * @brief Opens the socket and starts the thread.
     * @param[in] host       destination, a name or a dotted address
     * @param[in] port       destination port
     * @param[in] scheduled  true: hold every cue back until its due time (live playback). false: send at once, which is
     *                       what an offline render wants -- there the due times describe a timeline that is not being
     *                       lived through.
     * @return false if the host cannot be resolved or the socket cannot be opened; nothing is started and nothing is
     *         logged, because a visualiser that is not running is not an error
     */
    bool start(const std::string& host, int port, bool scheduled = true)
    {
        stop();
        if (port <= 0 || port > 65535) return false;
#if defined(_WIN32)
        if (!winsockUp()) return false;
#endif
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        addrinfo* res = nullptr;
        const std::string service = std::to_string(port);
        if (getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0 || res == nullptr) return false;
        std::memcpy(&m_to, res->ai_addr, sizeof(sockaddr_in));
        freeaddrinfo(res);

        m_sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (!validSocket(m_sock)) return false;
        m_scheduled = scheduled;
        m_sent.store(0, std::memory_order_relaxed);
        m_dropped.store(0, std::memory_order_relaxed);
        m_queue.clear();
        m_run.store(true, std::memory_order_release);
        m_thread = std::thread([this] { loop(); });
        return true;
    }

    /** @brief Stops the thread and closes the socket; safe to call when nothing is running. */
    void stop()
    {
        m_run.store(false, std::memory_order_release);
        if (m_thread.joinable()) m_thread.join();
        closeSocket();
    }

    /**
     * @brief Whether the thread is running.
     * @return true while the sending thread runs
     */
    bool running() const { return m_run.load(std::memory_order_acquire); }

    /**
     * @brief Queues a cue. Audio thread: wait-free, no allocation, no socket.
     * @param[in] c  the cue
     * @return false if the queue is full (the cue is counted as dropped and forgotten)
     * @pre called on the audio thread
     */
    bool push(const Cue& c)
    {
        if (!m_run.load(std::memory_order_relaxed)) return false;
        if (m_queue.push(c)) return true;
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    /**
     * @brief The queue, for the tap to push into directly.
     * @return the queue
     */
    CueRingT& queue() { return m_queue; }

    /**
     * @brief Datagrams sent since start().
     * @return the datagrams sent
     */
    uint64_t sent() const { return m_sent.load(std::memory_order_relaxed); }
    /**
     * @brief Cues lost to a full queue or an unencodable kind.
     * @return how many cues could not be sent
     */
    uint64_t dropped() const { return m_dropped.load(std::memory_order_relaxed); }
    /**
     * @brief Largest lateness measured against a due time, in nanoseconds (0 when nothing was late).
     *
     * The sender thread's own contribution to the end-to-end delay, and the number the demo prints.
     * @return the lateness in nanoseconds
     */
    int64_t worstLateNanos() const { return m_worstLate.load(std::memory_order_relaxed); }

    /**
     * @brief Sends one cue straight away, off the queue (tests and the offline demo).
     * @param[in] c  the cue
     */
    void sendNow(const Cue& c)
    {
        char buf[OscWriter::Capacity];
        const int n = cueToOsc(c, buf, sizeof(buf));
        if (n <= 0) { m_dropped.fetch_add(1, std::memory_order_relaxed); return; }
        if (!validSocket(m_sock)) return;
        ::sendto(m_sock, buf, n, 0, reinterpret_cast<const sockaddr*>(&m_to), sizeof(m_to));
        m_sent.fetch_add(1, std::memory_order_relaxed);
    }

private:
    /** @brief The sender thread: sends what is due, sleeps at most PollNanos when idle. */
    void loop()
    {
        while (m_run.load(std::memory_order_acquire)) {
            int64_t wait = PollNanos;
            Cue c;
            while (m_queue.peek(c)) {
                const int64_t now = cueNowNanos();
                if (m_scheduled && c.m_dueNanos > now) {
                    wait = c.m_dueNanos - now;
                    break;
                }
                m_queue.pop(c);
                if (m_scheduled && c.m_dueNanos > 0) {
                    const int64_t late = now - c.m_dueNanos;
                    if (late > m_worstLate.load(std::memory_order_relaxed))
                        m_worstLate.store(late, std::memory_order_relaxed);
                }
                sendNow(c);
            }
            cueSleepNanos(wait < PollNanos ? wait : PollNanos);
        }
    }

#if defined(_WIN32)
    using SocketHandleT = SOCKET;
    static bool validSocket(SocketHandleT s) { return s != INVALID_SOCKET; }
    void closeSocket() { if (validSocket(m_sock)) { ::closesocket(m_sock); m_sock = INVALID_SOCKET; } }
    /** @brief Winsock is started once per process and never stopped; the sockets outlive nothing. */
    static bool winsockUp()
    {
        static const bool up = [] {
            WSADATA d;
            return WSAStartup(MAKEWORD(2, 2), &d) == 0;
        }();
        return up;
    }
    SocketHandleT m_sock = INVALID_SOCKET;   ///< the UDP socket
#else
    using SocketHandleT = int;   ///< A socket.
    /**
     * @brief Whether @p s is an open socket.
     * @param[in] s  the socket
     * @return true when it is open
     */
    static bool validSocket(SocketHandleT s) { return s >= 0; }
    /** @brief Closes the socket. */
    void closeSocket() { if (validSocket(m_sock)) { ::close(m_sock); m_sock = -1; } }
    SocketHandleT m_sock = -1;   ///< the UDP socket
#endif

    sockaddr_in m_to{};   ///< the destination
    bool m_scheduled = true;   ///< hold each cue until its due time (false: send at once, the tests)
    std::atomic<bool> m_run{ false };   ///< the thread runs
    std::thread m_thread;   ///< the sender thread
    CueRingT m_queue{ 1024 };   ///< cues waiting to go out
    std::atomic<uint64_t> m_sent{ 0 };   ///< cues sent
    std::atomic<uint64_t> m_dropped{ 0 };   ///< cues that could not be sent
    std::atomic<int64_t> m_worstLate{ 0 };   ///< the latest a cue went out after its due time, ns
};

} // namespace phos
