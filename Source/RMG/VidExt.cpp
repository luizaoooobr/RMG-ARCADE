/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "VidExt.hpp"

#include <RMG-Core/Callback.hpp>
#include <RMG-Core/Directories.hpp>
#include <RMG-Core/Emulation.hpp>
#include <RMG-Core/Kaillera.hpp>
#include <RMG-Core/Netplay.hpp>
#include <RMG-Core/Settings.hpp>
#include <RMG-Core/rmgk_gekko.hpp>
#include <RMG-Core/VidExt.hpp>
#include <RMG-Core/Video.hpp>

#include "OnScreenDisplay.hpp"
#include "Utilities/QtKeyToSdl3Key.hpp"

#include <QVulkanInstance>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QApplication>
#include <QByteArray>
#include <QThread>
#include <QScreen>
#include <QKeyEvent>
#include <QTimer>

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>

#include <string>
#endif

//
// Local Variables
//

static Thread::EmulationThread* l_EmuThread             = nullptr;
static UserInterface::MainWindow* l_MainWindow          = nullptr;
static UserInterface::Widget::OGLWidget** l_OGLWidget   = nullptr;
static UserInterface::Widget::VKWidget** l_VulkanWidget = nullptr;
static QThread* l_RenderThread                          = nullptr;
static bool l_OpenGLInitialized                         = false;
static bool l_OsdInitialized                            = false;
static QSurfaceFormat l_SurfaceFormat;
static m64p_render_mode l_RenderMode;

#ifdef _WIN32
static constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
static constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
static constexpr int WGL_CONTEXT_PROFILE_MASK_ARB  = 0x9126;
static constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;
static constexpr int WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB = 0x00000002;

using PFNWGLCREATECONTEXTATTRIBSARBPROC = HGLRC(WINAPI *)(HDC, HGLRC, const int *);
using PFNWGLSWAPINTERVALEXTPROC = BOOL(WINAPI *)(int);

struct NativeWglState
{
    HWND hwnd = nullptr;
    HDC hdc = nullptr;
    HGLRC context = nullptr;
    int width = 0;
    int height = 0;
    bool displayModeChanged = false;
    std::wstring displayDevice;
};

static NativeWglState l_NativeWgl;
static bool l_NativeWglStopRequested = false;
static bool l_NativeWglRefocusRequested = false;

static bool VidExt_NativeWglShouldIgnoreAltEnter(void)
{
    return CoreHasInitNetplay() ||
        (CoreHasInitKaillera() && !CoreIsKailleraPlaybackMode());
}

static LRESULT CALLBACK VidExt_NativeWglWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CLOSE:
        l_NativeWglStopRequested = true;
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
        if (GetForegroundWindow() != hwnd)
        {
            BringWindowToTop(hwnd);
            SetForegroundWindow(hwnd);
            SetActiveWindow(hwnd);
            SetFocus(hwnd);
        }
        return 0;
    case WM_ACTIVATEAPP:
        if (wParam != FALSE)
        {
            l_NativeWglRefocusRequested = true;
        }
        else
        {
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE || wParam == VK_F12)
        {
            l_NativeWglStopRequested = true;
            return 0;
        }
        break;
    case WM_SYSKEYDOWN:
        if (wParam == VK_RETURN)
        {
            if (VidExt_NativeWglShouldIgnoreAltEnter())
            {
                return 0;
            }
            l_NativeWglStopRequested = true;
            return 0;
        }
        break;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static std::wstring VidExt_Utf8ToWide(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }

    int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0)
    {
        return {};
    }

    std::wstring wide(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    return wide;
}

static bool VidExt_NativeWglRegisterClass(void)
{
    static bool registered = false;
    if (registered)
    {
        return true;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = VidExt_NativeWglWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"RMGKNativeWglFullscreen";

    if (RegisterClassExW(&wc) == 0)
    {
        return false;
    }

    registered = true;
    return true;
}

static RECT VidExt_NativeWglTargetRect(LPCWSTR deviceName)
{
    DEVMODEW devmode = {};
    devmode.dmSize = sizeof(devmode);
    if (EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &devmode))
    {
        return {
            devmode.dmPosition.x,
            devmode.dmPosition.y,
            devmode.dmPosition.x + static_cast<LONG>(devmode.dmPelsWidth),
            devmode.dmPosition.y + static_cast<LONG>(devmode.dmPelsHeight)
        };
    }

    return {
        0,
        0,
        GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN)
    };
}

static bool VidExt_NativeWglApplyDisplayMode(LPCWSTR deviceName)
{
    std::string resolution = CoreSettingsGetStringValue(SettingsID::GUI_ExclusiveFullscreenResolution);
    int refreshRate = CoreSettingsGetIntValue(SettingsID::GUI_ExclusiveFullscreenRefreshRate);
    if (resolution.empty() && refreshRate == 0)
    {
        return true;
    }

    DEVMODEW devmode = {};
    devmode.dmSize = sizeof(devmode);
    if (!EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &devmode))
    {
        return false;
    }

    if (!resolution.empty())
    {
        size_t xPos = resolution.find('x');
        if (xPos != std::string::npos)
        {
            devmode.dmPelsWidth  = static_cast<DWORD>(std::stoul(resolution.substr(0, xPos)));
            devmode.dmPelsHeight = static_cast<DWORD>(std::stoul(resolution.substr(xPos + 1)));
            devmode.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT;
        }
    }

    if (refreshRate > 0)
    {
        devmode.dmDisplayFrequency = static_cast<DWORD>(refreshRate);
        devmode.dmFields |= DM_DISPLAYFREQUENCY;
    }

    LONG result = (deviceName != nullptr) ?
        ChangeDisplaySettingsExW(deviceName, &devmode, nullptr, CDS_FULLSCREEN, nullptr) :
        ChangeDisplaySettingsW(&devmode, CDS_FULLSCREEN);
    if (result != DISP_CHANGE_SUCCESSFUL)
    {
        return false;
    }

    l_NativeWgl.displayModeChanged = true;
    if (deviceName != nullptr)
    {
        l_NativeWgl.displayDevice = deviceName;
    }
    return true;
}

static void VidExt_NativeWglRestoreDisplayMode(void)
{
    if (l_NativeWgl.displayModeChanged && !l_NativeWgl.displayDevice.empty())
    {
        ChangeDisplaySettingsExW(l_NativeWgl.displayDevice.c_str(), nullptr, nullptr, 0, nullptr);
    }
    else if (l_NativeWgl.displayModeChanged)
    {
        ChangeDisplaySettingsW(nullptr, 0);
    }

    l_NativeWgl.displayModeChanged = false;
    l_NativeWgl.displayDevice.clear();
}

static m64p_function VidExt_NativeWglGetProcAddress(const char* proc)
{
    PROC address = wglGetProcAddress(proc);
    if (address != nullptr)
    {
        return reinterpret_cast<m64p_function>(address);
    }

    HMODULE module = GetModuleHandleW(L"opengl32.dll");
    if (module == nullptr)
    {
        module = LoadLibraryW(L"opengl32.dll");
    }
    if (module == nullptr)
    {
        return nullptr;
    }

    return reinterpret_cast<m64p_function>(GetProcAddress(module, proc));
}

static bool VidExt_NativeWglCreateContext(HDC hdc)
{
    HGLRC legacyContext = wglCreateContext(hdc);
    if (legacyContext == nullptr || !wglMakeCurrent(hdc, legacyContext))
    {
        if (legacyContext != nullptr)
        {
            wglDeleteContext(legacyContext);
        }
        return false;
    }

    auto createContextAttribs = reinterpret_cast<PFNWGLCREATECONTEXTATTRIBSARBPROC>(
        wglGetProcAddress("wglCreateContextAttribsARB"));
    if (createContextAttribs != nullptr)
    {
        int profile = (l_SurfaceFormat.profile() == QSurfaceFormat::CoreProfile) ?
            WGL_CONTEXT_CORE_PROFILE_BIT_ARB :
            WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB;
        int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, l_SurfaceFormat.majorVersion(),
            WGL_CONTEXT_MINOR_VERSION_ARB, l_SurfaceFormat.minorVersion(),
            WGL_CONTEXT_PROFILE_MASK_ARB, profile,
            0
        };

        HGLRC context = createContextAttribs(hdc, nullptr, attribs);
        if (context != nullptr)
        {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(legacyContext);
            if (!wglMakeCurrent(hdc, context))
            {
                wglDeleteContext(context);
                return false;
            }
            l_NativeWgl.context = context;
        }
    }

    if (l_NativeWgl.context == nullptr)
    {
        l_NativeWgl.context = legacyContext;
    }

    auto swapInterval = reinterpret_cast<PFNWGLSWAPINTERVALEXTPROC>(
        wglGetProcAddress("wglSwapIntervalEXT"));
    if (swapInterval != nullptr)
    {
        swapInterval(0);
    }

    return true;
}

