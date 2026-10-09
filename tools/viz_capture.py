"""Make small previews from raw texture dumps: scene color and motion-vector magnitude (brightness only)."""
import sys
import numpy as np
from PIL import Image

W, H = 3840, 2160

def r11g11b10(path):
    raw = np.fromfile(path, dtype=np.uint32).reshape(H, W)
    def f(bits, mant_bits):
        e = (bits >> mant_bits).astype(np.int32)
        m = (bits & ((1 << mant_bits) - 1)).astype(np.float32)
        val = np.where(e == 0, m / (1 << mant_bits) * 2.0 ** -14, (1 + m / (1 << mant_bits)) * 2.0 ** (e - 15))
        return val.astype(np.float32)
    r = f(raw & 0x7FF, 6); g = f((raw >> 11) & 0x7FF, 6); b = f((raw >> 22) & 0x3FF, 5)
    return np.stack([r, g, b], -1)

def rg16f(path):
    return np.fromfile(path, dtype=np.float16).reshape(H, W, 2).astype(np.float32)

def save(img, path, size=(1280, 720)):
    Image.fromarray(img).resize(size, Image.BILINEAR).save(path)

kind, src, dst = sys.argv[1:4]
if kind == "color":
    c = r11g11b10(src)
    c = c / (1 + c)                      # simple tone map for viewing
    c = np.clip(c / max(c.max(), 1e-6), 0, 1) ** (1 / 2.2)
    save((c * 255).astype(np.uint8), dst)
elif kind == "mv":
    mv = rg16f(src)
    px = np.stack([mv[..., 0] * 0.5625 * W, mv[..., 1] * H], -1)   # decode to pixels per frame
    mag = np.hypot(px[..., 0], px[..., 1])
    print(f"motion px: median {np.median(mag):.2f}, p99 {np.percentile(mag, 99):.2f}, max {mag.max():.2f}; "
          f"mean dx {px[..., 0].mean():+.2f} dy {px[..., 1].mean():+.2f}")
    v = np.clip(mag / max(np.percentile(mag, 99.5), 1e-6), 0, 1)
    save((v ** 0.5 * 255).astype(np.uint8), dst)
