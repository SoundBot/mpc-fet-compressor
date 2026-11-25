# Releasing FET Compressor

Releases go to GitHub (SoundBot/mpc-fet-compressor) and are listed in the
[OpenMPC catalog](https://open-mpc.github.io/). Tag format: `vX.Y.Z`.

## 1. Version

Bump `version` in `plugin.json` and `info.version` in `src/FETComp.cpp` (1.4.1 → `1410`), and add a
`CHANGELOG.md` entry. Patch for fixes, minor for new parameters or pages. Never change the `uid`
(`EGVo`), parameter ids or parameter order: saved projects depend on them. Add new parameters
after `GR Meter`, and new presets at the end of the list.

## 2. Build the release (CI)

1. In `.github/workflows/release.yml`, move both pins (`uses: …@<sha>` and `tools_ref`) to the
   current `main` of [open-mpc/mpc-vst-plugins](https://github.com/open-mpc/mpc-vst-plugins).
2. Actions → **Release** → run with the version, `dry_run` on. Check the log: both builds, and
   `catalog_check.py --catalog` OK for the per-CPU zips.
3. Run again with `dry_run` off. This makes a **draft** release with
   `FET-Compressor-<v>-mpc-armv7.zip`, `-mpc-aarch64.zip` and `-mpc-sdk.zip`.

CI builds with the SDK pinned in OpenMPC's repo, so the plugin must not depend on local SDK
changes. Preset refresh is handled in the plugin itself for this reason.

To build the same zips locally (as a check, or to attach by hand), with an OpenMPC checkout in
`$OPENMPC`:

    python3 "$MPC_SDK/tools/build.py" .
    python3 "$OPENMPC/tools/sdk_release.py" "build/Segeda - VST - FET Compressor" --version 1.4.1 \
        --id segeda-fet-compressor --repo SoundBot/mpc-fet-compressor --license MIT -o dist
    python3 "$OPENMPC/tools/sdk_zip.py" dist/FET-Compressor-1.4.1-mpc-sdk.zip \
        dist/FET-Compressor-1.4.1-mpc-armv7.zip dist/FET-Compressor-1.4.1-mpc-aarch64.zip
    python3 "$OPENMPC/tools/catalog_check.py" dist/FET-Compressor-1.4.1-mpc-armv7.zip --catalog

## 3. Test the draft's zips on hardware

Install the draft's own zips: aarch64 on a Live III, and armv7 on a 32-bit MPC if you have one.
Check that the plugin loads, that the meter needle moves, that presets update the panel, and
that it's quiet when bypassed. Optional: measure CPU with OpenMPC's `tools/bench.sh` and commit
the output as `bench.txt` at the repo root, then re-run the workflow.

Then add a line to `tested.json` (repo root, default branch), for example:

    [ { "id": "segeda-fet-compressor", "version": "1.4.1", "device": "MPC Live III (Gen2)",
        "firmware": "3.10.1", "date": "2026-11-25" } ]

A tested entry makes the catalog card "Verified" instead of "Listed".

## 4. Publish

Publish the draft. That creates the tag `v<version>`. The screenshot link in the catalog entry
points at that tag, so `docs/screenshots/segeda-fet-compressor.png` must be committed before it.

## 5. Catalog listing (first release only)

1. Fork open-mpc/mpc-vst-plugins and make a **branch**, not your fork's `main`.
2. Copy `docs/openmpc/segeda-fet-compressor.json` to `catalog/plugins/segeda-fet-compressor.json`.
   Touch nothing else (not `added.json`).
3. Run `python3 tools/catalog_build.py --check-registry`; it should report 0 problems.
4. Open one PR for this entry. Fill in the template: the `catalog_check.py --catalog` output, the
   SHA-256 of each zip, and what was tested on which device.

Later releases are picked up automatically, with no PR needed.
