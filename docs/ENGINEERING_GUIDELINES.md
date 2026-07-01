# Engineering Guidelines

**Read this before touching the repo. Every contributor — human or agent — follows it.**
It is linked first from the [README](../README.md) and the
[System Overview](SYSTEM_OVERVIEW.md) for that reason.

## 1. The cloud repo is the only workspace — never work locally on a workstation

The GitHub repo is the single source of truth. **Do not clone, download, or edit
this repo on a shared workstation (e.g. the office Windows machine).** Work in an
environment that commits and pushes back to the cloud — the target device's clone
or a dedicated dev/remote environment — and always through **commit → push →
pull**.

*Why:* a local working copy on a shared machine is how discipline breaks. An agent
finds the local copy, edits it, runs from it, never pushes — the guidelines get
skipped and the repo stops reflecting reality. No local copy, no shortcut.

- Clones (the Jetson, any dev env) are **pull-first**. Change source in the repo,
  push, then pull on the device. Never leave a change living only on a clone.
- No `sed`/`scp`/hand-edits to make a device diverge from the repo.

## 2. Documentation describes the system — it is not a development diary

Docs exist so a new engineer or agent gets context fast. Write what the system
**is** and how to run it — not the story of how it was built or what went wrong.

- Capture only the non-obvious **pitfalls** needed to stop someone repeating a
  mistake, as short "> Pitfall:" notes — not a play-by-play of the debugging.
- No narratives, no "first we tried X then Y." Keep every doc short. This project
  will have many iterations; long stories rot.
- If a fact matters to prevent a future error, it belongs in the docs — concisely.

## 3. Production code is optimized C/C++ — never Python

Anything that **runs on the seeker in production** — the camera driver, the
capture/ISP/AE datapath, exposure control, encode, fusion/tracking hot paths — is
**C/C++**, on the hardware blocks where they exist (Tegra VI/NVCSI, ISP, NVENC,
CUDA/VPI). No Python on the device datapath.

**Python is allowed for tools only** — offline focus/preview/bench/diagnostic
utilities an engineer runs by hand, clearly labelled and kept out of the shipping
pipeline (e.g. `eo/tools/`).

## 4. No loose patch files

Kernel/driver changes ship as **vendored source + a reproducible build script**,
not `*.patch` files. A patch is a diff against a moving upstream; months later it
no longer applies and nobody knows why it existed. Prefer solving it in our own
driver (e.g. mono **Y10** support lives in `nv_imx296` advertising
`MEDIA_BUS_FMT_Y10_1X10`, so no core-tree change is needed).

## 5. No bandaids

Build the architecturally correct version. Don't ship "stabilize now, fix later"
two-track solutions, opt-out flags, or timed workarounds. If something is interim,
say so explicitly and link the path to the real fix.

## 6. Repo layout — one folder per module

```
docs/            system-level docs: SYSTEM_OVERVIEW, this file
jetson/          compute-platform bring-up (flash, base config, fan) — not a module
eo/              electro-optical module: camera driver, tools, streaming, EO docs
illuminator/     NIR illuminator module: controller src + docs
radar/ …         future modules (detection, fusion, tracking, gimbal, guidance)
```

Each module folder owns its code **and** its docs (a `README.md` chapter, plus a
`docs/` subfolder for detail). System-wide docs live in top-level `docs/`. On-device
code is C/C++; bench tools (Python OK) sit in the module's `tools/`.
