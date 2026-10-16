#include "TestFramework.h"
#include "TestSignals.h"

#include "dsp/GlitchEngine.h"
#include "dsp/StepSequencer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

namespace splinter
{

/** The seam GlitchEngine.h befriends: swaps an instrumented effect into one of the engine's slots. */
struct GlitchEngineTestAccess
{
    static void replaceEffect (GlitchEngine& engine, EffectType type, GlitchEffect& effect)
    {
        engine.effects_[static_cast<size_t> (type)] = &effect;
    }

    static int crossfadeLength (const GlitchEngine& engine) { return engine.fadeLength_; }

    static int smoothingLength (double sampleRate)
    {
        return static_cast<int> (std::lround (GlitchEngine::smoothingSeconds * sampleRate));
    }
};

} // namespace splinter

using namespace splinter;
using namespace splinter::test;

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr int64_t sixteenth = 6000; // at 120 bpm
    constexpr double pi = 3.14159265358979323846;

    /** Known wet signal, and a record of how the engine drove it. */
    class FakeEffect final : public GlitchEffect
    {
    public:
        enum class Output
        {
            constant,      // every sample = level
            scaledCapture, // level * the captured dry input at the same index
            notANumber     // a broken effect
        };

        struct Trigger
        {
            int64_t index; // first sample processed after the trigger
            StepContext step;
        };

        struct Range
        {
            int64_t begin, end;
        };

        FakeEffect (Output outputType, float outputLevel) : output (outputType), level (outputLevel)
        {
            triggers.reserve (4096);
            pending.reserve (16);
            processed.reserve (4096);
        }

        const char* name() const noexcept override { return "Fake"; }

        void prepare (double rate, int maxBlockSize) override
        {
            ++prepareCount;
            preparedSampleRate = rate;
            preparedBlockSize = maxBlockSize;
        }

        void reset() noexcept override { pending.clear(); }

        void trigger (const StepContext& step) noexcept override { pending.push_back (step); }

        void process (const CaptureBuffer& capture, StereoBlock block, int64_t blockStartIndex) noexcept override
        {
            for (const auto& step : pending)
                triggers.push_back ({ blockStartIndex, step });

            pending.clear();

            // The engine must have captured the block before asking for it.
            if (capture.newestIndex() < blockStartIndex + block.numSamples - 1)
                readAhead = true;

            largestBlock = std::max (largestBlock, block.numSamples);

            if (! processed.empty() && processed.back().end == blockStartIndex)
                processed.back().end += block.numSamples;
            else
                processed.push_back ({ blockStartIndex, blockStartIndex + block.numSamples });

            for (int i = 0; i < block.numSamples; ++i)
            {
                const int64_t now = blockStartIndex + i;

                switch (output)
                {
                    case Output::constant:
                        block.left[i] = level;
                        block.right[i] = level;
                        break;
                    case Output::scaledCapture:
                        block.left[i] = level * capture.sample (0, now);
                        block.right[i] = level * capture.sample (1, now);
                        break;
                    case Output::notANumber:
                        block.left[i] = std::numeric_limits<float>::quiet_NaN();
                        block.right[i] = std::numeric_limits<float>::infinity();
                        break;
                }
            }
        }

        Output output;
        float level;
        std::vector<Trigger> triggers;
        std::vector<StepContext> pending;
        std::vector<Range> processed;
        bool readAhead = false;
        int largestBlock = 0;
        int prepareCount = 0;
        int preparedBlockSize = 0;
        double preparedSampleRate = 0.0;
    };

    using TransportAt = std::function<TransportInfo (int64_t blockStart)>;
    using BlockHook = std::function<void (GlitchEngine&, int64_t blockStart)>;

    TransportInfo playing (double ppq, double bpm = 120.0)
    {
        TransportInfo transport;
        transport.isPlaying = true;
        transport.hasPpqPosition = true;
        transport.ppqPosition = ppq;
        transport.bpm = bpm;
        return transport;
    }

    TransportInfo stopped (double bpm = 120.0)
    {
        TransportInfo transport;
        transport.bpm = bpm;
        return transport;
    }

    TransportAt hostFrom (double startPpq, double bpm = 120.0, double rate = sampleRate)
    {
        const double samplesPerBeat = rate * 60.0 / bpm;
        return [=] (int64_t blockStart) { return playing (startPpq + static_cast<double> (blockStart) / samplesPerBeat, bpm); };
    }

    TransportAt hostStopped (double bpm = 120.0)
    {
        return [=] (int64_t) { return stopped (bpm); };
    }

    using Pattern = std::array<EffectType, maxSteps>;

    Pattern everyStep (EffectType type)
    {
        Pattern pattern;
        pattern.fill (type);
        return pattern;
    }

    /** The default pattern from docs/ARCHITECTURE.md. */
    Pattern defaultPattern()
    {
        using E = EffectType;
        return { E::None, E::None, E::Stutter, E::None, E::None, E::None, E::Reverse, E::None,
                 E::None, E::Crusher, E::None, E::Gate, E::Stretch, E::Stretch, E::TapeStop, E::TapeStop };
    }

    /** A pattern that keeps switching between all six effects and dry. */
    Pattern busyPattern()
    {
        using E = EffectType;
        return { E::Stutter, E::Reverse, E::None, E::TapeStop, E::Stretch, E::Stretch, E::Crusher, E::Gate,
                 E::None, E::Stutter, E::Stutter, E::Reverse, E::Gate, E::None, E::TapeStop, E::Crusher };
    }

    EngineParams makeParams (const Pattern& pattern, float mix = 1.0f, float chance = 1.0f, double stepBeats = 0.25)
    {
        EngineParams params;
        params.mix = mix;
        params.sequencer.steps = pattern;
        params.sequencer.chance = chance;
        params.sequencer.stepBeats = stepBeats;
        return params;
    }

    StereoBuffer makeDc (int numSamples, float value)
    {
        StereoBuffer buffer (numSamples);
        std::fill (buffer.left.begin(), buffer.left.end(), value);
        std::fill (buffer.right.begin(), buffer.right.end(), value);
        return buffer;
    }

    /** Runs the input through the engine in blocks of blockSize; the hook runs before each block. */
    StereoBuffer render (GlitchEngine& engine, const StereoBuffer& input, int blockSize, const TransportAt& transportAt,
                         const BlockHook& beforeBlock = {})
    {
        StereoBuffer output = input;
        const auto total = static_cast<int64_t> (input.size());

        for (int64_t start = 0; start < total; start += blockSize)
        {
            const auto length = static_cast<int> (std::min<int64_t> (blockSize, total - start));

            if (beforeBlock)
                beforeBlock (engine, start);

            engine.process (output.block (static_cast<int> (start), length), transportAt (start));
        }

        return output;
    }

    StereoBuffer renderFresh (const EngineParams& params, const StereoBuffer& input, int blockSize, const TransportAt& transportAt,
                              int maxBlockSize = 0)
    {
        GlitchEngine engine;
        engine.prepare (sampleRate, maxBlockSize > 0 ? maxBlockSize : blockSize);
        engine.setParams (params);
        return render (engine, input, blockSize, transportAt);
    }

    double maxDifference (const StereoBuffer& a, const StereoBuffer& b, size_t begin = 0, size_t end = SIZE_MAX)
    {
        double result = 0.0;
        end = std::min ({ end, a.left.size(), b.left.size() });

        for (size_t i = begin; i < end; ++i)
        {
            result = std::max (result, static_cast<double> (std::fabs (a.left[i] - b.left[i])));
            result = std::max (result, static_cast<double> (std::fabs (a.right[i] - b.right[i])));
        }

        return result;
    }

    bool finite (const StereoBuffer& buffer)
    {
        return allFinite (buffer.left) && allFinite (buffer.right);
    }

    float stereoPeak (const StereoBuffer& buffer)
    {
        return std::max (peak (buffer.left), peak (buffer.right));
    }

    /** The engine's crossfade gain after `elapsed` samples of a fade (raised cosine). */
    float fadeWeight (int elapsed, int fadeLength)
    {
        return static_cast<float> (0.5 - 0.5 * std::cos (pi * (elapsed + 1) / (fadeLength + 1)));
    }

    size_t at (int64_t index)
    {
        return static_cast<size_t> (index);
    }

    /** Six fakes, one in each effect slot, each with its own level. */
    struct FakeRack
    {
        std::vector<FakeEffect> fakes;

        FakeRack (FakeEffect::Output output, std::initializer_list<float> levels)
        {
            fakes.reserve (6);

            for (const auto level : levels)
                fakes.emplace_back (output, level);
        }

        void install (GlitchEngine& engine)
        {
            for (size_t i = 0; i < fakes.size(); ++i)
                GlitchEngineTestAccess::replaceEffect (engine, static_cast<EffectType> (i + 1), fakes[i]);
        }

        FakeEffect& operator[] (EffectType type) { return fakes[static_cast<size_t> (type) - 1]; }
    };
} // namespace

