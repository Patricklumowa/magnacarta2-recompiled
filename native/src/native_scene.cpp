#include "generated/default/magnacarta2_init.h"
#include "native_scene.h"
#include "native_shader_input.h"
#include "native_sampled_texture.h"
#include "guest_graphics_read.h"
#include "draw_capture.h"
#include <bcrypt.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <fstream>
#include <stdexcept>

namespace mc2::native {
namespace {
using mc2::capture::ReadGuest;
using mc2::capture::LoadBE32;
void Require(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
std::string ShaderHashPrefix(const std::vector<uint8_t>& bytes) {
    std::array<uint8_t, 32> digest{};
    Require(bytes.size() <= UINT32_MAX && BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                const_cast<uint8_t*>(bytes.data()), static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size()))),
            "Could not hash the captured scene shader");
    return std::format("{:02x}{:02x}{:02x}{:02x}", digest[0], digest[1], digest[2], digest[3]);
}
bool HasShaderHashName(std::string_view name) {
    constexpr std::string_view hex = "0123456789abcdefABCDEF";
    return name.size() == 17 && name[8] == '-' && name.substr(0, 8).find_first_not_of(hex) == name.npos &&
           name.substr(9).find_first_not_of(hex) == name.npos;
}
constexpr uint32_t kOcclusionBoxVertices = 8, kOcclusionBoxIndices = 36;
struct DeviceState {
    std::vector<uint8_t> bytes;
    DeviceState(uint8_t* base, uint32_t address) {
        Require(ReadGuest(base,address,24064,bytes), "Unreadable native scene device");
    }
    uint32_t Word(uint32_t offset) const { return LoadBE32(bytes.data()+offset); }
    float Float(uint32_t offset) const { return std::bit_cast<float>(Word(offset)); }
    void NoColorAttachments() const {
        for (uint32_t i = 0; i < 4; ++i)
            Require(Word(12448+i*4) == 0, "This native depth pass requires no color attachments");
    }
    SceneSurface DepthSurface(uint8_t* base, uint32_t height, uint32_t samples = 2) const {
        SceneSurface result;
        result.address = Word(12464);
        std::vector<uint8_t> descriptor;
        Require(ReadGuest(base,result.address,48,descriptor) && (LoadBE32(descriptor.data()) & 15) == 4,
                "Invalid native depth surface");
        const auto surface_info = LoadBE32(descriptor.data()+24), dimensions = LoadBE32(descriptor.data()+36);
        result.width = ((dimensions>>18)&16383)+1; result.height = ((dimensions>>3)&32767)+1;
        result.samples = 1u << ((surface_info>>16)&3);
        Require(result.width == 1280 && result.height == height && result.samples == samples &&
                LoadBE32(descriptor.data()+32) == 17 && (surface_info & 16383) == 1280 &&
                Word(10376) == LoadBE32(descriptor.data()+28),
                "Depth surface differs from the verified float depth/stencil layout");
        result.description.assign(descriptor.begin()+24,descriptor.end());
        return result;
    }
    void FullViewport(const SceneSurface& surface, bool allow_scissor = false) const {
        Require(Float(12656) == 0 && Float(12660) == 0 && Float(12664) == surface.width &&
                Float(12668) == surface.height && Word(11864) <= uint32_t(allow_scissor),
                "Scene viewport/scissor differs from the verified pass");
    }
};

void RequireFullClearRect(const PPCContext& ctx, uint8_t* base, const SceneSurface& surface) {
    std::vector<uint8_t> rectangle;
    Require(ReadGuest(base, ctx.r5.u32, 16, rectangle) && LoadBE32(rectangle.data()) == 0 &&
            LoadBE32(rectangle.data() + 4) == 0 && LoadBE32(rectangle.data() + 8) == surface.width &&
            LoadBE32(rectangle.data() + 12) == surface.height, "Only the verified full surface clear rectangle is supported");
}

uint32_t PhysicalFetchAddress(uint32_t address) {
    // The guest's 4 KiB physical heap starts one page into physical memory.
    return (address & 0x1fffffff) + (address >= 0xe0000000 ? 0x1000 : 0);
}

void RequireSceneTexture(const DeviceState& state, const SceneResolveTexture& source, uint32_t clamp_bits,
                         uint32_t slot = 0, bool linear = false) {
    Require(source.address && state.Word(12552 + slot * 4) == source.address, "Fullscreen pass did not bind the scene resolve");
    for (uint32_t i = 0; i < source.fetch_words.size(); ++i) {
        uint32_t expected = source.fetch_words[i];
        if (i == 0) expected |= clamp_bits;
        if (i == 1) expected = PhysicalFetchAddress(expected);
        if (linear && i == 3) expected |= 0x280000;
        if (linear && i == 4) expected |= 3;
        const auto value = state.Word(1152 + slot * 24 + i * 4);
        if (value != expected) {
            throw std::runtime_error(std::format("Fullscreen scene texture {} fetch word {} differs ({:#x}, expected {:#x})",
                                                 slot, i, value, expected));
        }
    }
}

void BindSceneTexture(DepthDraw& draw, uint32_t slot, uint32_t mode = PackTextureFetchMode(0x688, 0, 0), bool cube = false) {
    auto* shared = draw.shared_constants.data();
    std::memcpy(shared + (cube ? 128 : 0) + slot * 4, &slot, sizeof(slot));
    std::memcpy(shared + 192 + slot * 4, &slot, sizeof(slot));
    std::memcpy(shared + 288 + slot * 4, &mode, sizeof(mode));
}

void ReadFullscreenConstants(const DeviceState& state, const SceneSurface& surface, DepthDraw& draw,
                             std::vector<uint8_t>& pixel_constants, bool reverse_depth) {
    draw.vertex_constants = NativeVertexBytes(std::span(state.bytes).subspan(1920,4096), 2);
    pixel_constants = NativeVertexBytes(std::span(state.bytes).subspan(6016,4096), 2);
    draw.shared_constants.resize(512);
    const float half_pixel[] = {1.0f / surface.width, -1.0f / surface.height};
    std::memcpy(draw.shared_constants.data() + 264, half_pixel, sizeof(half_pixel));
    BindSceneTexture(draw, 0);
    draw.viewport = plume::RenderViewport(0,0,surface.width,surface.height,reverse_depth ? 1.f : 0.f,reverse_depth ? 0.f : 1.f);
    draw.scissor = plume::RenderRect(0,0,surface.width,surface.height);
}

void RequireFullscreenDrawState(const DeviceState& state, const SceneSurface& surface, bool reverse_depth,
                                uint32_t color_mask = 15, uint32_t raster_mode = 0x18000,
                                uint32_t blend_control = 0x10001, uint32_t depth_control = 0x00700770) {
    const float half_width = surface.width * 0.5f, half_height = surface.height * 0.5f;
    Require(state.Word(11824) == 0 && state.Word(10452) == 0 && state.Word(10460) == color_mask &&
            state.Word(10548) == depth_control && state.Word(10552) == blend_control && (state.Word(10556) & 0x1f) == 7 &&
            state.Word(10564) == 0x80000 && state.Word(10568) == raster_mode && state.Word(10572) == 0x43f &&
            state.Float(12672) == (reverse_depth ? 1 : 0) && state.Float(12676) == (reverse_depth ? 0 : 1) &&
            state.Float(10504) == half_width && state.Float(10508) == half_width && state.Float(10512) == -half_height &&
            state.Float(10516) == half_height && state.Float(10520) == (reverse_depth ? -1 : 1) &&
            state.Float(10524) == (reverse_depth ? 1 : 0), "Fullscreen blend, mask, depth or viewport state changed");
}

void ReadBlockSceneTexture(const DeviceState& state, uint32_t slot, uint32_t expected_format, DepthDraw& draw,
                           mc2::texture::BlockTexture& texture, plume::RenderSamplerDesc& sampler, bool clamp_uv = false) {
    std::string error;
    if (!mc2::texture::DescribeBlockTexture(std::span(state.bytes).subspan(1152 + slot * 24, 24), texture, error))
        throw std::runtime_error(error);
    const auto& words = texture.fetch_words;
    constexpr uint32_t mip_max_mask = 15u << 6;
    // Xenos clamp-to-edge is 2 in each of the U and V fields.
    const uint32_t addressing = clamp_uv ? 0x00004800 : 0;
    if (!state.Word(12552 + 4 * slot) || (words[0] & 0x0007fc00) != addressing || words[3] != 0x06280d10 ||
        (words[4] & ~mip_max_mask) != 0x803 || (words[5] & 0x1ff) != 0 ||
        (expected_format && texture.format != expected_format) || texture.mip_min_level != 0) {
        throw std::runtime_error(std::format(
            "Native scene texture {} format {}, levels {}-{}, sampler {:#x}/{:#x}/{:#x}/{:#x} differs",
            slot, texture.format, texture.mip_min_level, texture.mip_max_level,
            words[0], words[3], words[4], words[5]));
    }
    sampler = OverlaySampler(texture.mip_max_level, clamp_uv);
    BindSceneTexture(draw, slot, PackTextureFetchMode(texture.swizzle, texture.gamma_mask, texture.exp_adjust), texture.faces == 6);
}

