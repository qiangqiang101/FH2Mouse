// FH2Mouse: a mouse cursor for Forza Horizon 2's menus.
//
// The game's UI library (AVUI, a C++ take on WPF) has WPF's mouse code, but the Xbox build never
// feeds it and its hit testing finds nothing. Once a frame, on the UI thread, this mod:
//   - finds the control under the cursor itself, from the active window's visual tree,
//   - moves the keyboard focus to it (what the d-pad would do),
//   - turns clicks into the keys the controller sends the menus: left = A, right = B,
//     wheel = d-pad up/down, back/forward side buttons = LB/RB.
// Pages then behave exactly as they do with a controller.
#include "fh2mod.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

const FH2ModApi* g_api = nullptr;
FH2Mod* g_mod          = nullptr;
uint8_t* g_base        = nullptr;

// ---- the game (24 November 2015 build), offsets from its base -------------------------------

constexpr uintptr_t RVA_UI_UPDATE_SLOT = 0x2ECFBF8;   // UserInterface vtable slot 15: Update(this, float dt)
constexpr uintptr_t RVA_UI_UPDATE      = 0x142C5E0;   // ... which is this; it runs the pad-to-key pump
constexpr uintptr_t RVA_APPLICATION    = 0x36B0418;   // AVUI::Application*; render size floats at +0x50/+0x54
constexpr uintptr_t RVA_GET_WM         = 0x157D110;   // WindowManager* (): Application vtable +0x78
constexpr uintptr_t RVA_IS_TYPE        = 0x154F960;   // bool Type::IsTypeOf(type, base)
constexpr uintptr_t RVA_FOCUS          = 0x14D01D0;   // Keyboard::Focus(Ptr* out, IInputElement*)
constexpr uintptr_t RVA_FOCUSED        = 0x1519FD0;   // IInputElement* Keyboard::FocusedElement()
constexpr uintptr_t RVA_KEY_CODE       = 0x1514530;   // the key code the window manager takes, from a WPF Key
constexpr uintptr_t RVA_OBJECT_FREE    = 0x0257730;   // frees an AVUI object whose refcount (+0x10) hit 0
constexpr uintptr_t RVA_TO_ANCESTOR    = 0x1576BB0;   // bool Visual::TransformToAncestor(v, ancestor, inverse, GeneralTransform**, Matrix*)
constexpr uintptr_t RVA_TRANSFORM_PT   = 0x146E570;   // Point* Matrix::Transform(Matrix*, Point* out, const Point* in)
constexpr uintptr_t RVA_GET_VALUE      = 0x1548310;   // boxed value of a property in an object's value store
constexpr uintptr_t RVA_UNSET_VALUE    = 0x160C340;   // DependencyProperty::UnsetValue
constexpr uintptr_t RVA_OPACITY_STORED = 0x36A61D0;   // bool: Opacity's index (next dword) is known
constexpr uintptr_t RVA_OPACITY_DEFAULT = 0x36A61C8;  // boxed default Opacity
constexpr uintptr_t RVA_DP_GET_VALUE   = 0x15E0F10;   // DependencyObject::GetValue(obj, boxed* out, property)
constexpr uintptr_t RVA_IS_FOCUSED_DP  = 0x36A48B8;   // UIElement::IsFocusedProperty
constexpr uintptr_t RVA_FOCUSABLE_DP   = 0x36A76F8;   // UIElement::FocusableProperty
constexpr uintptr_t RVA_DATA_CONTEXT_DP = 0x36C7128;  // FrameworkElement::DataContextProperty
constexpr uintptr_t RVA_SELECTED_ITEM_DP = 0x36CF118; // Selector::SelectedItemProperty

// The map: MapScene (the page) and the MapViewControllerInteractive behind it, which pans the
// map under a cursor fixed in the middle. The map is seen from straight above, `zoom` world
// units from the middle to the top and bottom edges.
constexpr uintptr_t VT_MAP_SCENE       = 0x2DFED48;
constexpr uintptr_t VT_MAP_CONTROLLER  = 0x2D9CD48;
constexpr uintptr_t MAP_CONTROLLER     = 0x410;   // MapScene: its controller
constexpr uintptr_t MAP_POS_X          = 0x2C0;   // controller: the map cursor, in the world (x, y, z)
constexpr uintptr_t MAP_POS_Z          = 0x2C8;
constexpr uintptr_t MAP_TARGET_X       = 0x2D0;   // controller: where the camera is going (x, y, z)
constexpr uintptr_t MAP_TARGET_Z       = 0x2D8;
constexpr uintptr_t MAP_CAM_X          = 0x27C;   // controller: where it is (eased x, then z)
constexpr uintptr_t MAP_CAM_Z          = 0x290;
constexpr uintptr_t MAP_ZOOM           = 0x2E0;   // controller: zoom, 0..1
constexpr uintptr_t MAP_ZOOM_SHOWN     = 0x2E4;   // ... as the distance from the middle to the top edge
constexpr uintptr_t MAP_LEASH          = 0x3B0;   // controller: how far the cursor goes before the camera follows
constexpr uintptr_t MAP_ZOOM_MIN       = 0x384;
constexpr uintptr_t MAP_ZOOM_MAX       = 0x388;

constexpr uintptr_t WM_ACTIVE_WINDOW = 0x68;   // WindowManager: the active window (a UIWindow)
constexpr uintptr_t WM_KEY_SLOT      = 0xD8;   // WindowManager vtable: key(wm, up, code, 0, 0, 0)
constexpr uintptr_t OBJ_TYPE         = 0x18;   // AVUI object: its Type, once asked for (else vtable +0x18)
constexpr uintptr_t VISUAL_PARENT    = 0x70;   // Visual: its visual parent (the "Parent" property's getter)
constexpr uintptr_t UIELEMENT_INPUT  = 0xB8;   // UIElement: its IInputElement
constexpr uintptr_t VISUAL_FLAGS     = 0xA8;   // Visual: bit 3 Opacity is set; bits 5/6 laid out
constexpr uintptr_t RENDER_SIZE      = 0x134;  // UIElement: RenderSize (two floats), when laid out
constexpr uintptr_t VT_CHILD_COUNT   = 0x98;   // Visual vtable: int VisualChildrenCount()
constexpr uintptr_t VT_CHILD         = 0xA0;   // Visual vtable: Visual* GetVisualChild(int)
constexpr uintptr_t VT_GT_TRANSFORM  = 0xE0;   // GeneralTransform vtable: bool TryTransform(Point* in, Point* out)

// AVUI's static Type objects.
constexpr uintptr_t TYPE_VISUAL        = 0x36A25B0;
constexpr uintptr_t TYPE_UIELEMENT     = 0x36A3520;
constexpr uintptr_t TYPE_BUTTON_BASE   = 0x36ABE70;
constexpr uintptr_t TYPE_REPEAT_BUTTON = 0x36D0F10;
constexpr uintptr_t TYPE_LIST_BOX_ITEM = 0x3689AE0;
constexpr uintptr_t TYPE_COMBO_ITEM    = 0x36B79D0;
constexpr uintptr_t TYPE_TAB_ITEM      = 0x36DABC0;
constexpr uintptr_t TYPE_TREE_ITEM     = 0x36DB0C0;
constexpr uintptr_t TYPE_MENU_ITEM     = 0x3686B00;
constexpr uintptr_t TYPE_SLIDER        = 0x36D0070;
constexpr uintptr_t TYPE_SELECTOR      = 0x36CE8F0;   // ListBox, the game's ItemsSelector, ...
constexpr uintptr_t TYPE_SPINNER       = 0x3681F40;   // the game's SpinnerControl
constexpr uintptr_t TYPE_HELP_BUTTON   = 0x34E3D00;   // the game's ControllerButtonControl (the help bar's "(A) Select")
constexpr uintptr_t RVA_HELP_BUTTON_DP = 0x34E35F0;   // ControllerButtonControl::ButtonProperty (a ControllerButtonType)

// WPF Key values, as the pad sends them (game-notes.md, "The pad in the menus").
constexpr int KEY_A = 18;          // Space
constexpr int KEY_B = 45;          // B
constexpr int KEY_UP = 24, KEY_DOWN = 26;
constexpr int KEY_LB = 19, KEY_RB = 20;   // PageUp, PageDown
constexpr int KEY_X = 67, KEY_Y = 68;
constexpr int KEY_RS_CLICK = 119;   // RightCtrl: what the pad's right stick click sends
constexpr int KEY_LEFT = 23, KEY_RIGHT = 25;

struct Expect {
    uintptr_t rva;
    uint8_t bytes[12];
};
const Expect kExpected[] = {
    {RVA_UI_UPDATE, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0xC5, 0xFA, 0x10, 0x05, 0x12, 0xAD}},
    {RVA_IS_TYPE, {0x40, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE}},
    {RVA_FOCUS, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B}},
    {RVA_FOCUSED, {0x48, 0x83, 0xEC, 0x28, 0xE8, 0x37, 0x8E, 0xFB, 0xFF, 0x48, 0x8B, 0x88}},
    {RVA_KEY_CODE, {0xFF, 0xC9, 0x81, 0xF9, 0xAA, 0x00, 0x00, 0x00, 0x0F, 0x87, 0x0C, 0x04}},
    {RVA_GET_WM, {0x48, 0x8B, 0x0D, 0x01, 0x33, 0x13, 0x02, 0x48, 0x85, 0xC9, 0x74, 0x07}},
    {RVA_OBJECT_FREE, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8, 0xB2, 0xA2}},
    {RVA_TO_ANCESTOR, {0x4C, 0x89, 0x4C, 0x24, 0x20, 0x44, 0x88, 0x44, 0x24, 0x18, 0x48, 0x89}},
    {RVA_TRANSFORM_PT, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x49, 0x8B, 0x00, 0x4C, 0x8D, 0x42}},
    {RVA_GET_VALUE, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48}},
    {RVA_UNSET_VALUE, {0x48, 0x8D, 0x05, 0xB1, 0xE1, 0x0A, 0x02, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC}},
    {RVA_DP_GET_VALUE, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10, 0x55, 0x56, 0x57, 0x48, 0x83}},
};

using UpdateFn  = void (*)(void* self, float dt);
using GetWmFn   = uint8_t* (*)();
using IsTypeFn  = bool (*)(void* type, void* base);
using FocusFn   = void (*)(void** out, void* element);
using FocusedFn = void** (*)();
using KeyCodeFn = int (*)(int key);
using KeyFn     = uint8_t (*)(void* wm, uint8_t up, int code, int scan, uint8_t a, uint8_t b);
using FreeFn    = void (*)(void* obj);
using TypeOfFn  = void* (*)(void* obj);
using ToAncestorFn  = bool (*)(void* v, void* ancestor, uint8_t inverse, void** gt, float* matrix);
using TransformPtFn = float* (*)(const float* matrix, float* out, const float* in);
using GetValueFn    = uint8_t* (*)(void* obj, int index);
using UnsetFn       = uint8_t* (*)();
using CountFn       = int (*)(void* v);
using ChildFn       = uint8_t* (*)(void* v, int i);
using GtFn          = bool (*)(void* gt, const float* in, float* out);
using DpGetValueFn  = void* (*)(void* obj, uint8_t** out, void* property);

UpdateFn o_update = nullptr;

template <class F> F game(uintptr_t rva) { return reinterpret_cast<F>(g_base + rva); }
template <class T> T at(const void* p, uintptr_t off) { return *reinterpret_cast<T const*>(static_cast<const uint8_t*>(p) + off); }

// ---- settings ----------------------------------------------------------------------------------

struct Settings {
    bool enabled      = true;
    bool hover_focus  = true;
    DWORD hide_ms     = 3000;   // the cursor hides this long after the mouse last moved (0: never)
    bool debug        = false;  // log what's under the cursor
} g_cfg;

std::wstring g_dir;

