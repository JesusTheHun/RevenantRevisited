"""Build the production reader with asset-free pane/font/renderer doubles.

The declarations and method bodies come from src/, so the fixture exercises
the shipped reader rather than a parallel implementation of its behavior.
"""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(path, signature):
    source = (ROOT / path).read_text(encoding="utf-8")
    start = source.index(signature)
    opening = source.index("{", start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


parser = argparse.ArgumentParser()
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
header = (ROOT / "src/scroll.h").read_text(encoding="utf-8")
source = (ROOT / "src/scroll.cpp").read_text(encoding="utf-8")
parts = [
    '#include "' + (ROOT / "tools/scroll_reader_doubles.h").as_posix() + '"',
    header[header.index("_CLASSDEF(TScrollPane)"):],
    "TScrollPane ScrollPane; TBookPane BookPane;",
    function("src/font.cpp", "int32_t WrapTextLines("),
    function("src/scroll.cpp", "bool TScroll::Use("),
    function("src/playscreen.cpp", "void TPlayScreen::OpenNextPane()"),
    source[source.index("bool TScrollPane::Initialize()"):],
    '#include "' + (ROOT / "tools/test_scroll_reader.cpp").as_posix() + '"',
]
args.output.write_text("\n\n".join(parts), encoding="utf-8")
