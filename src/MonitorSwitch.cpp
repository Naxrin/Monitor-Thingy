#include "MonitorSwitch.hpp"

#include <Geode/Geode.hpp>

using namespace geode::prelude;

#ifdef GEODE_IS_WINDOWS

// cocos2d already pulls in windows.h through CCEGLView.h on Windows, but
// include it explicitly so this file does not rely on include order.
#include <windows.h>

// windows.h defines these as macros, which breaks std::min/std::max.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <optional>
#include <string>
#include <vector>

namespace {
    struct MonitorData {
        HMONITOR handle = nullptr;
        MONITORINFOEXW info{};
    };

    BOOL CALLBACK collectMonitors(HMONITOR handle, HDC, LPRECT, LPARAM userdata) {
        auto& monitors = *reinterpret_cast<std::vector<MonitorData>*>(userdata);

        MonitorData data;
        data.handle = handle;
        data.info.cbSize = static_cast<DWORD>(sizeof(MONITORINFOEXW));

        if (GetMonitorInfoW(handle, reinterpret_cast<LPMONITORINFO>(&data.info))) {
            monitors.push_back(data);
        }
        return TRUE;
    }

    std::vector<MonitorData> getMonitors() {
        std::vector<MonitorData> monitors;
        EnumDisplayMonitors(nullptr, nullptr, collectMonitors, reinterpret_cast<LPARAM>(&monitors));

        // Sort monitors into a predictable left-to-right / top-to-bottom order
        // instead of relying on the enumeration order of the OS.
        std::sort(monitors.begin(), monitors.end(), [](MonitorData const& a, MonitorData const& b) {
            if (a.info.rcMonitor.left != b.info.rcMonitor.left) {
                return a.info.rcMonitor.left < b.info.rcMonitor.left;
            }
            if (a.info.rcMonitor.top != b.info.rcMonitor.top) {
                return a.info.rcMonitor.top < b.info.rcMonitor.top;
            }
            return std::wcscmp(a.info.szDevice, b.info.szDevice) < 0;
        });

        return monitors;
    }

    std::string deviceNameOf(MonitorData const& monitor) {
        return geode::utils::string::wideToUtf8(monitor.info.szDevice);
    }

