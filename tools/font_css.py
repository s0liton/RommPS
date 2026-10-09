#!/usr/bin/env python3
"""An @font-face rule with the font inlined: font_css.py <font.woff2> <out.css> <family>

The web UI is built into the payload and consoles may be offline, so fonts
can't come from the network."""
import base64
import sys

src, dst, family = sys.argv[1:4]
data = base64.b64encode(open(src, "rb").read()).decode()
with open(dst, "w") as f:
    f.write("/* generated from %s - do not edit */\n" % src)
    f.write("@font-face { font-family: '%s'; font-style: normal; font-weight: 100 900; font-display: swap;\n"
            "  src: url(data:font/woff2;base64,%s) format('woff2'); }\n" % (family, data))