static void VidExt_NativeWglReassertFullscreen(void)
{
    if (l_NativeWgl.hwnd == nullptr || l_NativeWgl.hdc == nullptr || l_NativeWgl.context == nullptr)
    {
        return;
    }

    LPCWSTR deviceName = l_NativeWgl.displayDevice.empty() ? nullptr : l_NativeWgl.displayDevice.c_str();
    RECT rect = VidExt_NativeWglTargetRect(deviceName);
    SetWindowPos(l_NativeWgl.hwnd, HWND_TOPMOST,
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top,
        SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    BringWindowToTop(l_NativeWgl.hwnd);
    SetForegroundWindow(l_NativeWgl.hwnd);
    SetActiveWindow(l_NativeWgl.hwnd);
    SetFocus(l_NativeWgl.hwnd);

    if (wglMakeCurrent(l_NativeWgl.hdc, l_NativeWgl.context))
    {
        glViewport(0, 0, l_NativeWgl.width, l_NativeWgl.height);
        auto swapInterval = reinterpret_cast<PFNWGLSWAPINTERVALEXTPROC>(
            wglGetProcAddress("wglSwapIntervalEXT"));
        if (swapInterval != nullptr)
        {
            swapInterval(0);
        }
    }
}

static void VidExt_NativeWglDestroy(void)
{
    if (l_NativeWgl.context != nullptr)
    {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(l_NativeWgl.context);
        l_NativeWgl.context = nullptr;
    }

    if (l_NativeWgl.hdc != nullptr && l_NativeWgl.hwnd != nullptr)
    {
        ReleaseDC(l_NativeWgl.hwnd, l_NativeWgl.hdc);
        l_NativeWgl.hdc = nullptr;
    }

    if (l_NativeWgl.hwnd != nullptr)
    {
        DestroyWindow(l_NativeWgl.hwnd);
        l_NativeWgl.hwnd = nullptr;
    }
    l_NativeWgl.width = 0;
    l_NativeWgl.height = 0;
    l_NativeWglStopRequested = false;
    l_NativeWglRefocusRequested = false;
    qunsetenv("RMG_NATIVE_WGL_DRAWABLE_WIDTH");
    qunsetenv("RMG_NATIVE_WGL_DRAWABLE_HEIGHT");

    VidExt_NativeWglRestoreDisplayMode();
}

static void VidExt_NativeWglPollEvents(void)
{
    MSG message = {};
    while (PeekMessageW(&message, l_NativeWgl.hwnd, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (l_NativeWglStopRequested)
    {
        l_NativeWglStopRequested = false;
        CoreStopEmulation();
    }

    if (l_NativeWglRefocusRequested)
    {
        l_NativeWglRefocusRequested = false;
        VidExt_NativeWglReassertFullscreen();
    }
}

static bool VidExt_NativeWglSetup(void)
{
    if (l_NativeWgl.hwnd != nullptr)
    {
        return true;
    }

    if (!VidExt_NativeWglRegisterClass())
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to register native WGL fullscreen window class");
        return false;
    }

    std::wstring deviceNameString = VidExt_Utf8ToWide(CoreSettingsGetStringValue(SettingsID::GUI_ExclusiveFullscreenMonitor));
    LPCWSTR deviceName = deviceNameString.empty() ? nullptr : deviceNameString.c_str();
    l_NativeWgl.displayDevice = deviceNameString;
    if (!VidExt_NativeWglApplyDisplayMode(deviceName))
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Warning, "Failed to apply exclusive fullscreen display mode; using current mode");
    }

    RECT rect = VidExt_NativeWglTargetRect(deviceName);
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        L"RMGKNativeWglFullscreen",
        L"RMG-K",
        WS_POPUP | WS_VISIBLE,
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    if (hwnd == nullptr)
    {
        VidExt_NativeWglRestoreDisplayMode();
        CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to create native WGL fullscreen window");
        return false;
    }

    HDC hdc = GetDC(hwnd);
    if (hdc == nullptr)
    {
        DestroyWindow(hwnd);
        VidExt_NativeWglRestoreDisplayMode();
        CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to get native WGL fullscreen device context");
        return false;
    }

    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = static_cast<BYTE>(l_SurfaceFormat.depthBufferSize() > 0 ? l_SurfaceFormat.depthBufferSize() : 24);
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int pixelFormat = ChoosePixelFormat(hdc, &pfd);
    if (pixelFormat == 0 || !SetPixelFormat(hdc, pixelFormat, &pfd))
    {
        ReleaseDC(hwnd, hdc);
        DestroyWindow(hwnd);
        VidExt_NativeWglRestoreDisplayMode();
        CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to set native WGL fullscreen pixel format");
        return false;
    }

    l_NativeWgl.hwnd = hwnd;
    l_NativeWgl.hdc = hdc;
    RECT clientRect = {};
    if (GetClientRect(hwnd, &clientRect))
    {
        l_NativeWgl.width = static_cast<int>(clientRect.right - clientRect.left);
        l_NativeWgl.height = static_cast<int>(clientRect.bottom - clientRect.top);
        qputenv("RMG_NATIVE_WGL_DRAWABLE_WIDTH", QByteArray::number(l_NativeWgl.width));
        qputenv("RMG_NATIVE_WGL_DRAWABLE_HEIGHT", QByteArray::number(l_NativeWgl.height));
        CoreAddCallbackMessage(CoreDebugMessageType::Info,
            "Native WGL drawable size " + std::to_string(l_NativeWgl.width) + "x" + std::to_string(l_NativeWgl.height));
    }
    if (!VidExt_NativeWglCreateContext(hdc))
    {
        VidExt_NativeWglDestroy();
        CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to create native WGL fullscreen context");
        return false;
    }

    ShowWindow(hwnd, SW_SHOW);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    CoreAddCallbackMessage(CoreDebugMessageType::Info, "Using native WGL exclusive fullscreen presenter");
    return true;
}

static bool VidExt_NativeWglActive(void)
{
    return l_NativeWgl.hwnd != nullptr && l_NativeWgl.context != nullptr;
}
#endif

static QVulkanInstance l_VulkanInstance;
static QVulkanInfoVector<QVulkanExtension> l_VulkanExtensions;
static QVector<const char*> l_VulkanExtensionList;

static void VidExt_UpdateOsdDisplaySize(void)
{
    if (l_RenderMode != M64P_RENDER_OPENGL || !l_OsdInitialized || l_OGLWidget == nullptr)
    {
        return;
    }

    QOpenGLContext* context = (*l_OGLWidget)->GetContext();
    if (context == nullptr)
    {
        return;
    }

    QOpenGLFunctions* functions = context->functions();
    if (functions == nullptr)
    {
        return;
    }

    GLint viewport[4] = {};
    functions->glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] > 0 && viewport[3] > 0)
    {
        OnScreenDisplaySetDisplaySize(viewport[2], viewport[3]);
    }
}

//
// VidExt Functions
//

static bool VidExt_OglSetup(int screenMode)
{
#ifdef _WIN32
    if (screenMode == M64VIDEO_FULLSCREEN &&
        CoreSettingsGetBoolValue(SettingsID::GUI_ExclusiveFullscreen) &&
        CoreSettingsGetBoolValue(SettingsID::GUI_BetaFullscreenBackend) &&
        VidExt_NativeWglSetup())
    {
        l_OpenGLInitialized = true;
        return true;
    }
#endif

    l_EmuThread->on_VidExt_SetupOGL(l_SurfaceFormat, QThread::currentThread());

    while (!(*l_OGLWidget)->isVisible())
    {
        continue;
    }

    if (!(*l_OGLWidget)->GetContext()->isValid())
    {
        if (QSurfaceFormat::defaultFormat().renderableType() == QSurfaceFormat::OpenGLES)
        {
            CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to retrieve valid OpenGL ES context");
        }
        else
        {
            CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to retrieve valid OpenGL context");
        }
        return false;
    }

    if (!(*l_OGLWidget)->GetContext()->makeCurrent((*l_OGLWidget)))
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to make OpenGL context current");
        return false;
    }

    // Some drivers hand back a *valid* context at a lower version than requested
    // (outdated AMD/Intel drivers, Microsoft Basic Render Driver, Remote Desktop,
    // VMs). GLideN64 needs desktop OpenGL 3.3+; on a lower context it fails later
    // with an opaque PLUGIN_FAIL, so catch it here and name the actual version.
    {
        const QSurfaceFormat actualFormat = (*l_OGLWidget)->GetContext()->format();
        if (actualFormat.renderableType() != QSurfaceFormat::OpenGLES &&
            (actualFormat.majorVersion() < 3 ||
             (actualFormat.majorVersion() == 3 && actualFormat.minorVersion() < 3)))
        {
            const QString msg = QString(
                "Your GPU/driver provides only OpenGL %1.%2, but the graphics plugin "
                "needs OpenGL 3.3 or newer. Update your graphics driver — outdated AMD/Intel "
                "drivers, Remote Desktop, and virtual machines commonly cause this.")
                .arg(actualFormat.majorVersion()).arg(actualFormat.minorVersion());
            CoreAddCallbackMessage(CoreDebugMessageType::Error, msg.toUtf8().constData());
            return false;
        }
    }

    l_OpenGLInitialized = true;
    return true;
}

static m64p_error VidExt_InitWithRenderMode(m64p_render_mode RenderMode)
{
    l_RenderMode = RenderMode;
    l_RenderThread = QThread::currentThread();

    if (RenderMode == M64P_RENDER_OPENGL)
    {
        l_SurfaceFormat = QSurfaceFormat::defaultFormat();
        l_SurfaceFormat.setOption(QSurfaceFormat::DeprecatedFunctions, 1);
        l_SurfaceFormat.setDepthBufferSize(24);
        l_SurfaceFormat.setProfile(QSurfaceFormat::CompatibilityProfile);
        l_SurfaceFormat.setSwapInterval(0);
        if (l_SurfaceFormat.renderableType() != QSurfaceFormat::OpenGLES)
        {
            l_SurfaceFormat.setMajorVersion(3);
            l_SurfaceFormat.setMinorVersion(3);
        }
        else
        {
            l_SurfaceFormat.setMajorVersion(2);
            l_SurfaceFormat.setMinorVersion(0);
        }
    }

    l_EmuThread->on_VidExt_Init(RenderMode == M64P_RENDER_OPENGL ? VidExtRenderMode::OpenGL : VidExtRenderMode::Vulkan);

    return M64ERR_SUCCESS;
}

static m64p_error VidExt_Init(void)
{
    return VidExt_InitWithRenderMode(M64P_RENDER_OPENGL);
}

static m64p_error VidExt_Quit(void)
{
    OnScreenDisplayShutdown();

    if (l_RenderMode == M64P_RENDER_OPENGL)
    {
#ifdef _WIN32
        if (VidExt_NativeWglActive())
        {
            VidExt_NativeWglDestroy();
        }
        else
#endif
        {
        // move OpenGL context back to the GUI thread
        (*l_OGLWidget)->MoveContextToThread(QApplication::instance()->thread());
        }
    }
    else
    {
        // remove vulkan instance from widget
        // and destroy the instance
        (*l_VulkanWidget)->setVulkanInstance(nullptr);
        if (l_VulkanInstance.isValid())
        {
            l_VulkanInstance.destroy();
        }
    }
    l_EmuThread->on_VidExt_Quit();

    l_OpenGLInitialized = false;
    l_OsdInitialized    = false;

    return M64ERR_SUCCESS;
}

