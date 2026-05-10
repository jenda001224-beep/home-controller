#!/usr/bin/env python3
"""
make_flasher.py — run from project root after `pio run`

  python tools/make_flasher.py          # build + serve + open browser
  python tools/make_flasher.py --build  # build only, no server
"""
import base64, json, os, sys, glob, webbrowser, threading, argparse
import http.server, socketserver

ROOT     = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD    = os.path.join(ROOT, ".pio", "build", "tdisplay-s3-pro")
TEMPLATE = os.path.join(ROOT, "tools", "flasher_template.html")
OUTPUT   = os.path.join(ROOT, "homecontroller-flash.html")
PORT     = 8099

PARTS = [
    ("bootloader.bin", 0x0000,  BUILD),
    ("partitions.bin", 0x8000,  BUILD),
    ("boot_app0.bin",  0xE000,  None),
    ("firmware.bin",   0x10000, BUILD),
]

# ── Find boot_app0.bin inside PlatformIO package cache ───────────────────

def find_boot_app0():
    pattern = os.path.expanduser(
        "~/.platformio/packages/framework-arduinoespressif32*/tools/partitions/boot_app0.bin")
    hits = glob.glob(pattern)
    if hits:
        return hits[0]
    for r, _, files in os.walk(os.path.expanduser("~/.platformio/packages")):
        if "boot_app0.bin" in files:
            return os.path.join(r, "boot_app0.bin")
    return None

# ── Encode one binary part ────────────────────────────────────────────────

def encode(filename, search_dir):
    if search_dir:
        path = os.path.join(search_dir, filename)
    else:
        path = find_boot_app0()

    if not path or not os.path.exists(path):
        sys.exit(
            f"\n✗  Missing: {filename}\n"
            f"   Run `pio run` first to compile the firmware.\n"
            f"   Expected at: {path}\n"
        )

    size = os.path.getsize(path)
    with open(path, "rb") as f:
        b64 = base64.b64encode(f.read()).decode()
    print(f"  ✓  {filename:<22} {size/1024:6.1f} KB")
    return b64

# ── Build the self-contained HTML ─────────────────────────────────────────

def build():
    print("\n📦  Building self-contained flasher...\n")

    if not os.path.isfile(TEMPLATE):
        sys.exit(f"Template not found: {TEMPLATE}\nRun from the project root.")

    parts = [
        {"b64": encode(fn, sd), "addr": addr}
        for fn, addr, sd in PARTS
    ]

    with open(TEMPLATE, encoding="utf-8") as f:
        html = f.read()

    if "%%FIRMWARE_PARTS%%" not in html:
        sys.exit("Template is missing %%FIRMWARE_PARTS%% placeholder.")

    html = html.replace("%%FIRMWARE_PARTS%%",
                        json.dumps(parts, separators=(",", ":")))

    with open(OUTPUT, "w", encoding="utf-8") as f:
        f.write(html)

    kb = os.path.getsize(OUTPUT) // 1024
    print(f"\n✅  homecontroller-flash.html  ({kb} KB)\n")

# ── Local HTTP server (so Chrome allows Web Serial) ───────────────────────

def serve():
    os.chdir(ROOT)   # serve from project root

    class QuietHandler(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *args): pass   # suppress request logs

    url = f"http://localhost:{PORT}/homecontroller-flash.html"

    def open_browser():
        import time; time.sleep(0.6)
        webbrowser.open(url)

    threading.Thread(target=open_browser, daemon=True).start()

    print(f"🌐  Serving on  {url}")
    print(f"   Chrome opened automatically.")
    print(f"   Press Ctrl+C to stop.\n")

    with socketserver.TCPServer(("", PORT), QuietHandler) as httpd:
        httpd.allow_reuse_address = True
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nServer stopped.")

# ── Entry point ───────────────────────────────────────────────────────────

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", action="store_true",
                        help="Build only — don't start a web server")
    args = parser.parse_args()

    build()

    if args.build:
        print("   (run without --build to also open the flasher in Chrome)\n")
    else:
        serve()
