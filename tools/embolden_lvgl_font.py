#!/usr/bin/env python3
"""Verdickt die Glyphen einer von lv_font_conv erzeugten LVGL-C-Schrift.

Wozu das gut ist
----------------
Für ``assets/StyreneB-Regular.otf`` gibt es keinen fetten Schnitt, und auf dem
System ist auch keiner installiert. Statt Fettdruck vorzutäuschen, indem
dasselbe Etikett mehrfach versetzt gezeichnet wird (das kostet Objekte,
flimmert an den Rändern und sieht trotzdem nicht fett aus), verdickt dieses
Werkzeug die **Bitmaps selbst** — einmalig, zur Übersetzungszeit.

Verfahren: waagerechte Dilatation um genau ein Pixel.

    neu[y][x] = max(alt[y][x-1], alt[y][x])        bei Breite box_w + 1

Der Strich wächst also nach rechts, so wie es echte Fettschnitte tun. Weil die
Werte mit ``max`` verschmolzen werden statt addiert, bleiben die
Antialiasing-Kanten sauber: ein halb gedeckter Randpixel bleibt halb gedeckt,
er wird nur um eine Spalte verbreitert.

Bitmapformat (an der erzeugten Datei nachgemessen, nicht geraten)
-----------------------------------------------------------------
Bei ``--bpp 4`` und ``bitmap_format = 0`` liegt jedes Pixel als 4-Bit-Wert im
Array ``glyph_bitmap``; das höherwertige Nibble kommt zuerst. Die Zeilen sind
**nicht** auf volle Bytes aufgefüllt — der Nibble-Strom läuft ohne Lücke von
einer Zeile in die nächste. Ein Glyph beginnt an ``bitmap_index`` und belegt
``ceil(box_w * box_h * 4 / 8)`` Byte. Nur am Glyphende wird auf ein volles
Byte aufgefüllt.

Nachgerechnet an der 24-px-Styrene: „%“ hat box_w=22, box_h=17 und liegt bei
Index 0, das nächste Glyph bei 187 — und ceil(22*17*4/8) ist genau 187.

Einheit von adv_w
-----------------
``adv_w`` steht in **1/16 Pixel**, nicht in ganzen Pixeln (die Ziffer „0“ hat
adv_w=237 bei box_w=13, also 14,8 px). Ein Pixel mehr Vorschub sind deshalb
+16 Einheiten, nicht +1 — siehe ``ADV_PRO_PIXEL``. ``ofs_x`` bleibt
unverändert, weil der Strich nach rechts wächst.

Senkrechte Maße
---------------
``line_height`` und ``base_line`` richten sich danach, welche Zeichen in der
Schrift stecken. Eine Teilmenge aus Ziffern und Prozentzeichen hat weder
Ober- noch Unterlängen und bekommt deshalb andere Werte als die vollständige
Schrift (bei der 24-px-Styrene 17/0 statt 31/6). Wer die verdickte Fassung
gegen die normale austauscht, bekommt damit einen senkrechten Versatz — das
Etikett ist plötzlich 14 px flacher und die Ziffern sitzen woanders.

``--vertikal-wie REFERENZ.c`` übernimmt die beiden Werte aus der vollständigen
Schrift, damit der Austausch rein optisch wirkt und nichts verrutscht. Die
Glyphen selbst bleiben davon unberührt (``ofs_y`` und ``box_h`` sind in beiden
Fassungen identisch).

Aufruf
------
    python3 tools/embolden_lvgl_font.py EINGABE.c -o AUSGABE.c \
        [--name font_styrene_24_fett] \
        [--vertikal-wie firmware/src/font_styrene_24.c] [--demo 8]

``--demo ZEICHEN`` gibt das betroffene Glyph vor und nach der Verdickung als
ASCII-Bild aus — der Beleg, dass das Entpacken stimmt.

Die Datei wird anschließend noch für LVGL 9 nachgezogen (siehe docs/fonts.md);
das erledigt ein eigenes Skript, nicht dieses hier.
"""

from __future__ import annotations

import argparse
import math
import pathlib
import re
import sys

# adv_w wird von lv_font_conv in 1/16 Pixel abgelegt.
ADV_PRO_PIXEL = 16

# Nibble (0..15) -> Zeichen, dunkel nach hell, für die ASCII-Vorschau.
GRAUSTUFEN = " .:-=+*oO0#@@@@@"[:16]

# Ein Pixel belegt bei bpp=4 genau ein Nibble.
BITS_PRO_PIXEL = 4


