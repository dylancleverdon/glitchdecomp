#include "EffectParams.h"

namespace splinter
{

const char* effectTypeName (EffectType type) noexcept
{
    switch (type)
    {
        case EffectType::None:     return "Off";
        case EffectType::Stutter:  return "Stutter";
        case EffectType::Reverse:  return "Reverse";
        case EffectType::TapeStop: return "Tape Stop";
        case EffectType::Stretch:  return "Stretch";
        case EffectType::Crusher:  return "Crusher";
        case EffectType::Gate:     return "Gate";
    }

    return "Off";
}

} // namespace splinter
