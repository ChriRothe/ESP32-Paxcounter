"""
PlatformIO pre-build script: packt web/setup.html (gzip) als C-Array in
include/setup_html.h, damit die Weboberfläche des Einrichtungsmodus in der
Firmware steckt. Läuft automatisch vor jedem Build; die erzeugte Datei
wird mit eingecheckt, damit der Build auch ohne das Script funktioniert.

Manuell:  python tools/embed_web.py
"""
import gzip
import os

try:  # in PlatformIO
    Import("env")  # noqa: F821
    ROOT = env["PROJECT_DIR"]  # noqa: F821
except NameError:  # standalone
    ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SRC = os.path.join(ROOT, "web", "setup.html")
DST = os.path.join(ROOT, "include", "setup_html.h")


def build():
    with open(SRC, "rb") as f:
        raw = f.read()
    gz = gzip.compress(raw, compresslevel=9, mtime=0)
    lines = []
    for i in range(0, len(gz), 20):
        lines.append("  " + ", ".join("0x%02x" % b for b in gz[i:i + 20]) + ",")
    body = (
        "// Automatisch erzeugt aus web/setup.html durch tools/embed_web.py - nicht von Hand ändern!\n"
        "#pragma once\n#include <Arduino.h>\n\n"
        "#define SETUP_HTML_GZ_LEN %d\n" % len(gz)
        + "static const uint8_t SETUP_HTML_GZ[] PROGMEM = {\n" + "\n".join(lines) + "\n};\n"
    )
    old = None
    if os.path.exists(DST):
        with open(DST, "r", encoding="utf-8") as f:
            old = f.read()
    if old != body:
        with open(DST, "w", encoding="utf-8", newline="\n") as f:
            f.write(body)
        print("embed_web: %s aktualisiert (%d -> %d Bytes gzip)" % (os.path.relpath(DST, ROOT), len(raw), len(gz)))


build()
