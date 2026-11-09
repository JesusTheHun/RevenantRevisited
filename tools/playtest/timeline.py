#!/usr/bin/env python3
"""The whole gameplay log of a session, by game tick, for finding bugs.

    timeline.py BUNDLE            writes BUNDLE/timeline.html (and prints its path)

Merges everything a session bundle holds into one page ordered by tick:
the inputs both games received and the check lines (session.rvrec), the
director's modes and decisions (director.jsonl), each Jev question with the
exact state it was asked about and every option's probability, and each
router consult with its frame (session.rvlog), the game as observed each
turn (observations.jsonl), the frames kept (frames/), the marks, and links
to the host's own logs (logs/). Kinds can be shown or hidden, and every
row opens to its full data. The page is self-contained apart from the
frames beside it.
"""
from __future__ import annotations

import argparse
import html
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rec  # noqa: E402

REPORT = 'timeline.html'
KINDS = ('input', 'check', 'mark', 'decision', 'jev', 'router', 'observation', 'frame')


def _lines(path: Path) -> list[dict]:
    if not path.exists():
        return []
    out = []
    for text in path.read_text(encoding='utf-8').splitlines():
        try:
            out.append(json.loads(text))
        except json.JSONDecodeError:
            continue
    return out


def _esc(value) -> str:
    return html.escape(str(value), quote=True)


def _details(summary: str, data) -> str:
    return (f'<details><summary>{summary}</summary><pre>'
            f'{_esc(json.dumps(data, indent=1, ensure_ascii=False))}</pre></details>')


def _bar(p: float) -> str:
    return f'<span class="bar" style="width:{round(max(0.0, min(1.0, p)) * 80)}px"></span>'


def _jev(entry: dict) -> str:
    parts = []
    for name, answer in (entry.get('answers') or {}).items():
        kind = answer.get('type')
        if kind == 'choice':
            meaning = ((entry.get('questions') or {}).get(name) or {}).get('criteria') or {}
            probs = sorted((answer.get('probabilities') or {}).items(), key=lambda kv: -kv[1])
            ranked = ''.join(f'<div class="opt{" won" if o == answer.get("choice") else ""}">'
                             f'{_bar(p)} {p:.2f} {_esc(o)} <small>{_esc(meaning.get(o, ""))}</small></div>'
                             for o, p in probs)
            parts.append(f'<div class="q"><b>{_esc(name)}</b> &rarr; {_esc(answer.get("choice"))} '
                         f'<small>confidence {answer.get("confidence", 0):.2f}</small>{ranked}</div>')
        elif kind == 'noul':
            parts.append(f'<div class="q"><b>{_esc(name)}</b> yes {_bar(answer["noul"])} {answer["noul"]:.2f}</div>')
        else:
            parts.append(f'<div class="q"><b>{_esc(name)}</b> score {answer.get("score")}</div>')
    head = (f'{_esc(entry.get("tag") or "decision")} &middot; {entry.get("seconds", 0):.2f} s'
            f' &middot; ${entry.get("cost") or 0:.6f}')
    return head + ''.join(parts) + _details('the state and the questions',
                                            {'state': entry.get('state'), 'questions': entry.get('questions')})


def _observation(obs: dict) -> str:
    player = obs.get('player') or {}
    enemies = [o for o in obs.get('objects') or [] if o.get('enemy')]
    bits = [f'screen {obs.get("screen")}', 'control' if obs.get('control') else 'no control',
            f'level {player.get("level")} at {player.get("pos")}', f'hp {player.get("health")}',
            f'{player.get("mode")} / {player.get("action_name")}']
    if enemies:
        e = enemies[0]
        bits.append(f'nearest enemy {e.get("type") or e.get("name")} hp {e.get("health")} '
                    f'{e.get("action_name")}@{e.get("action_tick")}')
    if obs.get('messages'):
        bits.append('messages: ' + ' | '.join(obs['messages']))
    return _details(_esc(' · '.join(str(b) for b in bits)), obs)


def rows(bundle: Path) -> tuple[dict, list[tuple[int, int, str, str]]]:
    """The header and (tick, order, kind, html) rows."""
    out: list[tuple[int, int, str, str]] = []
    header: dict = {}
    order = 0

    def add(t, kind, body):
        nonlocal order
        out.append((int(t or 0), order, kind, body))
        order += 1

    recording = rec.recording_path(bundle)
    if recording.exists():
        recorded = rec.Recording.load(recording)
        header = recorded.header
        for item in recorded.items:
            if isinstance(item, (rec.Key, rec.Char, rec.Mouse, rec.Wheel)):
                add(item.t, 'input', _esc(json.dumps(item.line())))
            elif isinstance(item, rec.Check):
                add(item.t, 'check', _esc(' · '.join(f'{k} {v}' for k, v in item.fields.items())))
            elif isinstance(item, rec.Mark):
                add(item.t, 'mark', f'<b>{_esc(item.kind)}</b> {_esc(json.dumps(item.extra))}')
            elif isinstance(item, rec.End):
                add(item.t, 'mark', '<b>end</b>')
    for entry in _lines(bundle / 'director.jsonl'):
        if entry.get('kind') == 'mode':
            continue
        fields = {k: v for k, v in entry.items() if k not in ('t', 'kind')}
        add(entry.get('t'), 'decision', f'<b>{_esc(entry.get("kind"))}</b> {_esc(json.dumps(fields))}')
    for entry in _lines(bundle / 'session.rvlog'):
        if entry.get('kind') == 'router':
            images = ''.join(f'<img src="{_esc(Path(p).relative_to(bundle) if Path(p).is_relative_to(bundle) else p)}">'
                             for p in entry.get('images', []))
            add(entry.get('t'), 'router',
                f'{_esc(entry.get("tag"))} &middot; {_esc(entry.get("model"))}<br>{_esc(entry.get("content"))}'
                f'<br>{_esc(json.dumps(entry.get("tool_calls")))}{images}' + _details('prompt', entry.get('prompt')))
        else:
            add(entry.get('t'), 'jev', _jev(entry))
    for obs in _lines(bundle / 'observations.jsonl'):
        add(obs.get('t'), 'observation', _observation(obs))
    for frame in sorted((bundle / 'frames').glob('t*.png')):
        add(int(frame.stem[1:]), 'frame', f'<img src="frames/{_esc(frame.name)}" loading="lazy">')
    return header, sorted(out)


