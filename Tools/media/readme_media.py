"""The README's pictures, made from GeometryView's own showcase models (Tools/showcase).

    python3 Tools/media/readme_media.py [--out DIR] [job ...]

With no jobs it makes every picture from what the captures left in build/; the capture jobs run
the browser viewer and the Mac app, and are run by name:

    capture-gallery   the gallery's views and posters, in Illustration and Blueprint (browser, 2x)
    capture-phone     the lens on a phone, in see-through (browser)
    capture-mac       the Mac app's window over the turbofan (needs a release build)

It needs a release build (gcli and the shaders), the browser engine (cmake --build --preset
wasm-webgpu) and the showcase's files (Tools/showcase/make.py library, then model ...). Pictures
are WebP at twice the README's 880 px column, light and dark twins drawn alike, into build/media/
readme unless --out says where (Docs/media for the README itself).
"""

from __future__ import annotations

import json
import math
import os
import random
import shutil
import subprocess
import sys
import time
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
GCLI = ROOT / "build/release/Tools/gcli"
SHADERS = ROOT / "build/release/shaders/default.metallib"
SHOWCASE = ROOT / "build/showcase"
BLOBS = SHOWCASE / "blobs"
LIB = BLOBS / "library"
SHOTS = ROOT / "build/fixtures/out"
WORK = ROOT / "build/media/readme-work"
FONTS = ROOT / "ThirdParty/ibm-plex-mono"
APP = ROOT / "build/release/App/GeometryView.app"
CONTAINER = Path.home() / "Library/Containers/com.aalok.geometryview/Data"
LSREGISTER = ("/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/"
              "Support/lsregister")
OUT = ROOT / "build/media/readme"

FOLDER = "Things That Come in Sizes"
CAPTURE = "media-readme"  # the gallery captures' label
WIDTH = 1760              # the column, 880 px, at 2x

# The house palette, and GitHub's dark page.
INK = (35, 30, 22)
PAPER = (242, 239, 231)
SHEET = (247, 245, 238)
MUSTARD = (244, 184, 41)
NIGHT_INK = (232, 228, 218)
NIGHT_PAPER = (23, 24, 27)
NIGHT_CARD = (34, 35, 39)
NIGHT_CHIP = (30, 68, 121)

# A theme: how a picture is drawn for GitHub's light page and for its dark one.
THEMES = {
    "light": {"style": 0, "ink": INK, "paper": PAPER, "chip": SHEET, "card": SHEET},
    "dark": {"style": 3, "ink": NIGHT_INK, "paper": NIGHT_PAPER, "chip": NIGHT_CHIP, "card": NIGHT_CARD},
}
STYLE_NAMES = ["illustration", "metal", "clay", "blueprint", "technical", "studio"]


def version() -> str:
    """The release the pictures are drawn for: major.minor of the npm package."""
    v = json.loads((ROOT / "Launcher/package.json").read_text())["version"]
    return ".".join(v.split(".")[:2])


def font(weight: str, size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(FONTS / f"IBMPlexMono-{weight}.ttf"), size)


# ---- Drawing ----------------------------------------------------------------------------------

def render(shots: list[tuple[str, Path, str]], size: str, supersample: int = 2) -> dict[str, dict]:
    """Renders (name, blob, flags) shots into WORK in one gcli batch; returns each one's report."""
    batch = "".join(f'"{blob}" "{WORK / (name + ".png")}" {flags}\n' for name, blob, flags in shots)
    out = subprocess.run([str(GCLI), "render", str(SHADERS), "--batch", "--size", size,
                          "--supersample", str(supersample), "--report"],
                         input=batch, text=True, capture_output=True, check=True).stdout
    return {name: json.loads(line) for (name, _, _), line in zip(shots, out.splitlines())}


def work(name: str) -> Image.Image:
    return Image.open(WORK / f"{name}.png").convert("RGB")


