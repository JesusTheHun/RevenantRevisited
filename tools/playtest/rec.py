#!/usr/bin/env python3
"""Read, write, check and compare playtest recordings (*.rvrec).

    rec.py show     BUNDLE_OR_FILE          header, counts, marks, last tick
    rec.py validate BUNDLE_OR_FILE          check the format (exit 1 on errors)
    rec.py diff     A B                     first tick whose check lines differ
    rec.py slotsha  SLOT_DIR                the slot hash a header records

The format and the bundle are specified in docs/playtest/PROTOCOL.md §2-§4
and §6; this module is the director's reader and writer. A recording is the
input both games receive at each tick, plus check lines that let a replay
find the first tick where it stops matching.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

FORMAT_VERSION = 1
TICK_HZ = 24
SCREEN = (640, 480)
RECORDING_NAME = 'session.rvrec'
INI_NAME = 'Revenant.ini'
START_DIR = 'start'

# Windows virtual-key names (without VK_) and codes: PROTOCOL.md §2.
VK: dict[str, int] = {
    'BACK': 0x08, 'TAB': 0x09, 'RETURN': 0x0D, 'SHIFT': 0x10, 'CONTROL': 0x11,
    'MENU': 0x12, 'PAUSE': 0x13, 'CAPITAL': 0x14, 'ESCAPE': 0x1B, 'SPACE': 0x20,
    'PRIOR': 0x21, 'NEXT': 0x22, 'END': 0x23, 'HOME': 0x24, 'LEFT': 0x25,
    'UP': 0x26, 'RIGHT': 0x27, 'DOWN': 0x28, 'INSERT': 0x2D, 'DELETE': 0x2E,
    'LWIN': 0x5B, 'RWIN': 0x5C, 'APPS': 0x5D,
    'MULTIPLY': 0x6A, 'ADD': 0x6B, 'SEPARATOR': 0x6C, 'SUBTRACT': 0x6D,
    'DECIMAL': 0x6E, 'DIVIDE': 0x6F, 'NUMLOCK': 0x90, 'SCROLL': 0x91,
    'OEM_1': 0xBA, 'OEM_PLUS': 0xBB, 'OEM_COMMA': 0xBC, 'OEM_MINUS': 0xBD,
    'OEM_PERIOD': 0xBE, 'OEM_2': 0xBF, 'OEM_3': 0xC0, 'OEM_4': 0xDB,
    'OEM_5': 0xDC, 'OEM_6': 0xDD, 'OEM_7': 0xDE,
    **{chr(c): c for c in range(ord('0'), ord('9') + 1)},
    **{chr(c): c for c in range(ord('A'), ord('Z') + 1)},
    **{f'NUMPAD{n}': 0x60 + n for n in range(10)},
    **{f'F{n}': 0x6F + n for n in range(1, 25)},
}
VK_NAME: dict[int, str] = {code: name for name, code in VK.items()}

BUTTON_LEFT, BUTTON_RIGHT, BUTTON_MIDDLE = 1, 2, 4
BUTTONS_ALL = BUTTON_LEFT | BUTTON_RIGHT | BUTTON_MIDDLE


class FormatError(ValueError):
    """A line or a recording that breaks PROTOCOL.md."""


def vk_code(name: str) -> int:
    """The code of a key name: a VK name, or a code written as "0x5D"."""
    if name in VK:
        return VK[name]
    if name.lower().startswith('0x'):
        code = int(name, 16)
        if 0 < code < 0x100:
            return code
    raise FormatError(f'unknown key name {name!r}')


# ---- lines ---------------------------------------------------------------

@dataclass(frozen=True)
class Key:
    t: int
    key: str
    down: bool

    def line(self) -> dict:
        return {'t': self.t, 'key': self.key, 'down': self.down}


@dataclass(frozen=True)
class Char:
    t: int
    code: int                        # Windows-1252

    def line(self) -> dict:
        return {'t': self.t, 'char': self.code}


@dataclass(frozen=True)
class Mouse:
    t: int
    x: int
    y: int
    buttons: int

    def line(self) -> dict:
        return {'t': self.t, 'mouse': [self.x, self.y], 'buttons': self.buttons}


@dataclass(frozen=True)
class Frame:
    """A display frame under frame timing: its exact time step."""
    f: int
    dt: float

    def line(self) -> dict:
        return {'f': self.f, 'dt': struct.pack('>d', self.dt).hex()}


@dataclass(frozen=True)
class Check:
    t: int
    fields: dict

    def line(self) -> dict:
        return {'t': self.t, 'check': self.fields}


@dataclass(frozen=True)
class Mark:
    t: int
    kind: str
    extra: dict = field(default_factory=dict)

    def line(self) -> dict:
        return {'t': self.t, 'mark': self.kind, **self.extra}


@dataclass(frozen=True)
class End:
    t: int

    def line(self) -> dict:
        return {'t': self.t, 'end': True}


@dataclass(frozen=True)
class Wheel:
    t: int
    x: int
    y: int
    units: int

    def line(self) -> dict:
        return {'t': self.t, 'mouse': [self.x, self.y], 'wheel': self.units}


Event = Key | Char | Mouse | Wheel
Item = Key | Char | Mouse | Wheel | Frame | Check | Mark | End


def _int(obj: dict, name: str) -> int:
    value = obj.get(name)
    if not isinstance(value, int) or isinstance(value, bool):
        raise FormatError(f'{name!r} must be an integer: {obj}')
    return value


def parse_item(obj: dict, timing: str = 'lockstep') -> Item:
    """One body line of a recording. Under frame timing, events carry `f`
    (the frame they precede) in place of `t`; they are returned with that
    frame number as their time."""
    if 'dt' in obj:
        dt = obj['dt']
        if not isinstance(dt, str) or len(dt) != 16:
            raise FormatError(f'dt must be 16 hex digits: {obj}')
        return Frame(_int(obj, 'f'), struct.unpack('>d', bytes.fromhex(dt))[0])
    if 'check' in obj:
        if not isinstance(obj['check'], dict):
            raise FormatError(f'check must be an object: {obj}')
        return Check(_int(obj, 't'), obj['check'])
    if 'mark' in obj:
        extra = {k: v for k, v in obj.items() if k not in ('t', 'mark')}
        return Mark(_int(obj, 't'), str(obj['mark']), extra)
    if obj.get('end') is True:
        return End(_int(obj, 't'))
    t = _int(obj, 'f' if timing == 'frames' else 't')
    if 'key' in obj:
        name = obj['key']
        if not isinstance(name, str):
            raise FormatError(f'key must be a name: {obj}')
        vk_code(name)
        if not isinstance(obj.get('down'), bool):
            raise FormatError(f'down must be true or false: {obj}')
        return Key(t, name, obj['down'])
    if 'char' in obj:
        code = _int(obj, 'char')
        if not 0 < code < 0x100:
            raise FormatError(f'char must be a Windows-1252 code: {obj}')
        return Char(t, code)
    if 'mouse' in obj:
        pos = obj['mouse']
        if (not isinstance(pos, list) or len(pos) != 2
                or not all(isinstance(v, int) and not isinstance(v, bool) for v in pos)):
            raise FormatError(f'mouse must be [x, y]: {obj}')
        if 'wheel' in obj:
            units = _int(obj, 'wheel')
            if not -(1 << 31) <= units < (1 << 31):
                raise FormatError(f'wheel units out of range: {obj}')
            return Wheel(t, pos[0], pos[1], units)
        buttons = _int(obj, 'buttons')
        if buttons & ~BUTTONS_ALL:
            raise FormatError(f'unknown button bits: {obj}')
        return Mouse(t, pos[0], pos[1], buttons)
    raise FormatError(f'unknown line: {obj}')


def item_line(item: Item, timing: str = 'lockstep') -> dict:
    line = item.line()
    if timing == 'frames' and isinstance(item, (Key, Char, Mouse, Wheel)):
        line['f'] = line.pop('t')
    return line


# ---- the recording -------------------------------------------------------

@dataclass
class Recording:
    header: dict
    items: list[Item] = field(default_factory=list)

    @property
    def timing(self) -> str:
        return self.header.get('timing', 'lockstep')

    @property
    def events(self) -> list[Event]:
        return [i for i in self.items if isinstance(i, (Key, Char, Mouse, Wheel))]

    @property
    def checks(self) -> dict[int, dict]:
        """Check fields by tick; a later line at the same tick wins."""
        return {i.t: i.fields for i in self.items if isinstance(i, Check)}

    @property
    def marks(self) -> list[Mark]:
        return [i for i in self.items if isinstance(i, Mark)]

    @property
    def end(self) -> int | None:
        ends = [i.t for i in self.items if isinstance(i, End)]
        return ends[-1] if ends else None

    @classmethod
    def load(cls, path: Path) -> Recording:
        path = recording_path(path)
        with path.open(encoding='utf-8') as f:
            lines = [json.loads(text) for text in f if text.strip()]
        if not lines:
            raise FormatError(f'{path}: empty')
        header = lines[0]
        if header.get('rvrec') != FORMAT_VERSION:
            raise FormatError(f'{path}: not an rvrec v{FORMAT_VERSION} header')
        timing = header.get('timing', 'lockstep')
        return cls(header, [parse_item(obj, timing) for obj in lines[1:]])

    def save(self, path: Path) -> None:
        with path.open('w', encoding='utf-8', newline='\n') as f:
            f.write(json.dumps(self.header, separators=(',', ':')) + '\n')
            for item in self.items:
                f.write(json.dumps(item_line(item, self.timing), separators=(',', ':')) + '\n')


def recording_path(path: Path) -> Path:
    """A bundle directory's recording, or the path itself."""
    path = Path(path)
    return path / RECORDING_NAME if path.is_dir() else path


