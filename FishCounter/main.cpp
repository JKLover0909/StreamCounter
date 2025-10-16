// main.cpp : Enumerate all currently connected USB devices on Windows.
// Build: Console x64, Unicode, C++17/20
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <iostream>
#include <vector>
#include <string>
#include <regex>
#include <io.h>
#include <fcntl.h>
#include <thread>
#include <atomic>
#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
// Link with SetupAPI, Cfgmgr32, and Media Foundation
#pragma comment(lib, "SetupAPI.lib")
#pragma comment(lib, "Cfgmgr32.lib")
#pragma comment(lib, "Mfplat.lib")
#pragma comment(lib, "Mf.lib")
#pragma comment(lib, "Mfreadwrite.lib")
#pragma comment(lib, "Mfuuid.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

// GUID for all USB device interfaces (device-level)
DEFINE_GUID(GUID_DEVINTERFACE_USB_DEVICE,
    0xA5DCBF10L, 0x6530, 0x11D2,
    0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED);

// Global variables cho livestream
struct LivestreamContext {
    HWND hwnd = nullptr;
    IMFSourceReader* pReader = nullptr;
    std::atomic<bool> isRunning{ false };
    UINT32 videoWidth = 0;
    UINT32 videoHeight = 0;
    BITMAPINFO bitmapInfo = {};
    std::vector<BYTE> frameBuffer;
    CRITICAL_SECTION cs;
};

LivestreamContext g_livestreamCtx;

// Thiết lập console để hiển thị Unicode
void SetupConsole()
{
    // Thiết lập console code page
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    
    // Bật Virtual Terminal Processing để hỗ trợ ANSI escape codes
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, dwMode);
}

static std::wstring GetRegProperty(HDEVINFO infoSet, SP_DEVINFO_DATA& devInfo, DWORD property)
{
    DWORD dataType = 0;
    WCHAR buffer[4096];
    if (SetupDiGetDeviceRegistryPropertyW(infoSet, &devInfo, property, &dataType,
        reinterpret_cast<PBYTE>(buffer), sizeof(buffer), nullptr))
    {
        return std::wstring(buffer);
    }
    return L"";
}

static std::vector<std::wstring> GetMultiSzRegProperty(HDEVINFO infoSet, SP_DEVINFO_DATA& devInfo, DWORD property)
{
    std::vector<std::wstring> result;
    DWORD dataType = 0;
    WCHAR buffer[8192] = {};
    if (SetupDiGetDeviceRegistryPropertyW(infoSet, &devInfo, property, &dataType,
        reinterpret_cast<PBYTE>(buffer), sizeof(buffer), nullptr))
    {
        // MULTI_SZ: chuỗi kết thúc kép '\0\0'
        const WCHAR* p = buffer;
        while (*p)
        {
            std::wstring s = p;
            result.push_back(s);
            p += s.size() + 1;
        }
    }
    return result;
}

static void ParseVidPidFromHardwareIds(const std::vector<std::wstring>& hwids, std::wstring& vid, std::wstring& pid)
{
    // Mẫu điển hình: "USB\\VID_046D&PID_C52B&REV_1201"
    std::wregex reVid(L"VID_([0-9A-Fa-f]{4})");
    std::wregex rePid(L"PID_([0-9A-Fa-f]{4})");
    for (const auto& h : hwids)
    {
        std::wsmatch m;
        if (std::regex_search(h, m, reVid) && m.size() > 1) vid = m[1].str();
        if (std::regex_search(h, m, rePid) && m.size() > 1) pid = m[1].str();
        if (!vid.empty() && !pid.empty()) return;
    }
}

// Liệt kê tất cả các thiết bị video capture (camera)
HRESULT EnumerateVideoCaptureDevices(std::vector<IMFActivate*>& devices)
{
    IMFAttributes* pAttributes = nullptr;
    HRESULT hr = MFCreateAttributes(&pAttributes, 1);
    
    if (SUCCEEDED(hr))
    {
        hr = pAttributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID
        );
    }

    IMFActivate** ppDevices = nullptr;
    UINT32 count = 0;

    if (SUCCEEDED(hr))
    {
        hr = MFEnumDeviceSources(pAttributes, &ppDevices, &count);
    }

    if (SUCCEEDED(hr))
    {
        for (UINT32 i = 0; i < count; i++)
        {
            devices.push_back(ppDevices[i]);
        }
        CoTaskMemFree(ppDevices);
    }

    if (pAttributes)
    {
        pAttributes->Release();
    }

    return hr;
}

