#!/usr/bin/env python3
"""Builds synthetic PSD/PSB fixtures and checks psdthumb.exe against expected colours.

usage: python tests/run_tests.py [path/to/psdthumb.exe]
"""
import os
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "psdthumb.exe")
OUT = os.path.join(ROOT, "tests", "out")

# 24x16 solid (255,128,0) baseline JPEG, used as an embedded Photoshop thumbnail.
JPEG_ORANGE = bytes.fromhex(
    "ffd8ffe000104a46494600010101006000600000ffdb0043000201010201010202020202020202030503030303030604040305070607070706"
    "070708090b0908080a0807070a0d0a0a0b0c0c0c0c07090e0f0d0c0e0b0c0c0cffdb004301020202030303060303060c0807080c0c0c0c0c0c"
    "0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0cffc00011080010001803012200"
    "021101031101ffc4001f0000010501010101010100000000000000000102030405060708090a0bffc400b5100002010303020403050504040000"
    "017d01020300041105122131410613516107227114328191a1082342b1c11552d1f02433627282090a161718191a25262728292a343536373839"
    "3a434445464748494a535455565758595a636465666768696a737475767778797a838485868788898a92939495969798999aa2a3a4a5a6a7a8a9"
    "aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9faffc4001f01000301010101"
    "01010101010000000000000102030405060708090a0bffc400b51100020102040403040705040400010277000102031104052131061241510761"
    "711322328108144291a1b1c109233352f0156272d10a162434e125f11718191a262728292a35363738393a434445464748494a53545556575859"
    "5a636465666768696a737475767778797a82838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6"
    "c7c8c9cad2d3d4d5d6d7d8d9dae2e3e4e5e6e7e8e9eaf2f3f4f5f6f7f8f9faffda000c03010002110311003f00f5ca28a2bfcab3fd280a28a280"
    "3fffd9"
)

W, H = 64, 48


def packbits(row: bytes) -> bytes:
    out = bytearray()
    i, n = 0, len(row)
    while i < n:
        j = i
        while j + 1 < n and row[j + 1] == row[i] and j - i < 126:
            j += 1
        run = j - i + 1
        if run >= 2:
            out.append((257 - run) & 0xFF)
            out.append(row[i])
            i = j + 1
        else:
            k = i
            while k < n and k - i < 128:
                if k + 2 < n and row[k] == row[k + 1] == row[k + 2]:
                    break
                k += 1
            out.append(k - i - 1)
            out += row[i:k]
            i = k
    return bytes(out)


def sample(depth: int, v) -> bytes:
    if depth == 8:
        return struct.pack(">B", v)
    if depth == 16:
        return struct.pack(">H", v * 257)
    if depth == 32:
        return struct.pack(">f", v)
    raise ValueError(depth)


