#include "TestFramework.h"
#include "TestSignals.h"

#include "render/DemoLoop.h"
#include "render/OfflineRender.h"
#include "render/RenderOptions.h"
#include "render/WavFile.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using namespace splinter;
using namespace splinter::render;
using namespace splinter::test;

namespace
{
    using Bytes = std::vector<uint8_t>;

    // An independent little-endian WAV builder, so the reader is not only tested against the writer.
    void putText (Bytes& out, const char* text)
    {
        out.insert (out.end(), text, text + std::strlen (text));
    }

    void putUint16 (Bytes& out, uint32_t value)
    {
        out.push_back (static_cast<uint8_t> (value & 0xFF));
        out.push_back (static_cast<uint8_t> ((value >> 8) & 0xFF));
    }

    void putUint32 (Bytes& out, uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
            out.push_back (static_cast<uint8_t> ((value >> shift) & 0xFF));
    }

    void putInt24 (Bytes& out, int32_t value)
    {
        const auto bits = static_cast<uint32_t> (value);
        out.push_back (static_cast<uint8_t> (bits & 0xFF));
        out.push_back (static_cast<uint8_t> ((bits >> 8) & 0xFF));
        out.push_back (static_cast<uint8_t> ((bits >> 16) & 0xFF));
    }

    void putFloat (Bytes& out, float value)
    {
        uint32_t bits;
        std::memcpy (&bits, &value, sizeof (bits));
        putUint32 (out, bits);
    }

    struct Chunk
    {
        std::string id;
        Bytes body;
        bool pad = true; // add the pad byte after an odd-sized body
    };

    Bytes makeRiff (const std::vector<Chunk>& chunks, const char* formType = "WAVE")
    {
        Bytes body;
        putText (body, formType);

        for (const auto& chunk : chunks)
        {
            putText (body, chunk.id.c_str());
            putUint32 (body, static_cast<uint32_t> (chunk.body.size()));
            body.insert (body.end(), chunk.body.begin(), chunk.body.end());

            if (chunk.pad && (chunk.body.size() & 1u) != 0)
                body.push_back (0);
        }

        Bytes file;
        putText (file, "RIFF");
        putUint32 (file, static_cast<uint32_t> (body.size()));
        file.insert (file.end(), body.begin(), body.end());
        return file;
    }

    Bytes formatBody (uint32_t formatCode, uint32_t channels, uint32_t sampleRate, uint32_t bits, uint32_t blockAlign = 0)
    {
        if (blockAlign == 0)
            blockAlign = channels * bits / 8;

        Bytes body;
        putUint16 (body, formatCode);
        putUint16 (body, channels);
        putUint32 (body, sampleRate);
        putUint32 (body, sampleRate * blockAlign);
        putUint16 (body, blockAlign);
        putUint16 (body, bits);
        return body;
    }

    /** WAVE_FORMAT_EXTENSIBLE fmt body; subFormat is 1 (PCM) or 3 (float). */
    Bytes extensibleBody (uint32_t subFormat, uint32_t channels, uint32_t sampleRate, uint32_t containerBits, uint32_t validBits,
                          bool validGuid = true)
    {
        Bytes body = formatBody (0xFFFE, channels, sampleRate, containerBits);
        putUint16 (body, 22);       // cbSize
        putUint16 (body, validBits);
        putUint32 (body, channels == 2 ? 3u : 0u); // speaker mask
        putUint16 (body, subFormat);
        const uint8_t tail[14] = { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, validGuid ? uint8_t { 0x71 } : uint8_t { 0x72 } };
        body.insert (body.end(), tail, tail + sizeof (tail));
        return body;
    }

    /** Interleaved 16-bit samples: frame f, channel c holds (f * 100 + c * 1000 + 7). */
    Bytes countingPcm16 (int numFrames, int numChannels)
    {
        Bytes data;

        for (int frame = 0; frame < numFrames; ++frame)
            for (int channel = 0; channel < numChannels; ++channel)
                putUint16 (data, static_cast<uint32_t> (static_cast<int16_t> (frame * 100 + channel * 1000 + 7)) & 0xFFFF);

        return data;
    }

    float countingValue (int frame, int channel)
    {
        return static_cast<float> (frame * 100 + channel * 1000 + 7) / 32768.0f;
    }

    Bytes validStereo16 (int numFrames = 50)
    {
        return makeRiff ({ { "fmt ", formatBody (1, 2, 44100, 16) }, { "data", countingPcm16 (numFrames, 2) } });
    }

    WavReadResult decode (const Bytes& bytes)
    {
        return decodeWav (bytes.data(), bytes.size());
    }

    bool matchesCounting (const WavAudio& audio, int numFrames)
    {
        if (audio.channels.size() != 2 || audio.numFrames() != static_cast<size_t> (numFrames))
            return false;

        for (int frame = 0; frame < numFrames; ++frame)
            for (int channel = 0; channel < 2; ++channel)
                if (audio.channels[static_cast<size_t> (channel)][static_cast<size_t> (frame)] != countingValue (frame, channel))
                    return false;

        return true;
    }

    std::vector<std::vector<float>> testSignal (int numChannels, int numFrames)
    {
        std::vector<std::vector<float>> channels;

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto noise = makeNoise (numFrames, static_cast<uint32_t> (channel + 1), 0.99f).left;

            // Negative full scale (positive full scale is not representable in integer PCM),
            // silence and a few exact values at the start, in a different order per channel.
            const float specials[] = { 0.0f, 0.5f, -0.5f, -1.0f, 0.25f, -0.125f };
            constexpr size_t numSpecials = std::size (specials);

            for (size_t i = 0; i < numSpecials && i < noise.size(); ++i)
                noise[i] = specials[channel % 2 == 0 ? i : numSpecials - 1 - i];

            channels.push_back (std::move (noise));
        }