void load_settings() {
    g_dir = std::wstring(g_api->azimuth_dir) + L"\\FH2Mouse";
    CreateDirectoryW(g_dir.c_str(), nullptr);
    const std::wstring ini = g_dir + L"\\FH2Mouse.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const char text[] =
            "; FH2Mouse: a mouse cursor for Forza Horizon 2's menus. 0 = off, 1 = on.\r\n"
            "; Left click = A (or select what's clicked), right click = B, wheel = up/down,\r\n"
            "; mouse side buttons = LB/RB, middle click = right stick click. Help bar buttons\r\n"
            "; are clickable. Map: drag to pan, click to select or set a waypoint, wheel to zoom.\r\n"
            "; Garage: drag empty space to turn the camera, wheel to zoom; W A S D move a decal.\r\n"
            "; Photo mode: drag to turn, W A S D to move, Q / E down / up, wheel to zoom.\r\n"
            "[Mouse]\r\n"
            "Enabled=1\r\n"
            "\r\n"
            "; Moving the mouse over a button, list row or slider selects it.\r\n"
            "HoverSelects=1\r\n"
            "\r\n"
            "; Hide the cursor after this many seconds without moving the mouse (0 = never).\r\n"
            "HideAfterSeconds=3\r\n"
            "\r\n"
            "; Log what is under the cursor to fh2-mods\\platform.log.\r\n"
            "Debug=0\r\n";
        HANDLE f = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD n = 0;
            WriteFile(f, text, sizeof text - 1, &n, nullptr);
            CloseHandle(f);
        }
    }
    g_cfg.enabled     = GetPrivateProfileIntW(L"Mouse", L"Enabled", 1, ini.c_str()) != 0;
    g_cfg.hover_focus = GetPrivateProfileIntW(L"Mouse", L"HoverSelects", 1, ini.c_str()) != 0;
    g_cfg.hide_ms     = GetPrivateProfileIntW(L"Mouse", L"HideAfterSeconds", 3, ini.c_str()) * 1000u;
    g_cfg.debug       = GetPrivateProfileIntW(L"Mouse", L"Debug", 0, ini.c_str()) != 0;
}

void logf(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    g_api->log(g_mod, line);
}

// ---- input from the window (its own thread) ----------------------------------------------------

enum class Ev { Move, LDown, LUp, RDown, RUp, Wheel, X1Down, X1Up, X2Down, X2Up, MDown, MUp };
struct Event {
    Ev type;
    int a;   // wheel delta
};

std::mutex g_mx;
std::vector<Event> g_events;   // guarded by g_mx
std::atomic<HWND> g_hwnd{nullptr};
WNDPROC g_orig_proc = nullptr;
std::atomic<int> g_cx{-1}, g_cy{-1};       // the cursor, in client pixels
std::atomic<DWORD> g_last_move{0};          // GetTickCount of the last real movement
std::atomic<DWORD> g_last_frame{0};         // GetTickCount of the last UI frame
std::atomic<bool> g_framed{false};          // a UI frame has run (the game is up)
std::atomic<bool> g_shown{false};           // what the window was last told
HCURSOR g_arrow = nullptr;

bool game_active() { return g_framed.load() && GetTickCount() - g_last_frame.load() < 1000; }

bool cursor_wanted() {
    if (!g_cfg.enabled || !game_active() || !g_last_move.load()) return false;
    return g_cfg.hide_ms == 0 || GetTickCount() - g_last_move.load() < g_cfg.hide_ms;
}

void push(Ev type, int a = 0) {
    std::lock_guard<std::mutex> lock(g_mx);
    if (g_events.size() < 256) g_events.push_back({type, a});
}

// The cursor's shape: the window is asked on every mouse move. Azimuth hides it there.
LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_SETCURSOR && LOWORD(l) == HTCLIENT && g_cfg.enabled && game_active()) {
        const bool show = cursor_wanted();
        g_shown = show;
        SetCursor(show ? g_arrow : nullptr);
        return TRUE;
    }
    return CallWindowProcW(g_orig_proc, h, msg, w, l);
}

// Mouse input. Azimuth keeps the usual mouse messages from its window, so the cursor and the
// buttons are read on the UI thread each frame (poll_mouse), while the game window is in front
// and the cursor is over it. Only the wheel, which can't be read that way, comes from a
// low-level mouse hook; Windows drops such a hook when it's slow to answer, so it's set again
// every few seconds, on a thread of its own that does nothing else. Nothing is swallowed.
LRESULT CALLBACK mouse_hook(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && w == WM_MOUSEWHEEL) {
        HWND h = g_hwnd.load();
        if (h && g_cfg.enabled && game_active() && GetForegroundWindow() == h) {
            const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(l);
            g_last_move = GetTickCount();
            push(Ev::Wheel, static_cast<short>(HIWORD(m->mouseData)));
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

HMODULE g_self = nullptr;

DWORD WINAPI hook_thread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HHOOK hook = nullptr;
    for (;;) {
        if (hook) UnhookWindowsHookEx(hook);
        hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_hook, g_self, 0);
        if (!hook) {
            logf("couldn't set the mouse hook (error %lu)", GetLastError());
            return 0;
        }
        const DWORD until = GetTickCount() + 3000;
        for (DWORD now = GetTickCount(); now < until; now = GetTickCount()) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, until - now, QS_ALLINPUT);
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        }
    }
}

// The cursor and the buttons, once a frame on the UI thread.
void poll_mouse() {
    static bool held[5] = {};
    static const int vk[5] = {VK_LBUTTON, VK_RBUTTON, VK_XBUTTON1, VK_XBUTTON2, VK_MBUTTON};
    static const Ev press[5] = {Ev::LDown, Ev::RDown, Ev::X1Down, Ev::X2Down, Ev::MDown};
    static const Ev lift[5]  = {Ev::LUp, Ev::RUp, Ev::X1Up, Ev::X2Up, Ev::MUp};
    HWND h = g_hwnd.load();
    const bool front = h && GetForegroundWindow() == h;
    POINT p{};
    RECT r{};
    bool inside = false;
    if (front && GetCursorPos(&p) && ScreenToClient(h, &p) && GetClientRect(h, &r)) inside = PtInRect(&r, p) != 0;
    if (inside && (p.x != g_cx.load() || p.y != g_cy.load())) {
        g_cx        = p.x;
        g_cy        = p.y;
        g_last_move = GetTickCount();
        push(Ev::Move);
    }
    for (int i = 0; i < 5; ++i) {
        const bool down = front && (GetAsyncKeyState(vk[i]) & 0x8000);
        if (down == held[i]) continue;
        if (down && !inside) continue;   // a press elsewhere isn't ours
        held[i] = down;
        if (down) g_last_move = GetTickCount();
        push(down ? press[i] : lift[i]);
    }
}

// The game window: Azimuth's, of this process.
HWND find_game_window() {
    for (HWND h = FindWindowExW(nullptr, nullptr, L"Azimuth.Game.Win32", nullptr); h;
         h = FindWindowExW(nullptr, h, L"Azimuth.Game.Win32", nullptr)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) return h;
    }
    return nullptr;
}

DWORD WINAPI window_thread(LPVOID) {
    for (;;) {
        Sleep(250);
        HWND h = find_game_window();
        if (!h || h == g_hwnd.load()) {
            // Tell the window when the cursor should hide (or show) without the mouse moving.
            HWND cur = g_hwnd.load();
            if (cur && cursor_wanted() != g_shown.load()) {
                POINT p;
                GetCursorPos(&p);
                if (WindowFromPoint(p) == cur) PostMessageW(cur, WM_SETCURSOR, reinterpret_cast<WPARAM>(cur), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
            }
            continue;
        }
        HWND old = g_hwnd.exchange(h);
        if (old && IsWindow(old) && g_orig_proc) SetWindowLongPtrW(old, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_orig_proc));
        g_orig_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wndproc)));
        RECT r{};
        GetClientRect(h, &r);
        logf("watching the game window (%ldx%ld)", r.right, r.bottom);
    }
}

// ---- the game's UI (UI thread only) --------------------------------------------------------------

void* type_of(uint8_t* obj) {
    void* t = at<void*>(obj, OBJ_TYPE);
    if (!t) t = reinterpret_cast<TypeOfFn>((*reinterpret_cast<void***>(obj))[3])(obj);
    return t;
}

bool is(void* type, uintptr_t base_rva) { return type && game<IsTypeFn>(RVA_IS_TYPE)(type, g_base + base_rva); }

const char* rtti_name(const void* obj) {
    auto* vt  = *reinterpret_cast<uint8_t* const*>(obj);
    auto* col = *reinterpret_cast<uint8_t* const*>(vt - 8);
    if (col < g_base || col >= g_base + 0x3A18000) return "?";
    return reinterpret_cast<const char*>(g_base + at<uint32_t>(col, 12) + 0x10);
}

void release(uint8_t* obj) {
    if (obj && InterlockedDecrement(reinterpret_cast<volatile LONG*>(obj + 0x10)) == 0) game<FreeFn>(RVA_OBJECT_FREE)(obj);
}

uint8_t* window_manager() { return game<GetWmFn>(RVA_GET_WM)(); }

uint8_t* focused_object() {
    void** iface = game<FocusedFn>(RVA_FOCUSED)();
    if (!iface) return nullptr;
    return reinterpret_cast<uint8_t* (*)(void*)>((*reinterpret_cast<void***>(iface))[0])(iface);
}

void focus(uint8_t* el) {
    void** iface = reinterpret_cast<void**>(el + UIELEMENT_INPUT);
    reinterpret_cast<void (*)(void*)>((*reinterpret_cast<void***>(iface))[3])(iface);   // the reference Focus takes
    void* out[4] = {};
    game<FocusFn>(RVA_FOCUS)(out, iface);
    release(static_cast<uint8_t*>(out[1]));
}

void send_key_now(int key, bool up) {
    uint8_t* wm = window_manager();
    if (!wm) return;
    const int code = game<KeyCodeFn>(RVA_KEY_CODE)(key);
    auto fn = *reinterpret_cast<KeyFn*>(*reinterpret_cast<uint8_t**>(wm) + WM_KEY_SLOT);
    fn(wm, up ? 1 : 0, code, 0, 0, 0);
}

// Key events go to the window manager one per frame, the way the pad's do: menus that look at
// what's held each frame miss a press and release in the same frame.
struct KeyEvent {
    int key;
    bool up;
    int wait;   // frames to wait before this one
};
std::vector<KeyEvent> g_keys;   // UI thread only

void send_key(int key, bool up, int wait = 0) {
    if (g_keys.size() < 64) g_keys.push_back({key, up, wait});
}

void flush_one_key() {
    if (g_keys.empty()) return;
    if (g_keys.front().wait > 0) {
        --g_keys.front().wait;
        return;
    }
    const KeyEvent k = g_keys.front();
    g_keys.erase(g_keys.begin());
    if (g_cfg.debug) logf("key %d %s", k.key, k.up ? "up" : "down");
    send_key_now(k.key, k.up);
}

uint8_t* g_hovered = nullptr;   // compared only, never followed: it may be gone by now
bool g_a_held = false, g_b_held = false, g_lb_held = false, g_rb_held = false;

template <class F> F vfn(void* obj, uintptr_t off) { return *reinterpret_cast<F*>(*static_cast<uint8_t**>(obj) + off); }

// The cursor in the UI's own pixels (the render size, which the window may stretch).
bool cursor_ui(float pt[2]) {
    HWND h       = g_hwnd.load();
    uint8_t* app = at<uint8_t*>(g_base, RVA_APPLICATION);
    if (!h || !app) return false;
    RECT r{};
    GetClientRect(h, &r);
    if (r.right <= 0 || r.bottom <= 0) return false;
    float w = at<float>(app, 0x50), hgt = at<float>(app, 0x54);
    if (w <= 0 || hgt <= 0) w = static_cast<float>(r.right), hgt = static_cast<float>(r.bottom);
    pt[0] = g_cx.load() * w / r.right;
    pt[1] = g_cy.load() * hgt / r.bottom;
    return true;
}

