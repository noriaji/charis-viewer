#!/usr/bin/env python3
"""The shader bundle (app_settings/vk_shader_bundle/shaders.vkb): make one from
the SPIR-V cache folders a --bake-shaders run fills, check one, and record
which keys it holds and what is in them.

  python -I shaderbundle.py pack DIR... -o OUT --commit SHA
  python -I shaderbundle.py info VKB
  python -I shaderbundle.py manifest VKB [-o TSV]
  python -I shaderbundle.py compare-manifest NEW BASELINE
  python -I shaderbundle.py check VKB --commit SHA [--manifest TSV] [--max-bytes N]

pack     reads the <16 hex>.vkp (SPIR-V and reflection) and <16 hex>.glv (GL
          view) files of the folders, checks each file's trailer (size and
          XXH3-64), and writes one bundle: modules once each, tables sorted by
          key, so the same files make the same bytes. A key in more than one
          folder: the first folder's file. The folders must come from one exe
          (one CACHE_VERSION, GL_VIEW_VERSION and glslang): a mixture stops.
info      the header, the counts and whether the trailer matches (exit 1 if not).
manifest  one line a key: kind (program / view), the key, and the SHA-256 of
          what the key stands for (a program: each stage and its SPIR-V, then
          the reflection; a view: its bytes). Two bundles with the same lines
          answer every key alike, however they were packed.
compare-manifest
          NEW against BASELINE (e.g. a bake on a hardware GPU): exit 1 when a
          key both have differs, or BASELINE has a key NEW lacks. Keys only NEW
          has are counted, not refused (research/312 section 10.4: the start-up load
          of a software device adds a few).
check     a bundle made by another job (the release workflow's bake job, which
          runs Mesa), read back and nothing in it run: at most --max-bytes
          (default BUNDLE_MAX_BYTES, looked at before reading), the trailer, the
          header (BUNDLE_VERSION, the commit), the counts, every table and
          entry in place, keys strictly ascending, whole SPIR-V modules with
          only the version, generator, memory model, capabilities, extensions
          and instruction sets glslang makes for this viewer (an allow list),
          known stages, reflections and GL views the viewer reads to their
          end, and with --manifest exactly the keys and contents listed there.
          Exit 1 when anything is off (check_bundle()). It checks the form,
          not that the SPIR-V is the one the sources make.

The layout is the one in indra/llvulkan/llvkshader.cpp ("The shader bundle");
change BUNDLE_VERSION in both. The viewer uses a bundle only when its header's
versions are its own, and checks the trailer before it reads anything.

No network. With numpy, XXH3 is much faster (pure Python works, slowly).
"""
import argparse
import collections
import hashlib
import os
import re
import struct
import sys

try:
    import numpy as np
except ImportError:     # the pure Python XXH3 below
    np = None

# ---- llvkshader.cpp's constants (change both) --------------------------------
CACHE_MAGIC = 0x4B564C4C        # "LLVK", a .vkp file
GL_VIEW_MAGIC = 0x56474C4C      # "LLGV", a .glv file
BUNDLE_MAGIC = 0x42564C4C       # "LLVB"
BUNDLE_VERSION = 1
BUNDLE_HEADER = 96
BUNDLE_COMMIT = 40
BUNDLE_ENTRY = 24
BUNDLE_MODULE_ENTRY = 16
HEADER_FORMAT = '<IIIIIIIIQQQ'  # magic, version, cache, view, glslang, programs, views, modules, 3 tables
TRAILER_SIZE = 16
SPIRV_MAGIC = 0x07230203
ESH_LANG_COMPUTE = 5            # glslang's EShLangCompute: no stage number above it

# ---- XXH3-64 (seed 0, default secret), from xxhash 0.8.2's xxhash.h ----------
M64 = (1 << 64) - 1
P32_1, P32_2, P32_3 = 0x9E3779B1, 0x85EBCA77, 0xC2B2AE3D
P64_1, P64_2, P64_3 = 0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9
P64_4, P64_5 = 0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5
PRIME_MX1, PRIME_MX2 = 0x165667919E3779F9, 0x9FB21C651E98DF25
SECRET = bytes([
    0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c,
    0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f,
    0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
    0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c,
    0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3,
    0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
    0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d,
    0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64,
    0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
    0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e,
    0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce,
    0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
])
STRIPE = 64
STRIPES_PER_BLOCK = (len(SECRET) - STRIPE) // 8     # 16
BLOCK = STRIPE * STRIPES_PER_BLOCK                  # 1024
SWAP = [1, 0, 3, 2, 5, 4, 7, 6]                     # acc[lane ^ 1] += data[lane]
INIT_ACC = [P32_3, P64_1, P64_2, P64_3, P64_4, P32_2, P64_5, P32_1]


class Stop(Exception):
    """A file or an argument this tool refuses (the message says why)."""


def _r64(b, o):
    return int.from_bytes(b[o:o + 8], 'little')


def _r32(b, o):
    return int.from_bytes(b[o:o + 4], 'little')


def _fold(a, b):
    p = a * b
    return (p ^ (p >> 64)) & M64


def _xxh64_avalanche(h):
    h ^= h >> 33
    h = (h * P64_2) & M64
    h ^= h >> 29
    h = (h * P64_3) & M64
    return h ^ (h >> 32)