// Lấy thông tin về camera
void GetCameraInfo(IMFActivate* pActivate, std::wstring& friendlyName, std::wstring& symbolicLink)
{
    WCHAR* szFriendlyName = nullptr;
    UINT32 cchName = 0;

    HRESULT hr = pActivate->GetAllocatedString(
        MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
        &szFriendlyName, &cchName
    );

    if (SUCCEEDED(hr))
    {
        friendlyName = szFriendlyName;
        CoTaskMemFree(szFriendlyName);
    }

    WCHAR* szSymbolicLink = nullptr;
    UINT32 cchLink = 0;

    hr = pActivate->GetAllocatedString(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
        &szSymbolicLink, &cchLink
    );

    if (SUCCEEDED(hr))
    {
        symbolicLink = szSymbolicLink;
        CoTaskMemFree(szSymbolicLink);
    }
}

// Kiểm tra xem camera có phải là thiết bị 32E6:9221 không
bool IsTargetCamera(const std::wstring& symbolicLink, const std::wstring& targetVid, const std::wstring& targetPid)
{
    std::wstring linkUpper = symbolicLink;
    for (auto& c : linkUpper) c = towupper(c);

    std::wstring searchVid = L"VID_" + targetVid;
    std::wstring searchPid = L"PID_" + targetPid;

    for (auto& c : searchVid) c = towupper(c);
    for (auto& c : searchPid) c = towupper(c);

    return (linkUpper.find(searchVid) != std::wstring::npos &&
            linkUpper.find(searchPid) != std::wstring::npos);
}

// Chuyển đổi NV12 sang RGB24
void ConvertNV12ToRGB24(const BYTE* nv12Data, BYTE* rgbData, UINT32 width, UINT32 height)
{
    const BYTE* yPlane = nv12Data;
    const BYTE* uvPlane = nv12Data + (width * height);

    for (UINT32 y = 0; y < height; y++)
    {
        for (UINT32 x = 0; x < width; x++)
        {
            int yIndex = y * width + x;
            int uvIndex = (y / 2) * width + (x & ~1);

            int Y = yPlane[yIndex];
            int U = uvPlane[uvIndex] - 128;
            int V = uvPlane[uvIndex + 1] - 128;

            // YUV to RGB conversion
            int R = Y + (1.370705 * V);
            int G = Y - (0.337633 * U) - (0.698001 * V);
            int B = Y + (1.732446 * U);

            // Clamp values to 0-255
            R = (R < 0) ? 0 : (R > 255 ? 255 : R);
            G = (G < 0) ? 0 : (G > 255 ? 255 : G);
            B = (B < 0) ? 0 : (B > 255 ? 255 : B);

            // BGR format for Windows DIB
            int rgbIndex = (height - 1 - y) * width * 3 + x * 3;
            rgbData[rgbIndex + 0] = (BYTE)B;
            rgbData[rgbIndex + 1] = (BYTE)G;
            rgbData[rgbIndex + 2] = (BYTE)R;
        }
    }
}

// Window procedure để xử lý sự kiện
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        EnterCriticalSection(&g_livestreamCtx.cs);

        if (!g_livestreamCtx.frameBuffer.empty())
        {
            StretchDIBits(hdc,
                0, 0, g_livestreamCtx.videoWidth, g_livestreamCtx.videoHeight,
                0, 0, g_livestreamCtx.videoWidth, g_livestreamCtx.videoHeight,
                g_livestreamCtx.frameBuffer.data(),
                &g_livestreamCtx.bitmapInfo,
                DIB_RGB_COLORS,
                SRCCOPY);
        }

        LeaveCriticalSection(&g_livestreamCtx.cs);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CLOSE:
        g_livestreamCtx.isRunning = false;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// Thread để đọc frame từ camera