SceneVertexShader LoadVertexShader(const std::filesystem::path& directory) {
    auto reference = ReadNativeFile(directory / "vertex-shader.container");
    auto inputs = ReadVertexInputs(reference);
    return {std::move(reference), ReadNativeFile(directory / "vertex.dxil"), std::move(inputs)};
}

SceneSurface ReadTiledColorSurface(const DeviceState& state, uint8_t* base) {
    Require(state.Word(12448)!=0 && state.Word(12452)==0 && state.Word(12456)==0 && state.Word(12460)==0,
            "Native scene requires the verified single color attachment");
    SceneSurface surface;
    surface.address=state.Word(12448);
    std::vector<uint8_t> descriptor;
    Require(ReadGuest(base,surface.address,48,descriptor) && (LoadBE32(descriptor.data())&15)==4,
            "Invalid native color surface");
    const auto info=LoadBE32(descriptor.data()+24), color_info=LoadBE32(descriptor.data()+28);
    const auto dimensions=LoadBE32(descriptor.data()+36);
    Require(((dimensions>>18)&16383)+1==1280 && ((dimensions>>3)&32767)+1==512 &&
            (info&16383)==1280 && ((info>>16)&3)==1 && ((color_info>>16)&1023)==3 &&
            (LoadBE32(descriptor.data()+40)&63)==63 && state.Word(10372)==color_info,
            "Scene color surface differs from the verified two-sample HDR format");
    Require((state.bytes[10940]&0x20)!=0 && state.Word(12756)==2 && state.Word(13196)==2 &&
            state.Word(13188)==1280 && state.Word(13192)==720 &&
            state.Word(12732)==surface.address && state.Word(12736)==surface.address &&
            state.Word(12740)==0 && state.Word(12744)==0 && state.Word(12748)==0 &&
            state.Word(12464)!=0 && state.Word(12752)==state.Word(12464),
            "Native scene has an unsupported tiling configuration");
    constexpr uint32_t tiles[]={0,0,1280,512,0,512,1280,720};
    for (uint32_t i=0;i<std::size(tiles);++i)
        Require(state.Word(12760+i*4)==tiles[i],"Scene tile rectangles changed");
    surface.width=1280; surface.height=720; surface.samples=2;
    surface.description.assign(descriptor.begin()+24,descriptor.end());
    state.FullViewport(surface);
    return surface;
}

SceneSurface ReadFullscreenColorSurface(const DeviceState& state, uint8_t* base, uint32_t format_word = 0x302d0,
                                        uint32_t blend_format_word = 0) {
    SceneSurface surface{state.Word(12448),1280,720,1,{}};
    std::vector<uint8_t> descriptor;
    Require(state.Word(12452) == 0 && state.Word(12456) == 0 && state.Word(12460) == 0 &&
            ReadGuest(base, surface.address, 48, descriptor), "Missing fullscreen color attachment");
    Require((LoadBE32(descriptor.data()) & 15) == 4 && LoadBE32(descriptor.data() + 24) == 0x14000500 &&
            LoadBE32(descriptor.data() + 28) == format_word && LoadBE32(descriptor.data() + 36) == 0x13fc1678 &&
            (state.Word(10372) == format_word || (blend_format_word && state.Word(10372) == blend_format_word)),
            "Fullscreen color attachment layout changed");
    surface.description.assign(descriptor.begin()+24,descriptor.end());
    state.FullViewport(surface, blend_format_word != 0);
    Require((state.bytes[10940] & 0x30) == 0 && state.Word(12724) == 0 && state.Word(12716) == UINT32_MAX,
            "Fullscreen color pass needs an untiled surface");
    return surface;
}

SceneSurface ReadBloomColorSurface(const DeviceState& state, uint8_t* base) {
    SceneSurface surface{state.Word(12448),322,182,1,{}};
    state.FullViewport(surface);
    std::vector<uint8_t> descriptor;
    Require((state.bytes[10940] & 0x30) == 0 && state.Word(12724) == 0 && state.Word(12716) == UINT32_MAX &&
            state.Word(12452) == 0 && state.Word(12456) == 0 && state.Word(12460) == 0 && state.Word(12464) == 0 &&
            ReadGuest(base, surface.address, 48, descriptor) && (LoadBE32(descriptor.data()) & 15) == 4 &&
            LoadBE32(descriptor.data() + 24) == 0x05800190 && LoadBE32(descriptor.data() + 28) == 0x505a0 &&
            LoadBE32(descriptor.data() + 36) == 0x050405a8 && state.Word(10372) == 0x505a0, "Unsupported bloom target layout");
    surface.description.assign(descriptor.begin()+24,descriptor.end());
    return surface;
}

void ReadDepthTransform(DepthDraw& draw, const DeviceState& state, const SceneSurface& surface) {
    draw.vertex_constants=NativeVertexBytes(std::span(state.bytes).subspan(1920,4096),2);
    draw.shared_constants.resize(512);
    const float half_pixel[] = {1.0f/surface.width,-1.0f/surface.height};
    std::memcpy(draw.shared_constants.data()+264,half_pixel,sizeof(half_pixel));
    const float x_scale = state.Float(10504), y_scale = state.Float(10512);
    const float x = state.Float(10508) - x_scale, y = state.Float(10516) + y_scale;
    Require(x_scale == float(surface.width) / 2 && y_scale == -float(surface.height) / 2 &&
            std::abs(x) <= 0.5f && std::abs(y) <= 0.5f && state.Float(10520) == -1 && state.Float(10524) == 1,
            "Native scene cached viewport transform differs");
    draw.viewport=plume::RenderViewport(x,y,float(surface.width),float(surface.height),1,0);
    draw.scissor=plume::RenderRect(0,0,surface.width,surface.height);
}

std::vector<uint8_t> ReadVertexStream(const DeviceState& state, uint8_t* base, uint32_t slot, const VertexInput& input) {
    Require(input.fetch_index < 96 && uint32_t(state.bytes[12536 + slot]) * 4 == input.stride,
            "Native scene vertex stride or fetch index changed");
    std::vector<uint8_t> descriptor, raw;
    uint32_t address = 0, size = 0;
    Require(ReadGuest(base, state.Word(12468 + slot * 4), 32, descriptor) &&
            mc2::draw_capture::BufferBytes(descriptor.data(), descriptor.size(), true, address, size) &&
            ReadGuest(base, address, size, raw), "Invalid scene vertex stream");
    const uint32_t fetch = 1152 + input.fetch_index * 8;
    const uint32_t info = LoadBE32(descriptor.data() + 28);
    Require(state.Word(fetch) == PhysicalFetchAddress(LoadBE32(descriptor.data() + 24)) && state.Word(fetch + 4) == info,
            "Bound vertex fetch differs from its resource descriptor");
    return NativeVertexBytes(raw, info & 3);
}

DepthDraw ReadGeometry(const PPCContext& ctx, uint8_t* base, const DeviceState& state,
                       const SceneSurface& surface, const SceneVertexShader& shader, uint32_t depth_control = 0x00700766) {
    Require(ctx.r4.u32 == 4 && state.Word(11824) == 0,
            "Native scene requires the verified triangle-list vertex inputs");
    Require(state.Word(10548) == depth_control && state.Word(10572) == 0x43f &&
            state.Word(10564) == 0x80000 && state.Word(10452) == 0 && state.Float(12672) == 1 && state.Float(12676) == 0,
            "Depth/stencil, clipping, vertex base or viewport state differs from the verified pass");
    Require(!shader.inputs.empty(), "Native scene vertex layout is missing");
    DepthDraw draw;
    std::array<bool, 96> bound{};
    uint32_t stream = 0;
    for (const auto& input : shader.inputs) {
        Require(input.fetch_index < bound.size(), "Native scene vertex fetch index is out of range");
        if (bound[input.fetch_index]) continue;
        Require(stream < kNativeVertexStreamLimit, "Too many native scene vertex streams");
        auto bytes = ReadVertexStream(state, base, stream, input);
        if (stream == 0) draw.vertices = std::move(bytes);
        else draw.additional_vertices.push_back(std::move(bytes));
        bound[input.fetch_index] = true;
        ++stream;
    }
    std::vector<uint8_t> index_descriptor, raw;
    uint32_t address=0,size=0;
    Require(ReadGuest(base,state.Word(12444),32,index_descriptor), "Unreadable native scene index descriptor");
    const auto index_type = LoadBE32(index_descriptor.data()) & 0xf000000f;
    Require((index_type == 0xc0000002 || index_type == 0x20000002) &&
            mc2::draw_capture::BufferBytes(index_descriptor.data(),index_descriptor.size(),false,address,size) &&
            ReadGuest(base,address,size,raw), "Unsupported native scene index buffer");
    const uint32_t index_stride = index_type == 0x20000002 ? 2 : 4;
    Require(raw.size() % index_stride == 0, "Invalid native scene index extent");
    draw.indices.resize(raw.size() / index_stride);
    for (size_t i = 0; i < draw.indices.size(); ++i) {
        const auto* index = raw.data() + i * index_stride;
        draw.indices[i] = index_stride == 2 ? (uint32_t(index[0]) << 8) | index[1] : LoadBE32(index);
    }
    draw.first_index=ctx.r5.u32; draw.index_count=ctx.r6.u32;
    ReadDepthTransform(draw, state, surface);
    return draw;
}
}