static m64p_error VidExt_ListModes(m64p_2d_size *SizeArray, int *NumSizes)
{
    return M64ERR_UNSUPPORTED;
}

static m64p_error VidExt_ListRates(m64p_2d_size Size, int *NumRates, int *Rates)
{
    return M64ERR_UNSUPPORTED;
}

static m64p_error VidExt_SetMode(int Width, int Height, int BitsPerPixel, int ScreenMode, int Flags)
{
    // initialize OpenGL when render mode
    // is OpenGL and not Vulkan
    if (l_RenderMode == M64P_RENDER_OPENGL)
    {
        if (!l_OpenGLInitialized && !VidExt_OglSetup(ScreenMode))
        {
            return M64ERR_SYSTEM_FAIL;
        }

        // try to initialize the OSD
        // when opengl 3 is used
#ifdef _WIN32
        bool nativeWglActive = VidExt_NativeWglActive();
#else
        bool nativeWglActive = false;
#endif
        if (!nativeWglActive &&
            l_SurfaceFormat.majorVersion() == 3 &&
            !l_OsdInitialized)
        {
            if (!OnScreenDisplayInit())
            {
                CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to initialize OSD");
                return M64ERR_SYSTEM_FAIL;
            }

            OnScreenDisplayLoadSettings();
            l_OsdInitialized = true;
        }
    }

    switch (ScreenMode)
    {
        default:
        case M64VIDEO_NONE:
            return M64ERR_INPUT_INVALID;
        case M64VIDEO_WINDOWED:
#ifdef _WIN32
            if (VidExt_NativeWglActive())
            {
                CoreAddCallbackMessage(CoreDebugMessageType::Info, "Leaving native WGL fullscreen by stopping emulation");
                CoreStopEmulation();
                return M64ERR_SUCCESS;
            }
#endif
            l_EmuThread->on_VidExt_SetWindowedMode(Width, Height, BitsPerPixel, Flags);
            break;
        case M64VIDEO_FULLSCREEN:
#ifdef _WIN32
            if (VidExt_NativeWglActive())
            {
                if (l_NativeWgl.width > 0 && l_NativeWgl.height > 0)
                {
                    Width = l_NativeWgl.width;
                    Height = l_NativeWgl.height;
                    glViewport(0, 0, l_NativeWgl.width, l_NativeWgl.height);
                    OnScreenDisplaySetDisplaySize(l_NativeWgl.width, l_NativeWgl.height);
                }
                break;
            }
#endif
            l_EmuThread->on_VidExt_SetFullscreenMode(Width, Height, BitsPerPixel, Flags);
            break;
    }

    OnScreenDisplaySetDisplaySize(Width, Height);
    return M64ERR_SUCCESS;
}

static m64p_error VidExt_SetModeWithRate(int Width, int Height, int RefreshRate, int BitsPerPixel, int ScreenMode, int Flags)
{
    return M64ERR_UNSUPPORTED;
}

static m64p_function VidExt_GLGetProc(const char *Proc)
{
    if (l_RenderMode != M64P_RENDER_OPENGL)
    {
        return nullptr;
    }

#ifdef _WIN32
    if (VidExt_NativeWglActive())
    {
        return VidExt_NativeWglGetProcAddress(Proc);
    }
#endif

    return (*l_OGLWidget)->GetContext()->getProcAddress(Proc);
}

static m64p_error VidExt_GLSetAttr(m64p_GLattr Attr, int Value)
{
    if (l_RenderMode != M64P_RENDER_OPENGL)
    {
        return M64ERR_INVALID_STATE;
    }

    switch (Attr)
    {
    case M64P_GL_DOUBLEBUFFER:
        if (Value == 1)
            l_SurfaceFormat.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
        else if (Value == 0)
            l_SurfaceFormat.setSwapBehavior(QSurfaceFormat::SingleBuffer);
        break;
    case M64P_GL_BUFFER_SIZE:
        break;
    case M64P_GL_DEPTH_SIZE:
        l_SurfaceFormat.setDepthBufferSize(Value);
        break;
    case M64P_GL_RED_SIZE:
        l_SurfaceFormat.setRedBufferSize(Value);
        break;
    case M64P_GL_GREEN_SIZE:
        l_SurfaceFormat.setGreenBufferSize(Value);
        break;
    case M64P_GL_BLUE_SIZE:
        l_SurfaceFormat.setBlueBufferSize(Value);
        break;
    case M64P_GL_ALPHA_SIZE:
        l_SurfaceFormat.setAlphaBufferSize(Value);
        break;
    case M64P_GL_SWAP_CONTROL: // vsync should be disabled during netplay
        l_SurfaceFormat.setSwapInterval((!CoreHasInitNetplay() && Value) ? 1 : 0);
        break;
    case M64P_GL_MULTISAMPLEBUFFERS:
        break;
    case M64P_GL_MULTISAMPLESAMPLES:
        l_SurfaceFormat.setSamples(Value);
        break;
    case M64P_GL_CONTEXT_MAJOR_VERSION:
        l_SurfaceFormat.setMajorVersion(Value);
        break;
    case M64P_GL_CONTEXT_MINOR_VERSION:
        l_SurfaceFormat.setMinorVersion(Value);
        break;
    case M64P_GL_CONTEXT_PROFILE_MASK:
        switch (Value)
        {
        case M64P_GL_CONTEXT_PROFILE_CORE:
            l_SurfaceFormat.setProfile(QSurfaceFormat::CoreProfile);
            break;
        case M64P_GL_CONTEXT_PROFILE_COMPATIBILITY:
            l_SurfaceFormat.setProfile(QSurfaceFormat::CompatibilityProfile);
            break;
        case M64P_GL_CONTEXT_PROFILE_ES:
            l_SurfaceFormat.setRenderableType(QSurfaceFormat::OpenGLES);
            break;
        }

        break;
    }

    return M64ERR_SUCCESS;
}

static m64p_error VidExt_GLGetAttr(m64p_GLattr Attr, int *pValue)
{
    if (l_RenderMode != M64P_RENDER_OPENGL)
    {
        return M64ERR_INVALID_STATE;
    }

    QSurfaceFormat::SwapBehavior SB = l_SurfaceFormat.swapBehavior();
    switch (Attr)
    {
    case M64P_GL_DOUBLEBUFFER:
        if (SB == QSurfaceFormat::SingleBuffer)
            *pValue = 0;
        else
            *pValue = 1;
        break;
    case M64P_GL_BUFFER_SIZE:
        *pValue =
            l_SurfaceFormat.alphaBufferSize() + l_SurfaceFormat.redBufferSize() + l_SurfaceFormat.greenBufferSize() + l_SurfaceFormat.blueBufferSize();
        break;
    case M64P_GL_DEPTH_SIZE:
        *pValue = l_SurfaceFormat.depthBufferSize();
        break;
    case M64P_GL_RED_SIZE:
        *pValue = l_SurfaceFormat.redBufferSize();
        break;
    case M64P_GL_GREEN_SIZE:
        *pValue = l_SurfaceFormat.greenBufferSize();
        break;
    case M64P_GL_BLUE_SIZE:
        *pValue = l_SurfaceFormat.blueBufferSize();
        break;
    case M64P_GL_ALPHA_SIZE:
        *pValue = l_SurfaceFormat.alphaBufferSize();
        break;
    case M64P_GL_SWAP_CONTROL:
        *pValue = l_SurfaceFormat.swapInterval();
        break;
    case M64P_GL_MULTISAMPLEBUFFERS:
        break;
    case M64P_GL_MULTISAMPLESAMPLES:
        *pValue = l_SurfaceFormat.samples();
        break;
    case M64P_GL_CONTEXT_MAJOR_VERSION:
        *pValue = l_SurfaceFormat.majorVersion();
        break;
    case M64P_GL_CONTEXT_MINOR_VERSION:
        *pValue = l_SurfaceFormat.minorVersion();
        break;
    case M64P_GL_CONTEXT_PROFILE_MASK:
        switch (l_SurfaceFormat.profile())
        {
        case QSurfaceFormat::CoreProfile:
            *pValue = M64P_GL_CONTEXT_PROFILE_CORE;
            break;
        case QSurfaceFormat::CompatibilityProfile:
            *pValue = M64P_GL_CONTEXT_PROFILE_COMPATIBILITY;
            break;
        case QSurfaceFormat::NoProfile:
            *pValue = M64P_GL_CONTEXT_PROFILE_COMPATIBILITY;
            break;
        }
        break;
    }
    return M64ERR_SUCCESS;
}

static m64p_error VidExt_GLSwapBuf(void)
{
    if (l_RenderMode != M64P_RENDER_OPENGL)
    {
        return M64ERR_INVALID_STATE;
    }

    if (l_RenderThread != QThread::currentThread())
    {
        return M64ERR_UNSUPPORTED;
    }

#ifdef _WIN32
    if (VidExt_NativeWglActive())
    {
        VidExt_NativeWglPollEvents();

        const auto swapBegin =
            std::chrono::steady_clock::now();

        SwapBuffers(l_NativeWgl.hdc);

        const auto swapUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() -
                swapBegin).count();

        rmgk_gekko::trace_swap_duration(
            swapUs,
            0,
            1);

        return M64ERR_SUCCESS;
    }
#endif

    VidExt_UpdateOsdDisplaySize();
    OnScreenDisplayRender();

    const auto swapBegin =
        std::chrono::steady_clock::now();

    (*l_OGLWidget)->GetContext()->swapBuffers(
        (*l_OGLWidget));

    const auto swapEnd =
        std::chrono::steady_clock::now();

    (*l_OGLWidget)->GetContext()->makeCurrent(
        (*l_OGLWidget));

    const auto makeCurrentEnd =
        std::chrono::steady_clock::now();

    const auto swapUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            swapEnd - swapBegin).count();

    const auto makeCurrentUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            makeCurrentEnd - swapEnd).count();

    rmgk_gekko::trace_swap_duration(
        swapUs,
        makeCurrentUs,
        2);

    return M64ERR_SUCCESS;
}

//
// FBNeo software-framebuffer presenter
//
// FBNeo renders into a plain 32bpp buffer instead of driving the mupen video
// plugin, so when a romset is played we bring up the same OpenGL output widget
// the plugin would have used and blit each frame ourselves. The callback runs on
// the emulation thread, which is where the render widget's context is current.
//

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif

