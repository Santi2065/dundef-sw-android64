package com.trendy.ddapp;

import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

/**
 * Physical controller support (Bluetooth / USB gamepads).
 *
 * The game's Xperia Play build already had a full controller layout (Tegra-UDKInput.ini, Xbox
 * button names); it was only reachable on that phone. This class forwards Android controller
 * input to the loader (port/src/gamepad.cpp), which feeds it to the engine the same way:
 * buttons as XboxTypeS_* keys, sticks as the left/right analog axes.
 *
 * The HUD follows the last input used: a controller button or stick switches the game into its
 * gamepad mode (KeyPadChange, as when the Xperia Play's gamepad slid out), touching the screen
 * switches it back.
 *
 * Compiled to patches/smali/com/trendy/ddapp/Gamepad.smali (see port/java/README.md).
 */
final class Gamepad {
    private static boolean active, swallowing;
    private static final boolean[] down = new boolean[256];  // per keycode: drops repeats and the
                                                              // duplicates of triggers sent both as
                                                              // keys and as axes

    /** From ddapp.dispatchKeyEvent. */
    static boolean onKey(ddapp a, KeyEvent e) {
        int code = e.getKeyCode();
        boolean dpad = code >= KeyEvent.KEYCODE_DPAD_UP && code <= KeyEvent.KEYCODE_DPAD_RIGHT;
        if (code == KeyEvent.KEYCODE_BUTTON_MODE
                || !(KeyEvent.isGamepadButton(code) || dpad && fromController(e.getSource()))) {
            return false;
        }
        int action = e.getAction();
        if (action == KeyEvent.ACTION_DOWN || action == KeyEvent.ACTION_UP) {
            key(a, code, action == KeyEvent.ACTION_DOWN);
        }
        return true;
    }

    /** From ddapp.dispatchGenericMotionEvent: sticks, analog triggers, hat D-pad. */
    static boolean onMotion(ddapp a, MotionEvent e) {
        if ((e.getSource() & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK
                || e.getAction() != MotionEvent.ACTION_MOVE) {
            return false;
        }
        float lx = e.getAxisValue(MotionEvent.AXIS_X), ly = e.getAxisValue(MotionEvent.AXIS_Y);
        float rx = e.getAxisValue(MotionEvent.AXIS_Z), ry = e.getAxisValue(MotionEvent.AXIS_RZ);
        if (Math.max(Math.max(Math.abs(lx), Math.abs(ly)), Math.max(Math.abs(rx), Math.abs(ry))) > 0.5f) {
            setActive(a, true);
        }
        a.ddportPadAxis(0, lx, ly);
        a.ddportPadAxis(1, rx, ry);
        trigger(a, KeyEvent.KEYCODE_BUTTON_L2,
                Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE)));
        trigger(a, KeyEvent.KEYCODE_BUTTON_R2,
                Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS)));
        hat(a, e.getAxisValue(MotionEvent.AXIS_HAT_X), KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT);
        hat(a, e.getAxisValue(MotionEvent.AXIS_HAT_Y), KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN);
        return true;
    }

    /**
     * From ddapp.onTouchEvent: touching the screen brings the touch HUD back. That first gesture
     * only switches the HUD and is swallowed (returns true): the game would read its press with
     * the gamepad HUD still up and lose the release, leaving the hero attacking forever.
     */
    static boolean onTouch(ddapp a, MotionEvent e) {
        int action = e.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN && active) {
            setActive(a, false);
            swallowing = true;
        }
        boolean swallow = swallowing;
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) swallowing = false;
        return swallow;
    }

    private static boolean fromController(int source) {
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private static void trigger(ddapp a, int code, float value) {
        if (value > 0.5f) key(a, code, true);
        else if (value < 0.3f) key(a, code, false);  // hysteresis: no chatter around the threshold
    }

    private static void hat(ddapp a, float value, int negative, int positive) {
        key(a, negative, value < -0.5f);
        key(a, positive, value > 0.5f);
    }

    private static void key(ddapp a, int code, boolean pressed) {
        if (code < 0 || code >= down.length || down[code] == pressed) return;
        down[code] = pressed;
        if (pressed) setActive(a, true);
        a.ddportPadKey(code, pressed);
    }

    private static void setActive(ddapp a, boolean on) {
        if (active != on) {
            active = on;
            a.KeyPadChange(on);
        }
    }
}