PAGE = """<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>{title}</title><style>
:root {{ --bg:#fff; --text:#1d1d1f; --dim:#6e6e73; --line:#e5e5ea; --accent:#9a6a12; --bar:#3a9a4a; --tag:#f2f2f7; }}
@media (prefers-color-scheme: dark) {{ :root {{ --bg:#141416; --text:#e8e8ea; --dim:#9a9aa0; --line:#2a2a2e;
  --accent:#d4a24c; --bar:#6cc070; --tag:#222226; }} }}
body {{ margin:0; background:var(--bg); color:var(--text); font:13px/1.45 -apple-system, system-ui, sans-serif; }}
header {{ padding:16px 20px; border-bottom:1px solid var(--line); }}
h1 {{ margin:0 0 6px; font-size:18px; }} .meta {{ color:var(--dim); }}
.filters {{ position:sticky; top:0; background:var(--bg); padding:8px 20px; border-bottom:1px solid var(--line);
  display:flex; gap:14px; flex-wrap:wrap; z-index:1; }}
table {{ border-collapse:collapse; width:100%; }}
td {{ padding:5px 10px; border-bottom:1px solid var(--line); vertical-align:top; }}
td.t {{ color:var(--dim); white-space:nowrap; font-variant-numeric:tabular-nums; width:90px; }}
td.k {{ width:90px; }} td.k span {{ background:var(--tag); border-radius:4px; padding:1px 6px; }}
a {{ color:var(--accent); }} tr.check td:last-child {{ color:var(--dim); font-variant-numeric:tabular-nums; }}
tr.decision td:last-child {{ font-weight:500; }}
tr.mark td {{ background:color-mix(in srgb, var(--accent) 12%, transparent); }}
img {{ max-width:320px; display:block; margin-top:4px; image-rendering:pixelated; }}
pre {{ white-space:pre-wrap; max-height:360px; overflow:auto; font-size:12px; }}
.bar {{ display:inline-block; height:6px; background:var(--bar); border-radius:3px; vertical-align:middle; }}
.opt {{ color:var(--dim); }} .opt.won {{ color:var(--text); font-weight:600; }} .q {{ margin:3px 0; }}
details summary {{ cursor:pointer; }}
</style></head><body>
<header><h1>{title}</h1><div class="meta">{meta}</div><div class="meta">{links}</div></header>
<div class="filters">{filters}</div>
<table>{rows}</table>
<script>
document.querySelectorAll('.filters input').forEach(box => box.addEventListener('change', () => {{
  document.querySelectorAll('tr.' + box.value).forEach(row => row.style.display = box.checked ? '' : 'none');
}}));
document.querySelectorAll('.filters input:not(:checked)').forEach(box => box.dispatchEvent(new Event('change')));
</script></body></html>
"""


def write(bundle: Path) -> Path:
    bundle = Path(bundle)
    header, body = rows(bundle)
    counts: dict[str, int] = {}
    for _, _, kind, _ in body:
        counts[kind] = counts.get(kind, 0) + 1
    title = f"{header.get('game', '?')} session, {header.get('start', {}).get('from', '?')} " \
            f"{header.get('start', {}).get('slot', '')}".strip()
    build = header.get('build', {})
    meta = (f"build {_esc(build.get('name'))} {_esc(str(build.get('sha', ''))[:12])} &middot; seed {header.get('seed')} "
            f"&middot; timing {header.get('timing')} &middot; " + ' &middot; '.join(f'{n} {k}' for k, n in sorted(counts.items())))
    links = ' &middot; '.join(f'<a href="logs/{p.name}">{p.name}</a>' for p in sorted((bundle / 'logs').glob('*')))
    filters = ''.join(f'<label><input type="checkbox" value="{k}"{"" if k in ("input", "observation") else " checked"}> '
                      f'{k} ({counts.get(k, 0)})</label>' for k in KINDS)
    table = ''.join(f'<tr class="{kind}"><td class="t">{t / rec.TICK_HZ:8.2f} s<br><small>t {t}</small></td>'
                    f'<td class="k"><span>{kind}</span></td><td>{cell}</td></tr>' for t, _, kind, cell in body)
    path = bundle / REPORT
    path.write_text(PAGE.format(title=_esc(title), meta=meta, links=links or 'no host logs', filters=filters,
                                rows=table), encoding='utf-8')
    return path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('bundle', type=Path)
    print(write(parser.parse_args().bundle))


if __name__ == '__main__':
    main()
