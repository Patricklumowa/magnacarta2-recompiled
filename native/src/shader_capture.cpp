#include "generated/default/magnacarta2_init.h"
#include "shader_capture.h"
#include <rex/cvar.h>
#include <rex/hook.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <Windows.h>
#endif

REXCVAR_DEFINE_STRING(mc2_shader_capture_dir, "", "Development",
                     "Capture original shader containers locally (empty disables capture)");

namespace {
constexpr uint64_t kMaxSessionBytes = 256ull * 1024 * 1024;
bool Readable(const uint8_t* address, size_t length) {
#ifdef _WIN32
  uintptr_t cursor = reinterpret_cast<uintptr_t>(address);
  if (length > UINTPTR_MAX - cursor) return false;
  const uintptr_t end = cursor + length;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
      return false;
    const auto readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                          PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(region.Protect & readable)) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(region.BaseAddress);
    if (region.RegionSize > UINTPTR_MAX - start) return false;
    const uintptr_t next = start + region.RegionSize;
    if (next <= cursor) return false;
    cursor = next;
  }
  return true;
#else
  (void)address;
  (void)length;
  return false;  // Capture mapping validation currently supports Windows only.
#endif
}

struct CaptureSession {
  struct CachedShader {
    std::vector<uint8_t> content;
    std::string stage;
    std::string name;
    uint32_t id;
  };
  std::mutex mutex;
  std::filesystem::path directory;
  std::ofstream index;
  uint64_t bytes = 0;
  uint32_t files = 0;
  uint32_t events = 0;
  bool warned = false;
  bool disabled = false;
  std::unordered_map<uint64_t, std::vector<CachedShader>> shaders;
  std::unordered_map<uint64_t, uint32_t> last_content;

  void Warn(const char* reason) {
    if (!warned) {
      REXLOG_WARN("MC2 shader capture: {}", reason);
      warned = true;
    }
  }

  void Open(const std::string& root) {
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::filesystem::create_directories(root);
    for (uint32_t suffix = 0; suffix < 1000; ++suffix) {
      auto candidate = std::filesystem::path(root) /
          ("session-" + std::to_string(stamp) + "-" + std::to_string(suffix));
      if (std::filesystem::create_directory(candidate)) {
        directory = candidate;
        break;
      }
    }
    if (directory.empty()) throw std::runtime_error("Cannot create unique capture session");
    index.open(directory / "index.csv", std::ios::out);
    index.exceptions(std::ios::badbit | std::ios::failbit);
    constexpr char columns[] = "file,stage,guest_address,callsite,virtual_bytes,physical_bytes,fnv1a64,origin,resource_address,physical_address\n";
    index << columns;
    index.flush();
    bytes += sizeof(columns) - 1;
    REXLOG_INFO("MC2 shader capture: {}", directory.string());
  }
};

