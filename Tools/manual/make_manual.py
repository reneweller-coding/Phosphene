"""Phosphene -- build the user manual out of the plugin itself.

Two steps, as Noctuary's manual is built:

    set PHOS_MANUAL=docs\\screenshots
    set PHOS_SHOT_WAIT=26
    build\\...\\Phosphene.exe                 writes one PNG per tab and manual.json, then quits
    python Tools/manual/make_manual.py        manual.json + chapters.txt -> HTML -> PDF in docs/manual

PHOS_MANUAL writes into docs/screenshots because those are the same pictures: one snapshot of each
tab at design size, which the repository keeps anyway. The manual is written next to them and points
at them, rather than carrying a second copy of two megabytes of PNG.

Nothing about the parameters is written down here. `manual.json` carries every entry of the engine's
descriptor tables (key, name, unit, range, default, curve, choices), the groups each tab really
built, and the macro table -- all read back out of the running editor, so the manual is the layout
rather than a description of it. The only hand-written part is `chapters.txt`, the prose that says
why a thing is the way it is, which no table can say.

That also buys a check worth more than the text: a parameter that is in the tables but on no page
cannot be reached by the user, which is a real bug that happened once (four parameters of Phase 5
existed and appeared nowhere, because no group claimed that slice of the table). The generator
refuses to print a manual with such a hole unless --allow-holes is given, and lists them either way.

The PDF is printed by Edge or Chrome in headless mode. `--headless=new` is the flag that works; the
old one exits without a word and without a file. If no browser is there the HTML is still written
and prints perfectly well by hand.
"""
import argparse
import html
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))

CSS = """
@page { size: A4; margin: 17mm 15mm 15mm 15mm; }
body { font: 10.5pt/1.55 "Segoe UI", "Helvetica Neue", Arial, sans-serif; color: #15181f;
       background: #fff; margin: 0; }
h1 { font-size: 30pt; margin: 0 0 2mm 0; letter-spacing: -0.5pt; }
h2 { font-size: 16pt; margin: 0 0 3mm 0; padding-bottom: 2mm; border-bottom: 1px solid #ccd2dc;
     color: #1d6f86; }
h3 { font-size: 10.5pt; margin: 6mm 0 1.5mm 0; color: #1d6f86; letter-spacing: 0.6pt;
     text-transform: uppercase; }
p { margin: 0 0 3.2mm 0; text-align: justify; hyphens: auto; }
.cover { page-break-after: always; text-align: center; padding-top: 24mm; }
.cover img { width: 100%; border: 1px solid #ccd2dc; border-radius: 3px; margin-top: 10mm; }
.sub { color: #5b6470; font-size: 12pt; margin: 0; }
.facts { margin-top: 7mm; color: #5b6470; font-size: 9.5pt; }
.toc { page-break-after: always; }
.toc ol { padding-left: 6mm; }
.toc li { margin: 1.2mm 0; }
.topic { page-break-before: always; }
figure { margin: 4mm 0 5mm 0; page-break-inside: avoid; }
figure img { max-width: 100%; border: 1px solid #d4d9e1; border-radius: 3px; display: block; }
figcaption { font-size: 8.5pt; color: #6b7480; margin-top: 1.2mm; }
pre { font: 8.4pt/1.35 Consolas, "DejaVu Sans Mono", monospace; background: #f3f5f8;
      border: 1px solid #e1e5eb; border-radius: 3px; padding: 3mm; white-space: pre-wrap;
      page-break-inside: avoid; margin: 0 0 4mm 0; }
table { border-collapse: collapse; width: 100%; font-size: 8.8pt; margin: 0 0 5mm 0;
        page-break-inside: auto; }
th { text-align: left; font-weight: 600; color: #46506080; border-bottom: 1px solid #ccd2dc;
     padding: 1mm 2mm 1mm 0; color: #46506d; }
td { padding: 0.9mm 2mm 0.9mm 0; border-bottom: 1px solid #eef1f5; vertical-align: top; }
td.key { font-family: Consolas, monospace; color: #6b7480; white-space: nowrap; }
td.rng { color: #46506d; white-space: nowrap; }
h4 { font-size: 11pt; margin: 6mm 0 1.5mm 0; color: #15181f; page-break-after: avoid; }
.hole { color: #a3272c; }
footer { margin-top: 10mm; padding-top: 3mm; border-top: 1px solid #ccd2dc; color: #6b7480;
         font-size: 8.5pt; }
"""


def read_chapters(path):
    """chapters.txt -> {tab name: [paragraph, ...]} in the order the file gives them."""
    out, name, block = {}, None, []
    def flush():
        if name is not None:
            out[name] = "\n".join(block).strip("\n")
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        m = re.match(r"^==\s*(.+?)\s*==\s*$", line)
        if m:
            flush()
            name, block = m.group(1), []
            continue
        if line.startswith("#"):
            continue
        if name is not None:
            block.append(line)
    flush()
    return out


