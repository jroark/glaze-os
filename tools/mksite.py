#!/usr/bin/env python3
"""Build glaze-os.com's images (site/img) from emulator screenshots.

The screenshots are the final screens of scripted runs (tests/run.sh writes
out/shots/<name>.png). Capture these, then run this script:

    tests/run.sh tests/t_site_lock.txt 8
    tests/run.sh tests/t_site_lockmid.txt 8
    tests/run.sh tests/t_site_lockdrag.txt 8
    tests/run.sh tests/unlock.txt 10
    tests/run.sh tests/notes.txt 16
    tests/run.sh tests/t_photos.txt 15
    tests/run.sh tests/t_music.txt 13
    tests/run.sh tests/t_browser.txt 11
    tests/run.sh tests/t_calendar.txt 13
    tests/run.sh tests/t_world.txt 12
    NET=1 tests/run.sh tests/t_appstore.txt 30
    tools/mkcfimg.sh out/cf.img 16 16 out/pkg/hello.gzp out/pkg/minesweeper.gzp out/pkg/solitaire.gzp
    CF=out/cf.img tests/run.sh tests/t_minesweeper.txt 34
    CF=out/cf.img tests/run.sh tests/t_solitaire.txt 40
    tools/mksite.py out/shots ../be300/src/ui/assets/be300_frame.png

The hero's lock screen is split into layers so the page can slide its knob:
the screen with an empty track, the knob, the label and the label's alpha
mask (for the shimmer). The empty track comes from two frames with the knob
held at different places (label faded), each supplying the columns the
other's knob hides. Needs Pillow.

usage: mksite.py SHOTS_DIR FRAME_PNG [OUT_DIR]
"""

import sys

from PIL import Image, ImageChops, ImageDraw, ImageFont

# servers/springboard/sb_internal.h
TRACK_X, TRACK_Y, TRACK_W, TRACK_H = 15, 260, 210, 40
KNOB_W, KNOB_H = 50, 36
KNOB_X, KNOB_Y = TRACK_X + 2, TRACK_Y + 2
LABEL = (70, KNOB_Y, 215, KNOB_Y + KNOB_H)     # the label, right of the knob at rest

GALLERY = {
    "home": "unlock", "notes": "notes", "photos": "t_photos", "music": "t_music",
    "browser": "t_browser", "calendar": "t_calendar", "clock": "t_world",
    "appstore": "t_appstore", "minesweeper": "t_minesweeper", "solitaire": "t_solitaire",
}
FRAME_W = 832               # twice the page's 415 px device width
# be300 web/styles.css: the LCD window within the frame artwork.
LCD_LEFT, LCD_TOP, LCD_WIDTH = 0.2109375, 0.1636328, 0.578125


