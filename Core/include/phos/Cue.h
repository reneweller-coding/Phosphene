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
 *    says which beat the next sample to be rendered carries. #CueTap therefore takes the beat range a
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
 * Both are a *lead*: the cue would be early, never late. #CueTap::scan therefore stamps every cue with
 * the instant the listener will hear it -- `now + (outputLatency + limiterLookahead + sampleOffset) /
 * sampleRate` -- and the sender thread holds it back until then. Being a little early is the safer
 * error of the two (a visual can be scheduled, it cannot be un-drawn), so the lead is only ever
 * corrected by a latency that is *reported*, plus a trim the user can dial in.
 *
 * **The audio thread sends nothing.** #CueTap::scan does arithmetic and one wait-free push per cue
 * into a phos::EventRing; #CueSender's own thread pops, waits for the due instant and calls sendto().
 * No socket, no allocation and no lock ever touches the audio thread.
 *
 * **Failure is silence.** UDP to a port nobody listens on is not an error anywhere in the stack: the
 * datagram is dropped and the generator plays on. A host that cannot be resolved, a socket that
 * cannot be opened and a full queue are all counted (#CueSender::dropped) and otherwise ignored.
 *
 * @note The OSC encoding is written out here rather than taken from a library: five message types,
 *       three argument types, forty lines. Tests/selftest.cpp checks it against the byte layout of
 *       the OSC 1.0 specification, and Tools/cue_check.py decodes the same bytes with an independent
 *       decoder.
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
constexpr const char* kCueDefaultHost = "127.0.0.1";
constexpr int kCueDefaultPort = 9000;   ///< @copydoc kCueDefaultHost

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
    static constexpr int kCapacity = 128;   ///< longest message this can build

    /**
     * @brief Starts a message.
     * @param address the address pattern, e.g. "/phos/section"
     * @param tags    the type tags without the leading comma, e.g. "sf"; "" for a message with no
     *                arguments (which still carries the tag string "," -- receivers rely on it)
     */
    OscWriter(const char* address, const char* tags)
    {
        putString(address);
        char t[16] = { ',' };
        const size_t n = std::strlen(tags);
        if (n + 2 > sizeof(t)) { ok_ = false; return; }
        std::memcpy(t + 1, tags, n + 1);
        putString(t);
    }

    /** @brief Appends a 32-bit integer (tag 'i'). */
    void putInt(int32_t v)
    {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        putWord(u);
    }
    /** @brief Appends a 32-bit float (tag 'f'). */
    void putFloat(float v)
    {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        putWord(u);
    }
    /** @brief Appends an OSC string (tag 's'): terminated and padded to a multiple of four. */
    void putString(const char* s)
    {
        const size_t n = std::strlen(s) + 1;
        if (!ok_ || pos_ + static_cast<int>(n) + 3 > kCapacity) { ok_ = false; return; }
        std::memcpy(buf_ + pos_, s, n);
        pos_ += static_cast<int>(n);
        while (pos_ & 3) buf_[pos_++] = 0;
    }

    /** @brief Whether everything fitted. */
    bool ok() const { return ok_; }
    /** @brief The bytes of the message. */
    const char* data() const { return buf_; }
    /** @brief Their number (always a multiple of four). */
    int size() const { return ok_ ? pos_ : 0; }

private:
    void putWord(uint32_t u)
    {
        if (!ok_ || pos_ + 4 > kCapacity) { ok_ = false; return; }
        buf_[pos_++] = static_cast<char>((u >> 24) & 0xFF);
        buf_[pos_++] = static_cast<char>((u >> 16) & 0xFF);
        buf_[pos_++] = static_cast<char>((u >> 8) & 0xFF);
        buf_[pos_++] = static_cast<char>(u & 0xFF);
    }
    char buf_[kCapacity] = {};
    int  pos_ = 0;
    bool ok_ = true;
};

// ---------------------------------------------------------------------------- the cues

