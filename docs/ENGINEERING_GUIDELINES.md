# Engineering Guidelines

Rules for what lives in this repo and how it is built. Read before adding code.

## 1. Production code is optimized C/C++ — never Python

Anything that **runs on the seeker in production** — the camera driver, the
capture/ISP/AE datapath, exposure control, encode, fusion hot paths — is
**C/C++**, on the hardware blocks where they exist (Tegra VI/NVCSI, ISP, NVENC,
CUDA/VPI). No Python on the device datapath. No per-frame Python in the loop.

**Python is allowed for tools only** — offline focus/preview/bench/diagnostic
utilities that an engineer runs by hand, not part of the shipping pipeline.
Those live under `jetson/tools/` and are clearly labelled as tools.

> Example: the focus and preview tools (`jetson/tools/*.py`) drive exposure over
> i2c from Python — fine, they are bench tools. The **shipping** exposure/AE/ISP
> is implemented in the C driver and a C++ pipeline.

## 2. No loose patch files

Kernel/driver changes ship as **vendored source + a reproducible build script**,
not as `*.patch` files dropped in the tree. A patch is a diff against a moving
upstream; six months later it no longer applies and nobody knows why it existed.

- Prefer solving it **in our own driver**: e.g. mono **Y10** support belongs in
  `nv_imx296` advertising `MEDIA_BUS_FMT_Y10_1X10`, so no core `tegra-camera`
  change is needed at all.
- If a core-tree change is genuinely unavoidable, vendor the **whole modified
  file** under a clearly-named path and have the build script copy it in, with a
  one-line note in the bring-up checklist.

## 3. Repo layout

```
docs/                     guides + bring-up checklist (this folder)
jetson/camera/            production driver: nv_imx296.c, mode tables, DT overlay
jetson/fan/               fan-always-100% service + installer
jetson/tools/             bench tools (Python OK): focus, preview, diagnostics
```

On-device/production code → `jetson/<subsystem>/`. Bench tools → `jetson/tools/`.
Docs → `docs/`. Nothing else at the root.

## 4. GitHub is the single source of truth

One clean cloud repo is authoritative. Local checkouts and the Jetson are
**pull-only clones**. Every change is **commit → push → pull**. No `sed`, `scp`,
or hand-edits on the Jetson, ever — they get silently overwritten on the next
pull and the repo stops reflecting reality.

## 5. No bandaids

Build the architecturally correct version. Don't ship "stabilize now, fix
later" two-track solutions, opt-out flags, or 30-second-window workarounds. If
something is interim, say so explicitly and link the path to the real fix.