SceneVertexShader LoadDepthShaders(const std::filesystem::path& directory) {
    auto shader = LoadVertexShader(directory);
    const std::vector<VertexInput> expected = {{0,0,95,40,0,4,true}, {10,0,95,40,0,3}, {5,0,95,40,24,2}};
    Require(shader.inputs == expected, "Scene depth shader input layout differs from the verified shader");
    return shader;
}
SceneVertexShader LoadOcclusionShader(const std::filesystem::path& directory) {
    auto shader = LoadVertexShader(directory);
    const std::vector<VertexInput> expected = {{0,0,95,12,0,3}};
    Require(shader.inputs == expected, "Occlusion shader does not use the verified float3 input");
    return shader;
}
static SceneColorShaders LoadMaterialShaders(const std::filesystem::path& vertex_directory,
                                            const std::filesystem::path& pixel_directory, std::string_view pixel_dxil) {
    auto vertex = LoadVertexShader(vertex_directory);
    // The vertex metadata gives the attribute types and offsets. Each draw
    // validates the bound fetch descriptors, and the pipeline checks its
    // supported stream count, strides and indexed range before submitting.
    SceneColorShaders shaders{std::move(vertex), ReadNativeFile(pixel_directory / "pixel-shader.container"),
                              ReadNativeFile(pixel_directory / pixel_dxil), {}, {}};
    const auto count_path = pixel_directory / "texture-count.txt";
    if (std::filesystem::exists(count_path)) {
        std::ifstream input(count_path);
        std::string trailing;
        Require((input >> shaders.texture_count) && shaders.texture_count && shaders.texture_count <= kNativeTextureSlotLimit &&
                !(input >> trailing), "Invalid native material texture count");
    }
    return shaders;
}
SceneColorShaders LoadColorShaders(const std::filesystem::path& directory, std::string_view pixel_dxil) {
    auto shaders = LoadMaterialShaders(directory, directory, pixel_dxil);
    const auto resolve_path = directory / "color-resolve.dxil";
    if (std::filesystem::exists(resolve_path)) shaders.resolve_dxil = ReadNativeFile(resolve_path);
    const auto depth_resolve_path = directory / "depth-resolve.dxil";
    if (std::filesystem::exists(depth_resolve_path)) shaders.depth_resolve_dxil = ReadNativeFile(depth_resolve_path);
    return shaders;
}
SceneMaterialLibrary::SceneMaterialLibrary(const std::filesystem::path& directory, std::string_view pixel_dxil)
    : directory_(directory), pixel_dxil_(pixel_dxil) {
    if (std::filesystem::is_regular_file(directory_ / "vertex-shader.container"))
        loaded_.push_back(LoadColorShaders(directory_, pixel_dxil_));
}
const SceneColorShaders* SceneMaterialLibrary::Find(const std::vector<uint8_t>& vertex, const std::vector<uint8_t>& pixel) {
    const auto matches = [&](const SceneColorShaders& program) {
        return vertex == program.vertex.reference && pixel == program.pixel_reference;
    };
    std::lock_guard lock(mutex_);
    const auto cached = std::find_if(loaded_.begin(), loaded_.end(), matches);
    if (cached != loaded_.end()) return &*cached;

    const auto vertex_prefix = ShaderHashPrefix(vertex), pixel_prefix = ShaderHashPrefix(pixel);
    const auto path = directory_ / (vertex_prefix + '-' + pixel_prefix);
    if (std::filesystem::is_regular_file(path / "vertex-shader.container")) {
        auto program = LoadColorShaders(path, pixel_dxil_);
        if (matches(program)) return &loaded_.emplace_back(std::move(program));
    }
    const auto vertex_path = directory_ / "vertex" / vertex_prefix;
    const auto pixel_path = directory_ / "pixel" / pixel_prefix;
    if (std::filesystem::is_regular_file(vertex_path / "vertex-shader.container") &&
        std::filesystem::is_regular_file(pixel_path / "pixel-shader.container")) {
        auto program = LoadMaterialShaders(vertex_path, pixel_path, pixel_dxil_);
        if (matches(program)) return &loaded_.emplace_back(std::move(program));
    }
    // Keep support for older shader banks whose subdirectory names were chosen by hand.
    if (!scanned_named_directories_) {
        if (std::filesystem::is_directory(directory_)) {
            for (const auto& material : std::filesystem::directory_iterator(directory_)) {
                if (material.is_directory() && !HasShaderHashName(material.path().filename().string()) &&
                    std::filesystem::is_regular_file(material.path() / "vertex-shader.container"))
                    loaded_.push_back(LoadColorShaders(material.path(), pixel_dxil_));
            }
        }
        scanned_named_directories_ = true;
        const auto named = std::find_if(loaded_.begin(), loaded_.end(), matches);
        if (named != loaded_.end()) return &*named;
    }
    return nullptr;
}
DepthConfiguration ConfigureDepth(const SceneSurface& surface, const SceneVertexShader& shader) {
    DepthConfiguration configuration;
    configuration.width=surface.width; configuration.height=surface.height; configuration.sample_count=surface.samples;
    configuration.vertex_shader=shader.dxil; configuration.inputs=shader.inputs;
    configuration.cull=plume::RenderCullMode::FRONT;
    return configuration;
}
DepthClear ReadDepthClear(const PPCContext& ctx, uint8_t* base) {
    Require(ctx.r4.u32 == 0x30, "Only the verified full depth/stencil clear is supported");
    DeviceState state(base,ctx.r3.u32);
    state.NoColorAttachments();
    DepthClear clear; clear.surface=state.DepthSurface(base,720); state.FullViewport(clear.surface);
    RequireFullClearRect(ctx, base, clear.surface);
    Require(std::isfinite(ctx.f1.f64) && ctx.f1.f64 == 0, "Scene clear value differs from the verified zero depth/stencil clear");
    return clear;
}
SceneSurface ReadPostprocessDepthStencilClear(const PPCContext& ctx, uint8_t* base) {
    Require(ctx.lr == 0x8238B590 && (ctx.r4.u32 == 0x20 || (ctx.r4.u32 == 0x30 && ctx.f1.f64 == 0)),
            "Unsupported native postprocess depth/stencil clear");
    const DeviceState state(base, ctx.r3.u32);
    const auto surface = state.DepthSurface(base, 720, 1);
    Require((state.bytes[10940] & 0x30) == 0 && state.Word(12724) == 0 && state.Word(12716) == UINT32_MAX,
            "Native stencil clear requires the untiled surface");
    state.FullViewport(surface);
    RequireFullClearRect(ctx, base, surface);
    // Both flag combinations use stencil replace, ref 0, and write mask 0xff.
    // Only flag 0x30 also writes depth.
    return surface;
}
SceneColorClear ReadSceneColorClear(const PPCContext& ctx,uint8_t* base) {
    Require(ctx.r4.u32==0xf,"Only the verified scene color clear is supported");
    DeviceState state(base,ctx.r3.u32);
    SceneColorClear clear;
    clear.surface = ReadTiledColorSurface(state, base);
    // Begin-frame selects the first tile explicitly before clearing it.
    Require(state.Word(12724)==1 && state.Word(12716)==2 && state.Word(10932)==2,
            "Scene color clear uses an unsupported tile predicate");
    RequireFullClearRect(ctx, base, clear.surface);
    std::vector<uint8_t> color;
    Require(ReadGuest(base,ctx.r6.u32 ? ctx.r6.u32 : 0x82247720,16,color),"Unreadable scene clear color");
    for (uint32_t i=0;i<4;++i) {
        clear.value.rgba[i]=std::bit_cast<float>(LoadBE32(color.data()+i*4));
        Require(clear.value.rgba[i]==0,"Only the verified zero HDR clear is supported");
    }
    clear.rectangle=plume::RenderRect(0,0,1280,512);
    return clear;
}
SceneDepthDraw ReadSceneDepthDraw(const PPCContext& ctx, uint8_t* base, const SceneVertexShader& shader) {
    DeviceState state(base, ctx.r3.u32);
    state.NoColorAttachments();
    const auto surface = state.DepthSurface(base,720);
    state.FullViewport(surface);
    Require(state.Word(12700) == 0 && state.Word(10568) == 0x18002, "Native depth pass requires the verified shader and winding");
    Require(ReadBoundShader(base, state.Word(12704), true) == shader.reference, "Untranslated native scene depth shader");
    return {surface, ReadGeometry(ctx, base, state, surface, shader)};
}
SceneDepthDraw ReadOcclusionDraw(const PPCContext& ctx, uint8_t* base, const SceneVertexShader& shader) {
    const auto vertices = ctx.r6.u32;
    Require(ctx.r4.u32 == 4 && ctx.r5.u32 == 0 && vertices && vertices <= 65536 &&
            !(vertices % kOcclusionBoxVertices) && ctx.r7.u32 == (vertices / kOcclusionBoxVertices) * kOcclusionBoxIndices &&
            ctx.r8.u32 == 1 && ctx.r9.u32 == 12, "Unsupported occlusion box geometry");
    const DeviceState state(base, ctx.r3.u32);
    state.NoColorAttachments();
    SceneDepthDraw pass;
    pass.surface = state.DepthSurface(base, 720);
    state.FullViewport(pass.surface);
    Require(state.Word(10452) == 0 && state.Word(10460) == 0 && state.Word(10548) == 0x00700762 &&
            state.Word(10564) == 0x80000 && state.Word(10568) == 0x18002 && state.Word(10572) == 0x43f &&
            state.Float(12672) == 1 && state.Float(12676) == 0 && state.Word(11824) == 0 &&
            (state.bytes[10940] & 0x30) == 0 && state.Word(12724) == 0 && state.Word(12716) == UINT32_MAX,
            "Occlusion box needs the verified untiled, read-only depth state");
    Require(state.Word(12700) == 0 && ReadBoundShader(base, state.Word(12704), true) == shader.reference,
            "Untranslated occlusion shaders");
    ReadDepthTransform(pass.draw, state, pass.surface);
    pass.draw.index_count = ctx.r7.u32;
    return pass;
}

