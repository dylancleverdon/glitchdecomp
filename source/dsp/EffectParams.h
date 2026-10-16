#pragma once

#include <array>
#include <cstdint>

namespace splinter
{

/** What a sequencer step does. The numeric values are stored in presets, so only append. */
enum class EffectType : int
{
    None = 0,
    Stutter,
    Reverse,
    TapeStop,
    Stretch,
    Crusher,
    Gate,
};

constexpr int numEffectTypes = 7; // including None
constexpr int maxSteps = 16;

const char* effectTypeName (EffectType type) noexcept;

// Musical lengths are expressed in beats (1 beat = 1 quarter note), so 0.25 = 1/16 note.

struct StutterParams
{
    float repeatBeats = 0.125f;       // length of the repeated slice, 1/32 note by default
    float pitchGlideSemitones = 0.0f; // pitch change reached by the end of the step, -24..24
    float decay = 0.0f;               // 0 = every repeat at full level, 1 = repeats fade out over the step
};

struct ReverseParams
{
    float speed = 1.0f; // playback speed of the reversed audio, 0.5..2
};

struct TapeStopParams
{
    float duration = 1.0f; // fraction of the step it takes to come to a halt, 0.05..1
    float curve = 0.5f;    // 0 = linear slow-down, 1 = strongly exponential
};

struct StretchParams
{
    float factor = 4.0f;   // time-stretch factor, 1..16 (4 = audio plays 4x slower, same pitch)
    float grainMs = 80.0f; // grain size, 10..250 ms
};

struct CrusherParams
{
    float bits = 6.0f;       // bit depth, 1..16
    float downsample = 8.0f; // sample-and-hold factor, 1..64
};

struct GateParams
{
    float divisionBeats = 0.125f; // gate cycle length, 1/32 note by default
    float duty = 0.5f;            // fraction of each cycle the gate is open, 0.05..0.95
};

struct SequencerParams
{
    int numSteps = 16;        // 1..maxSteps
    double stepBeats = 0.25;  // length of one step, 1/16 note by default
    float chance = 1.0f;      // probability that a non-empty step actually fires, 0..1
    std::array<EffectType, maxSteps> steps {};
};

struct EngineParams
{
    float mix = 1.0f;          // 0 = dry, 1 = fully wet while an effect is active
    float outputGainDb = 0.0f; // -24..12
    SequencerParams sequencer;
    StutterParams stutter;
    ReverseParams reverse;
    TapeStopParams tapeStop;
    StretchParams stretch;
    CrusherParams crusher;
    GateParams gate;
};

/** Host transport state for one block. */
struct TransportInfo
{
    bool isPlaying = false;
    bool hasPpqPosition = false;
    double ppqPosition = 0.0; // quarter notes at the first sample of the block
    double bpm = 120.0;
};

} // namespace splinter
