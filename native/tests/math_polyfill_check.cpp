#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>

extern "C" float roundevenf(float);

int main() {
  float (*volatile operation)(float) = roundevenf;
  const float cases[][2] = {{0.5f, 0.0f}, {1.5f, 2.0f}, {2.5f, 2.0f},
                            {3.5f, 4.0f}, {-0.5f, -0.0f}, {-1.5f, -2.0f},
                            {-2.5f, -2.0f}, {-3.5f, -4.0f}, {1.1f, 1.0f},
                            {-1.9f, -2.0f}, {0.0f, 0.0f}, {-0.0f, -0.0f},
                            {16777216.0f, 16777216.0f}};
  for (int mode : {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO}) {
    if (std::fesetround(mode) != 0) return 1;
    for (const auto& test : cases) {
      const auto actual = std::bit_cast<std::uint32_t>(operation(test[0]));
      if (actual != std::bit_cast<std::uint32_t>(test[1])) return 2;
    }
  }
  if (!std::isnan(operation(std::numeric_limits<float>::quiet_NaN()))) return 3;
  for (float value : {std::numeric_limits<float>::infinity(),
                      -std::numeric_limits<float>::infinity()}) {
    if (operation(value) != value) return 4;
  }
  return 0;
}
