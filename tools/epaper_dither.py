#!/usr/bin/env python3
"""Gör om ett foto till en bakgrund för reTerminal E1002 (Spectra 6, 800x480).

Fotot beskärs och skalas till 800x480 och ditheras (Floyd–Steinberg) mot skärmens
sex färger. Valet av färg görs mot hur bläcket faktiskt ser ut (dämpade toner),
men filen sparas med rena färger så att ESPHome översätter dem exakt.

    python3 tools/epaper_dither.py foto.jpg display/reterminal-e1002/guestwifi-bg.png
    python3 tools/epaper_dither.py foto.jpg ut.png --lighten 0.35   # ljusare bakgrund

--lighten blandar in vitt så att texten ovanpå syns tydligare (0 = oförändrat).
Kräver Pillow (pip install pillow).
"""
import argparse

from PIL import Image, ImageEnhance, ImageOps

# (ungefär uppmätt bläckfärg, ren färg som skärmdrivrutinen förväntar sig)
PALETTE = [
    ((25, 30, 33), (0, 0, 0)),          # svart
    ((232, 232, 228), (255, 255, 255)),  # vit
    ((235, 215, 60), (255, 255, 0)),     # gul
    ((175, 25, 30), (255, 0, 0)),        # röd
    ((35, 85, 180), (0, 0, 255)),        # blå
    ((40, 100, 50), (0, 255, 0)),        # grön
]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--lighten", type=float, default=0.0, help="0–1, blanda in vitt")
    ap.add_argument("--saturation", type=float, default=1.2, help="färgmättnad före dithering")
    ap.add_argument("--grayscale", action="store_true", help="bara svart/vitt-dithering")
    a = ap.parse_args()

    img = ImageOps.fit(Image.open(a.src).convert("RGB"), (800, 480), Image.LANCZOS)
    if a.lighten > 0:
        img = Image.blend(img, Image.new("RGB", img.size, (255, 255, 255)), a.lighten)
    img = ImageEnhance.Color(img).enhance(0 if a.grayscale else a.saturation)

    colors = PALETTE[:2] if a.grayscale else PALETTE
    pal = Image.new("P", (1, 1))
    flat = [c for measured, _ in colors for c in measured]
    pal.putpalette(flat + [0] * (768 - len(flat)))
    dithered = img.quantize(palette=pal, dither=Image.Dither.FLOYDSTEINBERG)

    # Byt bläckfärgerna mot rena färger (samma index i paletten).
    pure = [c for _, p in colors for c in p]
    dithered.putpalette(pure + [0] * (768 - len(pure)))
    dithered.convert("RGB").save(a.dst)
    print(f"sparade {a.dst} (800x480, {len(colors)} färger)")


if __name__ == "__main__":
    main()