        return channels;
    }

    float maxDifference (const std::vector<float>& a, const std::vector<float>& b)
    {
        if (a.size() != b.size())
            return std::numeric_limits<float>::infinity();

        float result = 0.0f;

        for (size_t i = 0; i < a.size(); ++i)
            result = std::max (result, std::fabs (a[i] - b[i]));

        return result;
    }

    bool contains (const std::string& text, const char* part)
    {
        return text.find (part) != std::string::npos;
    }

    /** A temporary file path no other test run (or agent) will use at the same time. */
    std::string uniqueTempPath (const char* name)
    {
        static int counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto file = "splinter_test_" + std::to_string (stamp) + "_" + std::to_string (++counter) + "_" + name;
        return (std::filesystem::temp_directory_path() / file).string();
    }

    ParseResult parse (std::vector<std::string> arguments)
    {
        return parseCommandLine (arguments);
    }

    bool sameSequencer (const SequencerParams& a, const SequencerParams& b)
    {
        return a.numSteps == b.numSteps && a.steps == b.steps && a.stepBeats == b.stepBeats && a.chance == b.chance;
    }
} // namespace

// ---------------------------------------------------------------------------
// WAV reading and writing
// ---------------------------------------------------------------------------

TEST_CASE ("wav round trip for every format and channel count")
{
    struct FormatCase
    {
        WavSampleFormat format;
        float tolerance; // half an integer step, or float rounding of a 32-bit integer
    };

    const FormatCase formats[] { { WavSampleFormat::Pcm16, 0.5f / 32768.0f + 1.0e-9f },
                                 { WavSampleFormat::Pcm24, 0.5f / 8388608.0f + 1.0e-9f },
                                 { WavSampleFormat::Pcm32, 1.0e-7f },
                                 { WavSampleFormat::Float32, 0.0f } };

    for (const auto& formatCase : formats)
    {
        for (const int numChannels : { 1, 2, 3 })
        {
            for (const int numFrames : { 0, 1, 777 }) // odd sizes exercise the pad byte
            {
                for (const double sampleRate : { 22050.0, 44100.0, 48000.0, 96000.0 })
                {
                    const auto signal = testSignal (numChannels, numFrames);
                    const auto encoded = encodeWav (signal, sampleRate, formatCase.format);
                    CHECK (encoded.ok);
                    CHECK ((encoded.bytes.size() & 1u) == 0);

                    const auto decoded = decode (encoded.bytes);
                    CHECK (decoded.ok);
                    CHECK (decoded.error.empty());
                    CHECK (decoded.audio.sampleRate == sampleRate);
                    CHECK (decoded.audio.fileChannels == numChannels);
                    CHECK (decoded.audio.format == formatCase.format);
                    CHECK (decoded.audio.channels.size() == static_cast<size_t> (std::min (numChannels, 2)));
                    CHECK (decoded.audio.numFrames() == static_cast<size_t> (numFrames));

                    for (size_t channel = 0; channel < decoded.audio.channels.size(); ++channel)
                        CHECK (maxDifference (decoded.audio.channels[channel], signal[channel]) <= formatCase.tolerance);
                }
            }
        }
    }
}

TEST_CASE ("wav files round trip on disk")
{
    const auto path = uniqueTempPath ("roundtrip.wav");
    const auto signal = testSignal (2, 1000);

    for (const auto format : { WavSampleFormat::Pcm24, WavSampleFormat::Float32 })
    {
        const auto written = writeWavFile (path, signal, 48000.0, format);
        CHECK (written.ok);

        const auto read = readWavFile (path);
        CHECK (read.ok);
        CHECK (read.audio.format == format);
        CHECK (read.audio.numFrames() == 1000);
        CHECK (read.audio.channels.size() == 2);

        if (read.audio.channels.size() == 2)
            CHECK (maxDifference (read.audio.channels[1], signal[1]) <= 0.5f / 8388608.0f + 1.0e-9f);
    }

    std::remove (path.c_str());

    // Errors name the file.
    const auto missing = readWavFile (path + ".missing");
    CHECK (! missing.ok);
    CHECK (contains (missing.error, "missing"));

    const auto unwritable = writeWavFile ("/nonexistent-directory-for-splinter/out.wav", signal, 48000.0, WavSampleFormat::Pcm24);
    CHECK (! unwritable.ok);
    CHECK (contains (unwritable.error, "nonexistent-directory-for-splinter"));

    // A file that is not a WAV file reports the path and the problem.
    const auto garbagePath = uniqueTempPath ("garbage.wav");

    if (std::FILE* file = std::fopen (garbagePath.c_str(), "wb"))
    {
        std::fputs ("this is not audio at all, just some text", file);
        std::fclose (file);
    }

    const auto garbage = readWavFile (garbagePath);
    CHECK (! garbage.ok);
    CHECK (contains (garbage.error, "garbage.wav"));
    CHECK (contains (garbage.error, "RIFF"));
    std::remove (garbagePath.c_str());
}

