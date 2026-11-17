#include "overlay_d3d12.h"

#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kBufferCount = 2;
constexpr UINT kInstanceCapacity = 16384;
constexpr UINT kInstanceStride = 40;
const wchar_t kWindowClass[] = L"WardogOverlayWindow";

const char kShaderSource[] = R"(
cbuffer Params : register(b0)
{
    float2 g_viewport;
};

struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

VSOutput VSMain(uint vertex_id : SV_VertexID,
                float2 center : INST_CENTER,
                float2 halfsize : INST_HALF,
                float2 angle_pad : INST_ANGLE,
                float4 color : INST_COLOR)
{
    float2 corners[6] =
    {
        float2(-0.5, -0.5), float2(0.5, -0.5), float2(-0.5, 0.5),
        float2(0.5, -0.5), float2(0.5, 0.5), float2(-0.5, 0.5)
    };
    float2 corner = corners[vertex_id];
    float s = sin(angle_pad.x);
    float c = cos(angle_pad.x);
    float2 local = corner * (halfsize * 2.0);
    float2 rotated = float2(local.x * c - local.y * s, local.x * s + local.y * c);
    float2 pixel = center + rotated;
    VSOutput output;
    output.position = float4(pixel.x / g_viewport.x * 2.0 - 1.0,
                             1.0 - pixel.y / g_viewport.y * 2.0, 0.0, 1.0);
    output.color = float4(color.rgb * color.a, color.a);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return input.color;
}
)";

std::string HexValue(long value) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(value));
    return buffer;
}

std::string FailureText(const char* what, HRESULT hr) {
    std::string text(what);
    text += " failed: ";
    text += HexValue(static_cast<long>(hr));
    return text;
}

bool SetDpiAwareness() {
    using Fn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return false;
    auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
    if (!fn) return false;
    return fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != 0;
}

D3D12_RESOURCE_DESC BufferDescription(UINT64 size) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

D3D12_HEAP_PROPERTIES UploadHeap() {
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_UPLOAD;
    props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    props.CreationNodeMask = 1;
    props.VisibleNodeMask = 1;
    return props;
}

D3D12_HEAP_PROPERTIES ReadbackHeap() {
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_READBACK;
    props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    props.CreationNodeMask = 1;
    props.VisibleNodeMask = 1;
    return props;
}

bool WriteBmp32(const std::wstring& path, const void* pixels, UINT width, UINT height, UINT row_pitch) {
    if (!pixels || width == 0 || height == 0) return false;
    const UINT row_bytes = width * 4;
    const UINT image_bytes = row_bytes * height;
    const UINT header_bytes = 14 + 40;
    std::vector<unsigned char> header(header_bytes, 0);
    header[0] = 'B';
    header[1] = 'M';
    const UINT file_bytes = header_bytes + image_bytes;
    std::memcpy(&header[2], &file_bytes, 4);
    const UINT data_offset = header_bytes;
    std::memcpy(&header[10], &data_offset, 4);
    const UINT info_size = 40;
    std::memcpy(&header[14], &info_size, 4);
    const LONG width_value = static_cast<LONG>(width);
    std::memcpy(&header[18], &width_value, 4);
    const LONG height_value = -static_cast<LONG>(height);
    std::memcpy(&header[22], &height_value, 4);
    const unsigned short planes = 1;
    std::memcpy(&header[26], &planes, 2);
    const unsigned short bits = 32;
    std::memcpy(&header[28], &bits, 2);
    std::memcpy(&header[34], &image_bytes, 4);

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, header.data(), header_bytes, &written, nullptr) != 0;
    const unsigned char* row = static_cast<const unsigned char*>(pixels);
    for (UINT y = 0; ok && y < height; ++y) {
        ok = WriteFile(file, row + static_cast<size_t>(y) * row_pitch, row_bytes, &written, nullptr) != 0;
    }
    CloseHandle(file);
    return ok;
}

}  

struct D3D12Overlay::Impl {
    HWND hwnd = nullptr;
    HINSTANCE instance = nullptr;
    int width = 0;
    int height = 0;
    bool vsync = true;
    bool visible = true;

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd_list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    UINT64 fence_value = 0;

