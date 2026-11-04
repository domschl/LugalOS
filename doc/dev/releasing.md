# Making a Release

[Documentation](../README.md) > [System Programmer Manual](index.md) > **Making a Release**

---

A release is a version number, a tag and a GitHub release, made from a tree that has passed the suites and whose images have been installed on the boards.

## 1. The version

* `kernel/include/kernel/version.h` is the only place the version lives: `LUGALOS_VERSION_MAJOR`, `_MINOR`, `_PATCH` and `LUGALOS_VERSION`. The boot banner, `/proc/version` and `tests/runner.py` read it from there.
* The plan files of the phases the release completes say so in their status line (for example `**Status: CLOSED 2026-11-04, version 0.19.0.**`).
* Commit the bump on its own (`release: version X.Y.Z`).

## 2. A clean tree, a clean build id

Every image carries a build id, `COUNT.HASH`, with a `+` when the tree had uncommitted or untracked changes (`cmake/gen_build_id.cmake`). Release images must not have the `+`: `git status --short` must print nothing (build products and the host toolchain packages are covered by `.gitignore` files). Then build every preset:

```bash
for d in build/*/; do ninja -C "$d"; done
```

## 3. Tests

* `python3 tests/runner.py`: everything passes, including the static checks (`racescan`, `sizereport`, `servecheck`) and the folded twin.
* The hardware suites on the boards that are attached ([Testing Guide](testing.md#5-tier-4-hardware-in-the-loop-silicon-suites-testshw)).
* `python3 tools/gendocs.py --check`: no drift between `doc/` and the on-device help.

## 4. The boards

Install the release images with the normal update path ([Updating Nodes](../admin/updates.md)), so the release is also a test of it:

```bash
uv run tools/lota.py push build/esp32c6-mini --p4 lcd7b --prefix /c6 --reboot --wait   # a P4's C6
uv run tools/lota.py push-board build/esp32p4 --p4 nano                                # a P4 and its C6
```

and check the result with `lota.py status` and the board's `version`.

## 5. Tag and publish

The tag is annotated and named by the version alone, without a `v`:

```bash
git tag -a X.Y.Z -m "LugalOS X.Y.Z"
git push origin main X.Y.Z
gh release create X.Y.Z --title "X.Y.Z - TITLE" --notes-file NOTES.md
```

The release notes lead with what the release is for, then list the rest by area, the fixes found along the way, and what was tested on which boards.