TEST_CASE ("wav writer produces canonical headers")
{
    const std::vector<std::vector<float>> stereo (2, std::vector<float> (10, 0.0f));

    const auto pcm = encodeWav (stereo, 48000.0, WavSampleFormat::Pcm24);
    CHECK (pcm.ok);
    CHECK (pcm.bytes.size() == 44 + 60);

    Bytes expectedHeader;
    putText (expectedHeader, "RIFF");
    putUint32 (expectedHeader, 36 + 60);
    putText (expectedHeader, "WAVE");
    putText (expectedHeader, "fmt ");
    putUint32 (expectedHeader, 16);
    const auto formatChunk = formatBody (1, 2, 48000, 24);
    expectedHeader.insert (expectedHeader.end(), formatChunk.begin(), formatChunk.end());
    putText (expectedHeader, "data");
    putUint32 (expectedHeader, 60);
    CHECK (std::equal (expectedHeader.begin(), expectedHeader.end(), pcm.bytes.begin()));

    // Float: an 18-byte fmt chunk with cbSize = 0 and a fact chunk holding the frame count.
    const auto floats = encodeWav (stereo, 44100.0, WavSampleFormat::Float32);
    CHECK (floats.ok);
    CHECK (floats.bytes.size() == 12 + 26 + 12 + 8 + 80);
    CHECK (std::memcmp (floats.bytes.data() + 12, "fmt ", 4) == 0);
    CHECK (floats.bytes[16] == 18);
    CHECK (floats.bytes[20] == 3);
    CHECK (std::memcmp (floats.bytes.data() + 38, "fact", 4) == 0);
    CHECK (floats.bytes[46] == 10);
    CHECK (std::memcmp (floats.bytes.data() + 50, "data", 4) == 0);
}

TEST_CASE ("wav writer rounds and clips integer formats")
{
    const std::vector<std::vector<float>> values { { 1.0f, -1.0f, 0.5f, 2.0f, -3.0f, std::nanf (""),
                                                     -std::numeric_limits<float>::infinity(), 1.0f / 8388608.0f, 0.4f / 8388608.0f } };

    const auto encoded = encodeWav (values, 48000.0, WavSampleFormat::Pcm24);
    CHECK (encoded.ok);

    const uint8_t* data = encoded.bytes.data() + 44;
    auto sampleAt = [data] (size_t index) {
        auto value = static_cast<int32_t> (data[3 * index] | (data[3 * index + 1] << 8) | (data[3 * index + 2] << 16));
        return (value & 0x800000) != 0 ? value - 0x1000000 : value;
    };

    CHECK (sampleAt (0) == 8388607);
    CHECK (sampleAt (1) == -8388608);
    CHECK (sampleAt (2) == 4194304);
    CHECK (sampleAt (3) == 8388607);
    CHECK (sampleAt (4) == -8388608);
    CHECK (sampleAt (5) == 0);
    CHECK (sampleAt (6) == -8388608);
    CHECK (sampleAt (7) == 1);
    CHECK (sampleAt (8) == 0);

    // 16-bit too, and float keeps out-of-range values untouched.
    const auto pcm16 = decode (encodeWav (values, 48000.0, WavSampleFormat::Pcm16).bytes);
    CHECK (pcm16.ok);
    CHECK (pcm16.audio.channels[0][0] == 32767.0f / 32768.0f);
    CHECK (pcm16.audio.channels[0][4] == -1.0f);

    const auto floats = decode (encodeWav (values, 48000.0, WavSampleFormat::Float32).bytes);
    CHECK (floats.ok);
    CHECK (floats.audio.channels[0][3] == 2.0f);
    CHECK (std::isnan (floats.audio.channels[0][5]));
}

TEST_CASE ("wav writer rejects unusable input")
{
    CHECK (! encodeWav ({}, 48000.0, WavSampleFormat::Pcm24).ok);
    CHECK (! encodeWav ({ { 0.0f, 0.0f }, { 0.0f } }, 48000.0, WavSampleFormat::Pcm24).ok);
    CHECK (! encodeWav ({ { 0.0f } }, 0.0, WavSampleFormat::Pcm24).ok);
    CHECK (! encodeWav ({ { 0.0f } }, std::nan (""), WavSampleFormat::Float32).ok);
    CHECK (! encodeWav ({ { 0.0f } }, -44100.0, WavSampleFormat::Pcm16).ok);
    CHECK (encodeWav ({ { 0.0f } }, 48000.0, WavSampleFormat::Pcm16).ok);

    const auto mismatched = encodeWav ({ { 0.0f, 0.0f }, { 0.0f } }, 48000.0, WavSampleFormat::Pcm24);
    CHECK (contains (mismatched.error, "same length"));
}

TEST_CASE ("wav reader keeps the first two of many channels")
{
    const auto quad = makeRiff ({ { "fmt ", formatBody (1, 4, 48000, 16) }, { "data", countingPcm16 (64, 4) } });
    const auto decoded = decode (quad);

    CHECK (decoded.ok);
    CHECK (decoded.audio.fileChannels == 4);
    CHECK (matchesCounting (decoded.audio, 64));

    // Six-channel 24-bit extensible: channel c of frame f holds c * 100000 + f.
    Bytes data;

    for (int frame = 0; frame < 30; ++frame)
        for (int channel = 0; channel < 6; ++channel)
            putInt24 (data, channel * 100000 + frame);

    const auto surround = decode (makeRiff ({ { "fmt ", extensibleBody (1, 6, 48000, 24, 24) }, { "data", data } }));
    CHECK (surround.ok);
    CHECK (surround.audio.fileChannels == 6);
    CHECK (surround.audio.channels.size() == 2);
    CHECK (surround.audio.channels[0][29] == 29.0f / 8388608.0f);
    CHECK (surround.audio.channels[1][29] == 100029.0f / 8388608.0f);

    // Mono stays mono.
    const auto mono = decode (makeRiff ({ { "fmt ", formatBody (1, 1, 8000, 16) }, { "data", countingPcm16 (10, 1) } }));
    CHECK (mono.ok);
    CHECK (mono.audio.channels.size() == 1);
    CHECK (mono.audio.channels[0][9] == countingValue (9, 0));
}