void ReadOcclusionVertices(DepthDraw& draw, uint8_t* base, uint32_t vertices, uint32_t indices) {
    const auto box_count = draw.index_count / kOcclusionBoxIndices;
    std::vector<uint8_t> vertex_data, index_data;
    Require(ReadGuest(base, vertices, box_count * kOcclusionBoxVertices * 12, vertex_data) &&
            ReadGuest(base, indices, draw.index_count * 2, index_data),
            "Unreadable occlusion box vertices or indices");
    draw.vertices = NativeVertexBytes(vertex_data, 2);
    draw.indices.resize(draw.index_count);
    for (size_t i = 0; i < draw.indices.size(); ++i)
        draw.indices[i] = (uint32_t(index_data[i * 2]) << 8) | index_data[i * 2 + 1];
}
SceneColorDraw ReadSceneColorDraw(const PPCContext& ctx, uint8_t* base, const SceneColorShaders& shaders,
                                 SceneMaterialLibrary* world_shaders, SceneMaterialLibrary* additive_shaders) {
    DeviceState state(base, ctx.r3.u32);
    SceneColorDraw snapshot;
    const bool tiled = (state.bytes[10940] & 0x20) != 0;
    if (tiled) {
        snapshot.color_surface = ReadTiledColorSurface(state, base);
        snapshot.depth_surface = state.DepthSurface(base, 512);
        Require(state.Word(12728) == 2, "Native color draw requires the verified automatic tile predicate");
    } else {
        // Format 12 requests higher precision blending with the same 7e3 storage as format 3.
        snapshot.color_surface = ReadFullscreenColorSurface(state, base, 0x302d0, 0xc02d0);
        Require(state.Word(10372) == 0xc02d0, "Additive HDR draw requires the verified blend format");
        snapshot.depth_surface = state.DepthSurface(base, 720, 1);
        Require(state.Word(10492) == 0x00ffff00 && state.Word(10496) == 0x00ffff00,
                "Additive HDR draw requires stencil reference zero and a full read mask");
    }
    Require(state.Word(12724) == 0 && state.Word(12716) == UINT32_MAX && state.Word(10932) == 0 &&
            state.Word(10460) == 0xf && state.Word(10552) == (tiled ? 0x10001 : 0x1000101) &&
            (state.Word(10556) & 0x1f) == 7,
            "Native color draw has unsupported color write, blend or alpha state");
    const auto raster_mode = state.Word(10568);
    const auto cull_bits = raster_mode & 3u;
    Require((raster_mode & ~7u) == 0x18000 && cull_bits != 3, "Unsupported native color raster state");
    // The D3D12 pipeline uses clockwise front faces. Convert the guest's
    // winding and cull selection together to keep the same triangles.
    const bool clockwise_front = (raster_mode & 4) != 0;
    const bool culls_clockwise = clockwise_front == ((cull_bits & 1) != 0);
    snapshot.cull = !cull_bits ? plume::RenderCullMode::NONE :
        culls_clockwise ? plume::RenderCullMode::FRONT : plume::RenderCullMode::BACK;
    const auto pixel_shader = ReadBoundShader(base, state.Word(12700), false);
    const auto vertex_shader = ReadBoundShader(base, state.Word(12704), true);
    if (tiled && pixel_shader == shaders.pixel_reference && vertex_shader == shaders.vertex.reference) snapshot.shaders = &shaders;
    else {
        auto* programs = tiled ? world_shaders : additive_shaders;
        if (programs) snapshot.shaders = programs->Find(vertex_shader, pixel_shader);
        Require(snapshot.shaders, "Untranslated native scene shader pair");
    }
    snapshot.draw = ReadGeometry(ctx, base, state, snapshot.color_surface, snapshot.shaders->vertex,
                                 tiled ? 0x00700766 : 0x00700263);
    if (!tiled && state.Word(11864)) {
        const auto left = state.Word(12684), top = state.Word(12688);
        const auto right = state.Word(12692), bottom = state.Word(12696);
        Require(left < right && top < bottom && right <= snapshot.color_surface.width && bottom <= snapshot.color_surface.height,
                "Additive HDR scissor is outside the color attachment");
        snapshot.draw.scissor = plume::RenderRect(left, top, right, bottom);
    }
    snapshot.pixel_constants = NativeVertexBytes(std::span(state.bytes).subspan(6016,4096),2);
    for (uint32_t slot = 0; slot < snapshot.shaders->texture_count; ++slot) {
        const auto fetch = std::span(state.bytes).subspan(1152 + slot * 24, 24);
        if (!tiled && (LoadBE32(fetch.data() + 4) & 63) == 6) {
            auto& texture = snapshot.textures[slot].emplace<mc2::texture::BaseRGBA8>();
            std::string error;
            if (!mc2::texture::DescribeBaseRGBA8(fetch, texture, error)) throw std::runtime_error(error);
            Require(state.Word(12552 + slot * 4) && texture.width == 1 && texture.height == 1 &&
                    texture.mip_min_level == 0 && texture.mip_max_level == 0 &&
                    (texture.fetch_words[0] & 0x0007fc00) == 0 && texture.fetch_words[3] == 0xc14 &&
                    texture.fetch_words[4] == 0 && texture.fetch_words[5] == 0x200,
                    "Additive HDR draw needs the verified single-texel texture layout");
            auto sampler = FullscreenColorSampler();
            sampler.addressU = sampler.addressV = plume::RenderTextureAddressMode::WRAP;
            snapshot.samplers[slot] = sampler;
            // The RGBA8 upload has already applied its fetch swizzle.
            BindSceneTexture(snapshot.draw, slot, PackTextureFetchMode(0x688, texture.gamma_mask, texture.exp_adjust));
        } else {
            auto& texture = std::get<mc2::texture::BlockTexture>(snapshot.textures[slot]);
            ReadBlockSceneTexture(state, slot, 0, snapshot.draw, texture, snapshot.samplers[slot]);
        }
    }
    return snapshot;
}

plume::RenderBlendDesc SceneAdditiveBlend() {
    auto blend = plume::RenderBlendDesc::Copy();
    blend.blendEnabled = true;
    blend.dstBlend = plume::RenderBlend::ONE;
    blend.srcBlendAlpha = plume::RenderBlend::ZERO;
    blend.dstBlendAlpha = plume::RenderBlend::ONE;
    return blend;
}