class Glyph:
    """Ein Eintrag aus glyph_dsc samt entpacktem Bitmap."""

    def __init__(self, bitmap_index, adv_w, box_w, box_h, ofs_x, ofs_y):
        self.bitmap_index = bitmap_index
        self.adv_w = adv_w
        self.box_w = box_w
        self.box_h = box_h
        self.ofs_x = ofs_x
        self.ofs_y = ofs_y
        self.pixel: list[list[int]] = []  # [zeile][spalte] -> 0..15


def byte_laenge(box_w: int, box_h: int) -> int:
    """Bytebedarf eines Glyphs: Nibbles fortlaufend, nur am Ende aufgefüllt."""
    return math.ceil(box_w * box_h * BITS_PRO_PIXEL / 8)


def block_ausschneiden(text: str, kopf: str) -> tuple[int, int]:
    """Liefert Start und Ende des Rumpfs einer Array-Initialisierung.

    ``kopf`` ist ein regulärer Ausdruck, der bis zur öffnenden Klammer passt.
    Zurück kommen die Indizes direkt nach ``{`` und direkt vor dem ``}``, das
    die Klammer schließt (Verschachtelung wird mitgezählt).
    """
    treffer = re.search(kopf, text)
    if not treffer:
        raise SystemExit(f"Array nicht gefunden: {kopf}")
    start = text.index("{", treffer.start()) + 1
    tiefe = 1
    i = start
    while i < len(text) and tiefe:
        if text[i] == "{":
            tiefe += 1
        elif text[i] == "}":
            tiefe -= 1
        i += 1
    if tiefe:
        raise SystemExit(f"Klammer nicht geschlossen: {kopf}")
    return start, i - 1


def bitmap_lesen(text: str) -> list[int]:
    """Alle Bytes aus glyph_bitmap[] einlesen."""
    a, e = block_ausschneiden(text, r"uint8_t\s+glyph_bitmap\s*\[\s*\]\s*=")
    return [int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]+)", text[a:e])]


def glyphen_lesen(text: str) -> list[Glyph]:
    """Alle Einträge aus glyph_dsc[] einlesen, Reihenfolge bleibt erhalten."""
    a, e = block_ausschneiden(
        text, r"lv_font_fmt_txt_glyph_dsc_t\s+glyph_dsc\s*\[\s*\]\s*="
    )
    muster = re.compile(
        r"\.bitmap_index\s*=\s*(-?\d+)\s*,\s*"
        r"\.adv_w\s*=\s*(-?\d+)\s*,\s*"
        r"\.box_w\s*=\s*(-?\d+)\s*,\s*"
        r"\.box_h\s*=\s*(-?\d+)\s*,\s*"
        r"\.ofs_x\s*=\s*(-?\d+)\s*,\s*"
        r"\.ofs_y\s*=\s*(-?\d+)"
    )
    glyphen = [Glyph(*map(int, m.groups())) for m in muster.finditer(text[a:e])]
    if not glyphen:
        raise SystemExit("glyph_dsc[] enthält keine lesbaren Einträge.")
    return glyphen


def unicode_je_glyph(text: str) -> dict[int, int]:
    """Aus den cmaps eine Zuordnung Glyph-ID -> Unicode bauen.

    Nur für die Kommentare im Ausgabearray und für ``--demo`` nötig. Es werden
    ausschließlich die lückenlosen FORMAT0-Bereiche ausgewertet; alles andere
    bleibt ohne Kommentar, was die Schrift nicht beeinträchtigt.
    """
    a, e = block_ausschneiden(text, r"lv_font_fmt_txt_cmap_t\s+cmaps\s*\[\s*\]\s*=")
    zuordnung: dict[int, int] = {}
    muster = re.compile(
        r"\.range_start\s*=\s*(\d+)\s*,\s*"
        r"\.range_length\s*=\s*(\d+)\s*,\s*"
        r"\.glyph_id_start\s*=\s*(\d+)"
    )
    for m in muster.finditer(text[a:e]):
        start, laenge, gid = map(int, m.groups())
        for i in range(laenge):
            zuordnung[gid + i] = start + i
    return zuordnung


