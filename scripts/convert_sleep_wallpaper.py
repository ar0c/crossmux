#!/usr/bin/env python3
"""Convert an existing pet photo to a transparent four-gray e-paper overlay.

The source file is read-only. Background removal runs locally with rembg;
Pillow performs crop, fit, grayscale mapping, and PNG encoding. No generative
image editing is involved.

Requirements: python -m pip install "rembg[cpu]" Pillow numpy scipy
"""

import argparse
import hashlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path, help="Original JPG/PNG; never overwritten")
    p.add_argument("output", type=Path, help="Device-ready transparent PNG")
    p.add_argument("--cutout-cache", type=Path, help="Save/reuse local rembg cutout")
    p.add_argument("--max-x", type=int, help="Mask out pixels at or right of this source-size X")
    p.add_argument("--max-y", type=int, help="Mask out pixels at or below this source-size Y")
    p.add_argument("--crop", type=int, nargs=4, metavar=("L", "T", "R", "B"),
                   help="Closer view in cutout coordinates; does not alter source file")
    p.add_argument("--preserve-frame", action="store_true",
                   help="Fit the entire source frame with letterboxing; no crop")
    p.add_argument("--sharpen", type=int, default=0,
                   help="Output-scale unsharp mask strength, 0..200")
    p.add_argument("--clarity", type=int, default=0,
                   help="Broader local contrast for photo detail, 0..100")
    p.add_argument("--gamma", type=float, default=1.0,
                   help="Grayscale tone curve; below 1 lifts dark detail")
    p.add_argument("--dither", type=int, default=0,
                   help="Ordered 4x4 tone modulation, 0..64; keeps four output grays")
    p.add_argument("--error-diffusion", action="store_true",
                   help="Mask-aware serpentine Floyd-Steinberg four-gray conversion")
    p.add_argument("--fill-internal-holes", action="store_true",
                   help="Restore enclosed subject pixels missing from the cutout alpha mask")
    p.add_argument("--outline", choices=("none", "white", "black"), default="none")
    p.add_argument("--outline-px", type=int, default=4)
    p.add_argument("--align-x", choices=("left", "center", "right"), default="center")
    p.add_argument("--align-y", choices=("top", "center", "bottom"), default="center")
    p.add_argument("--preview", type=Path, help="Save a preview over mid-gray")
    p.add_argument("--model", default="u2net", help="Local rembg model name")
    return p


