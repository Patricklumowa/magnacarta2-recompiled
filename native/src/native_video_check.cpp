#include "generated/default/magnacarta2_init.h"
#include "native_video_check.h"
#include "native_movie.h"
#include "shader_capture.h"
#include <rex/cvar.h>
#include <algorithm>
#include <chrono>
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

void Require(bool value, const char* error) {
  if (!value) throw std::runtime_error(error);
}
void WriteFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream file(path, std::ios::binary);
  file.exceptions(std::ios::badbit | std::ios::failbit);
  file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  file.close();
}
struct State {
  std::mutex mutex;
  uint64_t ordinal = 0;
  uint32_t rendered = 0;
  bool failed = false;
  fs::path directory;
  MovieShaders shaders;
  std::unique_ptr<QuadRenderer> renderer;

  void Open(const fs::path& parent, const fs::path& shaders) {
    this->shaders = LoadMovieShaders(shaders);
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
    auto snapshot = ReadMovieDraw(ctx, base, state.shaders);
    const auto& draw = snapshot.draw;
    const auto& device = snapshot.device;
    const auto& guest_vertices = snapshot.guest_vertices;
    // Do not enable the debug layer after the game's device has been created.
    if (!state.renderer) state.renderer = std::make_unique<QuadRenderer>(draw);
    const auto result = state.renderer->Render(draw);
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
        "  \"width\":1280,\"height\":720,\"movie_draw_ordinal\":{},\"completed_native_submission\":{},\n"
        "  \"original_draw_remains_enabled\":true,\"full_pipeline_parity\":false,\n"
        "  \"vertex_dxil_fnv1a64\":\"{:016x}\",\"pixel_dxil_fnv1a64\":\"{:016x}\",\n"
        "  \"output_fnv1a64\":\"{:016x}\"\n}}\n", state.ordinal - 1, result.completed_submission,
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