def entpacken(bytes_: list[int], g: Glyph) -> list[list[int]]:
    """Ein Glyph aus dem fortlaufenden Nibble-Strom in ein 2D-Feld holen."""
    if g.box_w <= 0 or g.box_h <= 0:
        return []
    noetig = byte_laenge(g.box_w, g.box_h)
    roh = bytes_[g.bitmap_index : g.bitmap_index + noetig]
    if len(roh) < noetig:
        raise SystemExit(
            f"glyph_bitmap zu kurz: ab {g.bitmap_index} fehlen "
            f"{noetig - len(roh)} Byte."
        )
    nibbles: list[int] = []
    for b in roh:
        nibbles.append((b >> 4) & 0xF)
        nibbles.append(b & 0xF)
    return [
        nibbles[y * g.box_w : (y + 1) * g.box_w] for y in range(g.box_h)
    ]


def verdicken(pixel: list[list[int]], box_w: int) -> list[list[int]]:
    """Waagerechte Dilatation um ein Pixel: neu[x] = max(alt[x-1], alt[x])."""
    neu = []
    for zeile in pixel:
        breiter = []
        for x in range(box_w + 1):
            links = zeile[x - 1] if 1 <= x <= box_w else 0
            hier = zeile[x] if x < box_w else 0
            breiter.append(max(links, hier))
        neu.append(breiter)
    return neu


def packen(pixel: list[list[int]]) -> list[int]:
    """2D-Feld wieder in den fortlaufenden Nibble-Strom schreiben."""
    nibbles = [p for zeile in pixel for p in zeile]
    if len(nibbles) % 2:
        nibbles.append(0)  # nur am Glyphende auffüllen
    return [
        (nibbles[i] << 4) | nibbles[i + 1] for i in range(0, len(nibbles), 2)
    ]


def als_ascii(pixel: list[list[int]]) -> str:
    return "\n".join(
        "|" + "".join(GRAUSTUFEN[p] for p in zeile) + "|" for zeile in pixel
    )


def array_formatieren(bytes_: list[int], glyphen: list[Glyph],
                      unicodes: dict[int, int]) -> str:
    """Bytes mit denselben Glyph-Kommentaren ausgeben wie lv_font_conv."""
    beginnt_bei = {
        g.bitmap_index: gid
        for gid, g in enumerate(glyphen)
        if g.box_w > 0 and g.box_h > 0
    }
    zeilen: list[str] = []
    puffer: list[str] = []

    def puffer_leeren():
        for i in range(0, len(puffer), 8):
            zeilen.append("    " + " ".join(puffer[i : i + 8]))
        puffer.clear()

    for i, b in enumerate(bytes_):
        gid = beginnt_bei.get(i)
        if gid is not None:
            puffer_leeren()
            cp = unicodes.get(gid)
            if cp is not None:
                zeichen = chr(cp)
                zeilen.append(f'\n    /* U+{cp:04X} "{zeichen}" */')
            else:
                zeilen.append(f"\n    /* glyph id {gid} */")
        puffer.append(f"0x{b:x},")
    puffer_leeren()
    # Das letzte Komma stört C nicht, aber sauber ist sauber.
    if zeilen:
        zeilen[-1] = zeilen[-1].rstrip(",")
    return "\n".join(zeilen) + "\n"


def glyph_dsc_formatieren(glyphen: list[Glyph]) -> str:
    zeilen = []
    for gid, g in enumerate(glyphen):
        rest = " /* id = 0 reserved */" if gid == 0 else ""
        zeilen.append(
            f"    {{.bitmap_index = {g.bitmap_index}, .adv_w = {g.adv_w}, "
            f".box_w = {g.box_w}, .box_h = {g.box_h}, "
            f".ofs_x = {g.ofs_x}, .ofs_y = {g.ofs_y}}}{rest},"
        )
    zeilen[-1] = zeilen[-1].rstrip(",")
    return "\n" + "\n".join(zeilen) + "\n"


def ersetzen(text: str, kopf: str, inhalt: str) -> str:
    a, e = block_ausschneiden(text, kopf)
    return text[:a] + inhalt + text[e:]


def senkrechte_masse_lesen(pfad: pathlib.Path) -> tuple[int, int]:
    """line_height und base_line aus einer Referenzschrift holen."""
    ref = pfad.read_text()
    lh = re.search(r"\.line_height\s*=\s*(-?\d+)", ref)
    bl = re.search(r"\.base_line\s*=\s*(-?\d+)", ref)
    if not lh or not bl:
        raise SystemExit(f"line_height/base_line nicht gefunden in {pfad}")
    return int(lh.group(1)), int(bl.group(1))


def senkrechte_masse_setzen(text: str, line_height: int, base_line: int) -> str:
    text = re.sub(r"\.line_height\s*=\s*-?\d+", f".line_height = {line_height}", text)
    return re.sub(r"\.base_line\s*=\s*-?\d+", f".base_line = {base_line}", text)