    ComPtr<IDXGIFactory2> dxgi_factory;
    ComPtr<IDXGIAdapter1> dxgi_adapter;
    ComPtr<IDXGISwapChain1> swapchain;
    ComPtr<IDXGISwapChain3> swapchain3;
    UINT frame_index = 0;
    ComPtr<IDCompositionDevice> comp_device;
    ComPtr<IDCompositionTarget> comp_target;
    ComPtr<IDCompositionVisual> comp_visual;

    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    UINT rtv_stride = 0;
    ComPtr<ID3D12Resource> buffers[kBufferCount];

    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pipeline;

    ComPtr<ID3D12Resource> quad_buffer;
    D3D12_VERTEX_BUFFER_VIEW quad_view{};
    ComPtr<ID3D12Resource> instance_buffer;
    D3D12_VERTEX_BUFFER_VIEW instance_view{};
    void* instance_mapped = nullptr;

    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};

    std::wstring capture_path;
    std::wstring capture_written;
    ComPtr<ID3D12Resource> capture_buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT capture_footprint{};
    UINT capture_width = 0;
    UINT capture_height = 0;
    bool capture_saved = false;
    std::string capture_note;

    std::string adapter_name;
    std::string shader_note;

    void Destroy();
    bool RegisterWindowClass(std::string* error);
    bool CreateWindowHandle(std::string* error);
    bool CreateDevice(std::string* error);
    bool CreateTargets(std::string* error);
    bool CreateSwapChain(int new_width, int new_height, std::string* error);
    bool CreateRendering(std::string* error);
    bool CreateBuffers(std::string* error);
    bool CreateAttachments(std::string* error);
    bool Resize(int new_width, int new_height, std::string* error);
    void WaitForGpu();
    bool EnsureCaptureBuffer(std::string* error);
    bool SaveCapture();
    bool Present(const DrawList& list, std::string* error);
};

void D3D12Overlay::Impl::Destroy() {
    if (comp_target) comp_target.Reset();
    if (comp_visual) comp_visual.Reset();
    if (comp_device) comp_device.Reset();
    if (swapchain) swapchain.Reset();
    if (swapchain3) swapchain3.Reset();
    for (UINT i = 0; i < kBufferCount; ++i) buffers[i].Reset();
    if (hwnd) {
        DestroyWindow(hwnd);
        hwnd = nullptr;
    }
    if (fence_event) {
        CloseHandle(fence_event);
        fence_event = nullptr;
    }
}

bool D3D12Overlay::Impl::RegisterWindowClass(std::string* error) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    if (RegisterClassExW(&wc)) return true;
    const DWORD last = GetLastError();
    if (last == ERROR_CLASS_ALREADY_EXISTS) return true;
    if (error) *error = "RegisterClassExW failed: " + std::to_string(last);
    return false;
}

bool D3D12Overlay::Impl::CreateWindowHandle(std::string* error) {
    const DWORD ex_style = WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
                           WS_EX_NOREDIRECTIONBITMAP;
    hwnd = CreateWindowExW(ex_style, kWindowClass, L"WardogOverlay", WS_POPUP, 0, 0, 256, 256, nullptr, nullptr,
                           instance, nullptr);
    if (!hwnd) {
        if (error) *error = "CreateWindowExW failed: " + std::to_string(GetLastError());
        return false;
    }
    return true;
}

