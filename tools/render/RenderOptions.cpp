#include "RenderOptions.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>

namespace splinter::render
{

namespace
{
    enum class Unit
    {
        None,
        Percent,
        Decibels,
        Semitones,
        Times,
        Milliseconds,
    };

    struct Choice
    {
        const char* label;
        double beats;
    };

    constexpr Choice stepRateChoices[] { { "1/4", 1.0 }, { "1/8", 0.5 }, { "1/8T", 1.0 / 3.0 },
                                         { "1/16", 0.25 }, { "1/16T", 1.0 / 6.0 }, { "1/32", 0.125 } };

    constexpr Choice noteChoices[] { { "1/8", 0.5 }, { "1/16", 0.25 }, { "1/32", 0.125 }, { "1/64", 0.0625 }, { "1/128", 0.03125 } };

    using ApplyFunction = void (*) (EngineParams&, double);

    /** One row of the plugin parameter table (docs/ARCHITECTURE.md). Choice parameters apply
        the chosen value in beats; numeric ones the value in the displayed unit. */
    struct ParameterInfo
    {
        const char* id;
        double minValue;
        double maxValue;
        Unit unit;
        bool wholeNumber;
        const Choice* choices; // null for numeric parameters
        size_t numChoices;
        const char* defaultText;
        ApplyFunction apply;
    };

    float toFloat (double value) { return static_cast<float> (value); }

    const ParameterInfo parameterTable[] {
        { "mix", 0.0, 100.0, Unit::Percent, false, nullptr, 0, "100",
          [] (EngineParams& params, double value) { params.mix = toFloat (value / 100.0); } },
        { "output", -24.0, 12.0, Unit::Decibels, false, nullptr, 0, "0",
          [] (EngineParams& params, double value) { params.outputGainDb = toFloat (value); } },
        { "stepRate", 0.0, 0.0, Unit::None, false, stepRateChoices, std::size (stepRateChoices), "1/16",
          [] (EngineParams& params, double beats) { params.sequencer.stepBeats = beats; } },
        { "steps", 1.0, static_cast<double> (maxSteps), Unit::None, true, nullptr, 0, "16",
          [] (EngineParams& params, double value) { params.sequencer.numSteps = static_cast<int> (value); } },
        { "chance", 0.0, 100.0, Unit::Percent, false, nullptr, 0, "100",
          [] (EngineParams& params, double value) { params.sequencer.chance = toFloat (value / 100.0); } },
        { "stutterRepeat", 0.0, 0.0, Unit::None, false, noteChoices, std::size (noteChoices), "1/32",
          [] (EngineParams& params, double beats) { params.stutter.repeatBeats = toFloat (beats); } },
        { "stutterPitch", -24.0, 24.0, Unit::Semitones, false, nullptr, 0, "0",
          [] (EngineParams& params, double value) { params.stutter.pitchGlideSemitones = toFloat (value); } },
        { "stutterDecay", 0.0, 100.0, Unit::Percent, false, nullptr, 0, "0",
          [] (EngineParams& params, double value) { params.stutter.decay = toFloat (value / 100.0); } },
        { "reverseSpeed", 0.5, 2.0, Unit::Times, false, nullptr, 0, "1",
          [] (EngineParams& params, double value) { params.reverse.speed = toFloat (value); } },
        { "tapeStopTime", 5.0, 100.0, Unit::Percent, false, nullptr, 0, "100",
          [] (EngineParams& params, double value) { params.tapeStop.duration = toFloat (value / 100.0); } },
        { "tapeStopCurve", 0.0, 100.0, Unit::Percent, false, nullptr, 0, "50",
          [] (EngineParams& params, double value) { params.tapeStop.curve = toFloat (value / 100.0); } },
        { "stretchFactor", 1.0, 16.0, Unit::Times, false, nullptr, 0, "4",
          [] (EngineParams& params, double value) { params.stretch.factor = toFloat (value); } },
        { "stretchGrain", 10.0, 250.0, Unit::Milliseconds, false, nullptr, 0, "80",
          [] (EngineParams& params, double value) { params.stretch.grainMs = toFloat (value); } },
        { "crusherBits", 1.0, 16.0, Unit::None, false, nullptr, 0, "6",
          [] (EngineParams& params, double value) { params.crusher.bits = toFloat (value); } },
        { "crusherDownsample", 1.0, 64.0, Unit::Times, false, nullptr, 0, "8",
          [] (EngineParams& params, double value) { params.crusher.downsample = toFloat (value); } },
        { "gateDivision", 0.0, 0.0, Unit::None, false, noteChoices, std::size (noteChoices), "1/32",
          [] (EngineParams& params, double beats) { params.gate.divisionBeats = toFloat (beats); } },
        { "gateDuty", 5.0, 95.0, Unit::Percent, false, nullptr, 0, "50",
          [] (EngineParams& params, double value) { params.gate.duty = toFloat (value / 100.0); } },
    };

