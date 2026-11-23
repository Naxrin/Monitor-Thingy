#pragma once

namespace onekey {
    enum class SwitchDirection {
        Next,
        Previous,
    };

    /**
     * Move the Geometry Dash window to the next (or previous) monitor.
     *
     * If the game is fullscreen, the window is resized to fill the target
     * monitor. If it is windowed, the window is moved and centered on the
     * target monitor instead (can be disabled in the mod settings).
     *
     * Shows a notification with the result, unless notifications are disabled
     * in the mod settings.
     */
    void switchMonitor(SwitchDirection direction);

    /**
     * Toggle between borderless fullscreen (fullscreen, borderless and fix all
     * enabled) and windowed mode.
     *
     * Uses the game's own screen mode switching, so the window and graphics
     * are reloaded exactly like GD's video settings do.
     */
    void toggleBorderlessFullscreen();

    /**
     * Remember the monitor that currently contains the game window.
     *
     * Called right before the game enters fullscreen, so that if the user
     * dragged the windowed game onto a monitor, the fullscreen window follows
     * it instead of an older remembered monitor.
     */
    void rememberWindowMonitor();

    /**
     * Move a fullscreen game back to the monitor that was last selected with
     * `switchMonitor`.
     *
     * The game recreates its window when the fullscreen mode changes, so this
     * is called after such a change (and when the main menu is first shown)
     * to keep the chosen monitor. Does nothing if the game is windowed or if
     * remembering the monitor is disabled in the mod settings.
     *
     * @param expectFullscreen Set when the game has just been switched to
     * fullscreen; the move is then retried for a few frames in case the game
     * is still recreating its window.
     */
    void applyRememberedMonitor(bool expectFullscreen = false);

    /**
     * Log the current render view state (sizes, policy, DPI and screen scale
     * factors). Used for diagnosing resolution changes.
     */
    void logViewState(char const* tag);

    /**
     * Called after the game presents a frame. If the game window was
     * temporarily cloaked while switching monitors, reveal it now that a frame
     * with the updated viewport has been drawn.
     */
    void onFramePresented();
}
