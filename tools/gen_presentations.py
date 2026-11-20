"""Generate prebuilt Mosaico Idea-grid presentations with Espressif's shared.ideas
generator, using Pillow's bundled FreeType + libraqm (no system libraqm needed)."""
import ctypes.util, glob, json, os, sys
from pathlib import Path
import PIL, PIL._imagingft as ft
libs = os.path.join(os.path.dirname(PIL.__file__), "..", "pillow.libs")
freetype_so = glob.glob(os.path.join(libs, "libfreetype-*.so*"))[0]
_orig = ctypes.util.find_library
ctypes.util.find_library = lambda n: {"freetype": freetype_so, "raqm": ft.__file__}.get(n) or _orig(n)
utils, freetype_py, font, icon, out = map(Path, sys.argv[1:6])
sys.path[:0] = [str(utils), str(freetype_py)]
from shared.ideas import generate_resources
texts = {
    "en": ("Flight Radar", "Live radar of nearby aircraft: position, altitude, speed and heading. Data: adsb.fi."),
    "zh-CN": ("航班追踪", "实时显示附近航班的位置、高度、速度和航向。数据来自 adsb.fi。"),
}
for locale, (name, desc) in texts.items():
    res = generate_resources(name=name, description=desc, locale=locale, font=font, icon=icon)
    d = out / locale
    d.mkdir(parents=True, exist_ok=True)
    for f in d.iterdir():
        f.unlink()
    for fname, data in res.items():
        (d / fname).write_bytes(data)
    print(locale, {k: len(v) for k, v in res.items()})