def _avalanche(h):
    h &= M64
    h ^= h >> 37
    h = (h * PRIME_MX1) & M64
    return h ^ (h >> 32)


def _rotl(v, r):
    return ((v << r) | (v >> (64 - r))) & M64


def _rrmxmx(h, length):
    h ^= _rotl(h, 49) ^ _rotl(h, 24)
    h = (h * PRIME_MX2) & M64
    h ^= ((h >> 35) + length) & M64
    h = (h * PRIME_MX2) & M64
    return h ^ (h >> 28)


def _mix16(b, o, so):
    return _fold(_r64(b, o) ^ _r64(SECRET, so), _r64(b, o + 8) ^ _r64(SECRET, so + 8))


def _short(b):
    n = len(b)
    if n > 8:
        lo = _r64(b, 0) ^ (_r64(SECRET, 24) ^ _r64(SECRET, 32))
        hi = _r64(b, n - 8) ^ (_r64(SECRET, 40) ^ _r64(SECRET, 48))
        swapped = int.from_bytes(lo.to_bytes(8, 'little'), 'big')
        return _avalanche(n + swapped + hi + _fold(lo, hi))
    if n >= 4:
        keyed = (_r32(b, n - 4) + (_r32(b, 0) << 32)) ^ (_r64(SECRET, 8) ^ _r64(SECRET, 16))
        return _rrmxmx(keyed, n)
    if n:
        combined = (b[0] << 16) | (b[n >> 1] << 24) | b[n - 1] | (n << 8)
        return _xxh64_avalanche(combined ^ (_r32(SECRET, 0) ^ _r32(SECRET, 4)))
    return _xxh64_avalanche(_r64(SECRET, 56) ^ _r64(SECRET, 64))