def diffuse_four_grays(gray: np.ndarray, active: np.ndarray) -> np.ndarray:
    """Represent continuous photo tones with exactly 0, 85, 170, 255."""
    height, width = gray.shape
    output = np.zeros((height, width), dtype=np.uint8)
    current = np.zeros(width + 2, dtype=np.float32)
    following = np.zeros(width + 2, dtype=np.float32)
    for y in range(height):
        direction = 1 if y % 2 == 0 else -1
        x_values = range(width) if direction == 1 else range(width - 1, -1, -1)
        for x in x_values:
            if not active[y, x]:
                continue
            value = float(np.clip(gray[y, x] + current[x + 1], 0, 255))
            level = min(3, int((value + 42.5) // 85)) * 85
            output[y, x] = level
            error = value - level
            side = x + direction
            if 0 <= side < width and active[y, side]:
                current[side + 1] += error * (7 / 16)
            if y + 1 < height:
                back = x - direction
                if 0 <= back < width and active[y + 1, back]:
                    following[back + 1] += error * (3 / 16)
                if active[y + 1, x]:
                    following[x + 1] += error * (5 / 16)
                if 0 <= side < width and active[y + 1, side]:
                    following[side + 1] += error * (1 / 16)
        current, following = following, current
        following.fill(0)
    return output


def main() -> None:
    args = parser().parse_args()
    if args.source.resolve() == args.output.resolve():
        raise SystemExit("Source and output must differ")
    if args.outline_px < 0 or args.outline_px > 16:
        raise SystemExit("--outline-px must be 0..16")
    if args.sharpen < 0 or args.sharpen > 200:
        raise SystemExit("--sharpen must be 0..200")
    if args.clarity < 0 or args.clarity > 100:
        raise SystemExit("--clarity must be 0..100")
    if args.preserve_frame and args.crop:
        raise SystemExit("--preserve-frame cannot be combined with --crop")
    if args.gamma <= 0 or args.gamma > 2:
        raise SystemExit("--gamma must be >0 and <=2")
    if args.dither < 0 or args.dither > 64:
        raise SystemExit("--dither must be 0..64")
    if args.dither and args.error_diffusion:
        raise SystemExit("Choose ordered dither or error diffusion")
    source_hash = sha256(args.source)

    if args.cutout_cache and args.cutout_cache.is_file():
        cutout = Image.open(args.cutout_cache).convert("RGBA")
    else:
        from rembg import new_session, remove

        photo = Image.open(args.source).convert("RGB")
        photo.thumbnail((1536, 2040), Image.Resampling.LANCZOS)
        cutout = remove(photo, session=new_session(args.model)).convert("RGBA")
        if args.cutout_cache:
            args.cutout_cache.parent.mkdir(parents=True, exist_ok=True)
            cutout.save(args.cutout_cache, optimize=True)

    a = np.asarray(cutout.getchannel("A")).copy()
    if args.max_x is not None:
        a[:, args.max_x :] = 0
    if args.max_y is not None:
        a[args.max_y :, :] = 0
    # Background models can leave isolated flecks at the photo edge. Keep the
    # main subject component; this changes only the transparency mask.
    from scipy.ndimage import binary_fill_holes, label

    components, count = label(a >= 80)
    if count:
        areas = np.bincount(components.ravel())
        areas[0] = 0
        a[components != areas.argmax()] = 0
    if args.fill_internal_holes:
        # Segmentation can mistake a dark feature enclosed by the subject
        # (such as this cat's nose) for background. Restore only enclosed
        # pixels; the original RGB and outer silhouette remain untouched.
        internal_holes = binary_fill_holes(a >= 100) & (a < 100)
        a[internal_holes] = 255
    cutout.putalpha(Image.fromarray(a))

    if args.crop:
        left, top, right, bottom = args.crop
        if not (0 <= left < right <= cutout.width and 0 <= top < bottom <= cutout.height):
            raise SystemExit("--crop must fit within the cutout")
        cutout = cutout.crop((left, top, right, bottom))

    if not args.preserve_frame:
        # This removes only fully transparent margins. Visible subject pixels
        # are retained unless the caller explicitly requests --crop.
        bbox = cutout.getchannel("A").point(lambda value: 255 if value >= 80 else 0).getbbox()
        if not bbox:
            raise SystemExit("No subject found")
        cutout = cutout.crop(bbox)
    cutout.thumbnail((480, 800) if args.preserve_frame else (448, 744),
                     Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (480, 800))
    spaces = (480 - cutout.width, 800 - cutout.height)
    x = {"left": 0, "center": spaces[0] // 2, "right": spaces[0]}[args.align_x]
    y = {"top": 0, "center": spaces[1] // 2, "bottom": spaces[1]}[args.align_y]
    canvas.alpha_composite(cutout, (x, y))

    luminosity = canvas.convert("RGB").convert("L")
    if args.clarity:
        luminosity = luminosity.filter(ImageFilter.UnsharpMask(
            radius=5.0, percent=args.clarity, threshold=3))
    if args.sharpen:
        luminosity = luminosity.filter(ImageFilter.UnsharpMask(
            radius=1.0, percent=args.sharpen, threshold=2))
    luminosity = np.asarray(luminosity, dtype=np.float32)
    opacity = np.asarray(canvas.getchannel("A"))
    foreground = luminosity[opacity >= 160]
    if foreground.size == 0:
        raise SystemExit("Subject is fully transparent")
    black, white = np.percentile(foreground, [2, 98])
    if white - black < 20:
        raise SystemExit("Image has insufficient tonal range")
    normalized = np.clip((luminosity - black) * (255 / (white - black)), 0, 255)
    normalized = np.power(normalized / 255, args.gamma) * 255
    if args.dither:
        bayer = np.array([[0, 8, 2, 10], [12, 4, 14, 6],
                          [3, 11, 1, 9], [15, 7, 13, 5]], dtype=np.float32)
        tile = np.tile((bayer - 7.5) / 15, (200, 120))
        normalized = np.clip(normalized + tile * args.dither, 0, 255)
    # The output always uses four native panel levels. Error diffusion can
    # approximate intermediate photographic tones using spatial dot density.
    alpha = Image.fromarray(np.where(opacity >= 100, 255, 0).astype(np.uint8))
    if args.error_diffusion:
        levels = diffuse_four_grays(normalized, opacity >= 100)
    else:
        levels = (np.searchsorted([42, 127, 212], normalized) * 85).astype(np.uint8)
    gray = Image.fromarray(levels)
    ink = Image.merge("RGBA", (gray, gray, gray, alpha))

    result = Image.new("RGBA", (480, 800))
    if args.outline != "none" and args.outline_px:
        # Outline surrounds, but does not repaint, source photograph pixels.
        body = alpha.filter(ImageFilter.MinFilter(5)).filter(ImageFilter.MaxFilter(5))
        expanded = body.filter(ImageFilter.MaxFilter(2 * args.outline_px + 1))
        value = 255 if args.outline == "white" else 0
        result = Image.new("RGBA", (480, 800), (value, value, value, 0))
        result.putalpha(expanded)
    result.alpha_composite(ink)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    result.save(args.output, optimize=True)
    if args.preview:
        backdrop = Image.new("RGBA", (480, 800), (170, 170, 170, 255))
        backdrop.alpha_composite(result)
        backdrop.convert("RGB").save(args.preview, optimize=True)
    if sha256(args.source) != source_hash:
        raise SystemExit("Source changed during conversion")
    print(f"source_sha256={source_hash}")
    print(f"output={args.output} size={result.size} sha256={sha256(args.output)}")


if __name__ == "__main__":
    main()
