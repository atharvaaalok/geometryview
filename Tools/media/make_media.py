#!/usr/bin/env python3
"""Makes the README's pictures and animations, from the sample models, into Docs/media.

Every picture is a real render: the native engine's (`gcli render`) for the stills and the
animations of a model, and the browser viewer's (Tools/media/page.mjs, in headless Chrome) for
see-through and for the viewer itself. Animations are animated WebP, a third of a GIF's size.

    python3 Tools/media/make_media.py [names...]     all of them, or only those named

Needs the release build (gcli), Launcher/dist/site from `node Launcher/scripts/site.mjs`, Pillow,
and Google Chrome. The Mac app's window is pictured by Tools/media/mac_window.sh, separately,
since it runs the app.

Sizes follow one rule. Each picture is drawn about four times the width the README shows it at,
then shrunk: stills to twice that width, for sharp screens, and animations to one and a half
times, which keeps each within a few megabytes. Lines are drawn at a fixed width in points, so
drawing large makes them weigh what they do in a full window seen at the README's size, as in the
picture of the Mac app; drawn at twice the README's size, they come out twice as heavy.
"""

from __future__ import annotations

import functools
import http.server
import json
import math
import mimetypes
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build" / "release"
SITE = ROOT / "Launcher" / "dist" / "site"
SAMPLES = SITE / "samples"
OUT = ROOT / "Docs" / "media"
BLOBS = ROOT / "build" / "media" / "blobs"
FONT = ROOT / "ThirdParty" / "ibm-plex-mono" / "IBMPlexMono-Medium.ttf"

ILLUSTRATION, METAL, CLAY, BLUEPRINT, TECHNICAL, STUDIO = range(6)
PARTS = 1


def blob(sample: str) -> Path:
    """The sample's cache file, imported once."""
    out = BLOBS / f"{Path(sample).stem}.geoblob"
    if not out.exists():
        BLOBS.mkdir(parents=True, exist_ok=True)
        subprocess.run([BUILD / "Tools/gcli", "import", SAMPLES / sample, out,
                        BUILD / "Import/geometryview-occt-worker"], check=True, capture_output=True)
    return out


def render(model: Path, out: Path, *, size=(1200, 900), az=35.0, el=22.0, style=ILLUSTRATION,
           section="", isolate="", perspective=False) -> Path:
    args = [BUILD / "Tools/gcli", "render", BUILD / "shaders/default.metallib", model, out,
            str(size[0]), str(size[1]), f"{az:.3f}", f"{el:.3f}",
            "perspective" if perspective else "ortho", str(style), str(PARTS), section or "none", "plain"]
    if isolate:
        args.append(isolate)
    subprocess.run(args, check=True, capture_output=True)
    return out


def load(frame) -> Image.Image:
    """A frame, given as an image or as the path of one, which is read only now."""
    return Image.open(frame).convert("RGB") if isinstance(frame, Path) else frame


def content_box(images: list, pad: int) -> tuple[int, int, int, int]:
    """The union of what differs from each image's backdrop (its top-left corner), padded."""
    box = None
    for image in map(load, images):
        backdrop = image.crop((0, 0, 32, 32)).resize((1, 1), Image.Resampling.BOX).getpixel((0, 0))
        diff = ImageChops.difference(image, Image.new("RGB", image.size, backdrop)).convert("L")
        found = diff.filter(ImageFilter.MaxFilter(5)).point(lambda v: 255 if v > 40 else 0).getbbox()
        if found:
            box = found if box is None else (min(box[0], found[0]), min(box[1], found[1]),
                                             max(box[2], found[2]), max(box[3], found[3]))
    w, h = load(images[0]).size
    return (max(0, box[0] - pad), max(0, box[1] - pad), min(w, box[2] + pad), min(h, box[3] + pad))


def fit(image: Image.Image, width: int) -> Image.Image:
    return image.resize((width, round(image.height * width / image.width)), Image.Resampling.LANCZOS)


def shrink(image: Image.Image, box, width: int, name: str) -> Image.Image:
    """The image cropped to `box` and fitted to `width`, saying how much it shrank (see the top)."""
    print(f"{name}: drawn {box[2] - box[0]} px wide, saved {width}: x{width / (box[2] - box[0]):.2f}")
    return fit(image.crop(box), width)


