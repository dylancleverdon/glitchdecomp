// splinter_render: runs a WAV file (or a built-in demo loop) through the Splinter engine offline.

#include "DemoLoop.h"
#include "OfflineRender.h"
#include "RenderOptions.h"
#include "WavFile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace splinter;
using namespace splinter::render;

namespace
{
    constexpr double demoSampleRate = 48000.0;
    constexpr double minSampleRate = 8000.0;
    constexpr double maxSampleRate = 768000.0;

    struct Levels
    {
        float peak = 0.0f;
        size_t nonFinite = 0;
    };

    Levels measure (const std::vector<std::vector<float>>& channels)
    {
        Levels levels;

        for (const auto& channel : channels)
        {
            for (const float sample : channel)
            {
                if (std::isfinite (sample))
                    levels.peak = std::max (levels.peak, std::fabs (sample));
                else
                    ++levels.nonFinite;
            }
        }

        return levels;
    }

    std::string decibelsText (float peak)
    {
        if (peak <= 0.0f)
            return "silent";

        char text[32];
        std::snprintf (text, sizeof (text), "%.1f dBFS", 20.0 * std::log10 (static_cast<double> (peak)));
        return text;
    }

    const char* channelsText (int channels)
    {
        return channels == 1 ? "mono" : (channels == 2 ? "stereo" : "multichannel, first two used");
    }
} // namespace

int main (int argc, char** argv)
{
    const auto parsed = parseCommandLine (std::vector<std::string> (argv + 1, argv + argc));

    if (! parsed.ok)
    {
        std::fprintf (stderr, "splinter_render: %s\nRun 'splinter_render --help' for usage.\n", parsed.error.c_str());
        return 2;
    }

    const auto& options = parsed.options;

    if (options.showHelp)
    {
        std::fputs (usageText().c_str(), stdout);
        return 0;
    }

    std::vector<std::vector<float>> audio;
    double sampleRate = demoSampleRate;
    std::string inputDescription;

    if (options.inputPath.empty())
    {
        audio = makeDemoLoop (sampleRate, options.bpm, options.demoSeconds);
        inputDescription = "built-in demo loop (kick, snare, hats, bass)";
    }
    else
    {
        auto loaded = readWavFile (options.inputPath);

        if (! loaded.ok)
        {
            std::fprintf (stderr, "splinter_render: %s\n", loaded.error.c_str());
            return 1;
        }

        sampleRate = loaded.audio.sampleRate;

        if (sampleRate < minSampleRate || sampleRate > maxSampleRate)
        {
            std::fprintf (stderr, "splinter_render: '%s': unsupported sample rate %.0f Hz (8 to 768 kHz)\n",
                          options.inputPath.c_str(), sampleRate);
            return 1;
        }

        if (loaded.audio.numFrames() == 0)
        {
            std::fprintf (stderr, "splinter_render: '%s' contains no audio\n", options.inputPath.c_str());
            return 1;
        }

        inputDescription = options.inputPath + " (" + channelsText (loaded.audio.fileChannels) + ", "
                         + sampleFormatName (loaded.audio.format) + ")";

        audio = std::move (loaded.audio.channels);

        // The engine is stereo: a mono file feeds both sides.
        if (audio.size() == 1)
            audio.push_back (audio.front());

        if (options.demoSecondsGiven)
            std::fprintf (stderr, "note: --demo-seconds only applies to the demo loop; ignored\n");
    }

    renderThroughEngine (options.params, sampleRate, options.bpm, options.blockSize, audio[0], audio[1]);

    const auto levels = measure (audio);
    const auto format = options.writeFloat ? WavSampleFormat::Float32 : WavSampleFormat::Pcm24;
    const auto written = writeWavFile (options.outputPath, audio, sampleRate, format);

    if (! written.ok)
    {
        std::fprintf (stderr, "splinter_render: %s\n", written.error.c_str());
        return 1;
    }

    const auto numFrames = audio[0].size();
    const auto& sequencer = options.params.sequencer;
    std::string settings;

    for (const auto& setting : options.settings)
        settings += (settings.empty() ? "" : ", ") + setting;

    std::printf ("Splinter offline render\n");
    std::printf ("  input:       %s\n", inputDescription.c_str());
    std::printf ("  length:      %.3f s (%zu samples)\n", static_cast<double> (numFrames) / sampleRate, numFrames);
    std::printf ("  sample rate: %.0f Hz\n", sampleRate);
    std::printf ("  bpm:         %g\n", options.bpm);
    std::printf ("  pattern:     %s (%d steps of %s)\n", patternString (sequencer).c_str(), sequencer.numSteps,
                 stepRateLabel (sequencer.stepBeats).c_str());

    if (! settings.empty())
        std::printf ("  settings:    %s\n", settings.c_str());

    std::printf ("  block size:  %d\n", options.blockSize);
    std::printf ("  output:      %s (stereo, %s), peak %s\n", options.outputPath.c_str(), sampleFormatName (format),
                 decibelsText (levels.peak).c_str());

    if (levels.nonFinite > 0)
        std::fprintf (stderr, "warning: the engine produced %zu non-finite samples\n", levels.nonFinite);

    if (levels.peak > 1.0f && ! options.writeFloat)
        std::fprintf (stderr, "warning: the output peaks above 0 dBFS and was clipped; use --float or --set output=-6\n");

    return 0;
}
