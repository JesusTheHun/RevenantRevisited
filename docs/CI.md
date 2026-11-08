# Continuous integration

C++ changes pushed to GitHub, on any branch, get a version and are built
and tested on macOS, Windows and Linux. Packages that pass are published to
`https://downloads.revenantrevisited.com`. When C++ is unchanged, the three
required build checks report **skipped** without starting platform runners;
no version is claimed and no package is published.

- Workflow: [.github/workflows/ci.yml](../.github/workflows/ci.yml)
- Driver: [tools/ci/ci.py](../tools/ci/ci.py), one subcommand per workflow step
- Package README: [tools/ci/PACKAGE_README.md](../tools/ci/PACKAGE_README.md)
- Change selection: [tools/ci/changes.py](../tools/ci/changes.py)

The C++ inputs are `src/`, `thirdparty/`, `assets/`, `cmake/`, `tools/`,
`CMakeLists.txt`, `editor.def`, `.gitattributes`, `.gitmodules`, and the C++
workflow itself. Rust-only and documentation-only changes skip the C++ jobs.
On feature branches, selection compares the entire branch against its merge
base with `main`: a later Rust-only push cannot hide an earlier C++ failure.
On `main`, it compares the commits before and after the push. Missing comparison
history runs the build conservatively; a manual dispatch always runs it.

A pull request from a fork cannot run on push (its pushes land in the fork),
so the workflow also runs on the `pull_request` event, for fork pull requests
only: a pull request from a branch in this repository skips that run, since
its push run already reported. A fork run has the same three build checks,
claims no version (the fork token is read-only, so the build takes
`BuildInfo.cmake`'s local fallback) and publishes nothing; its packages are
the run's artifacts.

The separate [Rust workflow](../.github/workflows/rust.yml) runs for `rust/`
changes, its own workflow changes, and `src/mappane.cpp`, which its terrain
comparison test compiles. It runs once per PR update and on pushes to `main`,
with a manual dispatch available. Other C++ changes do not start Rust CI.

## 1. Jobs

| Job | Runs on | Does |
|---|---|---|
| `Version` | `ubuntu-24.04` | selects C++ changes, then claims a version if needed (§6) |
| `Build & test (osx)` | `macos-latest` (Apple silicon) | configure, build, test, package |
| `Build & test (windows)` | `windows-latest` | the same |
| `Build & test (linux)` | `ubuntu-24.04` (x86-64) | the same |
| `Publish & report` | `ubuntu-24.04` | uploads passing packages to R2, writes the run summary |

A push to a branch other than `main` cancels that branch's run in flight;
runs on `main` are never cancelled, so each C++ change there gets its archive.
`workflow_dispatch` reruns any branch from the Actions tab.

### Toolchains

Clang everywhere, so warnings and language extensions match the macOS
development builds:

| Platform | Compiler | Graphics | Notes |
|---|---|---|---|
| macOS | Apple Clang | Metal | `CMAKE_OSX_DEPLOYMENT_TARGET=13.0` |
| Windows | clang-cl, MSVC libraries | Direct3D 11 | static C runtime (`/MT`) |
| Linux | Clang, libstdc++ | OpenGL 3.3 | X11 + ALSA |

All three build `RelWithDebInfo` with Ninja, the configuration the
development checkouts use. `CMakeLists.txt` pins `char` to signed on every
compiler (the 1998 code was written for MSVC on x86) and defines `NOMINMAX`
on Windows.

## 2. Steps

The workflow calls `tools/ci/ci.py`; each step leaves a record in
`ci-out/<step>.json`, and the report steps work from those records. The
same commands run locally:

```sh
python3 tools/ci/ci.py configure --platform osx   # cmake -G Ninja into build/
python3 tools/ci/ci.py build                      # cmake --build; counts warnings
python3 tools/ci/ci.py test                       # ctest, JUnit to ci-out/
python3 tools/ci/ci.py package --platform osx     # dist/RevenantRevisited-osx.zip
python3 tools/ci/ci.py report --platform osx      # ci-out/report-osx.json
```

`version` runs in its own job before the builds (§6), `publish` and
`summary` in the publish job. Locally there is no claimed version and the
build works one out from the tags.

## 3. Tests: the merge gate

`ctest --test-dir build` runs every test registered at the end of
`CMakeLists.txt`:

- plain executables: pass when they exit 0;
- GoogleTest executables, each case its own test
  (`test_particlefx.ParticleFxDefs.TorchFlame`);
- the Python source-slice tests (`tools/test_*.py`), which compile a
  function cut out of `src/` into a fixture. POSIX hosts only: Windows
  skips them.

To add a test, add its executable as usual and its name to one of the
lists there. A test must pass without the retail install. One that needs
game data checks `REVENANT_DATA_PATH` and skips itself when it is unset
(`GTEST_SKIP()`), as the shipped-voice cases in `test_audio_decode` do.
CI has no game data.

On Windows, `test_d3d11_shaders` compiles the public HLSL stages and checks
tile projection and composite tint on Direct3D's WARP software device. It
needs no window, hardware GPU, or retail data. Add new public shader stages
to its `ShaderStages` list so runtime compilation stays covered by CI.

`test_fx_source_alpha.py` is registered but disabled: its fixture predates
the prelit RGBA5 path in `TRenderer::SubmitFxQuad`. Reports list it as
disabled until the VFX track brings it up to date.

`cpu_tile_perspective_test` is a diagnostic image generator, not a test,
so it is built but not registered.

## 4. Packages

Each platform's zip holds:

```
RevenantRevisitedOSX        (RevenantRevisitedWindows.exe, RevenantRevisitedLinux)
assets/                     engine-owned data: effects.def, render_metadata.def, fonts, editor icons
README.md                   tools/ci/PACKAGE_README.md, filled in for the platform and commit
```

The program is renamed so the zip can be unpacked into the root of a
retail install (the README asks players for a copy of theirs) without
touching the game's own `Revenant.exe`; run from there it shares the
folder's `Revenant.ini` and saves, as retail does (docs/DATA_LAYOUT.md
§1). The Linux binary ships with its debug sections
stripped. Zip entries carry the commit's timestamp, so packing the same
build twice gives identical bytes.