def validate(rec: Recording) -> list[str]:
    """Format problems beyond what parsing checks: required header fields,
    ordering, pointer bounds, a single end."""
    problems = []
    for name in ('game', 'build', 'start', 'seed', 'tick_hz', 'timing'):
        if name not in rec.header:
            problems.append(f'header lacks {name!r}')
    if rec.header.get('game') not in ('port', 'retail'):
        problems.append(f"game must be 'port' or 'retail', not {rec.header.get('game')!r}")
    if rec.timing not in ('lockstep', 'frames'):
        problems.append(f'unknown timing {rec.timing!r}')
    start = rec.header.get('start', {})
    if start.get('from') == 'slot' and not start.get('slot_sha'):
        problems.append('a slot start needs slot_sha')
    last = {'t': -1, 'f': -1}
    for item in rec.items:
        key = 'f' if isinstance(item, Frame) or (rec.timing == 'frames'
                                                 and isinstance(item, (Key, Char, Mouse, Wheel))) else 't'
        when = item.f if isinstance(item, Frame) else item.t
        if when < last[key]:
            problems.append(f'{key}={when} goes back in time: {item_line(item, rec.timing)}')
        last[key] = max(last[key], when)
        if isinstance(item, Mouse) and not (0 <= item.x < SCREEN[0] and 0 <= item.y < SCREEN[1]):
            problems.append(f'pointer off the 640x480 screen: {item.line()}')
    ends = [i for i in rec.items if isinstance(i, End)]
    if len(ends) > 1 or (ends and rec.items[-1] is not ends[0]):
        problems.append('the end line must be the last line, once')
    return problems


