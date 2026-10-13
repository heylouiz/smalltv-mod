#!/usr/bin/env python3
"""Pins the web UI to one build of the theme editor library.

Writes the jsDelivr URL for <ref> (a tag or commit of heylouiz/smalltv-mod
that contains editor/dist/stv-editor.min.js) and that file's SRI hash into
src/webui.html, then regenerates src/webui.h. The hash is computed from the
local file, so run it from a checkout of exactly that ref.

    python3 tools/pin_editor.py <ref>
"""
import base64, hashlib, pathlib, re, subprocess, sys

root = pathlib.Path(__file__).resolve().parent.parent
if len(sys.argv) != 2:
    sys.exit(__doc__)
ref = sys.argv[1]
dist = (root / "editor/dist/stv-editor.min.js").read_bytes()
sri = "sha384-" + base64.b64encode(hashlib.sha384(dist).digest()).decode()
page = root / "src/webui.html"
html = page.read_text()
html, n1 = re.subn(r"(smalltv-mod@)[^/]+(/editor/dist/stv-editor\.min\.js)", rf"\g<1>{ref}\g<2>", html)
html, n2 = re.subn(r"var STV_EDITOR_SRI='[^']*';", f"var STV_EDITOR_SRI='{sri}';", html)
if n1 != 1 or n2 != 1:
    sys.exit("pin_editor: could not find the editor URL/SRI in src/webui.html")
page.write_text(html)
subprocess.run([sys.executable, str(root / "tools/gzip_webui.py")], check=True)
print(f"editor pinned to {ref} ({sri})")