bool D3D12Overlay::Impl::CreateDevice(std::string* error) {
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&dxgi_factory));
    if (FAILED(hr)) {
        if (error) *error = FailureText("CreateDXGIFactory2", hr);
        return false;
    }

    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(dxgi_factory.As(&factory6))) {
        hr = factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                  IID_PPV_ARGS(&dxgi_adapter));
    }
    if (!dxgi_adapter) hr = dxgi_factory->EnumAdapters1(0, &dxgi_adapter);
    if (!dxgi_adapter) {
        if (error) *error = FailureText("EnumAdapters1", hr);
        return false;
    }

    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(dxgi_adapter->GetDesc1(&desc))) {
        char name[256]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
        adapter_name = name;
    }

    hr = D3D12CreateDevice(dxgi_adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        if (error) *error = FailureText("D3D12CreateDevice", hr);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) {
        if (error) *error = FailureText("CreateCommandQueue", hr);
        return false;
    }
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(hr)) {
        if (error) *error = FailureText("CreateCommandAllocator", hr);
        return false;
    }
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                   IID_PPV_ARGS(&cmd_list));
    if (FAILED(hr)) {
        if (error) *error = FailureText("CreateCommandList", hr);
        return false;
    }
    cmd_list->Close();

    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) {
        if (error) *error = FailureText("CreateFence", hr);
        return false;
    }
    fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event) {
        if (error) *error = "CreateEventW failed";
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = kBufferCount;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap));
    if (FAILED(hr)) {
        if (error) *error = FailureText("CreateDescriptorHeap", hr);
        return false;
    }
    rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return true;
}

bool D3D12Overlay::Impl::CreateSwapChain(int new_width, int new_height, std::string* error) {
    if (!dxgi_factory || !dxgi_adapter) {
        if (error) *error = "DXGI factory or adapter missing";
        return false;
    }
    HRESULT hr = S_OK;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(new_width);
    desc.Height = static_cast<UINT>(new_height);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kBufferCount;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    desc.Flags = 0;

    hr = dxgi_factory->CreateSwapChainForComposition(queue.Get(), &desc, nullptr, &swapchain);
    if (SUCCEEDED(hr)) swapchain.As(&swapchain3);
    if (FAILED(hr)) {
        if (error) *error = FailureText("CreateSwapChainForComposition", hr);
        return false;
    }

    ComPtr<ID3D11Device> d3d11;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    hr = D3D11CreateDevice(dxgi_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                           &d3d11, &level, nullptr);
    if (FAILED(hr)) {
        if (error) *error = FailureText("D3D11CreateDevice", hr);
        return false;
    }
    ComPtr<IDXGIDevice> compose_device_source;
    hr = d3d11.As(&compose_device_source);
    if (FAILED(hr)) {
        if (error) *error = FailureText("QueryInterface(IDXGIDevice) composition", hr);
        return false;
    }
    hr = DCompositionCreateDevice(compose_device_source.Get(), IID_PPV_ARGS(&comp_device));
    if (FAILED(hr)) {
        if (error) *error = FailureText("DCompositionCreateDevice", hr);
        return false;
    }
    hr = comp_device->CreateTargetForHwnd(hwnd, TRUE, &comp_target);
    if (FAILED(hr)) {
        if (error) *error = FailureText("CreateTargetForHwnd", hr);
        return false;
    }
    hr = comp_device->CreateVisual(&comp_visual);
    if (FAILED(hr)) {
        if (error) *error = FailureText("CreateVisual", hr);
        return false;
    }
    hr = comp_visual->SetContent(swapchain.Get());
    if (FAILED(hr)) {
        if (error) *error = FailureText("SetContent", hr);
        return false;
    }
    hr = comp_target->SetRoot(comp_visual.Get());
    if (FAILED(hr)) {
        if (error) *error = FailureText("SetRoot", hr);
        return false;
    }
    comp_device->Commit();
    return true;
}

bool D3D12Overlay::Impl::CreateTargets(std::string* error) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kBufferCount; ++i) {
        buffers[i].Reset();
        const HRESULT hr = swapchain->GetBuffer(i, IID_PPV_ARGS(&buffers[i]));
        if (FAILED(hr)) {
            if (error) *error = FailureText("GetBuffer", hr);
            return false;
        }
        device->CreateRenderTargetView(buffers[i].Get(), nullptr, handle);
        handle.ptr += rtv_stride;
    }
    return true;
}