/**
 * @brief One cue on its way from the audio thread to the sender thread.
 *
 * Trivially copyable on purpose: it travels through a phos::EventRing, which copies rather than
 * allocates. Everything a message needs is a number here; the strings are looked up from the tables
 * (phos::kSectionNames, phos::kKeyNames, phos::kScaleNames) on the sender thread.
 */
struct Cue {
    /** @brief Which of the five messages of PLAN 8.3 this is. */
    enum class Kind : uint8_t { Beat = 0, Bar, Section, Key, Drop, Count };

    Kind    kind = Kind::Beat;   ///< what to send
    uint8_t section = 0;         ///< SectionType, for Kind::Section
    uint8_t key = 0;             ///< pitch class 0..11, for Kind::Key
    uint8_t scale = 0;           ///< index into kScaleNames, for Kind::Key
    int32_t index = 0;           ///< beat or bar number from the start of the set
    float   energy = 0.0f;       ///< 0..1 on the energy arc, for Kind::Section
    int64_t dueNanos = 0;        ///< steady_clock nanoseconds at which the listener hears it; 0 = at once
};

/** @brief steady_clock nanoseconds; the one clock every due time is measured on. */
inline int64_t cueNowNanos()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

/**
 * @brief Writes the key of a cue as the string `/phos/key` carries, e.g. "F# Phrygian".
 * @param c   the cue
 * @param out receives the text, at least 32 bytes
 */
inline void cueKeyText(const Cue& c, char* out, size_t cap)
{
    const char* root = kKeyNames[c.key % 12];
    const char* mode = kScaleNames[c.scale < 6 ? c.scale : 0];
    std::snprintf(out, cap, "%s %s", root, mode);
}

/**
 * @brief Encodes a cue as its OSC message.
 * @param c   the cue
 * @param buf receives the bytes
 * @param cap size of @p buf
 * @return the number of bytes written, or 0 if the cue does not fit or its kind is unknown
 */
