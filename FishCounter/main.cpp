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
#include <fstream>
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

// Global variables cho YOLO inference
struct YOLOConfig {
    cv::dnn::Net net;
    std::vector<std::string> classNames;
    int inputSize = 640;
    float confThreshold = 0.5f;
    float nmsThreshold = 0.45f;
    bool isLoaded = false;
};

YOLOConfig g_yoloConfig;
std::atomic<int> g_personCount{ 0 };
std::atomic<int> g_frameCounter{ 0 };
const int g_inferenceSkipFrames = 3; // Chạy inference mỗi 3 frame

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
// Tải model YOLO và class names
bool LoadModel(const std::string& modelPath, const std::string& classNamesPath)
{
    try
    {
        wprintf(L"[YOLO] Dang tai model: %hs\n", modelPath.c_str());
        g_yoloConfig.net = cv::dnn::readNetFromONNX(modelPath);
        
        if (g_yoloConfig.net.empty())
        {
            wprintf(L"[YOLO] Loi: Khong the tai model!\n");
            return false;
        }

        // Cau hinh backend (CPU by default, CUDA neu co)
        g_yoloConfig.net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        g_yoloConfig.net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
        // Neu co GPU: g_yoloConfig.net.setPreferableBackend(cv::dnn::DNN_BACKEND_CUDA);
        // g_yoloConfig.net.setPreferableTarget(cv::dnn::DNN_TARGET_CUDA);

        wprintf(L"[YOLO] Da tai model thanh cong!\n");

        // Tai class names
        std::ifstream ifs(classNamesPath);
        if (!ifs.is_open())
        {
            wprintf(L"[YOLO] Loi: Khong the mo file class names: %hs\n", classNamesPath.c_str());
            return false;
        }

        std::string line;
        while (std::getline(ifs, line))
        {
            if (!line.empty())
                g_yoloConfig.classNames.push_back(line);
        }
        ifs.close();

        wprintf(L"[YOLO] Da tai %zu class names\n", g_yoloConfig.classNames.size());
        g_yoloConfig.isLoaded = true;

        return true;
    }
    catch (const std::exception& e)
    {
        wprintf(L"[YOLO] Exception: %hs\n", e.what());
        return false;
    }
}

// Inference va dem nguoi
int RunInferenceAndCountPeople(cv::Mat& frame)
{
    if (!g_yoloConfig.isLoaded)
        return 0;

    try
    {
        // Resize va tao blob
        cv::Mat blob = cv::dnn::blobFromImage(
            frame,
            1.0 / 255.0,
            cv::Size(g_yoloConfig.inputSize, g_yoloConfig.inputSize),
            cv::Scalar(0, 0, 0),
            true,
            false
        );

        g_yoloConfig.net.setInput(blob);

        // Forward
        std::vector<cv::Mat> outputs;
        std::vector<std::string> outNames = g_yoloConfig.net.getUnconnectedOutLayersNames();
        g_yoloConfig.net.forward(outputs, outNames);

        // Parse outputs
        std::vector<int> classIds;
        std::vector<float> confidences;
        std::vector<cv::Rect> boxes;

        float scaleX = (float)frame.cols / g_yoloConfig.inputSize;
        float scaleY = (float)frame.rows / g_yoloConfig.inputSize;

        for (const auto& output : outputs)
        {
            const float* data = (float*)output.data;
            int rows = output.rows;
            int cols = output.cols;

            for (int i = 0; i < rows; ++i)
            {
                cv::Mat scores = output.row(i).colRange(4, output.cols);
                cv::Point classIdPoint;
                double confidence;
                minMaxLoc(scores, nullptr, &confidence, nullptr, &classIdPoint);

                if (confidence > g_yoloConfig.confThreshold)
                {
                    int centerX = (int)(data[i * cols + 0] * scaleX);
                    int centerY = (int)(data[i * cols + 1] * scaleY);
                    int width = (int)(data[i * cols + 2] * scaleX);
                    int height = (int)(data[i * cols + 3] * scaleY);
                    int left = centerX - width / 2;
                    int top = centerY - height / 2;

                    classIds.push_back(classIdPoint.x);
                    confidences.push_back((float)confidence);
                    boxes.push_back(cv::Rect(left, top, width, height));
                }
            }
        }

        // NMS
        std::vector<int> indices;
        cv::dnn::NMSBoxes(boxes, confidences, g_yoloConfig.confThreshold, g_yoloConfig.nmsThreshold, indices);

        // Dem nguoi (class 0)
        int personCount = 0;
        for (int idx : indices)
        {
            if (classIds[idx] == 0) // class 0 = person
            {
                personCount++;
            }
        }

        return personCount;
    }
    catch (const std::exception& e)
    {
        wprintf(L"[YOLO] Inference error: %hs\n", e.what());
        return 0;
    }
}

