#pragma once

#include "EffectParams.h"
#include "GlitchEffect.h"

#include <array>
#include <cstdint>

namespace splinter
{

/** A run of samples inside one block that belongs to a single sequencer step. */
struct StepSegment
{
    int offset = 0;                       // first sample, relative to the block start
    int length = 0;                       // always >= 1
    int stepIndex = 0;                    // pattern slot, 0 .. numSteps - 1
    EffectType effect = EffectType::None; // what the step plays: None if its slot is empty or chance skipped it
    bool startsStep = false;              // the step begins at this segment's first sample: trigger its effect here
    StepContext context;                  // timing of the step; its start may lie in an earlier block
};

/** Turns the host transport (or an internal clock while the host is stopped) into
    sequencer steps, and splits each block at the exact samples where steps begin.

    Per block, call beginBlock() and then nextSegment() until it returns false. The
    segments cover the block in order without gaps. Handing them out one at a time
    means there is no limit on how many steps fit in a block, so nothing needs sizing.

    Beat positions are closed-form functions of the absolute sample index (anchored at
    the block's PPQ while the host plays, or at the last tempo change while free-running),
    so the boundaries do not depend on the block size.

    A step's slot, effect and chance roll are decided when it begins; pattern edits made
    during a step apply from the next one. Everything is real-time safe. */
class StepSequencer
{
public:
    void prepare (double sampleRate) noexcept;

    /** Back to step 0 of the free-running clock and the initial random seed. */
    void reset() noexcept;

    /** Starts a block of numSamples samples whose first sample has the absolute index blockStartIndex. */
    void beginBlock (const SequencerParams& params, const TransportInfo& transport, int64_t blockStartIndex, int numSamples) noexcept;

    /** Fills in the next segment of the current block, or returns false once the block is covered. */
    bool nextSegment (StepSegment& segment) noexcept;

    /** Pattern slot of the step reached by the last segment handed out, or -1 before the first one. */
    int currentStep() const noexcept { return hasStep_ ? stepIndex_ : -1; }

    /** Effect of that step (None when its slot is empty or it did not fire). */
    EffectType currentEffect() const noexcept { return hasStep_ ? stepEffect_ : EffectType::None; }

    /** Non-finite or non-positive tempos fall back to defaultBpm; others are clamped to minBpm .. maxBpm. */
    static double sanitiseBpm (double bpm) noexcept;

    /** Length of a quarter note in samples at the sanitised tempo. */
    static double samplesPerBeat (double sampleRate, double bpm) noexcept;

    static constexpr double defaultBpm = 120.0;
    static constexpr double minBpm = 20.0;
    static constexpr double maxBpm = 999.0;

    /** Step lengths are clamped to a 1/64 note .. a quarter note. */
    static constexpr double minStepBeats = 0.0625;
    static constexpr double maxStepBeats = 1.0;

    /** A host position further than this from where the transport was expected counts as a jump (loop, seek). */
    static constexpr double jumpToleranceSeconds = 0.005;

    /** Rounding slack when deciding which sample a boundary lands on. */
    static constexpr double boundaryToleranceSamples = 1.0e-4;

    /** Larger host positions are treated as garbage, so step numbers always fit in 64 bits. */
    static constexpr double maxPpqMagnitude = 1.0e8;

private:
    enum class ClockMode
    {
        none,        // nothing processed since reset()
        freeRunning, // internal clock at the host tempo
        hostSynced   // following the host PPQ
    };

    /** Beat position as a function of the absolute sample index. */
    struct Clock
    {
        int64_t anchorIndex = 0;
        double anchorBeats = 0.0;
        double samplesPerBeat = 22050.0;

        double beatsAt (int64_t index) const noexcept;
    };

    /** Absolute index of the first sample of step `stepNumber` (an unwrapped step count). */
    int64_t stepStartIndex (int64_t stepNumber) const noexcept;
    int64_t stepNumberAt (int64_t index) const noexcept;
    void beginStep (int64_t stepNumber) noexcept;
    double nextRandom() noexcept;

    static constexpr uint64_t randomSeed = 0x9E3779B97F4A7C15ull;

    double sampleRate_ = 44100.0;
    Clock clock_;
    ClockMode mode_ = ClockMode::none;

    // Sanitised parameters of the current block.
    std::array<EffectType, maxSteps> pattern_ {};
    int numSteps_ = maxSteps;
    double stepBeats_ = 0.25;
    double chance_ = 1.0;

    int64_t blockStart_ = 0;
    int blockLength_ = 0;
    int cursor_ = 0;            // offset of the next segment within the block
    bool resyncPending_ = false; // the next segment starts a step wherever the clock now is

    bool hasStep_ = false;
    int64_t stepNumber_ = 0; // floor (beats / stepBeats), not wrapped to the pattern
    int stepIndex_ = 0;
    EffectType stepEffect_ = EffectType::None;
    StepContext stepContext_;

    uint64_t randomState_ = randomSeed;
};

} // namespace splinter
