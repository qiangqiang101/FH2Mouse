"""Drive the game for testing: python drive.py <cmd> [args]...  (several commands separated by ';')

  shot NAME            screenshot to tools/shots/NAME.png (half size)
  move X Y             move the cursor (screen pixels), in a few steps
  click | rclick       left / right click
  down | up            left button down / up
  wheel N              wheel notches (+ up, - down)
  xbutton 1|2          side button click
  key VK               tap a virtual key (hex like 0x1B or a letter)
  wait MS              sleep
  focus                bring the game window to the front
"""
import ctypes
import os
import sys
import time
from ctypes import wintypes

from PIL import ImageGrab

u32 = ctypes.windll.user32
u32.SetProcessDPIAware()
SHOTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'shots')

MOUSEEVENTF_MOVE, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP = 0x1, 0x2, 0x4
MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP = 0x8, 0x10
MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, MOUSEEVENTF_WHEEL = 0x80, 0x100, 0x800


def mouse(flags, data=0):
    u32.mouse_event(flags, 0, 0, ctypes.c_int(data).value & 0xFFFFFFFF, 0)


def move(x, y):
    # Absolute moves through the input stream, like a real mouse (SetCursorPos isn't seen by hooks).
    p = wintypes.POINT()
    u32.GetCursorPos(ctypes.byref(p))
    w, h = u32.GetSystemMetrics(0), u32.GetSystemMetrics(1)
    for i in range(1, 9):
        cx, cy = p.x + (x - p.x) * i / 8, p.y + (y - p.y) * i / 8
        u32.mouse_event(MOUSEEVENTF_MOVE | 0x8000, int(cx * 65535 / (w - 1)), int(cy * 65535 / (h - 1)), 0, 0)
        time.sleep(0.015)
    time.sleep(0.05)


def key(vk):
    sc = u32.MapVirtualKeyW(vk, 0)
    u32.keybd_event(vk, sc, 0, 0)
    time.sleep(0.12)
    u32.keybd_event(vk, sc, 2, 0)


def focus():
    pid = None
    import subprocess
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq azimuth.exe', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.strip('"').split('","')
        if len(parts) > 1 and parts[0].lower() == 'azimuth.exe':
            pid = int(parts[1])
    if pid is None:
        print('azimuth.exe not running')
        return
    best = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    def cb(h, _):
        p = wintypes.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        cn = ctypes.create_unicode_buffer(64)
        u32.GetClassNameW(h, cn, 64)
        if p.value == pid and cn.value == 'Azimuth.Game.Win32':
            best.append(h)
        return True
    u32.EnumWindows(cb, 0)
    for h in best:
        # SetForegroundWindow from a background process needs the foreground lock lifted: a key tap does it.
        u32.keybd_event(0x12, 0, 0, 0); u32.keybd_event(0x12, 0, 2, 0)
        u32.SetForegroundWindow(h)
    time.sleep(0.2)
    print('foreground is game:', u32.GetForegroundWindow() in best)
    print('windows', best)


def run(cmd):
    a = cmd.split()
    if not a:
        return
    c = a[0]
    if c == 'shot':
        os.makedirs(SHOTS, exist_ok=True)
        img = ImageGrab.grab()
        img = img.resize((img.width // 2, img.height // 2))
        path = os.path.join(SHOTS, a[1] + '.png')
        img.save(path)
        print('saved', path)
    elif c == 'move':
        move(int(a[1]), int(a[2]))
    elif c == 'click':
        mouse(MOUSEEVENTF_LEFTDOWN); time.sleep(0.08); mouse(MOUSEEVENTF_LEFTUP)
    elif c == 'rclick':
        mouse(MOUSEEVENTF_RIGHTDOWN); time.sleep(0.08); mouse(MOUSEEVENTF_RIGHTUP)
    elif c == 'down':
        mouse(MOUSEEVENTF_LEFTDOWN)
    elif c == 'up':
        mouse(MOUSEEVENTF_LEFTUP)
    elif c == 'wheel':
        for _ in range(abs(int(a[1]))):
            mouse(MOUSEEVENTF_WHEEL, 120 if int(a[1]) > 0 else -120)
            time.sleep(0.12)
    elif c == 'xbutton':
        mouse(MOUSEEVENTF_XDOWN, int(a[1])); time.sleep(0.08); mouse(MOUSEEVENTF_XUP, int(a[1]))
    elif c == 'key':
        key(int(a[1], 16) if a[1].startswith('0x') else ord(a[1].upper()))
    elif c == 'wait':
        time.sleep(int(a[1]) / 1000)
    elif c == 'focus':
        focus()
    else:
        print('unknown', c)


if __name__ == '__main__':
    for part in ' '.join(sys.argv[1:]).split(';'):
        run(part.strip())