    bool belongsToGame(HWND hwnd) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        return pid == GetCurrentProcessId();
    }

    struct WindowSearch {
        HWND glfwWindow = nullptr;
        HWND largestWindow = nullptr;
        LONG largestArea = 0;
    };

    BOOL CALLBACK findWindowProc(HWND hwnd, LPARAM userdata) {
        auto& search = *reinterpret_cast<WindowSearch*>(userdata);

        if (!belongsToGame(hwnd) || !IsWindowVisible(hwnd)) {
            return TRUE;
        }

        RECT rect{};
        if (!GetWindowRect(hwnd, &rect)) {
            return TRUE;
        }

        LONG width = rect.right - rect.left;
        LONG height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0) {
            return TRUE;
        }

        if (!search.glfwWindow) {
            wchar_t className[64]{};
            if (GetClassNameW(hwnd, className, 64) > 4 &&
                std::wcsncmp(className, L"GLFW", 4) == 0) {
                search.glfwWindow = hwnd;
            }
        }

        LONG area = width * height;
        if (area > search.largestArea) {
            search.largestArea = area;
            search.largestWindow = hwnd;
        }

        return TRUE;
    }

    HWND findGameWindow() {
        // The OpenGL context is the most reliable way to find the game window
        // while it is being rendered on the main thread.
        if (HDC dc = wglGetCurrentDC()) {
            if (HWND hwnd = WindowFromDC(dc); hwnd && belongsToGame(hwnd)) {
                return hwnd;
            }
        }

        // Otherwise look for the GLFW window; the process may also own other
        // top-level windows (a console, native file dialogs, ...).
        WindowSearch search;
        EnumWindows(findWindowProc, reinterpret_cast<LPARAM>(&search));
        if (search.glfwWindow) {
            return search.glfwWindow;
        }

        if (HWND foreground = GetForegroundWindow(); foreground && belongsToGame(foreground)) {
            return foreground;
        }
        if (HWND active = GetActiveWindow(); active && belongsToGame(active)) {
            return active;
        }

        return search.largestWindow;
    }

    bool isGameFullscreen(HWND hwnd) {
        if (auto view = cocos2d::CCEGLView::get(); view && view->getIsFullscreen()) {
            return true;
        }

        // Fallback in case the game's internal flag is not set yet: a window
        // that exactly covers a whole monitor (including the taskbar area) is
        // effectively fullscreen.
        if (!hwnd) {
            return false;
        }

        RECT window{};
        if (!GetWindowRect(hwnd, &window)) {
            return false;
        }

        MONITORINFO info{};
        info.cbSize = static_cast<DWORD>(sizeof(MONITORINFO));
        if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info)) {
            return false;
        }

        return window.left == info.rcMonitor.left &&
               window.top == info.rcMonitor.top &&
               window.right == info.rcMonitor.right &&
               window.bottom == info.rcMonitor.bottom;
    }

    std::ptrdiff_t monitorIndex(std::vector<MonitorData> const& monitors, HMONITOR handle) {
        for (std::size_t i = 0; i < monitors.size(); ++i) {
            if (monitors[i].handle == handle) {
                return static_cast<std::ptrdiff_t>(i);
            }
        }
        return -1;
    }

    std::ptrdiff_t monitorIndexForWindow(std::vector<MonitorData> const& monitors, HWND hwnd) {
        auto index = monitorIndex(monitors, MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
        if (index >= 0) {
            return index;
        }

        RECT rect{};
        if (GetWindowRect(hwnd, &rect)) {
            POINT center{ (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2 };
            index = monitorIndex(monitors, MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST));
        }
        return index;
    }

    void showNotification(std::string const& text, NotificationIcon icon) {
        if (!Mod::get()->getSettingValue<bool>("show-notification")) {
            return;
        }
        Notification::create(text, icon)->show();
    }

    // Window currently hidden from the compositor while its viewport is being
    // updated; revealed by onFramePresented() after the next buffer swap.
    HWND g_cloakedWindow = nullptr;

    // Placement/style to restore when leaving borderless fullscreen.
    struct WindowedState {
        RECT rect{};
        LONG_PTR style = 0;
        LONG_PTR exStyle = 0;
        cocos2d::CCSize clientSize{};
        bool valid = false;
    };
    WindowedState g_windowedState;

    // True while the window covers the monitor borderlessly but the game is
    // still in its windowed mode (the Fullscreen option stays unchecked).
    bool g_borderlessWindowed = false;
    HWND g_borderlessWindowedHwnd = nullptr;

    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

    DwmSetWindowAttributeFn getDwmSetWindowAttribute() {
        static auto fn = []() -> DwmSetWindowAttributeFn {
            auto dwmapi = LoadLibraryW(L"dwmapi.dll");
            if (!dwmapi) {
                return nullptr;
            }
            return reinterpret_cast<DwmSetWindowAttributeFn>(
                GetProcAddress(dwmapi, "DwmSetWindowAttribute")
            );
        }();
        return fn;
    }

    /**
     * Temporarily hide the window from the DWM compositor without changing
     * its focus or visibility state. Used to avoid showing a frame that still
     * uses the old monitor's viewport while the window is being resized.
     */
    bool setWindowCloaked(HWND hwnd, bool cloaked) {
        auto fn = getDwmSetWindowAttribute();
        if (!fn) {
            return false;
        }

        // DWMWA_CLOAK from dwmapi.h, hardcoded so dwmapi.lib is not needed.
        constexpr DWORD DWMWA_CLOAK = 13;
        BOOL value = cloaked ? TRUE : FALSE;
        if (!SUCCEEDED(fn(hwnd, DWMWA_CLOAK, &value, sizeof(value)))) {
            return false;
        }

        if (cloaked) {
            g_cloakedWindow = hwnd;
        }
        else if (g_cloakedWindow == hwnd) {
            g_cloakedWindow = nullptr;
        }
        return true;
    }

    /**
     * Force the game to recompute its frame/design resolution for the current
     * window size.
     *
     * A fullscreen window is normally never resized by the user, so the game
     * does not refresh its scaling when such a window is moved to a monitor
     * with a different resolution. Without this, the game keeps rendering
     * with the old monitor's scale.
     */
    void refreshGameView(HWND hwnd) {
        auto view = cocos2d::CCEGLView::get();
        if (!view) {
            return;
        }

        RECT client{};
        if (!GetClientRect(hwnd, &client)) {
            return;
        }

        int width = client.right - client.left;
        int height = client.bottom - client.top;
        if (width <= 0 || height <= 0) {
            return;
        }

        onekey::logViewState("View before monitor-switch refresh");

        // Windows has no retina displays, but moving a fullscreen window to a
        // high-DPI monitor can make the game mistake the DPI-scaled
        // framebuffer for a 2x retina surface. That doubles the GL viewport
        // and renders everything at 2x scale. A native start on the same
        // monitor keeps the retina factor at 1, so force the same state here.
        view->m_bInRetinaMonitor = false;
        view->m_bRetinaEnabled = false;
        view->m_nRetinaFactor = 1;

        // This is the same entry point the game uses when its window is
        // resized; it updates the view frame size, design resolution and the
        // director's screen scale / projection from the current GLFW window
        // size.
        view->updateWindow(width, height);

        onekey::logViewState("View after monitor-switch refresh");
    }

    WNDPROC g_resizeOrigWndProc = nullptr;
    HWND g_resizeSubclassedWindow = nullptr;

    // GD's updateWindow() re-applies the window's old windowed size while it
    // refreshes the view. WM_SIZE is suppressed during the Win32 style/size
    // change, and the target window rect is forced while the view is
    // refreshed so the game cannot shrink the borderless window again.
    bool g_suppressSizeMessages = false;
    bool g_forceWindowRect = false;
    LONG g_forcedWindowX = 0;
    LONG g_forcedWindowY = 0;
    LONG g_forcedWindowWidth = 0;
    LONG g_forcedWindowHeight = 0;

    // Short observation period after a switch, used to diagnose presentation
    // problems (frames rendered, view sizes, ...).
    std::chrono::steady_clock::time_point g_observeUntil;
    std::chrono::steady_clock::time_point g_lastObservation;
    int g_framesSinceObservation = 0;

    LRESULT CALLBACK ResizeWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_GETMINMAXINFO) {
            // Allow the window to grow to the target monitor size instead of
            // whatever limit GLFW/GD (or the default procedure) imposes.
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = 1;
            info->ptMinTrackSize.y = 1;
            info->ptMaxTrackSize.x = 16384;
            info->ptMaxTrackSize.y = 16384;
            info->ptMaxSize.x = 16384;
            info->ptMaxSize.y = 16384;
            return 0;
        }

        if (msg == WM_SIZE) {
            RECT window{};
            RECT client{};
            GetWindowRect(hwnd, &window);
            GetClientRect(hwnd, &client);
            log::debug(
                "ResizeWindowProc WM_SIZE wParam={} {}x{} -> window {}x{} client {}x{}{}",
                wParam, LOWORD(lParam), HIWORD(lParam),
                window.right - window.left, window.bottom - window.top,
                client.right - client.left, client.bottom - client.top,
                g_suppressSizeMessages ? " (suppressed)" : ""
            );

            if (g_suppressSizeMessages) {
                // Do not let the game react to this intermediate size; the
                // final size is applied to the view afterwards.
                return 0;
            }
        }

        if (msg == WM_WINDOWPOSCHANGING) {
            auto* pos = reinterpret_cast<WINDOWPOS*>(lParam);
            if (pos) {
                LONG requestedWidth = pos->cx;
                LONG requestedHeight = pos->cy;
                LONG requestedX = pos->x;
                LONG requestedY = pos->y;
                log::debug(
                    "ResizeWindowProc WM_WINDOWPOSCHANGING {}x{}+{},{} flags {:x}",
                    pos->cx, pos->cy, pos->x, pos->y, pos->flags
                );

                LRESULT result = CallWindowProcW(g_resizeOrigWndProc, hwnd, msg, wParam, lParam);
                if (pos->cx != requestedWidth || pos->cy != requestedHeight ||
                    pos->x != requestedX || pos->y != requestedY) {
                    log::debug(
                        "  WM_WINDOWPOSCHANGING was changed to {}x{}+{},{}",
                        pos->cx, pos->cy, pos->x, pos->y
                    );
                }

                if (g_forceWindowRect && !(pos->flags & SWP_NOSIZE)) {
                    if (pos->cx != g_forcedWindowWidth || pos->cy != g_forcedWindowHeight ||
                        pos->x != g_forcedWindowX || pos->y != g_forcedWindowY) {
                        log::debug(
                            "  forcing window rect to {}x{}+{},{}",
                            g_forcedWindowWidth, g_forcedWindowHeight,
                            g_forcedWindowX, g_forcedWindowY
                        );
                        pos->x = g_forcedWindowX;
                        pos->y = g_forcedWindowY;
                        pos->cx = g_forcedWindowWidth;
                        pos->cy = g_forcedWindowHeight;
                    }
                }
                return result;
            }
        }

        if (msg == WM_WINDOWPOSCHANGED) {
            auto* pos = reinterpret_cast<WINDOWPOS*>(lParam);
            if (pos) {
                log::debug(
                    "ResizeWindowProc WM_WINDOWPOSCHANGED {}x{}+{},{} flags {:x}",
                    pos->cx, pos->cy, pos->x, pos->y, pos->flags
                );
            }
        }

        return CallWindowProcW(g_resizeOrigWndProc, hwnd, msg, wParam, lParam);
    }

    void beginWindowResizeBypass(HWND hwnd) {
        g_suppressSizeMessages = false;
        g_forceWindowRect = false;

        if (g_resizeSubclassedWindow) {
            return;
        }

        auto previous = SetWindowLongPtrW(
            hwnd,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(ResizeWindowProc)
        );
        if (!previous) {
            log::warn("Could not subclass the game window procedure");
            return;
        }

        g_resizeOrigWndProc = reinterpret_cast<WNDPROC>(previous);
        g_resizeSubclassedWindow = hwnd;

        // GD reacts to intermediate sizes with another resize, so keep it from
        // seeing WM_SIZE until the target style and size are applied.
        g_suppressSizeMessages = true;
    }

    void endWindowResizeBypass() {
        if (g_resizeSubclassedWindow && g_resizeOrigWndProc) {
            SetWindowLongPtrW(
                g_resizeSubclassedWindow,
                GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(g_resizeOrigWndProc)
            );
        }

        g_resizeSubclassedWindow = nullptr;
        g_resizeOrigWndProc = nullptr;
        g_suppressSizeMessages = false;
        g_forceWindowRect = false;
    }

    void logWindowGeometry(char const* tag, HWND hwnd) {
        RECT window{};
        RECT client{};
        GetWindowRect(hwnd, &window);
        GetClientRect(hwnd, &client);
        log::debug(
            "{}: style {:x}, window {}x{}+{},{} client {}x{}, zoomed {}, iconic {}",
            tag,
            static_cast<uintptr_t>(GetWindowLongPtrW(hwnd, GWL_STYLE)),
            window.right - window.left, window.bottom - window.top,
            window.left, window.top,
            client.right - client.left, client.bottom - client.top,
            IsZoomed(hwnd) != FALSE, IsIconic(hwnd) != FALSE
        );
    }

    /**
     * Resize the window so that its *client* area has the requested size. The
     * caller must already have applied the target window style.
     */
    bool setWindowClientSize(HWND hwnd, LONG x, LONG y, LONG width, LONG height, UINT flags) {
        if (IsIconic(hwnd) || IsZoomed(hwnd)) {
            ShowWindow(hwnd, SW_RESTORE);
        }

        RECT desired{ 0, 0, width, height };
        AdjustWindowRectEx(
            &desired,
            static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)),
            FALSE,
            static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE))
        );

        // SWP_NOSENDCHANGING bypasses GD/GLFW's WM_WINDOWPOSCHANGING size
        // clamp, which would otherwise limit the window to its previous size.
        SetWindowPos(
            hwnd,
            nullptr,
            x + desired.left,
            y + desired.top,
            desired.right - desired.left,
            desired.bottom - desired.top,
            flags | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING
        );

        {
            RECT window{};
            RECT client{};
            GetWindowRect(hwnd, &window);
            GetClientRect(hwnd, &client);
            log::debug(
                "setWindowClientSize: requested client {}x{} at {},{} -> window {}x{}+{},{} client {}x{}",
                width, height, x, y,
                window.right - window.left, window.bottom - window.top,
                window.left, window.top,
                client.right - client.left, client.bottom - client.top
            );
        }

        // The window procedure may enforce size constraints, or the new style
        // may only take effect after the first resize. Correct the window size
        // until the client area matches the requested size.
        for (int attempt = 0; attempt < 3; ++attempt) {
            RECT client{};
            if (!GetClientRect(hwnd, &client)) {
                return false;
            }

            LONG clientWidth = client.right - client.left;
            LONG clientHeight = client.bottom - client.top;
            if (clientWidth == width && clientHeight == height) {
                return true;
            }

            RECT window{};
            GetWindowRect(hwnd, &window);
            SetWindowPos(
                hwnd,
                nullptr,
                window.left,
                window.top,
                (window.right - window.left) + (width - clientWidth),
                (window.bottom - window.top) + (height - clientHeight),
                flags | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSENDCHANGING
            );
        }

        RECT client{};
        GetClientRect(hwnd, &client);
        return (client.right - client.left) == width && (client.bottom - client.top) == height;
    }

    /**
     * Switch the existing window between windowed and borderless fullscreen by
     * changing its Win32 style and size. The GL window is not recreated, so no
     * resources or scenes need to be reloaded.
     */
    bool applyWindowMode(HWND hwnd, bool fullscreen) {
        auto view = cocos2d::CCEGLView::get();
        if (!view || !IsWindow(hwnd)) {
            return false;
        }

        HMONITOR monitorHandle = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(MONITORINFO);
        if (!GetMonitorInfoW(monitorHandle, &monitorInfo)) {
            return false;
        }

        log::debug(
            "Target monitor bounds {}x{}+{},{} work {}x{}+{},{}",
            monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
            monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
            monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top,
            monitorInfo.rcWork.right - monitorInfo.rcWork.left,
            monitorInfo.rcWork.bottom - monitorInfo.rcWork.top,
            monitorInfo.rcWork.left, monitorInfo.rcWork.top
        );
        beginWindowResizeBypass(hwnd);

        log::debug(
            "System window limits: maxTrack {}x{}, virtual screen {}x{}",
            GetSystemMetrics(SM_CXMAXTRACK), GetSystemMetrics(SM_CYMAXTRACK),
            GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN)
        );

        logWindowGeometry(fullscreen ? "Before entering fullscreen" : "Before entering windowed", hwnd);

        if (fullscreen) {
            // Remember the windowed placement and style for when we go back.
            GetWindowRect(hwnd, &g_windowedState.rect);
            g_windowedState.style = GetWindowLongPtrW(hwnd, GWL_STYLE);
            g_windowedState.exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            g_windowedState.valid = true;

            RECT client{};
            if (GetClientRect(hwnd, &client) && client.right > 0 && client.bottom > 0) {
                g_windowedState.clientSize = CCSize(
                    static_cast<float>(client.right),
                    static_cast<float>(client.bottom)
                );
            }

            RECT bounds = monitorInfo.rcMonitor;

            LONG_PTR style = g_windowedState.style;
            style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
            style |= WS_POPUP | WS_VISIBLE;
            SetWindowLongPtrW(hwnd, GWL_STYLE, style);

            // Apply the new frame before calculating the window rect for the
            // desired client size.
            SetWindowPos(
                hwnd,
                nullptr,
                0, 0, 0, 0,
                SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING
            );

            setWindowClientSize(
                hwnd,
                bounds.left,
                bounds.top,
                bounds.right - bounds.left,
                bounds.bottom - bounds.top,
                SWP_SHOWWINDOW
            );
        }
        else {
            LONG_PTR style = g_windowedState.valid
                ? g_windowedState.style
                : (WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
            SetWindowLongPtrW(hwnd, GWL_STYLE, style);

            SetWindowPos(
                hwnd,
                nullptr,
                0, 0, 0, 0,
                SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING
            );

            if (g_windowedState.valid) {
                view->m_obWindowedSize = g_windowedState.clientSize;
            }

            RECT work = monitorInfo.rcWork;
            LONG width = static_cast<LONG>(view->m_obWindowedSize.width);
            LONG height = static_cast<LONG>(view->m_obWindowedSize.height);
            if (width <= 0 || height <= 0) {
                RECT fallback = g_windowedState.valid ? g_windowedState.rect : work;
                width = fallback.right - fallback.left;
                height = fallback.bottom - fallback.top;
            }

            width = (std::min)(width, static_cast<LONG>(work.right - work.left));
            height = (std::min)(height, static_cast<LONG>(work.bottom - work.top));

            LONG x = work.left + (work.right - work.left - width) / 2;
            LONG y = work.top + (work.bottom - work.top - height) / 2;

            setWindowClientSize(hwnd, x, y, width, height, 0);
        }

        logWindowGeometry(fullscreen ? "After entering fullscreen" : "After entering windowed", hwnd);

        int targetWidth = 0;
        int targetHeight = 0;
        RECT finalClient{};
        if (GetClientRect(hwnd, &finalClient)) {
            int finalWidth = static_cast<int>(finalClient.right - finalClient.left);
            int finalHeight = static_cast<int>(finalClient.bottom - finalClient.top);
            targetWidth = finalWidth;
            targetHeight = finalHeight;

            // GD's updateWindow() uses the view's current frame size as the
            // desired window size. Point it at the size established above so
            // it does not resize the window back to the old windowed size.
            view->m_obScreenSize = CCSize(
                static_cast<float>(finalWidth),
                static_cast<float>(finalHeight)
            );

            // Keep the window at this exact rect while the game refreshes its
            // view, even if it asks for a different size.
            RECT targetWindow{};
            if (GetWindowRect(hwnd, &targetWindow)) {
                g_forcedWindowX = targetWindow.left;
                g_forcedWindowY = targetWindow.top;
                g_forcedWindowWidth = targetWindow.right - targetWindow.left;
                g_forcedWindowHeight = targetWindow.bottom - targetWindow.top;
                g_forceWindowRect = true;

                log::debug(
                    "Forcing window rect {}x{}+{},{} during the view refresh",
                    g_forcedWindowWidth, g_forcedWindowHeight,
                    g_forcedWindowX, g_forcedWindowY
                );
            }
        }

        // Keep the window subclassed during the refresh so the game cannot
        // resize the window while it refreshes its view.
        g_suppressSizeMessages = false;

        // The final WM_SIZE was swallowed during the resize, so GD never
        // learned that its window changed size. Deliver it now so the game
        // updates its own window state like after a normal resize.
        if (targetWidth > 0 && targetHeight > 0 && g_resizeOrigWndProc) {
            LPARAM sizeParam = MAKELPARAM(
                static_cast<WORD>(targetWidth),
                static_cast<WORD>(targetHeight)
            );
            log::debug(
                "Delivering WM_SIZE {}x{} to the game window procedure",
                targetWidth, targetHeight
            );
            CallWindowProcW(g_resizeOrigWndProc, hwnd, WM_SIZE, SIZE_RESTORED, sizeParam);
        }

        refreshGameView(hwnd);

        g_forceWindowRect = false;

        endWindowResizeBypass();

        g_observeUntil = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        g_lastObservation = std::chrono::steady_clock::now();
        g_framesSinceObservation = 0;

        RECT afterRefresh{};
        if (targetWidth > 0 && targetHeight > 0 && GetClientRect(hwnd, &afterRefresh)) {
            int afterWidth = static_cast<int>(afterRefresh.right - afterRefresh.left);
            int afterHeight = static_cast<int>(afterRefresh.bottom - afterRefresh.top);
            if (afterWidth != targetWidth || afterHeight != targetHeight) {
                log::warn(
                    "Window size changed during the view refresh: expected {}x{}, now {}x{}",
                    targetWidth, targetHeight, afterWidth, afterHeight
                );
            }
        }

        return true;
    }

    /**
     * Move the game window onto the given monitor.
     * Returns an error message on failure, or std::nullopt on success.
     */
    std::optional<std::string> moveWindowToMonitor(HWND hwnd, MonitorData const& monitor, bool fullscreen) {
        if (!IsWindow(hwnd)) {
            return "The Geometry Dash window is not available.";
        }

        RECT target = monitor.info.rcMonitor;
        LONG targetWidth = target.right - target.left;
        LONG targetHeight = target.bottom - target.top;
        if (targetWidth <= 0 || targetHeight <= 0) {
            return "The selected monitor has invalid bounds.";
        }

        BOOL moved = FALSE;
        bool cloaked = false;

        if (fullscreen) {
            // Hide the window from the compositor while it is resized and the
            // view is refreshed, so the old-resolution frame is never shown on
            // the new monitor. DWM cloaking keeps focus and game state.
            cloaked = setWindowCloaked(hwnd, true);

            // GLFW/fullscreen windows are borderless, so moving the actual
            // Win32 window is enough to move the game to the other monitor.
            // Resize it to the monitor's full bounds in case the resolutions
            // or DPI settings of the two monitors differ.
            moved = SetWindowPos(
                hwnd,
                HWND_TOP,
                target.left,
                target.top,
                targetWidth,
                targetHeight,
                SWP_NOACTIVATE | SWP_SHOWWINDOW
            );
        }
        else {
            RECT window{};
            if (!GetWindowRect(hwnd, &window)) {
                return "Could not read the game window bounds.";
            }

            LONG width = window.right - window.left;
            LONG height = window.bottom - window.top;

            RECT work = monitor.info.rcWork;
            LONG workWidth = work.right - work.left;
            LONG workHeight = work.bottom - work.top;

            // Keep the window inside the target monitor's work area, then
            // center it there.
            width = (std::min)(width, workWidth);
            height = (std::min)(height, workHeight);

            LONG x = work.left + (workWidth - width) / 2;
            LONG y = work.top + (workHeight - height) / 2;

            moved = SetWindowPos(
                hwnd,
                nullptr,
                x,
                y,
                width,
                height,
                SWP_NOZORDER | SWP_NOACTIVATE
            );
        }

        if (!moved) {
            if (cloaked) {
                setWindowCloaked(hwnd, false);
            }
            return "Windows refused to move the game window.";
        }

        // Make sure the window really ended up on the requested monitor.
        if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != monitor.handle) {
            if (cloaked) {
                setWindowCloaked(hwnd, false);
            }
            return "The game window did not end up on the target monitor.";
        }

        refreshGameView(hwnd);

        // If the window was cloaked, it is revealed by onFramePresented()
        // after the next buffer swap, so the first visible frame already uses
        // the new viewport.

        return std::nullopt;
    }

    void rememberMonitor(MonitorData const& monitor) {
        if (!Mod::get()->getSettingValue<bool>("remember-monitor")) {
            return;
        }
        Mod::get()->setSavedValue<std::string>("last-monitor", deviceNameOf(monitor));
    }

    void tryApplyRememberedMonitor(std::string const& deviceName, int attemptsLeft, bool expectFullscreen) {
        if (attemptsLeft <= 0) {
            return;
        }
        if (!Mod::get()->getSettingValue<bool>("remember-monitor")) {
            return;
        }

        bool shouldRetry = false;

        HWND hwnd = findGameWindow();
        if (!hwnd) {
            // The game recreates its window while changing the fullscreen
            // mode, so it may not exist yet.
            shouldRetry = expectFullscreen;
        }
        else if (!isGameFullscreen(hwnd)) {
            // Windowed games are left alone, unless the game is still in the
            // middle of switching to fullscreen.
            shouldRetry = expectFullscreen;
        }
        else {
            auto monitors = getMonitors();
            auto target = std::find_if(monitors.begin(), monitors.end(), [&](MonitorData const& monitor) {
                return deviceNameOf(monitor) == deviceName;
            });

            // The remembered monitor is no longer connected.
            if (target == monitors.end()) {
                return;
            }

            auto current = monitorIndexForWindow(monitors, hwnd);
            if (current >= 0 && monitors[static_cast<std::size_t>(current)].handle == target->handle) {
                return;
            }

            if (auto error = moveWindowToMonitor(hwnd, *target, true)) {
                log::debug("Could not re-apply the remembered monitor: {}", *error);
                shouldRetry = true;
            }
            else {
                return;
            }
        }

        if (shouldRetry) {
            queueInMainThread([deviceName, attemptsLeft, expectFullscreen]() {
                tryApplyRememberedMonitor(deviceName, attemptsLeft - 1, expectFullscreen);
            });
        }
    }
}

