"""Compile production keyboard/mouse use paths with an asset-free map double."""
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
parts = [
    '#include "' + (ROOT / "tools/test_use_reach.cpp").as_posix() + '"',
    function("src/character.cpp", "int32_t ObjectsAhead("),
    function("src/character.cpp", "TObjectInstance* TCharacter::FindUseTarget("),
    function("src/character.cpp", "bool TCharacter::TryUse()"),
    function("src/mappane.cpp", "void TMapPane::PlayMouseRelease("),
]
args.output.write_text("\n\n".join(parts), encoding="utf-8")