Only `assets/**` (fonts, editor icons) is fetched from Git LFS, through
an Actions cache keyed on those files' object ids: a run downloads from
LFS, whose bandwidth is metered, only when the assets change. Nothing
else in LFS (screenshots, captures) is read by the build or the tests.

## 5. Downloads

Bucket `revenant` on Cloudflare R2, served at
`https://downloads.revenantrevisited.com` (`download.revenantrevisited.com`
serves the same bucket):

| Path | What | Cache |
|---|---|---|
| `<hash>/RevenantRevisited-<platform>.zip` | every passing build, on every branch | immutable, one year |
| `<platform>/latest/RevenantRevisited-<platform>.zip` | the newest build of `main` that passed on all three platforms | `no-cache` |

`<hash>` is the commit's 7-character short hash, as GitHub shows it;
if another commit already holds that folder, publish lengthens it the way
git lengthens an ambiguous abbreviation. `<platform>` is `osx`, `windows`
or `linux`. A package is published only
when its build and tests passed. `latest/` moves for all three platforms
together, only when the commit passed on all three and is still the tip of
`main` at publish time (a slow run never overwrites a newer one). The three
`latest/` links therefore always name the same commit; a commit that fails
anywhere leaves them all where they were.

The archived object's metadata records the commit, branch, CI run and
SHA-256.

## 6. Versions

Every CI build, on any branch, gets the next version: the `Version` job
finds the highest `vX.Y.Z` tag on GitHub (numerically), adds one to its
last number, and pushes that tag onto the commit as an annotated tag
(`v0.0.1` comes first). Pushing the tag is the claim: if a build running
alongside took the number first, the push is refused and the job takes the
next one. A commit has one version: a rerun, or the same commit pushed to
another branch, keeps the tag it already has. Failed builds keep their tags
too, so the published versions can have gaps. To move to a new series, tag
a commit by hand (say `v0.2.0`); builds continue from it (`v0.2.1`, …).

The builds carry the version (`-DREVENANT_VERSION`, into `buildinfo.h` with
the branch and the 7-character commit hash), the main menu shows it in the
lower left (`RevenantRevisited v0.0.3 main ab84cff`, where retail showed
`Revenant v1.22`), and the package README and the run summary name it. A
local build without CI shows the tag on its commit, else the newest tag it
descends from plus `-dev`.

If the `Version` job fails, the builds still run with that local fallback:
a failed change detector must not turn into passed/skipped build checks.

## 7. The run summary

The `Publish & report` job writes the run's summary page, which also appears
in its log:

- one row per platform: build result, test counts, unique warnings, build
  time, download links;
- download URLs, and whether `latest/` moved;
- per platform: runner image, compiler, CMake, step times, package size and
  SHA-256, failed targets and the first build errors, warnings by kind, each
  failing test's output, and the full test list.

Download links and failures are also posted as annotations at the top of the
run.

## 8. Protecting `main`

A repository ruleset on `main`:

- changes arrive through pull requests, from everyone, administrators
  included; approvals are not required;
- `Build & test (osx)`, `Build & test (windows)` and `Build & test (linux)`
  must pass on the pull request's head commit, or report skipped when the
  branch has no C++ changes; for a fork's pull request they come from the
  `pull_request` run;
- the branch must be up to date with `main` (merge `main` in, push, let CI
  pass);
- no force pushes, and `main` cannot be deleted.

The checks come from the push of the branch, so a pull request is ready to
merge once its branch's latest push has passed. Pull requests from forks
don't trigger the workflow; a maintainer pushes the fork's branch to this
repository to run it.

The C++ workflow starts on every branch push so required checks can report a
result. It conditionally skips each named job when C++ inputs are unchanged;
filtering out the entire workflow would leave required checks pending. The
three jobs have static names and share their build steps through a YAML anchor,
so skipping them preserves all three required check names.

## 9. Secrets

Repository secrets used by the publish job:

The `Version` job pushes tags with the workflow's own token (`contents:
write` for that job only); the builds read the repository only.

| Secret | Use |
|---|---|
| `R2_ACCOUNT_ID` | R2 S3 endpoint `https://<account>.r2.cloudflarestorage.com` |
| `R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY` | bucket-scoped R2 key (object read and write) |

`R2_API_TOKEN` is also stored but unused: the bucket key can't administer
the bucket or its domains, which is done in the Cloudflare dashboard.

## 10. Known gaps

- GPU readback (`RendererReadback`) exists only for Metal. On Windows and
  Linux the stubs in `src/platform/` report it as unsupported, so
  `--snap`, `--filmstrip` and editor picking do nothing there.
- The editor's window resize is macOS-only; elsewhere the window keeps its
  size (`src/platform/editorplatform_stub.cpp`).
- Windows and Linux builds are compiled and unit-tested on every C++ change,
  but the game is played on macOS.
