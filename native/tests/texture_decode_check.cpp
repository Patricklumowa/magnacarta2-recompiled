#include "texture_decode.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
void Check(bool value, const char* reason) {
  if (!value) { std::cerr << reason << '\n'; std::exit(1); }
}
std::array<uint8_t, 24> Fetch(uint32_t width, uint32_t height, uint32_t pitch,
                              bool tiled, uint32_t endian = 0, uint32_t swizzle = 0x688,
                              uint32_t signs = 0, uint32_t format = 6) {
  const uint32_t words[] = {2u | signs | (pitch << 22) | (uint32_t(tiled) << 31),
                            0x00100000u | format | (endian << 6),
                            (width - 1) | ((height - 1) << 13), swizzle << 1, 0, 1u << 9};
  std::array<uint8_t, 24> result{};
  for (size_t i = 0; i < 6; ++i)
    for (uint32_t byte = 0; byte < 4; ++byte)
      result[i * 4 + byte] = uint8_t(words[i] >> (24 - byte * 8));
  return result;
}
}  // namespace

int main() {
  using namespace mc2::texture;
  std::string error;
  BaseRGBA8 description;
  std::vector<uint8_t> rgba;
  Check(DescribeBaseRGBA8(Fetch(3, 2, 2, false), description, error), error.c_str());
  Check(description.physical_address == 0x100000 && description.row_pitch_bytes == 256 &&
        description.source_bytes == 268, "Linear span must exclude unused last-row padding");
  std::vector<uint8_t> linear(description.source_bytes, 0);
  linear[256] = 11; linear[257] = 22; linear[258] = 33; linear[259] = 44;
  Check(DecodeBaseRGBA8(description, linear, rgba, error), error.c_str());
  Check(rgba[12] == 11 && rgba[13] == 22 && rgba[14] == 33 && rgba[15] == 44,
        "Linear pitch or RGBA component ordering is incorrect");
  const auto before = rgba;
  linear.pop_back();
  Check(!DecodeBaseRGBA8(description, linear, rgba, error) && rgba == before,
        "Truncated input must fail without altering the previous output");

  for (uint32_t endian = 0; endian < 4; ++endian) {
    Check(DescribeBaseRGBA8(Fetch(1, 1, 2, false, endian), description, error), error.c_str());
    const std::array<uint8_t, 4> source{10, 20, 30, 40};
    Check(DecodeBaseRGBA8(description, source, rgba, error), error.c_str());
    const std::array<std::array<uint8_t, 4>, 4> expected{{{10,20,30,40},{20,10,40,30},
                                                       {40,30,20,10},{30,40,10,20}}};
    Check(std::equal(rgba.begin(), rgba.end(), expected[endian].begin()), "Endian mode mismatch");
  }
  // Explicit independent tiled addresses exercise microtiles and a macro-tile
  // boundary. Fixture data is not generated with the decoder's address helper.
  Check(DescribeBaseRGBA8(Fetch(33, 33, 2, true), description, error), error.c_str());
  Check(description.source_bytes == 12292, "Exact tiled data bound mismatch");
  std::vector<uint8_t> tiled(description.source_bytes, 0);
  const uint32_t points[][3] = {{0,0,0},{1,0,4},{8,0,64},{0,1,16},{0,2,256},
                                {0,16,2048},{32,0,4096},{0,32,8192},{32,32,12288}};
  for (size_t i = 0; i < std::size(points); ++i) tiled[points[i][2]] = uint8_t(i + 1);
  Check(DecodeBaseRGBA8(description, tiled, rgba, error), error.c_str());
  for (size_t i = 0; i < std::size(points); ++i)
    Check(rgba[(points[i][1] * 33 + points[i][0]) * 4] == i + 1, "Tiled coordinate mismatch");

  // BGRA source selection plus constant alpha. Gamma follows the selected R.
  for (uint32_t endian = 0; endian < 4; ++endian) {
    Check(DescribeBaseRGBA8(Fetch(3, 2, 8, false, endian, 0x688, 0, 2), description, error), error.c_str());
    const uint32_t extents[] = {259, 260, 260, 260};
    if (description.row_pitch_bytes != 256 || description.source_bytes != extents[endian])
      std::cerr << "R8 mode " << endian << ": pitch=" << description.row_pitch_bytes
                << " extent=" << description.source_bytes << '\n';
    Check(description.row_pitch_bytes == 256 && description.source_bytes == extents[endian],
          "R8 endian mapping must include the actual last accessed byte");
    std::vector<uint8_t> source(description.source_bytes);
    for (size_t i = 0; i < source.size(); ++i) source[i] = uint8_t(i);
    const uint8_t expected[][3] = {{0,1,2},{1,0,3},{3,2,1},{2,3,0}};
    Check(DecodeBaseRGBA8(description, source, rgba, error), error.c_str());
    for (size_t pixel = 0; pixel < 6; ++pixel)
      for (size_t component = 0; component < 4; ++component)
        Check(rgba[pixel*4+component] == expected[endian][pixel%3], "R8 replication/endian mismatch");
    source.pop_back();
    Check(!DecodeBaseRGBA8(description, source, rgba, error), "Truncated R8 span accepted");
  }
  // A real video fetch uses RRR1, including a constant alpha independent of R.
  Check(DescribeBaseRGBA8(Fetch(2, 1, 2, false, 0, 0xa00, 0, 2), description, error), error.c_str());
  Check(DecodeBaseRGBA8(description, std::array<uint8_t, 2>{17, 29}, rgba, error), error.c_str());
  Check(rgba == std::vector<uint8_t>({17,17,17,255,29,29,29,255}), "Video texture swizzle mismatch");

  Check(DescribeBaseRGBA8(Fetch(1, 1, 2, false, 0, 2 | (1<<3) | (0<<6) | (5<<9), 3<<2),
                          description, error), error.c_str());
  Check(description.gamma_mask == 4, "Gamma metadata must follow output swizzle");
  const std::array<uint8_t, 4> source{10,20,30,40};
  Check(DecodeBaseRGBA8(description, source, rgba, error), error.c_str());
  Check(rgba == std::vector<uint8_t>({30,20,10,255}), "Swizzle/constant alpha mismatch");
  auto unsupported = Fetch(1, 1, 2, false);
  unsupported[7] = 18; // DXT1 must not be interpreted as raw RGBA8.
  Check(!DescribeBaseRGBA8(unsupported, description, error), "Unsupported format accepted");
  unsupported = Fetch(1, 1, 2, false, 0, 0x688, 1<<2);
  Check(!DescribeBaseRGBA8(unsupported, description, error), "Signed sample accepted as UNORM");
  std::cout << "Texture base decoding checks passed\n";
}
