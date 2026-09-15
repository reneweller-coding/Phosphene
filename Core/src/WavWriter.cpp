/**
 * @file WavWriter.cpp
 * @brief Streaming WAV writer.
 *
 * The header is written twice: a provisional one at open() so the data starts at a fixed offset,
 * and the final one at close(). Both are 'RIFF' with a 'JUNK' chunk of 28 bytes after 'WAVE'; if
 * the data turned out larger than a 32-bit size allows, close() rewrites 'RIFF' as 'RF64' and the
 * 'JUNK' chunk as the 'ds64' chunk that carries the 64-bit sizes (EBU Tech 3306).
 */
#include "phos/WavWriter.h"
#include <cmath>
#include <cstring>
#include <vector>

namespace phos {

namespace {
void le16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
void le32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
void le64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i)); }
constexpr int kHeaderBytes = 12 + 36 + 24 + 8;   ///< RIFF/WAVE + JUNK/ds64 + fmt + data header
}

bool WavWriter::open(const char* path, int sampleRate, int channels, WavFormat format)
{
    close();
    f_ = std::fopen(path, "wb");
    if (f_ == nullptr) return false;
    sampleRate_ = sampleRate;
    channels_ = channels == 1 ? 1 : 2;
    format_ = format;
    frames_ = 0;
    clipped_ = 0;
    return writeHeader(false);
}

bool WavWriter::writeHeader(bool final)
{
    const int bytesPerSample = format_ == WavFormat::Float32 ? 4 : 3;
    const uint64_t dataBytes = frames_ * static_cast<uint64_t>(channels_ * bytesPerSample);
    const uint64_t riffBytes = dataBytes + kHeaderBytes - 8;
    const bool rf64 = final && riffBytes > 0xFFFFFFFFull;

    uint8_t h[kHeaderBytes] = {};
    std::memcpy(h, rf64 ? "RF64" : "RIFF", 4);
    le32(h + 4, rf64 ? 0xFFFFFFFFu : static_cast<uint32_t>(riffBytes));
    std::memcpy(h + 8, "WAVE", 4);
    std::memcpy(h + 12, rf64 ? "ds64" : "JUNK", 4);
    le32(h + 16, 28);
    if (rf64) {
        le64(h + 20, riffBytes);
        le64(h + 28, dataBytes);
        le64(h + 36, frames_);
        le32(h + 44, 0);
    }
    std::memcpy(h + 48, "fmt ", 4);
    le32(h + 52, 16);
    le16(h + 56, format_ == WavFormat::Float32 ? 3 : 1);
    le16(h + 58, static_cast<uint16_t>(channels_));
    le32(h + 60, static_cast<uint32_t>(sampleRate_));
    le32(h + 64, static_cast<uint32_t>(sampleRate_ * channels_ * bytesPerSample));
    le16(h + 68, static_cast<uint16_t>(channels_ * bytesPerSample));
    le16(h + 70, static_cast<uint16_t>(bytesPerSample * 8));
    std::memcpy(h + 72, "data", 4);
    le32(h + 76, rf64 ? 0xFFFFFFFFu : static_cast<uint32_t>(dataBytes));
    if (std::fseek(f_, 0, SEEK_SET) != 0) return false;
    return std::fwrite(h, 1, sizeof(h), f_) == sizeof(h);
}

bool WavWriter::write(const float* L, const float* R, int n)
{
    if (f_ == nullptr || n <= 0) return f_ != nullptr;
    if (std::fseek(f_, 0, SEEK_END) != 0) return false;
    const int bps = format_ == WavFormat::Float32 ? 4 : 3;
    std::vector<uint8_t> buf(static_cast<size_t>(n) * static_cast<size_t>(channels_ * bps));
    uint8_t* p = buf.data();
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < channels_; ++c) {
            const float v = (c == 0 || R == nullptr) ? L[i] : R[i];
            if (format_ == WavFormat::Float32) {
                std::memcpy(p, &v, 4);
                p += 4;
            } else {
                double x = static_cast<double>(v) * 8388608.0;
                if (x > 8388607.0) { x = 8388607.0; ++clipped_; }
                if (x < -8388608.0) { x = -8388608.0; ++clipped_; }
                const int32_t s = static_cast<int32_t>(std::lround(x));
                p[0] = uint8_t(s); p[1] = uint8_t(s >> 8); p[2] = uint8_t(s >> 16);
                p += 3;
            }
        }
    }
    frames_ += static_cast<uint64_t>(n);
    return std::fwrite(buf.data(), 1, buf.size(), f_) == buf.size();
}

bool WavWriter::close()
{
    if (f_ == nullptr) return true;
    bool ok = writeHeader(true);
    ok = std::fclose(f_) == 0 && ok;
    f_ = nullptr;
    return ok;
}

} // namespace phos
