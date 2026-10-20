// Physical controllers. port/java/.../Gamepad.java forwards Android controller input here and we
// feed it to the engine the way its Xperia Play build did:
//  - buttons become XboxTypeS_* key events in the engine's Android key queue (what NativeKeyEvent
//    does for keyboard keys after looking them up in its map, which has no controller buttons);
//  - sticks go to ActiveLeftAxis / ActiveRightAxis, which FKdViewport::ProcessInput turns into the
//    XboxTypeS_LeftX/Y and RightX/Y axes while GIsMobileGamePadActive is set.
// The game's own Tegra-UDKInput.ini then maps them: left stick move, right stick camera, A jump,
// X/Y activate, B action wheel, RB attack, LT alt attack, D-pad hotkeys 1-4, Start pause,
// Back zoom, R3 minimap; menus navigate with D-pad / stick and A.
#include <jni.h>

#include "core.h"

extern GuestLib g_lib;

namespace {
// libDunDef-Android.so 7.6 addresses (the queue object is a file-local static, no symbol)
constexpr u32 kKeyQueuePtr = 0x10c5d74;  // TThreadSafeQueue<AndroidKeyEvent>*, see GetNextKeyEvent
constexpr u32 kKeyPadGate = 0xe0f060;    // KeyPadChange: "beq return" when !GIsXperiaPlay
constexpr u16 kThumbBeqReturn = 0xd007, kThumbNop = 0x46c0;

struct Button {
    int android;      // KeyEvent.KEYCODE_*
    const char* key;  // engine FName global
};
constexpr Button kButtons[] = {
    {96, "KEY_XboxTypeS_A"},              {97, "KEY_XboxTypeS_B"},
    {99, "KEY_XboxTypeS_X"},              {100, "KEY_XboxTypeS_Y"},
    {102, "KEY_XboxTypeS_LeftShoulder"},  {103, "KEY_XboxTypeS_RightShoulder"},
    {104, "KEY_XboxTypeS_LeftTrigger"},   {105, "KEY_XboxTypeS_RightTrigger"},
    {106, "KEY_XboxTypeS_LeftThumbstick"}, {107, "KEY_XboxTypeS_RightThumbstick"},
    {108, "KEY_XboxTypeS_Start"},         {109, "KEY_XboxTypeS_Back"},
    {19, "KEY_XboxTypeS_DPad_Up"},        {20, "KEY_XboxTypeS_DPad_Down"},
    {21, "KEY_XboxTypeS_DPad_Left"},      {22, "KEY_XboxTypeS_DPad_Right"},
};

u32 key_fname[256];  // guest address of the FName for each Android keycode, 0 = not a button
u32 insert_newest, left_axis, right_axis;

void JNICALL pad_key(JNIEnv*, jobject, jint code, jboolean pressed) {
    if (code < 0 || code >= 256 || !key_fname[code]) return;
    u32 queue = mem::rd32(g_lib.base + kKeyQueuePtr);
    if (!queue) return;  // engine not started yet
    // AndroidKeyEvent: { EInputEvent (0 pressed, 1 released), FName key, unused }
    thread_local u32 ev = mem::g(mem::calloc(1, 16));
    mem::wr32(ev, pressed ? 0 : 1);
    mem::wr32(ev + 4, mem::rd32(key_fname[code]));
    mem::wr32(ev + 8, mem::rd32(key_fname[code] + 4));
    call_guest(insert_newest, {queue, ev});
}

void JNICALL pad_axis(JNIEnv*, jobject, jint stick, jfloat x, jfloat y) {
    float v[2] = {x, -y};  // engine convention (NativeAxisEvent): stick up is +Y
    memcpy(mem::h(stick ? right_axis : left_axis), v, sizeof v);
}
}  // namespace

void init_gamepad(JNIEnv* env) {
    // KeyPadChange only switched the HUD to gamepad mode on the Xperia Play; let it work everywhere
    auto* gate = mem::h<u16>(g_lib.base + kKeyPadGate);
    insert_newest = g_lib.sym("_ZN16TThreadSafeQueueI15AndroidKeyEventE12InsertNewestERKS0_");
    left_axis = g_lib.sym("ActiveLeftAxis");
    right_axis = g_lib.sym("ActiveRightAxis");
    if (*gate != kThumbBeqReturn || !insert_newest || !left_axis || !right_axis) {
        LOGW("gamepad: unexpected game library, controller support disabled");
        return;
    }
    *gate = kThumbNop;
    for (const Button& b : kButtons) key_fname[b.android] = g_lib.sym(b.key);

    jclass cls = env->FindClass("com/trendy/ddapp/ddapp");
    const JNINativeMethod natives[] = {
        {"ddportPadKey", "(IZ)V", reinterpret_cast<void*>(pad_key)},
        {"ddportPadAxis", "(IFF)V", reinterpret_cast<void*>(pad_axis)},
    };
    if (!cls || env->RegisterNatives(cls, natives, 2) != JNI_OK) {
        env->ExceptionClear();
        LOGW("gamepad: could not register natives (APK built without the Gamepad patch?)");
    }
}
