#pragma once
#include "native_quad.h"
#include <filesystem>
struct PPCContext;
namespace mc2::native {
struct MovieShaders {
    std::vector<uint8_t> vs_reference, ps_reference, declaration_reference, vs_dxil, ps_dxil;
};
struct MovieSnapshot {
    QuadDraw draw;
    std::vector<uint8_t> device, guest_vertices;
};
MovieShaders LoadMovieShaders(const std::filesystem::path& directory);
// Value-owned inputs for the verified movie draw family. Rejects other layouts.
MovieSnapshot ReadMovieDraw(const PPCContext& context, uint8_t* base, const MovieShaders& shaders);
}