//==============================================================================
TEST_CASE ("engine: mix 0 gives the input times the output gain exactly")
{
    const auto input = makeNoise (48000, 7, 0.9f);
    const float nan = std::numeric_limits<float>::quiet_NaN();

    struct Case
    {
        float gainDb, expectedDb;
    };

    const Case cases[] = { { 0.0f, 0.0f }, { -6.0f, -6.0f }, { 12.0f, 12.0f }, { -24.0f, -24.0f },
                           { 40.0f, 12.0f }, { -90.0f, -24.0f }, { nan, 0.0f } };

    for (const auto& c : cases)
    {
        auto params = makeParams (busyPattern(), 0.0f);
        params.outputGainDb = c.gainDb;

        const auto output = renderFresh (params, input, 512, hostFrom (0.3));
        const auto gain = static_cast<float> (std::pow (10.0, static_cast<double> (c.expectedDb) / 20.0));

        bool exact = true;

        for (size_t i = 0; i < input.left.size(); ++i)
            exact = exact && output.left[i] == input.left[i] * gain && output.right[i] == input.right[i] * gain;

        CHECK (exact);
    }
}

TEST_CASE ("engine: an empty pattern or chance 0 passes the audio through untouched")
{
    const auto input = makeNoise (30000, 3);

    for (const bool useChance : { false, true })
    {
        GlitchEngine engine;
        engine.prepare (sampleRate, 256);
        FakeRack rack (FakeEffect::Output::constant, { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f });
        rack.install (engine);

        engine.setParams (useChance ? makeParams (busyPattern(), 1.0f, 0.0f) : makeParams (everyStep (EffectType::None)));
        const auto output = render (engine, input, 256, hostFrom (0.0));

        CHECK (maxDifference (output, input) == 0.0);
        CHECK (engine.currentEffect() == EffectType::None);

        for (const auto& fake : rack.fakes)
        {
            CHECK (fake.triggers.empty());
            CHECK (fake.processed.empty());
        }
    }
}

