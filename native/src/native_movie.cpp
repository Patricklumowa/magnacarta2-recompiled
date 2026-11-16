#include "generated/default/magnacarta2_init.h"
#include "native_movie.h"
#include "guest_graphics_read.h"
#include "draw_capture.h"
#include "texture_decode.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace mc2::native {
namespace {
namespace fs = std::filesystem;
using mc2::capture::LoadBE32;
using mc2::capture::ReadGuest;
void Require(bool value, const char* error) {
    if (!value) throw std::runtime_error(error);
}
std::vector<uint8_t> ReadFile(const fs::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  Require(bool(file), "Missing native movie shader input");
  const auto size = file.tellg();
  Require(size > 0 && uint64_t(size) <= mc2::capture::kMaxContainerBytes, "Invalid native movie shader input size");
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  Require(bool(file), "Short native movie shader read");
  return bytes;
}
std::vector<uint8_t> Shader(uint8_t* base, uint32_t resource, bool vertex) {
  const uint32_t offset = vertex ? 872 : 40;
  std::vector<uint8_t> descriptor, header, body, physical;
  mc2::capture::ContainerSize size{};
  Require(ReadGuest(base, resource, 36, descriptor) &&
      (LoadBE32(descriptor.data()) & 15) == (vertex ? 6u : 7u) &&
      mc2::capture::GuestRange(resource, offset + 12) &&
      ReadGuest(base, resource + offset, 12, header) &&
      mc2::capture::ReadContainerSize(header.data(), header.size(), resource + offset, size, false),
      "Invalid bound movie shader");
  Require(ReadGuest(base, resource + offset, size.virtual_bytes, body) &&
      ReadGuest(base, LoadBE32(descriptor.data() + (vertex ? 32 : 24)), size.physical_bytes, physical),
      "Unreadable bound movie shader");
  body.insert(body.end(), physical.begin(), physical.end());
  return body;
}
void ToLittleEndian(std::vector<uint8_t>& bytes) {
  Require(bytes.size() % 4 == 0, "Unaligned float upload");
  for (size_t i = 0; i < bytes.size(); i += 4)
    std::reverse(bytes.begin() + i, bytes.begin() + i + 4);
}
}  // namespace

MovieShaders LoadMovieShaders(const std::filesystem::path& directory) {
    MovieShaders result;
    result.vs_reference = ReadFile(directory / "vertex-shader.container");
    result.ps_reference = ReadFile(directory / "pixel-shader.container");
    result.declaration_reference = ReadFile(directory / "vertex-declaration.bin");
    Require(result.declaration_reference.size() == 80, "Expected the verified two-element movie declaration");
    result.vs_dxil = ReadFile(directory / "vertex.dxil");
    result.ps_dxil = ReadFile(directory / "pixel.dxil");
    return result;
}

MovieSnapshot ReadMovieDraw(const PPCContext& ctx, uint8_t* base, const MovieShaders& shaders) {
    Require(ctx.lr == 0x8276A6E0 && ctx.r4.u32 == 6 && ctx.r5.u32 == 4 && ctx.r7.u32 == 20,
            "Unrecognized native movie draw ABI");
    MovieSnapshot snapshot;
    auto& device = snapshot.device;
    Require(ReadGuest(base, ctx.r3.u32, mc2::draw_capture::kDeviceBytes, device), "Unreadable movie device snapshot");
    const auto word = [&](size_t offset) { return LoadBE32(device.data() + offset); };
    Require((word(10548) & 7) == 0 && (word(10556) & 8) == 0 &&
            (word(10568) & 3) == 0 && word(10572) == 0x400,
            "Movie depth, alpha, culling or viewport state differs from the verified draw");
    Require(Shader(base, word(12704), true) == shaders.vs_reference &&
            Shader(base, word(12700), false) == shaders.ps_reference,
            "Live movie shader differs from the locally verified DXIL source");
    std::vector<uint8_t> declaration;
    Require(ReadGuest(base, word(11824), 80, declaration) &&
            std::equal(declaration.begin() + 52, declaration.end(), shaders.declaration_reference.begin() + 52),
            "Movie vertex declaration differs from the verified layout");

    auto& draw = snapshot.draw;
    draw.width = 1280; draw.height = 720; draw.stride = 20;
    draw.vertex_shader = shaders.vs_dxil;
    draw.pixel_shader = shaders.ps_dxil;
    Require(ReadGuest(base, ctx.r6.u32, 80, draw.vertices), "Unreadable movie vertices");
    snapshot.guest_vertices = draw.vertices;
    ToLittleEndian(draw.vertices);
    for (size_t i = 0; i < draw.vertices.size(); i += 20) {
      float value[5];
      std::memcpy(value, draw.vertices.data() + i, sizeof(value));
      for (float f : value) Require(std::isfinite(f), "Non-finite movie vertex");
      value[0] = 2 * value[0] / draw.width - 1;
      value[1] = 1 - 2 * value[1] / draw.height;
      std::memcpy(draw.vertices.data() + i, value, sizeof(value));
    }
    draw.shared_constants.resize(512);
    const uint32_t indices[] = {0, 1, 2};
    std::memcpy(draw.shared_constants.data(), indices, sizeof(indices));
    const float half_pixel[] = {1.0f / draw.width, -1.0f / draw.height};
    std::memcpy(draw.shared_constants.data() + 264, half_pixel, sizeof(half_pixel));
    draw.pixel_constants.assign(device.begin() + 6016, device.begin() + 6016 + 4096);
    ToLittleEndian(draw.pixel_constants);
    for (uint32_t slot = 0; slot < 3; ++slot) {
      mc2::texture::BaseRGBA8 description;
      std::string error;
      const std::span<const uint8_t> fetch(device.data() + 1152 + slot * 24, 24);
      Require(mc2::texture::DescribeBaseRGBA8(fetch, description, error), error.c_str());
      const auto& w = description.fetch_words;
      Require(description.format == 2 && !description.tiled && description.endianness == 0 &&
              description.swizzle == 0xa00 && description.gamma_mask == 0 && description.exp_adjust == 0 &&
              description.mip_min_level == 0 && description.mip_max_level == 0 &&
              description.width == (slot ? 640u : 1280u) && description.height == (slot ? 360u : 720u) &&
              ((w[0] >> 10) & 7) == 2 && ((w[0] >> 13) & 7) == 2 &&
              ((w[3] >> 19) & 3) == 1 && ((w[3] >> 21) & 3) == 1,
              "Movie texture/sampler differs from the verified three-plane layout");
      uint32_t alias = 0;
      std::vector<uint8_t> bytes;
      Require(mc2::draw_capture::TexturePhysicalAlias(description.physical_address, description.source_bytes, alias) &&
              ReadGuest(base, alias, description.source_bytes, bytes), "Unreadable movie texture");
      QuadTexture texture{description.width, description.height};
      Require(mc2::texture::DecodeBaseRGBA8(description, bytes, texture.bytes, error), error.c_str());
      draw.textures.push_back(std::move(texture));
    }
    return snapshot;
}
}  // namespace mc2::native
