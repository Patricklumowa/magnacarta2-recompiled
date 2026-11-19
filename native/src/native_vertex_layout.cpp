#include "native_vertex_layout.h"
#include "shader_capture.h"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace mc2::native {
namespace {
void Require(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
}
const char* VertexSemantic(uint32_t usage) {
    static constexpr const char* names[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE",
        "TEXCOORD", "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"};
    Require(usage < std::size(names), "Invalid vertex semantic");
    return names[usage];
}
std::vector<VertexInput> ReadVertexInputs(std::span<const uint8_t> bytes) {
    const auto word = [&](uint64_t offset) {
        Require(offset <= bytes.size() && bytes.size() - offset >= 4, "Truncated vertex shader metadata");
        return mc2::capture::LoadBE32(bytes.data() + offset);
    };
    Require(bytes.size() >= 36 && (word(0) & 1) && bytes.size() <= mc2::capture::kMaxContainerBytes,
            "Invalid vertex shader container");
    const uint64_t virtual_size = word(4), physical_size = word(8), shader = word(24);
    Require(virtual_size + physical_size == bytes.size() && shader >= 36 && shader + 36 <= virtual_size,
            "Vertex shader container extents differ");
    const uint64_t code = virtual_size + word(shader), code_size = word(shader + 4);
    Require(code >= virtual_size && code + code_size <= bytes.size() && code_size % 12 == 0,
            "Invalid vertex instruction extent");
    const uint64_t count = word(shader + 28), elements = shader + 36 + uint64_t(word(shader + 24)) * 4;
    Require(count && count <= 32 && elements + count * 4 <= virtual_size, "Invalid vertex element count");
    std::vector<uint32_t> metadata;
    for (uint64_t i = 0; i < count; ++i) metadata.push_back(word(elements + i * 4));
    std::sort(metadata.begin(), metadata.end(), [](uint32_t a, uint32_t b) { return (a & 4095) < (b & 4095); });
    std::vector<VertexInput> result;
    uint32_t fetch = UINT32_MAX, stride = 0, last_address = UINT32_MAX;
    for (auto element : metadata) {
        const auto address = element & 4095, usage = (element >> 12) & 15, usage_index = (element >> 16) & 15;
        VertexSemantic(usage);
        Require(address != last_address && uint64_t(address) * 12 + 12 <= code_size, "Invalid vertex fetch address");
        last_address = address;
        const auto at = code + uint64_t(address) * 12;
        const auto w0 = word(at), w1 = word(at + 4), w2 = word(at + 8);
        Require((w0 & 31) == 0 && ((w0 >> 19) & 1) && !(w0 & ((1u << 11) | (1u << 18))) &&
                ((w0 >> 5) & 63) == 0 && (w0 >> 30) == 0,
                "Unsupported vertex fetch addressing");
        if (!(w1 & (1u << 30))) {
            const auto select = (w0 >> 25) & 3;
            Require(select < 3, "Invalid vertex fetch selector");
            fetch = ((w0 >> 20) & 31) * 3 + select;
            stride = (w2 & 255) * 4;
        }
        Require(fetch != UINT32_MAX && stride, "Mini vertex fetch has no full fetch");
        for (const auto& prior : result)
            Require(prior.usage != usage || prior.usage_index != usage_index, "Duplicate vertex input semantic");
        bool reads_input = false;
        for (uint32_t c = 0; c < 4; ++c) {
            const auto swizzle = (w1 >> (c * 3)) & 7;
            Require(swizzle != 6, "Invalid vertex fetch swizzle");
            reads_input |= swizzle < 4;
        }
        if (!reads_input) {
            // DXIL retains this input in its signature even though the shader
            // writes literal values. Bind a dummy stream, not guest data.
            result.push_back({usage, usage_index, fetch, stride, 0, 4, true});
            continue;
        }
        Require(((w1 >> 24) & 63) == 0 && !(w1 & (1u << 15)), "Unsupported vertex scaling or rounding");
        const auto format = (w1 >> 16) & 63;
        const uint32_t components = format == 36 ? 1 : (format == 37 || format == 25) ? 2 :
                                    format == 57 ? 3 : (format == 38 || format == 6) ? 4 : 0;
        Require(components != 0, "Unsupported native vertex format");
        auto storage=VertexStorage::Float32;
        if (format==6) {
            Require(!(w1&(1u<<12)),"Signed native byte inputs are not implemented");
            storage=(w1&(1u<<13)) ? VertexStorage::UInt8 : VertexStorage::UNorm8;
        } else if (format == 25) {
            Require((w1 & 0x3000) == 0x3000, "Native short2 input requires signed integer components");
            storage = VertexStorage::SInt16;
        }
        const uint64_t offset = uint64_t((w2 >> 8) & 0x7fffff) * 4;
        const VertexInput input{usage, usage_index, fetch, stride, uint32_t(offset), components, false, storage};
        Require(offset + input.ByteSize() <= stride, "Vertex element exceeds its stride");
        result.push_back(input);
    }
    Require(!result.empty(), "Vertex shader has no supported inputs");
    return result;
}
std::vector<uint8_t> NativeVertexBytes(std::span<const uint8_t> bytes, uint32_t endian) {
    Require(endian <= 3 && bytes.size() % 4 == 0, "Invalid vertex buffer byte order or alignment");
    std::vector<uint8_t> result(bytes.begin(), bytes.end());
    for (size_t i = 0; i < bytes.size(); i += 4) {
        // 0 none, 1 swap bytes within 16-bit words, 2 reverse 32-bit words,
        // 3 swap the two 16-bit halves. This also preserves packed attributes.
        const uint32_t mask = endian == 1 ? 1 : endian == 2 ? 3 : endian == 3 ? 2 : 0;
        for (size_t b = 0; b < 4; ++b) result[i + b] = bytes[i + (b ^ mask)];
    }
    return result;
}
}