static bool   s_FbneoPresenterUp = false;
static bool   s_FbneoPresenterLogged = false;
static bool   s_FbneoPresenterChecked = false;
static int    s_FbneoCheckReports = 0;
static bool   s_FbneoPresenterDrawChecked = false;
static unsigned s_FbneoPresenterFrames = 0;
// Display aspect ratio for the FBNeo presenter, taken from the video plugin's
// configuration (see fbneoConfiguredAspect). 0.0 means "stretch" (fill the
// viewport); otherwise it is width/height. Refreshed once per emulation.
static double   s_FbneoTargetAspect = 4.0 / 3.0;
static bool     s_FbneoAspectLoaded = false;
static GLuint s_FbneoTexture     = 0;
static int    s_FbneoTextureW    = 0;
static int    s_FbneoTextureH    = 0;

// PCM output for the FBNeo core, which produces interleaved signed 16-bit stereo
// at 48000 Hz. It is fed straight from the frame callback and handed to SDL,
// which resamples/converts to whatever the output device actually wants.
static SDL_AudioStream* s_FbneoAudioStream = nullptr;

// Netplay audio diagnostics (RMGK_FBNEO_AUDIO_TRACE=1). Writes a 1 Hz snapshot
// of the push path to "<rollback log dir>\<prefix>_audio_trace.log": pushes
// per second, samples pushed, device queue depth in bytes, how many times the
// overflow guard cleared the queue that second and the stream's frequency
// ratio. The repeat/glitch reports only happen netplay-side, so the trace
// names whether the queue starves, overflows or the ratio calibration is inert.
struct FbneoAudioTraceState
{
    bool          enabled = false;
    bool          loaded  = false;
    SDL_IOStream* out     = nullptr;
    Uint64        windowStartNs = 0;
    Uint64        windowPushes  = 0;
    Sint64        windowSamples = 0;
    Sint64        clears        = 0;
    Sint64        lastClears    = 0;
    Uint64        peakQueue     = 0;
    Sint64        peakWindowSamples = 0;
};

static FbneoAudioTraceState s_FbneoAudioTrace;
static Uint64 s_windowStartNs = 0;
static Sint64 s_windowSamples = 0;
static float  s_ratio = 1.0f;

static void FbneoAudioTraceTick(const Uint64 nowNs)
{
    if (!s_FbneoAudioTrace.enabled)
    {
        return;
    }

    const Uint64 windowNs = nowNs - s_FbneoAudioTrace.windowStartNs;
    if (windowNs < 1000000000)
    {
        return;
    }

    const double pushesPerSec =
        static_cast<double>(s_FbneoAudioTrace.windowPushes) *
        1e9 / static_cast<double>(windowNs);
    const double samplesPerSec =
        static_cast<double>(s_FbneoAudioTrace.windowSamples) *
        1e9 / static_cast<double>(windowNs);
    const Uint64 queueBytes =
        s_FbneoAudioStream != nullptr
            ? SDL_GetAudioStreamQueued(s_FbneoAudioStream)
            : 0;
    const Sint64 newClears =
        s_FbneoAudioTrace.clears - s_FbneoAudioTrace.lastClears;

    if (s_FbneoAudioTrace.out != nullptr)
    {
        char line[224];
        SDL_snprintf(line, sizeof(line),
            "t_ns=%llu fps=%d samples_s=%lld queue=%llu clears=%lld samples_win=%lld ratio=%.4f\n",
            static_cast<unsigned long long>(nowNs),
            static_cast<int>(pushesPerSec + 0.5),
            s_FbneoAudioTrace.windowSamples,
            static_cast<unsigned long long>(queueBytes),
            newClears,
            s_FbneoAudioTrace.windowSamples,
            static_cast<double>(s_ratio));
        SDL_IOprintf(s_FbneoAudioTrace.out, "%s", line);
        SDL_FlushIO(s_FbneoAudioTrace.out);
    }

    s_FbneoAudioTrace.windowStartNs = nowNs;
    s_FbneoAudioTrace.lastClears = s_FbneoAudioTrace.clears;
    s_FbneoAudioTrace.windowPushes = 0;
    s_FbneoAudioTrace.windowSamples = 0;
}

static void FbneoAudioTraceOpen()
{
    const char* dirEnv = std::getenv("RMGK_ROLLBACK_LOG_DIR");
    std::string prefix = "fbneo_audio";
    const char* prefixEnv = std::getenv("RMGK_ROLLBACK_LOG_PREFIX");
    if (prefixEnv != nullptr && prefixEnv[0] != 0)
    {
        prefix = prefixEnv;
    }
    std::filesystem::path path;
    if (dirEnv != nullptr && dirEnv[0] != 0)
    {
        path = std::filesystem::path(dirEnv) /
            (prefix + "_audio_trace.log");
    }
    else
    {
        path = prefix + "_audio_trace.log";
    }
    s_FbneoAudioTrace.out = SDL_IOFromFile(path.string().c_str(), "w");
    if (s_FbneoAudioTrace.out == nullptr)
    {
        fprintf(stderr, "[FBNEO-DBG] audio trace open failed: %s\n",
            SDL_GetError());
        fflush(stderr);
    }
}

static GLuint s_FbneoProgram = 0;
static GLuint s_FbneoVbo     = 0;
static int    s_FbneoSampler = -1;
static bool   s_FbneoEs      = false;

// Core profiles require a non-zero vertex array object bound for attribute setup
// and draws, but the VAO entry points are outside the ES2 subset exposed by
// QOpenGLFunctions, so they are resolved directly from the context.
typedef void (APIENTRY *PFN_glGenVertexArrays_t)(GLsizei, GLuint*);
typedef void (APIENTRY *PFN_glBindVertexArray_t)(GLuint);
static PFN_glGenVertexArrays_t s_pglGenVertexArrays = nullptr;
static PFN_glBindVertexArray_t s_pglBindVertexArray = nullptr;
static GLuint s_FbneoVao = 0;

