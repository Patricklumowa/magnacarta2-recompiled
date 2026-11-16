#pragma once
#include <cstdint>
struct PPCContext;

namespace mc2::native {
// A bounded, opt-in comparison draw. Never suppresses the original guest draw.
void CheckLiveMovie(const PPCContext& context, uint8_t* base) noexcept;
}