    constexpr EffectType allEffects[] { EffectType::None, EffectType::Stutter, EffectType::Reverse, EffectType::TapeStop,
                                        EffectType::Stretch, EffectType::Crusher, EffectType::Gate };

    const char* unitSymbol (Unit unit) noexcept
    {
        switch (unit)
        {
            case Unit::None:         return "";
            case Unit::Percent:      return "%";
            case Unit::Decibels:     return "dB";
            case Unit::Semitones:    return "st";
            case Unit::Times:        return "x";
            case Unit::Milliseconds: return "ms";
        }

        return "";
    }

    std::string_view trim (std::string_view text)
    {
        while (! text.empty() && std::isspace (static_cast<unsigned char> (text.front())))
            text.remove_prefix (1);

        while (! text.empty() && std::isspace (static_cast<unsigned char> (text.back())))
            text.remove_suffix (1);

        return text;
    }

    bool equalsIgnoringCase (std::string_view a, std::string_view b)
    {
        return a.size() == b.size()
            && std::equal (a.begin(), a.end(), b.begin(), [] (char x, char y) {
                   return std::tolower (static_cast<unsigned char> (x)) == std::tolower (static_cast<unsigned char> (y));
               });
    }

    /** Lower case without spaces, '-' and '_', so "Tape Stop", "tape-stop" and "TAPESTOP" all match. */
    std::string simplifiedName (std::string_view text)
    {
        std::string result;

        for (const char c : text)
            if (c != ' ' && c != '-' && c != '_')
                result += static_cast<char> (std::tolower (static_cast<unsigned char> (c)));

        return result;
    }

    std::string formatNumber (double value)
    {
        char text[32];
        std::snprintf (text, sizeof (text), "%g", value);
        return text;
    }

    /** A finite number, then optionally a unit suffix. */
    bool parseNumber (std::string_view text, double& value, std::string& suffix)
    {
        const std::string buffer (trim (text));
        char* end = nullptr;
        value = std::strtod (buffer.c_str(), &end);

        if (end == buffer.c_str() || ! std::isfinite (value))
            return false;

        suffix = std::string (trim (std::string_view (end)));
        return true;
    }

    /** A plain finite number with nothing after it. */
    bool parsePlainNumber (std::string_view text, double& value)
    {
        std::string suffix;
        return parseNumber (text, value, suffix) && suffix.empty();
    }

    const ParameterInfo* findParameter (std::string_view id)
    {
        for (const auto& parameter : parameterTable)
            if (equalsIgnoringCase (parameter.id, id))
                return &parameter;

        return nullptr;
    }

    bool parseEffect (std::string_view text, EffectType& type)
    {
        const auto name = simplifiedName (text);

        if (name.size() == 1) // a pattern letter
        {
            for (const auto effect : allEffects)
            {
                if (static_cast<char> (std::tolower (static_cast<unsigned char> (patternLetter (effect)))) == name[0])
                {
                    type = effect;
                    return true;
                }
            }
        }

        for (const auto effect : allEffects)
        {
            if (simplifiedName (effectTypeName (effect)) == name)
            {
                type = effect;
                return true;
            }
        }

        if (name == "none")
        {
            type = EffectType::None;
            return true;
        }

        return false;
    }

    std::string choiceList (const ParameterInfo& parameter)
    {
        std::string list;

        for (size_t i = 0; i < parameter.numChoices; ++i)
            list += (i == 0 ? "" : ", ") + std::string (parameter.choices[i].label);

        return list;
    }