bool SameScenePositionInputs(const DepthDraw& depth, const DepthDraw& color) {
    // The verified shaders form clip position from c0-c3 and c5-c8. Both
    // receive the same half-pixel offset from the full logical viewport.
    constexpr size_t position_bytes = 9 * 16;
    return depth.vertex_constants.size() >= position_bytes && color.vertex_constants.size() >= position_bytes &&
           std::equal(depth.vertex_constants.begin(), depth.vertex_constants.begin() + 4 * 16,
                      color.vertex_constants.begin()) &&
           std::equal(depth.vertex_constants.begin() + 5 * 16, depth.vertex_constants.begin() + position_bytes,
                      color.vertex_constants.begin() + 5 * 16) &&
           depth.vertices == color.vertices && depth.indices == color.indices &&
           depth.first_index == color.first_index && depth.index_count == color.index_count &&
           depth.viewport == color.viewport;
}

SceneTileResolve ReadSceneTileResolve(const PPCContext& ctx, uint8_t* base) {
    // Color 0, default sample selection (both samples), exponent -3, and
    // clear color after copy. The depth copy selects sample zero and clears depth/stencil.
    const bool color = ctx.lr == 0x826DA2D8 && ctx.r4.u32 == 0xF4000100;
    const bool depth = ctx.lr == 0x826DA314 && ctx.r4.u32 == 0x214;
    Require((color || depth) && ctx.r8.u32 == 0 && ctx.r9.u32 == 0,
            "Unsupported native tiled resolve flags or caller");
    DeviceState state(base, ctx.r3.u32);
    SceneTileResolve resolve;
    resolve.color = ReadTiledColorSurface(state, base);
    resolve.depth = state.DepthSurface(base, 512);
    resolve.copy_depth = depth;
    const uint32_t predicate = state.Word(12716);
    Require(state.Word(12724) == 1 && (predicate == 2 || predicate == 8) && state.Word(10932) == predicate,
            "Native scene resolve requires an explicit tile predicate");
    resolve.tile = predicate == 2 ? 0 : 1;
    const auto tile_state = 12760 + resolve.tile * 16;
    const auto top = state.Word(tile_state + 4), bottom = state.Word(tile_state + 12);
    std::vector<uint8_t> rectangle, origin, clear, destination;
    Require(ReadGuest(base, ctx.r5.u32, 16, rectangle) && LoadBE32(rectangle.data()) == 0 &&
            LoadBE32(rectangle.data() + 4) == top && LoadBE32(rectangle.data() + 8) == resolve.color.width &&
            LoadBE32(rectangle.data() + 12) == bottom, "Native resolve rectangle differs");
    Require(ReadGuest(base, ctx.r7.u32, 8, origin) && LoadBE32(origin.data()) == 0 && LoadBE32(origin.data() + 4) == top,
            "Native resolve destination origin differs");
    if (depth) {
        Require(ctx.f1.f64 == 0 && mc2::capture::GuestRange(ctx.r1.u32, 88) &&
                ReadGuest(base, ctx.r1.u32 + 84, 4, clear) && LoadBE32(clear.data()) == 0,
                "Native depth resolve requires a zero depth and stencil clear");
    } else {
        Require(ReadGuest(base, 0x82247720, 16, clear) &&
                std::all_of(clear.begin(), clear.end(), [](uint8_t byte) { return byte == 0; }),
                "Native color resolve requires the verified zero clear");
    }
    Require(ReadGuest(base, ctx.r6.u32, 52, destination) && (LoadBE32(destination.data()) & 15) == 3,
            "Invalid native resolve destination texture");
    const auto word = [&](uint32_t offset) { return LoadBE32(destination.data() + offset); };
    Require(word(28) == (depth ? 0x8a000002 : 0x8a000156) &&
            (word(32) & 4095) == (depth ? 0x97u : 0x60u) && (word(32) >> 12) != 0 &&
            word(36) == 0x59e4ff && word(40) == 0xd11 && word(44) == 0 && word(48) == 0xa00,
            "Native resolve requires the verified full-size tiled texture destination");
    resolve.destination.address = ctx.r6.u32;
    for (uint32_t i = 0; i < resolve.destination.fetch_words.size(); ++i)
        resolve.destination.fetch_words[i] = word(28 + i * 4);
    resolve.rectangle = plume::RenderRect(0, top, resolve.color.width, bottom);
    resolve.exponent = depth ? 0 : -3;
    return resolve;
}

static void NormalizeDepthRestoreVertex(std::vector<uint8_t>& shader) {
    constexpr size_t position_fetch = 280;
    if (shader.size() != 340 || LoadBE32(shader.data() + position_fetch) != 0x05f80000 ||
        LoadBE32(shader.data() + position_fetch + 4) != 0x0026323f ||
        LoadBE32(shader.data() + position_fetch + 8) != 4) return;
    // The guest patches its position fetch with the bound declaration:
    // float4, stride four dwords. The native input layout already supplies it.
    shader[position_fetch + 5] = 0;
    shader[position_fetch + 6] = 2;
    shader[position_fetch + 11] = 0;
}

ScenePassShaders LoadDepthRestoreShaders(const std::filesystem::path& directory) {
    // This shader uses the explicit UP declaration, which supplies the fetch
    // format and stride at draw time.
    auto vertex = ReadNativeFile(directory / "vertex-shader.container");
    NormalizeDepthRestoreVertex(vertex);
    return {{std::move(vertex), ReadNativeFile(directory / "vertex.dxil"),
             {{0,0,95,16,0,4}}}, ReadNativeFile(directory / "pixel-shader.container"),
            ReadNativeFile(directory / "pixel.dxil")};
}

SceneDepthRestore ReadSceneDepthRestore(const PPCContext& ctx, uint8_t* base,
        const ScenePassShaders& shaders, const SceneResolveTexture& source) {
    Require(ctx.lr == 0x826DA868 && ctx.r4.u32 == 8 && ctx.r5.u32 == 3 && ctx.r6.u32 == 16,
            "Native depth restore requires the verified user-memory rectangle");
    const DeviceState state(base, ctx.r3.u32);
    SceneDepthRestore restore;
    restore.depth = state.DepthSurface(base, 720, 1);
    state.FullViewport(restore.depth);
    Require((state.bytes[10940] & 0x30) == 0 && state.Word(12724) == 0 && state.Word(12716) == UINT32_MAX &&
            state.Word(10548) == 0x00700777 && state.Word(10460) == 0 && (state.Word(10556) & 0x1f) == 7 &&
            state.Word(10492) == 0x00ffff00 && state.Word(10496) == 0x00ffff00 && state.Word(10564) == 0x80000 &&
            state.Word(10568) == 0x18002 && state.Word(10572) == 0x43f &&
            state.Float(12672) == 1 && state.Float(12676) == 0 &&
            state.Float(10504) == 640 && state.Float(10508) == 640 && state.Float(10512) == -360 &&
            state.Float(10516) == 360 && state.Float(10520) == -1 && state.Float(10524) == 1,
            "Native depth restore depth/stencil, color mask or viewport state changed");
    auto vertex = ReadBoundShader(base, state.Word(12704), true);
    NormalizeDepthRestoreVertex(vertex);
    Require(vertex == shaders.vertex.reference &&
            ReadBoundShader(base, state.Word(12700), false) == shaders.pixel_reference,
            "Untranslated native depth restore shaders");
    std::vector<uint8_t> declaration;
    Require(ReadGuest(base, state.Word(11824), 64, declaration) && (LoadBE32(declaration.data()) & 15) == 5,
            "Invalid native depth restore declaration");
    constexpr uint32_t declaration_words[] = {1,0,0xff000000,0,0,0,0,0,0x001a23a6,0};
    for (uint32_t i = 0; i < std::size(declaration_words); ++i) {
        const auto value = LoadBE32(declaration.data() + 24 + i * 4);
        // The cached declaration changes its header flags after the first
        // draw, while the element at byte 52 stays the same.
        if (i == 6 && value == 3) continue;
        if (value != declaration_words[i])
            throw std::runtime_error(std::format("Native restore declaration word {} differs ({:#x}, expected {:#x})",
                                                 i, value, declaration_words[i]));
    }
    RequireSceneTexture(state, source, 0);
    ReadFullscreenConstants(state, restore.depth, restore.draw, restore.pixel_constants, true);
    return restore;
}

void ReadDepthRestoreVertices(DepthDraw& draw, uint8_t* base, uint32_t address) {
    constexpr float corners[][4] = {{0,0,0,1}, {1,0,0,1}, {0,1,0,1}, {1,1,0,1}};
    std::vector<uint8_t> raw;
    Require(ReadGuest(base, address, 3 * sizeof(corners[0]), raw), "Unreadable native depth restore vertices");
    draw.vertices = NativeVertexBytes(raw, 2);
    Require(std::memcmp(draw.vertices.data(), corners, draw.vertices.size()) == 0, "Native depth restore rectangle changed");
    draw.vertices.resize(sizeof(corners));
    // Expand the captured three-corner rectangle into two triangles.
    std::memcpy(draw.vertices.data() + raw.size(), corners[3], sizeof(corners[3]));
    draw.indices = {0,1,2,2,1,3};
    draw.index_count = uint32_t(draw.indices.size());
}