TEST_CASE ("engine: zero-length blocks are a no-op and process before prepare is safe")
{
    {
        GlitchEngine unprepared;
        auto buffer = makeNoise (100, 5);
        const auto original = buffer;
        unprepared.process (buffer.block(), playing (0.0));
        CHECK (maxDifference (buffer, original) == 0.0);
        CHECK (unprepared.currentStep() == -1);
    }

    const auto input = makeNoise (20000, 9);
    const auto params = makeParams (busyPattern());

    GlitchEngine engine;
    engine.prepare (sampleRate, 512);
    engine.setParams (params);

    StereoBuffer empty (1);
    engine.process (empty.block (0, 0), playing (0.0));
    CHECK (engine.currentStep() == -1);
    CHECK (engine.currentEffect() == EffectType::None);

    // Empty blocks sprinkled between real ones change nothing.
    const auto withEmpties = render (engine, input, 500, hostFrom (0.0), [&] (GlitchEngine& e, int64_t start) {
        e.process (empty.block (0, 0), hostFrom (0.0) (start));
    });

    const auto reference = renderFresh (params, input, 500, hostFrom (0.0), 512);
    CHECK (maxDifference (withEmpties, reference) == 0.0);
}

TEST_CASE ("engine: blocks larger than maxBlockSize are split internally")
{
    const auto input = makeNoise (40000, 11);
    const auto params = makeParams (busyPattern(), 1.0f, 0.7f);

    for (const auto& transportAt : { hostFrom (0.37, 133.0), hostStopped (133.0) })
    {
        // With the real effects: same audio and step as the same input in host blocks of 64.
        GlitchEngine small;
        small.prepare (sampleRate, 64);
        small.setParams (params);
        const auto reference = render (small, input, 64, transportAt);

        GlitchEngine big;
        big.prepare (sampleRate, 64);
        big.setParams (params);
        const auto oversized = render (big, input, 5000, transportAt);

        CHECK (maxDifference (oversized, reference) <= 1.0e-6);
        CHECK (finite (oversized));
        CHECK (big.currentStep() == small.currentStep());

        // And no effect is ever handed more than maxBlockSize samples.
        GlitchEngine checked;
        checked.prepare (sampleRate, 64);
        FakeRack rack (FakeEffect::Output::scaledCapture, { -1.0f, 0.5f, 0.25f, -0.5f, 2.0f, -2.0f });
        rack.install (checked);
        checked.setParams (params);
        render (checked, input, 5000, transportAt);

        for (const auto& fake : rack.fakes)
        {
            CHECK (fake.largestBlock <= 64);
            CHECK (! fake.readAhead);
        }
    }
}

TEST_CASE ("engine: output does not depend on the block size")
{
    const auto input = makeNoise (100000, 21, 0.7f);

    // Any effect set: the real ones as they stand, and fakes that isolate the engine's own logic.
    for (const bool useFakes : { false, true })
    {
        for (const auto& transportAt : { hostFrom (0.3, 133.0), hostFrom (-0.6, 174.0), hostStopped (97.0) })
        {
            auto params = makeParams (useFakes ? busyPattern() : defaultPattern(), 0.8f, 0.7f);
            params.outputGainDb = -3.0f;

            const auto renderWith = [&] (int blockSize, int maxBlockSize) {
                GlitchEngine engine;
                engine.prepare (sampleRate, maxBlockSize);
                FakeRack rack (FakeEffect::Output::scaledCapture, { -1.0f, 0.5f, 0.25f, -0.5f, 2.0f, -2.0f });

                if (useFakes)
                    rack.install (engine);

                engine.setParams (params);
                return render (engine, input, blockSize, transportAt);
            };

            const auto whole = renderWith (input.size(), input.size());
            CHECK (finite (whole));

            for (const int blockSize : { 1, 7, 64, 512 })
                CHECK (maxDifference (renderWith (blockSize, blockSize), whole) <= 1.0e-5);
        }
    }
}

TEST_CASE ("engine: steps follow the host position sample by sample")
{
    const double bpm = 133.0;
    const double rate = 44100.0;
    const double startPpq = 0.3;
    const double stepBeats = 1.0 / 6.0;
    const int numSteps = 12;

    auto params = makeParams (busyPattern(), 1.0f, 1.0f, stepBeats);
    params.sequencer.numSteps = numSteps;

    GlitchEngine engine;
    engine.prepare (rate, 1);
    engine.setParams (params);

    const long double samplesPerBeat = static_cast<long double> (rate) * 60.0L / bpm;
    StereoBuffer sample (1);
    int mismatches = 0;

    for (int64_t index = 0; index < 30000; ++index)
    {
        engine.process (sample.block(), playing (startPpq + static_cast<double> (index) / static_cast<double> (samplesPerBeat), bpm));

        const auto stepPosition = (startPpq + static_cast<long double> (index) / samplesPerBeat) / stepBeats;
        const auto stepNumber = static_cast<int64_t> (std::floor (stepPosition));

        // Skip samples whose position sits within rounding of a boundary.
        if (stepPosition - std::floor (stepPosition) < 1.0e-6L || std::ceil (stepPosition) - stepPosition < 1.0e-6L)
            continue;

        const auto expectedStep = static_cast<int> (stepNumber % numSteps);

        if (engine.currentStep() != expectedStep || engine.currentEffect() != params.sequencer.steps[static_cast<size_t> (expectedStep)])
            ++mismatches;
    }

    CHECK (mismatches == 0);
}

