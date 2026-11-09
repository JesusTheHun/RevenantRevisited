"""Compile the production bitmap cache and pane-release methods without a GPU."""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(file, signature):
    source = (ROOT / file).read_text(encoding="utf-8")
    start = source.index(signature)
    brace = source.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


parser = argparse.ArgumentParser()
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
decode = (ROOT / "src/bitmapdecode.h").read_text(encoding="utf-8")
start = decode.index("enum class EBitmapDecode")
parts = [
    "#include <cstdint>",
    decode[start:decode.index("};", start) + 2],
    '#include "' + (ROOT / "tools/test_bitmap_lifetime.cpp").as_posix() + '"',
    function("src/bitmapatlas.cpp", "inline void HashU64("),
    function("src/bitmapatlas.cpp", "uint64_t UIBitmapAtlasKey("),
    function("src/renderer.cpp", "void TRenderer::ReleaseBitmapTextures("),
    function("src/renderer.cpp", "TTextureHandle TRenderer::BitmapAsTexture("),
    "#define free RetireArchive",
    function("src/defpane.cpp", "void TDefPane::ReleaseAssets()"),
    "#undef free",
]
args.output.write_text("\n\n".join(parts), encoding="utf-8")
