#pragma once
#include <rex/system/interfaces/graphics.h>
#include <memory>
struct PPCContext;
namespace mc2::native {
bool NativeBootRequested();
bool NativeBootActive();
std::unique_ptr<rex::system::IGraphicsSystem> CreateNativeBootGraphics();
void SetNativeBootWindow(void* window);
void QueueNativeMovie(const PPCContext& context, uint8_t* base);
[[noreturn]] void UnsupportedNativeOperation(const char* operation, const PPCContext& context);
}