TEST_CASE ("engine: free-runs from step 0 while the host is stopped")
{
    GlitchEngine engine;
    engine.prepare (sampleRate, 100);
    engine.setParams (makeParams (busyPattern()));
    CHECK (engine.currentStep() == -1);

    StereoBuffer block (100);

    for (int64_t start = 0; start < sixteenth * 20; start += 100)
    {
        engine.process (block.block(), stopped());
        const auto lastSample = start + 99;
        CHECK (engine.currentStep() == static_cast<int> ((lastSample / sixteenth) % 16));
    }

    engine.reset();
    CHECK (engine.currentStep() == -1);
    engine.process (block.block(), stopped());
    CHECK (engine.currentStep() == 0);
    CHECK (engine.currentEffect() == busyPattern()[0]);
}

TEST_CASE ("engine: effects are triggered exactly when a step using them begins")
{
    using E = EffectType;
    const Pattern pattern = { E::Stutter, E::Stutter, E::None, E::Reverse, E::Stutter, E::Reverse, E::Reverse, E::None,
                              E::Stutter, E::None, E::None, E::Reverse, E::Stutter, E::Stutter, E::Reverse, E::None };
    const double bpm = 133.0;
    const double rate = 44100.0;
    auto params = makeParams (pattern, 1.0f, 0.75f, 1.0 / 6.0);

    GlitchEngine engine;
    engine.prepare (rate, 512);
    FakeEffect stutter (FakeEffect::Output::constant, 0.5f);
    FakeEffect reverse (FakeEffect::Output::constant, -0.5f);
    GlitchEngineTestAccess::replaceEffect (engine, E::Stutter, stutter);
    GlitchEngineTestAccess::replaceEffect (engine, E::Reverse, reverse);
    engine.setParams (params);

    const int fadeLength = GlitchEngineTestAccess::crossfadeLength (engine);
    CHECK (fadeLength == static_cast<int> (std::lround (0.003 * rate)));

    // The oracle: a sequencer fed the same blocks says which steps begin where.
    StepSequencer oracle;
    oracle.prepare (rate);
    std::vector<FakeEffect::Trigger> expectedStutter, expectedReverse;
    std::vector<EffectType> sourceAt;

    const auto total = 150000;
    const auto transportAt = hostFrom (0.3, bpm, rate);
    StereoBuffer buffer (512);

    for (int64_t start = 0; start < total; start += 512)
    {
        const auto transport = transportAt (start);
        engine.process (buffer.block(), transport);

        oracle.beginBlock (params.sequencer, transport, start, 512);
        StepSegment segment;

        while (oracle.nextSegment (segment))
        {
            if (segment.startsStep && segment.effect == E::Stutter)
                expectedStutter.push_back ({ start + segment.offset, segment.context });

            if (segment.startsStep && segment.effect == E::Reverse)
                expectedReverse.push_back ({ start + segment.offset, segment.context });

            sourceAt.insert (sourceAt.end(), static_cast<size_t> (segment.length), segment.effect);
        }
    }

    const auto sameTriggers = [&] (const std::vector<FakeEffect::Trigger>& actual, const std::vector<FakeEffect::Trigger>& expected) {
        if (actual.size() != expected.size())
            return false;

        for (size_t i = 0; i < actual.size(); ++i)
            if (actual[i].index != expected[i].index || actual[i].step.stepStart != expected[i].step.stepStart
                || actual[i].step.stepLength != expected[i].step.stepLength
                || actual[i].step.samplesPerBeat != expected[i].step.samplesPerBeat || actual[i].step.sampleRate != rate)
                return false;

        return true;
    };

    CHECK (expectedStutter.size() > 10);
    CHECK (expectedReverse.size() > 10);
    CHECK (sameTriggers (stutter.triggers, expectedStutter));
    CHECK (sameTriggers (reverse.triggers, expectedReverse));

    // Each effect is processed from its first step until its fade-out completes, and no longer.
    const auto expectedRanges = [&] (EffectType type) {
        std::vector<FakeEffect::Range> ranges;

        for (size_t i = 0; i < sourceAt.size(); ++i)
        {
            if (sourceAt[i] != type)
                continue;

            const auto runStart = static_cast<int64_t> (i);
            while (i < sourceAt.size() && sourceAt[i] == type)
                ++i;

            const auto end = std::min<int64_t> (static_cast<int64_t> (i) + fadeLength, total);

            if (i == sourceAt.size())
                ranges.push_back ({ runStart, static_cast<int64_t> (i) });
            else if (! ranges.empty() && ranges.back().end >= runStart)
                ranges.back().end = end;
            else
                ranges.push_back ({ runStart, end });
        }

        return ranges;
    };

    const auto sameRanges = [] (const std::vector<FakeEffect::Range>& a, const std::vector<FakeEffect::Range>& b) {
        if (a.size() != b.size())
            return false;

        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].begin != b[i].begin || a[i].end != b[i].end)
                return false;

        return true;
    };

    CHECK (sameRanges (stutter.processed, expectedRanges (E::Stutter)));
    CHECK (sameRanges (reverse.processed, expectedRanges (E::Reverse)));
}