float opacity(uint8_t* v) {
    if (!((at<uint32_t>(v, VISUAL_FLAGS) >> 3) & 1)) return 1.f;
    uint8_t* boxed = at<uint8_t*>(g_base, RVA_OPACITY_DEFAULT);
    if (at<uint8_t>(g_base, RVA_OPACITY_STORED)) {
        uint8_t* o = game<GetValueFn>(RVA_GET_VALUE)(v, at<int>(g_base, RVA_OPACITY_STORED + 4));
        if (o && o != game<UnsetFn>(RVA_UNSET_VALUE)()) boxed = o;
    }
    return boxed ? at<float>(boxed, 0x28) : 1.f;
}

// Where `v` (a laid-out UIElement) is laid out, in `root`'s pixels.
bool layout_bounds(uint8_t* v, uint8_t* root, float out[4]) {
    const uint32_t flags = at<uint32_t>(v, VISUAL_FLAGS);
    if (!((flags >> 5) & 1) && !((flags >> 6) & 1)) return false;
    const float w = at<float>(v, RENDER_SIZE), h = at<float>(v, RENDER_SIZE + 4);
    if (!(w > 0) || !(h > 0)) return false;
    alignas(16) float matrix[16] = {};
    void* gt          = nullptr;
    const bool simple = game<ToAncestorFn>(RVA_TO_ANCESTOR)(v, root, 0, &gt, matrix);
    const float corners[4][2] = {{0, 0}, {w, 0}, {0, h}, {w, h}};
    bool ok = simple || gt;
    out[0] = out[1] = 1e30f;
    out[2] = out[3] = -1e30f;
    for (int i = 0; ok && i < 4; ++i) {
        float p[2] = {corners[i][0], corners[i][1]};
        if (simple) game<TransformPtFn>(RVA_TRANSFORM_PT)(matrix, p, corners[i]);
        else ok = vfn<GtFn>(gt, VT_GT_TRANSFORM)(gt, corners[i], p);
        out[0] = std::min(out[0], p[0]), out[1] = std::min(out[1], p[1]);
        out[2] = std::max(out[2], p[0]), out[3] = std::max(out[3], p[1]);
    }
    release(static_cast<uint8_t*>(gt));
    if (ok) return true;
    // An animated transform on the way up: the layout offsets alone (VisualOffset, +0x90).
    float x = 0, y = 0;
    for (uint8_t* e = v; e && e != root; e = at<uint8_t*>(e, VISUAL_PARENT)) {
        if (!is(type_of(e), TYPE_VISUAL)) return false;
        x += at<float>(e, 0x90), y += at<float>(e, 0x94);
    }
    out[0] = x, out[1] = y, out[2] = x + w, out[3] = y + h;
    return true;
}

// Many menus are drawn tilted (the "screen tilt": a Grid with ZPanel.Transform, rotated 9 degrees
// back about its own bottom edge), seen in perspective from the middle of the screen. The layout
// doesn't know; this maps a laid-out point to where it's drawn. kTiltDistance was measured on
// the pause menu and the tuning page (in 1080p UI pixels).
constexpr uintptr_t RVA_ZPANEL_TRANSFORM_DP = 0x36A5460;   // ZPanel::TransformProperty (attached)
constexpr float kTiltDistance = 2184.f;
constexpr float kTiltSin = 0.15643447f, kTiltCos = 0.98768834f;   // 9 degrees

float ui_height();
float ui_width();

// The bottom edge of the nearest tilted ancestor of `v`, in root pixels; false if none.
bool tilt_pivot(uint8_t* v, uint8_t* root, float* bottom) {
    void* dp = at<void*>(g_base, RVA_ZPANEL_TRANSFORM_DP);
    if (!dp) return false;
    uint8_t* unset = game<UnsetFn>(RVA_UNSET_VALUE)();
    for (uint8_t* e = v; e && e != root; e = at<uint8_t*>(e, VISUAL_PARENT)) {
        if (!is(type_of(e), TYPE_UIELEMENT)) break;
        uint8_t* boxed = nullptr;
        game<DpGetValueFn>(RVA_DP_GET_VALUE)(e, &boxed, dp);
        const bool tilted = boxed && boxed != unset;
        release(boxed);
        if (!tilted) continue;
        float r[4];
        if (!layout_bounds(e, root, r)) return false;
        *bottom = r[3];
        return true;
    }
    return false;
}

void tilt_point(float bottom, float p[2]) {
    const float scale = ui_height() / 1080.f;
    const float h = bottom - p[1];
    const float s = kTiltDistance * scale / (kTiltDistance * scale + h * kTiltSin);
    const float y = bottom - h * kTiltCos;
    p[0] = ui_width() / 2 + (p[0] - ui_width() / 2) * s;
    p[1] = ui_height() / 2 + (y - ui_height() / 2) * s;
}

// Where `v` is drawn, in `root`'s pixels.
bool bounds(uint8_t* v, uint8_t* root, float out[4]) {
    if (!layout_bounds(v, root, out)) return false;
    float bottom;
    if (!tilt_pivot(v, root, &bottom)) return true;
    float a[2] = {out[0], out[1]}, b[2] = {out[2], out[3]}, c[2] = {out[0], out[3]}, d[2] = {out[2], out[1]};
    for (float* q : {a, b, c, d}) tilt_point(bottom, q);
    out[0] = std::min(a[0], c[0]), out[2] = std::max(b[0], d[0]);
    out[1] = std::min(a[1], d[1]), out[3] = std::max(b[1], c[1]);
    return true;
}

constexpr uintptr_t kItemTypes[] = {TYPE_LIST_BOX_ITEM, TYPE_COMBO_ITEM, TYPE_TAB_ITEM, TYPE_TREE_ITEM,
                                    TYPE_MENU_ITEM, TYPE_SLIDER, TYPE_SPINNER};

// 0: not a control the pad selects; 1: a RepeatButton (a slider's or spinner's arrow); 2: one.
int control_kind(void* t) {
    if (is(t, TYPE_HELP_BUTTON)) return 2;
    for (uintptr_t k : kItemTypes)
        if (is(t, k)) return 2;
    if (is(t, TYPE_BUTTON_BASE)) return is(t, TYPE_REPEAT_BUTTON) ? 1 : 2;
    return 0;
}

// The control the pad would select, at or above `el` in the visual tree.
uint8_t* control_at(uint8_t* el) {
    uint8_t* repeat = nullptr;
    for (int depth = 0; el && depth < 64; ++depth) {
        void* t = type_of(el);
        if (!is(t, TYPE_VISUAL)) break;
        const int kind = control_kind(t);
        if (kind == 2) return el;
        if (kind == 1 && !repeat) repeat = el;
        el = at<uint8_t*>(el, VISUAL_PARENT);
    }
    return repeat;
}

// A menu is up when the focus is on something the pad selects.
bool menu_active() {
    uint8_t* f = focused_object();
    return f && control_at(f) != nullptr;
}

// The topmost control under the cursor: the visual tree in drawing order, so later wins.
// Inside a control that's hit, a control in it wins too, except its RepeatButtons.
// Failing that, the nearest within kNear pixels: the menus are drawn slightly tilted, so what's
// on screen can stand a few pixels off the layout.
constexpr float kNear = 16.f;
bool bool_property(uint8_t* el, uintptr_t dp_rva);
bool is_adjustable(uint8_t* el);
struct Search {
    uint8_t* root;
    float pt[2];
    uint8_t* best;
    int visited;
    uint8_t* near_el = nullptr;
    float near_dist = kNear;
    uint8_t* focusable = nullptr;   // the topmost hit that can take the focus (under a tuning row's
                                    // slider lies one that can't: its history)
};

void search(Search& s, uint8_t* v, int depth, bool inside_control) {
    if (!v || depth > 80 || ++s.visited > 20000) return;
    void* t = type_of(v);
    if (!is(t, TYPE_VISUAL) || opacity(v) <= 0.01f) return;
    if (is(t, TYPE_UIELEMENT)) {
        const int kind = control_kind(t);
        if (kind && !(kind == 1 && inside_control)) {
            float r[4];
            if (bounds(v, s.root, r)) {
                if (s.pt[0] >= r[0] && s.pt[0] < r[2] && s.pt[1] >= r[1] && s.pt[1] < r[3]) {
                    s.best         = v;
                    inside_control = true;
                    if (bool_property(v, RVA_FOCUSABLE_DP)) s.focusable = v;
                } else {
                    const float dx = std::max({r[0] - s.pt[0], 0.f, s.pt[0] - r[2]});
                    const float dy = std::max({r[1] - s.pt[1], 0.f, s.pt[1] - r[3]});
                    if (std::max(dx, dy) < s.near_dist) s.near_el = v, s.near_dist = std::max(dx, dy);
                }
            }
        }
    }
    const int n = vfn<CountFn>(v, VT_CHILD_COUNT)(v);
    for (int i = 0; i < n && i < 4096; ++i) search(s, vfn<ChildFn>(v, VT_CHILD)(v, i), depth + 1, inside_control);
}

uint8_t* active_window() {
    uint8_t* wm = window_manager();
    return wm ? at<uint8_t*>(wm, WM_ACTIVE_WINDOW) : nullptr;
}

uint8_t* hovered_control() {
    Search s{active_window(), {}, nullptr, 0, nullptr, kNear};
    if (!s.root || !cursor_ui(s.pt)) return nullptr;
    search(s, s.root, 0, false);
    if (s.focusable && s.best && !bool_property(s.best, RVA_FOCUSABLE_DP) && is_adjustable(s.best)) s.best = s.focusable;
    if (!s.best) s.best = s.near_el;
    if (g_cfg.debug) {
        static DWORD last = 0;
        if (GetTickCount() - last > 2000) {
            last = GetTickCount();
            logf("hit test at (%.0f,%.0f): %d visuals, found %s", s.pt[0], s.pt[1], s.visited, s.best ? rtti_name(s.best) : "none");
        }
    }
    return s.best;
}

// Debug: the topmost element of any kind under the cursor, and its ancestors.
void deepest(Search& s, uint8_t* v, int depth) {
    if (!v || depth > 80 || ++s.visited > 20000) return;
    void* t = type_of(v);
    if (!is(t, TYPE_VISUAL) || opacity(v) <= 0.01f) return;
    float r[4];
    if (is(t, TYPE_UIELEMENT) && bounds(v, s.root, r) && s.pt[0] >= r[0] && s.pt[0] < r[2] && s.pt[1] >= r[1] &&
        s.pt[1] < r[3] && std::strcmp(rtti_name(v), ".?AVWindowFrame@AVUI@@"))
        s.best = v;
    const int n = vfn<CountFn>(v, VT_CHILD_COUNT)(v);
    for (int i = 0; i < n && i < 4096; ++i) deepest(s, vfn<ChildFn>(v, VT_CHILD)(v, i), depth + 1);
}

std::string chain_of(uint8_t* el) {
    std::string chain;
    for (int d = 0; el && d < 40; ++d, el = at<uint8_t*>(el, VISUAL_PARENT)) {
        if (!is(type_of(el), TYPE_VISUAL)) break;
        const char* n = rtti_name(el);
        chain += (chain.empty() ? "" : " < ") + std::string(n + (std::strncmp(n, ".?AV", 4) ? 0 : 4));
    }
    return chain;
}

void dump_under_cursor() {
    Search s{active_window(), {}, nullptr, 0, nullptr, kNear};
    if (!s.root || !cursor_ui(s.pt)) return;
    deepest(s, s.root, 0);
    logf("under the cursor (%.0f,%.0f): %s", s.pt[0], s.pt[1], s.best ? chain_of(s.best).c_str() : "nothing");
    if (uint8_t* f = focused_object()) logf("focus: %s", chain_of(f).c_str());
}

// ---- the help bar ---------------------------------------------------------------------------------
// "(A) Select (B) Back ...": ControllerButtonControls, each naming a pad button. A click on one is
// that button's key, as the pad would send it.

bool is_help_button(uint8_t* el) { return el && is(type_of(el), TYPE_HELP_BUTTON); }