// The output widget's context ends up being a desktop OpenGL core profile (the
// same one GLideN64 uses), where the fixed-function pipeline is unavailable, so
// the frame is blitted through a minimal textured-quad shader.
static bool FbneoPresenterBuildPipeline(QOpenGLContext* context)
{
    QOpenGLFunctions* f = context->functions();
    if (f == nullptr)
    {
        return false;
    }
    s_FbneoEs = context->format().renderableType() == QSurfaceFormat::OpenGLES;

    const char* vsSrc = s_FbneoEs
        ? "#version 100\n"
          "attribute vec2 aPos;\n"
          "attribute vec2 aTex;\n"
          "varying vec2 vTex;\n"
          "void main() { vTex = aTex; gl_Position = vec4(aPos, 0.0, 1.0); }\n"
        : "#version 330 core\n"
          "in vec2 aPos;\n"
          "in vec2 aTex;\n"
          "out vec2 vTex;\n"
          "void main() { vTex = aTex; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

    const char* fsSrc = s_FbneoEs
        ? "#version 100\n"
          "precision mediump float;\n"
          "varying vec2 vTex;\n"
          "uniform sampler2D uTex;\n"
          "void main() { gl_FragColor = texture2D(uTex, vTex); }\n"
        : "#version 330 core\n"
          "in vec2 vTex;\n"
          "uniform sampler2D uTex;\n"
          "out vec4 oCol;\n"
          "void main() { oCol = texture(uTex, vTex); }\n";

    auto compileShader = [f](GLenum type, const char* src) -> GLuint
    {
        GLuint shader = f->glCreateShader(type);
        f->glShaderSource(shader, 1, &src, nullptr);
        f->glCompileShader(shader);

        GLint ok = 0;
        f->glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[1024] = {0};
            GLsizei len = 0;
            f->glGetShaderInfoLog(shader, sizeof(log) - 1, &len, log);
            fprintf(stderr, "[FBNEO-DBG] shader compile failed: %s\n", log);
            fflush(stderr);
            f->glDeleteShader(shader);
            return 0;
        }
        return shader;
    };

    const GLuint vs = compileShader(GL_VERTEX_SHADER, vsSrc);
    const GLuint fs = compileShader(GL_FRAGMENT_SHADER, fsSrc);
    if (vs == 0 || fs == 0)
    {
        if (vs) f->glDeleteShader(vs);
        if (fs) f->glDeleteShader(fs);
        return false;
    }

    s_FbneoProgram = f->glCreateProgram();
    f->glAttachShader(s_FbneoProgram, vs);
    f->glAttachShader(s_FbneoProgram, fs);
    f->glBindAttribLocation(s_FbneoProgram, 0, "aPos");
    f->glBindAttribLocation(s_FbneoProgram, 1, "aTex");
    f->glLinkProgram(s_FbneoProgram);
    f->glDeleteShader(vs);
    f->glDeleteShader(fs);

    GLint linked = 0;
    f->glGetProgramiv(s_FbneoProgram, GL_LINK_STATUS, &linked);
    if (!linked)
    {
        char log[1024] = {0};
        GLsizei len = 0;
        f->glGetProgramInfoLog(s_FbneoProgram, sizeof(log) - 1, &len, log);
        fprintf(stderr, "[FBNEO-DBG] program link failed: %s\n", log);
        fflush(stderr);
        f->glDeleteProgram(s_FbneoProgram);
        s_FbneoProgram = 0;
        return false;
    }

    s_FbneoSampler = f->glGetUniformLocation(s_FbneoProgram, "uTex");
    f->glGenBuffers(1, &s_FbneoVbo);

    if (!s_FbneoEs)
    {
        s_pglGenVertexArrays = (PFN_glGenVertexArrays_t)context->getProcAddress("glGenVertexArrays");
        s_pglBindVertexArray = (PFN_glBindVertexArray_t)context->getProcAddress("glBindVertexArray");
        if (s_pglGenVertexArrays != nullptr && s_pglBindVertexArray != nullptr)
        {
            s_pglGenVertexArrays(1, &s_FbneoVao);
            s_pglBindVertexArray(s_FbneoVao);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// FBNeo input
//
// The FBNeo frontend feeds the core arcade inputs directly from the bindings
// the user configured for the arcade controls (FBNeo-style), instead of
// translating an emulated N64 controller. Bindings live as flat parallel lists
// (Names/Types/Data/ExtraData) under the settings sections
// "RMG Arcade Input - Global" and "RMG Arcade Input - <driver>", keyed by the
// driver input name (e.g. "P1 Button 1", "P1 Up", "P1 Coin 1"). The active
// device for each player is read from "RMG Arcade Device - P<n>".
//
// Keyboard state is tracked from Qt key events (the render window is a bare
// QWindow with no widget-level key handling of its own) while
// gamepads/joysticks are polled through SDL on a timer, the same way the
// input configuration dialog does. Events are observed, never consumed, so the
// normal UI hotkeys keep working.
// ---------------------------------------------------------------------------

namespace
{

constexpr int FBNEO_AXIS_PEAK  = 32767;
constexpr int FBNEO_MAX_PLAYERS = 4;

static bool fbneoTraceEnabled(void)
{
    static const bool enabled = (std::getenv("RMGK_FBNEO_TRACE") != nullptr);
    return enabled;
}

// Mirrors RMG-Input's InputType enum.
enum FbneoInputSource
{
    FbneoInputSource_Keyboard       = -1,
    FbneoInputSource_GamepadButton  = 0,
    FbneoInputSource_GamepadAxis    = 1,
    FbneoInputSource_JoystickButton = 2,
    FbneoInputSource_JoystickAxis   = 3,
    FbneoInputSource_JoystickHat    = 4,
};

// Mirrors RMG-Input's InputDeviceType enum.
enum FbneoInputDevice
{
    FbneoInputDevice_None      = 0,
    FbneoInputDevice_Automatic = 1,
    FbneoInputDevice_Keyboard  = 2,
    FbneoInputDevice_Joystick  = 3,
};

// One physical binding on an arcade input.
struct FbneoBinding
{
    int type  = 0;
    int data  = 0;
    int extra = 0;
};

// Per-player arcade device plus the bindings (input name -> bindings).
struct FbneoArcadePlayer
{
    int             device    = FbneoInputDevice_None;
    std::string     deviceName;
    std::string     devicePath;
    int             deviceNum = 0;

    SDL_Gamepad*    gamepad  = nullptr;
    SDL_Joystick*   joystick = nullptr;
    SDL_JoystickID  deviceId = 0;

    std::unordered_map<std::string, std::vector<FbneoBinding>> bindings;
};

// Loads the flat parallel binding lists for `section`, keyed by input name.
static void loadFbneoBindings(std::unordered_map<std::string, std::vector<FbneoBinding>>& out,
                              const std::string& section)
{
    out.clear();

    const std::vector<std::string> names = CoreSettingsGetStringListValue(SettingsID::FBNeo_BindingNames, section);
    const std::vector<int>         types = CoreSettingsGetIntListValue(SettingsID::FBNeo_BindingTypes, section);
    const std::vector<int>         data  = CoreSettingsGetIntListValue(SettingsID::FBNeo_BindingData, section);
    const std::vector<int>         extra = CoreSettingsGetIntListValue(SettingsID::FBNeo_BindingExtraData, section);

    const size_t count = std::min(names.size(), std::min(types.size(), std::min(data.size(), extra.size())));
    for (size_t i = 0; i < count; i++)
    {
        if (names[i].empty())
        {
            continue;
        }

        FbneoBinding binding;
        binding.type  = types[i];
        binding.data  = data[i];
        binding.extra = extra[i];
        out[names[i]].push_back(binding);
    }
}

// Evaluates a binding list (OR of every bound source) into a pressed state.
static bool evalFbneoBindings(const std::vector<FbneoBinding>& bindings, const bool* keyboardState,
                              SDL_Gamepad* gamepad, SDL_Joystick* joystick)
{
    for (const FbneoBinding& binding : bindings)
    {
        switch (binding.type)
        {
            case FbneoInputSource_Keyboard:
                if (binding.data >= 0 && binding.data < SDL_SCANCODE_COUNT && keyboardState[binding.data])
                {
                    return true;
                }
                break;
            case FbneoInputSource_GamepadButton:
                if (gamepad != nullptr && SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(binding.data)))
                {
                    return true;
                }
                break;
            case FbneoInputSource_GamepadAxis:
                if (gamepad != nullptr)
                {
                    const int value = SDL_GetGamepadAxis(gamepad, static_cast<SDL_GamepadAxis>(binding.data));
                    if (std::abs(value) >= (FBNEO_AXIS_PEAK / 2) && (binding.extra ? value > 0 : value < 0))
                    {
                        return true;
                    }
                }
                break;
            case FbneoInputSource_JoystickButton:
                if (joystick != nullptr && SDL_GetJoystickButton(joystick, binding.data))
                {
                    return true;
                }
                break;
            case FbneoInputSource_JoystickHat:
                if (joystick != nullptr && (SDL_GetJoystickHat(joystick, binding.data) & binding.extra))
                {
                    return true;
                }
                break;
            case FbneoInputSource_JoystickAxis:
                if (joystick != nullptr)
                {
                    const int value = SDL_GetJoystickAxis(joystick, binding.data);
                    if (std::abs(value) >= (FBNEO_AXIS_PEAK / 2) && (binding.extra ? value > 0 : value < 0))
                    {
                        return true;
                    }
                }
                break;
            default:
                break;
        }
    }

    return false;
}

class FbneoInputHandler : public QObject
{
public:
    explicit FbneoInputHandler(QObject* parent) : QObject(parent)
    {
        for (int i = 0; i < SDL_SCANCODE_COUNT; i++)
        {
            this->keyboardState[i] = false;
        }

        if (!SDL_WasInit(SDL_INIT_GAMEPAD))
        {
            SDL_InitSubSystem(SDL_INIT_GAMEPAD);
        }

        this->loadPlayers();
        this->openDevices();

        this->timer = new QTimer(this);
        this->timer->setInterval(8);
        connect(this->timer, &QTimer::timeout, this, [this]() { this->poll(); });
        this->timer->start();
    }

    ~FbneoInputHandler() override
    {
        this->closeDevices();
    }

    // Re-read the bindings/devices from settings and re-open the controllers.
    // Used when the user reconfigures controls while a game is already running,
    // where the inactive->active reload in poll() will not fire again.
    void reloadSettings(void)
    {
        this->closeDevices();
        this->loadPlayers();
        this->openDevices();
        this->m_openRetryTicks = 0;
        this->m_openAttempts   = 0;
        for (int player = 0; player < FBNEO_MAX_PLAYERS; player++)
        {
            this->lastMask[player] = 0;
        }
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        const QEvent::Type type = event->type();
        if (type != QEvent::KeyPress && type != QEvent::KeyRelease)
        {
            return QObject::eventFilter(watched, event);
        }

        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->isAutoRepeat())
        {
            return QObject::eventFilter(watched, event);
        }

        const int scancode = Utilities::QtKeyToSdl3Key(keyEvent->key());
        if (scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_SCANCODE_COUNT)
        {
            this->keyboardState[scancode] = (type == QEvent::KeyPress);
        }

        return QObject::eventFilter(watched, event);
    }

private:
    void loadPlayers(void)
    {
        const std::string driver = CoreGetFbneoDriverName();

        for (int player = 0; player < FBNEO_MAX_PLAYERS; player++)
        {
            FbneoArcadePlayer& arcadePlayer = this->players[player];
            arcadePlayer = FbneoArcadePlayer();

            const std::string deviceSection = "RMG Arcade Device - P" + std::to_string(player);
            arcadePlayer.device     = CoreSettingsGetIntValue(SettingsID::FBNeo_DeviceType, deviceSection);
            arcadePlayer.deviceName = CoreSettingsGetStringValue(SettingsID::FBNeo_DeviceName, deviceSection);
            arcadePlayer.devicePath = CoreSettingsGetStringValue(SettingsID::FBNeo_DevicePath, deviceSection);
            arcadePlayer.deviceNum  = CoreSettingsGetIntValue(SettingsID::FBNeo_DeviceNum, deviceSection);

            // Global bindings first, then the per-driver section overlays them.
            loadFbneoBindings(arcadePlayer.bindings, "RMG Arcade Input - Global");
            if (!driver.empty())
            {
                std::unordered_map<std::string, std::vector<FbneoBinding>> driverBindings;
                loadFbneoBindings(driverBindings, "RMG Arcade Input - " + driver);
                for (auto& entry : driverBindings)
                {
                    arcadePlayer.bindings[entry.first] = std::move(entry.second);
                }
            }

            if (fbneoTraceEnabled())
            {
                fprintf(stderr, "[FBNEO-IN] player %d driver='%s' device=%d bindings=%zu\n",
                        player + 1, driver.c_str(), arcadePlayer.device, arcadePlayer.bindings.size());
                fflush(stderr);
            }
        }
    }

    void openDevices(void)
    {
        // Make sure SDL has processed the initial device-added events before we
        // enumerate, otherwise the list can be empty right after the subsystem
        // is initialised and the controller would never be opened.
        SDL_PumpEvents();
        SDL_UpdateJoysticks();

        int               count     = 0;
        SDL_JoystickID*   joysticks = SDL_GetJoysticks(&count);
        std::vector<bool> used(static_cast<size_t>(count > 0 ? count : 0), false);

        for (int player = 0; player < FBNEO_MAX_PLAYERS; player++)
        {
            FbneoArcadePlayer& arcadePlayer = this->players[player];
            if (arcadePlayer.device == FbneoInputDevice_None ||
                arcadePlayer.device == FbneoInputDevice_Keyboard)
            {
                continue;
            }

            // Already open (called again to pick up a device that appeared
            // later): keep it and reserve its slot so we never open it twice.
            if (arcadePlayer.gamepad != nullptr || arcadePlayer.joystick != nullptr)
            {
                for (int i = 0; i < count; i++)
                {
                    if (joysticks[i] == arcadePlayer.deviceId)
                    {
                        used[i] = true;
                        break;
                    }
                }
                continue;
            }

            int match = -1;

            // prefer an exact device match (path first, then name)
            if (!arcadePlayer.devicePath.empty() || !arcadePlayer.deviceName.empty())
            {
                for (int i = 0; i < count; i++)
                {
                    if (used[i])
                    {
                        continue;
                    }

                    const char* path = SDL_GetJoystickPathForID(joysticks[i]);
                    const char* name = SDL_GetJoystickNameForID(joysticks[i]);

                    if ((!arcadePlayer.devicePath.empty() && path != nullptr && arcadePlayer.devicePath == path) ||
                        (!arcadePlayer.deviceName.empty() && name != nullptr && arcadePlayer.deviceName == name))
                    {
                        match = i;
                        break;
                    }
                }
            }

            // otherwise an exact device number, then the next unused device
            if (match < 0 &&
                arcadePlayer.deviceNum >= 0 && arcadePlayer.deviceNum < count && !used[arcadePlayer.deviceNum])
            {
                match = arcadePlayer.deviceNum;
            }

            if (match < 0)
            {
                for (int i = 0; i < count; i++)
                {
                    if (!used[i])
                    {
                        match = i;
                        break;
                    }
                }
            }

            if (match < 0)
            {
                continue;
            }

            used[match] = true;
            arcadePlayer.deviceId = joysticks[match];

            const SDL_JoystickID id = joysticks[match];
            if (SDL_IsGamepad(id))
            {
                arcadePlayer.gamepad = SDL_OpenGamepad(id);
                if (arcadePlayer.gamepad != nullptr)
                {
                    arcadePlayer.joystick = SDL_GetGamepadJoystick(arcadePlayer.gamepad);
                }
            }
            else
            {
                arcadePlayer.joystick = SDL_OpenJoystick(id);
            }

            if (fbneoTraceEnabled())
            {
                const char* name = arcadePlayer.gamepad != nullptr ? SDL_GetGamepadName(arcadePlayer.gamepad)
                                  : (arcadePlayer.joystick != nullptr ? SDL_GetJoystickName(arcadePlayer.joystick) : nullptr);
                fprintf(stderr, "[FBNEO-IN] player %d opened %s '%s'\n", player + 1,
                        arcadePlayer.gamepad != nullptr ? "gamepad" : "joystick", name != nullptr ? name : "?");
            }
        }

        if (joysticks != nullptr)
        {
            SDL_free(joysticks);
        }
    }

    void closeDevices(void)
    {
        for (FbneoArcadePlayer& arcadePlayer : this->players)
        {
            if (arcadePlayer.gamepad != nullptr)
            {
                SDL_CloseGamepad(arcadePlayer.gamepad);
                arcadePlayer.gamepad  = nullptr;
                arcadePlayer.joystick = nullptr;
            }
            else if (arcadePlayer.joystick != nullptr)
            {
                SDL_CloseJoystick(arcadePlayer.joystick);
                arcadePlayer.joystick = nullptr;
            }
            arcadePlayer.deviceId = 0;
        }
    }

    void poll(void)
    {
        bool active = false;
        for (int player = 0; player < FBNEO_MAX_PLAYERS; player++)
        {
            if (CoreGetFbneoPlayerInputCount(player) > 0)
            {
                active = true;
                break;
            }
        }

        // A configured joystick device that is not open yet (controller plugged
        // in after the game started, or not enumerated on the first attempt) is
        // retried a few times so it eventually binds.
        bool needsOpen = false;
        for (int player = 0; player < FBNEO_MAX_PLAYERS; player++)
        {
            const FbneoArcadePlayer& arcadePlayer = this->players[player];
            if ((arcadePlayer.device == FbneoInputDevice_Joystick ||
                 arcadePlayer.device == FbneoInputDevice_Automatic) &&
                arcadePlayer.gamepad == nullptr && arcadePlayer.joystick == nullptr)
            {
                needsOpen = true;
                break;
            }
        }

        // Re-read the bindings whenever a new game starts so freshly configured
        // controls/devices are picked up. Afterwards, keep retrying a configured
        // device that is still missing (plugged in late or not enumerated yet)
        // about twice a second, without disturbing players already bound.
        if (active && !this->wasActive)
        {
            this->m_openRetryTicks = 0;
            this->m_openAttempts   = 0;
            this->closeDevices();
            this->loadPlayers();
            this->openDevices();
        }
        else if (active && needsOpen && this->m_openAttempts < 30 &&
                 ++this->m_openRetryTicks >= 60)
        {
            this->m_openRetryTicks = 0;
            ++this->m_openAttempts;
            this->openDevices();
        }
        if (!needsOpen)
        {
            this->m_openRetryTicks = 0;
        }
        if (!active)
        {
            this->m_openAttempts = 0;
        }
        this->wasActive = active;

        if (!active)
        {
            return;
        }

        SDL_PumpEvents();
        SDL_UpdateJoysticks();

        for (int player = 0; player < FBNEO_MAX_PLAYERS; player++)
        {
            const int count = CoreGetFbneoPlayerInputCount(player);
            if (count <= 0)
            {
                continue;
            }

            const FbneoArcadePlayer& arcadePlayer = this->players[player];

            uint32_t mask = 0;
            for (int bit = 0; bit < count && bit < 32; bit++)
            {
                const char* name = CoreGetFbneoPlayerInputName(player, bit);
                if (name == nullptr)
                {
                    continue;
                }

                const auto it = arcadePlayer.bindings.find(name);
                if (it != arcadePlayer.bindings.end() &&
                    evalFbneoBindings(it->second, this->keyboardState, arcadePlayer.gamepad, arcadePlayer.joystick))
                {
                    mask |= (1u << bit);
                }
            }

            if (mask != this->lastMask[player])
            {
                this->lastMask[player] = mask;
                if (fbneoTraceEnabled())
                {
                    fprintf(stderr, "[FBNEO-IN] player %d mask=0x%08x\n", player + 1, mask);
                }
            }

            CoreSetFbneoLocalInput(player, mask);
        }
    }

    QTimer*         timer      = nullptr;
    bool            wasActive  = false;
    int             m_openRetryTicks = 0;
    int             m_openAttempts   = 0;
    bool            keyboardState[SDL_SCANCODE_COUNT];
    FbneoArcadePlayer players[FBNEO_MAX_PLAYERS];
    uint32_t        lastMask[FBNEO_MAX_PLAYERS] = { 0, 0, 0, 0 };
};

} // namespace