def symbol_umbenennen(text: str, neu: str) -> str:
    """Symbolnamen der Schrift (und den Include-Guard) umschreiben."""
    treffer = re.search(r"lv_font_t\s+(\w+)\s*=\s*\{", text)
    if not treffer:
        raise SystemExit("Symbolname der Schrift nicht gefunden.")
    alt = treffer.group(1)
    if alt == neu:
        return text
    return text.replace(alt, neu).replace(alt.upper(), neu.upper())


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("eingabe", type=pathlib.Path)
    p.add_argument("-o", "--ausgabe", type=pathlib.Path, required=True)
    p.add_argument("--name", help="neuer Symbolname der Schrift")
    p.add_argument("--vertikal-wie", dest="vertikal_wie", type=pathlib.Path,
                   help="line_height/base_line aus dieser Schrift übernehmen")
    p.add_argument("--demo", help="ein Zeichen vor/nach der Verdickung zeigen")
    args = p.parse_args()

    text = args.eingabe.read_text()
    bytes_alt = bitmap_lesen(text)
    glyphen = glyphen_lesen(text)
    unicodes = unicode_je_glyph(text)

    # 1. Alle Glyphen entpacken — noch ohne etwas zu ändern.
    for g in glyphen:
        g.pixel = entpacken(bytes_alt, g)

    demo_vorher = demo_nachher = None
    demo_gid = None
    if args.demo:
        cp = ord(args.demo)
        for gid, u in unicodes.items():
            if u == cp:
                demo_gid = gid
                break
        if demo_gid is None:
            raise SystemExit(f"Zeichen {args.demo!r} ist in dieser Schrift nicht enthalten.")
        demo_vorher = als_ascii(glyphen[demo_gid].pixel)

    # 2. Verdicken, Maße nachziehen, neu packen, bitmap_index fortschreiben.
    bytes_neu: list[int] = []
    for gid, g in enumerate(glyphen):
        if not g.pixel:
            g.bitmap_index = len(bytes_neu)
            continue
        g.pixel = verdicken(g.pixel, g.box_w)
        g.box_w += 1
        g.adv_w += ADV_PRO_PIXEL
        g.bitmap_index = len(bytes_neu)
        bytes_neu.extend(packen(g.pixel))

    if demo_gid is not None:
        demo_nachher = als_ascii(glyphen[demo_gid].pixel)

    # 3. Gegenprobe: erwartete Länge je Glyph gegen tatsächliche Indizes.
    for gid, g in enumerate(glyphen):
        if not g.pixel:
            continue
        erwartet = byte_laenge(g.box_w, g.box_h)
        naechster = next(
            (h.bitmap_index for h in glyphen[gid + 1:] if h.pixel), len(bytes_neu)
        )
        if naechster - g.bitmap_index != erwartet:
            raise SystemExit(
                f"Glyph {gid}: Indexabstand {naechster - g.bitmap_index} "
                f"passt nicht zu erwarteten {erwartet} Byte."
            )

    # 4. Datei neu schreiben.
    text = ersetzen(text, r"uint8_t\s+glyph_bitmap\s*\[\s*\]\s*=",
                    array_formatieren(bytes_neu, glyphen, unicodes))
    text = ersetzen(text, r"lv_font_fmt_txt_glyph_dsc_t\s+glyph_dsc\s*\[\s*\]\s*=",
                    glyph_dsc_formatieren(glyphen))
    if args.vertikal_wie:
        lh, bl = senkrechte_masse_lesen(args.vertikal_wie)
        text = senkrechte_masse_setzen(text, lh, bl)
        print(f"senkrechte Maße aus {args.vertikal_wie.name}: "
              f"line_height={lh}, base_line={bl}")
    if args.name:
        text = symbol_umbenennen(text, args.name)

    args.ausgabe.write_text(text)

    if demo_vorher is not None:
        print(f"--- {args.demo!r} vorher ({len(demo_vorher.splitlines()[0]) - 2} px breit) ---")
        print(demo_vorher)
        print(f"--- {args.demo!r} nachher ({len(demo_nachher.splitlines()[0]) - 2} px breit) ---")
        print(demo_nachher)

    print(
        f"geschrieben: {args.ausgabe}  "
        f"({len(bytes_alt)} -> {len(bytes_neu)} Byte Bitmapdaten, "
        f"{sum(1 for g in glyphen if g.pixel)} Glyphen verdickt)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
