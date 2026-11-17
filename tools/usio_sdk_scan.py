"""Find the Namco USIO SDK callback race in a PS3 game executable.

Usage:  python tools/usio_sdk_scan.py EBOOT.ELF [GAME.SELF ...]
Needs:  pip install pyelftools capstone

Accepts a plain ELF or a debug (fake-signed, unencrypted) SELF. Read-only.
For each file it reports:
  * race sites: a call to a sys_semaphore_post (syscall 0x5E) wrapper that is
    followed by a reload of a context field and an indirect call (bcctrl),
    i.e. "release the context, then read the callback" (docs/SDK-callback-race.md);
  * the SDK command chain near those sites: READ 0x1000 with flags 3 and
    WRITE 0x1400 with flags 1 (r5 = register, r6 = length, r7 = flags).
"""
import io
import struct
import sys
import zlib
from pathlib import Path

from elftools.elf.elffile import ELFFile

BLR, NOP = 0x4E800020, 0x60000000


def load_elf(path):
    raw = Path(path).read_bytes()
    if raw[:4] == b'\x7fELF':
        return raw
    if raw[:4] != b'SCE\0':
        raise ValueError('not an ELF or SELF')
    if raw[8:10] != b'\x80\0':
        raise ValueError('encrypted SELF: decrypt it first')
    ehdr, phdr, sinfo = struct.unpack_from('>QQ', raw, 0x30) + (struct.unpack_from('>Q', raw, 0x48)[0],)
    count = struct.unpack_from('>H', raw, ehdr + 56)[0]
    headers = [struct.unpack_from('>IIQQQQQQ', raw, phdr + i * 56) for i in range(count)]
    infos = [struct.unpack_from('>QQIIQ', raw, sinfo + i * 32) for i in range(count)]
    size = max(p[2] + p[5] for p in headers if p[0] == 1)
    out = bytearray(size)
    out[:0x40] = raw[ehdr:ehdr + 0x40]
    out[struct.unpack_from('>Q', out, 32)[0]:][:count * 56] = raw[phdr:phdr + count * 56]
    for p, info in zip(headers, infos):
        if p[0] != 1 or not p[5]:
            continue
        offset, length, compression = info[0], info[1], info[2]
        chunk = raw[offset:offset + length]
        if compression == 2:
            chunk = zlib.decompress(chunk)
        out[p[2]:p[2] + p[5]] = chunk
    struct.pack_into('>Q', out, 40, 0)          # drop the section table
    struct.pack_into('>HHH', out, 58, 0, 0, 0)
    return bytes(out)


def code_words(elf):
    code = {}
    for seg in elf.iter_segments():
        if seg['p_type'] != 'PT_LOAD' or not seg['p_flags'] & 1:
            continue
        data, base = seg.data(), seg['p_vaddr']
        for off in range(0, len(data) - 3, 4):
            code[base + off] = struct.unpack_from('>I', data, off)[0]
    return code


def call_target(at, word):
    if word >> 26 != 18 or not word & 1:
        return None
    rel = word & 0x3FFFFFC
    if rel & 0x2000000:
        rel -= 0x4000000
    return rel if word & 2 else at + rel


def function_start(code, at, limit=400):
    for _ in range(limit):
        if code.get(at - 4) in (BLR, None):
            break
        at -= 4
    while code.get(at) == NOP:
        at += 4
    return at


def semaphore_post_functions(code):
    stubs = set()
    for at, word in code.items():
        if word == 0x3960005E and any(code.get(at + 4 * k) == 0x44000002 for k in range(1, 4)):
            stubs.add(function_start(code, at, 80))
    direct = [a for a, w in code.items() if call_target(a, w) in stubs]
    for at in direct:                           # small wrappers around the stub
        start, n, end = function_start(code, at), 0, None
        end = start
        while code.get(end) != BLR and n < 60:
            end += 4
            n += 1
        if n < 60:
            stubs.add(start)
    return stubs


def reloads_and_calls(code, start, depth=0, seen=None):
    seen = set() if seen is None else seen
    loaded, at = False, start
    for _ in range(48):
        if at in seen:
            return False
        seen.add(at)
        word = code.get(at)
        if word is None or word == BLR:
            return False
        op = word >> 26
        if (word & 0xFC0007FF) == 0x4C000421:  # bcctrl/bctrl with link
            return loaded
        if op == 18:
            if word & 1:
                return False
            at = call_target(at, word | 1)
            continue
        if op in (32, 58) and (word >> 16) & 31 not in (1, 2):
            loaded = True
        if op == 16 and depth < 3:
            disp = word & 0xFFFC
            disp = disp - 0x10000 if disp & 0x8000 else disp
            if reloads_and_calls(code, at + disp, depth + 1, seen):
                return True
        at += 4
    return False


def command_chain(code, lo, hi):
    found = []
    for at in range(lo, hi, 4):
        word = code.get(at)
        if not word or word >> 26 != 14 or (word >> 16) & 31 or (word & 0xFFFF) not in (0x1000, 0x1400):
            continue
        regs = {}
        for k in range(-8, 10):
            w = code.get(at + 4 * k) or 0
            if w >> 26 == 14 and (w >> 16) & 31 == 0:
                regs[(w >> 21) & 31] = w & 0xFFFF
            if w >> 26 == 18 and w & 1 and k > 0:
                break
        if regs.get(5) in (0x1000, 0x1400) and 7 in regs:
            kind = 'READ ' if regs[5] == 0x1000 else 'WRITE'
            length = f'{regs[6]:#x}' if 6 in regs else 'computed'
            found.append(f'{at:08X} {kind} {regs[5]:#06x} len={length} flags={regs[7]}')
    return found


def scan(path):
    elf = ELFFile(io.BytesIO(load_elf(path)))
    code = code_words(elf)
    posts = semaphore_post_functions(code)
    sites = sorted(a for a, w in code.items() if call_target(a, w) in posts and reloads_and_calls(code, a + 4))
    print(f'== {path}')
    if not sites:
        print('   no post-then-callback site found')
        return
    print('   race sites:', ' '.join(f'{s:08X}' for s in sites))
    for line in command_chain(code, min(sites) - 0x2000, max(sites) + 0x3000):
        print('   ', line)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for name in sys.argv[1:]:
        scan(name)
