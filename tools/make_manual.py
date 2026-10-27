#!/usr/bin/env python3
# Makes a PDF of a manual in docs/ (pictures included) with Chrome:
#   python3 tools/make_manual.py docs/BASIC_MANUAL.md "manuals/Pico 2 mini OS BASIC User's Manual.pdf"
#   python3 tools/make_manual.py docs/OS_MANUAL.md "manuals/Pico 2 mini OS User's Manual.pdf" help.png
# (the last argument: the cover picture in docs/, basic_font.png by default, or
# none). The manuals are listed in the last section of docs/OS_MANUAL.md.
# Needs python3-markdown and google-chrome (or chromium) for printing.
import datetime, html, os, re, shutil, subprocess, sys, tempfile

import markdown

src, out = sys.argv[1], sys.argv[2]
cover_name = sys.argv[3] if len(sys.argv) > 3 else 'basic_font.png'
text = open(src, encoding='utf-8').read()
title = re.search(r'^# (.+)$', text, re.M).group(1)
body_md = re.sub(r'^# .+\n', '', text, count=1)

md = markdown.Markdown(extensions=['tables', 'fenced_code', 'toc', 'sane_lists'],
                       extension_configs={'toc': {'toc_depth': '2-3'}})
body = md.convert(body_md)
toc = md.toc
docs = os.path.abspath(os.path.dirname(src))
# Pictures from docs/, as files the browser can open.
body = re.sub(r'<img alt="([^"]*)" src="([^":]+)"',
              lambda m: '<figure><img alt="%s" src="file://%s"' % (m.group(1), os.path.join(docs, m.group(2))),
              body)
body = re.sub(r'(<figure><img[^>]*>)', r'\1</figure>', body)
body = body.replace('<p><figure>', '<figure>').replace('</figure></p>', '</figure>')

css = """
@page { size: A4; margin: 18mm 16mm 18mm 16mm;
        @bottom-center { content: counter(page); font: 9pt sans-serif; color: #666; } }
body { font: 10.5pt/1.45 "DejaVu Sans", "Liberation Sans", sans-serif; color: #111; }
h1.title { font-size: 30pt; margin: 60mm 0 4mm; color: #0b3d91; }
.sub { font-size: 13pt; color: #444; }
.cover img { width: 100%; margin-top: 16mm; image-rendering: pixelated; border: 1px solid #333; }
.contents { page-break-before: always; }
.tochead { font-size: 15pt; font-weight: bold; color: #0b3d91; margin-bottom: 4mm; }
.toc ul { list-style: none; padding-left: 1.2em; }
.toc > ul { padding-left: 0; }
.toc a { color: #111; text-decoration: none; }
h2 { color: #0b3d91; border-bottom: 2px solid #0b3d91; padding-bottom: 2px; margin-top: 1.6em;
     page-break-before: always; }
h3 { color: #0b3d91; margin-top: 1.3em; page-break-after: avoid; }
table { border-collapse: collapse; width: 100%; margin: .6em 0; font-size: 9.2pt; }
th, td { border: 1px solid #bbb; padding: 3px 6px; vertical-align: top; text-align: left; }
th { background: #e8eef8; }
tr { page-break-inside: avoid; }
code { font: 9pt "DejaVu Sans Mono", monospace; background: #f2f2f2; padding: 0 2px; }
pre { background: #10141c; color: #e6e6e6; padding: 8px 10px; border-radius: 4px;
      font-size: 8.8pt; line-height: 1.35; page-break-inside: avoid; white-space: pre-wrap; }
pre code { background: none; color: inherit; padding: 0; }
figure { margin: .8em 0; text-align: center; page-break-inside: avoid; }
figure img { max-width: 82%; image-rendering: pixelated; border: 1px solid #333; }
"""
cover = cover_name if os.path.exists(os.path.join(docs, cover_name)) else None
page = f"""<!doctype html><html><head><meta charset="utf-8"><title>{html.escape(title)}</title>
<style>{css}</style></head><body>
<div class="cover"><h1 class="title">{html.escape(title)}</h1>
<div class="sub">Pico 2 mini OS &middot; {datetime.date.today().strftime('%B %d, %Y')}</div>
{f'<img src="file://{os.path.join(docs, cover)}">' if cover else ''}</div>
<div class="contents"><div class="tochead">Contents</div>{toc}</div>
{body}
</body></html>"""

tmp = tempfile.mkdtemp()
page_path = os.path.join(tmp, 'manual.html')
open(page_path, 'w', encoding='utf-8').write(page)
chrome = shutil.which('google-chrome') or shutil.which('chromium') or shutil.which('chromium-browser')
subprocess.run([chrome, '--headless=new', '--disable-gpu', '--no-pdf-header-footer',
                '--allow-file-access-from-files', '--user-data-dir=' + os.path.join(tmp, 'profile'),
                '--print-to-pdf=' + os.path.abspath(out), 'file://' + page_path],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
shutil.rmtree(tmp, ignore_errors=True)
print('wrote', out)
