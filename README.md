# RevenantRevisited

RevenantRevisited is a retail-faithful version of *Revenant*, the 1999 Windows PC game released by Eidos Games. This repository contains the game's engine source code and links to executable builds of the engine only; it does not include the complete game.

You must purchase and install the original game for the engine to function. *Revenant* is available from [GOG](https://www.gog.com/en/game/revenant).

The original source code comes from my own archives. I was the original lead engineer and creator of *Revenant* and a co-founder of Cinematix Studios.

> **Alpha software.** Builds are made automatically from work in progress. Testers are welcome: please file bug reports, with screenshots, in [GitHub issues](https://github.com/benjcooley/RevenantRevisited/issues). Latest builds: [macOS](https://downloads.revenantrevisited.com/osx/latest/RevenantRevisited-osx.zip) · [Windows](https://downloads.revenantrevisited.com/windows/latest/RevenantRevisited-windows.zip) · [Linux](https://downloads.revenantrevisited.com/linux/latest/RevenantRevisited-linux.zip).

`RevenantRevisited` is an in-progress modernization of the original 1998 **Revenant** codebase for contemporary machines. The project keeps the original game data and gameplay intent, while replacing old platform dependencies with a modern build and rendering stack.

The current port already runs the game data, loads archived map sectors from the shipped module files, and includes an actively developed modern sector renderer with deferred lighting, shadows, ambient occlusion, and debug tooling.

## Project Goals

- Preserve the original game content and behavior wherever practical
- Make the source buildable on modern systems with CMake
- Replace obsolete DirectX-era rendering/input assumptions with modern platform code
- Expose internal systems through focused test modes so rendering and data-loading work can be verified quickly

## Fullscreen

Press F11 or Alt+Enter to switch between fullscreen and windowed mode.
Start with `--fullscreen` to open fullscreen, or `--windowed` for a window.
When both switches are supplied, the last one takes precedence.

## Current Highlights

- Cross-platform CMake-based build
- Modern GPU-backed renderer
- Deferred directional lighting
- Screen-space contact shadows
- Screen-space ambient occlusion
- Archive-backed level scanning and sector loading from the shipped Ahkuilon module
- Debug UI tabs for renderer tuning and inspection

## Screenshots

Some current captures from the modern renderer:

- [Dynamic shadows](images/revenant-dynamic-shadows.png)
- [Dynamic shadows 2](images/revenant-dynamic-shadows-2.png)
- [Lit Misthaven tower](images/revenant-lit-misthaven-tower.png)
- [Lit house scene](images/revenant-lit-house.png)
- [Dead sea monster](images/revenant-dead-seamonster.png)
- [Puppet show](images/revenant-puppet-show.png)

## Repository Layout

```
RevenantRevisited/
├── src/             ← game source — the modernized engine
├── data/            ← user's existing Revenant install (dev only — symlink
│                      or copy of a GoG/CD install). Shipped builds DO NOT
│                      include this; the user provides their own legally-
│                      owned copy. Kept stock: nothing the port authors
│                      lives here (docs/DATA_LAYOUT.md has the inventory).
├── assets/          ← engine-owned runtime data the port authors or supplies
│                      and needs in every mode (effects.def,
│                      render_metadata.def, editor icons, fonts/). Ships
│                      beside the binary.
├── revisited/       ← our enhancement layer. Sibling to src/. Strictly
│   │                  opt-in: empty/missing means the engine plays vanilla
│   │                  retail. See revisited/README.md for the contract.
│   ├── resources/   ← runtime overlay files (mirrors data/ tree). Mounted
│   │                  ahead of the user's pack so any file present here
│   │                  shadows its retail counterpart. In production these
│   │                  files are zipped into RevenantRevisited.rvr and
│   │                  shipped beside the per-platform binary.
│   ├── srcassets/   ← higher-resolution textures, .blend / .max sources,
│   │                  raw audio, source scripts. Inputs to the build
│   │                  pipeline; never read by the engine.
│   └── builders/    ← scripts that turn srcassets/ into resources/, and
│                      pack resources/ into RevenantRevisited.rvr.
├── recon/           ← Ghidra reverse-engineering notes, decompile drops,
│                      class mappings, port-status tracking, design docs.
├── thirdparty/      ← embedded dependencies (sokol, miniz, ImGui, …).
├── tools/           ← test harnesses (test_parse, test_anim_system, …).
├── docs/            ← architecture notes and subsystem deep-dives.
├── images/          ← screenshots and visual references.
└── legacy/          ← original 1998 source materials (read-only reference).
```

### Where things live at runtime

The engine resolves three platform-aware paths at boot:

| Path     | Contents                                          | Default location                                                     |
|---|---|---|
| RunPath  | User's existing Revenant install (read-only)      | env `$REVENANT_DATA_PATH`, else exe-dir / cwd / repo `data/` heuristic |
| SavePath | User saves, INI, prefs (writable)                 | env `$REVENANT_SAVE_PATH`, else the install when the program sits in it (retail's hard-drive install: settings and saves shared with the original game), else macOS `~/Library/Application Support/Revenant/` · Linux `$XDG_DATA_HOME/Revenant/` · Windows `%LOCALAPPDATA%\Revenant\` |
| Overlay  | Our enhancement pack (optional)                   | env `$REVENANT_REVISITED_PATH`, else `<exe-dir>/RevenantRevisited.rvr`, else dev `<repo>/revisited/resources/` |
| Assets   | Engine-owned data (required)                      | env `$REVENANT_ASSETS_PATH`, else `<exe-dir>/assets/`, else `.app` `Resources/assets/`, else dev `<repo>/assets/` |

`rev_fopen` reads walk **SavePath → Overlay → RunPath**. A mounted pack
answers for the directory it names (`resources.rvr` for `Resources\…`,
`Modules/Ahkuilon.rvm` for `Modules\Ahkuilon\…`) and, as in retail, wins
over a loose file at the same path unless the caller asks for loose-first.
User saves win over overlay; overlay wins over original assets. Overlay is
read-only — writes never go there. Details and the retail forensics:
[docs/DATA_LAYOUT.md](docs/DATA_LAYOUT.md).

The engine refuses to run if SavePath isn't writable (no installs on
read-only media). It runs fine without the overlay (vanilla retail).

### Fonts

The TrueType faces the port draws with are engine assets, kept in
`assets/fonts/` beside their licence files. Arimo and Tinos are the
metric-compatible stand-ins for retail's Arial and Times New Roman (the
text bar, dialog, HUD numbers and .def screens draw with them); Inter,
JetBrains Mono and Material Symbols serve the editor and capture labels.
They belong to Assets rather than the Overlay because Classic mode draws
with them, and the engine must run without the opt-in overlay.

Every load names a face by file name and asks one resolver,
`TTFFilePath("Arimo-Regular.ttf")` in `src/font.h`, which returns
`rev_engine_asset("fonts/<file>")`. The faces are therefore found from
any working directory: `<exe-dir>/assets/fonts/` in a GOG install, the
`.app` bundle's `Resources/assets/fonts/`, or, in development, the
source tree's `assets/fonts/` above `build/`. They ship as loose files
and are read with `fopen` like every engine asset, so no pack or VFS
read is involved. If the overlay ever carries an alternate face (the
"Enhanced UI" category in revisited/README.md), that lookup belongs in
`TTFFilePath`, ahead of the asset.

### SavePath layout

```
<SavePath>/
├── Revenant.ini             ← user prefs (paths, key bindings, lighting, [Revisited] toggles)
├── imgui.ini                ← ImGui debug-pane layout
├── curmap/                  ← active session sector state (TSector::Save target).
│                              Cleared / repopulated by LoadGame; safe to delete
│                              between sessions.
├── Save/                    ← named save slots
│   ├── Single/<slot>/{game.sav, ss.bmp, CurMap/*.DAT}
│   ├── Multi/<slot>/…
│   └── Chars/<slot>/…
└── cache/                   ← engine-converted GoG assets (regeneratable — see below)
```

### Cache (`<SavePath>/cache/`)

Some retail assets aren't in a form our modern engine can use directly
(different renderer pipeline, different sector format, unpacked vs.
packed conventions, etc.). Rather than ship pre-converted versions
(can't legally — they're derived from the user's original assets),
the engine **converts on the fly** the first time it needs them and
caches the result here.

Properties:

- **Regeneratable.** Deleting `cache/` only costs CPU on the next boot;
  it's never the source of truth for anything the user owns.
- **Mirrors source layout** under `RunPath`. e.g. a converted
  `<RunPath>/Imagery/Misc/StillWater.I3D` lands at
  `<SavePath>/cache/Imagery/Misc/StillWater.<format>.bin`.
- **Stale detection:** mtime check against the source under RunPath.
  If the user updates their GoG install, affected entries regenerate.
  An engine-version stamp in `cache/_version.txt` invalidates the
  whole cache when our converters change format.
- **Never read by the overlay path.** This is engine-owned working
  storage, distinct from `revisited/` overrides.

Conversion isn't wired up yet (no converters needed for Demo 1); this
documents the convention so the first converter that lands has a place
to put its output.

## Distribution model

We ship per-platform binaries plus a single optional pack:

```
RevenantRevisitedOSX             ← our binary (one of these per OS)
RevenantRevisitedLinux
RevenantRevisitedWindows.exe
assets/                          ← required: engine-owned data (the repo's assets/)
RevenantRevisited.rvr            ← optional: enhancement overlay (zipped revisited/resources/)
```

The user unpacks these into the root of a copy of their Revenant install
(the package README asks for a copy until save interop with retail is
proven both ways). Run from there, Revisited keeps `Revenant.ini` and
`Save/` in that folder exactly as retail does, so it lists and opens the
saves already there. We **never ship original Cinematix assets** — the pack only carries
patches that need a real install to function (similar to a Doom WAD
needing an IWAD). Without the .rvr the binary plays vanilla retail
against the user's existing data.

## Building

The project currently builds with CMake.

```bash
cmake -S . -B build
cmake --build build
```

On macOS, the project links Metal/Cocoa/AppKit frameworks through `CMakeLists.txt`.

`ctest --test-dir build` runs the unit tests. Every pushed commit is built
and tested on macOS, Windows and Linux, and its packages are published to
downloads.revenantrevisited.com; merges into `main` go through a pull
request that passes all three. See `docs/CI.md`.

For the ASan + UBSan build, headless test runs and lldb setup, see
`docs/DEBUG_TOOLING.md`.

## Running

Run the executable from the repository root so the game can find `data/` and the runtime INI/log paths land where expected:

```bash
./build/Revenant
```

You can also start directly into a save:

```bash
./build/Revenant --loadmap=SAVEFILE.SAV
```

## Test Modes

The active development workflow uses `--test=<mode>` to jump directly into focused subsystem checks.

### Basic modes

- `--test=blank` — simple animated clear color
- `--test=ticker` — logs frame/time progression
- `--test=font` — logs bitmap font diagnostics
- `--test=text` — renders bitmap font atlas text
- `--test=icon` — shows the built `SystemFont` atlas
- `--test=ttf` — exercises stb_truetype atlas rendering
- `--test=ui` — builds and previews a packed bitmap atlas from 2D imagery
- `--test=i3d` — probes a few imagery resources and logs state/header info

### Sector / map renderer mode

This is the main graphics bring-up path right now:

```bash
./build/Revenant --test=sector
```

Useful sector-renderer variants:

```bash
./build/Revenant --test=sector --sector=0_2_25
./build/Revenant --test=sector --level=0
./build/Revenant --test=sector --level=1
./build/Revenant --test=sector --level=51
```

Notes:

- `--sector=L_X_Y` centers startup on the requested sector
- `--level=L` loads every discovered sector for that level from the archived module data and picks an occupied startup area
- if a requested `--level` has no sectors, startup now fails cleanly instead of opening an empty screen
- startup logs now summarize sector count, object count, and renderable tile totals for large level loads

## Useful Files

- `docs/DEFERRED_LIGHTING.md` — current lighting/depth pipeline notes
- `docs/FILE_FORMATS.md` — data format notes, including map sectors
- `docs/PORT_PLAN.md` — broader porting roadmap
- `docs/WORLD_MATH.md` — coordinate-space and projection notes

## Status

This is an active port/recovery effort, not a finished remaster. The codebase still contains a mix of original game code, modern platform replacements, and temporary test harnesses used to verify subsystems while the port is underway.

## Authorship

Original game and source material:

- **Benjamin Cooley** and Cinematix Studios

Original game publication:

- **EIDOS Interactive** (1998)

## License

No new license text has been added to this repository yet. Treat the code and shipped game assets accordingly until an explicit license is documented.