def plane(w: int, h: int, depth: int, left, right) -> bytes:
    """One channel, row-major: left half = `left`, right half = `right`."""
    if depth == 1:
        rowbytes = (w + 7) // 8
        row = bytearray(rowbytes)
        for x in range(w):
            if left if x < w // 2 else right:
                row[x >> 3] |= 0x80 >> (x & 7)
        return bytes(row) * h
    row = sample(depth, left) * (w // 2) + sample(depth, right) * (w - w // 2)
    return row * h


def resource(rid: int, data: bytes) -> bytes:
    blk = b"8BIM" + struct.pack(">H", rid) + b"\x00\x00" + struct.pack(">I", len(data)) + data
    return blk + (b"\x00" if len(data) & 1 else b"")


def build(w, h, depth, mode, planes, compression=0, psb=False, palette=b"", layer_count=None,
          lr_count=None, resources=b"", truncate=None) -> bytes:
    L = ">Q" if psb else ">I"
    out = b"8BPS" + struct.pack(">H", 2 if psb else 1) + b"\x00" * 6
    out += struct.pack(">HIIHH", len(planes), h, w, depth, mode)
    out += struct.pack(">I", len(palette)) + palette
    out += struct.pack(">I", len(resources)) + resources
    if layer_count is not None:
        info = struct.pack(">h", layer_count)
        lm = struct.pack(L, len(info)) + info
        out += struct.pack(L, len(lm)) + lm
    elif lr_count is not None:
        # 16/32-bit layout: empty layer info, empty global mask, then an Lr16/Lr32 block.
        inner = struct.pack(">h", lr_count)
        data = struct.pack(L, len(inner)) + inner
        key = b"Lr16" if depth == 16 else b"Lr32"
        blk = b"8BIM" + key + struct.pack(L, len(data)) + data
        lm = struct.pack(L, 0) + struct.pack(">I", 0) + blk
        out += struct.pack(L, len(lm)) + lm
    else:
        out += struct.pack(L, 0)
    rowbytes = (w + 7) // 8 if depth == 1 else w * (depth // 8)
    out += struct.pack(">H", compression)
    if compression == 0:
        out += b"".join(planes)
    elif compression == 1:
        C = ">I" if psb else ">H"
        counts, data = bytearray(), bytearray()
        for p in planes:
            for y in range(h):
                enc = packbits(p[y * rowbytes:(y + 1) * rowbytes])
                counts += struct.pack(C, len(enc))
                data += enc
        out += bytes(counts) + bytes(data)
    else:
        out += b"\x00" * 16
    return out[:truncate] if truncate else out


def rgb(depth, left, right):
    return [plane(W, H, depth, left[i], right[i]) for i in range(3)]


RED, BLUE, WHITE, BLACK = (255, 0, 0), (0, 0, 255), (255, 255, 255), (0, 0, 0)
IDX_PALETTE = (bytes([10, 200] + [0] * 254) + bytes([20, 100] + [0] * 254) + bytes([30, 50] + [0] * 254))
NO_COMPOSITE = resource(0x0421, struct.pack(">IB", 1, 0) + b"\x00" * 10)
JPEG_THUMB = resource(0x040C, struct.pack(">IIIIIIHH", 1, 24, 16, 24 * 3, 24 * 3 * 16, len(JPEG_ORANGE), 24, 1) + JPEG_ORANGE)

# (name, bytes, expected) where expected = (left rgba, right rgba, tolerance), or None when decoding must fail cleanly.
FIXTURES = [
    ("rgb8_raw.psd", build(W, H, 8, 3, rgb(8, RED, BLUE)), (RED + (255,), BLUE + (255,), 1)),
    ("rgb8_rle.psd", build(W, H, 8, 3, rgb(8, RED, BLUE), compression=1), (RED + (255,), BLUE + (255,), 1)),
    ("rgba8_transparent.psd",
     build(W, H, 8, 3, rgb(8, RED, BLUE) + [plane(W, H, 8, 255, 0)], compression=1, layer_count=-1),
     (RED + (255,), BLUE + (0,), 1)),
    ("rgba8_alpha_channel_ignored.psd",
     build(W, H, 8, 3, rgb(8, RED, BLUE) + [plane(W, H, 8, 255, 0)], layer_count=1),
     (RED + (255,), BLUE + (255,), 1)),
    ("gray8.psd", build(W, H, 8, 1, [plane(W, H, 8, 64, 200)]), ((64, 64, 64, 255), (200, 200, 200, 255), 1)),
    ("gray16.psd", build(W, H, 16, 1, [plane(W, H, 16, 64, 200)], compression=1),
     ((64, 64, 64, 255), (200, 200, 200, 255), 1)),
    ("cmyk8.psd",
     build(W, H, 8, 4, [plane(W, H, 8, 0, 255), plane(W, H, 8, 255, 255), plane(W, H, 8, 255, 255), plane(W, H, 8, 255, 255)]),
     ((0, 255, 255, 255), WHITE + (255,), 1)),
    ("rgb16.psd", build(W, H, 16, 3, rgb(16, RED, BLUE)), (RED + (255,), BLUE + (255,), 1)),
    ("rgb32_float.psd", build(W, H, 32, 3, rgb(32, (1.0, 0.0, 0.0), (0.214, 0.214, 0.214))),
     (RED + (255,), (128, 128, 128, 255), 3)),
    ("rgba16_lr16.psd", build(W, H, 16, 3, rgb(16, RED, BLUE) + [plane(W, H, 16, 255, 0)], lr_count=-1),
     (RED + (255,), BLUE + (0,), 1)),
    ("indexed8.psd", build(W, H, 8, 2, [plane(W, H, 8, 0, 1)], palette=IDX_PALETTE),
     ((10, 20, 30, 255), (200, 100, 50, 255), 1)),
    ("bitmap1.psd", build(W, H, 1, 0, [plane(W, H, 1, 1, 0)], compression=1), (BLACK + (255,), WHITE + (255,), 1)),
    ("lab8.psd", build(W, H, 8, 9, [plane(W, H, 8, 255, 0), plane(W, H, 8, 128, 128), plane(W, H, 8, 128, 128)]),
     (WHITE + (255,), BLACK + (255,), 3)),
    ("duotone8.psd", build(W, H, 8, 8, [plane(W, H, 8, 64, 200)]), ((64, 64, 64, 255), (200, 200, 200, 255), 1)),
    ("multichannel3.psd",
     build(W, H, 8, 7, [plane(W, H, 8, 0, 255), plane(W, H, 8, 255, 255), plane(W, H, 8, 255, 255)]),
     ((0, 255, 255, 255), WHITE + (255,), 1)),
    ("psb_rgb8_rle.psb", build(W, H, 8, 3, rgb(8, RED, BLUE), compression=1, psb=True), (RED + (255,), BLUE + (255,), 1)),
    ("psb_rgba16_lr16.psb",
     build(W, H, 16, 3, rgb(16, RED, BLUE) + [plane(W, H, 16, 255, 0)], compression=1, psb=True, lr_count=-1),
     (RED + (255,), BLUE + (0,), 1)),
    ("nocomposite_jpegthumb.psd", build(W, H, 8, 3, rgb(8, BLACK, BLACK), resources=NO_COMPOSITE + JPEG_THUMB),
     ((255, 128, 0, 255), (255, 128, 0, 255), 8)),
    ("nocomposite_nothumb.psd", build(W, H, 8, 3, rgb(8, BLACK, BLACK), resources=NO_COMPOSITE), None),
    ("zip_compression.psd", build(W, H, 8, 3, rgb(8, RED, BLUE), compression=2), None),
    ("truncated_header.psd", build(W, H, 8, 3, rgb(8, RED, BLUE), truncate=20), None),
    ("truncated_imagedata.psd", build(W, H, 8, 3, rgb(8, RED, BLUE), truncate=200), None),
    ("not_a_psd.psd", b"GIF89a" + b"\x00" * 100, None),
    ("big_rgb8_raw.psd", build(4000, 3000, 8, 3, [plane(4000, 3000, 8, l, r) for l, r in zip(RED, BLUE)]),
     (RED + (255,), BLUE + (255,), 1)),
    ("big_rgb8_rle.psd",
     build(1200, 1200, 8, 3, [plane(1200, 1200, 8, l, r) for l, r in zip(RED, BLUE)], compression=1),
     (RED + (255,), BLUE + (255,), 1)),
]


def run(args):
    p = subprocess.run([CLI] + args, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def parse_probe(out):
    line = next((l for l in out.splitlines() if l.startswith("PROBE")), None)
    if not line:
        return None
    parts = line.split()
    w, h = int(parts[1]), int(parts[2])
    left = tuple(int(v) for v in parts[4:8])
    right = tuple(int(v) for v in parts[9:13])
    return w, h, left, right


def close(a, b, tol):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def check(name, data, expect, size=256):
    path = os.path.join(OUT, name)
    with open(path, "wb") as f:
        f.write(data)
    png = os.path.join(OUT, f"{name}.{size}.png")
    t0 = time.perf_counter()
    rc, out, err = run(["decode", path, png, "--size", str(size), "--probe"])
    ms = (time.perf_counter() - t0) * 1000
    if expect is None:
        ok = rc == 2
        return ok, f"exit={rc} ({'clean failure' if ok else 'expected exit code 2'}) {err.strip()}"
    if rc != 0:
        return False, f"exit={rc} {err.strip()}"
    probe = parse_probe(out)
    if not probe:
        return False, "no PROBE line in output"
    w, h, left, right = probe
    exp_l, exp_r, tol = expect
    ok = close(left, exp_l, tol) and close(right, exp_r, tol) and max(w, h) <= size
    return ok, f"{w}x{h} L={left} R={right} {ms:.0f} ms"


def main():
    if not os.path.exists(CLI):
        print(f"psdthumb.exe not found: {CLI}")
        return 1
    os.makedirs(OUT, exist_ok=True)
    failures = 0
    for name, data, expect in FIXTURES:
        ok, detail = check(name, data, expect)
        print(f"{'PASS' if ok else 'FAIL'}  {name:36s} {detail}")
        failures += 0 if ok else 1

    # Requested size smaller than the image must scale the output.
    ok, detail = check("rgb8_raw.psd", FIXTURES[0][1], (RED + (255,), BLUE + (255,), 1), size=32)
    ok = ok and detail.startswith("32x24 ")
    print(f"{'PASS' if ok else 'FAIL'}  {'rgb8_raw.psd @32':36s} {detail}")
    failures += 0 if ok else 1

    # `info` must work on a fixture and report the transparency flag.
    rc, out, err = run(["info", os.path.join(OUT, "rgba8_transparent.psd")])
    ok = rc == 0 and "merged alpha:      yes" in out
    print(f"{'PASS' if ok else 'FAIL'}  {'info rgba8_transparent.psd':36s} exit={rc}")
    failures += 0 if ok else 1

    total = len(FIXTURES) + 2
    print(f"\n{total - failures}/{total} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