static FbneoInputHandler* s_FbneoInputHandler = nullptr;

// Reload the live FBNeo input mapping from settings. Safe to call from the GUI
// thread (where both the dialog and the handler's poll() run).
void FbneoInputReloadSettings(void)
{
    if (s_FbneoInputHandler != nullptr)
    {
        s_FbneoInputHandler->reloadSettings();
    }
}

static void FbneoPushAudio(const int16_t* audio, int audioFrames)
{
    if (audio == nullptr || audioFrames <= 0)
    {
        return;
    }

    // The netcode paces visible frames at the timesync scale (0.99..1.01), so the
    // number of samples pushed per real second is not exactly the declared 48000
    // (e.g. 805 samples/frame at 59.0 fps = 47.5k/s starves the device and makes
    // netplay audio glitch/repeat, while pacing at 60.6 fps pushes 48.8k/s and
    // overflows the queue). Measure the real push rate over a rolling window and
    // feed the delta into the stream's frequency ratio: the queue then drains at
    // exactly the push rate and the pitch shift stays under 1%.
    const Uint64 nowNs = SDL_GetTicksNS();
    if (s_FbneoAudioStream == nullptr)
    {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            static bool loggedFailure = false;
            if (!loggedFailure)
            {
                loggedFailure = true;
                fprintf(stderr, "[FBNEO-DBG] audio: SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
                fflush(stderr);
            }
            return;
        }

        SDL_AudioSpec spec;
        SDL_zero(spec);
        spec.format   = SDL_AUDIO_S16;
        spec.channels = 2;
        spec.freq     = 48000;
        s_FbneoAudioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (s_FbneoAudioStream == nullptr)
        {
            fprintf(stderr, "[FBNEO-DBG] audio: open failed: %s\n", SDL_GetError());
            fflush(stderr);
            return;
        }
        SDL_ResumeAudioStreamDevice(s_FbneoAudioStream);
        fprintf(stderr, "[FBNEO-DBG] audio: opened S16 stereo 48000 stream\n");
        fflush(stderr);
        s_windowStartNs = 0;
        s_windowSamples = 0;
    }

    const int len = audioFrames * 2 * (int)sizeof(int16_t);

    if (!s_FbneoAudioTrace.loaded)
    {
        s_FbneoAudioTrace.loaded = true;
        const char* env = std::getenv("RMGK_FBNEO_AUDIO_TRACE");
        s_FbneoAudioTrace.enabled =
            env == nullptr || std::strcmp(env, "0") != 0;
        if (s_FbneoAudioTrace.enabled)
        {
            FbneoAudioTraceOpen();
            s_FbneoAudioTrace.windowStartNs = nowNs;
        }
    }
    if (s_FbneoAudioTrace.enabled)
    {
        s_FbneoAudioTrace.windowPushes += 1;
        s_FbneoAudioTrace.windowSamples += audioFrames;
    }
    if (s_windowStartNs == 0)
    {
        s_windowStartNs = nowNs;
        s_windowSamples = 0;
    }
    s_windowSamples += audioFrames;
    const Uint64 windowNs = nowNs - s_windowStartNs;
    if (windowNs >= 500000000)
    {
        if (s_windowSamples > 0)
        {
            float target = (float)(((double)s_windowSamples * 1e9) / ((double)windowNs * 48000.0));
            if (target < 0.90f) target = 0.90f;
            if (target > 1.10f) target = 1.10f;
            if (std::abs(target - s_ratio) * 48000.0f > 24.0f)
            {
                s_ratio = target;
                SDL_SetAudioStreamFrequencyRatio(s_FbneoAudioStream, s_ratio);
            }
        }
        s_windowStartNs = nowNs;
        s_windowSamples = 0;
    }

    // Keep latency bounded: if the core ever runs ahead of the device (e.g. during
    // fast-forward or a slow first frame), drop the backlog instead of drifting.
    if (SDL_GetAudioStreamQueued(s_FbneoAudioStream) > len * 8)
    {
        SDL_ClearAudioStream(s_FbneoAudioStream);
        if (s_FbneoAudioTrace.enabled)
        {
            s_FbneoAudioTrace.clears += 1;
        }
    }

    SDL_PutAudioStreamData(s_FbneoAudioStream, audio, len);

    FbneoAudioTraceTick(SDL_GetTicksNS());
}