TEST_CASE ("wav reader parses extensible headers")
{
    // 16-bit PCM.
    const auto pcm16 = decode (makeRiff ({ { "fmt ", extensibleBody (1, 2, 44100, 16, 16) }, { "data", countingPcm16 (20, 2) } }));
    CHECK (pcm16.ok);
    CHECK (pcm16.audio.format == WavSampleFormat::Pcm16);
    CHECK (matchesCounting (pcm16.audio, 20));

    // 24 valid bits in a 32-bit container: left-justified, so the low byte is zero.
    Bytes data;
    putUint32 (data, static_cast<uint32_t> (0x123456) << 8);
    putUint32 (data, static_cast<uint32_t> (-0x400000) << 8);
    const auto padded = decode (makeRiff ({ { "fmt ", extensibleBody (1, 2, 96000, 32, 24) }, { "data", data } }));
    CHECK (padded.ok);
    CHECK (padded.audio.format == WavSampleFormat::Pcm32);
    CHECK (padded.audio.validBits == 24);
    CHECK (padded.audio.sampleRate == 96000.0);
    CHECK_NEAR (padded.audio.channels[0][0], 0x123456 / 8388608.0, 1.0e-7);
    CHECK_NEAR (padded.audio.channels[1][0], -0.5, 1.0e-7);

    // 32-bit float.
    Bytes floats;
    putFloat (floats, 0.75f);
    putFloat (floats, -0.25f);
    const auto floatFile = decode (makeRiff ({ { "fmt ", extensibleBody (3, 2, 48000, 32, 32) }, { "data", floats } }));
    CHECK (floatFile.ok);
    CHECK (floatFile.audio.format == WavSampleFormat::Float32);
    CHECK (floatFile.audio.channels[0][0] == 0.75f);
    CHECK (floatFile.audio.channels[1][0] == -0.25f);

    // Valid bits left at zero mean "all of them".
    const auto zeroValid = decode (makeRiff ({ { "fmt ", extensibleBody (1, 2, 44100, 16, 0) }, { "data", countingPcm16 (4, 2) } }));
    CHECK (zeroValid.ok);
    CHECK (zeroValid.audio.validBits == 16);

    // Rejected: more valid bits than the container, an unknown sub-format GUID, a short extension.
    const auto tooManyBits = decode (makeRiff ({ { "fmt ", extensibleBody (1, 2, 44100, 16, 24) }, { "data", countingPcm16 (4, 2) } }));
    CHECK (! tooManyBits.ok);
    CHECK (contains (tooManyBits.error, "valid bits"));

    const auto badGuid = decode (makeRiff ({ { "fmt ", extensibleBody (1, 2, 44100, 16, 16, false) }, { "data", countingPcm16 (4, 2) } }));
    CHECK (! badGuid.ok);
    CHECK (contains (badGuid.error, "sub-format"));

    const auto adpcmGuid = decode (makeRiff ({ { "fmt ", extensibleBody (2, 2, 44100, 16, 16) }, { "data", countingPcm16 (4, 2) } }));
    CHECK (! adpcmGuid.ok);
    CHECK (contains (adpcmGuid.error, "0x0002"));

    auto shortBody = extensibleBody (1, 2, 44100, 16, 16);
    shortBody.resize (24);
    const auto shortExtension = decode (makeRiff ({ { "fmt ", shortBody }, { "data", countingPcm16 (4, 2) } }));
    CHECK (! shortExtension.ok);
    CHECK (contains (shortExtension.error, "EXTENSIBLE"));
}

TEST_CASE ("wav reader skips unknown chunks and odd layouts")
{
    Bytes listBody;
    putText (listBody, "INFOISFT");
    putUint32 (listBody, 5);
    putText (listBody, "test");
    listBody.push_back (0);

    Bytes fact;
    putUint32 (fact, 40);

    const Bytes junk (7, 0xEE); // odd size: followed by a pad byte

    // Metadata before and after the audio, odd-sized chunks, data before fmt.
    const auto file = makeRiff ({ { "JUNK", junk }, { "LIST", listBody }, { "data", countingPcm16 (40, 2) },
                                  { "bext", Bytes (13, 0x11) }, { "fmt ", formatBody (1, 2, 44100, 16) },
                                  { "fact", fact }, { "cue ", Bytes (3, 0x22) } });
    const auto decoded = decode (file);
    CHECK (decoded.ok);
    CHECK (matchesCounting (decoded.audio, 40));

    // An 18-byte PCM fmt chunk (cbSize = 0) is common too.
    auto formatWithExtension = formatBody (1, 2, 44100, 16);
    putUint16 (formatWithExtension, 0);
    CHECK (decode (makeRiff ({ { "fmt ", formatWithExtension }, { "data", countingPcm16 (5, 2) } })).ok);

    // Trailing bytes after the RIFF form are ignored...
    auto trailing = validStereo16 (10);
    trailing.insert (trailing.end(), { 'g', 'a', 'r', 'b', 0xFF, 0xFF, 0xFF, 0x7F, 1, 2, 3 });
    const auto withTrailing = decode (trailing);
    CHECK (withTrailing.ok);
    CHECK (matchesCounting (withTrailing.audio, 10));

    // ...and a RIFF size that is too small does not hide a complete data chunk.
    auto understated = validStereo16 (10);
    understated[4] = 20; // RIFF size: only covers the fmt chunk header
    understated[5] = understated[6] = understated[7] = 0;
    const auto fromUnderstated = decode (understated);
    CHECK (fromUnderstated.ok);
    CHECK (matchesCounting (fromUnderstated.audio, 10));

    // A missing pad byte after an odd-sized final chunk is tolerated.
    const auto unpadded = makeRiff ({ { "fmt ", formatBody (1, 1, 44100, 24) }, { "data", Bytes (9, 0) , false } });
    const auto fromUnpadded = decode (unpadded);
    CHECK (fromUnpadded.ok);
    CHECK (fromUnpadded.audio.numFrames() == 3);
}