namespace onekey {
    void switchMonitor(SwitchDirection direction) {
        HWND hwnd = findGameWindow();
        if (!hwnd) {
            showNotification("Could not find the Geometry Dash window.", NotificationIcon::Error);
            return;
        }

        auto monitors = getMonitors();
        if (monitors.size() < 2) {
            showNotification("Only one monitor is connected.", NotificationIcon::Warning);
            return;
        }

        bool fullscreen = isGameFullscreen(hwnd);
        if (!fullscreen && !Mod::get()->getSettingValue<bool>("move-windowed")) {
            showNotification(
                "The game is windowed. Enable \"Move Windowed Game\" to switch monitors.",
                NotificationIcon::Info
            );
            return;
        }

        auto current = monitorIndexForWindow(monitors, hwnd);
        if (current < 0) {
            current = 0;
        }

        auto count = static_cast<std::ptrdiff_t>(monitors.size());
        auto step = direction == SwitchDirection::Next ? 1 : -1;
        auto next = current + step;
        next = (next % count + count) % count;

        auto const& target = monitors[static_cast<std::size_t>(next)];

        if (auto error = moveWindowToMonitor(hwnd, target, fullscreen)) {
            showNotification(*error, NotificationIcon::Error);
            return;
        }

        rememberMonitor(target);

        if (Mod::get()->getSettingValue<bool>("show-notification")) {
            LONG width = target.info.rcMonitor.right - target.info.rcMonitor.left;
            LONG height = target.info.rcMonitor.bottom - target.info.rcMonitor.top;

            Notification::create(
                fmt::format("Monitor {}/{} ({}x{})", next + 1, count, width, height),
                NotificationIcon::Success
            )->show();
        }
    }