    std::string effectList()
    {
        std::string list;

        for (const auto effect : allEffects)
            list += (effect == EffectType::None ? "" : ", ") + std::string (effectTypeName (effect));

        return list;
    }

    bool applyStepSetting (std::string_view id, std::string_view valueText, EngineParams& params, std::string& error)
    {
        // "step1" ... "step16"
        const auto numberText = simplifiedName (id).substr (4);
        double stepNumber = 0.0;

        if (! parsePlainNumber (numberText, stepNumber) || stepNumber != std::floor (stepNumber)
            || stepNumber < 1.0 || stepNumber > static_cast<double> (maxSteps))
        {
            error = "unknown parameter '" + std::string (id) + "' (steps are step1 to step" + std::to_string (maxSteps) + ")";
            return false;
        }

        EffectType type = EffectType::None;

        if (! parseEffect (valueText, type))
        {
            error = std::string (id) + ": unknown effect '" + std::string (valueText) + "' (choose from " + effectList() + ")";
            return false;
        }

        params.sequencer.steps[static_cast<size_t> (stepNumber) - 1] = type;
        return true;
    }

    bool applyChoiceSetting (const ParameterInfo& parameter, std::string_view valueText, EngineParams& params, std::string& error)
    {
        for (size_t i = 0; i < parameter.numChoices; ++i)
        {
            if (equalsIgnoringCase (parameter.choices[i].label, valueText))
            {
                parameter.apply (params, parameter.choices[i].beats);
                return true;
            }
        }

        error = std::string (parameter.id) + ": '" + std::string (valueText) + "' is not one of " + choiceList (parameter);
        return false;
    }

    bool applyNumberSetting (const ParameterInfo& parameter, std::string_view valueText, EngineParams& params, std::string& error)
    {
        double value = 0.0;
        std::string suffix;
        const std::string unit = unitSymbol (parameter.unit);
        const std::string unitText = unit.empty() ? "" : " " + unit;

        if (! parseNumber (valueText, value, suffix))
        {
            error = std::string (parameter.id) + ": '" + std::string (valueText) + "' is not a number";
            return false;
        }

        if (! suffix.empty() && (unit.empty() || ! equalsIgnoringCase (suffix, unit)))
        {
            error = std::string (parameter.id) + ": unexpected unit '" + suffix + "'"
                  + (unit.empty() ? " (it has no unit)" : " (it is in " + unit + ")");
            return false;
        }

        if (value < parameter.minValue || value > parameter.maxValue)
        {
            error = std::string (parameter.id) + " must be between " + formatNumber (parameter.minValue) + " and "
                  + formatNumber (parameter.maxValue) + unitText + " (got " + formatNumber (value) + ")";
            return false;
        }

        if (parameter.wholeNumber && value != std::floor (value))
        {
            error = std::string (parameter.id) + " must be a whole number (got " + formatNumber (value) + ")";
            return false;
        }

        parameter.apply (params, value);
        return true;
    }