// The key for a ControllerButtonType (in the order the help bar template lists them), or 0.
// Help bar clicks press the pad button itself, as far as the menus can tell: the game's "was
// this pressed" check (WasPressed(input, context, control), context 7 being the menus) says yes
// to the button's control for one UI frame, then to its release for the next. The pad-to-key
// pump turns that into the key as for a real press, and pages that ask about the button
// directly (the pause menu's Y, Change time) see it too. The controls come from the pump's table.
constexpr uintptr_t RVA_WAS_PRESSED = 0x1072510;
constexpr uintptr_t RVA_PAD_KEYS    = 0x2254390;   // 24 x {pressed control, released control, key}
using WasPressedFn = bool (*)(void* input, int context, int control, uint8_t a);
WasPressedFn o_was_pressed = nullptr;

std::atomic<int> g_press_ctl{-1}, g_release_ctl{-1}, g_press_phase{0};   // 1: pressed, 2: released
std::atomic<DWORD> g_press_until{0};   // when the phase ends (0 while held: the middle button)
std::atomic<bool> g_press_held{false};
std::atomic<bool> g_press_told{false}, g_release_told{false};

bool was_pressed(void* input, int context, int control, uint8_t a) {
    if (o_was_pressed(input, context, control, a)) {
        if (g_cfg.debug) {
            static int last_ctx = -1, last_ctl = -1;
            static DWORD last_t = 0;
            if (context != last_ctx || control != last_ctl || GetTickCount() - last_t > 500)
                logf("pad pressed: context %d control %d (a %d), asked from game+0x%llX", context, control, a,
                     static_cast<unsigned long long>(static_cast<uint8_t*>(__builtin_return_address(0)) - g_base));
            last_ctx = context, last_ctl = control, last_t = GetTickCount();
        }
        return true;
    }
    if (context != 7) return false;
    // Like a real press: "pressed" to the first asker once, "released" once (later).
    const int phase = g_press_phase.load();
    bool r = false;
    if (phase == 1 && control == g_press_ctl.load()) r = !g_press_told.exchange(true);
    else if (phase == 2 && control == g_release_ctl.load()) r = !g_release_told.exchange(true);
    if (phase && g_cfg.debug && (control == g_press_ctl.load() || control == g_release_ctl.load()))
        logf("fake press: control %d asked from game+0x%llX in phase %d: %d", control,
             static_cast<unsigned long long>(static_cast<uint8_t*>(__builtin_return_address(0)) - g_base), phase, r);
    return r;
}

// Once a frame: the press is down as long as it's held (at least 80 ms), then released for 40 ms,
// in real time: some of what asks runs outside the UI's update.
void press_frame() {
    const int phase = g_press_phase.load();
    if (!phase || g_press_held.load() || GetTickCount() < g_press_until.load()) return;
    if (phase == 1) {
        g_press_phase = 2;
        g_press_until = GetTickCount() + 40;
    } else {
        g_press_phase = 0;
    }
}

// Presses the pad button that sends `key` to the menus; false if there's none (or no hook).
bool press_pad(int key, bool hold = false) {
    if (!o_was_pressed || g_press_phase.load()) return false;
    for (int i = 0; i < 24; ++i) {
        const uint8_t* e = g_base + RVA_PAD_KEYS + 12 * i;
        if (at<int>(e, 8) != key) continue;
        g_press_ctl     = at<int>(e, 0);
        g_release_ctl   = at<int>(e, 4);
        g_press_held    = hold;
        g_press_told    = false;
        g_release_told  = false;
        g_press_until   = GetTickCount() + 80;
        g_press_phase   = 1;
        if (g_cfg.debug) logf("pad press: control %d / %d (key %d)", at<int>(e, 0), at<int>(e, 4), key);
        return true;
    }
    return false;
}

int help_button_key(uint8_t* el) {
    void* dp = at<void*>(g_base, RVA_HELP_BUTTON_DP);
    if (!dp) return 0;
    uint8_t* boxed = nullptr;
    game<DpGetValueFn>(RVA_DP_GET_VALUE)(el, &boxed, dp);
    const int type = boxed && boxed != game<UnsetFn>(RVA_UNSET_VALUE)() ? at<int>(boxed, 0x28) : -1;
    release(boxed);
    // ControllerButtonType, in the help bar template's order, to the key the pump sends for it.
    // Start/View/triggers/sticks: Pause, Escape, the shifts and the controls, by the table's order.
    static const int keys[] = {
        KEY_A, KEY_B, KEY_X, KEY_Y,   // A B X Y
        7, 13,                        // Start (Menu), Back (View)
        KEY_LB, KEY_RB,               // bumpers
        116, 117,                     // triggers
        0, 0,                         // d-pad left/right, up/down (both ways: nothing to press)
        KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
        118, 0, 119, 119,             // left stick, left stick left/right, right stick, R3
    };
    const int key = type >= 0 && type < static_cast<int>(sizeof keys / sizeof keys[0]) ? keys[type] : 0;
    if (g_cfg.debug) logf("help bar button %d: key %d", type, key);
    return key;
}

bool help_click(uint8_t* ctl) {
    if (!is_help_button(ctl)) return false;
    if (const int key = help_button_key(ctl)) {
        if (!press_pad(key)) {
            send_key(key, false);
            send_key(key, true);
        }
    }
    return true;
}

// ---- tab strips ----------------------------------------------------------------------------------
// The pause menu's tabs are Buttons that can't take the focus, in a row with the LB and RB
// pictures at its ends; the current tab is the one marked IsFocused. A click on one is turned
// into the LB or RB presses that get there.

bool laid_out(uint8_t* el) {
    const uint32_t flags = at<uint32_t>(el, VISUAL_FLAGS);
    return (((flags >> 5) & 1) || ((flags >> 6) & 1)) && at<float>(el, RENDER_SIZE) > 0;
}

bool bool_property(uint8_t* el, uintptr_t dp_rva) {
    void* dp = at<void*>(g_base, dp_rva);
    if (!dp) return false;
    uint8_t* boxed = nullptr;
    game<DpGetValueFn>(RVA_DP_GET_VALUE)(el, &boxed, dp);
    const bool v = boxed && boxed != game<UnsetFn>(RVA_UNSET_VALUE)() && at<uint8_t>(boxed, 0x28);
    release(boxed);
    return v;
}

bool is_focused(uint8_t* el) { return bool_property(el, RVA_IS_FOCUSED_DP); }

// LB/RB skip disabled tabs (TileState Disabled), which the tab template shows at half opacity.
bool tab_disabled(uint8_t* tab) {
    if (vfn<CountFn>(tab, VT_CHILD_COUNT)(tab) < 1) return false;
    uint8_t* look = vfn<ChildFn>(tab, VT_CHILD)(tab, 0);
    return look && opacity(look) < 0.75f;
}

// A button in a tab row: a row of buttons, some of which (the tabs) can't take the focus. The
// LB/RB pictures at its ends can, but focusing them takes the current tab from the row.
bool in_tab_row(uint8_t* ctl) {
    if (!is(type_of(ctl), TYPE_BUTTON_BASE)) return false;
    uint8_t* row = at<uint8_t*>(ctl, VISUAL_PARENT);
    if (!row) return false;
    int buttons = 0, tabs = 0;
    const int n = vfn<CountFn>(row, VT_CHILD_COUNT)(row);
    for (int i = 0; i < n && i < 256; ++i) {
        uint8_t* c = vfn<ChildFn>(row, VT_CHILD)(row, i);
        if (!c || !is(type_of(c), TYPE_BUTTON_BASE) || !laid_out(c)) continue;
        ++buttons;
        if (!bool_property(c, RVA_FOCUSABLE_DP)) ++tabs;
    }
    return buttons >= 3 && tabs > 0;
}

bool tab_click(uint8_t* ctl) {
    uint8_t* row = at<uint8_t*>(ctl, VISUAL_PARENT);
    if (!row) return false;
    std::vector<uint8_t*> buttons;
    const int n = vfn<CountFn>(row, VT_CHILD_COUNT)(row);
    for (int i = 0; i < n && i < 256; ++i) {
        uint8_t* c = vfn<ChildFn>(row, VT_CHILD)(row, i);
        if (c && is(type_of(c), TYPE_BUTTON_BASE) && laid_out(c)) buttons.push_back(c);
    }
    if (buttons.size() < 3) return false;
    int key = 0, steps = 0;
    if (ctl == buttons.front()) {
        key = KEY_LB, steps = 1;
    } else if (ctl == buttons.back()) {
        key = KEY_RB, steps = 1;
    } else {
        int cur = -1;
        for (size_t i = 1; i + 1 < buttons.size(); ++i)
            if (is_focused(buttons[i])) cur = static_cast<int>(i);
        if (cur < 0) {
            if (g_cfg.debug) logf("tab click: no current tab (%zu buttons)", buttons.size());
            return false;
        }
        // Count only the tabs LB/RB stop at: those in the current tab's state.
        if (tab_disabled(ctl)) return true;   // a tab LB/RB can't reach: nothing to do
        int pos = 0, target = 0, here = 0;
        for (size_t i = 1; i + 1 < buttons.size(); ++i) {
            if (tab_disabled(buttons[i])) continue;
            if (static_cast<int>(i) == cur) here = pos;
            if (buttons[i] == ctl) target = pos;
            ++pos;
        }
        key   = target > here ? KEY_RB : KEY_LB;
        steps = std::abs(target - here);
    }
    if (g_cfg.debug) logf("tab click: %d x %s", steps, key == KEY_RB ? "RB" : "LB");
    for (int i = 0; i < steps; ++i) {
        send_key(key, false, i ? 10 : 0);   // a moment for each tab to come in
        send_key(key, true);
    }
    return true;
}

// ---- the map ------------------------------------------------------------------------------------
// The map has a cursor of its own, which the stick moves over the map and the camera follows
// once it strays too far from the middle. Here the map cursor sits under the mouse, a drag moves
// the camera (and the cursor with it), a click is A on the spot under the cursor (select an
// icon, set a waypoint), and the wheel zooms. While the mouse is in use the camera's leash is
// let out, so pointing near an edge doesn't scroll the map.

uint8_t* map_controller() {
    uint8_t* f = focused_object();
    if (!f || at<uint8_t*>(f, 0) != g_base + VT_MAP_SCENE) return nullptr;
    uint8_t* c = at<uint8_t*>(f, MAP_CONTROLLER);
    return c && at<uint8_t*>(c, 0) == g_base + VT_MAP_CONTROLLER ? c : nullptr;
}

float ui_height() {
    uint8_t* app = at<uint8_t*>(g_base, RVA_APPLICATION);
    const float h = app ? at<float>(app, 0x54) : 0.f;
    return h > 0 ? h : 1080.f;
}

float ui_width() {
    uint8_t* app = at<uint8_t*>(g_base, RVA_APPLICATION);
    const float w = app ? at<float>(app, 0x50) : 0.f;
    return w > 0 ? w : 1920.f;
}

float& f32(uint8_t* c, uintptr_t off) { return *reinterpret_cast<float*>(c + off); }

// World units per UI pixel: the map shows `zoom` from the middle to the top edge.
float map_scale(uint8_t* c) { return 2.f * f32(c, MAP_ZOOM_SHOWN) / ui_height(); }

uint8_t* g_leash_map = nullptr;   // the controller whose leash is let out (compared only)
float g_leash_saved  = 0.f;

void map_leash(uint8_t* c, bool out) {
    if (out && g_leash_map != c) {
        g_leash_map   = c;
        g_leash_saved = f32(c, MAP_LEASH);
        f32(c, MAP_LEASH) = 1000.f;
        if (g_cfg.debug) logf("map: leash %.3f let out", g_leash_saved);
    } else if (!out && g_leash_map == c && c) {
        f32(c, MAP_LEASH) = g_leash_saved;
        g_leash_map       = nullptr;
    }
}