TEST_CASE ("engine: dry to effect and back crossfades over 3 ms without a click")
{
    using E = EffectType;
    Pattern pattern = everyStep (E::None);
    pattern[1] = E::Stutter;

    GlitchEngine engine;
    engine.prepare (sampleRate, 512);
    FakeEffect fake (FakeEffect::Output::constant, 0.0f);
    GlitchEngineTestAccess::replaceEffect (engine, E::Stutter, fake);
    engine.setParams (makeParams (pattern));

    const int fade = GlitchEngineTestAccess::crossfadeLength (engine);
    CHECK (fade == 144);

    const auto input = makeDc (30000, 0.5f);
    const auto output = render (engine, input, 512, hostFrom (0.0));

    // Dry before, a raised-cosine fade to the wet signal from the step's first sample, wet, and back.
    CHECK (maxDifference (output, input, 0, at (sixteenth)) == 0.0);

    double fadeError = 0.0;

    for (int e = 0; e < fade; ++e)
    {
        fadeError = std::max (fadeError, std::fabs (output.left[at (sixteenth + e)] - 0.5 * (1.0 - fadeWeight (e, fade))));
        fadeError = std::max (fadeError, std::fabs (output.left[at (2 * sixteenth + e)] - 0.5 * fadeWeight (e, fade)));
    }

    CHECK (fadeError < 1.0e-6);
    CHECK (output.left[at (sixteenth)] < 0.5f);
    CHECK (peak (output.left, at (sixteenth + fade), at (2 * sixteenth)) == 0.0f);
    CHECK (maxDifference (output, input, at (2 * sixteenth + fade), at (3 * sixteenth)) == 0.0);
    CHECK (maxJump (output.left) < 0.5f * static_cast<float> (pi / 2.0 / (fade + 1)) * 1.01f);

    CHECK (fake.triggers.size() == 1);
    CHECK (fake.processed.size() == 1);

    if (fake.processed.size() == 1)
    {
        CHECK (fake.processed[0].begin == sixteenth);
        CHECK (fake.processed[0].end == 2 * sixteenth + fade);
    }
}

TEST_CASE ("engine: an effect to effect crossfade renders both sources")
{
    using E = EffectType;
    Pattern pattern = everyStep (E::None);
    pattern[0] = E::Stutter;
    pattern[1] = E::Reverse;

    GlitchEngine engine;
    engine.prepare (sampleRate, 256);
    FakeEffect first (FakeEffect::Output::constant, 0.5f);
    FakeEffect second (FakeEffect::Output::constant, -0.5f);
    GlitchEngineTestAccess::replaceEffect (engine, E::Stutter, first);
    GlitchEngineTestAccess::replaceEffect (engine, E::Reverse, second);
    engine.setParams (makeParams (pattern));

    const int fade = GlitchEngineTestAccess::crossfadeLength (engine);
    const auto output = render (engine, StereoBuffer (20000), 256, hostFrom (0.0));

    // Silence in, so the first step fades up from 0 to +0.5, then straight across to -0.5.
    double error = 0.0;

    for (int e = 0; e < fade; ++e)
    {
        error = std::max (error, std::fabs (output.left[at (e)] - 0.5 * fadeWeight (e, fade)));
        error = std::max (error, std::fabs (output.left[at (sixteenth + e)] - (0.5 - fadeWeight (e, fade))));
    }

    CHECK (error < 1.0e-6);
    CHECK (output.left[at (sixteenth - 1)] == 0.5f);
    CHECK (output.left[at (sixteenth + fade)] == -0.5f);
    CHECK (maxJump (output.left) < static_cast<float> (pi / 2.0 / (fade + 1)) * 1.01f);

    CHECK (first.processed.size() == 1 && first.processed[0].begin == 0 && first.processed[0].end == sixteenth + fade);
    CHECK (second.processed.size() == 1 && second.processed[0].begin == sixteenth && second.processed[0].end == 2 * sixteenth + fade);
}

TEST_CASE ("engine: the same effect on consecutive steps is retriggered, not crossfaded")
{
    GlitchEngine engine;
    engine.prepare (sampleRate, 333);
    FakeEffect fake (FakeEffect::Output::scaledCapture, -1.0f);
    GlitchEngineTestAccess::replaceEffect (engine, EffectType::Stutter, fake);
    engine.setParams (makeParams (everyStep (EffectType::Stutter)));

    const int fade = GlitchEngineTestAccess::crossfadeLength (engine);
    const auto input = makeSine (60000, 220.0, sampleRate);
    const auto output = render (engine, input, 333, hostFrom (0.0));

    // After the opening fade from dry, the output is exactly the wet signal, step boundaries included.
    bool fullyWet = true;

    for (size_t i = at (fade); i < input.left.size(); ++i)
        fullyWet = fullyWet && output.left[i] == -input.left[i] && output.right[i] == -input.right[i];

    CHECK (fullyWet);
    CHECK (fake.triggers.size() == 10);
    CHECK (fake.processed.size() == 1);

    for (size_t n = 0; n < fake.triggers.size(); ++n)
        CHECK (fake.triggers[n].index == static_cast<int64_t> (n) * sixteenth);
}

TEST_CASE ("engine: a transport jump retriggers the step's effect at once")
{
    GlitchEngine engine;
    engine.prepare (sampleRate, 480);
    FakeEffect fake (FakeEffect::Output::constant, 0.25f);
    GlitchEngineTestAccess::replaceEffect (engine, EffectType::Stutter, fake);
    engine.setParams (makeParams (everyStep (EffectType::Stutter)));

    for (int64_t start = 0; start < 48000; start += 480)
    {
        StereoBuffer silence (480);
        engine.process (silence.block(), playing (static_cast<double> (start) / 24000.0));
    }

    CHECK (fake.triggers.size() == 8);

    // Loop back to ppq 0.6: step 2 again, which began at ppq 0.5.
    StereoBuffer buffer (480);
    engine.process (buffer.block(), playing (0.6));
    CHECK (fake.triggers.size() == 9);
    CHECK (engine.currentStep() == 2);

    if (fake.triggers.size() == 9)
    {
        CHECK (fake.triggers.back().index == 48000);
        CHECK (fake.triggers.back().step.stepStart == 48000 - 2400);
        CHECK (fake.triggers.back().step.stepLength == sixteenth);
    }

    // The source did not change, so the engine stayed fully wet: no dry leaks in.
    CHECK (peak (buffer.left) == 0.25f && buffer.left[0] == 0.25f);
}

