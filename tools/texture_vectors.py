"""
Reference vectors for the UI's compressed-texture decoders (app/test/texture_decode.test.js).

    python tools/texture_vectors.py        # writes app/test/vectors/texture_decode.json

Blocks come from two places: random bytes, which every ETC2, EAC, BC7, BC6H and PVRTC bit
pattern decodes (reserved BC modes are skipped), and for ASTC an encoder (astc-encoder-py),
since random ASTC bits are mostly invalid blocks. The expected texels come from
texture2ddecoder, an independent decoder (Pillow for BC6H, which texture2ddecoder gets wrong in three
modes). All three are pip packages; the JSON is checked in so
the test runs without them.
"""
import base64
import io
import json
import os
import random
import struct
import sys

try:
    import texture2ddecoder as ref
except ImportError:
    sys.exit("pip install texture2ddecoder")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "app", "test", "vectors", "texture_decode.json")

rng = random.Random(1234)


def bgra_to_rgba(data):
    out = bytearray(data)
    out[0::4] = data[2::4]
    out[2::4] = data[0::4]
    return bytes(out)


def random_blocks(count, size, ok=lambda b: True):
    blocks = []
    while len(blocks) < count:
        b = bytes(rng.getrandbits(8) for _ in range(size))
        if ok(b):
            blocks.append(b)
    return b"".join(blocks)


vectors = []


def add(name, width, height, data, expected_bgra, tolerance, note=""):
    vectors.append({
        "format": name, "width": width, "height": height, "tolerance": tolerance, "note": note,
        "data": base64.b64encode(data).decode("ascii"),
        "expected": base64.b64encode(bgra_to_rgba(expected_bgra)).decode("ascii"),
    })


# BC7: every mode but the reserved one (a zero low byte).
w = h = 64
data = random_blocks(w * h // 16, 16, lambda b: b[0] != 0)
add("VK_FORMAT_BC7_UNORM_BLOCK", w, h, data, ref.decode_bc7(data, w, h), 1)


# BC6H: reserved 5-bit modes 10011, 10111, 11011, 11111 are skipped. texture2ddecoder's
# BC6H decoder misreads a green bit in three modes, so the reference here is Pillow's
# (through a DDS wrapper), which agrees with this decoder on every block; both clamp the
# floats to 0..1 for their 8-bit output.
def bc6_ok(b):
    mode = b[0] & 3
    if mode >= 2:
        mode = b[0] & 0x1F
    return mode not in (0x13, 0x17, 0x1B, 0x1F)


def pillow_bc6(data, width, height, signed):
    import struct
    from PIL import Image
    hdr = b"DDS " + struct.pack("<7I", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, height, width, width * height, 1, 1) + b"\0" * 44
    hdr += struct.pack("<5I", 32, 0x4, 0x30315844, 0, 0) + b"\0" * 12
    hdr += struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    hdr += struct.pack("<5I", 96 if signed else 95, 3, 0, 1, 0)
    img = Image.open(io.BytesIO(hdr + data)).convert("RGBA")
    rgba = img.tobytes()
    out = bytearray(rgba)
    out[0::4] = rgba[2::4]
    out[2::4] = rgba[0::4]
    return bytes(out)


data = random_blocks(w * h // 16, 16, bc6_ok)
add("VK_FORMAT_BC6H_UFLOAT_BLOCK", w, h, data, pillow_bc6(data, w, h, False), 1, "reference clamps to 0..1")
# No signed vector: Pillow drops the sign extension after the endpoint transform wraps, so it
# disagrees with the specification on every transformed mode below 16-bit endpoints (its
# untransformed and 16-bit modes match). bc6h_signed.test.js covers the signed path with blocks
# whose result the specification fixes exactly.

# ETC2 / EAC: every bit pattern is a valid block.
for name, size, fn, tol in [
    ("VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK", 8, ref.decode_etc2, 0),
    ("VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK", 8, ref.decode_etc2a1, 0),
    ("VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK", 16, ref.decode_etc2a8, 0),
    ("VK_FORMAT_EAC_R11_UNORM_BLOCK", 8, ref.decode_eacr, 1),
    ("VK_FORMAT_EAC_R11_SNORM_BLOCK", 8, ref.decode_eacr_signed, 2),
    ("VK_FORMAT_EAC_R11G11_UNORM_BLOCK", 16, ref.decode_eacrg, 1),
    ("VK_FORMAT_EAC_R11G11_SNORM_BLOCK", 16, ref.decode_eacrg_signed, 2),
]:
    data = random_blocks(w * h // 16, size)
    add(name, w, h, data, fn(data, w, h), tol)

# PVRTC: random blocks over the whole image. A texel's colour comes from four blocks, so this
# exercises the wrap-around at the edges too.
for name, two_bpp in [("VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG", True), ("VK_FORMAT_PVRTC1_4BPP_UNORM_BLOCK_IMG", False)]:
    blocks = (w // (8 if two_bpp else 4)) * (h // 4)
    data = random_blocks(blocks, 8)
    add(name, w, h, data, ref.decode_pvrtc(data, w, h, two_bpp), 1)

# ASTC: an encoder over a noisy gradient with alpha, for a spread of footprints.
try:
    import astc_encoder as astc
except ImportError:
    astc = None
    print("astc-encoder-py not installed: no ASTC vectors")

if astc is not None:
    aw = ah = 48
    pixels = bytearray()
    for y in range(ah):
        for x in range(aw):
            r = min(255, max(0, int(255 * x / aw) + rng.randint(-40, 40)))
            g = min(255, max(0, int(255 * y / ah) + rng.randint(-40, 40)))
            b = min(255, max(0, 128 + rng.randint(-100, 100)))
            a = 255 if (x // 8 + y // 8) % 3 else rng.randint(0, 255)
            pixels += bytes((r, g, b, a))
    for bw, bh in [(4, 4), (5, 4), (5, 5), (6, 5), (6, 6), (8, 5), (8, 6), (8, 8), (10, 5), (10, 6), (10, 8), (10, 10), (12, 10), (12, 12)]:
        config = astc.ASTCConfig(astc.ASTCProfile.LDR, bw, bh, 1, astc.ASTCQualityPreset.MEDIUM)
        context = astc.ASTCContext(config)
        image = astc.ASTCImage(astc.ASTCType.U8, aw, ah, 1, bytes(pixels))
        swizzle = astc.ASTCSwizzle.from_str("RGBA")
        data = context.compress(image, swizzle)
        add("VK_FORMAT_ASTC_%dx%d_UNORM_BLOCK" % (bw, bh), aw, ah, data, ref.decode_astc(data, aw, ah, bw, bh), 1)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w") as f:
    json.dump({"vectors": vectors}, f)
print("wrote %d vectors to %s" % (len(vectors), OUT))