def prose(text):
    """Blank lines separate paragraphs; a block indented by four spaces is printed as it stands."""
    out = []
    for block in re.split(r"\n\s*\n", text.strip("\n")):
        lines = block.strip("\n").split("\n")
        if lines and all(l.startswith("    ") or not l.strip() for l in lines):
            out.append("<pre>%s</pre>" % html.escape("\n".join(l[4:] for l in lines)))
            continue
        block = block.strip()
        if block:
            out.append("<p>%s</p>" % html.escape(block).replace("\n", " "))
    return "\n".join(out)


def fmt(v):
    """A number the way the panel shows it: no trailing zeros, no exponent for ordinary values."""
    if abs(v - round(v)) < 1e-9 and abs(v) < 1e9:
        return str(int(round(v)))
    return ("%.4g" % v)


def param_range(p):
    if p["curve"] == "choice":
        return " | ".join(p.get("choices") or [])
    if p["curve"] == "toggle":
        return "off / on"
    unit = (" " + p["unit"]) if p["unit"] else ""
    return "%s .. %s%s" % (fmt(p["min"]), fmt(p["max"]), unit)


def param_default(p):
    if p["curve"] == "choice":
        c = p.get("choices") or []
        i = int(round(p["default"]))
        return c[i] if 0 <= i < len(c) else fmt(p["default"])
    if p["curve"] == "toggle":
        return "on" if p["default"] >= 0.5 else "off"
    return fmt(p["default"])