TEST_CASE ("engine: source changes inside one crossfade stay smooth and bounded")
{
    // The host jumps every few samples, so new steps (and sources) keep arriving mid-fade.
    for (const int jumpEvery : { 50, 20, 3 })
    {
        GlitchEngine engine;
        engine.prepare (sampleRate, 512);
        FakeRack rack (FakeEffect::Output::constant, { 0.8f, -0.6f, 0.3f, -0.2f, 0.5f, -0.9f });
        rack.install (engine);
        engine.setParams (makeParams (busyPattern()));

        const auto input = makeDc (20000, 0.1f);
        uint32_t noise = 77;

        const auto output = render (engine, input, jumpEvery, [&] (int64_t) {
            noise ^= noise << 13;
            noise ^= noise >> 17;
            noise ^= noise << 5;
            return playing (static_cast<double> (noise % 16000) / 4000.0);
        });

        CHECK (finite (output));

        // Always a blend of the sources: never outside their range.
        const auto [lowest, highest] = std::minmax_element (output.left.begin(), output.left.end());
        CHECK (*lowest >= -0.9f - 1.0e-5f);
        CHECK (*highest <= 0.8f + 1.0e-5f);

        // Up to four sources overlap without dropping one, so no clicks. Faster than that, a
        // nearly silent layer may be dropped: bounded, but not guaranteed click-free.
        if (jumpEvery == 50)
            CHECK (maxJump (output.left) < 0.2f);
    }
}

TEST_CASE ("engine: mix changes are smoothed over about 20 ms")
{
    GlitchEngine engine;
    engine.prepare (sampleRate, 500);
    FakeRack rack (FakeEffect::Output::constant, { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f });
    rack.install (engine);

    auto params = makeParams (everyStep (EffectType::Stutter), 1.0f, 1.0f, 1.0);
    engine.setParams (params);

    const int ramp = GlitchEngineTestAccess::smoothingLength (sampleRate);
    CHECK (ramp == 960);

    // Wet is silence and dry is 1, so the output is 1 - mix.
    const auto output = render (engine, makeDc (60000, 1.0f), 500, hostStopped(), [&] (GlitchEngine& e, int64_t start) {
        if (start == 20000)
            params.mix = 0.0f;
        else if (start == 40000)
            params.mix = 1.0f;
        else if (start == 40500)
            params.mix = 0.5f; // a new target half-way through a ramp
        else
            return;

        e.setParams (params);
    });

    CHECK (output.left[19999] == 0.0f);

    double stepError = 0.0;

    for (int k = 0; k < ramp; ++k)
        stepError = std::max (stepError, std::fabs (output.left[at (20000 + k)] - (k + 1.0) / ramp));

    CHECK (stepError < 1.0e-4);
    CHECK (output.left[at (20000 + ramp - 1)] == 1.0f);
    CHECK (output.left[39999] == 1.0f);
    CHECK (std::fabs (output.left[at (40500 + ramp)] - 0.5f) < 1.0e-6f);
    CHECK (output.left[59999] == 0.5f);

    // No zipper steps anywhere after the opening fade from dry.
    CHECK (maxJump (output.left, 1000) <= 1.0f / static_cast<float> (ramp) * 1.01f);
}

TEST_CASE ("engine: output gain changes are smoothed")
{
    GlitchEngine engine;
    engine.prepare (sampleRate, 400);

    auto params = makeParams (busyPattern(), 0.0f);
    engine.setParams (params);

    const int ramp = GlitchEngineTestAccess::smoothingLength (sampleRate);
    const auto output = render (engine, makeDc (8000, 0.5f), 400, hostFrom (0.0), [&] (GlitchEngine& e, int64_t start) {
        if (start == 2000)
        {
            params.outputGainDb = -24.0f;
            e.setParams (params);
        }
    });

    const auto quiet = 0.5f * static_cast<float> (std::pow (10.0, -24.0 / 20.0));

    CHECK (output.left[1999] == 0.5f);
    CHECK (output.left[at (2000 + ramp - 1)] == quiet);
    CHECK (output.left[7999] == quiet);
    CHECK (maxJump (output.left) <= (0.5f - quiet) / static_cast<float> (ramp) * 1.01f);

    // A straight line, not a jump.
    CHECK (std::fabs (output.left[at (2000 + ramp / 2)] - (0.5f + quiet) / 2.0f) < 1.0e-3f);
}

