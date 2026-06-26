#include "capture.h"
#include <cstdio>

using Microsoft::WRL::ComPtr;

DesktopCapture::~DesktopCapture() {
    endFrame();
}

bool DesktopCapture::init(int outputIndex) {
    outputIndex_ = outputIndex;

    // Creamos el device sobre el adaptador que posee el output (evita fallos en
    // equipos con GPU hibrida si se eligiera el adaptador equivocado).
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        fprintf(stderr, "[capture] CreateDXGIFactory1 fallo\n");
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapters1(0, &adapter))) {
        fprintf(stderr, "[capture] EnumAdapters1(0) fallo\n");
        return false;
    }

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(
        adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
        levels, (UINT)(sizeof(levels) / sizeof(levels[0])),
        D3D11_SDK_VERSION, &device_, &got, &ctx_);
    if (FAILED(hr)) {
        fprintf(stderr, "[capture] D3D11CreateDevice fallo (0x%08lX)\n", hr);
        return false;
    }

    return initDuplication();
}

bool DesktopCapture::initDuplication() {
    output1_.Reset();
    dupl_.Reset();

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device_.As(&dxgiDevice))) return false;

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;

    ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(outputIndex_, &output))) {
        fprintf(stderr, "[capture] EnumOutputs(%d) fallo\n", outputIndex_);
        return false;
    }

    DXGI_OUTPUT_DESC odesc{};
    output->GetDesc(&odesc);
    descW_ = odesc.DesktopCoordinates.right  - odesc.DesktopCoordinates.left;
    descH_ = odesc.DesktopCoordinates.bottom - odesc.DesktopCoordinates.top;

    if (FAILED(output.As(&output1_))) return false;

    HRESULT hr = output1_->DuplicateOutput(device_.Get(), &dupl_);
    if (FAILED(hr)) {
        fprintf(stderr, "[capture] DuplicateOutput fallo (0x%08lX)\n", hr);
        return false;
    }
    fprintf(stderr, "[capture] escritorio %dx%d listo\n", descW_, descH_);
    return true;
}

bool DesktopCapture::ensureStaging(int w, int h) {
    if (staging_) {
        D3D11_TEXTURE2D_DESC d{};
        staging_->GetDesc(&d);
        if ((int)d.Width == w && (int)d.Height == h) return true;
        staging_.Reset();
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &staging_));
}

bool DesktopCapture::capture(CapturedFrame& out, uint32_t timeoutMs) {
    out = CapturedFrame{};

    if (!dupl_) {
        // Intento de recuperacion (p.ej. tras ACCESS_LOST).
        if (!initDuplication()) return false;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> res;
    HRESULT hr = dupl_->AcquireNextFrame(timeoutMs, &info, &res);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // Sin cambios en pantalla: no hay frame que liberar.
        out.valid = false;
        return true;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        // Cambio de modo / UAC / lock screen: hay que recrear la duplicacion.
        dupl_.Reset();
        return false;
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[capture] AcquireNextFrame fallo (0x%08lX)\n", hr);
        return false;
    }
    frameHeld_ = true;

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(res.As(&tex))) { endFrame(); return false; }

    D3D11_TEXTURE2D_DESC tdesc{};
    tex->GetDesc(&tdesc);
    if (!ensureStaging((int)tdesc.Width, (int)tdesc.Height)) { endFrame(); return false; }

    // Copia GPU->staging y mapeo a CPU.
    ctx_->CopyResource(staging_.Get(), tex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        endFrame();
        return false;
    }
    mapped_ = true;

    out.bgra     = static_cast<const uint8_t*>(mapped.pData);
    out.width    = (int)tdesc.Width;
    out.height   = (int)tdesc.Height;
    out.rowPitch = (int)mapped.RowPitch;
    out.valid    = true;
    return true;
}

void DesktopCapture::endFrame() {
    if (mapped_) {
        ctx_->Unmap(staging_.Get(), 0);
        mapped_ = false;
    }
    if (frameHeld_ && dupl_) {
        dupl_->ReleaseFrame();
        frameHeld_ = false;
    }
}
