#!/usr/bin/env python3
"""Embed a file as a C string constant: embed.py <input> <output.h> <SYMBOL>

Text inputs may contain <!--INCLUDE:relative/path--> markers, replaced by
that file's contents (paths relative to the project root)."""
import os
import re
import sys

src, dst, sym = sys.argv[1:4]
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
data = open(src, "rb").read()
data = re.sub(rb"<!--INCLUDE:([^>]+?)-->", lambda m: open(os.path.join(root, m.group(1).decode()), "rb").read(), data)
lines = []
for i in range(0, len(data), 64):
    chunk = data[i:i + 64]
    lines.append('"' + "".join("\\x%02x" % b for b in chunk) + '"')
with open(dst, "w") as f:
    f.write("/* generated from %s - do not edit */\n" % src)
    f.write("static const char %s[] =\n%s;\n" % (sym, "\n".join(lines) or '""'))