TEST_CASE ("engine: mix blends dry and wet linearly")
{
    for (const float mix : { 0.25f, 0.5f, 0.9f })
    {
        GlitchEngine engine;
        engine.prepare (sampleRate, 256);
        FakeEffect silent (FakeEffect::Output::constant, 0.0f);
        FakeEffect inverted (FakeEffect::Output::scaledCapture, -1.0f);
        GlitchEngineTestAccess::replaceEffect (engine, EffectType::Stutter, silent);
        GlitchEngineTestAccess::replaceEffect (engine, EffectType::Gate, inverted);

        Pattern pattern = everyStep (EffectType::Stutter);
        pattern[2] = EffectType::Gate;
        engine.setParams (makeParams (pattern, mix));

        const auto input = makeNoise (3 * sixteenth, 4);
        const auto output = render (engine, input, 256, hostFrom (0.0));

        // Silent wet: the dry part only. Inverted wet: dry (1 - mix) - dry mix.
        double silentError = 0.0;
        double invertedError = 0.0;

        for (size_t i = 200; i < at (sixteenth); ++i)
            silentError = std::max (silentError, std::fabs (output.left[i] - input.left[i] * (1.0 - mix)));

        for (size_t i = at (2 * sixteenth + 200); i < at (3 * sixteenth); ++i)
            invertedError = std::max (invertedError, std::fabs (output.left[i] - input.left[i] * (1.0 - 2.0 * mix)));

        CHECK (silentError < 1.0e-6);
        CHECK (invertedError < 1.0e-6);
    }
}

TEST_CASE ("engine: effects read the dry input from the capture buffer before they run")
{
    GlitchEngine engine;
    engine.prepare (sampleRate, 128);
    FakeRack rack (FakeEffect::Output::scaledCapture, { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f });
    rack.install (engine);
    engine.setParams (makeParams (busyPattern(), 1.0f, 0.8f));

    // An identity effect read from the capture buffer at `now`: every fade is between equal signals.
    const auto input = makeNoise (200000, 8);
    const auto output = render (engine, input, 100, hostFrom (0.1));

    CHECK (maxDifference (output, input) < 1.0e-6);

    for (const auto& fake : rack.fakes)
    {
        CHECK (! fake.readAhead);
        CHECK (! fake.processed.empty());
    }
}

TEST_CASE ("engine: preparing again at a new sample rate resets everything")
{
    GlitchEngine engine;
    FakeEffect fake (FakeEffect::Output::constant, 0.3f);
    GlitchEngineTestAccess::replaceEffect (engine, EffectType::Stutter, fake);

    engine.prepare (44100.0, 512);
    engine.setParams (makeParams (everyStep (EffectType::Stutter)));
    render (engine, makeNoise (30000, 2), 512, hostFrom (3.7, 140.0, 44100.0));
    CHECK (engine.currentStep() >= 0);

    engine.prepare (96000.0, 256);
    CHECK (engine.currentStep() == -1);
    CHECK (engine.currentEffect() == EffectType::None);
    CHECK (fake.prepareCount == 2);
    CHECK_NEAR (fake.preparedSampleRate, 96000.0, 0.0);
    CHECK (fake.preparedBlockSize == 256);

    // Free-running from step 0 at the new rate, with the capture buffer counting from 0 again.
    fake.triggers.clear();
    const auto output = render (engine, makeNoise (30000, 2), 256, hostStopped());

    CHECK (finite (output));
    CHECK (fake.triggers.size() == 3);

    if (! fake.triggers.empty())
    {
        CHECK (fake.triggers[0].index == 0);
        CHECK (fake.triggers[0].step.stepStart == 0);
        CHECK (fake.triggers[0].step.stepLength == 12000);
        CHECK_NEAR (fake.triggers[0].step.sampleRate, 96000.0, 0.0);
    }

    // Garbage arguments fall back to something safe.
    engine.prepare (std::numeric_limits<double>::quiet_NaN(), -5);
    auto buffer = makeNoise (1000, 3);
    engine.process (buffer.block(), stopped());
    CHECK (finite (buffer));
}

TEST_CASE ("engine: NaN, zero and negative tempos run at 120 bpm")
{
    const double nan = std::numeric_limits<double>::quiet_NaN();

    for (const double bpm : { nan, 0.0, -10.0, std::numeric_limits<double>::infinity() })
    {
        for (const bool hostPlaying : { false, true })
        {
            GlitchEngine engine;
            engine.prepare (sampleRate, 512);
            FakeEffect fake (FakeEffect::Output::constant, 0.3f);
            GlitchEngineTestAccess::replaceEffect (engine, EffectType::Stutter, fake);
            engine.setParams (makeParams (everyStep (EffectType::Stutter)));

            const auto output = render (engine, makeNoise (20000, 5), 512, [&] (int64_t start) {
                return hostPlaying ? playing (static_cast<double> (start) / 24000.0, bpm) : stopped (bpm);
            });

            CHECK (finite (output));
            CHECK (fake.triggers.size() == 4);

            for (const auto& trigger : fake.triggers)
            {
                CHECK_NEAR (trigger.step.samplesPerBeat, 24000.0, 1e-9);
                CHECK (trigger.step.stepLength == sixteenth);
            }
        }
    }
}

TEST_CASE ("engine: changing the step count mid-play keeps the reported step in range")
{
    GlitchEngine engine;
    engine.prepare (sampleRate, 128);
    auto params = makeParams (busyPattern());
    StereoBuffer buffer (128);
    uint32_t noise = 5;
    bool inRange = true;

    for (int64_t start = 0; start < 400000; start += 128)
    {
        noise ^= noise << 13;
        noise ^= noise >> 17;
        noise ^= noise << 5;
        params.sequencer.numSteps = 1 + static_cast<int> (noise % 16);
        engine.setParams (params);
        engine.process (buffer.block(), playing (static_cast<double> (start) / 24000.0));
        inRange = inRange && engine.currentStep() >= 0 && engine.currentStep() < params.sequencer.numSteps;
    }

    CHECK (inRange);
    CHECK (finite (buffer));
}

