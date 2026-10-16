#include "CaptureBuffer.h"

#include <algorithm>
#include <cmath>

namespace splinter
{

void CaptureBuffer::prepare (int capacitySamples)
{
    int size = 1;
    while (size < std::max (capacitySamples, 4))
        size <<= 1;

    capacity_ = size;
    mask_ = size - 1;

    for (auto& channel : data_)
        channel.assign (static_cast<size_t> (size), 0.0f);

    writeIndex_ = 0;
}

void CaptureBuffer::reset() noexcept
{
    for (auto& channel : data_)
        std::fill (channel.begin(), channel.end(), 0.0f);

    writeIndex_ = 0;
}

void CaptureBuffer::write (const float* left, const float* right, int n) noexcept
{
    if (capacity_ == 0)
        return;

    for (int i = 0; i < n; ++i)
    {
        const auto slot = static_cast<size_t> ((writeIndex_ + i) & mask_);
        data_[0][slot] = left[i];
        data_[1][slot] = right[i];
    }

    writeIndex_ += n;
}

int64_t CaptureBuffer::oldestIndex() const noexcept
{
    return std::max<int64_t> (0, writeIndex_ - capacity_);
}

float CaptureBuffer::sample (int channel, int64_t index) const noexcept
{
    if (capacity_ == 0 || channel < 0 || channel >= numChannels || ! contains (index))
        return 0.0f;

    return data_[channel][static_cast<size_t> (index & mask_)];
}

float CaptureBuffer::sampleInterpolated (int channel, double position) const noexcept
{
    if (! std::isfinite (position))
        return 0.0f;

    const auto base = static_cast<int64_t> (std::floor (position));
    const auto t = static_cast<float> (position - static_cast<double> (base));

    const float y0 = sample (channel, base - 1);
    const float y1 = sample (channel, base);
    const float y2 = sample (channel, base + 1);
    const float y3 = sample (channel, base + 2);

    const float c0 = y1;
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);

    return ((c3 * t + c2) * t + c1) * t + c0;
}

} // namespace splinter