// The map cursor to the spot under the mouse.
void map_point(uint8_t* c) {
    float pt[2];
    if (!cursor_ui(pt)) return;
    map_leash(c, true);
    const float k  = map_scale(c);
    f32(c, MAP_POS_X) = f32(c, MAP_CAM_X) + (pt[0] - ui_width() / 2) * k;
    f32(c, MAP_POS_Z) = f32(c, MAP_CAM_Z) - (pt[1] - ui_height() / 2) * k;
}

// Moves what's on the map by (dx, dy) UI pixels: the camera, where it's going and where it is.
void map_drag(uint8_t* c, float dx, float dy) {
    const float k = map_scale(c);
    for (uintptr_t off : {MAP_TARGET_X, MAP_CAM_X}) f32(c, off) -= dx * k;
    for (uintptr_t off : {MAP_TARGET_Z, MAP_CAM_Z}) f32(c, off) += dy * k;
}

bool g_map_drag = false, g_map_dragged = false;
float g_map_last[2] = {}, g_map_start[2] = {};

bool map_press() {
    uint8_t* c = map_controller();
    if (!c || !cursor_ui(g_map_start)) return false;
    g_map_drag    = true;
    g_map_dragged = false;
    g_map_last[0] = g_map_start[0], g_map_last[1] = g_map_start[1];
    return true;
}

bool on_move_map() {
    uint8_t* c = map_controller();
    if (!c) {
        g_map_drag = false;
        return false;
    }
    float pt[2];
    if (g_map_drag && cursor_ui(pt)) {
        if (std::abs(pt[0] - g_map_start[0]) + std::abs(pt[1] - g_map_start[1]) > 8) g_map_dragged = true;
        if (g_map_dragged) map_drag(c, pt[0] - g_map_last[0], pt[1] - g_map_last[1]);
        g_map_last[0] = pt[0], g_map_last[1] = pt[1];
    }
    map_point(c);
    return true;
}

bool map_release() {
    if (!g_map_drag) return false;
    g_map_drag = false;
    uint8_t* c = map_controller();
    if (g_cfg.debug && c)
        logf("map %s: cursor %.1f,%.1f target %.1f,%.1f cam %.1f,%.1f angle %.3f aspect %.3f zoom %.1f cam90 %.1f,%.1f,%.1f b8 %.3f",
             g_map_dragged ? "drag" : "click", f32(c, MAP_POS_X), f32(c, MAP_POS_Z), f32(c, MAP_TARGET_X),
             f32(c, MAP_TARGET_Z), f32(c, MAP_CAM_X), f32(c, MAP_CAM_Z), f32(c, 0xC8), f32(c, 0xBC),
             f32(c, MAP_ZOOM_SHOWN), f32(c, 0x90), f32(c, 0x94), f32(c, 0x98), f32(c, 0xB8));
    if (!c || g_map_dragged) return true;
    map_point(c);
    if (g_cfg.debug) logf("map click at (%.0f,%.0f), zoom %.1f (%.2f)", g_map_start[0], g_map_start[1], f32(c, MAP_ZOOM_SHOWN), f32(c, MAP_ZOOM));
    send_key(KEY_A, false, 1);
    send_key(KEY_A, true);
    return true;
}

bool map_wheel(int delta) {
    uint8_t* c = map_controller();
    if (!c) return false;
    const float lo = std::min(f32(c, MAP_ZOOM_MIN), f32(c, MAP_ZOOM_MAX));
    const float hi = std::max(f32(c, MAP_ZOOM_MIN), f32(c, MAP_ZOOM_MAX));
    f32(c, MAP_ZOOM) = std::max(lo, std::min(hi, f32(c, MAP_ZOOM) - 0.1f * delta / WHEEL_DELTA));
    if (g_cfg.debug) logf("map zoom %.2f [%.2f, %.2f]", f32(c, MAP_ZOOM), lo, hi);
    return true;
}

// Each frame: the leash back in once the mouse rests (the cursor has hidden) or the map is gone.
void map_frame() {
    if (!g_leash_map) return;
    uint8_t* c = map_controller();
    if (c != g_leash_map) {
        g_leash_map = nullptr;   // that map is closed; its controller may be gone
        return;
    }
    if (!cursor_wanted()) map_leash(c, false);
}

// ---- rows, sliders and selectors ----------------------------------------------------------------
// Lists keep the focus on a control inside each row (a Slider, a SpinnerControl, a Button), or
// on the list itself with a selection that the pad moves (the car carousels, the Stats and
// Tuning tabs and lists). A click focuses the row's control, or walks the selection to the
// item clicked with the d-pad (or LB/RB, where that's what moves it), as the pad would.

uint8_t* dp_object(uint8_t* el, uintptr_t dp_rva) {
    void* dp = at<void*>(g_base, dp_rva);
    if (!dp || !el) return nullptr;
    uint8_t* boxed = nullptr;
    game<DpGetValueFn>(RVA_DP_GET_VALUE)(el, &boxed, dp);
    uint8_t* v = boxed == game<UnsetFn>(RVA_UNSET_VALUE)() ? nullptr : boxed;
    release(boxed);   // identity only: whoever holds the value keeps it alive
    return v;
}

bool is_adjustable(uint8_t* el) {
    void* t = type_of(el);
    return is(t, TYPE_SLIDER) || is(t, TYPE_SPINNER);
}

// The control to focus for `ctl`: itself when it can take the focus, else the first control
// inside it that can (a settings row's slider or spinner).
uint8_t* focus_target_in(uint8_t* v, int depth) {
    if (!v || depth > 24) return nullptr;
    void* t = type_of(v);
    if (!is(t, TYPE_VISUAL)) return nullptr;
    if (is(t, TYPE_UIELEMENT) && laid_out(v) && control_kind(t) == 2 && !is(t, TYPE_LIST_BOX_ITEM) &&
        bool_property(v, RVA_FOCUSABLE_DP))
        return v;
    const int n = vfn<CountFn>(v, VT_CHILD_COUNT)(v);
    for (int i = 0; i < n && i < 256; ++i)
        if (uint8_t* f = focus_target_in(vfn<ChildFn>(v, VT_CHILD)(v, i), depth + 1)) return f;
    return nullptr;
}

// The game's ItemsSelector `el` is in, if any: its items (tabs, carousel cars) are selected, not
// focused; their buttons can take the focus, but A on them does the page's thing, not theirs.
uint8_t* items_selector_of(uint8_t* el) {
    for (int d = 0; el && d < 40; ++d, el = at<uint8_t*>(el, VISUAL_PARENT)) {
        if (!is(type_of(el), TYPE_VISUAL)) break;
        if (!std::strcmp(rtti_name(el), ".?AVItemsSelector@@")) return el;
    }
    return nullptr;
}

uint8_t* focus_target(uint8_t* ctl) {
    if (!ctl || is_help_button(ctl) || in_tab_row(ctl) || items_selector_of(ctl)) return nullptr;
    if (bool_property(ctl, RVA_FOCUSABLE_DP)) return ctl;
    if (uint8_t* f = focus_target_in(ctl, 0)) return f;
    // Part of something that can't take the focus (a balance slider's visible halves): the
    // row's control (there, an invisible slider holding the value).
    for (uint8_t* e = at<uint8_t*>(ctl, VISUAL_PARENT); e; e = at<uint8_t*>(e, VISUAL_PARENT)) {
        void* t = type_of(e);
        if (!is(t, TYPE_VISUAL) || is(t, TYPE_SELECTOR)) break;
        if (is(t, TYPE_LIST_BOX_ITEM)) return focus_target_in(e, 0);
    }
    return nullptr;
}

// -1 / +1: the click is on the left / right part of a slider or spinner (where its arrows are).
// The RepeatButtons (arrows) laid out inside `v`.
void arrows_in(uint8_t* v, std::vector<uint8_t*>& out, int depth) {
    if (!v || depth > 24 || out.size() > 8) return;
    void* t = type_of(v);
    if (!is(t, TYPE_VISUAL)) return;
    if (is(t, TYPE_REPEAT_BUTTON) && laid_out(v)) out.push_back(v);
    const int n = vfn<CountFn>(v, VT_CHILD_COUNT)(v);
    for (int i = 0; i < n && i < 256; ++i) arrows_in(vfn<ChildFn>(v, VT_CHILD)(v, i), out, depth + 1);
}

int adjust_side(uint8_t* control, const float pt[2]) {
    uint8_t* root = active_window();
    float r[4];
    if (!bounds(control, root, r)) return 0;
    const float w = r[2] - r[0];
    if (is(type_of(control), TYPE_SLIDER)) {
        // A slider: its track's "decrease" RepeatButton runs from the track's start to the
        // value. Left of the value is down, right of it up; the label before the track, nothing.
        std::vector<uint8_t*> parts;
        arrows_in(control, parts, 0);
        for (uint8_t* part : parts) {
            float d[4];
            if (!bounds(part, root, d)) continue;
            if (pt[0] < d[0] - 24.f) return 0;
            return pt[0] < d[2] ? -1 : 1;
        }
        // At its minimum the decrease part has no width: the right half of the row is "up".
        return pt[0] > r[0] + w * 0.45f ? 1 : 0;
    }
    // A spinner: label, then "<  value  >" in the right half; the value's middle is about three
    // quarters of the way across.
    const float at = (pt[0] - r[0]) / w;
    if (at < 0.45f) return 0;
    return at < 0.75f ? -1 : 1;
}

// Holding the mouse on a slider's side repeats, like holding the d-pad.
struct Adjust {
    int key = 0;
    DWORD next = 0;
} g_adjust;

// -- selectors --

struct SelHit {
    uint8_t* root;
    float pt[2];
    uint8_t* sel  = nullptr;   // the selector
    uint8_t* item = nullptr;   // the item under the cursor (its DataContext)
    int visited   = 0;
};

void selector_hit_in(SelHit& h, uint8_t* v, uint8_t* sel, int depth) {
    if (!v || depth > 80 || ++h.visited > 20000) return;
    void* t = type_of(v);
    if (!is(t, TYPE_VISUAL) || opacity(v) <= 0.01f) return;
    if (is(t, TYPE_SELECTOR)) sel = v;
    if (sel && v != sel && is(t, TYPE_UIELEMENT)) {
        float r[4];
        if (bounds(v, h.root, r) && h.pt[0] >= r[0] && h.pt[0] < r[2] && h.pt[1] >= r[1] && h.pt[1] < r[3]) {
            uint8_t* item = dp_object(v, RVA_DATA_CONTEXT_DP);
            if (item && item != dp_object(sel, RVA_DATA_CONTEXT_DP)) h.sel = sel, h.item = item;
        }
    }
    const int n = vfn<CountFn>(v, VT_CHILD_COUNT)(v);
    for (int i = 0; i < n && i < 4096; ++i) selector_hit_in(h, vfn<ChildFn>(v, VT_CHILD)(v, i), sel, depth + 1);
}

bool selector_hit(const float pt[2], uint8_t** sel, uint8_t** item) {
    SelHit h{active_window(), {pt[0], pt[1]}};
    if (!h.root) return false;
    selector_hit_in(h, h.root, nullptr, 0);
    *sel = h.sel, *item = h.item;
    return h.sel != nullptr;
}

// Where the item's container is: the first element under `sel` whose DataContext is `item`.
bool item_center_in(uint8_t* v, uint8_t* item, uint8_t* root, float c[2], int depth) {
    if (!v || depth > 40) return false;
    void* t = type_of(v);
    if (!is(t, TYPE_VISUAL)) return false;
    if (is(t, TYPE_UIELEMENT) && dp_object(v, RVA_DATA_CONTEXT_DP) == item) {
        float r[4];
        if (!bounds(v, root, r)) return false;
        c[0] = (r[0] + r[2]) / 2, c[1] = (r[1] + r[3]) / 2;
        return true;
    }
    const int n = vfn<CountFn>(v, VT_CHILD_COUNT)(v);
    for (int i = 0; i < n && i < 4096; ++i)
        if (item_center_in(vfn<ChildFn>(v, VT_CHILD)(v, i), item, root, c, depth + 1)) return true;
    return false;
}

