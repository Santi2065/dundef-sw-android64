package com.trendy.ddapp;

// Compile-time stub of the game's activity: only the members Gamepad.java uses.
class ddapp {
    public native void KeyPadChange(boolean visible);   // the game's own native method
    public native void ddportPadKey(int keyCode, boolean pressed);  // added by build.py, see gamepad.cpp
    public native void ddportPadAxis(int stick, float x, float y);  // added by build.py, see gamepad.cpp
}