# ---- comparing replays ---------------------------------------------------

@dataclass(frozen=True)
class Divergence:
    t: int
    fields: dict                     # name -> (expected, actual)

    def __str__(self) -> str:
        parts = ', '.join(f'{name}: {a!r} != {b!r}' for name, (a, b) in self.fields.items())
        return f'tick {self.t}: {parts}'


def compare_checks(expected: dict[int, dict], actual: dict[int, dict]) -> Divergence | None:
    """The first tick, among those both sides checked, whose fields differ.
    A field either side lacks is skipped (PROTOCOL.md §3)."""
    for t in sorted(expected.keys() & actual.keys()):
        a, b = expected[t], actual[t]
        diff = {name: (a[name], b[name]) for name in sorted(a.keys() & b.keys()) if a[name] != b[name]}
        if diff:
            return Divergence(t, diff)
    return None


# ---- slots ---------------------------------------------------------------

def slot_sha(slot: Path) -> str:
    """PROTOCOL.md §6: every file in sorted order of its lower-case relative
    path, each as the path, NUL, the bytes, NUL."""
    slot = Path(slot)
    files = sorted((p.relative_to(slot).as_posix().lower(), p)
                   for p in slot.rglob('*') if p.is_file())
    digest = hashlib.sha256()
    for name, path in files:
        digest.update(name.encode('utf-8') + b'\0')
        digest.update(path.read_bytes() + b'\0')
    return digest.hexdigest()


def file_sha(path: Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


# ---- command line --------------------------------------------------------

def _show(rec: Recording) -> None:
    h = rec.header
    print(f"{h.get('game')} {h.get('build', {}).get('name', '?')} "
          f"start={h.get('start', {}).get('from')} {h.get('start', {}).get('slot', '')} "
          f"seed={h.get('seed')} timing={rec.timing}")
    counts: dict[str, int] = {}
    for item in rec.items:
        counts[type(item).__name__] = counts.get(type(item).__name__, 0) + 1
    print('lines: ' + ', '.join(f'{n} {k}' for k, n in sorted(counts.items())))
    for mark in rec.marks:
        print(f'  t={mark.t} {mark.kind} {mark.extra}')
    print(f'end: {rec.end}')


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = parser.add_subparsers(dest='cmd', required=True)
    sub.add_parser('show').add_argument('path', type=Path)
    sub.add_parser('validate').add_argument('path', type=Path)
    diff = sub.add_parser('diff')
    diff.add_argument('a', type=Path)
    diff.add_argument('b', type=Path)
    sub.add_parser('slotsha').add_argument('slot', type=Path)
    args = parser.parse_args(argv)

    if args.cmd == 'slotsha':
        print(slot_sha(args.slot))
        return 0
    if args.cmd == 'diff':
        divergence = compare_checks(Recording.load(args.a).checks, Recording.load(args.b).checks)
        print(divergence or 'no divergence')
        return 1 if divergence else 0
    rec = Recording.load(args.path)
    if args.cmd == 'show':
        _show(rec)
        return 0
    problems = validate(rec)
    for problem in problems:
        print(problem)
    print('ok' if not problems else f'{len(problems)} problem(s)')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
