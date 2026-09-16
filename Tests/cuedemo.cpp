/**
 * @file cuedemo.cpp
 * @brief phos_cuedemo: the Kaleidoscope bridge of PLAN 8.3 demonstrated without ears.
 *
 * Three modes, driven by Tools/cue_check.py:
 *  - `--sections` prints the score's section marks -- bar, type, energy -- and sends nothing. This
 *    is the oracle: it is what phos::Composer::sections() wrote, read straight out of the composer.
 *  - the default renders the set offline with the cue bridge on and the sender in immediate mode,
 *    so the datagrams a listener would receive over a whole track arrive in a second. The checker
 *    decodes them with its own decoder and compares them with the section list, bar for bar.
 *  - `--realtime` renders in real time, with the sender holding every cue back until its due
 *    instant, and receives its own datagrams on a socket of its own. The delay it prints is the
 *    whole chain measured on one clock: the tap's stamp, the queue, the sender's wait, the encoder,
 *    sendto(), the loopback interface and recvfrom().
 *
 * Nothing here is a plugin: the chain is the same one PluginProcessor builds, assembled by hand, so
 * that the demonstration does not need a host, a device or a sound card.
 */
#include "phos/Composer.h"
#include "phos/Cue.h"
#include "phos/Engine.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

using namespace phos;

namespace {

/** @brief One received datagram: when it arrived and which address it carried. */
struct Arrival {
    int64_t nanos = 0;      ///< steady_clock, the same clock the due times are on
    std::string address;    ///< the OSC address pattern
};

/**
 * @brief A UDP socket that records arrivals on its own thread.
 *
 * Deliberately separate from phos::CueSender: the point of the measurement is that the bytes really
 * go through the operating system's loopback, so the receiving end must be a socket and not a
 * function call.
 */
class UdpIn {
public:
    bool open(int port)
    {
#if defined(_WIN32)
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
        sock_ = static_cast<intptr_t>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        if (sock_ == static_cast<intptr_t>(INVALID_SOCKET)) return false;
#else
        sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ < 0) return false;
#endif
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<uint16_t>(port));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int rcv = 4 * 1024 * 1024;
        ::setsockopt(static_cast<int>(sock_), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcv), sizeof(rcv));
        if (::bind(static_cast<int>(sock_), reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) return false;
        run_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }
    void close()
    {
        run_ = false;
#if defined(_WIN32)
        if (sock_ != static_cast<intptr_t>(INVALID_SOCKET)) ::closesocket(static_cast<SOCKET>(sock_));
#else
        if (sock_ >= 0) ::close(static_cast<int>(sock_));
#endif
        if (thread_.joinable()) thread_.join();
    }
    /** @brief What has arrived so far (read after close()). */
    const std::vector<Arrival>& arrivals() const { return arrivals_; }

private:
    void loop()
    {
        char buf[512];
        while (run_) {
            const int n = static_cast<int>(::recv(static_cast<int>(sock_), buf, sizeof(buf), 0));
            if (n <= 0) break;
            Arrival a;
            a.nanos = cueNowNanos();
            // The address pattern is the first null-terminated string of the message; that is all
            // this needs to tell the five kinds apart.
            a.address.assign(buf, static_cast<size_t>(std::min<size_t>(static_cast<size_t>(n), std::strlen(buf))));
            arrivals_.push_back(a);
        }
    }
    intptr_t sock_ = -1;
    bool run_ = false;
    std::thread thread_;
    std::vector<Arrival> arrivals_;
};

/** @brief The address a cue will carry, for matching an arrival to its due time. */
const char* addressOf(Cue::Kind k)
{
    switch (k) {
    case Cue::Kind::Beat: return "/phos/beat";
    case Cue::Kind::Bar: return "/phos/bar";
    case Cue::Kind::Section: return "/phos/section";
    case Cue::Kind::Key: return "/phos/key";
    default: return "/phos/drop";
    }
}