bool D3D12Overlay::Impl::CreateRendering(std::string* error) {
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameter.Constants.ShaderRegister = 0;
    parameter.Constants.RegisterSpace = 0;
    parameter.Constants.Num32BitValues = 2;

    D3D12_ROOT_SIGNATURE_DESC signature_desc{};
    signature_desc.NumParameters = 1;
    signature_desc.pParameters = &parameter;
    signature_desc.NumStaticSamplers = 0;
    signature_desc.pStaticSamplers = nullptr;
    signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> signature_error;
    HRESULT hr = D3D12SerializeRootSignature(&signature_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature,
                                             &signature_error);
    if (FAILED(hr)) {
        if (error) {
            *error = FailureText("D3D12SerializeRootSignature", hr);
            if (signature_error) *error += std::string(": ") + static_cast<const char*>(signature_error->GetBufferPointer());
        }
        return false;
    }
    hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                     IID_PPV_ARGS(&root_signature));
    if (FAILED(hr)) {
        if (error) *error = FailureText("CreateRootSignature", hr);
        return false;
    }

    ComPtr<ID3DBlob> vs;
    ComPtr<ID3DBlob> ps;
    ComPtr<ID3DBlob> compile_error;
    hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "wardog_overlay.hlsl", nullptr, nullptr, "VSMain",
                    "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &compile_error);
    if (FAILED(hr)) {
        if (error) {
            *error = FailureText("D3DCompile(VSMain)", hr);
            if (compile_error) *error += std::string(": ") + static_cast<const char*>(compile_error->GetBufferPointer());
        }
        return false;
    }
    compile_error.Reset();
    hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "wardog_overlay.hlsl", nullptr, nullptr, "PSMain",
                    "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &compile_error);
    if (FAILED(hr)) {
        if (error) {
            *error = FailureText("D3DCompile(PSMain)", hr);
            if (compile_error) *error += std::string(": ") + static_cast<const char*>(compile_error->GetBufferPointer());
        }
        return false;
    }
    shader_note = "runtime_d3dcompile";

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"INST_CENTER", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INST_HALF", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 8, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INST_ANGLE", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
        {"INST_COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 24, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root_signature.Get();
    desc.VS.pShaderBytecode = vs->GetBufferPointer();
    desc.VS.BytecodeLength = vs->GetBufferSize();
    desc.PS.pShaderBytecode = ps->GetBufferPointer();
    desc.PS.BytecodeLength = ps->GetBufferSize();
    desc.InputLayout.pInputElementDescs = layout;
    desc.InputLayout.NumElements = _countof(layout);
    desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    desc.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
    desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthBias = 0;
    desc.RasterizerState.DepthBiasClamp = 0.0f;
    desc.RasterizerState.SlopeScaledDepthBias = 0.0f;
    desc.RasterizerState.DepthClipEnable = FALSE;
    desc.RasterizerState.MultisampleEnable = FALSE;
    desc.RasterizerState.AntialiasedLineEnable = FALSE;
    desc.RasterizerState.ForcedSampleCount = 0;
    desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    desc.NodeMask = 0;
    desc.CachedPSO.pCachedBlob = nullptr;
    desc.CachedPSO.CachedBlobSizeInBytes = 0;
    desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
    if (FAILED(hr)) {
        if (error) *error = FailureText("CreateGraphicsPipelineState", hr);
        return false;
    }
    return true;
}

bool D3D12Overlay::Impl::CreateBuffers(std::string* error) {
    const D3D12_HEAP_PROPERTIES upload = UploadHeap();
    const D3D12_RESOURCE_DESC quad_desc = BufferDescription(6 * sizeof(float) * 2);
    HRESULT hr = device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &quad_desc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&quad_buffer));
    if (FAILED(hr)) {
        if (error) *error = FailureText("CreateCommittedResource(quad)", hr);
        return false;
    }
    const float corners[12] = {-0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f,
                               0.5f,  -0.5f, 0.5f, 0.5f,  -0.5f, 0.5f};
    void* mapped = nullptr;
    D3D12_RANGE read_range{0, 0};
    hr = quad_buffer->Map(0, &read_range, &mapped);
    if (FAILED(hr) || !mapped) {
        if (error) *error = FailureText("Map(quad)", hr);
        return false;
    }
    std::memcpy(mapped, corners, sizeof(corners));
    quad_buffer->Unmap(0, nullptr);
    quad_view.BufferLocation = quad_buffer->GetGPUVirtualAddress();
    quad_view.SizeInBytes = static_cast<UINT>(sizeof(corners));
    quad_view.StrideInBytes = sizeof(float) * 2;

    const D3D12_RESOURCE_DESC instance_desc =
        BufferDescription(static_cast<UINT64>(kInstanceCapacity) * kInstanceStride);
    hr = device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &instance_desc,
                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                         IID_PPV_ARGS(&instance_buffer));
    if (FAILED(hr)) {
        if (error) *error = FailureText("CreateCommittedResource(instances)", hr);
        return false;
    }
    D3D12_RANGE instance_range{0, 0};
    hr = instance_buffer->Map(0, &instance_range, &instance_mapped);
    if (FAILED(hr) || !instance_mapped) {
        if (error) *error = FailureText("Map(instances)", hr);
        return false;
    }
    instance_view.BufferLocation = instance_buffer->GetGPUVirtualAddress();
    instance_view.SizeInBytes = kInstanceCapacity * kInstanceStride;
    instance_view.StrideInBytes = kInstanceStride;
    return true;
}