TEST_CASE ("wav reader rejects garbage and malformed headers")
{
    struct BadFile
    {
        const char* what;
        Bytes bytes;
        const char* expectedInError;
    };

    const auto data = countingPcm16 (8, 2);
    const auto riffWith = [&data] (Bytes formatChunk) { return makeRiff ({ { "fmt ", std::move (formatChunk) }, { "data", data } }); };

    auto noise = makeNoise (300, 42, 1.0f).left;
    Bytes randomBytes;

    for (const float sample : noise)
        randomBytes.push_back (static_cast<uint8_t> (static_cast<int> ((sample + 1.0f) * 127.5f) & 0xFF));

    Bytes riffx = validStereo16();
    std::memcpy (riffx.data(), "RIFX", 4);
    Bytes rf64 = validStereo16();
    std::memcpy (rf64.data(), "RF64", 4);

    auto hugeChunk = validStereo16();
    hugeChunk[40] = hugeChunk[41] = hugeChunk[42] = hugeChunk[43] = 0xFF; // data size

    const std::vector<BadFile> badFiles {
        { "empty", {}, "too short" },
        { "a few bytes", { 'R', 'I', 'F', 'F' }, "too short" },
        { "text", { 'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', '!', '!' }, "RIFF" },
        { "random bytes", randomBytes, "RIFF" },
        { "big-endian", riffx, "RIFX" },
        { "RF64", rf64, "RF64" },
        { "not WAVE", makeRiff ({ { "fmt ", formatBody (1, 2, 44100, 16) } }, "AVI "), "WAVE" },
        { "no fmt", makeRiff ({ { "data", data } }), "no fmt" },
        { "no data", makeRiff ({ { "fmt ", formatBody (1, 2, 44100, 16) } }), "no data" },
        { "fmt too small", riffWith (Bytes (12, 0)), "too small" },
        { "no channels", riffWith (formatBody (1, 0, 44100, 16, 4)), "no channels" },
        { "zero sample rate", riffWith (formatBody (1, 2, 0, 16)), "sample rate" },
        { "8-bit PCM", riffWith (formatBody (1, 2, 44100, 8)), "bit depth: 8" },
        { "12-bit PCM", riffWith (formatBody (1, 2, 44100, 12, 4)), "bit depth: 12" },
        { "64-bit float", riffWith (formatBody (3, 2, 44100, 64)), "float bit depth" },
        { "ADPCM", riffWith (formatBody (2, 2, 44100, 4, 2048)), "0x0002" },
        { "mu-law", riffWith (formatBody (7, 2, 44100, 8)), "0x0007" },
        { "block align", riffWith (formatBody (1, 2, 44100, 16, 6)), "block align" },
        { "two fmt chunks", makeRiff ({ { "fmt ", formatBody (1, 2, 44100, 16) }, { "fmt ", formatBody (1, 2, 44100, 16) }, { "data", data } }), "more than one fmt" },
        { "two data chunks", makeRiff ({ { "fmt ", formatBody (1, 2, 44100, 16) }, { "data", data }, { "data", data } }), "more than one data" },
        { "chunk larger than the file", hugeChunk, "truncated" },
    };

    for (const auto& bad : badFiles)
    {
        const auto result = decode (bad.bytes);
        CHECK (! result.ok);
        CHECK (result.audio.channels.empty());

        if (! contains (result.error, bad.expectedInError))
        {
            std::printf ("    '%s' gave the error \"%s\"\n", bad.what, result.error.c_str());
            CHECK (contains (result.error, bad.expectedInError));
        }
    }

    CHECK (! decodeWav (nullptr, 0).ok);
}

TEST_CASE ("wav reader rejects every truncation of a valid file")
{
    for (const auto& file : { validStereo16 (25), makeRiff ({ { "LIST", Bytes (10, 1) }, { "fmt ", extensibleBody (3, 2, 48000, 32, 32) },
                                                               { "data", Bytes (64, 0) } }) })
    {
        CHECK (decode (file).ok);
        bool allRejected = true;
        bool allExplained = true;

        for (size_t length = 0; length < file.size(); ++length)
        {
            const auto result = decodeWav (file.data(), length);
            allRejected = allRejected && ! result.ok;
            allExplained = allExplained && ! result.error.empty();
        }

        CHECK (allRejected);
        CHECK (allExplained);
    }
}

