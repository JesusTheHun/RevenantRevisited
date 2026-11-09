"""Compile the production particle-template ownership decision without assets."""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
source = (ROOT / "src/3dimage.cpp").read_text(encoding="utf-8")
start = source.index("bool T3DAnimator::PartSysOwnsObject(")
brace = source.index("{", start)
end, depth = brace + 1, 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
args.output.write_text(
    '#include "' + (ROOT / "tools/test_partsys_ownership.cpp").as_posix() + '"\n'
    + source[start:end], encoding="utf-8")
