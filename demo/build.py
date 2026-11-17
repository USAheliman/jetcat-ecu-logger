#!/usr/bin/env python3
"""Builds the web demo into site/: the logger's page (data/index.html) with demo/shim.js
loaded first, plus demo/data.json. Run from the repo root; GitHub Pages publishes site/."""
import os, shutil
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
site = os.path.join(root, "site")
shutil.rmtree(site, ignore_errors=True)
os.makedirs(os.path.join(site, "demo"))
page = open(os.path.join(root, "data", "index.html"), encoding="utf-8").read()
shim = open(os.path.join(root, "demo", "shim.js"), encoding="utf-8").read()
i = page.index("<script>")
page = page[:i] + "<script>\n" + shim + "\n</script>\n" + page[i:]
page = page.replace("<title>", "<title>Demo · ", 1)
open(os.path.join(site, "demo", "index.html"), "w", encoding="utf-8").write(page)
shutil.copy(os.path.join(root, "demo", "data.json"), os.path.join(site, "demo", "data.json"))
open(os.path.join(site, "index.html"), "w").write(
    '<!doctype html><meta http-equiv="refresh" content="0; url=demo/"><a href="demo/">JetCat Logger demo</a>\n')
open(os.path.join(site, ".nojekyll"), "w").close()
print("site/ built")