ScenePassShaders LoadFullscreenColorShaders(const std::filesystem::path& directory, bool grade, const char* pixel_dxil) {
    auto vertex = LoadVertexShader(directory);
    const std::vector<VertexInput> copy_inputs = {{0,0,95,48,0,4}, {5,0,95,48,16,2}, {10,0,95,48,24,4},
        {10,1,95,48,40,4,false,VertexStorage::UNorm8}};
    const std::vector<VertexInput> grade_inputs = {{0,0,95,32,0,4,true}, {6,0,95,32,0,3},
        {7,0,95,32,12,4,false,VertexStorage::UInt8}, {3,0,95,32,16,4,false,VertexStorage::UInt8},
        {10,0,95,32,20,4,false,VertexStorage::UInt8}};
    Require(vertex.inputs == (grade ? grade_inputs : copy_inputs), "Fullscreen HDR vertex inputs changed");
    return {std::move(vertex), ReadNativeFile(directory / "pixel-shader.container"), ReadNativeFile(directory / pixel_dxil)};
}

SceneFullscreenDraw ReadSceneFullscreenDraw(const PPCContext& ctx, uint8_t* base,
        const ScenePassShaders& shaders, const SceneResolveTexture& source, bool grade) {
    Require(grade ? (ctx.lr == 0x827B061C && ctx.r4.u32 == 5 && ctx.r5.u32 == 0 && ctx.r6.u32 == 4) :
        (ctx.lr == 0x826D9CC0 && ctx.r4.u32 == 4 && ctx.r5.u32 == 0 && ctx.r6.u32 == 4 &&
         ctx.r7.u32 == 6 && ctx.r8.u32 == 6 && ctx.r9.u32 == 48), "Unsupported fullscreen HDR draw arguments");
    const DeviceState state(base, ctx.r3.u32);
    SceneFullscreenDraw pass;
    pass.surface = ReadFullscreenColorSurface(state, base);
    std::vector<uint8_t> descriptor;
    RequireFullscreenDrawState(state, pass.surface, grade, grade ? 7 : 15, grade ? 0x18002 : 0x18000);
    Require(ReadBoundShader(base, state.Word(12704), true) == shaders.vertex.reference &&
            ReadBoundShader(base, state.Word(12700), false) == shaders.pixel_reference, "Untranslated fullscreen HDR shaders");
    RequireSceneTexture(state, source, 0x4800);
    ReadFullscreenConstants(state, pass.surface, pass.draw, pass.pixel_constants, grade);
    if (grade) {
        std::vector<uint8_t> raw;
        uint32_t address = 0, size = 0;
        Require(state.bytes[12536] == 8 && ReadGuest(base,state.Word(12468),32,descriptor) &&
                mc2::draw_capture::BufferBytes(descriptor.data(), descriptor.size(), true, address, size) &&
                size == 128 && state.Word(1916) == LoadBE32(descriptor.data() + 28) &&
                (state.Word(1916) & 3) == 2 && ReadGuest(base,address,size,raw), "Invalid color grade vertex buffer");
        const auto vertex_address = LoadBE32(descriptor.data() + 24);
        const auto physical_fetch = PhysicalFetchAddress(vertex_address);
        Require(state.Word(1912) == physical_fetch, "Color grade vertex binding differs from its buffer");
        pass.draw.vertices = NativeVertexBytes(raw, 2);
        pass.draw.indices = {0,1,2,0,2,3};
        pass.draw.index_count = 6;
    } else {
        for (uint32_t i = 0; i < 16; ++i)
            Require(state.Float(1920 + (5 * 16) + 4 * i) == (i % 5 == 0 ? 1.f : 0.f), "Fullscreen HDR copy transform changed");
    }
    return pass;
}

void ReadFullscreenUpVertices(DepthDraw& draw, uint8_t* base, uint32_t address, uint32_t index_address) {
    std::vector<uint8_t> vertices, indices;
    Require(ReadGuest(base,address,192,vertices) && ReadGuest(base,index_address,24,indices), "Unreadable fullscreen HDR vertices or indices");
    const float half_x = 1.f / draw.viewport.width, half_y = 1.f / draw.viewport.height;
    for (uint32_t i = 0; i < 4; ++i) {
        const auto* position = vertices.data() + i * 48;
        const float x = std::bit_cast<float>(LoadBE32(position)), y = std::bit_cast<float>(LoadBE32(position + 4));
        Require(std::abs(x - ((i & 1 ? 1.f : -1.f) - half_x)) < 1.0e-6f &&
                std::abs(y - ((i & 2 ? -1.f : 1.f) + half_y)) < 1.0e-6f &&
                LoadBE32(position + 8) == 0 && LoadBE32(position + 12) == 0x3f800000, "HDR copy does not cover the full viewport");
    }
    draw.vertices = NativeVertexBytes(vertices, 2);
    draw.indices = {0,1,3,0,3,2};
    for (size_t i = 0; i < draw.indices.size(); ++i)
        Require(LoadBE32(indices.data() + i * 4) == draw.indices[i], "Fullscreen HDR copy indices changed");
    draw.index_count = uint32_t(draw.indices.size());
}

plume::RenderSamplerDesc FullscreenColorSampler(bool linear) {
    plume::RenderSamplerDesc sampler;
    sampler.minFilter = sampler.magFilter = linear ? plume::RenderFilter::LINEAR : plume::RenderFilter::NEAREST;
    sampler.mipmapMode = plume::RenderMipmapMode::NEAREST;
    sampler.addressU = sampler.addressV = plume::RenderTextureAddressMode::CLAMP;
    sampler.maxLOD = 0;
    return sampler;
}

static void RequireSceneResolveDestination(const PPCContext& ctx, uint8_t* base, const SceneResolveTexture& destination) {
    std::vector<uint8_t> descriptor;
    Require(ctx.r6.u32 == destination.address && ReadGuest(base, ctx.r6.u32, 52, descriptor) &&
            (LoadBE32(descriptor.data()) & 15) == 3, "Scene resolve did not reuse the scene texture");
    for (uint32_t i = 0; i < destination.fetch_words.size(); ++i)
        Require(LoadBE32(descriptor.data() + 28 + i * 4) == destination.fetch_words[i], "Scene resolve texture layout changed");
}

SceneSurface ReadFullscreenResolve(const PPCContext& ctx, uint8_t* base, const SceneResolveTexture& destination) {
    Require(ctx.lr == 0x827B3694 && ctx.r4.u32 == 0xf4000000 && !ctx.r5.u32 && !ctx.r7.u32 && !ctx.r8.u32 && !ctx.r9.u32,
            "Unsupported fullscreen resolve flags or region");
    const DeviceState state(base, ctx.r3.u32);
    // A full resolve covers the attachment even if the last light used a scissor.
    const auto surface = ReadFullscreenColorSurface(state, base, 0x302d0, 0xc02d0);
    RequireSceneResolveDestination(ctx, base, destination);
    return surface;
}

SceneSurface ReadPostprocessDepthResolve(const PPCContext& ctx, uint8_t* base, const SceneResolveTexture& destination) {
    Require(ctx.lr == 0x827B3560 && ctx.r4.u32 == 4 && !ctx.r5.u32 && !ctx.r7.u32 && !ctx.r8.u32 && !ctx.r9.u32,
            "Unsupported single-sample depth resolve flags or region");
    const DeviceState state(base, ctx.r3.u32);
    const auto surface = state.DepthSurface(base, 720, 1);
    state.FullViewport(surface, true);
    Require((state.bytes[10940] & 0x30) == 0 && state.Word(12724) == 0 && state.Word(12716) == UINT32_MAX,
            "Single-sample depth resolve requires the untiled depth attachment");
    RequireSceneResolveDestination(ctx, base, destination);
    return surface;
}

SceneFullscreenDraw ReadDepthMaskDraw(const PPCContext& ctx, uint8_t* base,
        const ScenePassShaders& shaders, const SceneResolveTexture& source) {
    Require(ctx.lr == 0x826D9CC0 && ctx.r4.u32 == 4 && !ctx.r5.u32 && ctx.r6.u32 == 4 &&
            ctx.r7.u32 == 6 && ctx.r8.u32 == 1 && ctx.r9.u32 == 16, "Unsupported scene depth mask geometry");
    const DeviceState state(base, ctx.r3.u32);
    ReadFullscreenColorSurface(state, base);
    SceneFullscreenDraw pass;
    pass.surface = state.DepthSurface(base, 720, 1);
    RequireFullscreenDrawState(state, pass.surface, true, 0, 0x18000, 0x10001, 0x00700716);
    Require(ReadBoundShader(base, state.Word(12704), true) == shaders.vertex.reference &&
            ReadBoundShader(base, state.Word(12700), false) == shaders.pixel_reference, "Untranslated scene depth mask shaders");
    RequireSceneTexture(state, source, 0x4800);
    ReadFullscreenConstants(state, pass.surface, pass.draw, pass.pixel_constants, true);
    pass.shaders = &shaders;
    return pass;
}