def save_animation(frames: list, out: Path, width: int, ms: int, quality=80, pad=56):
    """Frames, as images or paths, cropped to what changes in them and fitted to `width`."""
    box = content_box(frames, pad=pad)
    frames = [shrink(load(frames[0]), box, width, out.name)] + [fit(load(frame).crop(box), width)
                                                               for frame in frames[1:]]
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=ms, loop=0,
                   quality=quality, method=6)
    print(f"{out.relative_to(ROOT)}: {len(frames)} frames, {out.stat().st_size / 1e6:.2f} MB")


def ease(t: float) -> float:
    return 0.5 - 0.5 * math.cos(math.pi * min(1.0, max(0.0, t)))


def frames_of(scratch: Path, name: str, shots: list[dict], model: Path) -> list[Path]:
    """The shots rendered, as the paths of their pictures: large frames are read one at a time."""
    return [render(model, scratch / f"{name}{i:03d}.png", **shot) for i, shot in enumerate(shots)]


# ---- The pictures --------------------------------------------------------------------------------

def hero(scratch: Path):
    """The drive cut open as the view swings: the light page's in Illustration, the dark's in Studio."""
    model = blob("geared-drive.step")
    count = 84
    shots = []
    for i in range(count):
        t = i / count
        # Opens over the first fifth, stays open, closes over the last fifth: a seamless loop.
        opened = ease(t / 0.2) if t < 0.5 else ease((1 - t) / 0.2)
        cut = 1.0 - 0.5 * opened
        shots.append({"az": 62 + 26 * math.sin(2 * math.pi * t), "el": 22, "size": (4200, 3150),
                      "section": f"x{cut:.4f}" if cut < 0.999 else ""})
    for style, name in [(ILLUSTRATION, "hero-light"), (STUDIO, "hero-dark")]:
        frames = frames_of(scratch, name, [{**s, "style": style} for s in shots], model)
        # The paper's grain costs the light one more: a little less quality keeps it near 2 MB.
        save_animation(frames, OUT / f"{name}.webp", width=840, ms=45, quality=72 if style == ILLUSTRATION else 80,
                       pad=100)


