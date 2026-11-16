#include "native_device.h"
#include <plume_d3d12.h>
#include <d3d12sdklayers.h>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace mc2::native {
NativeDevice::NativeDevice(bool enable_debug) {
    ID3D12Debug* debug = nullptr;
    debug_enabled = enable_debug && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    if (debug_enabled) { debug->EnableDebugLayer(); debug->Release(); }
    render_interface = std::make_unique<plume::D3D12Interface>();
    device = render_interface->createDevice("");
    if (!device) throw std::runtime_error("D3D12 device creation failed");
    queue = device->createCommandQueue(plume::RenderCommandListType::DIRECT);
    if (!queue) throw std::runtime_error("D3D12 queue creation failed");
    commands = queue->createCommandList();
    fence = device->createCommandFence();
    if (!queue || !commands || !fence) throw std::runtime_error("D3D12 queue creation failed");
}
NativeDevice::~NativeDevice() = default;

uint64_t NativeDevice::Complete() {
    queue->waitForCommandFence(fence.get());
    auto* d3d = static_cast<plume::D3D12Device*>(device.get())->d3d;
    if (FAILED(d3d->GetDeviceRemovedReason())) throw std::runtime_error("Native D3D12 device was removed");
    ID3D12InfoQueue* messages = nullptr;
    bool errors = false;
    if (SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&messages)))) {
        for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0;
            if (FAILED(messages->GetMessage(i, nullptr, &size))) continue;
            std::vector<uint8_t> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(messages->GetMessage(i, message, &size))) continue;
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
                std::cerr << "D3D12: " << message->pDescription << '\n';
            errors |= message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
        }
        messages->ClearStoredMessages();
        messages->Release();
    }
    if (errors) throw std::runtime_error("D3D12 validation reported an error");
    return ++completed;
}
}