def crop(img: Image.Image, rect, margin: float = 0.06, below: float = 1.0) -> tuple[int, int, int, int]:
    """A box round a part's rectangle, with room below for its shadow."""
    x, y, w, h = rect
    m = int(margin * max(w, h))
    return (max(0, x - m), max(0, y - m), min(img.width, x + w + m), min(img.height, y + h + int(m * (1 + below))))


def square(img: Image.Image, side: int) -> Image.Image:
    s = min(img.size)
    x, y = (img.width - s) // 2, (img.height - s) // 2
    return img.crop((x, y, x + s, y + s)).resize((side, side), Image.LANCZOS)


def fit(img: Image.Image, width: int) -> Image.Image:
    return img if img.width == width else img.resize((width, round(img.height * width / img.width)), Image.LANCZOS)


def rounded(img: Image.Image, radius: int) -> Image.Image:
    """Corners rounded off, keeping whatever was already clear."""
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, img.width - 1, img.height - 1), radius, fill=255)
    out = img.convert("RGBA")
    out.putalpha(ImageChops.multiply(out.getchannel("A"), mask))
    return out


def sticker(img: Image.Image, radius: int = 22, border: int = 4, shadow: int = 8, ink=INK) -> Image.Image:
    """GeometryView's sticker: rounded, an ink border, a hard shadow down and right."""
    w, h = img.size
    out = Image.new("RGBA", (w + shadow + border, h + shadow + border), (0, 0, 0, 0))
    d = ImageDraw.Draw(out)
    d.rounded_rectangle((shadow, shadow, w + shadow + border - 1, h + shadow + border - 1), radius, fill=ink + (255,))
    framed = Image.new("RGBA", (w + border, h + border), (0, 0, 0, 0))
    ImageDraw.Draw(framed).rounded_rectangle((0, 0, w + border - 1, h + border - 1), radius, fill=ink + (255,))
    framed.alpha_composite(rounded(img, max(1, radius - border // 2)), (border // 2, border // 2))
    out.alpha_composite(framed, (0, 0))
    return out


def chip(text: str, size: int = 30, fill=SHEET, ink=INK, weight: str = "Medium", tick: bool = False) -> Image.Image:
    """A pill label in Plex Mono, with a drawn tick after it if asked (Plex Mono has no check mark)."""
    f = font(weight, size)
    box = f.getbbox(text)
    extra = int(size * 1.1) if tick else 0
    w, h = box[2] - box[0] + int(size * 1.2) + extra, int(size * 1.7)
    out = Image.new("RGBA", (w + 4, h + 4), (0, 0, 0, 0))
    d = ImageDraw.Draw(out)
    d.rounded_rectangle((1, 1, w + 2, h + 2), h // 2, fill=fill + (255,), outline=ink + (255,), width=3)
    d.text(((w + 4 - extra) / 2, (h + 4) / 2), text, font=f, fill=ink + (255,), anchor="mm")
    if tick:
        x, y, u = w + 2 - extra + size * 0.15, (h + 4) / 2, size * 0.32
        d.line((x, y, x + u * 0.8, y + u * 0.8, x + u * 2.1, y - u * 0.9), fill=ink + (255,), width=max(3, size // 9),
               joint="curve")
    return out


def labelled(img: Image.Image, label: str, theme: dict, size: int = 28) -> Image.Image:
    """A sticker with its label on it, top left."""
    st = sticker(img, 16, 4, 8, theme["ink"])
    st.alpha_composite(chip(label, size), (16, 16))
    return st


def row(tiles: list[Image.Image], gap: int) -> Image.Image:
    out = Image.new("RGBA", (sum(t.width for t in tiles) + gap * (len(tiles) - 1), max(t.height for t in tiles)),
                    (0, 0, 0, 0))
    x = 0
    for t in tiles:
        out.alpha_composite(t, (x, 0))
        x += t.width + gap
    return out


def save(img: Image.Image, name: str, quality: int = 84) -> Path:
    path = OUT / f"{name}.webp"
    img.save(path, "WEBP", quality=quality, method=6)
    print(f"  {path.name}: {img.size[0]}x{img.size[1]}, {path.stat().st_size / 1e3:.0f} KB")
    return path


# ---- Captures ---------------------------------------------------------------------------------

OPENED = ("(() => { const s = document.querySelector('.masthead .subtitle'); return !!s && "
          "s.textContent.includes('bodies') && !document.querySelector('.masthead .progress:not([hidden])'); })()")


def capture_gallery():
    """The gallery's five views and their posters, in Illustration and Blueprint, and a search for
    "C5", through the browser viewer's own check at 2x."""
    env = dict(os.environ, GEOMETRYVIEW_SCALE="2",
               GEOMETRYVIEW_GALLERY_FOLDERS=f"{FOLDER}={SHOWCASE / 'library' / FOLDER}")
    base = f"/viewer/?test=gallery&backend=visit&folder={FOLDER.replace(' ', '%20')}&views=all&shots=1"
    for style in (0, 3):
        subprocess.run(["node", str(ROOT / "Tools/web_report.mjs"),
                        f"{base}&styles={style}&poster=1&label={CAPTURE}-s{style}", "2300"], env=env, check=True)
    subprocess.run(["node", str(ROOT / "Tools/web_report.mjs"),
                    f"{base}&styles=0&search=C5&label={CAPTURE}-live", "2300"], env=env, check=True)


def capture_phone():
    """The lens on a phone, see-through, in the browser viewer: Illustration and Studio."""
    shutil.copy(SHOWCASE / "step/lens.step", ROOT / "build/fixtures/lens.step")
    port = 7600 + random.randrange(300)
    server = subprocess.Popen(["node", str(ROOT / "Web/spike/serve.mjs"), str(port)], stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, text=True)
    try:
        server.stdout.readline()
        time.sleep(0.5)
        for theme, style in (("light", 0), ("dark", 5)):
            url = f"http://127.0.0.1:{port}/viewer/?import=lens.step&plain"
            steps = [{"size": [390, 844, 3], "phone": True}, {"go": url}, {"until": OPENED, "timeout": 180},
                     {"wait": 1500}]
            if style:  # the look the page keeps, so its chrome follows the style, then the page again
                steps += [{"eval": f"localStorage.setItem('look', JSON.stringify({{style: {style}}}))"},
                          {"go": url}, {"until": OPENED, "timeout": 180}, {"wait": 1500}]
            steps += [{"eval": "window.geometryView.perform(18)"}, {"wait": 2500},
                      {"shot": str(WORK / f"phone-{theme}.png")}]
            subprocess.run(["node", str(ROOT / "Tools/media/page.mjs"), json.dumps(steps)], check=True,
                           stdout=subprocess.DEVNULL)
            print(f"  phone-{theme}.png")
    finally:
        server.terminate()


def capture_mac():
    """The Mac app's window: the turbofan cut, its structure card open. The app's settings are
    exported first and imported after, and the build is unregistered from Launch Services when done,
    so only the installed app is offered to open files."""
    prefs = CONTAINER / "Library/Preferences/com.aalok.geometryview"
    picture = CONTAINER / "tmp/chrome.png"
    saved = WORK / "prefs-backup.plist"
    subprocess.run(["defaults", "export", str(prefs), str(saved)], check=True)
    try:
        for theme, style in (("light", 0), ("dark", 5)):
            if picture.exists():
                picture.unlink()
            subprocess.run(["open", "-n", "-a", str(APP), "--env", "GEOMETRYVIEW_SNAPSHOT_SECTION=1",
                            "--env", f"GEOMETRYVIEW_SNAPSHOT_STYLE={style}", str(SHOWCASE / "step/turbofan.step"),
                            "--args", "--snapshot", "--plain", "--card", "structure", "--window", "1440x900"],
                           check=True)
            for _ in range(360):
                if picture.exists():
                    break
                time.sleep(0.5)
            time.sleep(3)
            subprocess.run(["pkill", "-f", f"{APP}/Contents/MacOS/GeometryView"])
            time.sleep(1)
            if not picture.exists():
                raise RuntimeError("the app wrote no picture")
            Image.open(picture).save(WORK / f"mac-{theme}.png")
            print(f"  mac-{theme}.png")
    finally:
        subprocess.run(["defaults", "import", str(prefs), str(saved)], check=True)
        subprocess.run([LSREGISTER, "-u", str(APP)])


def shot(style: int, name: str) -> Image.Image:
    return Image.open(SHOTS / f"{CAPTURE}-s{style}-{name}.png").convert("RGB")


def poster(style: int, view: str) -> Image.Image:
    return shot(style, f"poster-{view}")


def gallery(style: int, view: str) -> Image.Image:
    return shot(style, f"gallery-{FOLDER.replace(' ', '-')}-{view}-{style}")


# ---- Renders the pictures share ---------------------------------------------------------------

def renders():
    """The turbofan cut along its axis (Illustration, Blueprint) and the lens's optics alone
    (Illustration, Studio), which the section and isolate stickers crop."""
    tf, lens = BLOBS / "turbofan.geoblob", BLOBS / "lens.geoblob"
    render([("tf-cut-light", tf, "--az 62 --el 16 --section x0.5"),
            ("tf-cut-dark", tf, "--az 62 --el 16 --section x0.5 --style blueprint")], "3400x2200")
    optics = "--az 35 --el 18 --isolate lens_element,aperture_blades --colour material"
    # Framed with room round it: the stack is tall, and a square tile shows it whole.
    probe = render([("optics-probe", lens, optics)], "700x700", 1)
    wpp = probe["optics-probe"]["world_per_pixel"] * 0.5 * 1.12
    optics += f" --world-per-pixel {wpp:.5f}"
    render([("optics-light", lens, optics), ("optics-dark", lens, optics + " --style studio")], "1400x1400")


# ---- The pictures -----------------------------------------------------------------------------

def facts(blob: Path) -> dict:
    return json.loads(subprocess.run([str(GCLI), "facts", str(blob)], capture_output=True, text=True,
                                     check=True).stdout)


def projector(blob: Path, az: float, el: float, wpp: float, w: int, h: int):
    """World point to pixel for an orthographic shot framed on the file's bounds, as gcli frames it."""
    f = facts(blob)
    c = [(a + b) / 2 for a, b in zip(f["min"], f["max"])]
    a, e = math.radians(az), math.radians(el)
    fwd = (-math.sin(a) * math.cos(e), math.cos(a) * math.cos(e), -math.sin(e))
    right = (math.cos(a), math.sin(a), 0.0)
    up = (right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2],
          right[0] * fwd[1] - right[1] * fwd[0])

    def to_pixel(p):
        d = [p[i] - c[i] for i in range(3)]
        return (w / 2 + sum(d[i] * right[i] for i in range(3)) / wpp,
                h / 2 - sum(d[i] * up[i] for i in range(3)) / wpp)
    return to_pixel


def fig1():
    """FIG. 1, the hero: the turbofan cut along its axis as a manual's plate, with a frame, crop
    marks, numbered callouts and a drafting title block. Illustration on light, Blueprint on dark."""
    tf = BLOBS / "turbofan.geoblob"
    W, H, az, el = WIDTH, 1040, 62, 16
    probe = render([("fig1-probe", tf, f"--az {az} --el {el} --section x0.5")], f"{W}x{H}", 1)
    wpp = probe["fig1-probe"]["world_per_pixel"] * 1.18
    render([("fig1-light", tf, f"--az {az} --el {el} --section x0.5 --world-per-pixel {wpp}"),
            ("fig1-dark", tf, f"--az {az} --el {el} --section x0.5 --world-per-pixel {wpp} --style blueprint")],
           f"{W}x{H}")
    px = projector(tf, az, el, wpp, W, H)
    axis = 480.0
    callouts = [("1", (0, -352, axis + 230)), ("2", (0, -238, axis + 145)), ("3", (0, -70, axis + 142)),
                ("4", (0, 125, axis + 150)), ("5", (0, 320, axis + 165)), ("6", (0, 505, axis + 30))]
    for theme in ("light", "dark"):
        ink = THEMES[theme]["ink"]
        fill = SHEET if theme == "light" else NIGHT_CHIP
        img = Image.open(WORK / f"fig1-{theme}.png").convert("RGBA")
        d = ImageDraw.Draw(img)
        m = 44
        d.rectangle((m, m, W - m, H - m), outline=ink, width=3)
        for x, y, sx, sy in ((m, m, -1, -1), (W - m, m, 1, -1), (m, H - m, -1, 1), (W - m, H - m, 1, 1)):
            d.line((x + sx * 10, y, x + sx * 34, y), fill=ink, width=3)
            d.line((x, y + sy * 10, x, y + sy * 34), fill=ink, width=3)
        f_num = font("SemiBold", 30)
        top = m + 70
        for n, p in callouts:
            x, y = px(p)
            by = top if n != "6" else top + 60
            d.line((x, by + 26, x, y), fill=ink, width=3)
            d.ellipse((x - 7, y - 7, x + 7, y + 7), fill=ink)
            d.ellipse((x - 28, by - 28, x + 28, by + 28), fill=fill + (255,), outline=ink, width=3)
            d.text((x, by), n, font=f_num, fill=ink, anchor="mm")
        rows = [("FIG. 1", "TF-1 TURBOFAN"), ("VIEW", "SECTION A-A"),
                ("STYLE", "ILLUSTRATION" if theme == "light" else "BLUEPRINT"),
                ("DRAWN BY", f"GEOMETRYVIEW {version()}")]
        bw, rh = 600, 46
        x0, y0 = W - m - bw, H - m - rh * len(rows)
        f_key, f_val = font("Medium", 20), font("SemiBold", 24)
        for i, (k, v) in enumerate(rows):
            y = y0 + i * rh
            d.rectangle((x0, y, W - m, y + rh), outline=ink, width=3,
                        fill=(SHEET if theme == "light" else (24, 55, 100)) + (255,))
            d.line((x0 + 170, y, x0 + 170, y + rh), fill=ink, width=3)
            d.text((x0 + 16, y + rh / 2), k, font=f_key, fill=ink, anchor="lm")
            d.text((x0 + 186, y + rh / 2), v, font=f_val, fill=ink, anchor="lm")
        save(rounded(img, 20), f"fig1-{theme}", quality=86)


def plates():
    """The folder's Plates poster, whole: Illustration on light, Blueprint on dark."""
    for theme, t in THEMES.items():
        save(rounded(fit(poster(t["style"], "plates"), WIDTH), 18), f"plates-{theme}")


def pinned(img: Image.Image, label: str, tilt: float, t: dict, size: int = 30) -> Image.Image:
    """A sticker with its label hanging from its top edge, turned a little."""
    st = sticker(img, 16, 4, 8, t["ink"])
    tag = chip(label, size, t["chip"], t["ink"])
    hang = tag.height * 2 // 3  # the label stands mostly above the edge, clear of the poster's title
    held = Image.new("RGBA", (st.width, st.height + hang), (0, 0, 0, 0))
    held.alpha_composite(st, (0, hang))
    held.alpha_composite(tag, (28, 0))
    return held.rotate(tilt, resample=Image.BICUBIC, expand=True)


def wall():
    """The Gallery Issue: four of the folder's posters pinned up, a little askew, under a masthead."""
    def cut(style, v, w, h):
        img = poster(style, v)
        img = img.crop((0, 0, img.width, min(img.height, int(img.width * h / w))))
        return img.resize((w, h), Image.LANCZOS)

    for theme, t in THEMES.items():
        style = t["style"]
        W, H = WIDTH, 1300
        out = Image.new("RGBA", (W, H), t["paper"] + (255,))
        d = ImageDraw.Draw(out)
        d.text((56, 60), f"GEOMETRYVIEW  ·  NO. {version()}  ·  THE GALLERY ISSUE", font=font("SemiBold", 34),
               fill=t["ink"])
        d.line((56, 120, W - 56, 120), fill=t["ink"], width=4)
        items = [(cut(style, "plates", 1100, 600), "PLATES", (56, 180), -0.6),
                 (cut(style, "ladders", 470, 760), "LADDERS", (1220, 170), 0.8),
                 (cut(style, "atlas", 640, 400), "ATLAS", (56, 850), 0.7),
                 (cut(style, "catalogue", 620, 330), "CATALOGUE", (740, 890), -0.8)]
        for img, label, (x, y), tilt in items:
            p = pinned(img, label, tilt, t, 24)
            out.alpha_composite(p, (x, y - 20))
        save(rounded(out, 24), f"wall-{theme}", quality=84)


def inside():
    """Three ways in: the turbofan cut, the lens with its housings turned to glass, and the lens's
    glass and iris alone."""
    for theme, t in THEMES.items():
        tf = work(f"tf-cut-{theme}")
        tf = tf.crop((int(tf.width * 0.16), int(tf.height * 0.12), int(tf.width * 0.84), int(tf.height * 0.84)))
        phone = work(f"phone-{theme}")
        phone = phone.crop((0, int(phone.height * 0.22), phone.width, int(phone.height * 0.72)))
        optics = work(f"optics-{theme}")
        tiles = [labelled(square(img, 560), label, t, 26)
                 for img, label in ((tf, "SECTION"), (phone, "SEE-THROUGH"), (optics, "ISOLATE"))]
        save(fit(row(tiles, 20), WIDTH), f"inside-{theme}", quality=86)


def styles():
    """The orrery in all six styles, three by two, each labelled."""
    orrery = BLOBS / "orrery.geoblob"
    r = render([(f"orrery-{s}", orrery, f"--style {s} --az 35 --el 24") for s in STYLE_NAMES], "1200x1000")
    rects = [r[f"orrery-{s}"]["part_rect"] for s in STYLE_NAMES]
    x0 = min(x for x, _, _, _ in rects)
    y0 = min(y for _, y, _, _ in rects)
    x1 = max(x + w for x, _, w, _ in rects)
    y1 = max(y + h for _, y, _, h in rects)
    tw, th = 568, 476
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2 + 20
    side = max((x1 - x0) / tw, (y1 - y0) / th) * 1.22
    box = (int(cx - tw * side / 2), int(cy - th * side / 2), int(cx + tw * side / 2), int(cy + th * side / 2))
    for theme, t in THEMES.items():
        tiles = [labelled(work(f"orrery-{s}").crop(box).resize((tw, th), Image.LANCZOS), s.capitalize(), t, 26)
                 for s in STYLE_NAMES]
        grid = Image.new("RGBA", (tiles[0].width * 3 + 2 * 20, tiles[0].height * 2 + 20), (0, 0, 0, 0))
        for i, tile in enumerate(tiles):
            grid.alpha_composite(tile, ((i % 3) * (tile.width + 20), (i // 3) * (tile.height + 20)))
        save(fit(grid, WIDTH), f"styles-{theme}", quality=86)


def window(img: Image.Image, width: int, ink, bar_fill, title: str = "", url: str = "") -> Image.Image:
    """A window: a title bar with its three lights (and an address, for a browser) over the shot."""
    body = fit(img.convert("RGB"), width)
    bar = 46 if not url else 70
    w, h = body.width, body.height + bar
    win = Image.new("RGB", (w, h), bar_fill)
    win.paste(body, (0, bar))
    d = ImageDraw.Draw(win)
    for i, c in enumerate(((237, 106, 94), (245, 191, 79), (98, 197, 84))):
        d.ellipse((18 + i * 26, 15, 34 + i * 26, 31), fill=c)
    if url:
        d.rounded_rectangle((120, 12, w - 120, 58), 23, fill=(255, 255, 255) if bar_fill[0] > 128 else (52, 53, 58))
        d.text((w / 2, 35), url, font=font("Regular", 22), fill=ink, anchor="mm")
    elif title:
        d.text((w / 2, 23), title, font=font("Medium", 20), fill=ink, anchor="mm")
    return sticker(win, 18, 4, 8, ink)


def everywhere():
    """The Mac app, a browser and a phone at once: the turbofan cut in the app, the gallery in the
    browser, the lens in see-through on the phone."""
    for theme, t in THEMES.items():
        ink = t["ink"]
        bar = (236, 232, 221) if theme == "light" else (40, 41, 46)
        W, H = WIDTH, 1180
        out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        mac = window(work(f"mac-{theme}"), 1120, ink, bar, title="turbofan.step")
        browser = window(gallery(t["style"], "plates"), 1100, ink, bar, url="atharvaaalok.github.io/geometryview")
        out.alpha_composite(mac, (0, 0))
        out.alpha_composite(browser, (W - browser.width, H - browser.height - 10))
        screen = rounded(fit(work(f"phone-{theme}"), 300), 34)
        rim = 12
        phone = Image.new("RGBA", (screen.width + 2 * rim, screen.height + 2 * rim), (0, 0, 0, 0))
        ImageDraw.Draw(phone).rounded_rectangle((0, 0, phone.width - 1, phone.height - 1), 44, fill=ink + (255,))
        phone.alpha_composite(screen, (rim, rim))
        shadow = Image.new("RGBA", (phone.width + 10, phone.height + 10), (0, 0, 0, 0))
        ImageDraw.Draw(shadow).rounded_rectangle((10, 10, phone.width + 9, phone.height + 9), 44, fill=ink + (255,))
        shadow.alpha_composite(phone, (0, 0))
        out.alpha_composite(shadow, (60, H - shadow.height))
        save(out, f"everywhere-{theme}", quality=86)


def chat():
    """An AI app's answer: the prompt, the tools it called, and the picture render_view returned, on
    a neutral chat card."""
    tf = BLOBS / "turbofan.geoblob"
    png = WORK / "chat-render.png"
    # Exactly what render_view runs on a Mac: the positional form, Metal, overlay and all.
    subprocess.run([str(GCLI), "render", str(SHADERS), str(tf), str(png), "1200", "800", "62", "16", "ortho",
                    "1", "0", "x0.5", "overlay"], check=True)
    for theme, t in THEMES.items():
        ink, card = t["ink"], t["card"]
        body = Image.new("RGB", (WIDTH - 12, 1288), card)
        d = ImageDraw.Draw(body)
        f_msg, f_small = font("Medium", 38), font("Regular", 30)
        prompt = ["Cut the turbofan in half and", "show me the combustor, in metal."]
        bw = 860
        bx = body.width - bw - 48
        d.rounded_rectangle((bx, 40, bx + bw, 40 + 56 * len(prompt) + 48), 34, fill=MUSTARD, outline=INK, width=4)
        for i, line in enumerate(prompt):
            d.text((bx + 36, 64 + i * 56), line, font=f_msg, fill=INK)
        y = 40 + 56 * len(prompt) + 48 + 36
        x = 48
        for tool in ("open_model", "get_structure", "render_view"):
            c = chip(tool, 26, card, ink, tick=True)
            body.paste(c, (x, y), c)
            x += c.width + 14
        y += 76
        framed = sticker(Image.open(png).convert("RGB").resize((1080, 720), Image.LANCZOS), 18, 4, 6, ink)
        body.paste(framed, (48, y), framed)
        tx = 48 + framed.width + 40
        reply = ["Here it is, cut along", "its axis in Metal.", "", "The combustor is the", "ring between the",
                 "compressor and the", "turbines, its 18 fuel", "nozzles at the front."]
        for i, line in enumerate(reply):
            d.text((tx, y + 20 + i * 46), line, font=f_small, fill=ink)
        body = body.crop((0, 0, body.width, y + framed.height + 44))
        save(fit(sticker(body, 28, 4, 8, ink), WIDTH), f"chat-{theme}", quality=86)


def social():
    """The repository's social card, 1280 x 640 as GitHub asks: the name and what it does, large
    enough to read in a link preview, beside the turbofan cut open, in FIG. 1's drafting frame."""
    tf = BLOBS / "turbofan.geoblob"
    r = render([("social-tf", tf, "--az 62 --el 16 --section x0.5 --transparent")], "2000x1400")
    art = Image.open(WORK / "social-tf.png").convert("RGBA")
    art = art.crop(art.getchannel("A").getbbox())
    W, H, m = 1280, 640, 26
    card = Image.new("RGBA", (W, H), PAPER + (255,))
    art = art.resize((590, round(art.height * 590 / art.width)), Image.LANCZOS)
    card.alpha_composite(art, (W - m - 22 - art.width, (H - art.height) // 2 - 6))
    d = ImageDraw.Draw(card)
    d.rectangle((m, m, W - m, H - m), outline=INK, width=3)
    for x, y, sx, sy in ((m, m, -1, -1), (W - m, m, 1, -1), (m, H - m, -1, 1), (W - m, H - m, 1, 1)):
        d.line((x + sx * 8, y, x + sx * 22, y), fill=INK, width=3)
        d.line((x, y + sy * 8, x, y + sy * 22), fill=INK, width=3)
    x0 = 72
    d.text((x0, 156), "GeometryView", font=font("SemiBold", 66), fill=INK)
    for i, line in enumerate(["See inside your CAD models,", "and a whole folder of them", "at once."]):
        d.text((x0, 262 + i * 44), line, font=font("Medium", 30), fill=INK)
    d.text((x0, 432), "STEP · STL", font=font("SemiBold", 22), fill=INK)
    d.text((x0, 466), "Browser · Mac · iPhone · AI apps", font=font("Regular", 22), fill=(111, 104, 92))
    # FIG. 1's title block, small, bottom right.
    rows = [("FIG. 1", "TF-1 TURBOFAN"), ("VIEW", "SECTION A-A")]
    bw, rh = 330, 30
    bx, by = W - m - bw, H - m - rh * len(rows)
    for i, (k, v) in enumerate(rows):
        y = by + i * rh
        d.rectangle((bx, y, W - m, y + rh), outline=INK, width=2, fill=SHEET + (255,))
        d.line((bx + 96, y, bx + 96, y + rh), fill=INK, width=2)
        d.text((bx + 10, y + rh / 2), k, font=font("Medium", 14), fill=INK, anchor="lm")
        d.text((bx + 108, y + rh / 2), v, font=font("SemiBold", 16), fill=INK, anchor="lm")
    path = OUT / "social-card.png"
    card.convert("RGB").save(path, "PNG", optimize=True)
    print(f"  {path.name}: {W}x{H}, {path.stat().st_size / 1e3:.0f} KB")


CAPTURES = {"capture-gallery": capture_gallery, "capture-phone": capture_phone, "capture-mac": capture_mac}
PICTURES = {f.__name__.replace("_", "-"): f for f in
            (renders, fig1, plates, wall, inside, styles, everywhere, chat, social)}

if __name__ == "__main__":
    args = sys.argv[1:]
    if "--out" in args:
        i = args.index("--out")
        OUT = Path(args[i + 1]).resolve()
        del args[i:i + 2]
    if any("designlib" in str(p).lower() for p in (OUT, WORK, SHOWCASE)):
        sys.exit("refusing: the README's pictures come from the showcase, never from designlib")
    WORK.mkdir(parents=True, exist_ok=True)
    OUT.mkdir(parents=True, exist_ok=True)
    jobs = {**CAPTURES, **PICTURES}
    for name in args or PICTURES:
        if name not in jobs:
            sys.exit(f"no job {name}: {', '.join(jobs)}")
        print(name)
        jobs[name]()