void D3D12Overlay::Impl::WaitForGpu() {
    if (!queue || !fence || !fence_event) return;
    const UINT64 target = ++fence_value;
    if (FAILED(queue->Signal(fence.Get(), target))) return;
    if (fence->GetCompletedValue() >= target) return;
    if (FAILED(fence->SetEventOnCompletion(target, fence_event))) return;
    WaitForSingleObject(fence_event, 2000);
}

bool D3D12Overlay::Impl::Resize(int new_width, int new_height, std::string* error) {
    if (new_width < 16) new_width = 16;
    if (new_height < 16) new_height = 16;
    if (swapchain && new_width == width && new_height == height) return true;
    WaitForGpu();
    for (UINT i = 0; i < kBufferCount; ++i) buffers[i].Reset();
    if (!swapchain) {
        if (!CreateSwapChain(new_width, new_height, error)) return false;
    } else {
        const HRESULT hr = swapchain->ResizeBuffers(0, static_cast<UINT>(new_width),
                                                    static_cast<UINT>(new_height), DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            if (error) *error = FailureText("ResizeBuffers", hr);
            return false;
        }
    }
    width = new_width;
    height = new_height;
    if (!CreateTargets(error)) return false;
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    scissor.left = 0;
    scissor.top = 0;
    scissor.right = static_cast<LONG>(width);
    scissor.bottom = static_cast<LONG>(height);
    return true;
}

bool D3D12Overlay::Impl::EnsureCaptureBuffer(std::string* error) {
    if (!buffers[0]) {
        if (error) *error = "no back buffer to capture";
        return false;
    }
    if (capture_buffer && capture_width == static_cast<UINT>(width) &&
        capture_height == static_cast<UINT>(height)) {
        return true;
    }
    capture_buffer.Reset();
    const D3D12_RESOURCE_DESC texture = buffers[0]->GetDesc();
    UINT rows = 0;
    UINT64 row_size = 0;
    UINT64 total = 0;
    capture_footprint = D3D12_PLACED_SUBRESOURCE_FOOTPRINT{};
    device->GetCopyableFootprints(&texture, 0, 1, 0, &capture_footprint, &rows, &row_size, &total);
    if (total == 0) {
        if (error) *error = "capture footprint is empty";
        return false;
    }
    const D3D12_HEAP_PROPERTIES readback = ReadbackHeap();
    const D3D12_RESOURCE_DESC desc = BufferDescription(total);
    const HRESULT hr = device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&capture_buffer));
    if (FAILED(hr)) {
        if (error) *error = FailureText("CreateCommittedResource(capture)", hr);
        return false;
    }
    capture_width = static_cast<UINT>(width);
    capture_height = static_cast<UINT>(height);
    return true;
}