void CaptureThread()
{
    wprintf(L"[Thread] Bat dau capture thread...\n");

    while (g_livestreamCtx.isRunning)
    {
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        IMFSample* pSample = nullptr;

        HRESULT hr = g_livestreamCtx.pReader->ReadSample(
            (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
            0,
            &streamIndex,
            &flags,
            &timestamp,
            &pSample
        );

        if (SUCCEEDED(hr) && pSample)
        {
            IMFMediaBuffer* pBuffer = nullptr;
            hr = pSample->ConvertToContiguousBuffer(&pBuffer);

            if (SUCCEEDED(hr))
            {
                BYTE* pData = nullptr;
                DWORD dataLength = 0;

                hr = pBuffer->Lock(&pData, nullptr, &dataLength);

                if (SUCCEEDED(hr))
                {
                    EnterCriticalSection(&g_livestreamCtx.cs);

                    // Chuyển đổi NV12 sang RGB24
                    ConvertNV12ToRGB24(pData, g_livestreamCtx.frameBuffer.data(),
                        g_livestreamCtx.videoWidth, g_livestreamCtx.videoHeight);

                    LeaveCriticalSection(&g_livestreamCtx.cs);

                    pBuffer->Unlock();

                    // Cập nhật cửa sổ
                    InvalidateRect(g_livestreamCtx.hwnd, nullptr, FALSE);
                }

                pBuffer->Release();
            }

            pSample->Release();
        }
        else if (flags & MF_SOURCE_READERF_ERROR)
        {
            wprintf(L"[Thread] Loi khi doc frame\n");
            break;
        }

        Sleep(33); // ~30 FPS
    }

    wprintf(L"[Thread] Ket thuc capture thread\n");
}

// Hiển thị livestream
HRESULT ShowLivestream(IMFActivate* pActivate)
{
    wprintf(L"\n=== BAT DAU LIVESTREAM ===\n");
    wprintf(L"Dang mo camera...\n");

    // Tạo media source
    IMFMediaSource* pSource = nullptr;
    HRESULT hr = pActivate->ActivateObject(IID_PPV_ARGS(&pSource));

    if (FAILED(hr))
    {
        wprintf(L"Khong the kich hoat camera. Error: 0x%08X\n", hr);
        return hr;
    }

    // Tạo Source Reader
    IMFSourceReader* pReader = nullptr;
    IMFAttributes* pAttributes = nullptr;

    hr = MFCreateAttributes(&pAttributes, 2);
    if (SUCCEEDED(hr))
    {
        pAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        pAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }

    if (SUCCEEDED(hr))
    {
        hr = MFCreateSourceReaderFromMediaSource(pSource, pAttributes, &pReader);
    }

    if (FAILED(hr))
    {
        wprintf(L"Khong the tao Source Reader. Error: 0x%08X\n", hr);
        if (pAttributes) pAttributes->Release();
        pSource->Release();
        return hr;
    }

    // Cấu hình output format là RGB24
    IMFMediaType* pType = nullptr;
    hr = MFCreateMediaType(&pType);
    if (SUCCEEDED(hr))
    {
        pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB24);
        hr = pReader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType);
        pType->Release();
    }

    // Lấy kích thước video
    IMFMediaType* pCurrentType = nullptr;
    hr = pReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrentType);

    if (SUCCEEDED(hr))
    {
        MFGetAttributeSize(pCurrentType, MF_MT_FRAME_SIZE, &g_livestreamCtx.videoWidth, &g_livestreamCtx.videoHeight);
        pCurrentType->Release();

        wprintf(L"Kich thuoc video: %dx%d\n", g_livestreamCtx.videoWidth, g_livestreamCtx.videoHeight);
    }

    // Khởi tạo bitmap info
    ZeroMemory(&g_livestreamCtx.bitmapInfo, sizeof(BITMAPINFO));
    g_livestreamCtx.bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_livestreamCtx.bitmapInfo.bmiHeader.biWidth = g_livestreamCtx.videoWidth;
    g_livestreamCtx.bitmapInfo.bmiHeader.biHeight = g_livestreamCtx.videoHeight;
    g_livestreamCtx.bitmapInfo.bmiHeader.biPlanes = 1;
    g_livestreamCtx.bitmapInfo.bmiHeader.biBitCount = 24;
    g_livestreamCtx.bitmapInfo.bmiHeader.biCompression = BI_RGB;

    // Cấp phát buffer cho frame
    g_livestreamCtx.frameBuffer.resize(g_livestreamCtx.videoWidth * g_livestreamCtx.videoHeight * 3);

    g_livestreamCtx.pReader = pReader;
    InitializeCriticalSection(&g_livestreamCtx.cs);

    // Tạo window class
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"CameraLivestream";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);

    RegisterClassW(&wc);

    // Tạo window
    g_livestreamCtx.hwnd = CreateWindowExW(
        0,
        L"CameraLivestream",
        L"USB Camera Livestream - 32E6:9221",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        g_livestreamCtx.videoWidth + 16, g_livestreamCtx.videoHeight + 39,
        nullptr, nullptr,
        GetModuleHandle(nullptr),
        nullptr
    );

    if (!g_livestreamCtx.hwnd)
    {
        wprintf(L"Khong the tao window!\n");
        pReader->Release();
        pSource->Release();
        return E_FAIL;
    }

    wprintf(L"Da tao window livestream thanh cong!\n");
    wprintf(L"Nhan X tren cua so de dong livestream.\n\n");

    // Bắt đầu capture thread
    g_livestreamCtx.isRunning = true;
    std::thread captureThread(CaptureThread);

    // Message loop
    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Cleanup
    g_livestreamCtx.isRunning = false;
    if (captureThread.joinable())
    {
        captureThread.join();
    }

    DeleteCriticalSection(&g_livestreamCtx.cs);

    if (pReader) pReader->Release();
    if (pAttributes) pAttributes->Release();
    if (pSource) pSource->Release();

    wprintf(L"\nDa dong livestream.\n");

    return S_OK;
}