    bool startsWith (std::string_view text, std::string_view prefix)
    {
        return text.size() >= prefix.size() && text.substr (0, prefix.size()) == prefix;
    }
} // namespace

char patternLetter (EffectType type) noexcept
{
    switch (type)
    {
        case EffectType::None:     return '.';
        case EffectType::Stutter:  return 'S';
        case EffectType::Reverse:  return 'R';
        case EffectType::TapeStop: return 'T';
        case EffectType::Stretch:  return 'X';
        case EffectType::Crusher:  return 'C';
        case EffectType::Gate:     return 'G';
    }

    return '.';
}

EngineParams defaultEngineParams()
{
    EngineParams params;
    std::string error;
    applyPattern (defaultPattern, params.sequencer, error);
    return params;
}

bool applyPattern (std::string_view pattern, SequencerParams& sequencer, std::string& error)
{
    std::array<EffectType, maxSteps> steps {};
    int numSteps = 0;

    for (size_t i = 0; i < pattern.size(); ++i)
    {
        const char letter = pattern[i];

        if (letter == ' ' || letter == '|')
            continue;

        EffectType type = EffectType::None;
        const bool known = letter == '-' || parseEffect (std::string_view (&letter, 1), type);

        if (! known)
        {
            error = "unknown step '" + std::string (1, letter) + "' at position " + std::to_string (i + 1)
                  + " (use . off, S stutter, R reverse, T tape stop, X stretch, C crusher, G gate)";
            return false;
        }

        if (numSteps == maxSteps)
        {
            error = "too many steps (at most " + std::to_string (maxSteps) + ")";
            return false;
        }

        steps[static_cast<size_t> (numSteps++)] = type;
    }

    if (numSteps == 0)
    {
        error = "the pattern has no steps";
        return false;
    }

    sequencer.steps = steps;
    sequencer.numSteps = numSteps;
    return true;
}

bool applySetting (std::string_view setting, EngineParams& params, std::string& error)
{
    const auto equals = setting.find ('=');

    if (equals == std::string_view::npos)
    {
        error = "expected id=value, got '" + std::string (setting) + "'";
        return false;
    }

    const auto id = trim (setting.substr (0, equals));
    const auto valueText = trim (setting.substr (equals + 1));

    if (id.empty() || valueText.empty())
    {
        error = "expected id=value, got '" + std::string (setting) + "'";
        return false;
    }

    if (const auto* parameter = findParameter (id))
    {
        return parameter->choices != nullptr ? applyChoiceSetting (*parameter, valueText, params, error)
                                             : applyNumberSetting (*parameter, valueText, params, error);
    }

    if (startsWith (simplifiedName (id), "step"))
        return applyStepSetting (id, valueText, params, error);

    error = "unknown parameter '" + std::string (id) + "' (run with --help to list them)";
    return false;
}

std::string patternString (const SequencerParams& sequencer)
{
    std::string pattern;
    const int numSteps = std::clamp (sequencer.numSteps, 1, maxSteps);

    for (int step = 0; step < numSteps; ++step)
        pattern += patternLetter (sequencer.steps[static_cast<size_t> (step)]);

    return pattern;
}

std::string stepRateLabel (double stepBeats)
{
    for (const auto& choice : stepRateChoices)
        if (std::fabs (choice.beats - stepBeats) < 1.0e-9)
            return choice.label;

    return formatNumber (stepBeats) + " beats";
}

ParseResult parseCommandLine (const std::vector<std::string>& arguments)
{
    ParseResult result;
    auto& options = result.options;
    std::vector<std::string> files;

    auto fail = [&result] (std::string message) {
        result.ok = false;
        result.error = std::move (message);
        return result;
    };

    for (size_t i = 0; i < arguments.size(); ++i)
    {
        const std::string& argument = arguments[i];

        if (argument == "--help" || argument == "-h")
        {
            options.showHelp = true;
            result.ok = true;
            return result;
        }

        if (! startsWith (argument, "-") || argument == "-")
        {
            files.push_back (argument);
            continue;
        }

        if (! startsWith (argument, "--"))
            return fail ("unknown option '" + argument + "'");

        // Both "--bpm 140" and "--bpm=140".
        const auto equals = argument.find ('=');
        const std::string name = argument.substr (2, equals == std::string::npos ? std::string::npos : equals - 2);
        const bool hasInlineValue = equals != std::string::npos;
        std::string value = hasInlineValue ? argument.substr (equals + 1) : std::string();

        auto takeValue = [&] {
            if (hasInlineValue)
                return true;

            if (i + 1 >= arguments.size())
                return false;

            value = arguments[++i];
            return true;
        };

        if (name == "float")
        {
            if (hasInlineValue)
                return fail ("--float does not take a value");

            options.writeFloat = true;
            continue;
        }

        if (name != "bpm" && name != "block" && name != "pattern" && name != "set" && name != "demo-seconds")
            return fail ("unknown option '--" + name + "'");

        if (! takeValue())
            return fail ("--" + name + " needs a value");

        if (name == "bpm")
        {
            if (! parsePlainNumber (value, options.bpm) || options.bpm < 20.0 || options.bpm > 999.0)
                return fail ("--bpm must be a number from 20 to 999 (got '" + value + "')");
        }
        else if (name == "block")
        {
            double blockSize = 0.0;

            if (! parsePlainNumber (value, blockSize) || blockSize != std::floor (blockSize) || blockSize < 1.0 || blockSize > 16384.0)
                return fail ("--block must be a whole number of samples from 1 to 16384 (got '" + value + "')");

            options.blockSize = static_cast<int> (blockSize);
        }
        else if (name == "pattern")
        {
            options.pattern = value;
        }
        else if (name == "set")
        {
            options.settings.push_back (value);
        }
        else if (name == "demo-seconds")
        {
            if (! parsePlainNumber (value, options.demoSeconds) || options.demoSeconds <= 0.0 || options.demoSeconds > 600.0)
                return fail ("--demo-seconds must be more than 0 and at most 600 (got '" + value + "')");

            options.demoSecondsGiven = true;
        }
    }

    if (files.empty())
        return fail ("missing the output file");

    if (files.size() > 2)
        return fail ("too many file arguments: expected [input.wav] output.wav");

    options.outputPath = files.back();
    options.inputPath = files.size() == 2 ? files.front() : std::string();

    if (options.inputPath == options.outputPath)
        return fail ("the output file would overwrite the input file");

    std::string error;
    options.params = EngineParams {};

    if (! applyPattern (options.pattern, options.params.sequencer, error))
        return fail ("--pattern: " + error);

    for (const auto& setting : options.settings)
        if (! applySetting (setting, options.params, error))
            return fail ("--set " + setting + ": " + error);

    result.ok = true;
    return result;
}

std::string usageText()
{
    std::string text =
        "Usage: splinter_render [input.wav] output.wav [options]\n"
        "\n"
        "Runs a WAV file (or, without an input, a built-in drum and bass loop) through the\n"
        "Splinter glitch engine as if a host were playing from bar 1, and writes the result.\n"
        "\n"
        "Options:\n"
        "  --bpm N            tempo, 20 to 999 (default 120)\n"
        "  --block N          host block size in samples, 1 to 16384 (default 512)\n"
        "  --pattern P        1 to 16 steps, one letter each (default \"" + std::string (defaultPattern) + "\"):\n"
        "                       . off  S stutter  R reverse  T tape stop  X stretch  C crusher  G gate\n"
        "                     the pattern length sets the step count; spaces and | are ignored\n"
        "  --set id=value     set a parameter, repeatable; applied after --pattern, in order\n"
        "  --float            write 32-bit float instead of 24-bit PCM\n"
        "  --demo-seconds N   length of the demo loop, up to 600 (default 8)\n"
        "  --help             show this help\n"
        "\n"
        "Parameters for --set (IDs and units as in the plugin; a unit suffix like 80% is optional):\n";

    auto addRow = [&text] (const std::string& id, const std::string& range, const std::string& defaultText) {
        char row[160];
        std::snprintf (row, sizeof (row), "  %-20s %-36s %s%s\n", id.c_str(), range.c_str(),
                       defaultText.empty() ? "" : "default ", defaultText.c_str());
        text += row;
    };

    for (const auto& parameter : parameterTable)
    {
        const std::string unit = unitSymbol (parameter.unit);
        const std::string range = parameter.choices != nullptr
                                    ? choiceList (parameter)
                                    : formatNumber (parameter.minValue) + " to " + formatNumber (parameter.maxValue) + (unit.empty() ? "" : " " + unit);
        addRow (parameter.id, range, parameter.defaultText);

        if (std::string_view (parameter.id) == "chance")
        {
            addRow ("step1 ... step16", "Off, Stutter, Reverse, Tape Stop,", "from --pattern");
            addRow ("", "Stretch, Crusher, Gate (or a letter)", "");
        }
    }

    text +=
        "\n"
        "Examples:\n"
        "  splinter_render demo.wav\n"
        "  splinter_render loop.wav out.wav --bpm 174 --pattern \"S.R.X.C.G.T.\" --set stepRate=1/8\n"
        "  splinter_render out.wav --set stutterPitch=-12 --set mix=80 --set step1=Stutter\n";

    return text;
}

} // namespace splinter::render
