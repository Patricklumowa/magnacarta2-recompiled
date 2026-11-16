#include "generated/default/magnacarta2_init.h"
#include "native_video_check.h"
#include "native_quad.h"
#include "guest_graphics_read.h"
#include "draw_capture.h"
#include "texture_decode.h"
#include <rex/cvar.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>

REXCVAR_DEFINE_STRING(mc2_native_video_check_dir, "", "Development", "Bounded native movie check outputs; empty disables");
REXCVAR_DEFINE_STRING(mc2_native_video_shader_dir, "", "Development", "Locally verified movie containers, declaration and DXIL");
REXCVAR_DEFINE_UINT32(mc2_native_video_check_skip, 180, "Development", "Movie draws to skip before native comparison");
REXCVAR_DEFINE_UINT32(mc2_native_video_check_count, 1, "Development", "Native movie checks, clamped to 1..4");

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
void WriteFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream file(path, std::ios::binary);
  file.exceptions(std::ios::badbit | std::ios::failbit);
  file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  file.close();
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
struct State {
  std::mutex mutex;
  uint64_t ordinal = 0;
  uint32_t rendered = 0;
  bool failed = false;
  fs::path directory;
  std::vector<uint8_t> vs_reference, ps_reference, declaration_reference, vs_dxil, ps_dxil;

  void Open(const fs::path& parent, const fs::path& shaders) {
    vs_reference = ReadFile(shaders / "vertex-shader.container");
    ps_reference = ReadFile(shaders / "pixel-shader.container");
    declaration_reference = ReadFile(shaders / "vertex-declaration.bin");
    Require(declaration_reference.size() == 80, "Expected the verified two-element movie declaration");
    vs_dxil = ReadFile(shaders / "vertex.dxil");
    ps_dxil = ReadFile(shaders / "pixel.dxil");
    fs::create_directories(parent);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    directory = parent / ("session-" + std::to_string(stamp));
    Require(fs::create_directory(directory), "Native movie check output already exists");
  }
};
}  // namespace

void CheckLiveMovie(const PPCContext& ctx, uint8_t* base) noexcept {
  static State state;
  try {
    const auto output = REXCVAR_GET(mc2_native_video_check_dir);
    if (output.empty() || ctx.lr != 0x8276A6E0) return;
    std::lock_guard lock(state.mutex);
    const auto count = std::clamp(REXCVAR_GET(mc2_native_video_check_count), 1u, 4u);
    if (state.failed || state.rendered >= count || state.ordinal++ < REXCVAR_GET(mc2_native_video_check_skip)) return;
    Require(ctx.r4.u32 == 6 && ctx.r5.u32 == 4 && ctx.r7.u32 == 20, "Unrecognized movie draw ABI");
    if (state.directory.empty()) state.Open(output, REXCVAR_GET(mc2_native_video_shader_dir));
    std::vector<uint8_t> device;
    Require(ReadGuest(base, ctx.r3.u32, mc2::draw_capture::kDeviceBytes, device), "Unreadable movie device snapshot");
    const auto word = [&](size_t offset) { return LoadBE32(device.data() + offset); };
    Require((word(10548) & 7) == 0 && (word(10556) & 8) == 0 &&
            (word(10568) & 3) == 0 && word(10572) == 0x400,
            "Movie depth, alpha, culling or viewport state differs from the verified draw");
    Require(Shader(base, word(12704), true) == state.vs_reference &&
            Shader(base, word(12700), false) == state.ps_reference,
            "Live movie shader differs from the locally verified DXIL source");
    std::vector<uint8_t> declaration;
    Require(ReadGuest(base, word(11824), 80, declaration) &&
            std::equal(declaration.begin() + 52, declaration.end(), state.declaration_reference.begin() + 52),
            "Movie vertex declaration differs from the verified layout");

    QuadDraw draw;
    draw.width = 1280; draw.height = 720; draw.stride = 20;
    draw.vertex_shader = state.vs_dxil;
    draw.pixel_shader = state.ps_dxil;
    Require(ReadGuest(base, ctx.r6.u32, 80, draw.vertices), "Unreadable movie vertices");
    const auto guest_vertices = draw.vertices;
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
    const auto result = RenderQuad(draw);  // Do not enable the debug layer on a running game.
    const auto frame = state.directory / ("draw-" + std::to_string(state.ordinal - 1));
    Require(fs::create_directory(frame), "Native movie frame output already exists");
    WriteFile(frame / "output.rgba", result.pixels);
    WriteFile(frame / "device.bin", device);
    WriteFile(frame / "vertices.bin", guest_vertices);
    WriteFile(frame / "ps.bin", draw.pixel_constants);
    WriteFile(frame / "shared.bin", draw.shared_constants);
    for (size_t i = 0; i < draw.textures.size(); ++i)
      WriteFile(frame / ("texture" + std::to_string(i) + ".rgba"), draw.textures[i].bytes);
    // Written last: a directory without this marker is an incomplete check.
    const auto metadata = fmt::format(
        "{{\n  \"schema_version\":1,\n  \"kind\":\"live_native_movie_check\",\n"
        "  \"width\":1280,\"height\":720,\"movie_draw_ordinal\":{},\n"
        "  \"original_draw_remains_enabled\":true,\"full_pipeline_parity\":false,\n"
        "  \"vertex_dxil_fnv1a64\":\"{:016x}\",\"pixel_dxil_fnv1a64\":\"{:016x}\",\n"
        "  \"output_fnv1a64\":\"{:016x}\"\n}}\n", state.ordinal - 1,
        mc2::capture::ContentHash(draw.vertex_shader.data(), draw.vertex_shader.size()),
        mc2::capture::ContentHash(draw.pixel_shader.data(), draw.pixel_shader.size()),
        mc2::capture::ContentHash(result.pixels.data(), result.pixels.size()));
    WriteFile(frame / "metadata.json", std::vector<uint8_t>(metadata.begin(), metadata.end()));
    ++state.rendered;
    REXLOG_INFO("MC2 native movie check: {}x{}, draw {}, output {}; original draw remains enabled, full pipeline parity unverified",
                draw.width, draw.height, state.ordinal - 1, frame.string());
  } catch (const std::exception& error) {
    std::lock_guard lock(state.mutex);
    state.failed = true;
    REXLOG_WARN("MC2 native movie check disabled: {}", error.what());
  } catch (...) {
    std::lock_guard lock(state.mutex);
    state.failed = true;
    REXLOG_WARN("MC2 native movie check disabled after unexpected error");
  }
}
}  // namespace mc2::native