def styles(scratch: Path):
    """The six drawing styles, on the drive cut open: a 3 by 2 grid, each tile labelled."""
    model = blob("geared-drive.step")
    names = ["Illustration", "Metal", "Clay", "Blueprint", "Technical", "Studio"]
    images = [Image.open(render(model, scratch / f"style{s}.png", style=s, section="x0.5",
                                size=(2700, 2160))).convert("RGB") for s in range(6)]
    box = content_box(images, pad=110)
    tile_w = 560
    tiles = [shrink(image, box, tile_w, f"styles {name}") for image, name in zip(images, names)]
    tile_h = tiles[0].height
    sheet = Image.new("RGB", (tile_w * 3, tile_h * 2))
    font = ImageFont.truetype(str(FONT), 24)
    for i, (tile, name) in enumerate(zip(tiles, names)):
        x, y = (i % 3) * tile_w, (i // 3) * tile_h
        sheet.paste(tile, (x, y))
        draw = ImageDraw.Draw(sheet)
        tw = draw.textlength(name, font=font)
        chip = (x + 20, y + 20, x + 20 + tw + 32, y + 20 + 46)
        draw.rounded_rectangle(chip, radius=23, fill=(247, 245, 238), outline=(35, 30, 22), width=3)
        draw.text((chip[0] + 16, chip[1] + 7), name, font=font, fill=(35, 30, 22))
    out = OUT / "styles.jpg"
    sheet.save(out, quality=86, optimize=True, progressive=True)
    print(f"{out.relative_to(ROOT)}: {out.stat().st_size / 1e6:.2f} MB")


def section(scratch: Path):
    """A cut gliding down through the gearbox, resting at each of its three planetary stages, then
    rising back."""
    model = blob("planetary-gearbox.step")
    # (where the cut goes, frames to get there, frames resting there)
    moves = [(0.525, 26, 9), (0.43, 10, 9), (0.262, 14, 14), (0.985, 24, 6)]
    cuts, at = [], 0.985
    for to, frames, rest in moves:
        cuts += [at + (to - at) * ease((i + 1) / frames) for i in range(frames)] + [to] * rest
        at = to
    shots = [{"az": 35, "el": 48, "section": f"z{cut:.4f}", "size": (3900, 3900)} for cut in cuts]
    save_animation(frames_of(scratch, "section", shots, model), OUT / "section.webp", width=630, ms=45,
                   pad=110)


def agent(scratch: Path):
    """What an AI app gets back: the gearbox's second stage alone, and the whole of it cut in half,
    from the tool server's own render_view, called as an AI app calls it."""
    calls = [
        {"name": "open_model", "arguments": {"path": str(SAMPLES / "planetary-gearbox.step")}},
        {"name": "get_structure", "arguments": {"path": str(SAMPLES / "planetary-gearbox.step"), "filter": "stage2"}},
        {"name": "render_view", "arguments": {"path": str(SAMPLES / "planetary-gearbox.step"),
                                              "isolate": ["stage2"], "azimuth": 35, "elevation": -32,
                                              "width": 2700, "height": 2700}},
        {"name": "render_view", "arguments": {"path": str(SAMPLES / "planetary-gearbox.step"),
                                              "section": [{"axis": "x", "at": 0.5}], "style": "metal",
                                              "azimuth": 35, "elevation": 22, "width": 3400, "height": 3400}},
    ]
    results = mcp(calls)
    print("get_structure:", results[1]["content"][0]["text"][:400].replace("\n", " | "))
    # Saved at twice the README's widths, cropped to the model: the view cube and scale bar the tool
    # draws for the AI, at the right, are left out.
    for result, name, width in zip(results[2:], ["agent-stage2", "agent-cut"], [760, 600]):
        data = next(c for c in result["content"] if c["type"] == "image")["data"]
        path = scratch / f"{name}.png"
        path.write_bytes(__import__("base64").b64decode(data))
        image = Image.open(path).convert("RGB")
        image = shrink(image, content_box([without_overlay(image)], pad=190), width, name)
        out = OUT / f"{name}.jpg"
        image.save(out, quality=86, optimize=True, progressive=True)
        print(f"{out.relative_to(ROOT)}: {out.stat().st_size / 1e6:.2f} MB")


def without_overlay(image: Image.Image) -> Image.Image:
    """The picture with its top and bottom right corners, where the view cube and the scale bar are
    drawn (2x), painted over in its backdrop, to find the model alone."""
    plain = image.copy()
    backdrop = image.crop((0, 0, 32, 32)).resize((1, 1), Image.Resampling.BOX).getpixel((0, 0))
    draw = ImageDraw.Draw(plain)
    draw.rectangle((image.width - 300, 0, image.width, 300), fill=backdrop)
    draw.rectangle((image.width - 300, image.height - 300, image.width, image.height), fill=backdrop)
    return plain


def mcp(calls: list[dict]) -> list[dict]:
    """Starts the tool server as an AI app does, initialises it and makes the calls, in turn."""
    server = subprocess.Popen(["node", str(ROOT / "Launcher/bin/geometryview.mjs"), "--mcp"],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def ask(id_, method, params):
        server.stdin.write(json.dumps({"jsonrpc": "2.0", "id": id_, "method": method, "params": params}) + "\n")
        server.stdin.flush()
        while True:
            message = json.loads(server.stdout.readline())
            if message.get("id") == id_:
                if "error" in message or message["result"].get("isError"):
                    raise RuntimeError(f"{method} failed: {message}")
                return message["result"]

    try:
        ask(0, "initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "make_media", "version": "1"}})
        return [ask(i + 1, "tools/call", call) for i, call in enumerate(calls)]
    finally:
        server.terminate()


# ---- The browser viewer ----------------------------------------------------------------------------

class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def serve_site() -> tuple[http.server.ThreadingHTTPServer, str]:
    """Serves the hosted site as GitHub Pages would: plain files, no isolation headers."""
    mimetypes.add_type("application/javascript", ".mjs")
    mimetypes.add_type("application/wasm", ".wasm")
    mimetypes.add_type("application/manifest+json", ".webmanifest")
    handler = functools.partial(Quiet, directory=str(SITE))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, f"http://127.0.0.1:{server.server_address[1]}/"


def page(steps: list[dict]) -> dict:
    result = subprocess.run(["node", str(ROOT / "Tools/media/page.mjs"), json.dumps(steps)],
                            capture_output=True, text=True)
    report = json.loads(result.stdout)
    if result.returncode != 0 or report.get("errors"):
        raise RuntimeError(f"page.mjs: {report}")
    return report


OPENED = ("(() => { const s = document.querySelector('.masthead .subtitle'); return !!s && "
          "s.textContent.includes('bodies') && !document.querySelector('.masthead .progress:not([hidden])'); })()")


def see_through(scratch: Path):
    """T in the browser viewer: the drive's case fades to reveal the mechanism, then returns."""
    server, url = serve_site()
    try:
        # A large window at one pixel per point: the model is drawn large, for the lines' weight,
        # and frames are quick enough to catch the fade in 20 or more.
        steps = [{"size": [1800, 1250, 1]}, {"go": f"{url}?sample=drive&plain"},
                 {"until": OPENED, "timeout": 120}, {"wait": 1500},
                 {"eval": "document.querySelectorAll('.card, .toolbar, .masthead')"
                          ".forEach((e) => e.style.visibility = 'hidden')"},
                 {"wait": 300}]
        for i, key in enumerate(["t", "t"]):
            steps += [{"shots": str(scratch / f"see{i}-"), "every": 0, "until": "true", "jpeg": True},
                      {"key": key},
                      {"eval": "window.__t = performance.now()"},
                      {"shots": str(scratch / f"see{i}-run-"), "every": 30, "jpeg": True,
                       "until": "performance.now() > window.__t + 900", "timeout": 10},
                      {"wait": 900}]
        page(steps)
    finally:
        server.shutdown()
    fade_in = sorted(scratch.glob("see0-run-*.jpg"))
    fade_out = sorted(scratch.glob("see1-run-*.jpg"))
    print(f"see-through: {len(fade_in)} frames fading in, {len(fade_out)} out")
    closed = Image.open(scratch / "see0-0000.jpg").convert("RGB")
    opened = Image.open(fade_in[-1]).convert("RGB")
    frames = ([closed] * 16 + [Image.open(f).convert("RGB") for f in fade_in] + [opened] * 24 +
              [Image.open(f).convert("RGB") for f in fade_out])
    save_animation(frames, OUT / "see-through.webp", width=630, ms=45)


def viewer(scratch: Path):
    """The hosted viewer's first page, its samples offered; and the gearbox on a phone."""
    server, url = serve_site()
    try:
        page([{"size": [1440, 900, 2]}, {"go": url},
              {"until": "crossOriginIsolated && !!document.querySelector('.status .sample')", "timeout": 30},
              {"wait": 800}, {"shot": str(scratch / "browser.png")}])
        page([{"size": [390, 844, 3], "phone": True}, {"go": f"{url}?sample=gearbox&plain"},
              {"until": OPENED, "timeout": 120}, {"wait": 1500}, {"shot": str(scratch / "phone.png")}])
    finally:
        server.shutdown()
    browser = Image.open(scratch / "browser.png").convert("RGB")
    browser = fit(browser, 1760)
    out = OUT / "browser.jpg"
    browser.save(out, quality=84, optimize=True, progressive=True)
    print(f"{out.relative_to(ROOT)}: {out.stat().st_size / 1e6:.2f} MB")
    (scratch / "phone.png").replace(OUT.parent.parent / "build" / "media" / "phone.png")


def rounded(image: Image.Image, radius: int, border: int = 0, colour=(35, 30, 22)) -> Image.Image:
    """The image with rounded corners (an alpha channel), and a border drawn inside its edge."""
    image = image.convert("RGBA")
    mask = Image.new("L", image.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, image.width - 1, image.height - 1), radius, fill=255)
    image.putalpha(mask)
    if border:
        ImageDraw.Draw(image).rounded_rectangle((0, 0, image.width - 1, image.height - 1), radius,
                                                outline=colour + (255,), width=border)
    return image


def everywhere(scratch: Path):
    """The Mac app's window, with the same viewer on a phone in front of it. Needs build/media's
    mac.png (Tools/media/mac_window.sh) and phone.png (the viewer job)."""
    media = ROOT / "build" / "media"
    k = 1760 / 1240  # laid out at 1240 wide, saved at 1760
    mac = Image.open(media / "mac.png").convert("RGB")
    mac = rounded(shrink(mac, (0, 0, mac.width, mac.height), round(1100 * k), "mac window"), round(20 * k), 4)
    # The phone's screen inside a frame of its own, so the frame covers none of the screen.
    screen = rounded(fit(Image.open(media / "phone.png").convert("RGB"), round(254 * k)), round(28 * k))
    rim = round(8 * k)
    phone = Image.new("RGBA", (screen.width + 2 * rim, screen.height + 2 * rim), (0, 0, 0, 0))
    ImageDraw.Draw(phone).rounded_rectangle((0, 0, phone.width - 1, phone.height - 1), round(34 * k),
                                            fill=(35, 30, 22, 255))
    phone.alpha_composite(screen, (rim, rim))
    sheet = Image.new("RGBA", (1760, mac.height + round(110 * k)), (0, 0, 0, 0))
    shadow = Image.new("RGBA", sheet.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(shadow)
    phone_at = (sheet.width - phone.width - round(10 * k), sheet.height - phone.height - round(10 * k))
    draw.rounded_rectangle((round(10 * k), round(18 * k), round(10 * k) + mac.width, round(18 * k) + mac.height),
                           round(20 * k), fill=(0, 0, 0, 70))
    draw.rounded_rectangle((phone_at[0] + round(8 * k), phone_at[1] + round(10 * k), phone_at[0] + round(8 * k) + phone.width,
                            phone_at[1] + round(10 * k) + phone.height), round(34 * k), fill=(0, 0, 0, 90))
    sheet = Image.alpha_composite(sheet, shadow.filter(ImageFilter.GaussianBlur(20)))
    sheet.alpha_composite(mac, (0, 0))
    sheet.alpha_composite(phone, phone_at)
    out = OUT / "everywhere.webp"
    sheet.save(out, quality=86, method=6)
    print(f"{out.relative_to(ROOT)}: {out.stat().st_size / 1e6:.2f} MB")


def social(scratch: Path):
    """The card shown where the repository's link is shared, 1280 by 640: the drive cut open, the
    name and what it is. Uploaded by hand, in the repository's settings; written to build/media."""
    model = blob("geared-drive.step")
    figure = Image.open(render(model, scratch / "social.png", size=(2000, 1700), az=62, el=22,
                               section="x0.5")).convert("RGB")
    figure = figure.crop(content_box([figure], pad=60))
    figure.thumbnail((560, 600), Image.Resampling.LANCZOS)
    card = Image.new("RGB", (1280, 640), (236, 232, 222))
    # The render's own paper, from a corner the model is far from, tiled.
    paper = Image.open(scratch / "social.png").convert("RGB").crop((0, 0, 320, 160))
    for x in range(0, 1280, 320):
        for y in range(0, 640, 160):
            card.paste(paper, (x, y))
    card.paste(figure, (1280 - figure.width - 70, (640 - figure.height) // 2))
    draw = ImageDraw.Draw(card)
    ink = (35, 30, 22)
    bold = ImageFont.truetype(str(FONT.with_name("IBMPlexMono-SemiBold.ttf")), 66)
    text = ImageFont.truetype(str(FONT), 27)
    draw.text((80, 190), "GeometryView", font=bold, fill=ink)
    for i, line in enumerate(["Fast, beautiful STEP and STL viewing", "on a Mac, in your browser, on a phone,",
                              "and in your AI apps."]):
        draw.text((82, 300 + i * 42), line, font=text, fill=(80, 72, 60))
    out = ROOT / "build" / "media" / "social.jpg"
    card.save(out, quality=90)
    print(f"{out.relative_to(ROOT)}: {out.stat().st_size / 1e6:.2f} MB")


JOBS = {"hero": hero, "styles": styles, "section": section, "agent": agent,
        "see-through": see_through, "viewer": viewer, "everywhere": everywhere,
        "social": social}

if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    (ROOT / "build" / "media").mkdir(parents=True, exist_ok=True)
    chosen = sys.argv[1:] or list(JOBS)
    for name in chosen:
        with tempfile.TemporaryDirectory() as scratch:
            JOBS[name](Path(scratch))
