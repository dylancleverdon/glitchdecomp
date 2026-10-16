#include "WavFile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace splinter::render
{

namespace
{
    constexpr uint16_t formatPcm = 0x0001;
    constexpr uint16_t formatFloat = 0x0003;
    constexpr uint16_t formatExtensible = 0xFFFE;

    // KSDATAFORMAT_SUBTYPE_PCM and _IEEE_FLOAT are {0000000X-0000-0010-8000-00AA00389B71}; the
    // first two bytes hold the format code, these are the remaining fourteen.
    constexpr uint8_t subFormatGuidTail[14] = { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                                0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };

    uint16_t readUint16 (const uint8_t* bytes) noexcept
    {
        return static_cast<uint16_t> (bytes[0] | (bytes[1] << 8));
    }

    uint32_t readUint32 (const uint8_t* bytes) noexcept
    {
        return static_cast<uint32_t> (bytes[0]) | (static_cast<uint32_t> (bytes[1]) << 8)
             | (static_cast<uint32_t> (bytes[2]) << 16) | (static_cast<uint32_t> (bytes[3]) << 24);
    }

    bool hasId (const uint8_t* bytes, const char* id) noexcept
    {
        return std::memcmp (bytes, id, 4) == 0;
    }

    /** A chunk id for error messages, with anything unprintable shown as '?'. */
    std::string printableId (const uint8_t* bytes)
    {
        std::string id;

        for (int i = 0; i < 4; ++i)
            id += bytes[i] >= 0x20 && bytes[i] < 0x7F ? static_cast<char> (bytes[i]) : '?';

        return id;
    }

    int bytesPerSample (WavSampleFormat format) noexcept
    {
        switch (format)
        {
            case WavSampleFormat::Pcm16:   return 2;
            case WavSampleFormat::Pcm24:   return 3;
            case WavSampleFormat::Pcm32:   return 4;
            case WavSampleFormat::Float32: return 4;
        }

        return 4;
    }

    struct FormatChunk
    {
        uint16_t channels = 0;
        uint32_t sampleRate = 0;
        uint16_t blockAlign = 0;
        uint16_t containerBits = 0;
        uint16_t validBits = 0;
        WavSampleFormat format = WavSampleFormat::Pcm16;
    };

    /** Parses and validates a fmt chunk body. Returns an empty string on success. */
    std::string parseFormatChunk (const uint8_t* body, uint32_t size, FormatChunk& chunk)
    {
        if (size < 16)
            return "the fmt chunk is too small (" + std::to_string (size) + " bytes, expected at least 16)";

        uint16_t formatCode = readUint16 (body);
        chunk.channels = readUint16 (body + 2);
        chunk.sampleRate = readUint32 (body + 4);
        chunk.blockAlign = readUint16 (body + 12);
        chunk.containerBits = readUint16 (body + 14);
        chunk.validBits = chunk.containerBits;

        if (formatCode == formatExtensible)
        {
            if (size < 40)
                return "the WAVE_FORMAT_EXTENSIBLE fmt chunk is too small (" + std::to_string (size) + " bytes, expected 40)";

            const uint8_t* subFormat = body + 24;

            if (std::memcmp (subFormat + 2, subFormatGuidTail, sizeof (subFormatGuidTail)) != 0)
                return "unsupported WAVE_FORMAT_EXTENSIBLE sub-format (only PCM and IEEE float are supported)";

            formatCode = readUint16 (subFormat);

            // Some writers leave the valid bits at zero; that means "all of them".
            if (const auto validBits = readUint16 (body + 18); validBits != 0)
                chunk.validBits = validBits;

            if (chunk.validBits > chunk.containerBits)
                return "the fmt chunk says " + std::to_string (chunk.validBits) + " valid bits in a "
                     + std::to_string (chunk.containerBits) + "-bit sample";
        }

        if (chunk.channels == 0)
            return "the fmt chunk says the file has no channels";

        if (chunk.sampleRate == 0)
            return "the fmt chunk says the sample rate is 0 Hz";

        if (formatCode == formatPcm)
        {
            switch (chunk.containerBits)
            {
                case 16: chunk.format = WavSampleFormat::Pcm16; break;
                case 24: chunk.format = WavSampleFormat::Pcm24; break;
                case 32: chunk.format = WavSampleFormat::Pcm32; break;
                default:
                    return "unsupported PCM bit depth: " + std::to_string (chunk.containerBits)
                         + " (supported: 16, 24 and 32-bit integer, 32-bit float)";
            }
        }
        else if (formatCode == formatFloat)
        {
            if (chunk.containerBits != 32)
                return "unsupported float bit depth: " + std::to_string (chunk.containerBits) + " (only 32-bit float is supported)";

            chunk.format = WavSampleFormat::Float32;
        }
        else
        {
            char code[8];
            std::snprintf (code, sizeof (code), "0x%04X", static_cast<unsigned> (formatCode));
            return std::string ("unsupported WAV encoding (format code ") + code + "); only PCM and IEEE float are supported";
        }

        const auto expectedBlockAlign = static_cast<uint32_t> (chunk.channels) * static_cast<uint32_t> (bytesPerSample (chunk.format));

        if (chunk.blockAlign != expectedBlockAlign)
            return "the fmt chunk's block align (" + std::to_string (chunk.blockAlign) + ") does not match "
                 + std::to_string (chunk.channels) + " channel(s) of " + std::to_string (chunk.containerBits) + "-bit samples";

        return {};
    }

    float decodeSample (const uint8_t* bytes, WavSampleFormat format) noexcept
    {
        switch (format)
        {
            case WavSampleFormat::Pcm16:
                return static_cast<float> (static_cast<int16_t> (readUint16 (bytes))) / 32768.0f;

            case WavSampleFormat::Pcm24:
            {
                auto value = static_cast<int32_t> (bytes[0] | (bytes[1] << 8) | (bytes[2] << 16));

                if (value & 0x800000)
                    value -= 0x1000000;

                return static_cast<float> (value) / 8388608.0f;
            }

            case WavSampleFormat::Pcm32:
                return static_cast<float> (static_cast<double> (static_cast<int32_t> (readUint32 (bytes))) / 2147483648.0);

            case WavSampleFormat::Float32:
            {
                const uint32_t bits = readUint32 (bytes);
                float value;
                std::memcpy (&value, &bits, sizeof (value));
                return value;
            }
        }

        return 0.0f;
    }

    void appendBytes (std::vector<uint8_t>& out, const char* text)
    {
        out.insert (out.end(), text, text + std::strlen (text));
    }

    void appendUint16 (std::vector<uint8_t>& out, uint32_t value)
    {
        out.push_back (static_cast<uint8_t> (value & 0xFF));
        out.push_back (static_cast<uint8_t> ((value >> 8) & 0xFF));
    }

    void appendUint32 (std::vector<uint8_t>& out, uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
            out.push_back (static_cast<uint8_t> ((value >> shift) & 0xFF));
    }

    /** Full-scale integer for a sample, rounded and clipped. NaN is silence. */
    int64_t toInteger (float sample, double scale) noexcept
    {
        if (std::isnan (sample))
            return 0;

        const double scaled = std::round (static_cast<double> (std::clamp (sample, -1.0f, 1.0f)) * scale);
        return static_cast<int64_t> (std::clamp (scaled, -scale, scale - 1.0));
    }

    void appendSample (std::vector<uint8_t>& out, float sample, WavSampleFormat format)
    {
        switch (format)
        {
            case WavSampleFormat::Pcm16:
                appendUint16 (out, static_cast<uint32_t> (toInteger (sample, 32768.0)) & 0xFFFF);
                break;

            case WavSampleFormat::Pcm24:
            {
                const auto value = static_cast<uint32_t> (toInteger (sample, 8388608.0));
                out.push_back (static_cast<uint8_t> (value & 0xFF));
                out.push_back (static_cast<uint8_t> ((value >> 8) & 0xFF));
                out.push_back (static_cast<uint8_t> ((value >> 16) & 0xFF));
                break;
            }

            case WavSampleFormat::Pcm32:
                appendUint32 (out, static_cast<uint32_t> (toInteger (sample, 2147483648.0)));
                break;

            case WavSampleFormat::Float32:
            {
                uint32_t bits;
                std::memcpy (&bits, &sample, sizeof (bits));
                appendUint32 (out, bits);
                break;
            }
        }
    }
} // namespace

const char* sampleFormatName (WavSampleFormat format) noexcept
{
    switch (format)
    {
        case WavSampleFormat::Pcm16:   return "16-bit PCM";
        case WavSampleFormat::Pcm24:   return "24-bit PCM";
        case WavSampleFormat::Pcm32:   return "32-bit PCM";
        case WavSampleFormat::Float32: return "32-bit float";
    }

    return "unknown";
}

WavReadResult decodeWav (const uint8_t* data, size_t size)
{
    WavReadResult result;

    auto fail = [&result] (std::string message) {
        result.ok = false;
        result.error = std::move (message);
        result.audio = {};
        return result;
    };

    if (data == nullptr || size < 12)
        return fail ("the file is too short to be a WAV file (" + std::to_string (size) + " bytes)");

    if (hasId (data, "RIFX"))
        return fail ("big-endian (RIFX) WAV files are not supported");

    if (hasId (data, "RF64") || hasId (data, "BW64"))
        return fail ("RF64 / BW64 files are not supported");

    if (! hasId (data, "RIFF"))
        return fail ("not a WAV file (no RIFF header)");

    if (! hasId (data + 8, "WAVE"))
        return fail ("not a WAV file (the RIFF form type is '" + printableId (data + 8) + "', not 'WAVE')");

    const auto riffEnd = static_cast<uint64_t> (readUint32 (data + 4)) + 8;

    FormatChunk format;
    bool hasFormat = false;
    const uint8_t* sampleData = nullptr;
    uint32_t sampleDataSize = 0;

    for (uint64_t position = 12; position + 8 <= size;)
    {
        // Bytes after the RIFF form are ignored once the audio is found. Until then scanning goes
        // on, since some writers leave the RIFF size short.
        if (position >= riffEnd && hasFormat && sampleData != nullptr)
            break;

        const uint8_t* header = data + position;
        const uint32_t chunkSize = readUint32 (header + 4);
        const uint64_t bodyStart = position + 8;
        const uint64_t available = size - bodyStart;

        if (chunkSize > available)
            return fail ("the file is truncated: the '" + printableId (header) + "' chunk declares " + std::to_string (chunkSize)
                         + " bytes but only " + std::to_string (available) + " follow");

        if (hasId (header, "fmt "))
        {
            if (hasFormat)
                return fail ("the file has more than one fmt chunk");

            if (auto error = parseFormatChunk (data + bodyStart, chunkSize, format); ! error.empty())
                return fail (std::move (error));

            hasFormat = true;
        }
        else if (hasId (header, "data"))
        {
            if (sampleData != nullptr)
                return fail ("the file has more than one data chunk");

            sampleData = data + bodyStart;
            sampleDataSize = chunkSize;
        }

        // Chunks are padded to an even size; a missing pad byte at the very end is tolerated.
        position = bodyStart + chunkSize + (chunkSize & 1u);
    }

    if (! hasFormat)
        return fail ("the file has no fmt chunk");

    if (sampleData == nullptr)
        return fail ("the file has no data chunk");

    const size_t numFrames = sampleDataSize / format.blockAlign;
    const int bytesEach = bytesPerSample (format.format);
    const int numChannels = std::min<int> (format.channels, 2);

    WavAudio& audio = result.audio;
    audio.sampleRate = static_cast<double> (format.sampleRate);
    audio.fileChannels = format.channels;
    audio.format = format.format;
    audio.validBits = format.validBits;
    audio.channels.assign (static_cast<size_t> (numChannels), std::vector<float> (numFrames));

    for (size_t frame = 0; frame < numFrames; ++frame)
    {
        const uint8_t* frameBytes = sampleData + frame * format.blockAlign;

        for (int channel = 0; channel < numChannels; ++channel)
            audio.channels[static_cast<size_t> (channel)][frame] = decodeSample (frameBytes + channel * bytesEach, format.format);
    }

    result.ok = true;
    return result;
}

WavReadResult readWavFile (const std::string& path)
{
    WavReadResult result;
    std::FILE* file = std::fopen (path.c_str(), "rb");

    if (file == nullptr)
    {
        result.error = "cannot open '" + path + "' for reading";
        return result;
    }

    std::vector<uint8_t> bytes;
    uint8_t chunk[65536];
    size_t got = 0;

    while ((got = std::fread (chunk, 1, sizeof (chunk), file)) > 0)
        bytes.insert (bytes.end(), chunk, chunk + got);

    const bool readFailed = std::ferror (file) != 0;
    std::fclose (file);

    if (readFailed)
    {
        result.error = "error while reading '" + path + "'";
        return result;
    }

    result = decodeWav (bytes.data(), bytes.size());

    if (! result.ok)
        result.error = "'" + path + "': " + result.error;

    return result;
}

WavEncodeResult encodeWav (const std::vector<std::vector<float>>& channels, double sampleRate, WavSampleFormat format)
{
    WavEncodeResult result;

    if (channels.empty() || channels.size() > 64)
    {
        result.error = "a WAV file needs between 1 and 64 channels (got " + std::to_string (channels.size()) + ")";
        return result;
    }

    const size_t numFrames = channels.front().size();

    for (const auto& channel : channels)
    {
        if (channel.size() != numFrames)
        {
            result.error = "all channels must have the same length";
            return result;
        }
    }

    if (! std::isfinite (sampleRate) || sampleRate < 1.0 || sampleRate > 4.0e9)
    {
        result.error = "invalid sample rate for a WAV file: " + std::to_string (sampleRate);
        return result;
    }

    const auto numChannels = static_cast<uint32_t> (channels.size());
    const auto bytesEach = static_cast<uint32_t> (bytesPerSample (format));
    const uint32_t blockAlign = numChannels * bytesEach;
    const bool isFloat = format == WavSampleFormat::Float32;
    const uint32_t formatChunkSize = isFloat ? 18 : 16;
    const uint32_t factChunkSize = isFloat ? 12 : 0;
    const uint32_t headerSize = 12 + 8 + formatChunkSize + factChunkSize + 8;
    const uint64_t dataSize = static_cast<uint64_t> (numFrames) * blockAlign;
    const uint64_t paddedDataSize = dataSize + (dataSize & 1u);

    if (headerSize + paddedDataSize > std::numeric_limits<uint32_t>::max())
    {
        result.error = "the audio is too long for a WAV file (4 GB limit)";
        return result;
    }

    const auto rate = static_cast<uint32_t> (std::lround (sampleRate));
    auto& out = result.bytes;
    out.reserve (static_cast<size_t> (headerSize + paddedDataSize));

    appendBytes (out, "RIFF");
    appendUint32 (out, static_cast<uint32_t> (headerSize - 8 + paddedDataSize));
    appendBytes (out, "WAVE");

    appendBytes (out, "fmt ");
    appendUint32 (out, formatChunkSize);
    appendUint16 (out, isFloat ? formatFloat : formatPcm);
    appendUint16 (out, numChannels);
    appendUint32 (out, rate);
    appendUint32 (out, rate * blockAlign);
    appendUint16 (out, blockAlign);
    appendUint16 (out, bytesEach * 8);

    if (isFloat)
    {
        appendUint16 (out, 0); // cbSize

        // Non-PCM formats should carry a fact chunk with the frame count.
        appendBytes (out, "fact");
        appendUint32 (out, 4);
        appendUint32 (out, static_cast<uint32_t> (numFrames));
    }

    appendBytes (out, "data");
    appendUint32 (out, static_cast<uint32_t> (dataSize));

    for (size_t frame = 0; frame < numFrames; ++frame)
        for (const auto& channel : channels)
            appendSample (out, channel[frame], format);

    if (dataSize & 1u)
        out.push_back (0);

    result.ok = true;
    return result;
}

WavWriteResult writeWavFile (const std::string& path, const std::vector<std::vector<float>>& channels,
                             double sampleRate, WavSampleFormat format)
{
    WavWriteResult result;
    const auto encoded = encodeWav (channels, sampleRate, format);

    if (! encoded.ok)
    {
        result.error = encoded.error;
        return result;
    }

    std::FILE* file = std::fopen (path.c_str(), "wb");

    if (file == nullptr)
    {
        result.error = "cannot open '" + path + "' for writing";
        return result;
    }

    const size_t written = std::fwrite (encoded.bytes.data(), 1, encoded.bytes.size(), file);
    const bool closed = std::fclose (file) == 0;

    if (written != encoded.bytes.size() || ! closed)
    {
        result.error = "error while writing '" + path + "' (disk full?)";
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace splinter::render
