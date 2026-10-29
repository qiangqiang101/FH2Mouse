"""Virtual Xbox 360 pad for testing (ViGEmBus), driven over a local socket.

  python pad.py serve <path-to-ViGEmClient.dll>     keeps the pad plugged in
  python pad.py <cmd> [; <cmd>...]                  sends commands to it

Commands: press BUTTON [ms] | hold BUTTON ms | stick lx ly ms | trigger lt rt ms | wait ms | quit
Buttons: A B X Y LB RB START BACK UP DOWN LEFT RIGHT LS RS GUIDE
"""
import ctypes
import socket
import sys
import time

PORT = 47123
BUTTONS = {'UP': 0x1, 'DOWN': 0x2, 'LEFT': 0x4, 'RIGHT': 0x8, 'START': 0x10, 'BACK': 0x20, 'LS': 0x40, 'RS': 0x80,
           'LB': 0x100, 'RB': 0x200, 'GUIDE': 0x400, 'A': 0x1000, 'B': 0x2000, 'X': 0x4000, 'Y': 0x8000}


class Report(ctypes.Structure):
    _fields_ = [('wButtons', ctypes.c_ushort), ('bLeftTrigger', ctypes.c_ubyte), ('bRightTrigger', ctypes.c_ubyte),
                ('sThumbLX', ctypes.c_short), ('sThumbLY', ctypes.c_short), ('sThumbRX', ctypes.c_short),
                ('sThumbRY', ctypes.c_short)]


def serve(dll_path):
    vg = ctypes.CDLL(dll_path)
    vg.vigem_alloc.restype = ctypes.c_void_p
    vg.vigem_target_x360_alloc.restype = ctypes.c_void_p
    vg.vigem_connect.argtypes = [ctypes.c_void_p]
    vg.vigem_target_add.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    vg.vigem_target_x360_update.argtypes = [ctypes.c_void_p, ctypes.c_void_p, Report]
    client = vg.vigem_alloc()
    err = vg.vigem_connect(client)
    if err & 0xFFFFFFFF != 0x20000000:
        print('connect failed', hex(err & 0xFFFFFFFF)); return
    pad = vg.vigem_target_x360_alloc()
    err = vg.vigem_target_add(client, pad)
    if err & 0xFFFFFFFF != 0x20000000:
        print('add failed', hex(err & 0xFFFFFFFF)); return
    print('pad plugged in', flush=True)

    def send(r):
        vg.vigem_target_x360_update(client, pad, r)

    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', PORT))
    srv.listen(1)
    while True:
        conn, _ = srv.accept()
        data = conn.recv(4096).decode()
        for cmd in data.split(';'):
            a = cmd.split()
            if not a:
                continue
            c = a[0].lower()
            r = Report()
            if c in ('press', 'hold'):
                r.wButtons = BUTTONS[a[1].upper()]
                send(r); time.sleep(int(a[2]) / 1000 if len(a) > 2 else 0.12); send(Report())
            elif c == 'stick':
                r.sThumbLX, r.sThumbLY = int(a[1]), int(a[2])
                send(r); time.sleep(int(a[3]) / 1000); send(Report())
            elif c == 'trigger':
                r.bLeftTrigger, r.bRightTrigger = int(a[1]), int(a[2])
                send(r); time.sleep(int(a[3]) / 1000); send(Report())
            elif c == 'wait':
                time.sleep(int(a[1]) / 1000)
            elif c == 'quit':
                conn.sendall(b'bye'); conn.close(); return
            time.sleep(0.15)
        conn.sendall(b'ok')
        conn.close()


if __name__ == '__main__':
    if sys.argv[1] == 'serve':
        serve(sys.argv[2])
    else:
        s = socket.create_connection(('127.0.0.1', PORT))
        s.sendall(' '.join(sys.argv[1:]).encode())
        s.settimeout(120)
        print(s.recv(16).decode())