SceneColorClear ReadBloomClear(const PPCContext& ctx, uint8_t* base) {
    Require(ctx.lr == 0x8238B590 && ctx.r4.u32 == 0xf, "Unsupported bloom color clear arguments");
    const DeviceState state(base, ctx.r3.u32);
    SceneColorClear clear;
    clear.surface = ReadBloomColorSurface(state, base);
    RequireFullClearRect(ctx, base, clear.surface);
    std::vector<uint8_t> value;
    Require(ReadGuest(base, ctx.r6.u32 ? ctx.r6.u32 : 0x82247720, 16, value) &&
            std::all_of(value.begin(), value.end(), [](uint8_t byte) { return byte == 0; }), "Bloom target clear requires zero");
    clear.value = plume::RenderColor(0,0,0,0);
    clear.rectangle = plume::RenderRect(0,0,clear.surface.width,clear.surface.height);
    return clear;
}

ScenePassShaders LoadQuadShaders(const std::filesystem::path& directory, const char* pixel_dxil) {
    auto vertex = LoadVertexShader(directory);
    const std::vector<VertexInput> inputs = {{0,0,95,32,0,4}, {5,0,95,32,16,2}};
    Require(vertex.inputs == inputs, "Scene quad vertex layout changed");
    return {std::move(vertex), ReadNativeFile(directory / "pixel-shader.container"), ReadNativeFile(directory / pixel_dxil)};
}

static void RequireQuadArguments(const PPCContext& ctx) {
    Require(ctx.lr == 0x826D9CC0 && ctx.r4.u32 == 4 && ctx.r5.u32 == 0 && ctx.r6.u32 == 4 && ctx.r7.u32 == 6 &&
            ctx.r8.u32 == 1 && ctx.r9.u32 == 32, "Unsupported indexed scene quad arguments");
}

SceneFullscreenDraw ReadBloomDraw(const PPCContext& ctx, uint8_t* base, std::span<const ScenePassShaders> programs,
        const SceneResolveTexture& primary, const SceneResolveTexture* color) {
    RequireQuadArguments(ctx);
    const DeviceState state(base, ctx.r3.u32);
    SceneFullscreenDraw pass;
    pass.surface = ReadBloomColorSurface(state, base);
    RequireFullscreenDrawState(state, pass.surface, false);
    const auto vertex = ReadBoundShader(base, state.Word(12704), true);
    const auto pixel = ReadBoundShader(base, state.Word(12700), false);
    const auto program = std::find_if(programs.begin(), programs.end(), [&](const ScenePassShaders& candidate) {
        return vertex == candidate.vertex.reference && pixel == candidate.pixel_reference;
    });
    Require(program != programs.end(), "Untranslated bloom shaders");
    pass.shaders = &*program;
    RequireSceneTexture(state, primary, 0x4800, 0, !color);
    ReadFullscreenConstants(state, pass.surface, pass.draw, pass.pixel_constants, false);
    if (color) {
        RequireSceneTexture(state, *color, 0x4800, 1);
        BindSceneTexture(pass.draw, 1);
    }
    return pass;
}

SceneFullscreenDraw ReadCompositionDraw(const PPCContext& ctx, uint8_t* base, const ScenePassShaders& shaders,
        const SceneResolveTexture& depth, const SceneResolveTexture& color, const SceneResolveTexture& bloom) {
    RequireQuadArguments(ctx);
    const DeviceState state(base, ctx.r3.u32);
    SceneFullscreenDraw pass;
    pass.surface = ReadFullscreenColorSurface(state, base);
    RequireFullscreenDrawState(state, pass.surface, true);
    Require(ReadBoundShader(base, state.Word(12704), true) == shaders.vertex.reference &&
            ReadBoundShader(base, state.Word(12700), false) == shaders.pixel_reference, "Untranslated composition shaders");
    RequireSceneTexture(state, depth, 0x4800);
    RequireSceneTexture(state, color, 0x4800, 1);
    RequireSceneTexture(state, bloom, 0x4800, 2, true);
    ReadFullscreenConstants(state, pass.surface, pass.draw, pass.pixel_constants, true);
    BindSceneTexture(pass.draw, 1);
    BindSceneTexture(pass.draw, 2);
    return pass;
}

SceneFullscreenDraw ReadBackBufferDraw(const PPCContext& ctx, uint8_t* base, const ScenePassShaders& shaders,
        const SceneResolveTexture& color) {
    RequireQuadArguments(ctx);
    const DeviceState state(base, ctx.r3.u32);
    SceneFullscreenDraw pass;
    pass.surface = ReadFullscreenColorSurface(state, base, 0x2d0);
    Require(state.Word(12464) == 0, "Back-buffer copy requires no depth attachment");
    RequireFullscreenDrawState(state, pass.surface, false);
    Require(ReadBoundShader(base, state.Word(12704), true) == shaders.vertex.reference &&
            ReadBoundShader(base, state.Word(12700), false) == shaders.pixel_reference, "Untranslated back-buffer shaders");
    RequireSceneTexture(state, color, 0x4800);
    ReadFullscreenConstants(state, pass.surface, pass.draw, pass.pixel_constants, false);
    return pass;
}

SceneFullscreenDraw ReadBackBufferOverlay(const PPCContext& ctx, uint8_t* base,
        const ScenePassShaders& shaders, SceneOverlayBinding& binding) {
    Require(ctx.lr == 0x826D9CC0 && ctx.r4.u32 == 4 && ctx.r5.u32 == 0 && ctx.r6.u32 && ctx.r6.u32 <= 4096 &&
            ctx.r7.u32 && ctx.r7.u32 <= 12288 && !(ctx.r7.u32 % 3) && ctx.r8.u32 == 6 && ctx.r9.u32 == 48,
            "Unsupported overlay geometry");
    const DeviceState state(base, ctx.r3.u32);
    SceneFullscreenDraw pass;
    pass.surface = ReadFullscreenColorSurface(state, base, 0x2d0);
    Require(state.Word(12464) == 0 && state.Word(10560) == 0x10, "Unsupported back-buffer overlay depth or blend state");
    RequireFullscreenDrawState(state, pass.surface, false, 15, 0x18000, 0x01000706);
    Require(ReadBoundShader(base, state.Word(12704), true) == shaders.vertex.reference &&
            ReadBoundShader(base, state.Word(12700), false) == shaders.pixel_reference, "Untranslated overlay shaders");
    const auto fetch = std::span(state.bytes).subspan(1152, 24);
    const auto word0 = state.Word(1152), word3 = state.Word(1164), word4 = state.Word(1168), word5 = state.Word(1172);
    constexpr uint32_t mip_max_mask = 15u << 6;
    const uint32_t addressing = word0 & 0x0007fc00;
    const uint32_t format = state.Word(1156) & 63;
    const uint32_t filtering = word3 & 0xffffe000;
    const bool point = format == 6 && !filtering && !word4;
    const bool anisotropic = filtering == 0x06280000 && (word4 & ~mip_max_mask) == 0x803;
    if (!state.Word(12552) || (addressing != 0 && addressing != 0x00004800) || (!point && !anisotropic) || (word5 & 0x1ff)) {
        throw std::runtime_error(std::format("Unsupported overlay texture format {} sampler {:#x}/{:#x}/{:#x}/{:#x}",
                                            format, word0, word3, word4, word5));
    }
    ReadFullscreenConstants(state, pass.surface, pass.draw, pass.pixel_constants, false);
    binding.sampler = point ? FullscreenColorSampler() : OverlaySampler();
    binding.sampler.addressU = binding.sampler.addressV = addressing
        ? plume::RenderTextureAddressMode::CLAMP : plume::RenderTextureAddressMode::WRAP;
    std::string error;
    uint32_t mode;
    if (format == 6) {
        mc2::texture::BaseRGBA8 texture;
        if (!mc2::texture::DescribeBaseRGBA8(fetch, texture, error)) throw std::runtime_error(error);
        Require(texture.mip_min_level == 0 && texture.mip_max_level == 0, "Overlay texture needs its base level");
        // DecodeBaseRGBA8 already applies the fetch swizzle to the upload.
        mode = PackTextureFetchMode(0x688, texture.gamma_mask, texture.exp_adjust);
        binding.texture = texture;
    } else {
        mc2::texture::BlockTexture texture;
        if (!mc2::texture::DescribeBlockTexture(fetch, texture, error)) throw std::runtime_error(error);
        Require(texture.mip_min_level == 0, "Overlay atlas needs its base level");
        binding.sampler.maxLOD = float(texture.mip_max_level);
        mode = PackTextureFetchMode(texture.swizzle, texture.gamma_mask, texture.exp_adjust);
        binding.texture = std::move(texture);
    }
    std::memcpy(pass.draw.shared_constants.data() + 288, &mode, sizeof(mode));
    binding.vertex_count = ctx.r6.u32;
    binding.index_count = ctx.r7.u32;
    return pass;
}

