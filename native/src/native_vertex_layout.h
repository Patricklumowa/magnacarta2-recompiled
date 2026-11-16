#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace mc2::native {
enum class VertexStorage { Float32, UInt8, UNorm8 };
struct VertexInput {
    uint32_t usage, usage_index, fetch_index, stride, offset, components;
    bool constant_only = false;
    VertexStorage storage = VertexStorage::Float32;
    bool operator==(const VertexInput&) const = default;
};
// Decode verified float and unsigned-byte inputs. Mini fetches inherit their
// full fetch's stream/stride, including when it writes constants only.
// Unsupported addressing/format/scaling fails before any host buffer is read.
std::vector<VertexInput> ReadVertexInputs(std::span<const uint8_t> container);
const char* VertexSemantic(uint32_t usage);
// Convert a complete guest buffer with its fetch descriptor's byte order.
std::vector<uint8_t> NativeVertexBytes(std::span<const uint8_t> bytes, uint32_t endian);
}