    void toggleBorderlessFullscreen() {
        auto view = cocos2d::CCEGLView::get();
        HWND hwnd = findGameWindow();
        if (!view || !hwnd) {
            return;
        }

        // Only available while the game's own Fullscreen option is unchecked.
        if (view->getIsFullscreen()) {
            g_borderlessWindowed = false;
            g_borderlessWindowedHwnd = nullptr;

            Notification::create(
                "Disable Fullscreen in the Graphics settings first.",
                NotificationIcon::Warning
            )->show();
            return;
        }

        // The window may have been recreated since we entered this state.
        if (g_borderlessWindowed && g_borderlessWindowedHwnd != hwnd) {
            g_borderlessWindowed = false;
            g_borderlessWindowedHwnd = nullptr;
        }

        bool enterBorderless = !g_borderlessWindowed;

        if (enterBorderless) {
            log::info("Switching to borderless windowed fullscreen");
        }
        else {
            log::info("Switching back to windowed mode");
        }

        if (!applyWindowMode(hwnd, enterBorderless)) {
            Notification::create(
                "Failed to change the window mode.",
                NotificationIcon::Error
            )->show();
            return;
        }

        g_borderlessWindowed = enterBorderless;
        g_borderlessWindowedHwnd = enterBorderless ? hwnd : nullptr;
    }

