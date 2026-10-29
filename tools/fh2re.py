"""Small static-analysis helpers for forza_durango_release_final.exe (RTTI, xrefs, disasm)."""
import bisect
import re
import struct

import capstone
import numpy as np
import pefile

EXE = r"D:\Games\FH2\Anthem_1.0.0.64_x64__8wekyb3d8bbwe\Mount\forza_durango_release_final.exe"
BASE = 0x140000000


class Image:
    def __init__(self, path=EXE):
        self.pe = pefile.PE(path, fast_load=True)
        self.size = self.pe.OPTIONAL_HEADER.SizeOfImage
        self.mem = bytearray(self.size)
        self.mem[:len(self.pe.header)] = self.pe.header
        self.secs = []
        for s in self.pe.sections:
            data = s.get_data()
            va, vs = s.VirtualAddress, s.Misc_VirtualSize
            n = min(len(data), vs) if vs else len(data)
            self.mem[va:va + n] = data[:n]
            self.secs.append((s.Name.rstrip(b'\0').decode(), va, max(vs, len(data)), s.Characteristics))
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self._pdata = None
        self._i32 = {}

    def sec(self, name):
        for n, va, vs, c in self.secs:
            if n == name:
                return va, vs

    def code_ranges(self):
        return [(va, vs) for n, va, vs, c in self.secs if c & 0x20000000]

    def u32(self, rva): return struct.unpack_from('<I', self.mem, rva)[0]
    def i32(self, rva): return struct.unpack_from('<i', self.mem, rva)[0]
    def u64(self, rva): return struct.unpack_from('<Q', self.mem, rva)[0]

    def cstr(self, rva, n=256):
        e = self.mem.find(b'\0', rva, rva + n)
        return self.mem[rva:e].decode('latin1')

    # ---- RTTI ----
    def type_descriptor(self, mangled):
        i = self.mem.find(mangled.encode() + b'\0')
        return None if i < 0 else i - 0x10

    def cols_for_td(self, td):
        out, start, pat = [], 0, struct.pack('<I', td)
        while True:
            i = self.mem.find(pat, start)
            if i < 0:
                return out
            start = i + 1
            col = i - 12
            if col >= 0 and self.u32(col) == 1 and self.u32(col + 20) == col:
                out.append(col)

    def vtables_for_col(self, col):
        return [p + 8 for p in self.find_ptr(col)]

    def vtables(self, mangled):
        td = self.type_descriptor(mangled)
        if td is None:
            return []
        return [(vt, self.u32(col + 4)) for col in self.cols_for_td(td) for vt in self.vtables_for_col(col)]

    def code_bounds(self):
        rs = self.code_ranges()
        return min(r[0] for r in rs), max(r[0] + r[1] for r in rs)

    def vt_slots(self, vt, n=200):
        out = []
        lo, hi = self.code_bounds()
        for k in range(n):
            if k and self.rtti_name_of_vtable(vt + 8 * k):
                break
            p = self.u64(vt + 8 * k)
            if not (BASE + lo <= p < BASE + hi):
                break
            out.append(p - BASE)
        return out

    def rtti_name_of_vtable(self, vt):
        col = self.u64(vt - 8) - BASE
        if not (0 < col < self.size - 24) or self.u32(col) != 1 or self.u32(col + 20) != col:
            return None
        return self.cstr(self.u32(col + 12) + 0x10)

    # ---- functions ----
    def pdata(self):
        if self._pdata is None:
            va, vs = self.sec('.pdata')
            ents = []
            for o in range(va, va + vs - 11, 12):
                b, e, u = struct.unpack_from('<III', self.mem, o)
                if b == 0:
                    break
                ents.append((b, e, u))
            ents.sort()
            self._pdata = ents
            self._pstarts = [x[0] for x in ents]
        return self._pdata

    def func_of(self, rva):
        p = self.pdata()
        i = bisect.bisect_right(self._pstarts, rva) - 1
        if i < 0:
            return None
        b, e, u = p[i]
        if not (b <= rva < e):
            return None
        for _ in range(16):
            ui = u & ~1
            if (self.mem[ui] >> 3) & 4:  # chained unwind info
                cnt = self.mem[ui + 2]
                b, e, u = struct.unpack_from('<III', self.mem, ui + 4 + ((cnt + 1) & ~1) * 2)
            else:
                break
        return b

    def func_end(self, start):
        p = self.pdata()
        i = bisect.bisect_left(self._pstarts, start)
        if i < len(p) and p[i][0] == start:
            return p[i][1]
        return start + 0x200

    def disasm(self, rva, n=None, end=None):
        if end is None and n is None:
            end = self.func_end(rva)
        code = bytes(self.mem[rva:(end if end else rva + n * 15)])
        out = []
        for ins in self.md.disasm(code, BASE + rva):
            out.append(ins)
            if n and len(out) >= n:
                break
        return out

    def dis_text(self, rva, n=None, end=None):
        lines = []
        for ins in self.disasm(rva, n, end):
            s = f"{ins.address - BASE:08X}  {ins.mnemonic:7s} {ins.op_str}"
            m = re.search(r'\[rip ([+-]) 0x([0-9a-f]+)\]', ins.op_str)
            if m:
                t = ins.address + ins.size + int(m.group(2), 16) * (1 if m.group(1) == '+' else -1) - BASE
                s += f"   ; ->{t:X}" + self.describe(t)
            elif ins.mnemonic in ('call', 'jmp') or ins.mnemonic.startswith('j'):
                if ins.op_str.startswith('0x'):
                    s = s.replace(ins.op_str, f"{int(ins.op_str, 16) - BASE:X}")
            lines.append(s)
        return '\n'.join(lines)

    def describe(self, t):
        if not (8 <= t < self.size - 8):
            return ''
        n = self.rtti_name_of_vtable(t)
        if n:
            return f" vtable {n}"
        b = self.mem[t:t + 8]
        if all(32 <= c < 127 for c in b[:4]):
            return ' "' + self.cstr(t, 80) + '"'
        if b[1] == 0 and b[3] == 0 and 32 <= b[0] < 127 and 32 <= b[2] < 127:
            try:
                return ' L"' + bytes(self.mem[t:t + 160]).decode('utf-16le', 'replace').split('\0')[0] + '"'
            except Exception:
                pass
        return ''

    # ---- xrefs ----
    def _rel_index(self):
        if not self._i32:
            for va, vs in self.code_ranges():
                arr = np.frombuffer(bytes(self.mem[va:va + vs]), dtype=np.uint8)
                d = arr[:len(arr) - 3]
                v = (d.astype(np.int64) | (arr[1:len(arr) - 2].astype(np.int64) << 8) |
                     (arr[2:len(arr) - 1].astype(np.int64) << 16) | (arr[3:].astype(np.int64) << 24))
                v = np.where(v >= 2 ** 31, v - 2 ** 32, v)
                self._i32[va] = v + np.arange(len(v), dtype=np.int64) + va + 4
        return self._i32

    def find_refs(self, target):
        """rel32 operands (calls, jmps, rip-relative) resolving to target (allowing up to 4 bytes of trailing imm)."""
        res = set()
        for va, t in self._rel_index().items():
            for extra in (0, 1, 2, 4):
                for h in np.nonzero(t + extra == target)[0]:
                    res.add(int(h) + va)
        return sorted(res)

    def find_ptr(self, target):
        pat = struct.pack('<Q', BASE + target)
        out, s = [], 0
        while True:
            i = self.mem.find(pat, s)
            if i < 0:
                return out
            out.append(i)
            s = i + 1

    def find_bytes(self, b):
        out, s = [], 0
        while True:
            i = self.mem.find(b, s)
            if i < 0:
                return out
            out.append(i)
            s = i + 1