TEST_CASE ("wav reader survives random corruption")
{
    const auto valid = encodeWav (testSignal (2, 200), 44100.0, WavSampleFormat::Pcm24).bytes;
    uint32_t state = 12345;

    auto nextRandom = [&state] {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };

    int accepted = 0;
    bool consistent = true;

    for (int trial = 0; trial < 3000; ++trial)
    {
        auto corrupted = valid;
        const int numEdits = 1 + static_cast<int> (nextRandom() % 6);

        // Mostly hit the header, where the interesting bugs live.
        for (int edit = 0; edit < numEdits; ++edit)
        {
            const auto limit = nextRandom() % 4 == 0 ? corrupted.size() : size_t { 64 };
            corrupted[nextRandom() % limit] = static_cast<uint8_t> (nextRandom());
        }

        if (trial % 3 == 0)
            corrupted.resize (nextRandom() % corrupted.size());

        const auto result = decode (corrupted);

        if (result.ok)
        {
            ++accepted;
            const auto& audio = result.audio;
            consistent = consistent && ! audio.channels.empty() && audio.channels.size() <= 2
                         && audio.numFrames() * 2 <= corrupted.size() && audio.sampleRate > 0.0;

            for (const auto& channel : audio.channels)
                consistent = consistent && channel.size() == audio.numFrames();
        }
        else
        {
            consistent = consistent && ! result.error.empty();
        }
    }

    CHECK (consistent);
    CHECK (accepted > 0); // edits to the sample data alone leave a valid file
}

// ---------------------------------------------------------------------------
// Render tool: patterns, parameter settings, command line, demo loop, engine driving
// ---------------------------------------------------------------------------

TEST_CASE ("render pattern letters set the steps and the step count")
{
    SequencerParams sequencer;
    std::string error;

    CHECK (applyPattern ("..S...R..CXG..TT", sequencer, error));
    CHECK (sequencer.numSteps == 16);
    CHECK (sequencer.steps[0] == EffectType::None);
    CHECK (sequencer.steps[2] == EffectType::Stutter);
    CHECK (sequencer.steps[6] == EffectType::Reverse);
    CHECK (sequencer.steps[9] == EffectType::Crusher);
    CHECK (sequencer.steps[10] == EffectType::Stretch);
    CHECK (sequencer.steps[11] == EffectType::Gate);
    CHECK (sequencer.steps[14] == EffectType::TapeStop);
    CHECK (patternString (sequencer) == "..S...R..CXG..TT");

    CHECK (applyPattern ("S", sequencer, error));
    CHECK (sequencer.numSteps == 1);
    CHECK (sequencer.steps[0] == EffectType::Stutter);
    CHECK (sequencer.steps[1] == EffectType::None); // the rest is cleared

    // Lower case, separators and '-' for an empty step.
    CHECK (applyPattern ("s r t | x c g -", sequencer, error));
    CHECK (patternString (sequencer) == "SRTXCG.");

    // Errors leave the sequencer as it was.
    const auto before = sequencer;

    for (const char* bad : { "", "   ", "| |", "..Q.", "SSSSSSSSSSSSSSSSS", "S.R.o" })
    {
        error.clear();
        CHECK (! applyPattern (bad, sequencer, error));
        CHECK (! error.empty());
        CHECK (sameSequencer (sequencer, before));
    }

    CHECK (applyPattern ("..Q.", sequencer, error) == false);
    CHECK (contains (error, "'Q' at position 3"));

    // The plugin's default pattern.
    CHECK (patternString (defaultEngineParams().sequencer) == "..S...R..C.GXXTT");
}

TEST_CASE ("render settings use the plugin parameter IDs and units")
{
    EngineParams params = defaultEngineParams();
    std::string error;

    auto set = [&] (const char* setting) {
        const bool ok = applySetting (setting, params, error);

        if (! ok)
            std::printf ("    '%s' was rejected: %s\n", setting, error.c_str());

        return ok;
    };

    CHECK (set ("mix=80"));
    CHECK_NEAR (params.mix, 0.8, 1e-6);
    CHECK (set ("mix = 25 %"));
    CHECK_NEAR (params.mix, 0.25, 1e-6);
    CHECK (set ("output=-6dB"));
    CHECK_NEAR (params.outputGainDb, -6.0, 1e-6);
    CHECK (set ("output=12"));
    CHECK_NEAR (params.outputGainDb, 12.0, 1e-6);
    CHECK (set ("stepRate=1/8"));
    CHECK_NEAR (params.sequencer.stepBeats, 0.5, 1e-12);
    CHECK (set ("stepRate=1/16t"));
    CHECK_NEAR (params.sequencer.stepBeats, 1.0 / 6.0, 1e-12);
    CHECK (set ("stepRate=1/4"));
    CHECK_NEAR (params.sequencer.stepBeats, 1.0, 1e-12);
    CHECK (set ("steps=8"));
    CHECK (params.sequencer.numSteps == 8);
    CHECK (set ("chance=50"));
    CHECK_NEAR (params.sequencer.chance, 0.5, 1e-6);
    CHECK (set ("step3=Tape Stop"));
    CHECK (params.sequencer.steps[2] == EffectType::TapeStop);
    CHECK (set ("step16=g"));
    CHECK (params.sequencer.steps[15] == EffectType::Gate);
    CHECK (set ("step1=off"));
    CHECK (params.sequencer.steps[0] == EffectType::None);
    CHECK (set ("Step2=STRETCH"));
    CHECK (params.sequencer.steps[1] == EffectType::Stretch);
    CHECK (set ("stutterRepeat=1/64"));
    CHECK_NEAR (params.stutter.repeatBeats, 0.0625, 1e-9);
    CHECK (set ("stutterPitch=-12"));
    CHECK_NEAR (params.stutter.pitchGlideSemitones, -12.0, 1e-6);
    CHECK (set ("stutterPitch=24st"));
    CHECK_NEAR (params.stutter.pitchGlideSemitones, 24.0, 1e-6);
    CHECK (set ("stutterDecay=25"));
    CHECK_NEAR (params.stutter.decay, 0.25, 1e-6);
    CHECK (set ("reverseSpeed=2x"));
    CHECK_NEAR (params.reverse.speed, 2.0, 1e-6);
    CHECK (set ("tapeStopTime=5"));
    CHECK_NEAR (params.tapeStop.duration, 0.05, 1e-6);
    CHECK (set ("tapeStopCurve=0"));
    CHECK_NEAR (params.tapeStop.curve, 0.0, 1e-6);
    CHECK (set ("stretchFactor=16"));
    CHECK_NEAR (params.stretch.factor, 16.0, 1e-6);
    CHECK (set ("stretchGrain=10ms"));
    CHECK_NEAR (params.stretch.grainMs, 10.0, 1e-6);
    CHECK (set ("crusherBits=3.5"));
    CHECK_NEAR (params.crusher.bits, 3.5, 1e-6);
    CHECK (set ("crusherDownsample=64"));
    CHECK_NEAR (params.crusher.downsample, 64.0, 1e-6);
    CHECK (set ("gateDivision=1/128"));
    CHECK_NEAR (params.gate.divisionBeats, 0.03125, 1e-9);
    CHECK (set ("gateDuty=95"));
    CHECK_NEAR (params.gate.duty, 0.95, 1e-6);
    CHECK (set ("MIX=10"));
    CHECK_NEAR (params.mix, 0.1, 1e-6);
}