void tap(int key, int wait = 0) {
    send_key(key, false, wait);
    send_key(key, true);
}

struct Walk {
    bool active = false;
    uint8_t* sel = nullptr;
    uint8_t* item = nullptr;
    float pt[2] = {};
    int steps = 0, stalls = 0, wait = 0;
    uint8_t* last = nullptr;
    bool bumpers = false;   // LB/RB instead of left/right
    int dx = 0, dy = 0;     // the last direction
} g_walk;

void walk_stop(const char* why) {
    if (g_cfg.debug) logf("selection walk: %s after %d steps", why, g_walk.steps);
    g_walk.active = false;
}

void walk_step() {
    if (!g_walk.active || !g_keys.empty()) return;
    if (g_walk.wait > 0) {
        --g_walk.wait;
        return;
    }
    // Still there? The same selector under the same spot (what's in it may have scrolled).
    uint8_t *sel = nullptr, *item = nullptr;
    if (!selector_hit(g_walk.pt, &sel, &item) || sel != g_walk.sel) return walk_stop("gone");
    uint8_t* cur = dp_object(sel, RVA_SELECTED_ITEM_DP);
    if (cur == g_walk.item) return walk_stop("there");
    if (g_walk.steps && cur == g_walk.last) {
        if (++g_walk.stalls >= 3) return walk_stop("stuck");
        if (g_walk.dx && items_selector_of(g_walk.sel) == g_walk.sel)
            g_walk.bumpers = !g_walk.bumpers;   // that way didn't move it: the other
    } else {
        g_walk.stalls = 0;
    }
    float t[2], c[2];
    uint8_t* root = active_window();
    // Sideways only in the game's ItemsSelectors (tabs, carousels): in a list, left/right would
    // change the focused row's slider or option.
    const bool sideways_ok = items_selector_of(sel) == sel;
    if (cur && item_center_in(sel, g_walk.item, root, t, 0) && item_center_in(sel, cur, root, c, 0)) {
        const float dx = t[0] - c[0], dy = t[1] - c[1];
        if (sideways_ok && std::abs(dx) >= std::abs(dy)) g_walk.dx = dx > 0 ? 1 : -1, g_walk.dy = 0;
        else if (std::abs(dy) > 4.f) g_walk.dx = 0, g_walk.dy = dy > 0 ? 1 : -1;
        else return walk_stop("no way there");
    } else if (!g_walk.dx && !g_walk.dy) {
        return walk_stop("lost");
    }
    int key;
    if (g_walk.dx) key = g_walk.bumpers ? (g_walk.dx > 0 ? KEY_RB : KEY_LB) : (g_walk.dx > 0 ? KEY_RIGHT : KEY_LEFT);
    else key = g_walk.dy > 0 ? KEY_DOWN : KEY_UP;
    tap(key);
    g_walk.last = cur;
    g_walk.wait = 8;
    if (++g_walk.steps > 40) walk_stop("too far");
}

void walk_start(uint8_t* sel, uint8_t* item, const float pt[2]) {
    g_walk        = Walk{};
    g_walk.active = true;
    // A selector without the focus (a tab row over a list) moves with LB/RB: the d-pad goes to
    // whatever has the focus (a tuning slider would change).
    g_walk.bumpers = focused_object() != sel && items_selector_of(sel) == sel;
    g_walk.sel = sel, g_walk.item = item;
    g_walk.pt[0] = pt[0], g_walk.pt[1] = pt[1];
    walk_step();
}

// -- what counts as a menu --

const char* focus_name() {
    uint8_t* f = focused_object();
    return f ? rtti_name(f) : "";
}

// A menu page has the focus (not the driving HUD's window).
bool in_menu() {
    const char* n = focus_name();
    return *n && std::strcmp(n, ".?AVUIWindow@@") != 0;
}

// Screens with nothing to point at, where any click is A: the title screen, videos, the wheelspin.
bool click_anywhere_screen() {
    const char* n = focus_name();
    return !std::strcmp(n, ".?AVSplash@@") || !std::strcmp(n, ".?AVVideoPlayer@@") || !std::strcmp(n, ".?AVTombola@@");
}

// ---- photo mode ---------------------------------------------------------------------------------
// The photo camera (CCamPhotoMode) reads the sticks into fields of its own each frame. After it
// does, the mouse and keyboard are added: dragging with the left button looks around (the right
// stick), W A S D move (the left stick), Q / E go down and up, the wheel zooms.

constexpr uintptr_t RVA_PHOTO_INPUT_SLOT = 0x2C6C5B0;   // CCamPhotoMode vtable slot 7: read the input
constexpr uintptr_t RVA_PHOTO_INPUT      = 0x103F660;
constexpr uintptr_t PHOTO_MOVE_X  = 0x1F0;   // the camera's stick values, -1..1: forward/back
constexpr uintptr_t PHOTO_MOVE_Y  = 0x1F4;   // right/left
constexpr uintptr_t PHOTO_RISE    = 0x1F8;   // up/down
constexpr uintptr_t PHOTO_LOOK_X  = 0x1FC;   // turning (0x204/0x208 are zoom and roll)
constexpr uintptr_t PHOTO_LOOK_Y  = 0x200;
constexpr uintptr_t PHOTO_ZOOM    = 0x204;   // zoom (0x208 is roll)

using PhotoInputFn = void (*)(void* cam);
PhotoInputFn o_photo_input = nullptr;

// Mouse movement for the camera, in UI pixels, gathered on the UI thread and used up by the camera.
std::atomic<float> g_photo_look_x{0}, g_photo_look_y{0}, g_photo_move_x{0}, g_photo_move_y{0};
std::atomic<int> g_photo_zoom{0};   // camera frames left to zoom in (+) or out (-), from the wheel

float take(std::atomic<float>& a) { return a.exchange(0.f); }

void add(std::atomic<float>& a, float v) {
    float cur = a.load();
    while (!a.compare_exchange_weak(cur, cur + v)) {
    }
}

std::atomic<bool> g_photo_keys{false};   // photo mode is up and the game window is in front

float key_axis(int minus, int plus) {
    return ((GetAsyncKeyState(plus) & 0x8000) ? 1.f : 0.f) - ((GetAsyncKeyState(minus) & 0x8000) ? 1.f : 0.f);
}

float stick(float pixels, float per_pixel) { return std::max(-1.f, std::min(1.f, pixels * per_pixel)); }

void photo_input(void* cam) {
    o_photo_input(cam);
    auto* c = static_cast<uint8_t*>(cam);
    const float lx = take(g_photo_look_x), ly = take(g_photo_look_y);
    const float mx = take(g_photo_move_x), my = take(g_photo_move_y);
    int z = g_photo_zoom.load();
    const float zoom = z > 0 ? 1.f : z < 0 ? -1.f : 0.f;
    if (z) g_photo_zoom.compare_exchange_strong(z, z > 0 ? z - 1 : z + 1);
    if (lx || ly) f32(c, PHOTO_LOOK_X) -= stick(lx, 0.05f), f32(c, PHOTO_LOOK_Y) -= stick(ly, 0.05f);
    if (mx || my) f32(c, PHOTO_MOVE_X) -= stick(mx, 0.05f), f32(c, PHOTO_MOVE_Y) -= stick(my, 0.05f);
    if (zoom) f32(c, PHOTO_ZOOM) += zoom;
    if (g_photo_keys.load()) {
        f32(c, PHOTO_MOVE_X) += key_axis('S', 'W');   // 0x1F0 is forward/back
        f32(c, PHOTO_MOVE_Y) += key_axis('A', 'D');   // 0x1F4 is right/left
        f32(c, PHOTO_RISE) += key_axis('Q', 'E');
    }
    if (g_cfg.debug) {
        static DWORD last = 0;
        bool any = false;
        for (uintptr_t o = 0x1F0; o <= 0x208; o += 4) any |= f32(c, o) != 0.f;
        if (any && GetTickCount() - last > 300) {
            last = GetTickCount();
            logf("photo camera input: 1f0 %.2f 1f4 %.2f 1f8 %.2f 1fc %.2f 200 %.2f 204 %.2f 208 %.2f (layout %d)",
                 f32(c, 0x1F0), f32(c, 0x1F4), f32(c, 0x1F8), f32(c, 0x1FC), f32(c, 0x200), f32(c, 0x204), f32(c, 0x208),
                 at<uint8_t>(c, 0x22D));
        }
    }
}

bool in_photo_mode() { return !std::strcmp(focus_name(), ".?AVPhotoModeScene@@"); }

// 0: no drag; 1: looking (left button); 2: moving (right button).
int g_photo_drag = 0;
bool g_photo_dragged = false;
float g_photo_last[2] = {}, g_photo_start[2] = {};

bool photo_press(int kind) {
    if (!in_photo_mode() || !cursor_ui(g_photo_start)) return false;
    g_photo_drag    = kind;
    g_photo_dragged = false;
    g_photo_last[0] = g_photo_start[0], g_photo_last[1] = g_photo_start[1];
    return true;
}

bool on_move_photo() {
    if (!g_photo_drag) return false;
    float pt[2];
    if (!in_photo_mode() || !cursor_ui(pt)) {
        g_photo_drag = 0;
        return false;
    }
    if (std::abs(pt[0] - g_photo_start[0]) + std::abs(pt[1] - g_photo_start[1]) > 6) g_photo_dragged = true;
    if (g_photo_dragged) {
        const float dx = pt[0] - g_photo_last[0], dy = pt[1] - g_photo_last[1];
        add(g_photo_look_x, dx), add(g_photo_look_y, dy);
    }
    g_photo_last[0] = pt[0], g_photo_last[1] = pt[1];
    return true;
}

// The button's release: true when it was a drag (and so nothing else should happen).
bool photo_release() {
    if (!g_photo_drag) return false;
    g_photo_drag = 0;
    return g_photo_dragged;
}

bool photo_wheel(int delta) {
    if (!in_photo_mode()) return false;
    g_photo_zoom += 10 * delta / WHEEL_DELTA;
    return true;
}

using GetAnalogFn = float (*)(void* input, int context, int control);
GetAnalogFn o_get_analog = nullptr;
std::atomic<float> g_cam_stick_x{0}, g_cam_stick_y{0};   // see get_analog
// The garage camera (0xAA02E0, Update(camera, input)): mode 0 orbits by the right stick, and
// +0xA8 is its distance (the game's presets: -2, -3.5, -5; further is more negative). The
// wheel zooms it: steps waiting for the camera's next update.
constexpr uintptr_t RVA_GARAGE_CAMERA = 0xAA02E0;
constexpr uintptr_t GARAGE_CAM_MODE = 0x15C, GARAGE_CAM_DISTANCE = 0xA8;
using GarageCameraFn = void (*)(void* cam, void* input);
GarageCameraFn o_garage_camera = nullptr;
std::atomic<int> g_garage_zoom{0};          // wheel notches, + in
std::atomic<DWORD> g_garage_seen{0};         // when the garage camera last updated

void garage_camera(void* cam, void* input) {
    g_garage_seen = GetTickCount();
    if (const int z = g_garage_zoom.exchange(0)) {
        auto* c = static_cast<uint8_t*>(cam);
        if (at<int>(c, GARAGE_CAM_MODE) == 0) {
            float& d = *reinterpret_cast<float*>(c + GARAGE_CAM_DISTANCE);
            d = std::max(-8.f, std::min(-1.5f, d + 0.3f * z));
            if (g_cfg.debug) logf("garage camera distance %.2f", d);
        }
    }
    o_garage_camera(cam, input);
}

bool garage_wheel(int delta) {
    if (!o_garage_camera || GetTickCount() - g_garage_seen.load() > 200) return false;
    g_garage_zoom += delta / WHEEL_DELTA;
    return true;
}

bool g_cam_drag = false;
float g_cam_last[2] = {}, g_cam_dx = 0, g_cam_dy = 0;   // UI thread

bool cam_press() {
    if (!o_get_analog || !in_menu() || !cursor_ui(g_cam_last)) return false;
    g_cam_drag = true;
    g_cam_dx = g_cam_dy = 0;
    return true;
}

