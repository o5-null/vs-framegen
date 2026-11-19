"""Comparison images: the real middle frame next to what each engine generated.

For three triplets it writes
  compare/panel_<gt>.png  full frames: left neighbour | ground truth | 8 engines
  compare/zoom_<gt>.png   1:1 crop of the busiest area, same order
  compare/diff_<gt>.png   |generated - ground truth| x4, so blur and hallucination show
"""

import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import common as C

OUT = os.path.join(C.DATA, "quality", "compare")
PICKS = [2, 9, 16]          # triplet indices, spread over the 19
TILE = (480, 270)
ZOOM = (640, 360)


def font(size):
    for path in (r"C:\Windows\Fonts\msyh.ttc", r"C:\Windows\Fonts\arial.ttf"):
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, size)
            except OSError:
                pass
    return ImageFont.load_default()


def readable(color):
    """Dark captions (vfg is #1a1a1a) disappear on the dark strip: lift them towards white."""
    try:
        rgb = tuple(int(color[i:i + 2], 16) for i in (1, 3, 5))
    except (ValueError, IndexError):
        return color
    luma = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]
    if luma >= 110:
        return color
    lift = min(0.75, (110 - luma) / 255.0 + 0.35)
    mix = tuple(int(v + (255 - v) * lift) for v in rgb)
    return "#%02x%02x%02x" % mix


def grid(tiles, cols, tile_w, tile_h, cap=30, font_size=20):
    """tiles: [(rgb uint8, caption, colour)] -> PIL image with a caption above each tile."""
    rows = (len(tiles) + cols - 1) // cols
    canvas = Image.new("RGB", (cols * tile_w, rows * (tile_h + cap)), (18, 18, 18))
    draw = ImageDraw.Draw(canvas)
    f = font(font_size)
    for i, (rgb, caption, color) in enumerate(tiles):
        r, c = divmod(i, cols)
        y = r * (tile_h + cap)
        draw.rectangle([c * tile_w, y, c * tile_w + tile_w - 1, y + cap - 1], fill=(32, 32, 32))
        draw.text((c * tile_w + 8, y + 5), caption, fill=readable(color), font=f)
        img = Image.fromarray(np.ascontiguousarray(rgb)).resize((tile_w, tile_h), Image.LANCZOS)
        canvas.paste(img, (c * tile_w, y + cap))
    return canvas


def busiest_window(gray, w, h):
    """Top-left corner of the w x h crop with the most gradient energy."""
    import cv2

    energy = cv2.Laplacian(gray, cv2.CV_32F, ksize=3)
    box = cv2.boxFilter(np.abs(energy), -1, (w, h), normalize=True)
    box[:h, :] = 0
    box[:, :w] = 0
    cy, cx = np.unravel_index(np.argmax(box), box.shape)
    return int(cx - w // 2), int(cy - h // 2)


def main():
    picks = [int(a) for a in sys.argv[1:]] or PICKS
    names = C.gt_names()
    srcs = C.png_names()
    os.makedirs(OUT, exist_ok=True)
    for k in picks:
        gt_name = names[k]
        gt = C.read_rgb(os.path.join(C.SRC_PNG, gt_name))
        a = C.read_rgb(srcs[2 * k])
        gens = {e: C.read_rgb(os.path.join(C.quality_dir(e), gt_name)) for e in C.ENGINES}

        tiles = [(C.to_u8(a), "左邻帧 A", "#9aa0a6"),
                 (C.to_u8(gt), "真实中间帧 GT", "#ffffff")]
        tiles += [(C.to_u8(gens[e]), e, C.COLORS[e]) for e in C.ENGINES]
        grid(tiles, 5, *TILE).save(os.path.join(OUT, "panel_%s" % gt_name))
        print("wrote panel_%s" % gt_name)

        gray = C.to_gray(gt)
        x, y = busiest_window(gray, *ZOOM)
        x = max(0, min(x, gt.shape[1] - ZOOM[0]))
        y = max(0, min(y, gt.shape[0] - ZOOM[1]))
        crop = lambda rgb: rgb[y:y + ZOOM[1], x:x + ZOOM[0]]
        zt = [(C.to_u8(crop(a)), "左邻帧 A（1:1）", "#9aa0a6"),
              (C.to_u8(crop(gt)), "真实中间帧 GT（1:1）", "#ffffff")]
        zt += [(C.to_u8(crop(gens[e])), e, C.COLORS[e]) for e in C.ENGINES]
        grid(zt, 5, *ZOOM).save(os.path.join(OUT, "zoom_%s" % gt_name))
        print("wrote zoom_%s" % gt_name)

        dt = [(np.abs(crop(gens[e]) - crop(gt)) * 4.0, "%s 误差x4" % e, C.COLORS[e]) for e in C.ENGINES]
        grid([(C.to_u8(np.clip(t[0], 0, 1)), t[1], t[2]) for t in dt], 4, *TILE).save(
            os.path.join(OUT, "diff_%s" % gt_name))
        print("wrote diff_%s" % gt_name)


if __name__ == "__main__":
    main()
