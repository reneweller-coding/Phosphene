/**
 * @file Engine.cpp
 * @brief Engine implementation.
 */
#include "phos/Engine.h"
#include <algorithm>
#include <cmath>

namespace phos {

Engine::Engine() : ring_(16384) {}

void Engine::prepare(double sampleRate, int maxBlockSize)
{
    sr_ = sampleRate;
    maxBlock_ = std::max(kChunk, maxBlockSize);
    kickBuf_.assign(static_cast<size_t>(kChunk), 0.0f);
    bassBuf_.assign(static_cast<size_t>(kChunk), 0.0f);
    kick_.prepare(sr_);
    bass_.prepare(sr_);
    reset();
}

void Engine::reset()
{
    NoteEvent e;
    while (ring_.pop(e)) {}
    samples_ = 0;
    chunkBeat_ = 0.0;
    chunkPos_ = 0;
    beatsPerSample_ = 0.0;
    beatNow_.store(0.0, std::memory_order_relaxed);
    kick_.reset();
    bass_.reset();
    clipL_.reset();
    clipR_.reset();
}

void Engine::applyParams()
{
    const ParamStore& p = params_;
    const int cb = p.base(Module::Compose);
    keyRoot_ = p.getInt(cb + compose::Key);
    const double bpm = useTempoMap_ ? tempo_.bpmAt(chunkBeat_) : static_cast<double>(p.get(cb + compose::Bpm));
    beatsPerSample_ = bpm / 60.0 / sr_;
    kick_.update(p, p.base(Module::Kick), keyRoot_);
    bass_.update(p, p.base(Module::Bass));
    const int mb = p.base(Module::Mix);
    kickMute_ = p.getBool(mb + mix::KickMute);
    bassMute_ = p.getBool(mb + mix::BassMute);
    const int ms = p.base(Module::Master);
    masterGain_ = dbToGain(p.get(ms + master::Gain));
    ceiling_ = dbToGain(p.get(ms + master::Ceiling));
    clip_ = p.getBool(ms + master::Clip);
}

void Engine::dispatch(const NoteEvent& e)
{
    const float vel = static_cast<float>(e.velocity) / 127.0f;
    switch (e.part) {
    case Part::Kick:
        kick_.trigger(vel);
        bass_.duck();
        break;
    case Part::Bass: {
        const double samples = static_cast<double>(e.length) / beatsPerSample_;
        bass_.noteOn(e.pitch, vel, std::max(1, static_cast<int>(std::lround(samples))));
        break;
    }
    default:
        break;
    }
}

void Engine::renderSegment(float* L, float* R, int offset, int count)
{
    if (count <= 0) return;
    kick_.process(kickBuf_.data(), count);
    bass_.process(bassBuf_.data(), count);
    const float kg = kickMute_ ? 0.0f : 1.0f, bg = bassMute_ ? 0.0f : 1.0f;
    const float invCeil = 1.0f / ceiling_;
    for (int i = 0; i < count; ++i) {
        // Kick and bass are mono and centred; the master is stereo from here on.
        float x = (kg * kickBuf_[static_cast<size_t>(i)] + bg * bassBuf_[static_cast<size_t>(i)]) * masterGain_;
        float l = x, r = x;
        if (clip_) {
            l = ceiling_ * clipL_(l * invCeil);
            r = ceiling_ * clipR_(r * invCeil);
        }
        L[offset + i] = l;
        R[offset + i] = r;
    }
    chunkPos_ += count;
    samples_ += static_cast<uint64_t>(count);
}

void Engine::process(float* L, float* R, int n)
{
    int done = 0;
    while (done < n) {
        if (chunkPos_ == 0) applyParams();
        int left = std::min(n - done, kChunk - chunkPos_);

        NoteEvent e;
        while (left > 0 && ring_.peek(e)) {
            // First sample of this chunk whose beat has reached the event.
            const double rel = (e.beat - chunkBeat_) / beatsPerSample_;
            int m = rel <= 0.0 ? 0 : static_cast<int>(std::ceil(rel - 1e-7));
            if (m < chunkPos_) m = chunkPos_;
            if (m >= chunkPos_ + left) break;
            const int before = m - chunkPos_;
            renderSegment(L, R, done, before);
            done += before;
            left -= before;
            ring_.pop(e);
            dispatch(e);
        }
        renderSegment(L, R, done, left);
        done += left;

        if (chunkPos_ >= kChunk) {
            chunkBeat_ += kChunk * beatsPerSample_;
            chunkPos_ = 0;
        }
        beatNow_.store(chunkBeat_ + chunkPos_ * beatsPerSample_, std::memory_order_relaxed);
    }
}

} // namespace phos