int usage()
{
    std::printf("phos_cuedemo [--bars N] [--seed S] [--host H] [--port P] [--sections] [--realtime] [--nobeats]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    int bars = 96, port = kCueDefaultPort;
    uint64_t seed = 1;
    std::string host = kCueDefaultHost;
    bool sectionsOnly = false, realtime = false, beats = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--bars") bars = std::atoi(next());
        else if (a == "--seed") seed = std::strtoull(next(), nullptr, 10);
        else if (a == "--host") host = next();
        else if (a == "--port") port = std::atoi(next());
        else if (a == "--sections") sectionsOnly = true;
        else if (a == "--realtime") realtime = true;
        else if (a == "--nobeats") beats = false;
        else return usage();
    }
    if (bars < 1) return usage();

    // The knobs of the demo: a short track so a whole form fits in the run, and the two expensive
    // probe renders off, because what is being demonstrated is the timing of the cues and not the
    // loudness of the mix.
    ParamStore params;
    std::string err;
    if (!params.parseText("compose.track_bars=128 compose.level_match=Off master.auto_gain=Off", &err)) {
        std::fprintf(stderr, "knobs: %s\n", err.c_str());
        return 1;
    }
    Composer composer(seed);

    if (sectionsOnly) {
        // The oracle, printed the way the checker reads it: one line per section mark of the score.
        for (const SectionMark& m : composer.sections(params, bars)) {
            const int bar = static_cast<int>(m.beat / kBeatsPerBar);
            if (bar >= bars) continue;
            std::printf("section %d %s %.4f\n", bar, kSectionNames[static_cast<int>(m.type)], m.energy);
        }
        // The key of every track, in the same form, so the checker can count them.
        for (int i = 0;; ++i) {
            const TrackPlan t = composer.track(params, i);
            if (t.firstBar >= bars) break;
            std::printf("key %d %s %s\n", t.firstBar, kKeyNames[((t.key % 12) + 12) % 12], kScaleNames[t.scale]);
            if (t.firstBar + t.bars >= bars) break;
        }
        std::printf("bars %d\n", bars);
        std::fflush(stdout);
        return 0;
    }

    const double sr = 48000.0;
    const int block = 256;
    Engine engine;
    engine.prepare(sr, block);
    engine.params().copyValuesFrom(params);
    Conductor conductor(engine, composer);

    CueSender sender;
    CueTap tap;
    tap.setSendBeats(beats);
    CueMarkRing marks(1024);
    CueRing staged(4096);
    if (!sender.start(host, port, realtime)) {
        std::fprintf(stderr, "cue bridge: cannot reach %s:%d\n", host.c_str(), port);
        return 1;
    }

    UdpIn in;
    const bool listening = realtime && in.open(port);
    if (realtime && !listening) {
        std::fprintf(stderr, "cannot listen on port %d; the delay cannot be measured\n", port);
        return 1;
    }

    // What was staged, with the instant the tap said the listener would hear it. The arrivals are
    // matched against this list in order, which loopback preserves.
    struct Staged { int64_t due; const char* address; };
    std::vector<Staged> staging;

    const double lead = static_cast<double>(block + engine.latencySamples()) / sr;
    const int64_t leadNanos = static_cast<int64_t>(lead * 1.0e9);
    const int64_t blockNanos = static_cast<int64_t>(static_cast<double>(block) / sr * 1.0e9);
    int lastKey = -1, nextBar = 0;
    std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
    const double endBeat = static_cast<double>(bars) * kBeatsPerBar;
    int64_t playhead = cueNowNanos();

    while (engine.beatPosition() < endBeat) {
        conductor.pump(engine.params(), 8 * kBeatsPerBar);
        while (nextBar < conductor.nextBar() && nextBar < bars) {
            const TrackPlan plan = composer.track(params, composer.trackOfBar(params, nextBar));
            cueMarksForBar(plan.form, plan.firstBar, plan.key, plan.scale, nextBar, lastKey, marks);
            ++nextBar;
        }
        const double from = engine.beatPosition();
        engine.process(L.data(), R.data(), block);
        const int64_t now = cueNowNanos();
        // Clamped to the end of the run: the last block overshoots the bar count, and a demo that
        // announced a bar it did not render would be comparing two different things.
        tap.scan(from, std::min(engine.beatPosition(), endBeat), now, leadNanos, blockNanos, marks, staged);
        Cue c;
        while (staged.pop(c)) {
            staging.push_back({ c.dueNanos, addressOf(c.kind) });
            sender.push(c);
        }
        if (realtime) {
            // A device consumes one block every block. Waiting here is what makes the wall clock and
            // the musical clock the same clock, which is the only condition under which a due time
            // means anything.
            playhead += blockNanos;
            cueSleepNanos(playhead - cueNowNanos());
        }
    }

    // Let the queue empty: in the scheduled mode the last cues are still waiting for their instant.
    cueSleepNanos(realtime ? 400000000 : 200000000);
    sender.stop();
    if (listening) {
        cueSleepNanos(50000000);
        in.close();
    }

    std::printf("staged %d\n", static_cast<int>(staging.size()));
    std::printf("sent %llu\n", static_cast<unsigned long long>(sender.sent()));
    std::printf("dropped %llu\n", static_cast<unsigned long long>(sender.dropped()));

    if (listening) {
        const std::vector<Arrival>& got = in.arrivals();
        std::vector<double> delay;
        size_t matched = 0, mismatched = 0;
        for (size_t i = 0; i < got.size(); ++i) {
            if (i >= staging.size()) break;
            if (got[i].address != staging[i].address) { ++mismatched; continue; }
            ++matched;
            delay.push_back(static_cast<double>(got[i].nanos - staging[i].due) * 1.0e-6);
        }
        std::printf("received %d\n", static_cast<int>(got.size()));
        std::printf("matched %d\n", static_cast<int>(matched));
        std::printf("mismatched %d\n", static_cast<int>(mismatched));
        if (!delay.empty()) {
            std::vector<double> sorted = delay;
            std::sort(sorted.begin(), sorted.end());
            double sum = 0.0;
            for (double d : delay) sum += d;
            std::printf("delay_ms_mean %.3f\n", sum / static_cast<double>(delay.size()));
            std::printf("delay_ms_median %.3f\n", sorted[sorted.size() / 2]);
            std::printf("delay_ms_min %.3f\n", sorted.front());
            std::printf("delay_ms_p95 %.3f\n", sorted[static_cast<size_t>(0.95 * static_cast<double>(sorted.size() - 1))]);
            std::printf("delay_ms_max %.3f\n", sorted.back());
        }
    }
    std::fflush(stdout);
    return 0;
}