    void rememberWindowMonitor() {
        if (!Mod::get()->getSettingValue<bool>("remember-monitor")) {
            return;
        }

        HWND hwnd = findGameWindow();
        if (!hwnd) {
            return;
        }

        auto monitors = getMonitors();
        if (monitors.size() < 2) {
            return;
        }

        auto current = monitorIndexForWindow(monitors, hwnd);
        if (current < 0) {
            return;
        }

        auto const& monitor = monitors[static_cast<std::size_t>(current)];
        if (monitor.info.dwFlags & MONITORINFOF_PRIMARY) {
            // The game tends to put the windowed window back on the primary
            // monitor; that is not a deliberate choice by the user.
            return;
        }

        rememberMonitor(monitor);
    }

    void applyRememberedMonitor(bool expectFullscreen) {
        if (!Mod::get()->getSettingValue<bool>("remember-monitor")) {
            return;
        }

        auto deviceName = Mod::get()->getSavedValue<std::string>("last-monitor", "");
        if (deviceName.empty()) {
            return;
        }

        // Defer to the next main-thread tick so a window that is currently
        // being recreated has a chance to finish first.
        queueInMainThread([deviceName = std::move(deviceName), expectFullscreen]() {
            tryApplyRememberedMonitor(deviceName, expectFullscreen ? 10 : 1, expectFullscreen);
        });
    }