def _mid(b):
    n = len(b)
    acc = n * P64_1
    if n <= 128:
        if n > 32:
            if n > 64:
                if n > 96:
                    acc += _mix16(b, 48, 96) + _mix16(b, n - 64, 112)
                acc += _mix16(b, 32, 64) + _mix16(b, n - 48, 80)
            acc += _mix16(b, 16, 32) + _mix16(b, n - 32, 48)
        acc += _mix16(b, 0, 0) + _mix16(b, n - 16, 16)
        return _avalanche(acc)
    for i in range(8):
        acc += _mix16(b, 16 * i, 16 * i)
    acc_end = _mix16(b, n - 16, 136 - 17)
    acc = _avalanche(acc)
    for i in range(8, n // 16):
        acc_end += _mix16(b, 16 * i, 16 * (i - 8) + 3)
    return _avalanche(acc + acc_end)


def _merge(acc, n):
    result = (n * P64_1) & M64
    for i in range(4):
        result += _fold(acc[2 * i] ^ _r64(SECRET, 11 + 16 * i), acc[2 * i + 1] ^ _r64(SECRET, 11 + 16 * i + 8))
    return _avalanche(result)


def _long_python(b):
    n = len(b)
    acc = list(INIT_ACC)

    def stripe(o, so):
        for lane in range(8):
            data = _r64(b, o + lane * 8)
            key = data ^ _r64(SECRET, so + lane * 8)
            acc[lane ^ 1] = (acc[lane ^ 1] + data) & M64
            acc[lane] = (acc[lane] + (key & 0xFFFFFFFF) * (key >> 32)) & M64

    blocks = (n - 1) // BLOCK
    for blk in range(blocks):
        for s in range(STRIPES_PER_BLOCK):
            stripe(blk * BLOCK + s * STRIPE, s * 8)
        for lane in range(8):
            a = acc[lane]
            a ^= a >> 47
            a ^= _r64(SECRET, len(SECRET) - STRIPE + lane * 8)
            acc[lane] = (a * P32_1) & M64
    for s in range(((n - 1) - BLOCK * blocks) // STRIPE):
        stripe(blocks * BLOCK + s * STRIPE, s * 8)
    stripe(n - STRIPE, len(SECRET) - STRIPE - 7)
    return _merge(acc, n)


def _long_numpy(b):
    n = len(b)
    u64 = np.uint64
    keys = np.stack([np.frombuffer(SECRET, dtype='<u8', count=8, offset=s * 8) for s in range(STRIPES_PER_BLOCK)])
    scramble = np.frombuffer(SECRET, dtype='<u8', count=8, offset=len(SECRET) - STRIPE)
    low = u64(0xFFFFFFFF)
    acc = np.array(INIT_ACC, dtype=np.uint64)

    def contribution(d, k):     # d, k: (..., stripes, 8) -> what the stripes add to each lane
        dk = d ^ k
        prod = (dk & low) * (dk >> u64(32))
        return prod.sum(axis=-2, dtype=np.uint64) + d.sum(axis=-2, dtype=np.uint64)[..., SWAP]

    blocks = (n - 1) // BLOCK
    data = np.frombuffer(b, dtype='<u8', count=blocks * (BLOCK // 8)).reshape(blocks, STRIPES_PER_BLOCK, 8)
    chunk = 8192
    with np.errstate(over='ignore'):
        for first in range(0, blocks, chunk):
            for add in contribution(data[first:first + chunk], keys):
                acc += add
                acc ^= acc >> u64(47)
                acc ^= scramble
                acc *= u64(P32_1)
        stripes = ((n - 1) - BLOCK * blocks) // STRIPE
        if stripes:
            d = np.frombuffer(b, dtype='<u8', count=stripes * 8, offset=blocks * BLOCK).reshape(stripes, 8)
            acc += contribution(d, keys[:stripes])
        last = np.frombuffer(b, dtype='<u8', count=8, offset=n - STRIPE).reshape(1, 8)
        key = np.frombuffer(SECRET[len(SECRET) - STRIPE - 7:len(SECRET) - 7], dtype='<u8').reshape(1, 8)
        acc += contribution(last, key)
    return _merge([int(v) for v in acc], n)


def xxh3_64(b):
    """XXH3_64bits(b), as the viewer's HBXXH64::digest."""
    b = bytes(b)
    n = len(b)
    if n <= 16:
        return _short(b)
    if n <= 240:
        return _mid(b)
    return _long_numpy(b) if np is not None else _long_python(b)


# ---- the cache files ---------------------------------------------------------

def strip_trailer(data):
    """The contents without the trailer (u64 size, u64 XXH3-64); None when it does not match."""
    if len(data) < TRAILER_SIZE:
        return None
    body = len(data) - TRAILER_SIZE
    size, digest = struct.unpack_from('<QQ', data, body)
    if size != body or digest != xxh3_64(memoryview(data)[:body]):
        return None
    return data[:body]


def whole_module(spirv):
    """As llvkshader.cpp's whole_module: the magic, and instruction lengths that end at the end."""
    words = len(spirv) // 4
    if words < 5 or struct.unpack_from('<I', spirv, 0)[0] != SPIRV_MAGIC:
        return False
    i = 5
    while i < words:
        count = struct.unpack_from('<I', spirv, i * 4)[0] >> 16
        if not count or i + count > words:
            return False
        i += count
    return True


def entry_kind(name):
    """'vkp' or 'glv' for a cache file's name (16 lower-case hex digits and the extension); else None."""
    stem, dot, ext = name.partition('.')
    if ext in ('vkp', 'glv') and len(stem) == 16 and all(c in '0123456789abcdef' for c in stem):
        return ext
    return None


def read_vkp(body):
    """(version, [(stage, SPIR-V bytes)], reflection bytes); None when malformed."""
    if len(body) < 12:
        return None
    magic, version, count = struct.unpack_from('<III', body, 0)
    if magic != CACHE_MAGIC:
        return None
    pos = 12
    modules = []
    for _ in range(count):
        if pos + 8 > len(body):
            return None
        lang, words = struct.unpack_from('<II', body, pos)
        pos += 8
        if lang > ESH_LANG_COMPUTE or words < 5 or pos + words * 4 > len(body):
            return None
        modules.append((lang, bytes(body[pos:pos + words * 4])))
        pos += words * 4
    if not modules:
        return None
    return version, modules, bytes(body[pos:])


def read_glv(body):
    """(version, glslang version); None when malformed."""
    if len(body) < 12:
        return None
    magic, version, glslang = struct.unpack_from('<III', body, 0)
    if magic != GL_VIEW_MAGIC:
        return None
    return version, glslang


def only_value(values, what):
    """The one value of values; Stop when there are none or several (the folders mix builds)."""
    found = collections.Counter(values)
    if len(found) != 1:
        raise Stop('%s: %s in the cache folders (one exe must make them all)'
                   % (what, dict(found) if found else 'none'))
    return next(iter(found))


def load_caches(dirs):
    """Reads the cache folders: (programs {key: (modules, reflection)}, views {key: body},
    (cache version, view version, glslang), {why: count} of the files left out)."""
    vkp, glv = {}, {}
    bad = collections.Counter()
    for folder in dirs:
        if not os.path.isdir(folder):
            raise Stop('no folder %s' % folder)
        for name in sorted(os.listdir(folder)):
            kind = entry_kind(name)
            if kind is None:
                continue
            key = int(name[:16], 16)
            if key in (vkp if kind == 'vkp' else glv):
                continue    # the earlier folder's
            try:
                with open(os.path.join(folder, name), 'rb') as f:
                    data = f.read()
            except OSError:
                bad[kind + ': cannot be read'] += 1
                continue
            body = strip_trailer(data)
            parsed = None if body is None else (read_vkp(body) if kind == 'vkp' else read_glv(body))
            if parsed is None:
                bad[kind + (': trailer does not match' if body is None else ': malformed')] += 1
            elif kind == 'vkp':
                vkp[key] = parsed
            else:
                glv[key] = (parsed[0], parsed[1], bytes(body))
    if not vkp or not glv:
        raise Stop('no usable %s file in %s' % ('.vkp' if not vkp else '.glv', ', '.join(dirs)))
    versions = (only_value((v[0] for v in vkp.values()), 'CACHE_VERSION'),
                only_value((v[0] for v in glv.values()), 'GL_VIEW_VERSION'),
                only_value((v[1] for v in glv.values()), 'glslang version'))
    programs = {k: (v[1], v[2]) for k, v in vkp.items()}
    views = {k: v[2] for k, v in glv.items()}
    return programs, views, versions, bad


# ---- writing -----------------------------------------------------------------

def align(out, boundary):
    out.extend(b'\0' * (-len(out) % boundary))


def build(programs, views, versions, commit):
    """The bundle's bytes, and the number of programs left out for a cut SPIR-V module."""
    modules = {}            # SPIR-V bytes -> index
    module_list = []
    bodies = {}             # key -> the program's bytes in the bundle
    cut = 0
    for key, (mods, reflection) in programs.items():
        refs = []
        for lang, spirv in mods:
            index = modules.get(spirv)
            if index is None:
                if not whole_module(spirv):
                    refs = None
                    break
                index = modules[spirv] = len(module_list)
                module_list.append(spirv)
            refs.append((lang, index))
        if refs is None:
            cut += 1
            continue
        bodies[key] = struct.pack('<I', len(refs)) + b''.join(struct.pack('<II', l, i) for l, i in refs) + reflection
    if not bodies:
        raise Stop('no program to write')

    program_keys = sorted(bodies)
    view_keys = sorted(views)
    program_table = BUNDLE_HEADER
    view_table = program_table + BUNDLE_ENTRY * len(program_keys)
    module_table = view_table + BUNDLE_ENTRY * len(view_keys)
    out = bytearray(module_table + BUNDLE_MODULE_ENTRY * len(module_list))

    def place(items):       # equal contents once
        offsets, placed = {}, {}
        for key, body in items:
            at = placed.get(body)
            if at is None:
                align(out, 4)
                at = placed[body] = len(out)
                out.extend(body)
            offsets[key] = (at, len(body))
        return offsets

    program_at = place((k, bodies[k]) for k in program_keys)
    view_at = place((k, views[k]) for k in view_keys)
    module_at = []
    for spirv in module_list:
        align(out, 4)
        module_at.append(len(out))
        out.extend(spirv)
    align(out, 4)

    struct.pack_into(HEADER_FORMAT, out, 0, BUNDLE_MAGIC, BUNDLE_VERSION, *versions,
                     len(program_keys), len(view_keys), len(module_list), program_table, view_table, module_table)
    out[56:56 + BUNDLE_COMMIT] = commit.encode('ascii', 'replace')[:BUNDLE_COMMIT].ljust(BUNDLE_COMMIT, b'\0')
    for i, key in enumerate(program_keys):
        struct.pack_into('<QQII', out, program_table + i * BUNDLE_ENTRY, key, *program_at[key], 0)
    for i, key in enumerate(view_keys):
        struct.pack_into('<QQII', out, view_table + i * BUNDLE_ENTRY, key, *view_at[key], 0)
    for i, spirv in enumerate(module_list):
        struct.pack_into('<QII', out, module_table + i * BUNDLE_MODULE_ENTRY, module_at[i], len(spirv) // 4, 0)
    body = len(out)
    out.extend(struct.pack('<QQ', body, xxh3_64(out)))
    return bytes(out), cut


def pack(dirs, output, commit):
    """Packs the cache folders into output (written to a temporary file, then put in place).
    Returns a summary dict."""
    if np is None:
        print('shaderbundle: no numpy, XXH3 in pure Python (slower)', flush=True)
    programs, views, versions, bad = load_caches(dirs)
    data, cut = build(programs, views, versions, commit)
    if cut:
        bad['vkp: a SPIR-V module is cut short'] += cut
    folder = os.path.dirname(os.path.abspath(output))
    os.makedirs(folder, exist_ok=True)
    temp = output + '.tmp'
    try:
        with open(temp, 'wb') as f:
            f.write(data)
        os.replace(temp, output)
    finally:
        if os.path.exists(temp):
            os.remove(temp)
    return {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
            'programs': len(programs) - cut, 'views': len(views), 'left_out': dict(bad)}


# ---- reading -----------------------------------------------------------------

def _load(path):
    """(header {name: value}, body, program entries, view entries, module entries) of a bundle,
    each entry a tuple of its table row (key, offset, size, reserved) or (offset, words, reserved).
    Stop when it is not a bundle, is damaged or a table lies outside it."""
    with open(path, 'rb') as f:
        data = f.read()
    body = strip_trailer(data)
    if body is None or len(body) < BUNDLE_HEADER:
        raise Stop('%s: not a shader bundle, or damaged (the trailer does not match)' % path)
    fields = struct.unpack_from(HEADER_FORMAT, body, 0)
    if fields[0] != BUNDLE_MAGIC:
        raise Stop('%s: not a shader bundle (magic)' % path)
    header = dict(zip(('magic', 'version', 'cache_version', 'view_version', 'glslang', 'programs', 'views',
                       'modules', 'program_table', 'view_table', 'module_table'), fields))
    header['commit'] = body[56:56 + BUNDLE_COMMIT].rstrip(b'\0').decode('ascii', 'replace')
    header['bytes'] = len(data)
    header['sha256'] = hashlib.sha256(data).hexdigest()

    def table(at, count, size):
        if at < BUNDLE_HEADER or at + count * size > len(body):
            raise Stop('%s: damaged (a table lies outside it)' % path)
        return [struct.unpack_from('<QQII' if size == BUNDLE_ENTRY else '<QII', body, at + i * size)
                for i in range(count)]

    return (header, body, table(header['program_table'], header['programs'], BUNDLE_ENTRY),
            table(header['view_table'], header['views'], BUNDLE_ENTRY),
            table(header['module_table'], header['modules'], BUNDLE_MODULE_ENTRY))


def _assemble(path, body, program_rows, view_rows, module_rows):
    """(programs {key: (modules [(stage, SPIR-V)], reflection)}, views {key: body}) from _load's rows."""
    def chunk(at, size):
        if at + size > len(body):
            raise Stop('%s: damaged (an entry lies outside it)' % path)
        return bytes(body[at:at + size])

    module_list = [chunk(at, words * 4) for at, words, _ in module_rows]
    programs = {}
    for key, at, size, _ in program_rows:
        entry = chunk(at, size)
        if len(entry) < 4:
            raise Stop('%s: damaged (program %016x)' % (path, key))
        count, = struct.unpack_from('<I', entry, 0)
        if 4 + count * 8 > len(entry):
            raise Stop('%s: damaged (program %016x)' % (path, key))
        mods = []
        for m in range(count):
            lang, index = struct.unpack_from('<II', entry, 4 + m * 8)
            if index >= len(module_list):
                raise Stop('%s: damaged (program %016x names module %d)' % (path, key, index))
            mods.append((lang, module_list[index]))
        programs[key] = (mods, entry[4 + count * 8:])
    views = {key: chunk(at, size) for key, at, size, _ in view_rows}
    return programs, views


def read_bundle(path):
    """(header {name: value}, programs {key: (modules [(stage, SPIR-V)], reflection)},
    views {key: body}). Stop when it is not a bundle, is damaged or a table points outside it."""
    header, body, program_rows, view_rows, module_rows = _load(path)
    return (header,) + _assemble(path, body, program_rows, view_rows, module_rows)


def describe(header):
    """Two lines about a bundle (info and the bake's log)."""
    return ('%d bytes, sha256 %s\n'
            'version %d, CACHE_VERSION %d, GL_VIEW_VERSION %d, glslang %d, commit %s; %d programs, %d GL views, %d modules'
            % (header['bytes'], header['sha256'], header['version'], header['cache_version'], header['view_version'],
               header['glslang'], header['commit'] or '?', header['programs'], header['views'], header['modules']))


# ---- the manifest ------------------------------------------------------------

MANIFEST_HEADER = '# shader bundle manifest 1: kind<TAB>key<TAB>sha256 of what the key stands for'


def program_digest(mods, reflection):
    h = hashlib.sha256()
    for lang, spirv in mods:
        h.update(struct.pack('<II', lang, len(spirv)))
        h.update(spirv)
    h.update(reflection)
    return h.hexdigest()


def manifest_lines(header, programs, views):
    lines = [MANIFEST_HEADER, '# ' + describe(header).replace('\n', '\n# ')]
    lines += ['program\t%016x\t%s' % (k, program_digest(*programs[k])) for k in sorted(programs)]
    lines += ['view\t%016x\t%s' % (k, hashlib.sha256(views[k]).hexdigest()) for k in sorted(views)]
    return lines


def read_manifest(path):
    """{(kind, key): digest} of a manifest file."""
    entries = {}
    with open(path, encoding='utf-8') as f:
        first = f.readline().rstrip('\r\n')
        if first != MANIFEST_HEADER:
            raise Stop('%s: not a shader bundle manifest' % path)
        for number, line in enumerate(f, 2):
            line = line.rstrip('\r\n')
            if not line or line.startswith('#'):
                continue
            cols = line.split('\t')
            if len(cols) != 3 or cols[0] not in ('program', 'view'):
                raise Stop('%s:%d: not "kind<TAB>key<TAB>sha256"' % (path, number))
            entries[(cols[0], cols[1])] = cols[2]
    return entries


def compare_manifests(new_path, baseline_path):
    """(ok, {count name: count}, ["kind key" of the keys whose contents differ])."""
    new, base = read_manifest(new_path), read_manifest(baseline_path)
    counts = collections.Counter()
    differ = []
    for item, digest in base.items():
        if item not in new:
            counts[item[0] + ' only in baseline'] += 1
        elif new[item] == digest:
            counts[item[0] + ' same'] += 1
        else:
            counts[item[0] + ' differ'] += 1
            differ.append('%s %s' % item)
    for item in new:
        if item not in base:
            counts[item[0] + ' only in new'] += 1
    ok = not differ and not any(n.endswith('only in baseline') for n in counts)
    return ok, dict(counts), differ


# ---- checking a bundle made elsewhere ------------------------------------------

# Bounds for check: the release bundle is about 250 MB with tens of thousands of keys
# (research/312); one far above them is not a bundle of this viewer.
BUNDLE_MAX_BYTES = 512 << 20
BUNDLE_MAX_ENTRIES = 1 << 20        # in each table
MANIFEST_MAX_BYTES = 64 << 20
MAX_PROGRAM_MODULES = ESH_LANG_COMPUTE + 1      # llvkshader.cpp's

# What glslang (the Vulkan SDK's, through the viewer) puts in the modules of this viewer's
# bundles, and nothing else: an allow list against a bake that hands over modules made by
# something else. Taken from the bundles of 2026-11-08 (research/312: a bake on an RTX 2080
# and one on lavapipe, the same sets). A new shader that needs another capability or
# extension stops check: look at it, then add it here.
SPIRV_MAX_VERSION = 0x10600                     # SPIR-V 1.6
SPIRV_GENERATOR_GLSLANG = 8                     # the upper 16 bits of the generator word
SPIRV_CAPABILITIES = frozenset((1, 2, 45, 50, 51, 61, 68, 5301, 5302, 5307, 5347, 5379))
SPIRV_EXTENSIONS = frozenset(('SPV_EXT_demote_to_helper_invocation', 'SPV_KHR_terminate_invocation'))
SPIRV_IMPORTS = frozenset(('GLSL.std.450',))
SPIRV_ADDRESSING_LOGICAL, SPIRV_ADDRESSING_PSB64 = 0, 5348
SPIRV_MEMORY_MODEL_GLSL450 = 1
SPIRV_CAPABILITY_PSB = 5347                     # PhysicalStorageBufferAddresses
# Modules that address physical storage buffers (buffer device addresses): 4 in those bundles
# (the same in both); more is not this viewer's.
PHYSICAL_STORAGE_BUFFER_MAX_MODULES = 8
OP_EXTENSION, OP_EXT_INST_IMPORT, OP_MEMORY_MODEL, OP_CAPABILITY = 10, 11, 14, 17

# The reflection and GL view (llvkshader.cpp read_reflection, parse_gl_view): bounds well above
# those bundles' (128 uniforms, 3 blocks, 8 inputs, names of up to 36 characters, empty logs).
REFLECTION_MAX_ITEMS = 1024
NAME_RE = re.compile(rb'[A-Za-z0-9_.\[\]]{0,256}')
GL_VIEW_LOG_MAX = 4096
LOG_RE = re.compile(rb'[\t\n\r\x20-\x7e]*')
GL_TYPE_MAX = 0xFFFF
STAGE_MASK_MAX = (1 << (ESH_LANG_COMPUTE + 1)) - 1


def _spirv_string(words, first, last):
    """The literal string in words[first:last] (UTF-8, ended by a 0 byte inside them); None if not."""
    raw = struct.pack('<%dI' % (last - first), *words[first:last])
    end = raw.find(b'\0')
    if end < 0:
        return None
    try:
        return raw[:end].decode('utf-8')
    except UnicodeDecodeError:
        return None


def spirv_problem(spirv):
    """(why the module is not one glslang makes for this viewer, or None; whether it addresses
    physical storage buffers). Walks every instruction: whole (whole_module), the header (version,
    generator), one OpMemoryModel (Logical or PhysicalStorageBuffer64, GLSL450), and only the
    allowed OpCapability, OpExtension and OpExtInstImport."""
    if not whole_module(spirv):
        return 'not whole (the magic, or an instruction length)', False
    words = struct.unpack('<%dI' % (len(spirv) // 4), spirv)
    if not 0x10000 <= words[1] <= SPIRV_MAX_VERSION or words[1] & 0xFF0000FF:
        return 'SPIR-V version %#x' % words[1], False
    if words[2] >> 16 != SPIRV_GENERATOR_GLSLANG:
        return 'made by generator %d, not glslang (%d)' % (words[2] >> 16, SPIRV_GENERATOR_GLSLANG), False
    if words[4] != 0:
        return 'schema %d' % words[4], False
    models, psb, i = [], False, 5
    while i < len(words):
        op, count = words[i] & 0xFFFF, words[i] >> 16
        if op == OP_CAPABILITY:
            if count != 2 or words[i + 1] not in SPIRV_CAPABILITIES:
                return 'capability %s is not on the allow list' % (words[i + 1] if count == 2 else '?'), False
            psb = psb or words[i + 1] == SPIRV_CAPABILITY_PSB
        elif op == OP_EXTENSION:
            name = _spirv_string(words, i + 1, i + count)
            if name not in SPIRV_EXTENSIONS:
                return 'extension %r is not on the allow list' % name, False
        elif op == OP_EXT_INST_IMPORT:
            name = _spirv_string(words, i + 2, i + count) if count > 2 else None
            if name not in SPIRV_IMPORTS:
                return 'instruction set %r is not on the allow list' % name, False
        elif op == OP_MEMORY_MODEL:
            if count != 3:
                return 'an OpMemoryModel of %d words' % count, False
            models.append((words[i + 1], words[i + 2]))
        i += count
    if len(models) != 1:
        return '%d OpMemoryModel instructions' % len(models), False
    addressing, memory = models[0]
    if memory != SPIRV_MEMORY_MODEL_GLSL450 or addressing not in (SPIRV_ADDRESSING_LOGICAL, SPIRV_ADDRESSING_PSB64):
        return 'memory model %d with addressing %d (only GLSL450, Logical or PhysicalStorageBuffer64)' \
            % (memory, addressing), False
    if (addressing == SPIRV_ADDRESSING_PSB64) != psb:
        return 'PhysicalStorageBuffer64 addressing and the capability do not go together', False
    return None, psb


class _Reader:
    """llvkshader.cpp's Reader over bytes: u32 and length-prefixed strings; ok turns False when
    something is cut short."""

    def __init__(self, data, pos=0):
        self.data, self.pos, self.ok = data, pos, True

    def u32(self):
        if self.pos + 4 > len(self.data):
            self.ok = False
            return 0
        value = struct.unpack_from('<I', self.data, self.pos)[0]
        self.pos += 4
        return value

    def str(self, pattern=NAME_RE):
        n = self.u32()
        if not self.ok or n > len(self.data) - self.pos:
            self.ok = False
            return b''
        text = self.data[self.pos:self.pos + n]
        self.pos += n
        if not pattern.fullmatch(text):
            self.ok = False
        return text

    def items(self, read_one):
        count = self.u32()
        if count > REFLECTION_MAX_ITEMS:
            self.ok = False
            return
        for _ in range(count):
            if not self.ok:
                return
            read_one()


def reflection_problem(data):
    """Why a program's reflection is not what read_reflection reads (to its end, every name plain,
    counts, stage masks and GL types in bounds), or None."""
    r = _Reader(data)

    def uniform():
        r.str()
        gl_type = r.u32()
        for _ in range(5):          # offset, binding, size, array stride, block (signed)
            r.u32()
        stages = r.u32()
        if gl_type > GL_TYPE_MAX or stages > STAGE_MASK_MAX:
            r.ok = False

    def block():
        r.str()
        r.u32()
        r.u32()

    def attribute():
        r.str()
        r.u32()
        if r.u32() > GL_TYPE_MAX:
            r.ok = False

    for read_one in (uniform, block, attribute):
        r.items(read_one)
    if not r.ok or r.pos != len(data):
        return 'is cut short, has bytes left over, or a name, count or value out of bounds'
    return None


def gl_view_problem(data, view_version, glslang):
    """Why a GL view is not what parse_gl_view reads with the header's versions, or None."""
    r = _Reader(data)
    if r.u32() != GL_VIEW_MAGIC or r.u32() != view_version or r.u32() != glslang:
        return 'not a GL view of this bundle\'s GL_VIEW_VERSION and glslang'
    if r.u32() > 1:
        r.ok = False
    log = r.str(LOG_RE)
    if len(log) > GL_VIEW_LOG_MAX:
        r.ok = False

    def uniform():
        r.str()
        if r.u32() > GL_TYPE_MAX:
            r.ok = False
        r.u32()
        if r.u32() > 1:
            r.ok = False

    for read_one in (uniform, r.str, r.str):
        r.items(read_one)
    if not r.ok or r.pos != len(data):
        return 'is cut short, has bytes left over, or a name, count or value out of bounds'
    return None


def _row_problems(kind, rows, data_start, body_size, keyed):
    """What is wrong with a table's rows: an entry outside the data after the tables, not 4-byte
    aligned, empty, a reserved field that is not 0, or (keyed) keys not strictly ascending."""
    problems, last = [], None
    for row in rows:
        if keyed:
            key, at, length, reserved = row
            name = '%s %016x' % (kind, key)
            if last is not None and key <= last:
                problems.append('%s: keys not strictly ascending (a key twice, or out of order)' % name)
            last = key
        else:
            at, words, reserved = row
            length, name = words * 4, '%s at %d' % (kind, at)
        if reserved != 0:
            problems.append('%s: reserved field %d' % (name, reserved))
        if not length or at < data_start or at % 4 or at + length > body_size:
            problems.append('%s: %d bytes at %d, not inside the data (%d to %d, 4-byte aligned)'
                            % (name, length, at, data_start, body_size))
        if len(problems) >= 5:
            break
    return problems


def check_bundle(path, commit, manifest=None, max_bytes=BUNDLE_MAX_BYTES):
    """Reads a bundle made by another job (the release workflow's bake job, which runs Mesa) back
    and checks its form, without running anything in it: the size (before reading it), the
    trailer, the header (magic, BUNDLE_VERSION, the commit), the counts (1 to BUNDLE_MAX_ENTRIES),
    the layout (the three tables one after the other right after the header, every entry inside
    the data after them, 4-byte aligned, reserved fields 0), keys strictly ascending in each table,
    every SPIR-V module whole and within the allow list (spirv_problem), every program with 1 to
    MAX_PROGRAM_MODULES modules of known stages and a reflection read_reflection reads
    (reflection_problem), every GL view one parse_gl_view reads with the header's versions
    (gl_view_problem), and, with manifest, exactly the keys and contents that manifest lists.
    The manifest comes from the same bake, so it only shows that what arrived is what the bake
    listed. Returns the header; Stop with the first reason found."""
    if os.path.islink(path) or not os.path.isfile(path):
        raise Stop('%s: not a regular file' % path)
    size = os.path.getsize(path)
    if size > max_bytes:
        raise Stop('%s: %d bytes, more than the %d a bundle may have' % (path, size, max_bytes))
    if not re.fullmatch(r'[0-9a-f]{40}', commit or ''):
        raise Stop('the commit to check against is not a full commit id: %r' % commit)
    header, body, program_rows, view_rows, module_rows = _load(path)
    problems = []
    if header['version'] != BUNDLE_VERSION:
        problems.append('bundle version %d, not %d' % (header['version'], BUNDLE_VERSION))
    if header['commit'] != commit:
        problems.append('made for commit %r, not %s' % (header['commit'], commit))
    for name in ('programs', 'views', 'modules'):
        if not 1 <= header[name] <= BUNDLE_MAX_ENTRIES:
            problems.append('%d %s (1 to %d)' % (header[name], name, BUNDLE_MAX_ENTRIES))
    if (header['program_table'] != BUNDLE_HEADER
            or header['view_table'] != header['program_table'] + BUNDLE_ENTRY * header['programs']
            or header['module_table'] != header['view_table'] + BUNDLE_ENTRY * header['views']):
        problems.append('the tables are not one after the other right after the header')
    if problems:
        raise Stop('%s: %s' % (path, '; '.join(problems)))
    data_start = header['module_table'] + BUNDLE_MODULE_ENTRY * header['modules']
    problems += _row_problems('program', program_rows, data_start, len(body), True)
    problems += _row_problems('view', view_rows, data_start, len(body), True)
    problems += _row_problems('module', module_rows, data_start, len(body), False)
    if problems:
        raise Stop('%s: %s' % (path, '; '.join(problems[:5])))
    psb = 0
    for at, words, _ in module_rows:
        why, uses_psb = spirv_problem(body[at:at + words * 4])
        if why:
            raise Stop('%s: the SPIR-V module at %d: %s' % (path, at, why))
        psb += uses_psb
    if psb > PHYSICAL_STORAGE_BUFFER_MAX_MODULES:
        raise Stop('%s: %d modules address physical storage buffers (at most %d)'
                   % (path, psb, PHYSICAL_STORAGE_BUFFER_MAX_MODULES))
    programs, views = _assemble(path, body, program_rows, view_rows, module_rows)
    for key, (mods, reflection) in programs.items():
        if not 1 <= len(mods) <= MAX_PROGRAM_MODULES or any(lang > ESH_LANG_COMPUTE for lang, _ in mods):
            raise Stop('%s: program %016x: %d modules or an unknown stage' % (path, key, len(mods)))
        why = reflection_problem(reflection)
        if why:
            raise Stop('%s: program %016x: its reflection %s' % (path, key, why))
    for key, view in views.items():
        why = gl_view_problem(view, header['view_version'], header['glslang'])
        if why:
            raise Stop('%s: GL view %016x: %s' % (path, key, why))
    if manifest is not None:
        if os.path.getsize(manifest) > MANIFEST_MAX_BYTES:
            raise Stop('%s: larger than %d bytes' % (manifest, MANIFEST_MAX_BYTES))
        listed = read_manifest(manifest)
        held = {('program', '%016x' % k): program_digest(*v) for k, v in programs.items()}
        held.update({('view', '%016x' % k): hashlib.sha256(v).hexdigest() for k, v in views.items()})
        missing = sorted(set(listed) - set(held))
        extra = sorted(set(held) - set(listed))
        differ = sorted(k for k in set(listed) & set(held) if listed[k] != held[k])
        if missing or extra or differ:
            first = (missing or extra or differ)[0]
            raise Stop('%s is not the manifest of %s: %d keys only in the manifest, %d only in the bundle, '
                       '%d with other contents (first: %s %s)' % (manifest, path, len(missing), len(extra),
                                                                len(differ), first[0], first[1]))
    return header


# ---- command line --------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description='Make, check and record a shader bundle (shaders.vkb).')
    sub = ap.add_subparsers(dest='command', required=True)
    p = sub.add_parser('pack', help='pack cache folders into a bundle')
    p.add_argument('dirs', nargs='+')
    p.add_argument('-o', '--output', required=True)
    p.add_argument('--commit', required=True, help="the viewer's commit, for the header")
    p = sub.add_parser('info', help="a bundle's header and counts; exit 1 when damaged")
    p.add_argument('bundle')
    p = sub.add_parser('manifest', help='one line a key with the SHA-256 of its contents')
    p.add_argument('bundle')
    p.add_argument('-o', '--output', help='write it here (default: print it)')
    p = sub.add_parser('compare-manifest', help='exit 1 when NEW differs from BASELINE or lacks one of its keys')
    p.add_argument('new')
    p.add_argument('baseline')
    p = sub.add_parser('check', help='read a bundle made elsewhere back (nothing in it is run); exit 1 when off')
    p.add_argument('bundle')
    p.add_argument('--commit', required=True, help='the commit the bundle must be made for')
    p.add_argument('--manifest', help='the manifest that must list exactly its keys and contents')
    p.add_argument('--max-bytes', type=int, default=BUNDLE_MAX_BYTES)
    a = ap.parse_args()
    try:
        if a.command == 'pack':
            s = pack(a.dirs, a.output, a.commit)
            print('%s: %d bytes, sha256 %s, %d programs, %d GL views' % (a.output, s['bytes'], s['sha256'],
                                                                         s['programs'], s['views']))
            for why, count in sorted(s['left_out'].items()):
                print('  left out: %s, %d' % (why, count))
        elif a.command == 'info':
            print('%s: %s' % (a.bundle, describe(read_bundle(a.bundle)[0])))
        elif a.command == 'manifest':
            text = '\n'.join(manifest_lines(*read_bundle(a.bundle))) + '\n'
            if a.output:
                with open(a.output, 'w', encoding='utf-8', newline='\n') as f:
                    f.write(text)
            else:
                sys.stdout.write(text)
        elif a.command == 'check':
            header = check_bundle(a.bundle, a.commit, a.manifest, a.max_bytes)
            print('%s: %s' % (a.bundle, describe(header)))
            print('OK: a well-formed bundle for %s%s' % (a.commit, ', as its manifest lists' if a.manifest else ''))
        else:
            ok, counts, differ = compare_manifests(a.new, a.baseline)
            print(', '.join('%s %d' % kv for kv in sorted(counts.items())))
            for item in differ[:20]:
                print('  differs: ' + item)
            print('OK: every baseline key is there with the same contents' if ok else
                  'NG: a key differs, or the baseline has keys the new bundle lacks')
            return 0 if ok else 1
    except (Stop, OSError) as e:
        print('shaderbundle: %s' % e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