void Capture(PPCContext& ctx, uint8_t* base, const char* stage, bool binding = false) noexcept {
  // This callback observes input only. It never changes guest registers or RAM.
  static CaptureSession session;
  try {
    const auto root = REXCVAR_GET(mc2_shader_capture_dir);
    if (root.empty()) return;
    std::lock_guard lock(session.mutex);
    if (session.disabled) return;
    if (session.events >= 100000 || session.files >= 4096) {
      session.disabled = true;
      session.Warn("session count limit reached; further capture disabled");
      return;
    }
    uint32_t guest = ctx.r3.u32;
    uint32_t physical = 0;
    const uint32_t resource = binding ? ctx.r4.u32 : 0;
    if (binding) {
      if (!resource) return;
      const bool vertex = stage[0] == 'v';
      const uint32_t header_offset = vertex ? 872 : 40;
      const uint32_t physical_offset = vertex ? 32 : 24;
      if (!mc2::capture::GuestRange(resource, header_offset + 12) ||
          !Readable(rex::memory::GuestPtr(base, resource), physical_offset + 4)) {
        session.Warn("unreadable shader resource; skipping");
        return;
      }
      if ((mc2::capture::LoadBE32(rex::memory::GuestPtr(base, resource)) & 15) != (vertex ? 6u : 7u)) {
        session.Warn("unexpected shader resource type; skipping");
        return;
      }
      physical = mc2::capture::LoadBE32(rex::memory::GuestPtr(base, resource + physical_offset));
      guest = resource + header_offset;
    }
    if (!mc2::capture::GuestRange(guest, 12) ||
        !Readable(rex::memory::GuestPtr(base, guest), 12)) {
      session.Warn("unreadable container header; skipping");
      return;
    }
    uint8_t header[12];
    std::memcpy(header, rex::memory::GuestPtr(base, guest), sizeof(header));
    mc2::capture::ContainerSize size{};
    if (!mc2::capture::ReadContainerSize(header, sizeof(header), guest, size, !binding)) {
      session.Warn("invalid size or unreadable container; skipping");
      return;
    }
    if (!binding) physical = guest + size.virtual_bytes;
    if (!mc2::capture::GuestRange(physical, size.physical_bytes) ||
        !Readable(rex::memory::GuestPtr(base, guest), size.virtual_bytes) ||
        !Readable(rex::memory::GuestPtr(base, physical), size.physical_bytes)) {
      session.Warn(fmt::format("unreadable {} shader segments: resource={:#010x} virtual={:#010x}+{} "
                              "physical={:#010x}+{} translated_physical={}; skipping",
                              stage, resource, guest, size.virtual_bytes, physical,
                              size.physical_bytes, fmt::ptr(rex::memory::GuestPtr(base, physical))).c_str());
      return;
    }
    std::vector<uint8_t> bytes(size.total_bytes);
    std::memcpy(bytes.data(), rex::memory::GuestPtr(base, guest), size.virtual_bytes);
    std::memcpy(bytes.data() + size.virtual_bytes, rex::memory::GuestPtr(base, physical), size.physical_bytes);
    if (std::memcmp(bytes.data(), header, sizeof(header)) != 0) {
      session.Warn("container changed during capture; skipping");
      return;
    }
    const uint64_t hash = mc2::capture::ContentHash(bytes.data(), bytes.size());
    std::ostringstream hash_text;
    hash_text << std::hex << std::setw(16) << std::setfill('0') << hash;
    if (session.directory.empty()) session.Open(root);
    auto& bucket = session.shaders[hash];
    CaptureSession::CachedShader* shader = nullptr;
    for (auto& candidate : bucket) {
      if (candidate.stage == stage && candidate.content == bytes) {
        shader = &candidate;
        break;
      }
    }
    if (!shader) {
      const auto collision = bucket.size();
      auto name = hash_text.str() + (collision ? "-" + std::to_string(collision) : "") +
                  "." + stage + ".container";
      const auto path = session.directory / name;
      if (session.bytes + bytes.size() > kMaxSessionBytes) {
        session.disabled = true;
        session.Warn("256 MiB session limit reached; further capture disabled");
        return;
      }
      std::ofstream output(path, std::ios::binary);
      output.exceptions(std::ios::badbit | std::ios::failbit);
      output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      output.close();
      session.bytes += bytes.size();
      ++session.files;
      bucket.push_back({std::move(bytes), stage, std::move(name), session.files});
      shader = &bucket.back();
    }
    // Compare full bytes in memory; repeated binds perform no file reads or
    // index writes. This observes address/content changes, not full resource
    // generation lifetimes. Creation and reconstructed-binding provenance are
    // deliberately distinct even when they produce identical container bytes.
    const uint64_t key = (uint64_t(binding ? resource : guest) << 2) |
                         (stage[0] == 'v' ? 1 : 0) | (binding ? 2 : 0);
    const auto previous = session.last_content.find(key);
    if (previous != session.last_content.end() && previous->second == shader->id) return;
    std::ostringstream row;
    row << shader->name << ',' << stage << ',' << guest << ','
        << (ctx.lr >= 4 ? ctx.lr - 4 : 0) << ',' << size.virtual_bytes << ','
        << size.physical_bytes << ',' << hash_text.str() << ','
        << (binding ? "binding_reconstructed" : "creation_contiguous") << ','
        << resource << ',' << physical << '\n';
    const auto record = row.str();
    if (session.bytes + record.size() > kMaxSessionBytes) {
      session.disabled = true;
      session.Warn("256 MiB session limit reached; further capture disabled");
      return;
    }
    session.index << record;
    session.index.flush();
    session.bytes += record.size();
    ++session.events;
    session.last_content[key] = shader->id;
  } catch (const std::exception& error) {
    std::lock_guard lock(session.mutex);
    session.disabled = true;
    session.Warn(error.what());
  } catch (...) {
    std::lock_guard lock(session.mutex);
    session.disabled = true;
    session.Warn("unexpected capture error; further capture disabled");
  }
}
}  // namespace

REX_HOOK_RAW(sub_823724C8) {
  Capture(ctx, base, "vs");
  __imp__sub_823724C8(ctx, base);
}

REX_HOOK_RAW(sub_82372120) {
  Capture(ctx, base, "ps");
  __imp__sub_82372120(ctx, base);
}

REX_HOOK_RAW(sub_823725B8) {
  Capture(ctx, base, "vs", true);
  __imp__sub_823725B8(ctx, base);
}

REX_HOOK_RAW(sub_82372210) {
  Capture(ctx, base, "ps", true);
  __imp__sub_82372210(ctx, base);
}