bool D3D12Overlay::Impl::SaveCapture() {
    if (!capture_buffer) {
        capture_note = "capture buffer missing";
        return false;
    }
    const D3D12_RANGE range{0, static_cast<SIZE_T>(capture_footprint.Footprint.RowPitch) * capture_height};
    void* mapped = nullptr;
    if (FAILED(capture_buffer->Map(0, &range, &mapped)) || !mapped) {
        capture_note = "map failed";
        capture_path.clear();
        return false;
    }
    const bool ok = WriteBmp32(capture_path, mapped, capture_width, capture_height,
                               capture_footprint.Footprint.RowPitch);
    capture_buffer->Unmap(0, nullptr);
    capture_note = ok ? "saved" : "write failed";
    if (ok) {
        capture_saved = true;
        capture_written = capture_path;
    }
    capture_path.clear();
    return ok;
}

bool D3D12Overlay::Impl::Present(const DrawList& list, std::string* error) {
    if (!swapchain || !cmd_list || !instance_mapped) {
        if (error) *error = "overlay is not ready";
        return false;
    }
    WaitForGpu();

    UINT count = static_cast<UINT>(list.quads.size());
    if (count > kInstanceCapacity) count = kInstanceCapacity;
    auto* destination = static_cast<float*>(instance_mapped);
    for (UINT i = 0; i < count; ++i) {
        const DrawQuad& quad = list.quads[i];
        float* slot = destination + static_cast<size_t>(i) * 10u;
        slot[0] = quad.cx;
        slot[1] = quad.cy;
        slot[2] = quad.hw;
        slot[3] = quad.hh;
        slot[4] = quad.angle;
        slot[5] = quad.pad;
        slot[6] = quad.r;
        slot[7] = quad.g;
        slot[8] = quad.b;
        slot[9] = quad.a;
    }

    if (swapchain3) {
        frame_index = swapchain3->GetCurrentBackBufferIndex();
    } else {
        frame_index = (frame_index + 1u) % kBufferCount;
    }
    const UINT index = frame_index;
    HRESULT hr = allocator->Reset();
    if (FAILED(hr)) {
        if (error) *error = FailureText("Reset(allocator)", hr);
        return false;
    }
    hr = cmd_list->Reset(allocator.Get(), nullptr);
    if (FAILED(hr)) {
        if (error) *error = FailureText("Reset(cmdlist)", hr);
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(index) * rtv_stride;
    cmd_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    cmd_list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    cmd_list->RSSetViewports(1, &viewport);
    cmd_list->RSSetScissorRects(1, &scissor);

    if (count > 0) {
        const float dimensions[2] = {static_cast<float>(width), static_cast<float>(height)};
        cmd_list->SetGraphicsRootSignature(root_signature.Get());
        cmd_list->SetPipelineState(pipeline.Get());
        cmd_list->SetGraphicsRoot32BitConstants(0, 2, dimensions, 0);
        cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd_list->IASetVertexBuffers(0, 1, &quad_view);
        cmd_list->IASetVertexBuffers(1, 1, &instance_view);
        cmd_list->DrawInstanced(6, count, 0, 0);
    }

    const bool want_capture = !capture_path.empty();
    if (want_capture) {
        if (!EnsureCaptureBuffer(error)) return false;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = buffers[index].Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cmd_list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION copy_destination{};
        copy_destination.pResource = capture_buffer.Get();
        copy_destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        copy_destination.PlacedFootprint = capture_footprint;
        D3D12_TEXTURE_COPY_LOCATION copy_source{};
        copy_source.pResource = buffers[index].Get();
        copy_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        copy_source.SubresourceIndex = 0;
        cmd_list->CopyTextureRegion(&copy_destination, 0, 0, 0, &copy_source, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cmd_list->ResourceBarrier(1, &barrier);
    }

    hr = cmd_list->Close();
    if (FAILED(hr)) {
        if (error) *error = FailureText("Close(cmdlist)", hr);
        return false;
    }
    ID3D12CommandList* lists[] = {cmd_list.Get()};
    queue->ExecuteCommandLists(1, lists);
    hr = swapchain->Present(vsync ? 1 : 0, 0);
    if (FAILED(hr)) {
        if (error) *error = FailureText("Present", hr);
        return false;
    }
    if (vsync) DwmFlush();
    WaitForGpu();
    if (want_capture && !SaveCapture()) {
        if (error) *error = capture_note;
        return false;
    }
    return true;
}

D3D12Overlay::D3D12Overlay() : impl_(new Impl()) {}

D3D12Overlay::~D3D12Overlay() {
    Shutdown();
    delete impl_;
    impl_ = nullptr;
}

bool D3D12Overlay::Create(bool vsync, std::string* error) {
    if (!impl_) return false;
    impl_->vsync = vsync;
    impl_->instance = GetModuleHandleW(nullptr);
    SetDpiAwareness();
    if (!impl_->RegisterWindowClass(error)) return false;
    if (!impl_->CreateWindowHandle(error)) return false;
    if (!impl_->CreateDevice(error)) return false;
    if (!impl_->Resize(320, 180, error)) return false;
    if (!impl_->CreateRendering(error)) return false;
    if (!impl_->CreateBuffers(error)) return false;
    return true;
}

bool D3D12Overlay::Attach(HWND target, std::string* error) {
    if (!impl_ || !impl_->hwnd) {
        if (error) *error = "overlay window missing";
        return false;
    }
    RECT client{};
    if (!target || !GetClientRect(target, &client)) {
        if (error) *error = "target window has no client rect";
        return false;
    }
    POINT origin{0, 0};
    if (!ClientToScreen(target, &origin)) {
        if (error) *error = "ClientToScreen failed";
        return false;
    }
    const int new_width = client.right - client.left;
    const int new_height = client.bottom - client.top;
    if (!impl_->Resize(new_width, new_height, error)) return false;
    SetWindowPos(impl_->hwnd, HWND_TOPMOST, origin.x, origin.y, impl_->width, impl_->height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    impl_->visible = true;
    return true;
}

bool D3D12Overlay::Render(const DrawList& list, std::string* error) {
    if (!impl_) {
        if (error) *error = "overlay not created";
        return false;
    }
    if (!impl_->visible) return true;
    return impl_->Present(list, error);
}

void D3D12Overlay::SetCapturePath(const std::wstring& path) {
    if (impl_) impl_->capture_path = path;
}

bool D3D12Overlay::capture_saved() const { return impl_ && impl_->capture_saved; }

const std::string& D3D12Overlay::capture_note() const {
    static const std::string empty;
    return impl_ ? impl_->capture_note : empty;
}

void D3D12Overlay::Detach() {
    if (!impl_ || !impl_->hwnd) return;
    ShowWindow(impl_->hwnd, SW_HIDE);
    impl_->visible = false;
}

void D3D12Overlay::SetVisible(bool visible) {
    if (!impl_ || !impl_->hwnd) return;
    ShowWindow(impl_->hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    impl_->visible = visible;
}

bool D3D12Overlay::visible() const { return impl_ && impl_->visible; }

HWND D3D12Overlay::window() const { return impl_ ? impl_->hwnd : nullptr; }

int D3D12Overlay::width() const { return impl_ ? impl_->width : 0; }

int D3D12Overlay::height() const { return impl_ ? impl_->height : 0; }

const char* D3D12Overlay::present_path() const { return "composition_premultiplied"; }

const std::string& D3D12Overlay::adapter_name() const {
    static const std::string empty;
    return impl_ ? impl_->adapter_name : empty;
}

const std::string& D3D12Overlay::shader_note() const {
    static const std::string empty;
    return impl_ ? impl_->shader_note : empty;
}

void D3D12Overlay::Shutdown() {
    if (!impl_) return;
    impl_->Destroy();
    if (impl_->cmd_list) impl_->cmd_list.Reset();
    if (impl_->allocator) impl_->allocator.Reset();
    if (impl_->pipeline) impl_->pipeline.Reset();
    if (impl_->root_signature) impl_->root_signature.Reset();
    if (impl_->quad_buffer) impl_->quad_buffer.Reset();
    if (impl_->instance_buffer) impl_->instance_buffer.Reset();
    if (impl_->rtv_heap) impl_->rtv_heap.Reset();
    if (impl_->fence) impl_->fence.Reset();
    if (impl_->queue) impl_->queue.Reset();
    if (impl_->device) impl_->device.Reset();
    impl_->instance_mapped = nullptr;
}