def knob_left(img):
    for x in range(TRACK_X, TRACK_X + TRACK_W):
        if img.getpixel((x, TRACK_Y + TRACK_H // 2))[0] > 150:
            return x - 1
    raise SystemExit("no knob found on the lock screen track")


def lock_layers(shots, out):
    rest = Image.open(f"{shots}/t_site_lock.png").convert("RGB")
    mid = Image.open(f"{shots}/t_site_lockmid.png").convert("RGB")
    end = Image.open(f"{shots}/t_site_lockdrag.png").convert("RGB")
    if knob_left(rest) != KNOB_X:
        raise SystemExit("t_site_lock.png: the knob is not at rest")
    mx, ex = knob_left(mid), knob_left(end)
    if not (LABEL[2] <= ex or mx + KNOB_W <= ex) or mx < LABEL[0]:
        raise SystemExit("the drag frames' knobs overlap; recapture them")

    track = mid.crop((TRACK_X, TRACK_Y, TRACK_X + TRACK_W, TRACK_Y + TRACK_H))
    track.paste(end.crop((mx - 1, TRACK_Y, mx + KNOB_W + 1, TRACK_Y + TRACK_H)), (mx - 1 - TRACK_X, 0))
    base = rest.copy()
    base.paste(track, (TRACK_X, TRACK_Y))
    for img, kx in ((mid, mx), (end, ex)):
        for x in range(TRACK_X, TRACK_X + TRACK_W):
            if kx - 1 <= x <= kx + KNOB_W:
                continue
            for y in range(TRACK_Y, TRACK_Y + TRACK_H):
                if base.getpixel((x, y)) != img.getpixel((x, y)):
                    raise SystemExit(f"empty track differs from a drag frame at {x},{y}; recapture")

    base.save(f"{out}/lock.png", optimize=True)
    rest.crop((KNOB_X, KNOB_Y, KNOB_X + KNOB_W, KNOB_Y + KNOB_H)).save(f"{out}/knob.png", optimize=True)
    label = rest.crop(LABEL)
    label.save(f"{out}/label.png", optimize=True)
    text = ImageChops.subtract(label.convert("L"), base.crop(LABEL).convert("L")).point(lambda v: min(255, v * 3))
    white = Image.new("L", text.size, 255)
    Image.merge("RGBA", (white, white, white, text)).save(f"{out}/label-mask.png", optimize=True)
    return base


def frame_image(frame_png, out):
    frame = Image.open(frame_png).convert("RGBA")
    h = round(frame.height * FRAME_W / frame.width)
    frame.resize((FRAME_W, h), Image.LANCZOS).save(f"{out}/frame.webp", quality=88, method=6)
    return frame


def framed(frame, screen, width):
    """The frame artwork with `screen` in its LCD window, `width` px wide."""
    w, h = frame.size
    lx, ly, lw = round(w * LCD_LEFT), round(h * LCD_TOP), round(w * LCD_WIDTH)
    lh = lw * 320 // 240
    canvas = Image.new("RGBA", frame.size, (0, 0, 0, 0))
    canvas.paste(screen.convert("RGBA").resize((lw, lh), Image.NEAREST), (lx, ly))
    canvas.alpha_composite(frame)
    return canvas.resize((width, round(h * width / w)), Image.LANCZOS)


def pinstripes(size):
    img = Image.new("RGB", size, (0xC5, 0xCC, 0xD4))
    draw = ImageDraw.Draw(img)
    for x in range(2, size[0], 4):
        draw.rectangle((x, 0, x + 1, size[1]), fill=(0xCB, 0xD2, 0xD8))
    return img


def social_card(frame, lock, out, fonts):
    """A 1200x630 Open Graph image: the device and the page's headline."""
    card = pinstripes((1200, 630))
    device = framed(frame, lock, 380)
    card.paste(device, (760, 40), device)
    draw = ImageDraw.Draw(card)
    bold = ImageFont.truetype(f"{fonts}/VeraBd.ttf", 64)
    regular = ImageFont.truetype(f"{fonts}/Vera.ttf", 28)
    y = 150
    for line in ("A new operating", "system for an old", "Casio PDA"):
        draw.text((80, y + 1), line, font=bold, fill=(255, 255, 255))
        draw.text((80, y), line, font=bold, fill=(0, 0, 0))
        y += 76
    draw.text((82, y + 30), "Glaze for the Cassiopeia BE-300", font=regular, fill=(0x4C, 0x56, 0x6C))
    card.save(f"{out}/og.png", optimize=True)


def favicon(out):
    knob = Image.open(f"{out}/knob.png").convert("RGBA")
    icon = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    big = knob.resize((60, 44), Image.LANCZOS)
    icon.paste(big, (2, 10))
    icon.save(f"{out}/../favicon.png", optimize=True)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    shots, frame_png = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else "site/img"
    lock = lock_layers(shots, out)
    for name, shot in GALLERY.items():
        img = Image.open(f"{shots}/{shot}.png").convert("RGB")
        if img.size != (240, 320):
            raise SystemExit(f"{shot}.png is {img.size}, not 240x320")
        img.save(f"{out}/{name}.png", optimize=True)
    frame = frame_image(frame_png, out)
    social_card(frame, lock, out, "assets/fonts")
    favicon(out)


if __name__ == "__main__":
    main()