// Reads the aspect ratio the user selected in the video plugin configuration
// (GLideN64 stores it as "frameBufferEmulation\aspect"), so the FBNeo presenter
// matches the same choice the N64 games use. GLideN64 aspect values:
//   0 = stretch, 1 = force 4:3, 2 = force 16:9, 3 = adjust 4:3, 4 = adjust 16:9
// Returns 0.0 for stretch, otherwise width/height. When nothing is configured we
// default to 4:3, which is the correct display aspect for arcade (FBNeo) games.
static double fbneoConfiguredAspect(void)
{
    const std::filesystem::path dir = CoreGetUserConfigDirectory();

    int aspect = -1;
    for (const char* name : { "GLideN64.custom.ini", "GLideN64.ini" })
    {
        std::ifstream in(dir / name);
        if (!in.is_open())
        {
            continue;
        }

        std::string line;
        while (std::getline(in, line))
        {
            const size_t keyPos = line.find("frameBufferEmulation");
            const size_t valuePos = line.find("aspect=");
            if (keyPos == std::string::npos || valuePos == std::string::npos)
            {
                continue;
            }

            try
            {
                aspect = std::stoi(line.substr(valuePos + std::string("aspect=").size()));
            }
            catch (...)
            {
                aspect = -1;
            }
            break;
        }

        if (aspect >= 0)
        {
            break;
        }
    }

    switch (aspect)
    {
        case 0:  return 0.0;        // stretch
        case 2:  return 16.0 / 9.0; // force 16:9
        case 4:  return 16.0 / 9.0; // adjust 16:9
        case 1:  return 4.0 / 3.0;  // force 4:3
        case 3:  return 4.0 / 3.0;  // adjust 4:3
        default: return 4.0 / 3.0;  // not configured -> arcade default
    }
}

static void RMG_FbneoVideoPresent(const void* video, int width, int height, int pitch,
                                  int orientationFlags, const int16_t* audio, int audioFrames,
                                  void* user)
{
    (void)user;

    FbneoPushAudio(audio, audioFrames);

    if (l_OGLWidget == nullptr)
    {
        return;
    }

    // Bring the output widget up on the first visible frame. Both calls block on
    // the GUI thread, so the widget exists (and the context is current) once they
    // return.
    if (!s_FbneoPresenterUp)
    {
        if (VidExt_InitWithRenderMode(M64P_RENDER_OPENGL) != M64ERR_SUCCESS)
        {
            return;
        }
        if (VidExt_SetMode(width, height, 32, M64VIDEO_WINDOWED, 0) != M64ERR_SUCCESS)
        {
            return;
        }
        s_FbneoPresenterUp = true;
    }

    if (*l_OGLWidget == nullptr || l_RenderMode != M64P_RENDER_OPENGL)
    {
        return;
    }

    // Refresh the target aspect once per emulation (the presenter is torn down
    // between games), so changing the video plugin's aspect ratio takes effect on
    // the next launch.
    if (!s_FbneoAspectLoaded)
    {
        s_FbneoAspectLoaded = true;
        s_FbneoTargetAspect = fbneoConfiguredAspect();
    }

    QOpenGLContext* context = (*l_OGLWidget)->GetContext();
    if (context == nullptr || !context->isValid())
    {
        return;
    }

    const QSize windowSize = (*l_OGLWidget)->size();
    const qreal dpr        = (*l_OGLWidget)->devicePixelRatio();
    const int   viewW      = static_cast<int>(windowSize.width() * dpr);
    const int   viewH      = static_cast<int>(windowSize.height() * dpr);
    if (viewW <= 0 || viewH <= 0)
    {
        return;
    }

    if (!s_FbneoPresenterLogged)
    {
        s_FbneoPresenterLogged = true;
        const QSurfaceFormat f = context->format();
        fprintf(stderr, "[FBNEO-DBG] presenter: frame=%dx%d pitch=%d view=%dx%d err=%d\n",
                width, height, pitch, viewW, viewH, (int)glGetError());
        fprintf(stderr, "[FBNEO-DBG] gl version=%s renderer=%s profile=%d fmt=%d.%d type=%d\n",
                (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER),
                (int)f.profile(), f.majorVersion(), f.minorVersion(), (int)f.renderableType());
        fflush(stderr);
    }

    QOpenGLFunctions* f = context->functions();
    if (f == nullptr)
    {
        return;
    }

    if (s_FbneoTexture == 0)
    {
        f->glGenTextures(1, &s_FbneoTexture);
    }
    if (s_FbneoProgram == 0 && !FbneoPresenterBuildPipeline(context))
    {
        return;
    }

    f->glViewport(0, 0, viewW, viewH);
    f->glDisable(GL_DEPTH_TEST);
    f->glDisable(GL_BLEND);
    f->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    f->glClear(GL_COLOR_BUFFER_BIT);

    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, s_FbneoTexture);
    f->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (!s_FbneoEs)
    {
        f->glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / 4);
    }

    if (width != s_FbneoTextureW || height != s_FbneoTextureH)
    {
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, video);
        s_FbneoTextureW = width;
        s_FbneoTextureH = height;
    }
    else
    {
        f->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, video);
    }
    if (!s_FbneoEs)
    {
        f->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }

    // Display the frame at the aspect ratio the user selected in the video plugin
    // config (0 => stretch/fill). Rotated (vertical) arcade games display in
    // portrait, so their target ratio is the inverse of the horizontal one.
    double targetAspect = s_FbneoTargetAspect;
    if ((orientationFlags & 1 /* FBNeo kVideoVertical */) != 0 && targetAspect > 0.0)
    {
        targetAspect = 1.0 / targetAspect;
    }

    double dstW = static_cast<double>(viewW);
    double dstH = static_cast<double>(viewH);
    if (targetAspect > 0.0)
    {
        const double viewAspect = static_cast<double>(viewW) / static_cast<double>(viewH);
        if (viewAspect > targetAspect)
        {
            dstH = static_cast<double>(viewH);
            dstW = dstH * targetAspect;
        }
        else
        {
            dstW = static_cast<double>(viewW);
            dstH = dstW / targetAspect;
        }
    }
    const double x0 = (static_cast<double>(viewW) - dstW) * 0.5;
    const double y0 = (static_cast<double>(viewH) - dstH) * 0.5;
    const double x1 = x0 + dstW;
    const double y1 = y0 + dstH;

    // Window coordinates have y pointing up, matching clip space (no flip).
    // glTexImage2D treats the first buffer row as t = 0, i.e. FBNeo's top row, so
    // t = 0 is mapped to the top of the quad to keep the image upright.
    const float nx0 = static_cast<float>(2.0 * x0 / viewW - 1.0);
    const float nx1 = static_cast<float>(2.0 * x1 / viewW - 1.0);
    const float ny0 = static_cast<float>(2.0 * y0 / viewH - 1.0);
    const float ny1 = static_cast<float>(2.0 * y1 / viewH - 1.0);

    const float verts[16] = {
        nx0, ny0, 0.0f, 1.0f,
        nx1, ny0, 1.0f, 1.0f,
        nx0, ny1, 0.0f, 0.0f,
        nx1, ny1, 1.0f, 0.0f,
    };

    f->glUseProgram(s_FbneoProgram);
    f->glUniform1i(s_FbneoSampler, 0);
    if (s_FbneoVao != 0 && s_pglBindVertexArray != nullptr)
    {
        s_pglBindVertexArray(s_FbneoVao);
    }
    f->glBindBuffer(GL_ARRAY_BUFFER, s_FbneoVbo);
    f->glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    f->glEnableVertexAttribArray(0);
    f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void*)0);
    f->glEnableVertexAttribArray(1);
    f->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void*)(2 * sizeof(float)));
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (!s_FbneoPresenterDrawChecked)
    {
        s_FbneoPresenterDrawChecked = true;
        fprintf(stderr, "[FBNEO-DBG] draw err=%d vao=%u prog=%u\n",
                (int)f->glGetError(), (unsigned)s_FbneoVao, (unsigned)s_FbneoProgram);
        fflush(stderr);
    }
    f->glDisableVertexAttribArray(0);
    f->glDisableVertexAttribArray(1);
    f->glBindBuffer(GL_ARRAY_BUFFER, 0);
    f->glUseProgram(0);

    s_FbneoPresenterFrames++;
    const bool doGlCheck = s_FbneoCheckReports < 3 &&
                           s_FbneoPresenterFrames >= 300 &&
                           (s_FbneoPresenterFrames % 60 == 0);
    if (doGlCheck)
    {
        auto meanQuad = [](const unsigned char* p, int stride, int x0, int y0, int w, int h, int& r, int& g, int& b)
        {
            double sr = 0, sg = 0, sb = 0; int n = 0;
            for (int y = y0; y < y0 + h; y += 3)
            {
                for (int x = x0; x < x0 + w; x += 3)
                {
                    const unsigned char* px = p + (size_t)y * stride + (size_t)x * 4;
                    sr += px[2]; sg += px[1]; sb += px[0]; n++;
                }
            }
            r = n ? (int)(sr / n) : -1; g = n ? (int)(sg / n) : -1; b = n ? (int)(sb / n) : -1;
        };

        // CPU side: source buffer (BGRA), row 0 = top of the image
        int cr = 0, cg = 0, cb = 0, cbr = 0, cbg = 0, cbb = 0;
        int clr = 0, clg = 0, clb = 0, crr = 0, crg = 0, crb = 0;
        {
            const unsigned char* p = (const unsigned char*)video;
            meanQuad(p, pitch, 0, 0, width, height / 2, cr, cg, cb);
            meanQuad(p, pitch, 0, height / 2, width, height / 2, cbr, cbg, cbb);
            meanQuad(p, pitch, 0, 0, width / 2, height, clr, clg, clb);
            meanQuad(p, pitch, width / 2, 0, width / 2, height, crr, crg, crb);
        }

        // GL side: read back what was just drawn. glReadPixels row 0 = bottom row of
        // the quad = bottom of the displayed image.
        const int qw = (int)(x1 - x0);
        const int qh = (int)(y1 - y0);
        std::vector<unsigned char> buf((size_t)qw * qh * 4);
        f->glPixelStorei(GL_PACK_ALIGNMENT, 1);
        f->glReadPixels((GLint)x0, (GLint)y0, qw, qh, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());

        int gr = 0, gg = 0, gb = 0, gtr = 0, gtg = 0, gtb = 0;
        {
            const unsigned char* p = buf.data();
            int stride = qw * 4;
            // readback row 0 = image bottom, so the lower half of the buffer is the image top
            auto mean = [&](int ry0, int rh, int& r, int& g, int& b)
            {
                double sr = 0, sg = 0, sb = 0; int n = 0;
                for (int y = ry0; y < ry0 + rh; y += 3)
                    for (int x = 0; x < qw; x += 3)
                    {
                        const unsigned char* px = p + (size_t)y * stride + (size_t)x * 4;
                        sr += px[0]; sg += px[1]; sb += px[2]; n++;
                    }
                r = n ? (int)(sr / n) : -1; g = n ? (int)(sg / n) : -1; b = n ? (int)(sb / n) : -1;
            };
            mean(0, qh / 2, gr, gg, gb);      // image bottom
            mean(qh / 2, qh / 2, gtr, gtg, gtb); // image top
        }

        fprintf(stderr, "[FBNEO-DBG] glchk src top=(%d,%d,%d) bot=(%d,%d,%d) left=(%d,%d,%d) right=(%d,%d,%d)\n",
                cr, cg, cb, cbr, cbg, cbb, clr, clg, clb, crr, crg, crb);
        fprintf(stderr, "[FBNEO-DBG] glchk dst top=(%d,%d,%d) bot=(%d,%d,%d) quad=%dx%d\n",
                gtr, gtg, gtb, gr, gg, gb, qw, qh);
        fflush(stderr);

        if (cr + cg + cb + cbr + cbg + cbb + clr + clg + clb + crr + crg + crb > 0)
        {
            s_FbneoCheckReports++;
        }
    }

    VidExt_GLSwapBuf();
}

