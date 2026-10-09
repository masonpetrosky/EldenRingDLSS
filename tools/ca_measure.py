"""Measure chromatic aberration in raw add-on screenshots (shots/<name>.bin + .txt).

usage: ca_measure.py <shots dir> <name> [<name> ...]
Splits the frame into a 9x5 grid of 256 px tiles and finds how far the red and blue channels are shifted against
green in each tile (phase correlation with a sub-pixel peak). Prints the horizontal shift per screen column,
averaged over the rows, and the largest vertical shift. The game's aberration is ~0 at the center and grows
toward the left and right edges, with red and blue moving in opposite directions.
"""
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shots  # noqa: E402

TILE, COLS, ROWS = 256, 9, 5


def shift(a, b):
    """Offset d (dx, dy) with a(x) ~ b(x - d), to a fraction of a pixel."""
    win = np.outer(np.hanning(a.shape[0]), np.hanning(a.shape[1]))
    cross = np.fft.fft2((a - a.mean()) * win) * np.conj(np.fft.fft2((b - b.mean()) * win))
    corr = np.fft.fftshift(np.real(np.fft.ifft2(cross / (np.abs(cross) + 1e-9))))
    py, px = np.unravel_index(np.argmax(corr), corr.shape)

    def refine(m, p):
        if not 0 < p < len(m) - 1:
            return float(p)
        return p + (m[p - 1] - m[p + 1]) / (2 * (m[p - 1] - 2 * m[p] + m[p + 1]) + 1e-12)

    return refine(corr[py, :], px) - corr.shape[1] // 2, refine(corr[:, px], py) - corr.shape[0] // 2


def measure(img):
    h, w = img.shape[:2]
    red = np.zeros((ROWS, COLS, 2))
    blue = np.zeros((ROWS, COLS, 2))
    for j in range(ROWS):
        for i in range(COLS):
            x0 = min(max(int((i + 0.5) * w / COLS) - TILE // 2, 0), w - TILE)
            y0 = min(max(int((j + 0.5) * h / ROWS) - TILE // 2, 0), h - TILE)
            tile = img[y0:y0 + TILE, x0:x0 + TILE]
            red[j, i] = shift(tile[..., 0], tile[..., 1])
            blue[j, i] = shift(tile[..., 2], tile[..., 1])
    return red, blue


def main():
    shots_dir, names = sys.argv[1], sys.argv[2:]
    for name in names:
        red, blue = measure(shots.load(shots_dir, name))
        print("%s: horizontal shift against green per screen column, left to right (px)" % name)
        print("  red  " + " ".join("%+5.2f" % v for v in red[..., 0].mean(axis=0)))
        print("  blue " + " ".join("%+5.2f" % v for v in blue[..., 0].mean(axis=0)))
        print("  largest vertical shift %.2f px" % max(np.abs(red[..., 1]).max(), np.abs(blue[..., 1]).max()))


if __name__ == "__main__":
    main()
