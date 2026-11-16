#include "generated/default/magnacarta2_init.h"
#include "draw_capture.h"
#include "texture_decode.h"
#include "guest_graphics_read.h"
#ifdef MC2_ENABLE_NATIVE_BOOT
#include "native_boot.h"
#endif
#ifdef MC2_ENABLE_NATIVE_VIDEO_CHECK
#include "native_video_check.h"
#endif
#include <rex/cvar.h>
#include <rex/hook.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <Windows.h>
#endif

REXCVAR_DEFINE_STRING(mc2_draw_capture_dir, "", "Development", "Capture selected draw inputs locally; empty disables");
REXCVAR_DEFINE_STRING(mc2_draw_capture_hook, "all", "Development", "Draw capture filter: all, up, indexed, nonindexed");
REXCVAR_DEFINE_UINT32(mc2_draw_capture_count, 1, "Development", "Draw snapshots to capture, clamped to 1..32");
REXCVAR_DEFINE_UINT32(mc2_draw_capture_skip, 0, "Development", "Matching draw calls to skip before capture");

namespace {
using mc2::capture::ReadGuest;

struct Artifact {
  std::string name;
  std::string status;
  std::string origin;
  uint32_t address;
  uint32_t physical = 0;
  uint64_t size = 0;
  uint64_t hash = 0;
};

struct Session {
  std::mutex mutex;
  std::filesystem::path root;
  uint64_t matching_calls = 0;
  uint64_t bytes = 0;
  uint32_t captured = 0;
  bool disabled = false;

  void Open(const std::string& directory) {
    std::filesystem::create_directories(directory);
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (unsigned suffix = 0; suffix < 1000; ++suffix) {
      auto candidate = std::filesystem::path(directory) /
          fmt::format("session-{}-{}", stamp, suffix);
      if (std::filesystem::create_directory(candidate)) {
        root = candidate;
        REXLOG_INFO("MC2 draw capture: {}", root.string());
        return;
      }
    }
    throw std::runtime_error("Unable to create draw capture session");
  }

  void Write(const std::filesystem::path& path, const uint8_t* data, size_t length) {
    if (length > mc2::draw_capture::kSessionLimit - bytes)
      throw std::runtime_error("draw capture session byte limit reached");
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream.write(reinterpret_cast<const char*>(data), length);
    stream.close();
    bytes += length;
  }
};

struct Snapshot {
  Session& session;
  uint8_t* base;
  std::filesystem::path directory;
  std::vector<Artifact> artifacts;
  std::vector<std::string> missing;
  std::array<mc2::texture::BaseRGBA8, 3> textures;
  std::array<bool, 3> texture_described{};
  std::array<bool, 3> texture_decoded{};

  void Save(const std::string& name, uint32_t address, const std::vector<uint8_t>& content,
            const char* origin = "guest_bytes", uint32_t physical = 0) {
    session.Write(directory / name, content.data(), content.size());
    artifacts.push_back({name, "captured", origin, address, physical,
                         content.size(), mc2::capture::ContentHash(content.data(), content.size())});
  }

  bool Copy(const std::string& name, uint32_t address, uint64_t length) {
    std::vector<uint8_t> content;
    if (!ReadGuest(base, address, length, content)) {
      artifacts.push_back({name, address ? "unreadable_or_invalid_extent" : "unbound", "guest_bytes", address});
      return false;
    }
    Save(name, address, content);
    return true;
  }

  void Shader(const char* name, uint32_t resource, bool vertex) {
    const uint32_t offset = vertex ? 872 : 40;
    std::vector<uint8_t> descriptor, header, virtual_bytes, physical_bytes;
    mc2::capture::ContainerSize size{};
    if (!ReadGuest(base, resource, 36, descriptor) ||
        (mc2::capture::LoadBE32(descriptor.data()) & 15) != (vertex ? 6u : 7u) ||
        !mc2::capture::GuestRange(resource, offset + 12) ||
        !ReadGuest(base, resource + offset, 12, header) ||
        !mc2::capture::ReadContainerSize(header.data(), header.size(), resource + offset, size, false)) {
      artifacts.push_back({name, "unbound_or_invalid_resource", "binding_reconstructed", resource});
      missing.push_back(name);
      return;
    }
    const auto physical = mc2::capture::LoadBE32(descriptor.data() + (vertex ? 32 : 24));
    if (!ReadGuest(base, resource + offset, size.virtual_bytes, virtual_bytes) ||
        !ReadGuest(base, physical, size.physical_bytes, physical_bytes)) {
      artifacts.push_back({name, "unreadable_segments", "binding_reconstructed", resource, physical});
      missing.push_back(name);
      return;
    }
    virtual_bytes.insert(virtual_bytes.end(), physical_bytes.begin(), physical_bytes.end());
    Save(name, resource, virtual_bytes, "binding_reconstructed", physical);
  }

