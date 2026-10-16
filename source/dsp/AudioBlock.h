#pragma once

namespace splinter
{

/** A non-owning view of a stereo run of samples. Both pointers are always valid
    and never alias (the plugin copies mono input into a scratch right channel). */
struct StereoBlock
{
    float* left = nullptr;
    float* right = nullptr;
    int numSamples = 0;

    StereoBlock subBlock (int offset, int length) const noexcept
    {
        return { left + offset, right + offset, length };
    }
};

} // namespace splinter