TEST_CASE ("render settings are validated")
{
    const char* badSettings[] { "mix=101", "mix=-1", "mix=", "=5", "mix", "mix=abc", "mix=80dB", "mix=nan", "mix=inf",
                                "output=13", "output=-25", "stutterPitch=25", "steps=0", "steps=17", "steps=2.5",
                                "reverseSpeed=0.4", "gateDuty=4", "gateDuty=96", "crusherBits=0.5", "crusherBits=4%",
                                "crusherDownsample=65", "stretchGrain=9", "tapeStopTime=4", "stepRate=1/5", "stepRate=1/64",
                                "gateDivision=1/4", "stutterRepeat=0.5", "step0=S", "step17=S", "step=S", "stepx=S",
                                "step3=banana", "step3=", "unknownThing=1", "mix=50 percent" };

    for (const char* setting : badSettings)
    {
        EngineParams params = defaultEngineParams();
        const EngineParams before = params;
        std::string error;

        const bool accepted = applySetting (setting, params, error);
        CHECK (! accepted);
        CHECK (! error.empty());
        CHECK (params.mix == before.mix && params.outputGainDb == before.outputGainDb && params.crusher.bits == before.crusher.bits);
        CHECK (sameSequencer (params.sequencer, before.sequencer));

        if (accepted)
            std::printf ("    '%s' was accepted\n", setting);
    }

    // The messages say what is allowed.
    EngineParams params;
    std::string error;
    applySetting ("mix=120", params, error);
    CHECK (contains (error, "between 0 and 100 %"));
    applySetting ("stepRate=1/5", params, error);
    CHECK (contains (error, "1/16T"));
    applySetting ("step3=banana", params, error);
    CHECK (contains (error, "Tape Stop"));
    applySetting ("mixx=1", params, error);
    CHECK (contains (error, "unknown parameter 'mixx'"));
}

TEST_CASE ("render command line parsing")
{
    const auto demo = parse ({ "out.wav" });
    CHECK (demo.ok);
    CHECK (demo.options.inputPath.empty());
    CHECK (demo.options.outputPath == "out.wav");
    CHECK (demo.options.bpm == 120.0);
    CHECK (demo.options.blockSize == 512);
    CHECK (! demo.options.writeFloat);
    CHECK (patternString (demo.options.params.sequencer) == "..S...R..C.GXXTT");

    const auto full = parse ({ "--bpm", "140", "in.wav", "--block=256", "out.wav", "--pattern", "S.R.", "--set", "mix=50",
                               "--set=step1=G", "--float", "--demo-seconds", "3", "--set", "steps=2" });
    CHECK (full.ok);
    CHECK (full.options.inputPath == "in.wav");
    CHECK (full.options.outputPath == "out.wav");
    CHECK (full.options.bpm == 140.0);
    CHECK (full.options.blockSize == 256);
    CHECK (full.options.writeFloat);
    CHECK (full.options.demoSeconds == 3.0);
    CHECK (full.options.demoSecondsGiven);
    CHECK (full.options.settings.size() == 3);
    CHECK_NEAR (full.options.params.mix, 0.5, 1e-6);

    // Settings apply after the pattern, whatever the order on the command line.
    CHECK (full.options.params.sequencer.steps[0] == EffectType::Gate);
    CHECK (full.options.params.sequencer.steps[2] == EffectType::Reverse);
    CHECK (full.options.params.sequencer.numSteps == 2);

    const auto help = parse ({ "out.wav", "--help", "--bogus" });
    CHECK (help.ok);
    CHECK (help.options.showHelp);
    CHECK (parse ({ "-h" }).options.showHelp);

    const std::vector<std::vector<std::string>> badCommandLines {
        {}, { "a.wav", "b.wav", "c.wav" }, { "out.wav", "--bpm" }, { "out.wav", "--bpm", "10" }, { "out.wav", "--bpm", "1000" },
        { "out.wav", "--bpm", "fast" }, { "out.wav", "--bpm", "120bpm" }, { "out.wav", "--block", "0" }, { "out.wav", "--block", "1.5" },
        { "out.wav", "--block", "20000" }, { "out.wav", "--unknown" }, { "out.wav", "-x" }, { "out.wav", "--float=yes" },
        { "same.wav", "same.wav" }, { "out.wav", "--pattern", "Q" }, { "out.wav", "--pattern", "" }, { "out.wav", "--set", "mix" },
        { "out.wav", "--set", "mix=500" }, { "out.wav", "--demo-seconds", "0" }, { "out.wav", "--demo-seconds", "601" }, { "--float" },
    };

    for (const auto& arguments : badCommandLines)
    {
        const auto result = parse (arguments);
        CHECK (! result.ok);
        CHECK (! result.error.empty());
    }

    CHECK (contains (parse ({ "out.wav", "--set", "mix=500" }).error, "--set mix=500: mix must be between"));
    CHECK (contains (parse ({ "out.wav", "--pattern", "..Q" }).error, "--pattern"));
    CHECK (contains (parse ({ "out.wav", "--bpm" }).error, "--bpm needs a value"));
}

