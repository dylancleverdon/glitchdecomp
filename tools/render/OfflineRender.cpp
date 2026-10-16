#include "OfflineRender.h"

#include "dsp/GlitchEngine.h"

#include <algorithm>
#include <memory>

namespace splinter::render
{

TransportInfo transportForBlock (int64_t blockStartSample, double sampleRate, double bpm) noexcept
{
    TransportInfo transport;
    transport.isPlaying = true;
    transport.hasPpqPosition = true;
    transport.bpm = bpm;

    // From the sample count rather than accumulated per block, so long renders do not drift.
    transport.ppqPosition = static_cast<double> (blockStartSample) * bpm / (60.0 * sampleRate);
    return transport;
}

void renderThroughEngine (const EngineParams& params, double sampleRate, double bpm, int blockSize,
                          std::vector<float>& left, std::vector<float>& right)
{
    blockSize = std::max (1, blockSize);

    // On the heap: the engine may hold large buffers directly.
    auto engine = std::make_unique<GlitchEngine>();
    engine->prepare (sampleRate, blockSize);
    engine->reset();
    engine->setParams (params);

    const auto total = static_cast<int64_t> (std::min (left.size(), right.size()));

    for (int64_t start = 0; start < total; start += blockSize)
    {
        const auto length = static_cast<int> (std::min<int64_t> (blockSize, total - start));
        const auto offset = static_cast<size_t> (start);
        engine->process ({ left.data() + offset, right.data() + offset, length }, transportForBlock (start, sampleRate, bpm));
    }
}

} // namespace splinter::render