    void logViewState(char const* tag) {
        auto view = cocos2d::CCEGLView::get();
        if (!view) {
            return;
        }

        auto director = cocos2d::CCDirector::sharedDirector();
        auto display = view->getDisplaySize();

        RECT client{};
        HWND hwnd = findGameWindow();
        if (hwnd) {
            GetClientRect(hwnd, &client);
        }

        GLint viewport[4] = { -1, -1, -1, -1 };
        if (wglGetCurrentContext()) {
            glGetIntegerv(GL_VIEWPORT, viewport);
        }

        log::debug(
            "{}: client {}x{}, frame {}x{}, design {}x{}, windowed {}x{}, policy {}, win {}x{}, display {}x{}, dpi {}, scale {}/{}/{}/{}, "
            "retina {}/{}/{}, zoom {}, contentScale {}, quality {}, viewport {}x{}+{},{}",
            tag,
            client.right - client.left, client.bottom - client.top,
            view->getFrameSize().width, view->getFrameSize().height,
            view->getDesignResolutionSize().width, view->getDesignResolutionSize().height,
            view->m_obWindowedSize.width, view->m_obWindowedSize.height,
            static_cast<int>(view->m_eResolutionPolicy),
            director->getWinSize().width, director->getWinSize().height,
            display.width, display.height,
            hwnd ? GetDpiForWindow(hwnd) : 0u,
            director->getScreenScaleFactor(), director->getScreenScaleFactorMax(),
            director->getScreenScaleFactorW(), director->getScreenScaleFactorH(),
            static_cast<int>(view->m_bInRetinaMonitor),
            static_cast<int>(view->m_bRetinaEnabled),
            view->m_nRetinaFactor,
            view->m_fFrameZoomFactor,
            director->getContentScaleFactor(),
            static_cast<int>(director->getLoadedTextureQuality()),
            viewport[2], viewport[3], viewport[0], viewport[1]
        );
    }

    void onFramePresented() {
        auto now = std::chrono::steady_clock::now();
        if (now < g_observeUntil) {
            ++g_framesSinceObservation;
            if (now - g_lastObservation >= std::chrono::milliseconds(500)) {
                log::debug(
                    "Post-switch: {} frames since the last check",
                    g_framesSinceObservation
                );
                g_framesSinceObservation = 0;
                g_lastObservation = now;
                logViewState("Post-switch view");
            }
        }

        if (!g_cloakedWindow) {
            return;
        }

        auto hwnd = g_cloakedWindow;
        setWindowCloaked(hwnd, false);
    }
}

#else

namespace onekey {
    void switchMonitor(SwitchDirection) {
        if (Mod::get()->getSettingValue<bool>("show-notification")) {
            Notification::create(
                "Monitor switching is only supported on Windows.",
                NotificationIcon::Warning
            )->show();
        }
    }

    void toggleBorderlessFullscreen() {}

    void rememberWindowMonitor() {}

    void applyRememberedMonitor(bool) {}

    void logViewState(char const*) {}

    void onFramePresented() {}
}

#endif
