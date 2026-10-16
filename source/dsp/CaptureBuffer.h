#pragma once

#include <cstdint>
#include <vector>

namespace splinter
{

/** Ring buffer holding the most recent dry input, addressed by absolute sample index.

    The engine appends every incoming block before any effect runs, so while
    processing the sample at absolute index `now`, an effect may read anything in
    [oldestIndex(), now]. Reads outside the stored range return silence, so a
    stale index can never touch memory outside the buffer.

    prepare() allocates; every other member is real-time safe. */
class CaptureBuffer
{
public:
    static constexpr int numChannels = 2;

    void prepare (int capacitySamples);
    void reset() noexcept;

    /** Appends n samples to both channels and advances writeIndex() by n. */
    void write (const float* left, const float* right, int n) noexcept;

    /** Absolute index the next written sample will get (= total samples written since reset). */
    int64_t writeIndex() const noexcept { return writeIndex_; }

    /** Oldest absolute index still stored. */
    int64_t oldestIndex() const noexcept;

    /** Newest absolute index stored (writeIndex() - 1), or -1 when empty. */
    int64_t newestIndex() const noexcept { return writeIndex_ - 1; }

    int capacity() const noexcept { return capacity_; }

    bool contains (int64_t index) const noexcept { return index >= oldestIndex() && index <= newestIndex(); }

    /** Sample at an absolute index, or 0 if it is not stored. */
    float sample (int channel, int64_t index) const noexcept;

    /** 4-point cubic Hermite interpolation at a fractional absolute position. */
    float sampleInterpolated (int channel, double position) const noexcept;

private:
    std::vector<float> data_[numChannels];
    int capacity_ = 0; // power of two
    int64_t mask_ = 0;
    int64_t writeIndex_ = 0;
};

} // namespace splinter