inline int cueToOsc(const Cue& c, char* buf, int cap)
{
    switch (c.kind) {
    case Cue::Kind::Beat: {
        OscWriter w("/phos/beat", "i");
        w.putInt(c.index);
        if (!w.ok() || w.size() > cap) return 0;
        std::memcpy(buf, w.data(), static_cast<size_t>(w.size()));
        return w.size();
    }
    case Cue::Kind::Bar: {
        OscWriter w("/phos/bar", "i");
        w.putInt(c.index);
        if (!w.ok() || w.size() > cap) return 0;
        std::memcpy(buf, w.data(), static_cast<size_t>(w.size()));
        return w.size();
    }
    case Cue::Kind::Section: {
        OscWriter w("/phos/section", "sf");
        const int t = c.section < static_cast<int>(SectionType::Count) ? c.section : 0;
        w.putString(kSectionNames[t]);
        w.putFloat(c.energy);
        if (!w.ok() || w.size() > cap) return 0;
        std::memcpy(buf, w.data(), static_cast<size_t>(w.size()));
        return w.size();
    }
    case Cue::Kind::Key: {
        char text[32];
        cueKeyText(c, text, sizeof(text));
        OscWriter w("/phos/key", "s");
        w.putString(text);
        if (!w.ok() || w.size() > cap) return 0;
        std::memcpy(buf, w.data(), static_cast<size_t>(w.size()));
        return w.size();
    }
    case Cue::Kind::Drop: {
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
    double  beat = 0.0;      ///< musical beat from the start of the set on which the section starts
    uint8_t type = 0;        ///< SectionType
    float   energy = 0.5f;   ///< 0..1 on the energy arc
    uint8_t key = 0;         ///< pitch class of the key from here
    uint8_t scale = 0;       ///< index into kScaleNames
    uint8_t keyChanged = 0;  ///< 1: the key changes here, so `/phos/key` is sent as well
};

/** @brief The ring the composing thread fills and #CueTap drains. */
using CueMarkRing = EventRing<CueMark>;

/**
 * @brief Pushes the marks of one bar, in the order phos::Composer::sections() lists them.
 *
 * The same three rules that build a SectionMark there: the cut at the head of a breakdown, the
 * section itself, and the pre-drop break in the last bar of a buildup. Matching that function
 * exactly is what the offline demo checks -- what a listener receives and what the composer wrote
 * have to be the same list of boundaries, bar for bar. Whoever composes calls this, bars ahead of
 * the sound; only the *sending* waits for the play position.
 *
 * @param form      the track's form
 * @param firstBar  the bar the track starts on
 * @param key       pitch class of the track's key
 * @param scale     index into phos::kScaleNames
 * @param bar       the bar being composed
 * @param lastKey   the key the caller last announced (-1 = none); updated when a mark goes out
 * @param out       receives the marks
 */
inline void cueMarksForBar(const FormPlan& form, int firstBar, int key, int scale, int bar,
                           int& lastKey, CueMarkRing& out)
{
    const int barInTrack = bar - firstBar;
    CueMark base;
    base.beat = static_cast<double>(bar) * kBeatsPerBar;
    base.key = static_cast<uint8_t>(((key % 12) + 12) % 12);
    base.scale = static_cast<uint8_t>(scale);
    base.keyChanged = key != lastKey ? 1 : 0;

    for (int s = 0; s < form.count; ++s) {
        const Section& sec = form.section[s];
        if (sec.startBar == barInTrack) {
            base.energy = sec.energy;
            if (sec.type == SectionType::Break && sec.cutBeats > 0.0f) {
                CueMark c = base;
                c.type = static_cast<uint8_t>(SectionType::Cut);
                out.push(c);
                base.keyChanged = 0;   // the key travels with the first mark of the bar, once
            }
            CueMark m = base;
            m.type = static_cast<uint8_t>(sec.type);
            out.push(m);
            base.keyChanged = 0;
            lastKey = key;
        }
        if (sec.type == SectionType::Build && sec.startBar + sec.bars - 1 == barInTrack) {
            CueMark d = base;
            d.type = static_cast<uint8_t>(SectionType::Pdb);
            // The buildup's own energy, not the energy it climbs to: phos::Composer::sections()
            // gives the pre-drop break the mark of the buildup it belongs to, and the two lists have
            // to be the same list.
            d.energy = sec.energy;
            out.push(d);
            base.keyChanged = 0;
            lastKey = key;
        }
    }
}

/**
 * @brief Pushes one mark describing where a jump landed: the section that contains @p bar.
 *
 * A listener that joins in the middle of a track would otherwise know nothing until the next
 * boundary. The key always travels with it, because after a jump nothing is known.
 */
inline void cueLandingMark(const FormPlan& form, int firstBar, int key, int scale, int bar,
                           int& lastKey, CueMarkRing& out)
{
    const int si = sectionOfBar(form, bar - firstBar);
    CueMark m;
    m.beat = static_cast<double>(bar) * kBeatsPerBar;
    m.type = static_cast<uint8_t>(form.section[si].type);
    m.energy = form.section[si].energy;
    m.key = static_cast<uint8_t>(((key % 12) + 12) % 12);
    m.scale = static_cast<uint8_t>(scale);
    m.keyChanged = 1;
    out.push(m);
    lastKey = key;
}
/** @brief The ring #CueTap fills and #CueSender drains. */
using CueRing = EventRing<Cue>;

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
    /** @brief Whether `/phos/beat` is sent at all (the busiest message; a visualiser may not want it). */
    void setSendBeats(bool on) { sendBeats_ = on; }

    /**
     * @brief Emits every cue whose musical instant falls in `[beatFrom, beatTo)`.
     * @param beatFrom   musical beat of sample 0 of the block
     * @param beatTo     musical beat just past its last sample
     * @param nowNanos   steady_clock time at which the block was handed to the engine
     * @param leadNanos  how long after @p nowNanos sample 0 of this block reaches the listener
     *                   (output buffering + the limiter's lookahead + the user's trim)
     * @param blockNanos how long the block lasts, so a cue inside it can be placed within it
     * @param marks      the section and key marks, in beat order
     * @param out        receives the cues
     * @return how many cues were dropped because @p out was full
     */
    int scan(double beatFrom, double beatTo, int64_t nowNanos, int64_t leadNanos, int64_t blockNanos,
             CueMarkRing& marks, CueRing& out)
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
        while (marks.peek(m) && m.beat < beatFrom) {
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
            if (b % kBeatsPerBar == 0) {
                Cue c;
                c.kind = Cue::Kind::Bar;
                c.index = static_cast<int32_t>(b / kBeatsPerBar);
                c.dueNanos = due;
                emit(c);
            }
            while (marks.peek(m) && m.beat <= beat + eps) {
                marks.pop(m);
                emitMark(m, due, emit);
            }
            if (sendBeats_) {
                Cue c;
                c.kind = Cue::Kind::Beat;
                c.index = static_cast<int32_t>(b);
                c.dueNanos = due;
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
     * @param due what to stamp the cues with
     * @param out receives them
     * @return false if nothing has been seen yet
     */
    bool announce(int64_t due, CueRing& out) const
    {
        if (!haveLast_) return false;
        CueMark m = last_;
        m.keyChanged = 1;
        if (m.keyChanged) {
            Cue c;
            c.kind = Cue::Kind::Key;
            c.key = m.key;
            c.scale = m.scale;
            c.dueNanos = due;
            out.push(c);
        }
        Cue s;
        s.kind = Cue::Kind::Section;
        s.section = m.type;
        s.energy = m.energy;
        s.dueNanos = due;
        out.push(s);
        return true;
    }

    /** @brief Forgets the last mark (a new set, a new seed). */
    void forget() { haveLast_ = false; }

private:
    /** @brief A section mark becomes a key cue (if the key moved), a section cue and, on a core, a drop. */
    template <class Emit>
    void emitMark(const CueMark& m, int64_t due, Emit&& emit)
    {
        last_ = m;
        haveLast_ = true;
        if (m.keyChanged) {
            Cue c;
            c.kind = Cue::Kind::Key;
            c.key = m.key;
            c.scale = m.scale;
            c.dueNanos = due;
            emit(c);
        }
        Cue s;
        s.kind = Cue::Kind::Section;
        s.section = m.type;
        s.energy = m.energy;
        s.dueNanos = due;
        emit(s);
        if (static_cast<SectionType>(m.type) == SectionType::Drop) {
            Cue d;
            d.kind = Cue::Kind::Drop;
            d.dueNanos = due;
            emit(d);
        }
    }

    bool sendBeats_ = true;
    CueMark last_;              ///< the last section mark that went out, for #announce
    bool haveLast_ = false;     ///< @copydoc last_
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
 * @param nanos how long to wait; <= 0 returns at once
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
 * wakes every #kPollNanos, sends everything that is due, and waits for the next due instant when
 * there is one -- so a cue is never sent more than a fraction of a millisecond after its time and
 * the thread costs nothing when there is nothing to send.
 *
 * Nothing here ever fails loudly. An unresolvable host or a socket that cannot be opened makes
 * start() return false and the generator plays on; a datagram to a port with no listener is dropped
 * by the operating system without an error, which is the whole reason UDP was chosen.
 */
class CueSender {
public:
    static constexpr int64_t kPollNanos = 1000000;   ///< 1 ms: the longest the thread sleeps when idle

    CueSender() = default;
    CueSender(const CueSender&) = delete;
    CueSender& operator=(const CueSender&) = delete;
    ~CueSender() { stop(); }

    /**
     * @brief Opens the socket and starts the thread.
     * @param host      destination, a name or a dotted address
     * @param port      destination port
     * @param scheduled true: hold every cue back until its due time (live playback). false: send at
     *                  once, which is what an offline render wants -- there the due times describe a
     *                  timeline that is not being lived through.
     * @return false if the host cannot be resolved or the socket cannot be opened; nothing is started
     *         and nothing is logged, because a visualiser that is not running is not an error
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
        std::memcpy(&to_, res->ai_addr, sizeof(sockaddr_in));
        freeaddrinfo(res);

        sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (!validSocket(sock_)) return false;
        scheduled_ = scheduled;
        sent_.store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
        queue_.clear();
        run_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    /** @brief Stops the thread and closes the socket; safe to call when nothing is running. */
    void stop()
    {
        run_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
        closeSocket();
    }

    /** @brief Whether the thread is running. */
    bool running() const { return run_.load(std::memory_order_acquire); }

    /**
     * @brief Queues a cue. Audio thread: wait-free, no allocation, no socket.
     * @return false if the queue is full (the cue is counted as dropped and forgotten)
     */
    bool push(const Cue& c)
    {
        if (!run_.load(std::memory_order_relaxed)) return false;
        if (queue_.push(c)) return true;
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    /** @brief The queue, for the tap to push into directly. */
    CueRing& queue() { return queue_; }

    /** @brief Datagrams sent since start(). */
    uint64_t sent() const { return sent_.load(std::memory_order_relaxed); }
    /** @brief Cues lost to a full queue or an unencodable kind. */
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    /**
     * @brief Largest lateness measured against a due time, in nanoseconds (0 when nothing was late).
     *
     * The sender thread's own contribution to the end-to-end delay, and the number the demo prints.
     */
    int64_t worstLateNanos() const { return worstLate_.load(std::memory_order_relaxed); }

    /** @brief Sends one cue straight away, off the queue (tests and the offline demo). */
    void sendNow(const Cue& c)
    {
        char buf[OscWriter::kCapacity];
        const int n = cueToOsc(c, buf, sizeof(buf));
        if (n <= 0) { dropped_.fetch_add(1, std::memory_order_relaxed); return; }
        if (!validSocket(sock_)) return;
        ::sendto(sock_, buf, n, 0, reinterpret_cast<const sockaddr*>(&to_), sizeof(to_));
        sent_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    void loop()
    {
        while (run_.load(std::memory_order_acquire)) {
            int64_t wait = kPollNanos;
            Cue c;
            while (queue_.peek(c)) {
                const int64_t now = cueNowNanos();
                if (scheduled_ && c.dueNanos > now) {
                    wait = c.dueNanos - now;
                    break;
                }
                queue_.pop(c);
                if (scheduled_ && c.dueNanos > 0) {
                    const int64_t late = now - c.dueNanos;
                    if (late > worstLate_.load(std::memory_order_relaxed))
                        worstLate_.store(late, std::memory_order_relaxed);
                }
                sendNow(c);
            }
            cueSleepNanos(wait < kPollNanos ? wait : kPollNanos);
        }
    }

#if defined(_WIN32)
    using SocketHandle = SOCKET;
    static bool validSocket(SocketHandle s) { return s != INVALID_SOCKET; }
    void closeSocket() { if (validSocket(sock_)) { ::closesocket(sock_); sock_ = INVALID_SOCKET; } }
    /** @brief Winsock is started once per process and never stopped; the sockets outlive nothing. */
    static bool winsockUp()
    {
        static const bool up = [] {
            WSADATA d;
            return WSAStartup(MAKEWORD(2, 2), &d) == 0;
        }();
        return up;
    }
    SocketHandle sock_ = INVALID_SOCKET;
#else
    using SocketHandle = int;
    static bool validSocket(SocketHandle s) { return s >= 0; }
    void closeSocket() { if (validSocket(sock_)) { ::close(sock_); sock_ = -1; } }
    SocketHandle sock_ = -1;
#endif

    sockaddr_in to_{};
    bool scheduled_ = true;
    std::atomic<bool> run_{ false };
    std::thread thread_;
    CueRing queue_{ 1024 };
    std::atomic<uint64_t> sent_{ 0 }, dropped_{ 0 };
    std::atomic<int64_t> worstLate_{ 0 };
};

} // namespace phos
