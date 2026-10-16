#pragma once

#include "dsp/EffectParams.h"

#include <string>
#include <string_view>
#include <vector>

namespace splinter::render
{

/** The plugin's default pattern (docs/ARCHITECTURE.md), one letter per step. */
inline constexpr const char* defaultPattern = "..S...R..C.GXXTT";

struct RenderOptions
{
    std::string inputPath;  // empty: render the built-in demo loop
    std::string outputPath;
    double bpm = 120.0;
    int blockSize = 512;
    double demoSeconds = 8.0;
    bool demoSecondsGiven = false;
    bool writeFloat = false;
    bool showHelp = false;
    std::string pattern = defaultPattern;
    std::vector<std::string> settings; // "id=value" exactly as given, applied after the pattern
    EngineParams params;               // the pattern and settings applied to the defaults
};

struct ParseResult
{
    bool ok = false;
    std::string error;
    RenderOptions options;
};

/** Parses the arguments after the program name. On success the options hold fully validated
    engine parameters; with --help, ok is true and showHelp is set. */
ParseResult parseCommandLine (const std::vector<std::string>& arguments);

/** Engine parameters at the plugin's defaults, including the default pattern. */
EngineParams defaultEngineParams();

/** Sets the steps and step count from a pattern of 1..16 letters (". S R T X C G", any case;
    spaces and '|' are ignored as separators). Leaves the parameters untouched on error. */
bool applyPattern (std::string_view pattern, SequencerParams& sequencer, std::string& error);

/** Applies one "id=value" setting using the IDs and units of the plugin parameter table, e.g.
    "mix=80", "stepRate=1/8", "stutterPitch=-12", "step3=Tape Stop". A matching unit suffix is
    allowed ("80%", "-6dB"). Leaves the parameters untouched on error. */
bool applySetting (std::string_view setting, EngineParams& params, std::string& error);

/** The active steps as pattern letters. */
std::string patternString (const SequencerParams& sequencer);

/** "1/16" etc. for a step length in beats, or the number of beats if it is not one of the choices. */
std::string stepRateLabel (double stepBeats);

char patternLetter (EffectType type) noexcept;

std::string usageText();

} // namespace splinter::render