// Helper function: Convert NV12 to BGR using OpenCV (for fallback if RGB24 fails)
void ConvertNV12ToBGR(const BYTE* nv12Data, cv::Mat& bgrFrame, UINT32 width, UINT32 height)
{
    cv::Mat nv12(height + height / 2, width, CV_8UC1, (BYTE*)nv12Data);
    cv::cvtColor(nv12, bgrFrame, cv::COLOR_YUV2BGR_NV12);
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

    cv::Mat displayFrame;
    static int frameCount = 0;
    static bool firstFrame = true;

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
                    frameCount++;
                    
                    // Log chi tiet lan dau
                    if (firstFrame)
                    {
                        wprintf(L"[Thread] LAM DAU NHAN FRAME:\n");
                        wprintf(L"  - Data length: %u bytes\n", dataLength);
                        wprintf(L"  - NV12 size: %u bytes (width:%u * height:%u * 1.5)\n", 
                            g_livestreamCtx.videoWidth * g_livestreamCtx.videoHeight * 3 / 2,
                            g_livestreamCtx.videoWidth, g_livestreamCtx.videoHeight);
                        wprintf(L"  - Frame count: %d\n", frameCount);
                        
                        // Detect format: neu data length ~ height*width*1.5 thi NV12
                        UINT32 nv12Size = g_livestreamCtx.videoWidth * g_livestreamCtx.videoHeight * 3 / 2;
                        if (abs((int)dataLength - (int)nv12Size) < 1024)
                        {
                            wprintf(L"  - FORMAT: NV12 (camera return NV12, khong phai RGB24!)\n");
                        }
                        firstFrame = false;
                    }

                    // CONVERT NV12 -> BGR
                    cv::Mat nv12(g_livestreamCtx.videoHeight + g_livestreamCtx.videoHeight / 2, 
                                 g_livestreamCtx.videoWidth, CV_8UC1, pData);
                    cv::Mat bgrFrame;
                    cv::cvtColor(nv12, bgrFrame, cv::COLOR_YUV2BGR_NV12);
                    displayFrame = bgrFrame;

                    pBuffer->Unlock();

                    // Inference moi N frame
                    int currentFrame = g_frameCounter.fetch_add(1);
                    if (currentFrame % g_inferenceSkipFrames == 0 && g_yoloConfig.isLoaded)
                    {
                        int personCount = RunInferenceAndCountPeople(displayFrame);
                        g_personCount.store(personCount);
                        
                        if (currentFrame % 30 == 0)
                        {
                            wprintf(L"[Thread] Inference: %d people detected\n", personCount);
                        }
                    }

                    int personCount = g_personCount.load();

                    // Ve overlay
                    cv::Rect overlayRect(10, 10, 200, 50);
                    cv::Mat overlay = displayFrame.clone();
                    cv::rectangle(overlay, overlayRect, cv::Scalar(0, 0, 0), -1);
                    cv::addWeighted(overlay, 0.6, displayFrame, 0.4, 0, displayFrame);

                    std::string countText = "People: " + std::to_string(personCount);
                    cv::putText(displayFrame, countText, cv::Point(20, 45),
                        cv::FONT_HERSHEY_SIMPLEX, 1.2, cv::Scalar(0, 255, 255), 2);

                    // Convert BGR -> RGB24 de copy vao frameBuffer
                    cv::Mat rgbFrame;
                    cv::cvtColor(displayFrame, rgbFrame, cv::COLOR_BGR2RGB);

                    // Copy vao frameBuffer
                    EnterCriticalSection(&g_livestreamCtx.cs);

                    int stride = (g_livestreamCtx.videoWidth * 3 + 3) & ~3;

                    if (g_livestreamCtx.frameBuffer.size() >= stride * g_livestreamCtx.videoHeight)
                    {
                        for (UINT32 y = 0; y < g_livestreamCtx.videoHeight; y++)
                        {
                            BYTE* srcRow = rgbFrame.data + y * rgbFrame.step;
                            BYTE* dstRow = g_livestreamCtx.frameBuffer.data() + y * stride;
                            memcpy(dstRow, srcRow, g_livestreamCtx.videoWidth * 3);
                        }
                        
                        if (frameCount % 30 == 0)
                        {
                            wprintf(L"[Thread] Frame #%d copied to buffer\n", frameCount);
                        }
                    }

                    LeaveCriticalSection(&g_livestreamCtx.cs);

                    InvalidateRect(g_livestreamCtx.hwnd, nullptr, FALSE);
                }
                else
                {
                    wprintf(L"[Thread] ERROR: Lock buffer failed: 0x%08X\n", hr);
                }

                pBuffer->Release();
            }
            else
            {
                wprintf(L"[Thread] ERROR: ConvertToContiguousBuffer failed: 0x%08X\n", hr);
            }

            pSample->Release();
        }
        else if (flags & MF_SOURCE_READERF_ERROR)
        {
            wprintf(L"[Thread] ERROR: ReadSample error\n");
            break;
        }

        Sleep(33);
    }

    wprintf(L"[Thread] Ket thuc. Tong frame: %d\n", frameCount);
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

    // Khởi tạo bitmap info (top-down DIB: biHeight phải âm)
    ZeroMemory(&g_livestreamCtx.bitmapInfo, sizeof(BITMAPINFO));
    g_livestreamCtx.bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_livestreamCtx.bitmapInfo.bmiHeader.biWidth = (LONG)g_livestreamCtx.videoWidth;
    g_livestreamCtx.bitmapInfo.bmiHeader.biHeight = -((LONG)g_livestreamCtx.videoHeight);  // Âm = top-down (không đảo)
    g_livestreamCtx.bitmapInfo.bmiHeader.biPlanes = 1;
    g_livestreamCtx.bitmapInfo.bmiHeader.biBitCount = 24;
    g_livestreamCtx.bitmapInfo.bmiHeader.biCompression = BI_RGB;

    // Cấp phát buffer cho frame (align stride to 4 bytes)
    int stride = (g_livestreamCtx.videoWidth * 3 + 3) & ~3;
    g_livestreamCtx.frameBuffer.resize(stride * g_livestreamCtx.videoHeight);

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

    // Tai model YOLO (tim file relative voi executable hoac absolute)
    wprintf(L"\n[*] Dang tai model YOLO...\n");
    
    // Cac duong dan co the (theo thu tu uu tien)
    std::vector<std::string> modelPaths = {
        "AIStuff/yolov8n.onnx",           // Relative: executable/../AIStuff
        "../AIStuff/yolov8n.onnx",        // Relative: build/../AIStuff
        "../../AIStuff/yolov8n.onnx",     // Relative: build/Release/../../AIStuff
    };
    
    std::vector<std::string> classNamesPaths = {
        "AIStuff/coco.names",
        "../AIStuff/coco.names",
        "../../AIStuff/coco.names",
    };
    
    bool modelLoaded = false;
    for (size_t i = 0; i < modelPaths.size(); i++)
    {
        if (LoadModel(modelPaths[i], classNamesPaths[i]))
        {
            modelLoaded = true;
            break;
        }
    }
    
    if (!modelLoaded)
    {
        wprintf(L"[WARNING] Khong the tai model YOLO. Livestream se khong deem nguoi!\n");
    }

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