static void RMG_FbneoVideoStop(void* user)
{
    (void)user;

    if (s_FbneoAudioStream != nullptr)
    {
        SDL_DestroyAudioStream(s_FbneoAudioStream);
        s_FbneoAudioStream = nullptr;
    }

    if (!s_FbneoPresenterUp)
    {
        return;
    }

    if (s_FbneoTexture != 0)
    {
        glDeleteTextures(1, &s_FbneoTexture);
        s_FbneoTexture = 0;
    }
    s_FbneoTextureW = 0;
    s_FbneoTextureH = 0;

    VidExt_Quit();

    // The shader program / buffers belong to the OpenGL context that was just
    // torn down; the next game creates a fresh context, so forget the old
    // handles or the presenter would skip rebuilding against a dead context.
    s_FbneoProgram = 0;
    s_FbneoVbo     = 0;
    s_FbneoVao     = 0;
    s_FbneoSampler = -1;
    s_FbneoEs      = false;

    s_FbneoPresenterUp = false;
    s_FbneoAspectLoaded = false;
}

static m64p_error VidExt_SetCaption(const char *Title)
{
    l_EmuThread->on_VidExt_SetCaption(QString(Title));
    return M64ERR_SUCCESS;
}

static m64p_error VidExt_ToggleFS(void)
{
#ifdef _WIN32
    if (VidExt_NativeWglActive())
    {
        if (VidExt_NativeWglShouldIgnoreAltEnter())
        {
            return M64ERR_SUCCESS;
        }
        CoreAddCallbackMessage(CoreDebugMessageType::Info, "Leaving native WGL fullscreen by stopping emulation");
        CoreStopEmulation();
        return M64ERR_SUCCESS;
    }
#endif

    CoreVideoMode videoMode;

    if (!CoreGetVideoMode(videoMode))
    {
        return M64ERR_SYSTEM_FAIL;
    }

    if (QThread::currentThread() != l_RenderThread)
    {
        l_MainWindow->on_VidExt_ToggleFS((videoMode == CoreVideoMode::Windowed));
    }
    else
    {
        l_EmuThread->on_VidExt_ToggleFS((videoMode == CoreVideoMode::Windowed));
    }

    return M64ERR_SUCCESS;
}

static m64p_error VidExt_ResizeWindow(int Width, int Height)
{
    l_EmuThread->on_VidExt_ResizeWindow(Width, Height);
    OnScreenDisplaySetDisplaySize(Width, Height);
    return M64ERR_SUCCESS;
}

static uint32_t VidExt_GLGetDefaultFramebuffer(void)
{
    if (l_RenderMode != M64P_RENDER_OPENGL)
    {
        return 0;
    }

#ifdef _WIN32
    if (VidExt_NativeWglActive())
    {
        return 0;
    }
#endif

    return (*l_OGLWidget)->GetContext()->defaultFramebufferObject();
}

static m64p_error VidExt_VK_GetSurface(void** Surface, void* Instance)
{
    if (l_RenderMode != M64P_RENDER_VULKAN)
    {
        return M64ERR_INVALID_STATE;
    }

    // we don't support receiving a null handle
    // for the VkInstance
    if ((VkInstance)Instance == VK_NULL_HANDLE)
    {
        return M64ERR_UNSUPPORTED;
    }

    // use VkInstance from plugin
    // when we don't have a VkInstance yet
    if (l_VulkanInstance.vkInstance() == VK_NULL_HANDLE)
    {
        l_VulkanInstance.setVkInstance((VkInstance)Instance);
        if (!l_VulkanInstance.create())
        {
            CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to create vulkan instance");
            return M64ERR_SYSTEM_FAIL;
        }
        (*l_VulkanWidget)->setVulkanInstance(&l_VulkanInstance);
    }

    // attempt to retrieve vulkan surface for window
    VkSurfaceKHR vulkanSurface = QVulkanInstance::surfaceForWindow((*l_VulkanWidget));
    if (vulkanSurface == VK_NULL_HANDLE)
    {
        CoreAddCallbackMessage(CoreDebugMessageType::Error, "Failed to retrieve vulkan surface for window");
        return M64ERR_SYSTEM_FAIL;
    }

    *Surface = (void*)vulkanSurface;
    return M64ERR_SUCCESS;
}

static m64p_error VidExt_VK_GetInstanceExtensions(const char** Extensions[], uint32_t* NumExtensions)
{
    if (l_RenderMode != M64P_RENDER_VULKAN)
    {
        return M64ERR_INVALID_STATE;
    }

    l_VulkanExtensions = l_VulkanInstance.supportedExtensions();
    l_VulkanExtensionList.clear();

    // only add surface extensions
    for (int i = 0; i < l_VulkanExtensions.size(); i++)
    {
        if (l_VulkanExtensions[i].name.startsWith("VK_KHR_") &&
            l_VulkanExtensions[i].name.endsWith("surface"))
        {
            l_VulkanExtensionList.append(l_VulkanExtensions[i].name.data());
        }
    }

    *Extensions    = l_VulkanExtensionList.data();
    *NumExtensions = l_VulkanExtensionList.size();
    return M64ERR_SUCCESS;
}

//
// Exported Functions
//

bool SetupVidExt(Thread::EmulationThread* emuThread, UserInterface::MainWindow* mainWindow, 
    UserInterface::Widget::OGLWidget** oglWidget, UserInterface::Widget::VKWidget** vulkanWidget)
{
    l_EmuThread    = emuThread;
    l_MainWindow   = mainWindow;
    l_OGLWidget    = oglWidget;
    l_VulkanWidget = vulkanWidget;

    m64p_video_extension_functions vidext_funcs;

    vidext_funcs.Functions = 17;
    vidext_funcs.VidExtFuncInit = &VidExt_Init;
    vidext_funcs.VidExtFuncInitWithRenderMode = &VidExt_InitWithRenderMode;
    vidext_funcs.VidExtFuncQuit = &VidExt_Quit;
    vidext_funcs.VidExtFuncListModes = &VidExt_ListModes;
    vidext_funcs.VidExtFuncListRates = &VidExt_ListRates;
    vidext_funcs.VidExtFuncSetMode = &VidExt_SetMode;
    vidext_funcs.VidExtFuncSetModeWithRate = &VidExt_SetModeWithRate;
    vidext_funcs.VidExtFuncGLGetProc = &VidExt_GLGetProc;
    vidext_funcs.VidExtFuncGLSetAttr = &VidExt_GLSetAttr;
    vidext_funcs.VidExtFuncGLGetAttr = &VidExt_GLGetAttr;
    vidext_funcs.VidExtFuncGLSwapBuf = &VidExt_GLSwapBuf;
    vidext_funcs.VidExtFuncSetCaption = &VidExt_SetCaption;
    vidext_funcs.VidExtFuncToggleFS = &VidExt_ToggleFS;
    vidext_funcs.VidExtFuncResizeWindow = &VidExt_ResizeWindow;
    vidext_funcs.VidExtFuncGLGetDefaultFramebuffer = &VidExt_GLGetDefaultFramebuffer;
    vidext_funcs.VidExtFuncVKGetSurface = &VidExt_VK_GetSurface;
    vidext_funcs.VidExtFuncVKGetInstanceExtensions = &VidExt_VK_GetInstanceExtensions;

    if (!CoreSetupVidExt(vidext_funcs))
    {
        return false;
    }

    // FBNeo drives its own software framebuffer, so it bypasses the mupen video
    // extension; register our presenter to blit its frames into the same OGL widget.
    CoreSetFbneoFrameCallback(&RMG_FbneoVideoPresent, nullptr);
    CoreSetFbneoStopCallback(&RMG_FbneoVideoStop, nullptr);

    // Feed FBNeo's live input from the mapping the user configured for the RMG
    // (N64) input plugin. The handler is installed once, on the GUI thread,
    // where this setup runs.
    if (s_FbneoInputHandler == nullptr)
    {
        s_FbneoInputHandler = new FbneoInputHandler(QCoreApplication::instance());
        QCoreApplication::instance()->installEventFilter(s_FbneoInputHandler);
    }

    return true;
}
