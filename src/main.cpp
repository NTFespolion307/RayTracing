// main.cpp - entry point. Sets Per-Monitor-V2 DPI awareness, initializes COM
// for the native file dialogs, and runs the application. All Vulkan loading is
// dynamic via volk; if vulkan-1.dll or a device is missing the App shows one
// friendly message box and exits cleanly.
#include "ui.h"
#include <windows.h>
#include <objbase.h>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Per-Monitor-V2 DPI awareness so the UI is crisp on high-DPI displays.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // COM for IFileDialog (open/save/folder pickers).
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool comReady = SUCCEEDED(hr);

    bool selftest = false;
    {
        LPWSTR cmd = GetCommandLineW();
        if (cmd && wcsstr(cmd, L"--selftest")) selftest = true;
    }

    App app;
    int rc = selftest ? app.runSelfTest() : app.run();

    if (comReady) CoUninitialize();
    return rc;
}