def table(rows, by_key):
    out = ['<table><tr><th style="width:26%">Control</th><th style="width:24%">Key</th>'
           '<th style="width:34%">Range</th><th style="width:16%">Default</th></tr>']
    for key in rows:
        p = by_key.get(key)
        if p is None:
            continue
        out.append('<tr><td>%s</td><td class="key">%s</td><td class="rng">%s</td><td class="rng">%s</td></tr>'
                   % (html.escape(p["name"]), html.escape(key), html.escape(param_range(p)),
                      html.escape(param_default(p))))
    out.append("</table>")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(ROOT, "docs", "screenshots"),
                    help="the folder PHOS_MANUAL wrote (manual.json and one picture per tab)")
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "manual"),
                    help="where the manual is written")
    ap.add_argument("--chapters", default=os.path.join(HERE, "chapters.txt"))
    ap.add_argument("--no-pdf", action="store_true")
    ap.add_argument("--allow-holes", action="store_true",
                    help="print the manual even though some parameter is on no tab")
    a = ap.parse_args()

    src = os.path.join(a.dir, "manual.json")
    if not os.path.isfile(src):
        sys.exit("no manual.json in %s -- run the standalone with PHOS_MANUAL set first" % a.dir)
    with open(src, encoding="utf-8") as f:
        man = json.load(f)
    chapters = read_chapters(a.chapters)
    by_key = {p["key"]: p for p in man["params"]}
    os.makedirs(a.out, exist_ok=True)
    # The pictures stay where the plugin put them; the manual points at them from wherever it lives.
    prefix = os.path.relpath(a.dir, a.out).replace("\\", "/")
    def image(name):
        return name if prefix == "." else prefix + "/" + name

    # The check that makes this worth running: a parameter the editor never shows.
    shown = set(man.get("shown") or [])
    holes = [k for k in by_key if k not in shown]
    if holes:
        print("%d parameter(s) appear on no tab:" % len(holes))
        for k in sorted(holes):
            print("   " + k)

    body = []
    body.append('<div class="cover">')
    body.append("<h1>%s</h1>" % html.escape(man.get("name", "Phosphene")))
    body.append('<p class="sub">Manual &middot; version %s</p>' % html.escape(man.get("version", "")))
    cover = (man["tabs"][0] or {}).get("image")
    if cover and os.path.isfile(os.path.join(a.dir, cover)):
        body.append('<img src="%s" alt="">' % html.escape(image(cover)))
    body.append('<p class="facts">%d parameters &middot; %d tabs &middot; %d perform macros &middot; '
                'every picture in this manual is the plugin drawing itself at design size</p>'
                % (len(man["params"]), len(man["tabs"]), len(man.get("macros") or [])))
    body.append("</div>")

    order = []
    if "About" in chapters:
        order.append(("About", None))
    # The signal flow second (26.09.2026): the picture the plugin draws of itself (EditorFlow.cpp, flow.png).
    if "Signal flow" in chapters:
        order.append(("Signal flow", None))
    for t in man["tabs"]:
        order.append((t["name"], t))
    # Topics that are not a tab (sound presets, shortcuts, updates): after the tabs, in the file's order.
    # The plugin's help page shows the same blocks (Plugin/EditorHelp.cpp reads this file, compiled in).
    tab_names = set(t["name"] for t in man["tabs"])
    for name in chapters:
        if name not in ("About", "Signal flow") and name not in tab_names:
            order.append((name, None))

    body.append('<div class="toc"><h2>Contents</h2><ol>')
    for name, _ in order:
        body.append("<li>%s</li>" % html.escape(name))
    if holes:
        body.append("<li>Parameters that appear on no tab</li>")
    body.append("</ol></div>")

    for i, (name, tab) in enumerate(order):
        body.append('<div class="topic">')
        body.append("<h2>%d. %s</h2>" % (i + 1, html.escape(name)))
        if name in chapters:
            body.append(prose(chapters[name]))
        elif tab is not None:
            body.append("<p>%s.</p>" % html.escape("The " + name + " tab"))
        if name == "Signal flow" and os.path.isfile(os.path.join(a.dir, "flow.png")):
            body.append('<figure><img src="%s" alt=""><figcaption>The signal flow, drawn by the plugin '
                        '(the same picture as the help page\'s).</figcaption></figure>' % html.escape(image("flow.png")))
        if tab is None:
            body.append("</div>")
            continue
        img = tab.get("image")
        if img and os.path.isfile(os.path.join(a.dir, img)):
            body.append('<figure><img src="%s" alt=""><figcaption>The %s tab.</figcaption></figure>'
                        % (html.escape(image(img)), html.escape(name)))
        if tab.get("lanes"):
            body.append("<p>The %d lanes of the kit share one table; lane 1 is printed here and the "
                        "others are the same controls under the keys <code>perc2.*</code> to "
                        "<code>perc%d.*</code>.</p>" % (tab["lanes"], tab["lanes"]))
        # The macro table belongs to the tab that carries the macros.
        for macro in (man.get("macros") or []):
            if name != "Perform":
                break
            body.append("<h4>%s%s</h4>" % (html.escape(macro["name"]),
                                           " (press)" if macro["momentary"] else ""))
            body.append("<p>%s</p>" % html.escape(macro["help"]))
            moves = []
            for mv in macro["moves"]:
                moves.append("%s %s" % (mv["key"],
                                        ("= %s" % fmt(mv["value"])) if mv["absolute"]
                                        else ("%+.2f" % mv["value"])))
            if moves:
                body.append("<p><i>Moves:</i> <code>%s</code></p>" % html.escape(", ".join(moves)))
        for group in tab.get("groups") or []:
            keys = group.get("params") or []
            if not keys:
                continue
            body.append("<h3>%s</h3>" % html.escape(group["title"]))
            body.append(table(keys, by_key))
        body.append("</div>")

    if holes:
        body.append('<div class="topic"><h2>Parameters that appear on no tab</h2>')
        body.append('<p class="hole">These exist in the engine and can be set from a preset or the '
                    'host, but no page of the editor shows them. That is a bug, not a feature.</p>')
        body.append(table(sorted(holes), by_key))
        body.append("</div>")

    body.append('<footer>%s %s &middot; generated from the plugin: the parameter tables and the '
                'pictures come out of the running editor (Tools/manual/make_manual.py).</footer>'
                % (html.escape(man.get("name", "Phosphene")), html.escape(man.get("version", ""))))

    out_html = os.path.join(a.out, "Phosphene-Manual.html")
    with open(out_html, "w", encoding="utf-8") as f:
        f.write('<!doctype html>\n<html lang="en"><head><meta charset="utf-8">\n'
                "<title>Phosphene Manual</title>\n<style>%s</style></head><body>\n%s\n</body></html>\n"
                % (CSS, "\n".join(body)))
    print("wrote %s (%.0f KB)" % (out_html, os.path.getsize(out_html) / 1024))

    rc = 0
    if holes and not a.allow_holes:
        print("refusing to call this manual complete; pass --allow-holes to print it anyway")
        rc = 1
    if a.no_pdf:
        return rc

    browser = next((p for p in (r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
                                r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
                                r"C:\Program Files\Google\Chrome\Application\chrome.exe")
                    if os.path.isfile(p)), None)
    if browser is None:
        print("no Edge or Chrome found -- the HTML is written, print it yourself")
        return rc
    pdf = os.path.join(a.out, "Phosphene-Manual.pdf")
    if os.path.exists(pdf):
        os.remove(pdf)
    url = "file:///" + out_html.replace("\\", "/")
    # A fresh profile folder every time: with the default one, or one this script used before, the
    # launcher returns at once and no file ever appears (Noctuary, 06.09.). And the process we start
    # is only the launcher, so the file is waited for rather than looked for once.
    for flag in ("--headless=new", "--headless"):
        profile = tempfile.mkdtemp(prefix="phosphene-manual-")
        cmd = [browser, flag, "--disable-gpu", "--no-pdf-header-footer",
               "--user-data-dir=" + profile, "--print-to-pdf=" + pdf, url]
        try:
            subprocess.run(cmd, timeout=180, capture_output=True)
        except subprocess.TimeoutExpired:
            print("%s did not finish in three minutes" % flag)
        for _ in range(60):
            if os.path.isfile(pdf):
                break
            time.sleep(0.5)
        if os.path.isfile(pdf):
            size = -1
            while size != os.path.getsize(pdf):   # still being written
                size = os.path.getsize(pdf)
                time.sleep(0.5)
        shutil.rmtree(profile, ignore_errors=True)
        if os.path.isfile(pdf):
            break
    if os.path.isfile(pdf):
        print("wrote %s (%.1f MB)" % (pdf, os.path.getsize(pdf) / 1e6))
    else:
        print("the browser produced no PDF; the HTML is there and prints fine by hand")
    return rc


if __name__ == "__main__":
    sys.exit(main())
