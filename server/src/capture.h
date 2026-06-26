// DXGI Desktop Duplication: captura el escritorio a nivel driver (GPU) y expone
// el frame BGRA en memoria CPU via una staging texture.
#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>

struct CapturedFrame {
    const uint8_t* bgra = nullptr; // B,G,R,A por pixel (apunta a la staging mapeada)
    int width = 0;
    int height = 0;
    int rowPitch = 0;              // bytes por fila (OJO: puede ser > width*4)
    bool valid = false;            // false => no hubo frame nuevo (pantalla estatica)
};

class DesktopCapture {
public:
    DesktopCapture() = default;
    ~DesktopCapture();

    // Inicializa D3D11 + duplicacion sobre el output indicado (0 = monitor primario).
    bool init(int outputIndex = 0);

    // Adquiere el siguiente frame.
    //  - Devuelve true y out.valid=true si hay frame nuevo (staging mapeada).
    //  - Devuelve true y out.valid=false si expiro el timeout (nada cambio).
    //  - Devuelve false ante error fatal recuperable (se intenta reinit la prox vez).
    // Tras usar `out` hay que llamar a endFrame().
    bool capture(CapturedFrame& out, uint32_t timeoutMs = 16);

    // Libera el mapeo y el frame de la duplicacion. Seguro llamarlo siempre.
    void endFrame();

    int desktopWidth()  const { return descW_; }
    int desktopHeight() const { return descH_; }

private:
    bool initDuplication();
    bool ensureStaging(int w, int h);

    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D11Device>            device_;
    ComPtr<ID3D11DeviceContext>     ctx_;
    ComPtr<IDXGIOutput1>            output1_;
    ComPtr<IDXGIOutputDuplication> dupl_;
    ComPtr<ID3D11Texture2D>        staging_;

    int  outputIndex_ = 0;
    int  descW_ = 0, descH_ = 0;  // tamano del escritorio
    bool frameHeld_ = false;      // hay un AcquireNextFrame pendiente de ReleaseFrame
    bool mapped_   = false;       // staging mapeada pendiente de Unmap
};
