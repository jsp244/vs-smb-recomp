/*
 * gpu_info.c - lists the DXGI adapters and which displays each one drives.
 * A NES port wants to render on the adapter that owns the display: a frame
 * rendered on another GPU has to be copied across and composed by the desktop
 * compositor, which costs latency. Build (MSYS2 MinGW64):
 *   gcc -O2 tools/gpu_info.c -o build/gpu_info.exe -ldxgi -ldxguid -lole32
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_2.h>
#include <stdio.h>

int main(void) {
    IDXGIFactory1 *factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)) || !factory) { puts("no DXGI"); return 1; }
    for (UINT a = 0;; a++) {
        IDXGIAdapter1 *adapter = NULL;
        if (IDXGIFactory1_EnumAdapters1(factory, a, &adapter) != S_OK) break;
        DXGI_ADAPTER_DESC1 d;
        IDXGIAdapter1_GetDesc1(adapter, &d);
        printf("adapter %u: %ls  luid 0x%08lX_0x%08lX  dedicated %llu MB%s\n", a, d.Description,
               (unsigned long)d.AdapterLuid.HighPart, (unsigned long)d.AdapterLuid.LowPart,
               (unsigned long long)(d.DedicatedVideoMemory >> 20), (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (software)" : "");
        for (UINT o = 0;; o++) {
            IDXGIOutput *out = NULL;
            if (IDXGIAdapter1_EnumOutputs(adapter, o, &out) != S_OK) break;
            DXGI_OUTPUT_DESC od;
            IDXGIOutput_GetDesc(out, &od);
            printf("    drives display %ls  %ldx%ld  attached=%d\n", od.DeviceName,
                   od.DesktopCoordinates.right - od.DesktopCoordinates.left, od.DesktopCoordinates.bottom - od.DesktopCoordinates.top,
                   od.AttachedToDesktop);
            IDXGIOutput_Release(out);
        }
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);
    return 0;
}
