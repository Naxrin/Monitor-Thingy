#include <Geode/Geode.hpp>
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/MenuLayer.hpp>

#include "MonitorSwitch.hpp"

using namespace geode::prelude;

$on_mod(Loaded) {
    listenForKeybindSettingPresses("switch-monitor", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) {
            onekey::switchMonitor(onekey::SwitchDirection::Next);
        }
    });

    listenForKeybindSettingPresses("toggle-borderless", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat) {
            onekey::toggleBorderlessFullscreen();
        }
    });

    ButtonSettingPressedEventV3(Mod::get(), "switch-now").listen([](std::string_view button) {
        if (button == "switch") {
            onekey::switchMonitor(onekey::SwitchDirection::Next);
        }
        return false;
    }).leak();
}

class $modify(OneKeyMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) {
            return false;
        }

        onekey::logViewState("Startup view");

        // The game may start directly in fullscreen, before the user ever
        // toggles it. Restore the monitor that was selected previously.
        onekey::applyRememberedMonitor();

        return true;
    }
};

#ifdef GEODE_IS_WINDOWS
class $modify(OneKeyCCEGLView, cocos2d::CCEGLView) {
    void swapBuffers() {
        cocos2d::CCEGLView::swapBuffers();

        // Reveals the game window after a frame with the new viewport has
        // been presented (see MonitorSwitch.cpp).
        onekey::onFramePresented();
    }

    void toggleFullScreen(bool fullscreen, bool borderless, bool fix) {
        // If the user dragged the windowed game onto another monitor, follow
        // it instead of using a previously remembered monitor.
        auto view = cocos2d::CCEGLView::get();
        if (fullscreen && view && !view->getIsFullscreen()) {
            onekey::rememberWindowMonitor();
        }

        cocos2d::CCEGLView::toggleFullScreen(fullscreen, borderless, fix);

        // Changing the fullscreen mode recreates the window, which puts it
        // back on the game's default monitor. Move it to the remembered one.
        if (fullscreen) {
            onekey::applyRememberedMonitor(true);
        }
    }
};
#endif
