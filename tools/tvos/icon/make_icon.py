#!/usr/bin/env python3
"""Builds the Apple TV app icon and Top Shelf images from dist/azahar.svg.

tvOS app icons are layered: the system moves the layers against each other
when the icon is focused (parallax). The logo's groups become the layers,
back to front:

  Back    dark teal background with a soft light behind the flower
  Glow    the logo's blurred outline glow (moves least)
  Petals  the petals
  Center  the pistil and stamens (moves most)

Writes src/dekui_visionos/TV/Assets.xcassets ("App Icon & Top Shelf Image").
Needs macOS (AppKit renders the SVG), Pillow and NumPy.

usage: python3 tools/tvos/icon/make_icon.py [--preview DIR]
"""
from __future__ import annotations

import argparse
import copy
import json
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[3]
SVG = ROOT / "dist/azahar.svg"
RENDERER = Path(__file__).with_name("render_svg.swift")
CATALOG = ROOT / "src/dekui_visionos/TV/Assets.xcassets"
BRAND = "App Icon & Top Shelf Image"

SVG_NS = "http://www.w3.org/2000/svg"
INKSCAPE_NS = "http://www.inkscape.org/namespaces/inkscape"
for prefix, uri in (("", SVG_NS), ("xlink", "http://www.w3.org/1999/xlink"),
                    ("inkscape", INKSCAPE_NS), ("svg", SVG_NS)):
    ET.register_namespace(prefix, uri)

# Element ids in dist/azahar.svg kept by each layer.
LAYERS = {
    "Glow": {"back_color"},
    "Petals": {"p_ne", "p_se", "p_n", "p_nw", "p_sw", "p_s", "c_cover", "c_green"},
    "Center": {"pistil", "stamen"},
}
# Teal of the Dekui mark (resources/mm3d/dekui.svg), light enough behind the
# flower for the logo's dark glow to show.
EDGE, MIDDLE, LIGHT = (10, 30, 34), (30, 70, 68), (86, 158, 136)


def layer_svg(keep: set[str], out: Path) -> None:
    """dist/azahar.svg without the drawn elements outside `keep`."""
    tree = ET.parse(SVG)
    drawn = {"path", "ellipse", "g"}

    def prune(parent: ET.Element, inside: bool) -> None:
        for child in list(parent):
            tag = child.tag.rsplit("}", 1)[-1]
            if tag not in drawn:
                continue  # defs, gradients and filters stay
            kept = inside or child.get("id") in keep
            if tag == "g" and not kept:
                prune(child, False)
                if not any(c.tag.rsplit("}", 1)[-1] in drawn for c in child):
                    parent.remove(child)
            elif not kept:
                parent.remove(child)

    prune(tree.getroot(), False)
    tree.write(out, xml_declaration=True, encoding="UTF-8")


def render(svg: Path, size: int, work: Path) -> Image.Image:
    out = work / f"{svg.stem}-{size}.png"
    subprocess.run(["xcrun", "swift", str(RENDERER), str(svg), str(out), str(size), str(size)],
                   check=True)
    return Image.open(out).convert("RGBA")


def background(width: int, height: int, cx: float, cy: float, radius: float) -> Image.Image:
    """Teal light behind the flower fading to near black, with a slight vignette."""
    y, x = np.mgrid[0:height, 0:width].astype(np.float32)
    d = np.hypot((x - cx) / radius, (y - cy) / radius)
    t = np.clip(d, 0, 1.6) / 1.6
    light, middle, edge = (np.array(c, np.float32) for c in (LIGHT, MIDDLE, EDGE))
    near = np.clip(t * 2, 0, 1)[..., None]
    far = np.clip(t * 2 - 1, 0, 1)[..., None]
    rgb = light + (middle - light) * near
    rgb = rgb + (edge - rgb) * far
    return Image.fromarray(np.clip(np.round(rgb), 0, 255).astype(np.uint8), "RGB")


def place(layer: Image.Image, width: int, height: int, size: int, cx: float, cy: float) -> Image.Image:
    canvas = Image.new("RGBA", (width, height))
    sprite = layer.resize((size, size), Image.LANCZOS)
    canvas.alpha_composite(sprite, (round(cx - size / 2), round(cy - size / 2)))
    return canvas


def icon_layers(width: int, height: int, sources: dict[str, Image.Image]) -> dict[str, Image.Image]:
    """Layer images at one icon size. The flower stays inside the center, away
    from the edges the parallax effect crops."""
    cx, cy, size = width / 2, height / 2, round(height * 0.78)
    layers = {"Back": background(width, height, cx, cy, height * 0.62)}
    for name, image in sources.items():
        layers[name] = place(image, width, height, size, cx, cy)
    return layers