TEST_CASE ("render usage lists every parameter")
{
    const auto usage = usageText();

    for (const char* id : { "mix", "output", "stepRate", "steps", "chance", "step1 ... step16", "stutterRepeat", "stutterPitch",
                            "stutterDecay", "reverseSpeed", "tapeStopTime", "tapeStopCurve", "stretchFactor", "stretchGrain",
                            "crusherBits", "crusherDownsample", "gateDivision", "gateDuty", "--bpm", "--block", "--pattern",
                            "--set", "--float", "--demo-seconds", "--help" })
        CHECK (contains (usage, id));
}

TEST_CASE ("render step rate labels")
{
    CHECK (stepRateLabel (0.25) == "1/16");
    CHECK (stepRateLabel (1.0 / 3.0) == "1/8T");
    CHECK (stepRateLabel (1.0) == "1/4");
    CHECK (stepRateLabel (0.3) == "0.3 beats");
}

TEST_CASE ("render transport plays from ppq 0")
{
    const auto start = transportForBlock (0, 48000.0, 120.0);
    CHECK (start.isPlaying);
    CHECK (start.hasPpqPosition);
    CHECK (start.ppqPosition == 0.0);
    CHECK (start.bpm == 120.0);

    CHECK_NEAR (transportForBlock (24000, 48000.0, 120.0).ppqPosition, 1.0, 1e-12);
    CHECK_NEAR (transportForBlock (22050, 44100.0, 120.0).ppqPosition, 1.0, 1e-12);
    CHECK_NEAR (transportForBlock (512, 48000.0, 140.0).ppqPosition, 512.0 * 140.0 / (60.0 * 48000.0), 1e-12);

    // Hours in, the position is still exact to well under a sample.
    CHECK_NEAR (transportForBlock (int64_t { 48000 } * 3600 * 5, 48000.0, 120.0).ppqPosition, 2.0 * 3600.0 * 5.0, 1e-6);
}

TEST_CASE ("render demo loop is in time, finite and below full scale")
{
    const auto loop = makeDemoLoop (48000.0, 120.0, 4.0);
    CHECK (loop.size() == 2);
    CHECK (loop[0].size() == 192000);
    CHECK (loop[1].size() == 192000);
    CHECK (allFinite (loop[0]));
    CHECK (allFinite (loop[1]));
    CHECK (peak (loop[0]) <= 0.892f);
    CHECK (peak (loop[0]) > 0.3f);
    CHECK (maxDifference (makeDemoLoop (48000.0, 120.0, 4.0)[0], loop[0]) == 0.0f); // deterministic

    // The kick lands on every beat: the 20 ms after a beat are much louder than the 20 ms before it.
    for (const double bpm : { 90.0, 120.0, 174.0 })
    {
        const auto demo = makeDemoLoop (48000.0, bpm, 3.0);
        const double beat = 48000.0 * 60.0 / bpm;
        bool onBeat = true;

        for (int index = 1; static_cast<double> (index + 1) * beat < 3.0 * 48000.0; ++index)
        {
            const auto onset = static_cast<size_t> (std::llround (index * beat));
            onBeat = onBeat && rms (demo[0], onset, onset + 960) > 2.0 * rms (demo[0], onset - 960, onset);
        }

        CHECK (onBeat);
    }

    CHECK (makeDemoLoop (48000.0, 120.0, 0.0)[0].empty());
    CHECK (makeDemoLoop (44100.0, 120.0, 0.5)[1].size() == 22050);
}

TEST_CASE ("render through the engine keeps the length and stays finite")
{
    auto audio = makeDemoLoop (48000.0, 120.0, 2.0);
    const auto dry = audio;

    for (const int blockSize : { 64, 511 })
    {
        auto left = dry[0];
        auto right = dry[1];
        renderThroughEngine (defaultEngineParams(), 48000.0, 120.0, blockSize, left, right);

        CHECK (left.size() == dry[0].size());
        CHECK (right.size() == dry[1].size());
        CHECK (allFinite (left));
        CHECK (allFinite (right));
        CHECK (peak (left) < 4.0f);
    }

    // With every step off the engine plays the input through (after any start-up smoothing).
    auto params = defaultEngineParams();
    std::string error;
    CHECK (applyPattern ("....", params.sequencer, error));
    auto left = dry[0];
    auto right = dry[1];
    renderThroughEngine (params, 48000.0, 120.0, 256, left, right);

    float worst = 0.0f;

    for (size_t i = 4800; i < left.size(); ++i)
        worst = std::max ({ worst, std::fabs (left[i] - dry[0][i]), std::fabs (right[i] - dry[1][i]) });

    CHECK (worst < 1.0e-5f);

    // Empty input is fine.
    std::vector<float> none;
    std::vector<float> alsoNone;
    renderThroughEngine (params, 48000.0, 120.0, 512, none, alsoNone);
    CHECK (none.empty());
}
