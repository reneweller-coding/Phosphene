/**
 * @file WavWriter.h
 * @brief Streaming WAV writer for renders and stems.
 *
 * Writes as it goes, so a two-hour set does not have to sit in memory. 32-bit float or 24-bit PCM.
 * A RIFF file cannot exceed 4 GiB; beyond that the writer switches the header to RF64 (EBU Tech
 * 3306) on close, which every current editor reads.
 */
#pragma once
#include <cstdint>
#include <cstdio>

namespace phos {

/** @brief Sample format of a written file. */
enum class WavFormat { Float32, Pcm24 };

/** @brief Streaming stereo or mono WAV writer. */
class WavWriter {
public:
    WavWriter() = default;
    ~WavWriter() { close(); }
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    /**
     * @brief Creates the file and writes a provisional header.
     * @param path       file name
     * @param sampleRate frames per second
     * @param channels   1 or 2
     * @param format     sample format
     * @return false if the file cannot be created
     */
    bool open(const char* path, int sampleRate, int channels, WavFormat format);
    /** @brief Appends @p n frames; @p R is ignored for mono and may be null. */
    bool write(const float* L, const float* R, int n);
    /** @brief Finalises the header and closes the file. */
    bool close();
    /** @brief Frames written so far. */
    uint64_t frames() const { return frames_; }

private:
    bool writeHeader(bool final);
    FILE* f_ = nullptr;
    int sampleRate_ = 48000, channels_ = 2;
    WavFormat format_ = WavFormat::Float32;
    uint64_t frames_ = 0;
    uint32_t clipped_ = 0;
};

} // namespace phos
