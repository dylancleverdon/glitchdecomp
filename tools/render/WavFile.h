#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace splinter::render
{

/** Sample encodings the reader understands. Renders are written as Pcm24 or Float32; the
    writer handles the others too so every readable format can be round-tripped. */
enum class WavSampleFormat
{
    Pcm16,
    Pcm24,
    Pcm32,
    Float32,
};

const char* sampleFormatName (WavSampleFormat format) noexcept;

/** Decoded audio. Integer samples are scaled to [-1, 1); float samples are passed through. */
struct WavAudio
{
    double sampleRate = 44100.0;
    int fileChannels = 0; // channels stored in the file; only the first two are decoded
    WavSampleFormat format = WavSampleFormat::Pcm16;
    int validBits = 16;   // bits actually used, which WAVE_FORMAT_EXTENSIBLE may set below the container size
    std::vector<std::vector<float>> channels; // one or two channels of equal length

    size_t numFrames() const noexcept { return channels.empty() ? 0 : channels.front().size(); }
};

struct WavReadResult
{
    bool ok = false;
    std::string error; // what was wrong with the file, when ok is false
    WavAudio audio;
};

struct WavEncodeResult
{
    bool ok = false;
    std::string error;
    std::vector<uint8_t> bytes; // a complete RIFF/WAVE file
};

struct WavWriteResult
{
    bool ok = false;
    std::string error;
};

/** Parses a complete RIFF/WAVE file held in memory: PCM 16/24/32-bit integer or 32-bit float,
    plain or WAVE_FORMAT_EXTENSIBLE, any channel count (the first two are kept). Unknown chunks
    are skipped; truncated or malformed files are rejected with a description of the problem. */
WavReadResult decodeWav (const uint8_t* data, size_t size);

WavReadResult readWavFile (const std::string& path);

/** Encodes equally long channels as a canonical little-endian WAV file. Integer formats round
    to the nearest step and clip at full scale (NaN becomes silence); Float32 stores the samples as they are. */
WavEncodeResult encodeWav (const std::vector<std::vector<float>>& channels, double sampleRate, WavSampleFormat format);

WavWriteResult writeWavFile (const std::string& path, const std::vector<std::vector<float>>& channels,
                             double sampleRate, WavSampleFormat format);

} // namespace splinter::render