int wmain()
{
    // Thiết lập console
    SetupConsole();

    wprintf(L"===============================================================\n");
    wprintf(L"  USB CAMERA LIVESTREAM VIEWER - Camera 32E6:9221\n");
    wprintf(L"===============================================================\n\n");

    // Khởi tạo COM
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr))
    {
        wprintf(L"Khong the khoi tao COM. Error: 0x%08X\n", hr);
        wprintf(L"Nhan Enter de thoat...\n");
        getchar();
        return 1;
    }

    wprintf(L"[OK] Da khoi tao COM thanh cong\n");

    // Khởi tạo Media Foundation
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr))
    {
        wprintf(L"Khong the khoi tao Media Foundation. Error: 0x%08X\n", hr);
        CoUninitialize();
        wprintf(L"Nhan Enter de thoat...\n");
        getchar();
        return 1;
    }

    wprintf(L"[OK] Da khoi tao Media Foundation thanh cong\n");
    wprintf(L"\n[*] Dang quet cac thiet bi camera USB...\n\n");

    // Liệt kê tất cả camera
    std::vector<IMFActivate*> devices;
    hr = EnumerateVideoCaptureDevices(devices);

    if (FAILED(hr) || devices.empty())
    {
        wprintf(L"Khong tim thay camera nao!\n");
        MFShutdown();
        CoUninitialize();
        wprintf(L"\nNhan Enter de thoat...\n");
        getchar();
        return 1;
    }

    wprintf(L"Tim thay %zu camera:\n\n", devices.size());

    // Tìm camera 32E6:9221
    IMFActivate* targetCamera = nullptr;
    int targetIndex = -1;

    for (size_t i = 0; i < devices.size(); i++)
    {
        std::wstring friendlyName, symbolicLink;
        GetCameraInfo(devices[i], friendlyName, symbolicLink);

        wprintf(L"+-- Camera #%zu -------------------------------------\n", (i + 1));
        wprintf(L"| Ten: %s\n", friendlyName.c_str());

        if (IsTargetCamera(symbolicLink, L"32E6", L"9221"))
        {
            wprintf(L"| >>> CAMERA MUC TIEU (32E6:9221) <<<\n");
            targetCamera = devices[i];
            targetIndex = static_cast<int>(i);
        }

        wprintf(L"+----------------------------------------------------\n\n");
    }

    // Hiển thị livestream
    if (targetCamera)
    {
        ShowLivestream(targetCamera);
    }
    else
    {
        wprintf(L"Khong tim thay camera 32E6:9221\n");
        wprintf(L"Nhap so thu tu camera de xem (1-%zu), hoac 0 de thoat: ", devices.size());
        int choice = 0;
        if (scanf_s("%d", &choice) == 1 && choice > 0 && choice <= static_cast<int>(devices.size()))
        {
            ShowLivestream(devices[choice - 1]);
        }
    }

    // Cleanup
    for (auto device : devices)
    {
        device->Release();
    }

    MFShutdown();
    CoUninitialize();

    return 0;
}