TEST_CASE ("engine: output stays finite for silence, DC, full-scale noise, denormals and extreme parameters")
{
    using E = EffectType;
    const Pattern everyEffect = { E::Stutter, E::Reverse, E::TapeStop, E::Stretch, E::Crusher, E::Gate, E::Stutter, E::Reverse,
                                  E::TapeStop, E::Stretch, E::Crusher, E::Gate, E::None, E::Stutter, E::Stretch, E::Reverse };

    EngineParams low = makeParams (everyEffect, 1.0f, 1.0f, 0.125);
    low.outputGainDb = 12.0f;
    low.stutter = { 0.03125f, -24.0f, 1.0f };
    low.reverse = { 0.5f };
    low.tapeStop = { 0.05f, 0.0f };
    low.stretch = { 16.0f, 10.0f };
    low.crusher = { 1.0f, 64.0f };
    low.gate = { 0.03125f, 0.05f };

    EngineParams high = makeParams (everyEffect, 1.0f, 1.0f, 1.0);
    high.outputGainDb = 12.0f;
    high.stutter = { 0.5f, 24.0f, 0.0f };
    high.reverse = { 2.0f };
    high.tapeStop = { 1.0f, 1.0f };
    high.stretch = { 1.0f, 250.0f };
    high.crusher = { 16.0f, 1.0f };
    high.gate = { 0.5f, 0.95f };

    const int length = 96000;
    const StereoBuffer inputs[] = { StereoBuffer (length), makeDc (length, 1.0f), makeDc (length, -1.0f),
                                    makeNoise (length, 13, 1.0f), makeDc (length, 1.0e-40f) };

    for (const auto& params : { low, high })
    {
        for (const double bpm : { 20.0, 999.0 })
        {
            for (const auto& input : inputs)
            {
                const auto output = renderFresh (params, input, 512, hostFrom (0.0, bpm));
                CHECK (finite (output));
                CHECK (stereoPeak (output) <= 8.0f);
            }
        }
    }
}

TEST_CASE ("engine: a broken effect cannot push NaN into the output")
{
    GlitchEngine engine;
    engine.prepare (sampleRate, 256);
    FakeEffect broken (FakeEffect::Output::notANumber, 0.0f);
    GlitchEngineTestAccess::replaceEffect (engine, EffectType::Stutter, broken);
    engine.setParams (makeParams (everyStep (EffectType::Stutter), 0.5f));

    const auto output = render (engine, makeNoise (20000, 6), 256, hostFrom (0.0));
    CHECK (finite (output));
}

TEST_CASE ("engine: output never depends on future input")
{
    const int length = 120000;
    const auto first = makeNoise (length, 31, 0.8f);
    const auto other = makeNoise (length, 32, 0.8f);

    for (const int64_t from : { int64_t { 30123 }, int64_t { 6000 }, int64_t { 77777 } })
    {
        auto spliced = first;
        std::copy (other.left.begin() + from, other.left.end(), spliced.left.begin() + from);
        std::copy (other.right.begin() + from, other.right.end(), spliced.right.begin() + from);

        for (const auto& pattern : { defaultPattern(), busyPattern() })
        {
            const auto params = makeParams (pattern, 1.0f, 0.8f);
            const auto a = renderFresh (params, first, 512, hostFrom (0.2, 128.0));
            const auto b = renderFresh (params, spliced, 512, hostFrom (0.2, 128.0));

            CHECK (maxDifference (a, b, 0, at (from)) == 0.0);
        }
    }
}

TEST_CASE ("engine: the real effects do not click at step boundaries, source changes or loop jumps")
{
    // A 50 Hz sine moves at most 0.0033 per sample; an unfaded switch would jump by up to 1.
    // The Crusher's quantisation steps (1/32 at 6 bits) are its sound, not a click.
    const auto input = makeSine (static_cast<int> (sampleRate) * 6, 50.0, sampleRate, 0.5f);

    for (const auto& pattern : { busyPattern(), defaultPattern() })
    {
        for (const bool loopJumps : { false, true })
        {
            // Optionally the host loops 2.3 beats every 3.1 beats, retriggering steps part-way through.
            const auto output = renderFresh (makeParams (pattern), input, 480, [&] (int64_t start) {
                const double beats = static_cast<double> (start) / 24000.0;
                return playing (loopJumps ? std::fmod (beats, 3.1) + 0.8 * std::floor (beats / 3.1) : beats);
            });

            CHECK (finite (output));
            CHECK (std::max (maxJump (output.left), maxJump (output.right)) < 0.05f);
        }
    }
}

TEST_CASE ("engine: the default pattern keeps empty steps dry and changes the effect steps")
{
    // Integration smoke test with the real effects: something happens on the effect steps,
    // the empty steps stay dry, and nothing clicks into a broken state.
    const auto input = makeSine (sixteenth * 16, 330.0, sampleRate, 0.5f);
    const auto output = renderFresh (makeParams (defaultPattern()), input, 512, hostFrom (0.0));

    CHECK (finite (output));

    // Steps 0 and 1 are empty: exactly dry.
    CHECK (maxDifference (output, input, 0, at (2 * sixteenth)) == 0.0);

    // Step 4 is empty too, once the fade out of step 2's stutter is over.
    CHECK (maxDifference (output, input, at (4 * sixteenth + 200), at (5 * sixteenth)) == 0.0);

    // Step 2 stutters: once its 1/32 slice starts repeating, it no longer matches the input.
    CHECK (maxDifference (output, input, at (2 * sixteenth + 3200), at (3 * sixteenth)) > 0.05);
}