def top_shelf(width: int, height: int, sources: dict[str, Image.Image]) -> Image.Image:
    cx, cy, size = width * 0.74, height * 0.5, round(height * 0.92)
    image = background(width, height, cx, cy, height * 0.75).convert("RGBA")
    for source in sources.values():
        image.alpha_composite(place(source, width, height, size, cx, cy))
    return image.convert("RGB")


def write_json(path: Path, data: dict) -> None:
    path.mkdir(parents=True, exist_ok=True)
    (path / "Contents.json").write_text(json.dumps(data, indent=2) + "\n")


INFO = {"author": "xcode", "version": 1}


def write_stack(folder: Path, scales: dict[str, dict[str, Image.Image]]) -> None:
    """An .imagestack, front layer first; `scales` maps "1x"/"2x" to layer images."""
    names = ["Center", "Petals", "Glow", "Back"]
    write_json(folder, {"info": INFO, "layers": [{"filename": f"{n}.imagestacklayer"} for n in names]})
    for name in names:
        layer = folder / f"{name}.imagestacklayer"
        write_json(layer, {"info": INFO})
        images = []
        for scale, layers in scales.items():
            filename = f"{name.lower()}@{scale}.png"
            image = layers[name]
            image = image.convert("RGB") if name == "Back" else image  # the back layer is opaque
            (layer / "Content.imageset").mkdir(parents=True, exist_ok=True)
            image.save(layer / "Content.imageset" / filename, optimize=True)
            images.append({"filename": filename, "idiom": "tv", "scale": scale})
        write_json(layer / "Content.imageset", {"images": images, "info": INFO})


def write_imageset(folder: Path, scales: dict[str, Image.Image], stem: str) -> None:
    images = []
    folder.mkdir(parents=True, exist_ok=True)
    for scale, image in scales.items():
        filename = f"{stem}@{scale}.png"
        image.save(folder / filename, optimize=True)
        images.append({"filename": filename, "idiom": "tv", "scale": scale})
    write_json(folder, {"images": images, "info": INFO})


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", type=Path, help="also save flattened previews here")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        sources = {}
        for name, keep in LAYERS.items():
            svg = work / f"{name.lower()}.svg"
            layer_svg(keep, svg)
            # Square render, large enough for the 1280x768 App Store icon.
            sources[name] = render(svg, 1600, work)

        small = {"1x": icon_layers(400, 240, sources), "2x": icon_layers(800, 480, sources)}
        store = {"1x": icon_layers(1280, 768, sources)}
        shelf = {s: top_shelf(1920 * m, 720 * m, sources) for s, m in (("1x", 1), ("2x", 2))}
        wide = {s: top_shelf(2320 * m, 720 * m, sources) for s, m in (("1x", 1), ("2x", 2))}

        if CATALOG.exists():
            shutil.rmtree(CATALOG)
        write_json(CATALOG, {"info": INFO})
        brand = CATALOG / f"{BRAND}.brandassets"
        write_json(brand, {"info": INFO, "assets": [
            {"filename": "App Icon - App Store.imagestack", "idiom": "tv",
             "role": "primary-app-icon", "size": "1280x768"},
            {"filename": "App Icon.imagestack", "idiom": "tv",
             "role": "primary-app-icon", "size": "400x240"},
            {"filename": "Top Shelf Image Wide.imageset", "idiom": "tv",
             "role": "top-shelf-image-wide", "size": "2320x720"},
            {"filename": "Top Shelf Image.imageset", "idiom": "tv",
             "role": "top-shelf-image", "size": "1920x720"},
        ]})
        write_stack(brand / "App Icon.imagestack", small)
        write_stack(brand / "App Icon - App Store.imagestack", store)
        write_imageset(brand / "Top Shelf Image.imageset", shelf, "top-shelf")
        write_imageset(brand / "Top Shelf Image Wide.imageset", wide, "top-shelf-wide")

        if args.preview:
            args.preview.mkdir(parents=True, exist_ok=True)
            for scale, layers in small.items():
                flat = layers["Back"].convert("RGBA")
                for name in ("Glow", "Petals", "Center"):
                    flat.alpha_composite(layers[name])
                flat.convert("RGB").save(args.preview / f"icon@{scale}.png")
            shelf["1x"].save(args.preview / "top-shelf.png")
            wide["1x"].save(args.preview / "top-shelf-wide.png")
    print(f"wrote {CATALOG.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