bool on_move_cam() {
    if (!g_cam_drag) return false;
    float pt[2];
    if (!cursor_ui(pt)) return true;
    g_cam_dx += pt[0] - g_cam_last[0], g_cam_dy += pt[1] - g_cam_last[1];
    g_cam_last[0] = pt[0], g_cam_last[1] = pt[1];
    return true;
}

// Before the UI's update: this frame's drag as stick values ("grab and turn the car").
void cam_frame_begin() {
    g_cam_stick_x = g_cam_drag ? -g_cam_dx * 0.04f : 0.f;
    g_cam_stick_y = g_cam_drag ? g_cam_dy * 0.04f : 0.f;
    g_cam_dx = g_cam_dy = 0;
}

// Which way a list scrolls: its items (the topmost elements whose DataContext is an item, not
// the list's own) overflow it sideways (the car carousels, the manufacturer grid) or not.
void item_boxes(uint8_t* v, uint8_t* own, uint8_t* root, float box[4], int& n, int depth) {
    if (!v || depth > 40 || n > 400) return;
    void* t = type_of(v);
    if (!is(t, TYPE_VISUAL)) return;
    if (is(t, TYPE_UIELEMENT) && depth > 0) {
        uint8_t* dc = dp_object(v, RVA_DATA_CONTEXT_DP);
        if (dc && dc != own) {
            float r[4];
            if (laid_out(v) && layout_bounds(v, root, r)) {
                box[0] = std::min(box[0], r[0]), box[1] = std::min(box[1], r[1]);
                box[2] = std::max(box[2], r[2]), box[3] = std::max(box[3], r[3]);
                ++n;
            }
            return;
        }
    }
    const int c = vfn<CountFn>(v, VT_CHILD_COUNT)(v);
    for (int i = 0; i < c && i < 4096; ++i) item_boxes(vfn<ChildFn>(v, VT_CHILD)(v, i), own, root, box, n, depth + 1);
}

bool scrolls_sideways(uint8_t* el) {
    // The list: the focused element if it's one, else its nearest list ancestor.
    uint8_t* sel = el;
    while (sel && is(type_of(sel), TYPE_VISUAL) && !is(type_of(sel), TYPE_SELECTOR)) sel = at<uint8_t*>(sel, VISUAL_PARENT);
    if (!sel || !is(type_of(sel), TYPE_SELECTOR)) return false;
    uint8_t* root = active_window();
    float r[4], box[4] = {1e30f, 1e30f, -1e30f, -1e30f};
    int n = 0;
    if (!layout_bounds(sel, root, r)) return false;
    item_boxes(sel, dp_object(sel, RVA_DATA_CONTEXT_DP), root, box, n, 0);
    if (n < 2) return false;
    const float over_x = std::max(0.f, r[0] - box[0]) + std::max(0.f, box[2] - r[2]);
    const float over_y = std::max(0.f, r[1] - box[1]) + std::max(0.f, box[3] - r[3]);
    return over_x > 4.f && over_x > over_y;
}

// The click: what it's on decides what the pad would have done.
void click_press() {
    float pt[2];
    if (!cursor_ui(pt)) return;
    uint8_t* ctl = hovered_control();
    if (help_click(ctl)) return;
    if (map_press()) return;
    if (ctl && in_tab_row(ctl)) {
        tab_click(ctl);
        return;
    }
    if (uint8_t* target = focus_target(ctl)) {
        const bool was = focused_object() == target;
        if (!was) focus(target);
        if (focused_object() != target) {
            if (g_cfg.debug) logf("%s didn't take the focus", rtti_name(target));
        } else if (is_adjustable(target)) {
            if (const int side = adjust_side(target, pt)) {
                g_adjust.key  = side < 0 ? KEY_LEFT : KEY_RIGHT;
                g_adjust.next = GetTickCount() + 400;
                tap(g_adjust.key);
            }
        } else {
            send_key(KEY_A, false);
            g_a_held = true;
        }
        return;
    }
    uint8_t *sel = nullptr, *item = nullptr;
    if (selector_hit(pt, &sel, &item)) {
        if (dp_object(sel, RVA_SELECTED_ITEM_DP) == item) {
            // Already selected: A, where the selector is what has the focus (a car carousel).
            // A tab row's A would be the page's (tuning: apply the setup).
            if (focused_object() == sel) {
                if (g_cfg.debug) logf("click on the selected item of %s: A", rtti_name(sel));
                send_key(KEY_A, false);
                g_a_held = true;
            }
        } else {
            if (g_cfg.debug) logf("click on an item of %s: walking the selection there", rtti_name(sel));
            walk_start(sel, item, pt);
        }
        return;
    }
    if (photo_press(1)) return;   // photo mode: drag to look around
    if (!click_anywhere_screen() && cam_press()) return;   // empty space in a menu: turn the camera
    if (click_anywhere_screen()) {
        send_key(KEY_A, false);
        g_a_held = true;
    } else if (g_cfg.debug) {
        logf("click on nothing (%s): ignored", focus_name());
    }
}

void click_frame() {
    walk_step();
    if (g_adjust.key && GetTickCount() >= g_adjust.next && g_keys.empty()) {
        tap(g_adjust.key);
        g_adjust.next = GetTickCount() + 90;
    }
}

void on_move() {
    if (on_move_cam()) return;
    if (on_move_photo()) return;
    if (on_move_map()) return;
    uint8_t* ctl = hovered_control();
    if (ctl == g_hovered) return;
    g_hovered = ctl;
    if (g_cfg.debug) {
        float r[4] = {};
        if (ctl) bounds(ctl, active_window(), r);
        uint8_t* f = focused_object();
        logf("control under the cursor (%d,%d): %s [%.0f,%.0f - %.0f,%.0f]; focus %s", g_cx.load(), g_cy.load(),
             ctl ? rtti_name(ctl) : "none", r[0], r[1], r[2], r[3], f ? rtti_name(f) : "none");
    }
    // Only what can take the focus: the pause menu's tabs can't, and asking them to takes the
    // current tab from the tab row.
    if (!g_cfg.hover_focus || g_walk.active) return;
    if (uint8_t* target = focus_target(ctl))
        if (focused_object() != target) focus(target);
}

void on_event(const Event& e) {
    if (g_cfg.debug) {
        uint8_t* f = focused_object();
        logf("event %d (%d), cursor %s, focus %s, menu %d", static_cast<int>(e.type), e.a, cursor_wanted() ? "shown" : "hidden",
             f ? rtti_name(f) : "none", menu_active() ? 1 : 0);
    }
    switch (e.type) {
    case Ev::Move: break;   // handled once per frame
    case Ev::LDown:
        if (g_cfg.debug) dump_under_cursor();
        g_walk.active = false;   // a new click: whatever was under way stops
        click_press();
        break;
    case Ev::LUp:
        g_adjust.key = 0;
        g_cam_drag   = false;
        if (photo_release()) break;
        if (map_release()) break;
        if (g_a_held) send_key(KEY_A, true), g_a_held = false;
        break;
    case Ev::RDown:
        if (in_menu()) send_key(KEY_B, false), g_b_held = true;
        break;
    case Ev::RUp:
        if (g_b_held) send_key(KEY_B, true), g_b_held = false;
        break;
    case Ev::Wheel:
        if (map_wheel(e.a)) break;
        if (photo_wheel(e.a)) break;
        if (garage_wheel(e.a)) break;
        if (in_menu()) {
            // The car carousels (the game's ItemsSelector with the focus) go sideways.
            const bool sideways = scrolls_sideways(focused_object());
            const int key = sideways ? (e.a > 0 ? KEY_LEFT : KEY_RIGHT) : (e.a > 0 ? KEY_UP : KEY_DOWN);
            for (int n = 0, steps = std::max(1, std::abs(e.a) / WHEEL_DELTA); n < steps && n < 10; ++n) {
                send_key(key, false);
                send_key(key, true);
            }
        }
        break;
    case Ev::X1Down:
        if (in_menu()) send_key(KEY_LB, false), g_lb_held = true;
        break;
    case Ev::X1Up:
        if (g_lb_held) send_key(KEY_LB, true), g_lb_held = false;
        break;
    case Ev::X2Down:
        if (in_menu()) send_key(KEY_RB, false), g_rb_held = true;
        break;
    case Ev::MDown:
        // The right stick's click (the pad's control, key 119).
        if (in_menu() && !press_pad(KEY_RS_CLICK, true)) tap(KEY_RS_CLICK);
        break;
    case Ev::MUp:
        g_press_held = false;   // the release follows (after at least 80 ms down)
        break;
    case Ev::X2Up:
        if (g_rb_held) send_key(KEY_RB, true), g_rb_held = false;
        break;
    }
}

std::vector<Event> g_batch;   // UI thread only

void frame_inner() {
    bool moved = false;
    for (const Event& e : g_batch) {
        if (e.type == Ev::Move) {
            moved = true;
            continue;
        }
        if (moved) on_move(), moved = false;   // a click goes where the cursor was at the time
        on_event(e);
    }
    if (moved) on_move();
    click_frame();
    flush_one_key();
}

std::atomic<bool> g_broken{false};

// Debug: F9 logs every control's box in the active window, to set against a screenshot.
void dump_controls_in(uint8_t* v, uint8_t* root, int depth, int& n) {
    if (!v || depth > 80 || n > 400) return;
    void* t = type_of(v);
    if (!is(t, TYPE_VISUAL) || opacity(v) <= 0.01f) return;
    if (is(t, TYPE_UIELEMENT) && (control_kind(t) || is(t, TYPE_SELECTOR))) {
        float r[4];
        alignas(16) float matrix[16] = {};
        void* gt = nullptr;
        const bool simple = game<ToAncestorFn>(RVA_TO_ANCESTOR)(v, root, 0, &gt, matrix);
        release(static_cast<uint8_t*>(gt));
        if (bounds(v, root, r)) {
            ++n;
            logf("  %s [%.0f,%.0f - %.0f,%.0f] %s%s", rtti_name(v), r[0], r[1], r[2], r[3], simple ? "" : "(not simple) ",
                 bool_property(v, RVA_FOCUSABLE_DP) ? "focusable" : "");
        }
    }
    const int c = vfn<CountFn>(v, VT_CHILD_COUNT)(v);
    for (int i = 0; i < c && i < 4096; ++i) dump_controls_in(vfn<ChildFn>(v, VT_CHILD)(v, i), root, depth + 1, n);
}

std::atomic<uint8_t*> g_ui{nullptr};   // the UserInterface object (its +0x120: the camera tweak)

void dump_floats(const char* what, uint8_t* p, int from, int to) {
    std::string out;
    char b[48];
    for (int o = from; o < to; o += 4) {
        std::snprintf(b, sizeof b, " +%X=%.3f", o, *reinterpret_cast<float*>(p + o));
        out += b;
    }
    logf("%s %p:%s", what, p, out.c_str());
}

void debug_frame() {
    static bool f10 = false;
    HWND hw = g_hwnd.load();
    const bool now10 = hw && GetForegroundWindow() == hw && (GetAsyncKeyState(VK_F10) & 0x8000);
    if (now10 && !f10 && g_cfg.debug) {
        if (uint8_t* ui = g_ui.load())
            if (uint8_t* cam = at<uint8_t*>(ui, 0x120)) {
                logf("camera tweak %s", rtti_name(cam));
                dump_floats("  behavior", cam, 0x18, 0x80);
                if (uint8_t* t = at<uint8_t*>(cam, 0x10)) dump_floats("  target", t, 0, 0x40);
            }
    }
    f10 = now10;
    static bool down = false;
    HWND h = g_hwnd.load();
    const bool now = h && GetForegroundWindow() == h && (GetAsyncKeyState(VK_F9) & 0x8000);
    if (now && !down && g_cfg.debug) {
        int n = 0;
        logf("controls (focus %s):", focus_name());
        if (uint8_t* root = active_window()) dump_controls_in(root, root, 0, n);
    }
    down = now;
}

