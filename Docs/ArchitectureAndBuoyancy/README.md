# FluidNinja architecture and buoyancy guide

Open [the illustrated HTML guide](index.html) or [the printable PDF](FluidNinja_Architecture_and_Buoyancy_Guide.pdf).

On GitHub, the HTML link displays source. Download `index.html` and open it in a browser to use the calculator and property search. Keep the PDF beside it for the download link, or read the PDF directly on GitHub. No web server or installation is needed to read the guide.

The guide covers the modified FluidNinja core, reusable FluidNinja Buoyancy plugin, public integration APIs, physics, implementation, and worked beach-ball, log, boat, and wrecking-ball examples. HTML adds a buoyancy calculator and property search. It is intended for any compatible Unreal Engine project using the modified core and plugin.

Source audit date: 18 October 2026. This is a source-based explanation, not a new runtime certification. See the evidence boundary on the cover and the validation chapter.

**Source snapshot:** This edition describes the implementation audited on 18 October 2026. Some tools and APIs described are newer than the repository code at the time the guide was added. Use `source_manifest.json` to match the documented implementation to your plugin source distribution; consult the guide's compatibility chapter before using version-dependent features. Rebuilding the guide requires that newer source snapshot, including the files listed in the manifest. Adding this documentation does not add those runtime features to the repository.

## Files and maintenance

- `guide_body.html`: authored chapters and inline SVG diagrams.
- `build_guide.py`: combines the chapters, styles, calculator, source map, and generated property reference into `index.html`. It reads text sources only.
- `property_inventory.json`: 184 editable declarations extracted from body, project, simulation, and preset headers; header defaults are distinct from runtime and project overrides.
- `source_manifest.json`: plugin-relative source paths and SHA-256 hashes for this audit. Locate these files in your plugin source package; no local checkout is required to read either guide.
- `print_page_map.json`: verified chapter page numbers for this PDF.
- `render_guide.cjs`: browser checks, desktop/mobile screenshots, and PDF export using Playwright and Chrome.

To rebuild, run `python build_guide.py --plugin-root /path/to/FluidNinjaBuoyancy`, then `node render_guide.cjs`. Python 3, Node.js, Playwright, and Chrome are required. The builder also supports `FNB_PLUGIN_ROOT` and automatic discovery under a surrounding project's `Plugins/FluidNinjaBuoyancy` directory. The renderer uses an installed `playwright` package and Chrome; `PLAYWRIGHT_MODULE` and `CHROME_PATH` can override their locations. Browser QA output goes to a `qa` folder beside the guide, or to `GUIDE_QA_DIR` when set.

Update `print_page_map.json` after content changes, render again, and verify that the page numbers remain stable. Source paths in the guide are relative to the plugin root. The HTML uses inline diagrams, styles, and scripts and can be opened directly in a browser. Keep the PDF beside it for the download link to work.

## Verification for this edition

All 45 PDF pages were visually inspected. The contents page, bookmarks, source links, tables, and diagrams were checked. Desktop and 390-pixel mobile layouts have no horizontal overflow. Browser JavaScript reports no errors, internal anchors resolve, and property search returns the expected entry. Calculator checks cover the four worked objects, a half-submerged box, neutral buoyancy, and invalid mass input.

No Unreal asset or gameplay code was changed for this guide.