  bool Texture(uint32_t slot, const std::vector<uint8_t>& fetch) {
    std::string error;
    auto& texture = textures.at(slot);
    const auto raw_name = fmt::format("textures/slot-{:02}.base.bin", slot);
    const auto rgba_name = fmt::format("textures/slot-{:02}.rgba8.bin", slot);
    texture_described[slot] = mc2::texture::DescribeBaseRGBA8(fetch, texture, error);
    uint32_t alias = 0;
    if (!texture_described[slot] ||
        !mc2::draw_capture::TexturePhysicalAlias(texture.physical_address, texture.source_bytes, alias)) {
      artifacts.push_back({raw_name, "unsupported_or_invalid_fetch",
                           "guest_physical_bytes", 0});
      return false;
    }
    std::vector<uint8_t> payload;
    if (!ReadGuest(base, alias, texture.source_bytes, payload)) {
      artifacts.push_back({raw_name, "unreadable_physical_span",
                           "guest_physical_bytes", alias, texture.physical_address});
      return false;
    }
    Save(raw_name, alias, payload, "guest_physical_bytes", texture.physical_address);
    std::vector<uint8_t> rgba;
    if (uint64_t(texture.width) * texture.height * 4 <= mc2::draw_capture::kArtifactLimit &&
        mc2::texture::DecodeBaseRGBA8(texture, payload, rgba, error)) {
      Save(rgba_name, alias, rgba, "decoded_rgba8_swizzled_gamma_unapplied",
           texture.physical_address);
      texture_decoded[slot] = true;
    } else {
      artifacts.push_back({rgba_name, "unsupported_or_over_limit",
                           "decoded_rgba8_swizzled_gamma_unapplied", alias, texture.physical_address});
    }
    return true;
  }
};

void Capture(PPCContext& ctx, uint8_t* base, const char* hook, uint32_t guest_hook) noexcept {
  static Session session;
  try {
    const auto directory = REXCVAR_GET(mc2_draw_capture_dir);
    if (directory.empty()) return;
    const auto filter = REXCVAR_GET(mc2_draw_capture_hook);
    if (filter != "all" && filter != hook) return;
    std::lock_guard lock(session.mutex);
    if (session.disabled || session.captured >= 32) return;
    const auto ordinal = session.matching_calls++;
    if (!mc2::draw_capture::Selected(ordinal, REXCVAR_GET(mc2_draw_capture_skip),
                                    REXCVAR_GET(mc2_draw_capture_count))) return;
    ++session.captured;
    if (session.root.empty()) session.Open(directory);
    Snapshot snapshot{session, base, session.root / fmt::format("draw-{:08}", ordinal)};
    const auto device_address = ctx.r3.u32;
    std::vector<uint8_t> device;
    if (ReadGuest(base, device_address, mc2::draw_capture::kDeviceBytes, device)) {
      snapshot.Save("device.bin", device_address, device);
      const auto pointer = [&](size_t offset) { return mc2::capture::LoadBE32(device.data() + offset); };
      if (pointer(10896)) snapshot.Copy("gpu-progress.bin", pointer(10896), 96);
      if (pointer(10900)) snapshot.Copy("gpu-auxiliary.bin", pointer(10900), 32);
      snapshot.Shader("vertex-shader.container", pointer(12704), true);
      snapshot.Shader("pixel-shader.container", pointer(12700), false);
      const auto declaration = pointer(11824);
      std::vector<uint8_t> declaration_header;
      uint32_t declaration_bytes = 0;
      if (ReadGuest(base, declaration, 28, declaration_header) &&
          mc2::draw_capture::DeclarationBytes(declaration_header.data(), declaration_header.size(), declaration_bytes)) {
        if (!snapshot.Copy("vertex-declaration.bin", declaration, declaration_bytes))
          snapshot.missing.push_back("vertex_declaration");
      } else {
        snapshot.artifacts.push_back({"vertex-declaration.bin", "unbound_or_invalid_header", "guest_bytes", declaration});
        snapshot.missing.push_back("vertex_declaration");
      }
      for (uint32_t slot = 0; slot < 16; ++slot) {
        const auto address = pointer(12468 + slot * 4);
        if (address) snapshot.Copy(fmt::format("vertices/slot-{:02}.descriptor.bin", slot), address, 32);
      }
      if (pointer(12444)) snapshot.Copy("index-buffer.descriptor.bin", pointer(12444), 32);
      for (uint32_t slot = 0; slot < 26; ++slot) {
        const auto address = pointer(12552 + slot * 4);
        // Preserve every fetch, including inline descriptors without a resource.
        std::vector<uint8_t> fetch(device.begin() + 1152 + slot * 24,
                                   device.begin() + 1152 + (slot + 1) * 24);
        snapshot.Save(fmt::format("textures/slot-{:02}.fetch.bin", slot),
                      device_address + 1152 + slot * 24, fetch);
        const bool payload_captured = slot < snapshot.textures.size() && snapshot.Texture(slot, fetch);
        if (address) {
          snapshot.Copy(fmt::format("textures/slot-{:02}.descriptor.bin", slot), address, 52);
        }
        if (!payload_captured && (address || (mc2::capture::LoadBE32(fetch.data()) & 3) == 2))
          snapshot.missing.push_back(fmt::format("texture_payload_slot_{}", slot));
      }
      for (uint32_t slot = 0; slot < 4; ++slot) {
        const auto address = pointer(12448 + slot * 4);
        if (address) snapshot.Copy(fmt::format("targets/color-{}.descriptor.bin", slot), address, 48);
      }
      if (pointer(12464)) snapshot.Copy("targets/depth.descriptor.bin", pointer(12464), 48);
    } else {
      snapshot.artifacts.push_back({"device.bin", "unreadable_or_invalid_extent", "guest_bytes", device_address});
      snapshot.missing.push_back("device_state");
    }
    if (std::string_view(hook) == "up") {
      uint32_t vertex_bytes = 0;
      if (!mc2::draw_capture::UpVertexBytes(ctx.r6.u32, ctx.r5.u32, ctx.r7.u32, vertex_bytes) ||
          !snapshot.Copy("up-vertices.bin", ctx.r6.u32, vertex_bytes)) snapshot.missing.push_back("up_vertices");
    } else {
      snapshot.missing.push_back("vertex_buffer_payloads");
      if (std::string_view(hook) == "indexed") snapshot.missing.push_back("index_buffer_payload");
    }
    snapshot.missing.push_back("attachment_contents_and_prior_draw_dependencies");
    snapshot.missing.push_back("native_pipeline_state_validation");
    std::ostringstream json;
    json << "{\n  \"schema_version\": 2,\n  \"hook\": \"" << hook
         << "\",\n  \"guest_hook\": " << guest_hook << ",\n  \"matching_draw_ordinal\": " << ordinal
         << ",\n  \"device_address\": " << device_address << ",\n  \"lr\": " << ctx.lr
         << ",\n  \"args\": {\"r3\":" << ctx.r3.u64 << ",\"r4\":" << ctx.r4.u64
         << ",\"r5\":" << ctx.r5.u64 << ",\"r6\":" << ctx.r6.u64
         << ",\"r7\":" << ctx.r7.u64 << ",\"r8\":" << ctx.r8.u64
         << ",\"r9\":" << ctx.r9.u64 << ",\"r10\":" << ctx.r10.u64
         << "},\n  \"replayable\": false,\n  \"textures\": [";
    for (size_t slot = 0; slot < snapshot.textures.size(); ++slot) {
      if (slot) json << ',';
      if (snapshot.texture_described[slot]) {
        const auto& texture = snapshot.textures[slot];
        json << "{\"slot\":" << slot << ",\"format\":" << texture.format << ",\"width\":" << texture.width << ",\"height\":" << texture.height
             << ",\"physical_address\":" << texture.physical_address << ",\"source_bytes\":" << texture.source_bytes
             << ",\"row_pitch_bytes\":" << texture.row_pitch_bytes << ",\"tiled\":" << (texture.tiled ? "true" : "false")
             << ",\"offset_x\":" << texture.offset_x << ",\"offset_y\":" << texture.offset_y
             << ",\"endianness\":" << texture.endianness << ",\"swizzle\":" << texture.swizzle
             << ",\"gamma_mask\":" << texture.gamma_mask << ",\"exp_adjust\":" << texture.exp_adjust
             << ",\"mip_min_level\":" << texture.mip_min_level << ",\"mip_max_level\":" << texture.mip_max_level
             << ",\"rgba8_decoded\":" << (snapshot.texture_decoded[slot] ? "true" : "false") << ",\"fetch_words\":[";
        for (size_t i = 0; i < texture.fetch_words.size(); ++i) json << (i ? "," : "") << texture.fetch_words[i];
        json << "]}";
      } else {
        json << "null";
      }
    }
    json << "],\n  \"artifacts\": [\n";
    for (size_t i = 0; i < snapshot.artifacts.size(); ++i) {
      const auto& artifact = snapshot.artifacts[i];
      json << "    {\"name\":\"" << artifact.name << "\",\"status\":\"" << artifact.status
           << "\",\"origin\":\"" << artifact.origin << "\",\"address\":" << artifact.address
           << ",\"physical_address\":" << artifact.physical << ",\"size\":" << artifact.size
           << ",\"fnv1a64\":\"" << fmt::format("{:016x}", artifact.hash) << "\"}"
           << (i + 1 == snapshot.artifacts.size() ? "\n" : ",\n");
    }
    json << "  ],\n  \"missing_for_replay\": [";
    for (size_t i = 0; i < snapshot.missing.size(); ++i)
      json << (i ? "," : "") << '"' << snapshot.missing[i] << '"';
    json << "]\n}\n";
    const auto metadata = json.str();
    session.Write(snapshot.directory / "metadata.json",
                  reinterpret_cast<const uint8_t*>(metadata.data()), metadata.size());
    REXLOG_INFO("MC2 draw capture: {} ({})", snapshot.directory.string(), hook);
  } catch (const std::exception& error) {
    std::lock_guard lock(session.mutex);
    session.disabled = true;
    REXLOG_WARN("MC2 draw capture disabled after error: {}", error.what());
  } catch (...) {
    std::lock_guard lock(session.mutex);
    session.disabled = true;
    REXLOG_WARN("MC2 draw capture disabled after unexpected error");
  }
}
}  // namespace

REX_HOOK_RAW(sub_828652A0) {
#ifdef MC2_ENABLE_NATIVE_BOOT
  if (mc2::native::NativeBootActive()) mc2::native::UnsupportedNativeOperation("nonindexed draw", ctx);
#endif
  Capture(ctx, base, "nonindexed", 0x828652A0);
  __imp__sub_828652A0(ctx, base);
}
REX_HOOK_RAW(sub_82865690) {
#ifdef MC2_ENABLE_NATIVE_BOOT
  if (mc2::native::NativeBootActive()) mc2::native::UnsupportedNativeOperation("indexed draw", ctx);
#endif
  Capture(ctx, base, "indexed", 0x82865690);
  __imp__sub_82865690(ctx, base);
}
REX_HOOK_RAW(sub_82864D40) {
#ifdef MC2_ENABLE_NATIVE_BOOT
  if (mc2::native::NativeBootActive()) { mc2::native::QueueNativeMovie(ctx, base); return; }
#endif
#ifdef MC2_ENABLE_NATIVE_VIDEO_CHECK
  mc2::native::CheckLiveMovie(ctx, base);
#endif
  Capture(ctx, base, "up", 0x82864D40);
  __imp__sub_82864D40(ctx, base);
}