// ---- the decal editor ---------------------------------------------------------------------------
// Design editor / Edit layer (LiveryLayerPosition) moves, sizes, spins and skews the layer by
// two analog menu controls, 111 (right +) and 112 (up +), read with GetAnalog(input, context,
// control) (0x106F040; context 7 is the menus). On that page, with the game window in front,
// W A S D push them all the way.
constexpr uintptr_t RVA_GET_ANALOG = 0x106F040;
constexpr int kDecalX = 111, kDecalY = 112;
std::atomic<bool> g_decal_keys{false};

// The garage's camera (UICameraTweakBehavior) turns by the right stick, menu controls 113 (right
// +) and 114 (up +). A left drag on empty space in a menu turns it: this frame's stick values,
// set before the UI's update and cleared after.
constexpr int kCamX = 113, kCamY = 114;

void log_analog(const char* what, int context, int control, float v, void* from) {
    static int lc = -1, ll = -1;
    static DWORD lt = 0;
    if (context == lc && control == ll && GetTickCount() - lt < 500) return;
    lc = context, ll = control, lt = GetTickCount();
    logf("%s: context %d control %d = %.2f, asked from game+0x%llX", what, context, control, v,
         static_cast<unsigned long long>(static_cast<uint8_t*>(from) - g_base));
}

// Debug only: what asks about buttons, through the other queries.
using ButtonStateFn = int (*)(void* input, int context, int control, uint8_t a);
ButtonStateFn o_dbg_state = nullptr;
int dbg_state(void* input, int context, int control, uint8_t a) {
    const int v = o_dbg_state(input, context, control, a);
    if (v) log_analog("state", context, control, static_cast<float>(v), __builtin_return_address(0));
    return v;
}
using GetDigitalFn = float (*)(void* input, int context, int control);
GetDigitalFn o_dbg_digital = nullptr;
float dbg_digital(void* input, int context, int control) {
    const float v = o_dbg_digital(input, context, control);
    if (v != 0.f) log_analog("digital", context, control, v, __builtin_return_address(0));
    return v;
}

float get_analog(void* input, int context, int control) {
    const float v = o_get_analog(input, context, control);
    if (v != 0.f && g_cfg.debug && !(context == 7 && (control == kDecalX || control == kDecalY)))
        log_analog("analog", context, control, v, __builtin_return_address(0));
    if (context == 7 && (control == kCamX || control == kCamY)) {
        const float d = control == kCamX ? g_cam_stick_x.load() : g_cam_stick_y.load();
        if (d) return std::max(-1.f, std::min(1.f, v + d));
    }
    if (context != 7 || (control != kDecalX && control != kDecalY) || !g_decal_keys.load()) return v;
    const float k = control == kDecalX ? key_axis('A', 'D') : key_axis('S', 'W');
    return k ? std::max(-1.f, std::min(1.f, v + k)) : v;
}

void photo_frame() {
    HWND h = g_hwnd.load();
    const bool front = h && GetForegroundWindow() == h;
    g_photo_keys = front && in_photo_mode();
    g_decal_keys = front && !std::strcmp(focus_name(), ".?AVLiveryLayerPosition@@");
}

void map_frame_guarded() {
    __try {
        photo_frame();
        debug_frame();
        map_frame();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool frame_guarded() {
    __try {
        frame_inner();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void detour_update(void* self, float dt) {
    g_ui = static_cast<uint8_t*>(self);
    press_frame();
    g_last_frame = GetTickCount();
    g_framed     = true;
    if (g_cfg.enabled && !g_broken.load()) {
        poll_mouse();
        map_frame_guarded();
        {
            std::lock_guard<std::mutex> lock(g_mx);
            g_batch.swap(g_events);
        }
        if ((!g_batch.empty() || !g_keys.empty() || g_walk.active || g_adjust.key) && !frame_guarded()) {
            g_broken = true;
            logf("stopped: the game's UI code didn't behave as expected (exception)");
        }
        g_batch.clear();
        cam_frame_begin();
    }
    o_update(self, dt);
}

// ---- start-up -----------------------------------------------------------------------------------

// Where a jump someone else put at the start of a function goes (jmp rel32/rel8, jmp [rip+x],
// mov rax, imm64; jmp rax), or nullptr if it doesn't start with one.
const uint8_t* jump_target(const uint8_t* p) {
    if (p[0] == 0xE9) { int32_t d; std::memcpy(&d, p + 1, 4); return p + 5 + d; }
    if (p[0] == 0xEB) return p + 2 + static_cast<int8_t>(p[1]);
    if (p[0] == 0xFF && p[1] == 0x25) {
        int32_t d; std::memcpy(&d, p + 2, 4);
        const uint8_t* t; std::memcpy(&t, p + 6 + d, sizeof t); return t;
    }
    if (p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0) {
        const uint8_t* t; std::memcpy(&t, p + 2, sizeof t); return t;
    }
    return nullptr;
}

// "name.dll+0x1234" for an address, for the log.
std::string module_of(const void* addr) {
    HMODULE m = nullptr;
    char path[MAX_PATH] = "?";
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(addr, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return "unmapped memory";
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(addr), &m) || !m)
        return "memory outside any DLL";
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* name = std::strrchr(path, '\\');
    char buf[MAX_PATH + 32];
    std::snprintf(buf, sizeof buf, "%s+0x%llX", name ? name + 1 : path,
                  static_cast<unsigned long long>(static_cast<const uint8_t*>(addr) - reinterpret_cast<uint8_t*>(m)));
    return buf;
}

bool code_matches() {
    for (const Expect& e : kExpected) {
        const uint8_t* p = g_base + e.rva;
        if (std::memcmp(p, e.bytes, sizeof e.bytes) == 0) continue;
        char hex[3 * sizeof e.bytes + 1] = {};
        for (std::size_t i = 0; i < sizeof e.bytes; ++i) std::snprintf(hex + 3 * i, 4, "%02X ", p[i]);
        hex[3 * sizeof e.bytes - 1] = 0;
        const uint8_t* target = jump_target(p);
        std::string where = target ? ", a jump into " + module_of(target) : "";
        // Update is only hooked through its vtable slot, so another tool's detour on its code is fine.
        if (e.rva == RVA_UI_UPDATE && target) {
            logf("UserInterface::Update was already hooked by something else (%s); going on", where.c_str() + 2);
            continue;
        }
        logf("game code at +0x%llX isn't as expected (%s%s); not starting", static_cast<unsigned long long>(e.rva), hex,
             where.c_str());
        return false;
    }
    if (at<uint8_t*>(g_base, RVA_UI_UPDATE_SLOT) != g_base + RVA_UI_UPDATE) {
        logf("UserInterface::Update isn't where expected; not starting");
        return false;
    }
    return true;
}

DWORD WINAPI start(LPVOID) {
    g_base = static_cast<uint8_t*>(g_api->wait_for_code(0xFFFFFFFFu));
    if (!g_base || !code_matches()) return 0;
    void** slot = reinterpret_cast<void**>(g_base + RVA_UI_UPDATE_SLOT);
    if (!g_api->hook_pointer(g_mod, "UserInterface::Update", slot, reinterpret_cast<void*>(&detour_update),
                             reinterpret_cast<void**>(&o_update)) ||
        !g_api->enable_hooks(g_mod, nullptr)) {
        logf("couldn't hook UserInterface::Update; not starting");
        return 0;
    }
    // Pad-level presses for the help bar: optional, keys are sent without it.
    static const uint8_t pressed_bytes[] = {0x44, 0x88, 0x4C, 0x24, 0x20, 0x55, 0x57, 0x41, 0x54, 0x41, 0x57, 0x48};
    if (!std::memcmp(g_base + RVA_WAS_PRESSED, pressed_bytes, sizeof pressed_bytes) &&
        g_api->hook_function(g_mod, "input WasPressed", g_base + RVA_WAS_PRESSED, reinterpret_cast<void*>(&was_pressed),
                             reinterpret_cast<void**>(&o_was_pressed)))
        g_api->enable_hooks(g_mod, nullptr);
    else
        o_was_pressed = nullptr, logf("the input check isn't as expected: help bar clicks send keys only");
    // The decal editor's W A S D: optional too.
    static const uint8_t analog_bytes[] = {0x40, 0x53, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x68};
    if (!std::memcmp(g_base + RVA_GET_ANALOG, analog_bytes, sizeof analog_bytes) &&
        g_api->hook_function(g_mod, "input GetAnalog", g_base + RVA_GET_ANALOG, reinterpret_cast<void*>(&get_analog),
                             reinterpret_cast<void**>(&o_get_analog)))
        g_api->enable_hooks(g_mod, nullptr);
    else
        o_get_analog = nullptr, logf("the analog input isn't as expected: no W A S D for decals");
    if (g_cfg.debug) {   // which game code asks about which button (for finding new ones)
        if (g_api->hook_function(g_mod, "input ButtonState (debug)", g_base + 0x106E690, reinterpret_cast<void*>(&dbg_state),
                                 reinterpret_cast<void**>(&o_dbg_state)) &&
            g_api->hook_function(g_mod, "input GetDigital (debug)", g_base + 0x106E3E0, reinterpret_cast<void*>(&dbg_digital),
                                 reinterpret_cast<void**>(&o_dbg_digital)))
            g_api->enable_hooks(g_mod, nullptr);
    }
    // The garage camera's wheel zoom: optional.
    static const uint8_t garage_bytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89};
    if (!std::memcmp(g_base + RVA_GARAGE_CAMERA, garage_bytes, sizeof garage_bytes) &&
        g_api->hook_function(g_mod, "garage camera", g_base + RVA_GARAGE_CAMERA, reinterpret_cast<void*>(&garage_camera),
                             reinterpret_cast<void**>(&o_garage_camera)))
        g_api->enable_hooks(g_mod, nullptr);
    else
        o_garage_camera = nullptr, logf("the garage camera isn't as expected: no wheel zoom there");
    // Photo mode's camera: optional, the rest works without it.
    void** photo_slot = reinterpret_cast<void**>(g_base + RVA_PHOTO_INPUT_SLOT);
    static const uint8_t photo_bytes[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x80, 0xB9, 0x2C, 0x02, 0x00, 0x00};
    if (*photo_slot == g_base + RVA_PHOTO_INPUT && !std::memcmp(g_base + RVA_PHOTO_INPUT, photo_bytes, sizeof photo_bytes) &&
        g_api->hook_pointer(g_mod, "CCamPhotoMode input", photo_slot, reinterpret_cast<void*>(&photo_input),
                            reinterpret_cast<void**>(&o_photo_input)))
        g_api->enable_hooks(g_mod, nullptr);
    else
        logf("photo mode's camera isn't as expected: no mouse there");
    g_arrow = LoadCursorW(nullptr, IDC_ARROW);
    if (HANDLE t = CreateThread(nullptr, 0, window_thread, nullptr, 0, nullptr)) CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, hook_thread, nullptr, 0, nullptr)) CloseHandle(t);
    logf("ready (hover selects: %s, hides after %lu s)", g_cfg.hover_focus ? "on" : "off", g_cfg.hide_ms / 1000);
    return 0;
}

} // namespace

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    if (api->api_version < 3) return 1;
    g_api = api;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&fh2mod_init), &g_self);
    FH2ModInfo info{};
    info.size             = sizeof info;
    info.name             = "FH2Mouse";
    info.version          = FH2MOUSE_VERSION;
    info.github_repo      = "qiangqiang101/FH2Mouse";
    info.platform_version = FH2MOD_PLATFORM_VERSION;
    info.check_updates    = 1;
    g_mod                 = api->register_mod(&info);
    if (!g_mod) return 1;
    load_settings();
    if (!g_cfg.enabled) {
        api->log(g_mod, "off (FH2Mouse\\FH2Mouse.ini, Enabled=0)");
        return 0;
    }
    if (HANDLE t = CreateThread(nullptr, 0, start, nullptr, 0, nullptr)) CloseHandle(t);
    return 0;
}