void ReadOverlayUpVertices(DepthDraw& draw, uint8_t* base, uint32_t address, uint32_t index_address,
                           const SceneOverlayBinding& binding) {
    std::vector<uint8_t> vertices, indices;
    Require(binding.vertex_count && binding.vertex_count <= 4096 && binding.index_count && binding.index_count <= 12288 &&
            ReadGuest(base, address, uint64_t(binding.vertex_count) * 48, vertices) &&
            ReadGuest(base, index_address, uint64_t(binding.index_count) * 4, indices), "Unreadable overlay vertices or indices");
    draw.vertices = NativeVertexBytes(vertices, 2);
    draw.indices.resize(binding.index_count);
    for (uint32_t i = 0; i < binding.index_count; ++i) draw.indices[i] = LoadBE32(indices.data() + i * 4);
    draw.index_count = binding.index_count;
}

plume::RenderBlendDesc OverlayBlend() {
    auto blend = plume::RenderBlendDesc::AlphaBlend();
    blend.srcBlendAlpha = plume::RenderBlend::ZERO;
    blend.dstBlendAlpha = plume::RenderBlend::ONE;
    return blend;
}

plume::RenderSamplerDesc OverlaySampler(uint32_t max_mip_level, bool clamp_uv) {
    plume::RenderSamplerDesc sampler;
    sampler.anisotropyEnabled = true;
    sampler.maxAnisotropy = 4;
    sampler.maxLOD = float(max_mip_level);
    if (clamp_uv) sampler.addressU = sampler.addressV = plume::RenderTextureAddressMode::CLAMP;
    return sampler;
}

ScenePassShaders LoadMenuFanShaders(const std::filesystem::path& directory) {
    auto vertex = LoadVertexShader(directory);
    const std::vector<VertexInput> expected = {{0,0,95,32,0,4,true}, {6,0,95,32,0,3},
        {7,0,95,32,12,4,false,VertexStorage::UInt8}, {3,0,95,32,16,4,false,VertexStorage::UInt8},
        {10,0,95,32,20,4,false,VertexStorage::UInt8}, {5,0,95,32,24,2}};
    Require(vertex.inputs == expected, "Menu fan vertex layout changed");
    return {std::move(vertex), ReadNativeFile(directory / "pixel-shader.container"), ReadNativeFile(directory / "pixel.dxil")};
}

SceneMenuFan ReadMenuFan(const PPCContext& ctx, uint8_t* base, std::span<const SceneMenuFanProgram> programs) {
    Require(ctx.lr == 0x826D9A60 && ctx.r4.u32 == 5 && ctx.r5.u32 == 4 && ctx.r6.u32 == 32,
            "Unsupported menu triangle fan");
    const DeviceState state(base, ctx.r3.u32);
    SceneMenuFan menu;
    auto& pass = menu.pass;
    pass.surface = ReadFullscreenColorSurface(state, base, 0x2d0);
    Require(state.Word(12464) == 0, "Menu fan needs only the back buffer");
    const auto vertex = ReadBoundShader(base, state.Word(12704), true);
    const auto pixel = ReadBoundShader(base, state.Word(12700), false);
    const auto program = std::find_if(programs.begin(), programs.end(), [&](const SceneMenuFanProgram& candidate) {
        return vertex == candidate.shaders.vertex.reference && pixel == candidate.shaders.pixel_reference;
    });
    Require(program != programs.end(), "Untranslated menu fan shaders");
    RequireFullscreenDrawState(state, pass.surface, false, 15, 0x18002, program->blend_control);
    pass.shaders = &program->shaders;
    ReadFullscreenConstants(state, pass.surface, pass.draw, pass.pixel_constants, false);
    menu.blend = program->blend_control == 0x10001 ? plume::RenderBlendDesc::Copy() : OverlayBlend();
    if (program->blend_control == 0x01000106) menu.blend.dstBlend = plume::RenderBlend::ONE;
    for (uint32_t format : program->block_formats) {
        if (!format) break;
        const auto slot = menu.texture_count++;
        ReadBlockSceneTexture(state, slot, format, pass.draw, menu.textures[slot], menu.samplers[slot],
                              (program->clamp_slots & (1u << slot)) != 0);
    }
    return menu;
}

void ReadMenuFanVertices(DepthDraw& draw, uint8_t* base, uint32_t address) {
    std::vector<uint8_t> vertices;
    Require(ReadGuest(base, address, 128, vertices), "Unreadable menu fan vertices");
    draw.vertices = NativeVertexBytes(vertices, 2);
    draw.indices = {0,1,2,0,2,3};
    draw.index_count = uint32_t(draw.indices.size());
}

uint32_t ReadScanoutResolve(const PPCContext& ctx, uint8_t* base, const SceneSurface& back_buffer) {
    Require(ctx.lr == 0x8248B7AC && !ctx.r4.u32 && !ctx.r5.u32 && !ctx.r7.u32 && !ctx.r8.u32 && !ctx.r9.u32,
            "Unsupported display resolve flags or region");
    const DeviceState state(base, ctx.r3.u32);
    Require(ReadFullscreenColorSurface(state, base, 0x2d0) == back_buffer, "Display resolve source differs from the back buffer");
    std::vector<uint8_t> descriptor;
    Require(ReadGuest(base, ctx.r6.u32, 52, descriptor) && (LoadBE32(descriptor.data()) & 15) == 3,
            "Invalid display resolve destination");
    const auto word = [&](uint32_t offset) { return LoadBE32(descriptor.data() + offset); };
    Require(word(28) == 0x8a000002 && (word(32) & 4095) == 6 && (word(32) >> 12) != 0 &&
            word(36) == 0x59e4ff && word(40) == 0x1414 && word(44) == 0 && word(48) == 0x200,
            "Display resolve requires the verified full RGBA8 texture");
    return ctx.r6.u32;
}

void ReadQuadUpVertices(DepthDraw& draw, uint8_t* base, uint32_t address, uint32_t index_address, uint32_t stride) {
    std::vector<uint8_t> vertices, indices;
    Require((stride == 16 || stride == 32) && ReadGuest(base,address,4 * stride,vertices) &&
            ReadGuest(base,index_address,12,indices), "Unreadable scene quad vertices or indices");
    draw.vertices = NativeVertexBytes(vertices, 2);
    constexpr uint32_t expected[] = {0,1,3,0,3,2};
    draw.indices.resize(std::size(expected));
    for (size_t i = 0; i < draw.indices.size(); ++i) {
        const auto index = uint32_t(indices[i*2]) << 8 | indices[i*2+1];
        Require(index < 4, "Scene quad index exceeds the four supplied vertices");
        if (stride == 32) Require(index == expected[i], "Scene quad triangle indices changed");
        draw.indices[i] = index;
    }
    draw.index_count = uint32_t(draw.indices.size());
}

std::pair<SceneSurface, SceneResolveTexture> ReadBloomResolve(const PPCContext& ctx, uint8_t* base) {
    Require(ctx.lr == 0x827B3694 && ctx.r4.u32 == 0 && !ctx.r5.u32 && !ctx.r7.u32 && !ctx.r8.u32 && !ctx.r9.u32,
            "Unsupported bloom resolve flags or region");
    const DeviceState state(base, ctx.r3.u32);
    const auto surface = ReadBloomColorSurface(state, base);
    std::vector<uint8_t> descriptor;
    Require(ReadGuest(base, ctx.r6.u32, 52, descriptor) && (LoadBE32(descriptor.data()) & 15) == 3,
            "Invalid bloom resolve destination");
    const auto word = [&](uint32_t offset) { return LoadBE32(descriptor.data() + offset); };
    Require(word(28) == 0x82c00002 && (word(32) & 4095) == 0x5a && (word(32) >> 12) != 0 &&
            word(36) == 0x16a141 && word(40) == 0xd10 && word(44) == 0 && word(48) == 0x200,
            "Bloom resolve requires the verified 322x182 RGBA16_UNORM texture");
    SceneResolveTexture destination;
    destination.address = ctx.r6.u32;
    for (uint32_t i = 0; i < destination.fetch_words.size(); ++i) destination.fetch_words[i] = word(28 + i * 4);
    return {surface, destination};
}
}
