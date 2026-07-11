# PHASE-3 FIRMWARE SPEC — ON-CHIP ANGLE-MOTION DETECTOR ("Build C")
**AWR2944P DDMA all-direction detection: the junction/highway crossing-vehicle blindness fix**
Spec date: 2026-07-10. Design-only; no files edited. Executes AFTER Build B (comb killer) merges.
SDK tree read (do not edit until Build B lands): `C:\ti\mmwave_mcuplus_sdk_04_07_02_01\`
Anchor chirp cfg: `C:\seeker-ground-station\radar\cfg\awr2944P_ag.cfg` (A/G, N=384 real ADC).

---

## 0. EXECUTIVE SUMMARY — the one architectural fact that changes the plan

The standing plan assumed the range-azimuth map must be *formed* (new HWA pass over the
zero-doppler slice of the compressed cube). **It does not.** In the objdethwaDDMA pipeline the
azimuth FFT is computed for **every range gate, every frame** — not per detected candidate:

- The Doppler DDMA DPU loops over ALL range gates two at a time (ping/pong) and, per gate, runs
  Doppler FFT → DDMA demod → **32-bin azimuth FFT over all doppler sub-bins** → doppler-CFAR →
  local-max (`dopplerprocDDMAcommon.h:85-116`, pipeline detail `:497-516`).
- The azimuth FFT output is **uint16 log2-magnitude**, layout `[dopSubBin][azimBin]`, sitting in
  HWA membank **M4 (ping) / M6 (pong)** for the whole extract window of that gate:
  - output banks: `dopplerprochwaDDMA.c:180,188` (`AZIMFFT_PING_OUT`=M4, `AZIMFFT_PONG_OUT`=M6)
  - sample type: `dopplerprochwaDDMA.c:213-215` (`AZIMFFTIO_OUTPUT` = real uint16)
  - layout proof — the CM4 extract code indexes it as
    `azimFFTMat[(AzimIdx + DopIdx * numAzimFFTBins) * 2]` (`dopplerprochwaDDMA.c:650-651`, `:913`)
- The **zero-doppler azimuth row of every range gate therefore already exists on-chip each frame**
  as the first 64 bytes of M4/M6 (row `dopSubBin==0` = 32 × uint16).

So Build C is NOT a new HWA pass. It is:
1. **one new SW-triggered EDMA channel** that side-copies the near-DC azimuth rows of each range
   gate (512 B/gate) from M4/M6 into an L3 heatmap `H[r][row][θ]` during the existing per-gate
   extract step (zero added HWA paramsets, zero decompression cost — the BFP-0.75 question is moot);
2. **~1.5 ms of CM4 C code per frame** (EMA clutter map `C[r,θ]`, residual `R = H_fold − C`,
   margin + azimuth-local-max detector) placed in the DPC where the CM4 currently idle-waits;
3. **one new TLV (type 10)** carrying ≤64 `(rangeIdx, azimuth, residual_snr)` points/frame;
4. **one new CLI command** `angleMotionCfg`, default OFF ⇒ byte-identical behavior when disabled.

The doppler path is untouched. No compression change. No paramset change. No frame-rate change.

---

## 1. WHERE THE RANGE-AZIMUTH HEATMAP COMES FROM (options evaluated)

### Chosen: (c') tap the existing per-gate azimuth-FFT output = a natively decimated 192×32 map

| Option | Verdict | Why |
|---|---|---|
| (a) New HWA pass over zero-doppler slice of the compressed radar cube | **REJECT** | The cube is BFP-compressed along range×rx per chirp (`objectdetection.c:3374-3391`); a zero-doppler slice does not exist until doppler FFT. You would have to re-decompress the whole cube (the decomp stage streams ALL chirps per outer block, `dopplerprocDDMAcommon.h:109-126`), redo a doppler FFT (or a chirp-sum) AND an azimuth FFT — a second copy of half the pipeline: ~10+ paramsets, several ms of HWA time, big L3 scratch. All to recompute data the azim stage already produces. |
| (b) Detection-matrix / pre-CFAR sum products | **REJECT** | The detection matrix is SumTx output `[rangeBins][dopSubBins]` uint16 (`objectdetection.c:3403`, `dopplerprocDDMAcommon.h:242-265`) — log-mag summed over ALL antennas. It has **no azimuth dimension**; cannot give (r,θ). |
| (c) Decimated range×azimuth map | **CHOSEN, for free** | The azim-FFT output *is* the decimated map: 192 range gates × 32 azimuth bins (`OBJECTDETECTION_NUM_AZIM_FFT_BINS`, `objectdetection.c:156`, forced at `:2717-2719`), uint16 log2-mag, computed per gate per frame. We only add the export EDMA. |

### Anchor-profile dimensions (awr2944P_ag.cfg)
- `numRangeBins` **Nr = 192** (384 real ADC samples → 192 bins; `objectdetection.c:2388-2395`,
  `mss_main.c:1920-1927`; range step 2.604 m, 500 m max — cfg header comments)
- `numChirpsPerFrame = numDopplerBins` **Nd = 768** (6 chirps × 128 loops, `frameCfg 0 5 128`; 3·2^8 as DDMA requires, `dopplerprocDDMAcommon.h:100-107`)
- `numBandsTotal` **B = 6** (4 TX + 2 empty; `mss_main.c:1933`)
- doppler sub-bins per band **Ns = Nd/B = 128** (`dopplerprochwaDDMA.c:576`)
- azimuth FFT bins **Na = 32**; azim-FFT output per gate = 128×32×2 B = **8 KB** of the 16 KB membank (`mmw_resDDM.h:59`)
- doppler bin width Δv ≈ λ/(2·Nd·Tc) ≈ **0.064 m/s** (Tc = 39.5 µs)

### What exactly is exported per gate
Near-DC rows, symmetric around zero doppler, width `W` (CLI, default 4, max 8):
- **head block**: rows `0 … W−1` → sub-bin velocities `0 … +(W−1)·Δv` — contiguous at membank offset 0, size `W·64` B
- **tail block**: rows `Ns−W … Ns−1` → velocities `−W·Δv … −Δv` — contiguous at offset `(Ns−W)·64`, size `W·64` B

W=4 covers |v_r| ≲ 0.26 m/s; W=8 ≲ 0.51 m/s. This is the exact blind region: doppler-CFAR runs
*along the doppler direction per azimuth column* (`dopplerprocDDMAcommon.h:346-354`), so cells at/near
DC are the CUT-inside-clutter cases it can never pass. 2·W rows × 64 B = **512 B/gate at W=4**.

Note on DDMA demod validity at DC: the azim row 0 content is demodulated using the per-sub-bin
winning-band shuffle LUT (`dopplerprocDDMAcommon.h:267-309`); every zero-radial-velocity scatterer in a
gate shares the same true band rotation, and at sub-bin 0 the LUT is driven by the dominant
(static-clutter) energy — i.e. the demod is *correct* for crossers sharing that cell. In empty
cells the band pick is noise-random, which only randomizes noise — handled by the clutter map.

---

## 2. MEMORY BUDGET (L3) — fits with margin at either compression ratio; no tradeoff needed

L3 pool: `DSS_L3_U_SIZE = 0x2FE000` = 3,137,536 B, bottom 0x1E000 = 122,880 B reserved for
persistent/config allocations (`dss_cm4_main.c:90-95`, pool init `:378-382`; bi-dir allocator
`objectdetection.c:553-596`).

Current top-pool (scratch/grow-up) users per preStart config (`objectdetection.c:3368-3412`, `:2817-2875`, `:3105-3154`):

| Buffer | Size (AG cfg, compression 0.5) | Cite |
|---|---|---|
| Radar cube (compressed) | 1,179,648 B (0.5× of 2,359,296 B decompressed = 192·768·4rx·4B) | `objectdetection.c:3386-3391` |
| Detection matrix | 192·128·2 = 49,152 B | `:3403-3405` |
| objOut (cartesian) | finalMaxNumDetObjs·16 B ≈ 15.9 KB | `:2816-2823` |
| sideInfo | ≈ 4 KB | `:2825-2831` |
| Decomp scratch | 2,359,296/(192/8) = 98,304 B | `:2854-2871` |
| rangeCfarList (500·sizeof(RangeCfarListObj)) + perDopplerBin | ≈ 6–16 KB | `:3104-3111,3145-3154`; `objectdetection.h:123` |

Total top ≈ **1.36 MB** at compression 0.5 → ≈ **1.65 MB free**.
At compression 0.75 (C3 ships): cube = 1.77 MB → total ≈ 1.96 MB → ≈ **1.06 MB free**.

**New Build C allocations** (from the same top pool, `isPersistent=false`, allocated in
`DPC_ObjDet_preStartConfig` immediately after the detection matrix at `objectdetection.c:3412`):

| Buffer | Layout | Size (W=4) | Size (W=8) |
|---|---|---|---|
| `H` raw near-DC rows | `uint16 H[192][2W][32]` | 98,304 B | 196,608 B |
| `C` clutter EMA map | `int32 C[192][32]` (Q11 log2 << 8 = 8 EMA fraction bits) | 24,576 B | 24,576 B |
| `angleMotionList` output | 64 × 8 B + count | 520 B | 520 B |
| **Total** | | **≈120 KB** | **≈222 KB** |

Fits at either compression ratio with ≥0.8 MB to spare. **The 0.75-compression question does not
gate this design** (we never touch the cube). Top-pool allocations persist across frames and are
re-cut only on preStart reconfig — exactly when the clutter map must re-zero anyway.

**Complex vs magnitude EMA — magnitude chosen.** The pipeline discards phase before azimuth CFAR:
the exported quantity is uint16 log2|azimFFT| (`dopplerprochwaDDMA.c:213-215`). A complex map would
require tapping the pre-logAbs doppler-FFT complex samples (cmplx32, `:144`) *before* demod, per
antenna — 8× the data, a different (per-antenna, pre-beamforming) domain, and coherent EMA is
fragile against the phase steps introduced by periodic calibration (Build A A1) and against the
sample-value `antennaCalibParams` (angle gap, ledger). Log-magnitude EMA subtraction = ratio in
linear domain = "dB above per-cell clutter average" — precisely the CFAR-like statistic we need,
robust to bulk gain drift (further compensated in §4). Cost of the tradeoff: cannot un-bury a
crosser *weaker than same-cell clutter* — accepted; junction crossers measured snr 26–47 dB.

---

## 3. TIMING BUDGET — ≥26 Hz holds by construction

Frame @26 Hz = 38.4 ms. CM4 (DPC core, 200 MHz — `sys_common_awr2x44P.h:60`) interframe
processing = the measured 17 ms "dsp_proc" (stats math `mss_main.c:2444-2446`; DPC timestamps
`objectdetection.c:648,2048`). The C66x DSP is parked in this build (`MSS_AOA_ENABLED=1`:
`makefile_awr2x44P.mak:12`, `seeker_build2.bat:6`; XYZ estimation runs on MSS R5F at
`mss_main.c:2528-2537`; C66x stalls at `dss_main.c:336-337`).

| Added work | Where | Cost | Overlap |
|---|---|---|---|
| Per-gate EDMA trigger (PaRAM dst poke + ESR write) | CM4, entry of `DPU_DopplerProcHWA_extractObjectList` | ~15 register writes ≈ <0.5 µs × 192 gates ≈ **0.1 ms** | inside existing extract slot |
| EDMA transfer 512 B M4/M6→L3 per gate | EDMA TPCC_A | ~1–2 µs/gate, **fully parallel** to CM4 extract loop and HWA azim stage of the other parity | completes long before extract exit poll |
| Fold + EMA + residual + histogram + threshold + local-max | CM4, post-doppler-DPU | 6,144 cells × ~25–40 cyc ≈ 250 k cyc ≈ **1.25 ms** (W=4; ~1.9 ms at W=8) | placed in the window where CM4 currently *blocks* on `gDPCStateSemHandle` waiting for MSS's previous-frame AoA ack (`objectdetection.c:1941-1943` ← posted via `mss_main.c:2540-2542`) |
| Peak list build (≤64 pts, insertion pattern of `updateSNRList`, `dopplerprochwaDDMA.c:493`) | CM4 | <0.1 ms | same window |
| UART TLV (≤524 B) | MSS UART DMA | 524 B / 312.5 kB/s ≈ **1.7 ms wire** | inside existing 38.4 ms wire budget (§5) |

Worst-case CM4 interframe: 17 + ~1.5 = **18.5 ms « 38.4 ms**. HWA schedule: unchanged (zero added
paramsets, zero added HWA loops). EDMA bandwidth: +98 KB/frame ≈ 2.6 MB/s on a multi-GB/s TPCC —
noise. **Hold-26 Hz gate is expected to pass untouched; the added `angleMotionCycles` stat (§5.3)
proves it numerically.**

Membank-lifetime safety: the EDMA reads M4 (ping) exactly during the window in which the CM4
extract itself reads M4 (`dopplerprochwaDDMA.c:609`) — the bank is not rewritten until the
same-parity gate two steps later, whose doppler stage starts only after this extract returns
(pipeline interleave, `dopplerprocDDMAcommon.h:497-516`). Completion is polled before extract
returns (mirror of `:776-783`). The known HWA/EDMA same-bank hazard (`dopplerprocDDMAcommon.h:233`)
does not arise: during ping extract the HWA is writing M6/M7 (opposite parity).

---

## 4. THE DETECTOR — what runs where, with cycle-level placement

### 4.1 Data movement (EDMA, per range gate)
New channel pair is **not needed** — a single SW-triggered channel suffices (the source bank
alternates but src address is re-poked per gate anyway, mirroring the existing in-extract pattern
`dopplerprochwaDDMA.c:601-606, 713-721, 776-783` used by `edmaDetObjAntSamples`).

- **Channel**: `EDMA_DSS_TPCC_A_EVT_RTIA_DMA_REQ1` = DMA ch/param/tcc **1** (free: DPC currently
  uses 0, 18-23, 31, 32-43, 52-53, 60-63 — `mmw_resDDM.h:79-199`; value map
  `cslr_soc_defines.h:128-191`). Shadow param: `DPC_OBJDET_EDMA_SHADOW_BASE + 57` (used shadows
  today: +0..+38, +56 — `mmw_resDDM.h:81-202`).
- **PaRAM** (AB-sync, one trigger per gate):
  `ACNT = W·64` B, `BCNT = 2` (head block then tail block), `CCNT = 1`,
  `SRCBIDX = (Ns−W)·64`, `DSTBIDX = W·64`,
  `src = hwaMemBankAddr[M4 or M6 per rangeBinIdx&1]` (same parity select as `azimFFTMat`,
  `dopplerprochwaDDMA.c:609`), `dst = &H[rangeIdx][0][0]` (poked per gate).
- **Trigger point**: first statement of `DPU_DopplerProcHWA_extractObjectList` — **before** the
  `rangeIdx==0` and `maxNumObj` early-returns (`dopplerprochwaDDMA.c:589-598`), so H has no holes.
  Extract call sites covered: `dopplerprochwaDDMA.c:4170, 4238, 4264, 4449, 4529`.
- **Completion**: poll IPR bit for ch1 just before extract returns (both normal and `exit:` paths).

Allocation/config plumbing: channel allocated with `DPC_ObjDet_EDMAChannelConfigAssist`
(`objectdetection.c:866-885`) inside `DPC_ObjDet_dopplerConfig` next to the existing
`edmaDetObjAntSamples` allocation (`objectdetection.c:3011-3017`); H pointer, 2W, and enable flag
passed to the DPU via new fields in `DPU_DopplerProcHWA_HW_Resources`
(`dopplerprochwaDDMA.h`, next to `dopMaxSubBandScratchBuf`).

### 4.2 Math (CM4 C loop, DPC level — NOT HWA)
**Decision: C66x loops vs HWA-native ops → neither; plain CM4 C.** Rationale:
- HWA-native EMA does not exist (no persistent-state accumulate op); faking it costs paramsets
  (scarce shared resource, Build B interaction) and common-register reconfig mid-frame — all to
  save ~1 ms on a core that is *blocked waiting* at that moment.
- The C66x is parked in this build (§3) and powering it into the loop reopens the MSS/DSS DPM
  topology — a build-level change out of proportion to 250 k cycles.
- 6,144 cells of int math on a 200 MHz M4 ≈ 1.25 ms, measured against a ≥19 ms idle window.

New static function `DPC_ObjDet_angleMotionProcess()` in
`ti/datapath/dpc/objectdetection/objdethwaDDMA/src/objectdetection.c`, called from
`DPC_ObjectDetection_execute` **after** `DPU_RangeCFARProcHWA_process` returns
(`objectdetection.c:1919-1924`) and **before** the `SemaphoreP_pend(&gDPCStateSemHandle,…)` at
`:1941-1943` (that pend is the CM4's idle wait → net frame cost ≈ 0). Gated on
`staticCfg.angleMotionCfg.isEnabled` — disabled ⇒ function not called, EDMA channel not allocated,
**byte-identical frame flow**.

Per frame (all fixed-point; azim-FFT samples are log2 magnitude in Q11 — the 5.11 log2 format
documented for the CFAR threshold path, `dopplerprocDDMAcommon.h:386-406`):

1. **Fold**: `Hf[r][θ] = max over the 2W exported rows of H[r][·][θ]` (uint16).
2. **EMA + residual** (per cell): `R = (Hf<<8) − C[r][θ]` (int32, Q19 = Q11·2^8).
   Shielded update: if `R < (margin − 3 dB)` → `C += (R × alphaQ8) >> 8`;
   else `C += (R × alphaQ8) >> 11` (slow-8× update under detections, so a parked-then-moving
   scene converges but a transiting target is not absorbed). α default 0.02 (Q8=5), CLI 0.004–0.25.
3. **Bulk-offset compensation**: 64-bin histogram of R (±8 dB span) → approximate median → subtract
   from every R before thresholding. This self-cancels global gain steps from periodic RF
   calibration (Build A A1) and slow thermal drift — the EMA then only has to track *per-cell*
   change.
4. **Detection**: cell qualifies if `R' > margin` (CLI, default 12 dB → Q11 log2 units, encoding
   pattern of `mmw_cli.c:494-503`) AND azimuth local-max (`R'[θ] ≥ R'[θ±1]`, circular — same
   convention as extract `dopplerprochwaDDMA.c:726-741`) AND range non-strict local-max
   (`R'[r] ≥ R'[r±1]`, suppresses range-sidelobe doubles).
5. **θ interpolation**: 3-point parabolic on `R'` around the peak (the DPC already does exactly
   this for its own AoA — `objectdetection.c:1124-1171`) → signed azimuth in Q7 bins.
6. **Top-K**: keep ≤ `maxPoints` (≤64) by residual, insertion-list pattern of
   `DPU_DopplerProcHWA_updateSNRList` (`dopplerprochwaDDMA.c:493`).
7. **Warm-up**: first 64 frames after `DPC_ObjectDetection_start` (`objectdetection.c:2163`):
   α_init = 0.25, detector muted, C seeded from first frame's Hf.

Azimuth convention (host converts): bin k∈[0,32), signed k′ = k−32 for k≥16;
sin(θ) = k′/16 (12 virtual az antennas at d = 0.5λ zero-padded to 32 — `antGeometryCfg` in the
cfg; zero-insertion `objectdetection.c:2730-2731`). Elevation: none in this channel (azimuth-only
map) — host treats angle-motion points as ground-plane; documented limitation.

---

## 5. NEW TLV — format, fw export path, UART budget

### 5.1 Wire format
- **Type ID = 10**: append `MMWDEMO_OUTPUT_MSG_ANGLE_MOTION_POINTS = 10` to
  `MmwDemo_output_message_type` immediately before `MMWDEMO_OUTPUT_MSG_MAX` (`mmw_output.h:60-91`;
  the MSS `tl[MMWDEMO_OUTPUT_MSG_MAX]` array at `mss_main.c:1138` grows automatically).
- **Length** = `numPoints × 8`; count is implied by length (no sub-header).
- **Point** (8 B, little-endian, int16-based as required):

```c
typedef struct MmwDemo_angleMotionPoint_t   /* new, in mmw_output.h */
{
    uint16_t rangeIdx;      /* 0..numRangeBins-1; meters = rangeIdx * rangeStep (2.604 m A/G) */
    int16_t  azimQ7;        /* signed azimuth FFT bin × 128 (parabolic-interpolated);
                               az_deg = asin((azimQ7/128)/16) * 180/pi                       */
    int16_t  residSnr;      /* residual over clutter map, 0.1 dB steps
                               (Q11 log2 → dB: ×6.0206/2048; same 0.1 dB convention as TLV7,
                               dpif_pointcloud.h:95-102)                                     */
    int16_t  zeroDopRow;    /* signed doppler sub-bin of max contribution, -W..W-1 (diagnostic;
                               v_hint = zeroDopRow * 0.064 m/s)                              */
} MmwDemo_angleMotionPoint;
```

### 5.2 Firmware export path (MSS)
- Extend `DPC_ObjectDetection_ExecuteResult` (`objectdetection.h:626-666`) with
  `uint32_t numAngleMotionPoints;` and `MmwDemo_angleMotionPoint *angleMotionList;`
  (CM4 fills from §4; struct is DPM-shared, all three core images rebuild together in the
  appimage — same pattern as `detObjList` at `:647`).
- `MmwDemo_transmitProcessedOutput` (`mss_main.c:1123-1374`): declare the TLV in the sizing pass
  when `numAngleMotionPoints > 0` (pattern of TLV7, `:1195-1201`), `AddrTranslateP_getLocalAddr`
  the list pointer (pattern `:1163-1171`), and emit tl + payload in the send pass
  (pattern `:1271-1285`). `header.numTLVs` and padding logic are untouched patterns.
- Emission gate: `angleMotionCfg.isEnabled` only (no new guiMonitor flag — the C0 item already
  showed the daemon tolerates TLV-set changes only when told; see §7 host contract).

### 5.3 Stats (timing proof)
Add `uint32_t angleMotionCycles;` to `DPC_ObjectDetection_Stats` (`objectdetection.h:567-594`) —
internal DPM struct, **wire TLV6 (`MmwDemo_output_message_stats`, `mmw_output.h:140-159`)
unchanged**. Read on bench via CLI debug print at sensorStop (pattern
`objectdetection.c:2276-2283`).

### 5.4 UART budget (baud 3,125,000 ≈ 312.5 kB/s)
| Item | Bytes/frame | Wire time @26 Hz |
|---|---|---|
| Current typical (≈300 pts: TLV1 16 B/pt + TLV7 4 B/pt + hdr/stats/temp/rangeProfile) | ≈ 6.7 kB | ≈ 21.5 ms |
| **New TLV10, worst case (64 pts)** | 8 + 512 = **520 B** | **1.7 ms (+4.3 % of frame)** |
| Typical (≤10 pts) | 88 B | 0.3 ms |
Frame budget @26 Hz = 12.0 kB. Headroom holds; the existing "watch ppf vs 793 cap / bytes-per-frame"
discipline from the CFG wave (AG_FW_PLAN C1) already gates the combined load, and drops=0 stays a
hard acceptance gate.

---

## 6. CLI / CONFIG

### 6.1 New command
```
angleMotionCfg <subFrameIdx> <isEnabled> <alpha> <marginDb> <zeroDopWidth> <maxPoints>
default (compiled-in): angleMotionCfg -1 0 0.02 12.0 4 64
```
| Arg | Range | Maps to |
|---|---|---|
| isEnabled | 0/1 | gates everything (EDMA alloc, H/C alloc, detector call, TLV) |
| alpha | 0.004–0.25 float | `alphaQ8` uint8 (×256, min 1) |
| marginDb | 3.0–30.0 float | Q11 log2 units: `round(dB/6.0206*2048)` (encoding comment pattern `mmw_cli.c:494-503`) |
| zeroDopWidth W | 1–8 | rows each side of DC (§1) |
| maxPoints | 1–64 | TLV cap |

### 6.2 Implementation pattern (all cited)
- Table entry in `MmwDemo_CLIInit` next to `cfarCfg` (`mmw_cli.c:1884-1960`); handler modeled on
  `MmwDemo_CLICfarCfg` (`mmw_cli.c:475-557`): parse → `MmwDemo_CfgUpdate(&cfg,
  MMWDEMO_ANGLEMOTIONCFG_OFFSET, sizeof(cfg), subFrameNum)`.
- New struct `DPC_ObjectDetection_AngleMotionCfg {uint8_t isEnabled; uint8_t alphaQ8; uint8_t
  zeroDopWidth; uint8_t maxPoints; uint16_t marginQ11;}` added to (a) `MmwDemo_datapathCfg` and
  offset macro in `mmw_mss.h` (pattern `mmw_mss.h:100-114`), and (b)
  `DPC_ObjectDetection_StaticCfg` (`objectdetection.h:380-463`, next to `localMaxCfg` at `:463`).
- Copied into the pre-start static cfg in the sensorStart assembly block alongside the existing
  memcpys (`mss_main.c:1964-1979`), delivered via `DPC_OBJDET_IOCTL__STATIC_PRE_START_CFG`
  (`mss_main.c:1985-1988` → CM4 `dss_cm4_main.c:256-268` → `objectdetection.c:3361`).

### 6.3 ddmEnabled guard
`gMmwMssMCB.ddmEnabled` is set only when the cfg contains `ddmPhaseShiftAntOrder`
(SEEKER PATCH, `mmw_cli.c:763-765`) and the pre-start ioctl is already skipped when it is 0
(`mss_main.c:1981-1994`). **Guard**: in the sensorStart config path, if
`angleMotionCfg.isEnabled && !gMmwMssMCB.ddmEnabled` → `CLI_write` error
`"angleMotionCfg requires a DDM cfg (ddmPhaseShiftAntOrder)"` and fail sensorStart. (On a raw-capture
/ non-DDM cfg there is no azim stage to tap.)

### 6.4 Deployment
Chip reconfigures only from power-on INIT: the AIRPOC daemon pushes the cfg on start
(12 V-cycle procedure, AG_FW_PLAN §C0-C5 preamble). The `angleMotionCfg` line is added to the
shipping cfg in the AIRPOC cloud repo `radar/cfg` by the daemon owner (one commit, push→pull),
initially `isEnabled=0`, flipped to 1 only after the §8 gates pass.

---

## 7. HOST CONTRACT — SPEC ONLY (radar daemon owner implements; GUI out of scope)

### 7.1 Parser (radar daemon `tlv.c`)
The frame walk (magic word → `MmwDemo_output_message_header` (`mmw_output.h:100-130`) →
`numTLVs` × {uint32 type, uint32 length, payload}) gains one case, exactly parallel to the
existing type-1/type-7 handling:

```c
case 10: /* MMWDEMO_OUTPUT_MSG_ANGLE_MOTION_POINTS */
    n = tl.length / 8;                      /* 8 B/point, count implied by length   */
    /* struct: u16 range_idx; i16 azim_q7; i16 resid_snr_q1; i16 zero_dop_row       */
    /* store into frame->am_points[n], clamp n at 64                                */
```
**Hard requirements** for the daemon:
1. Unknown/new TLV types must be skipped by length, never fatal (protects replay of pre-Build-C
   recordings and future additions).
2. TLV10 absent ⇒ `am_points` empty — no schema error (old firmware / feature off).
3. Conversions (daemon owns cfg knowledge):
   `r_m = range_idx × rangeStep` (2.604 m for the A/G cfg);
   `az_deg = asin((azim_q7/128)/16)·180/π`; `snr_db = resid_snr/10.0`;
   `vr_hint = zero_dop_row × Δv` (Δv ≈ 0.064 m/s).

### 7.2 Wire JSON (SSE :8092) extension
Frame message gains one optional array (omitted when empty):
```json
"am": [[r_m, az_deg, snr_db, vr_hint], ...]
```
`/stats` gains `"am_ppf"` (per-frame TLV10 point count, EMA'd like existing ppf) and
`"tlv10_present": true|false`.

### 7.3 Tracker feed
`am` points enter the existing temporal tracker (blob + M-of-N in `cluster.c` on main) as a
**second detection channel** tagged `src="am"`, with `v_r := vr_hint (≈0)`. The crossing-car
signature the host already proved it can track when fed points: near-constant `r`, monotonic
azimuth walk (~3.6°/bin, 1–2 bins over a 2–3 s crossing at 330–440 m), frame-persistent.
Channel-level dedup: an `am` point within ±1 range bin and ±4° of a doppler-channel point in the
same frame is fused into that track, not double-counted. Per guideline (radar module boundary),
this section is the TEXT SPEC handed over — no app/ or GUI work is part of Build C.

---

## 8. VALIDATION PLAN

Pre-conditions: Build B merged and its baselines re-frozen (AG_FW_PLAN §F: TLV-replay cannot
validate DPU changes; same rule applies here for the ON-state).

### 8.1 What offline fixtures can and cannot do
`rj`/`rh` `points.bin` fixtures (in `C:\jetson-stage\ag_regression\fixtures\`) carry only the OLD
TLVs — they validate **non-regression of the doppler channel** (feature OFF and ON) but cannot
exercise TLV10. New recordings post-flash are mandatory; convert with `conv_airec.py`, gate with
`baseline.py` / `regression_gates.py` (durable copies in `C:\jetson-stage\ag_regression\`).

### 8.2 Regression gates (feature OFF — must be *identical*, not merely within tolerance)
- OFF-state code path allocates nothing, triggers nothing, emits nothing (§4.2, §6.1). Verify:
  same-scene sequential A/B old-fw vs new-fw-OFF → all existing fingerprints hold:
  rate ≥ 26.0 Hz, 0 drops; comb fingerprint `false_mv ≤ 105`, `false_snr ≤ 19 dB`; ppf /
  static_snr / real_mv vs paired same-scene reference; corner-reflector centroid shift
  < 0.2° az / < 1 range bin (AG_FW_PLAN §C pass criteria — reuse verbatim).

### 8.3 Additive-ness gates (feature ON — doppler path statistically unchanged)
- Same-scene A/B OFF vs ON: TLV1/TLV7 ppf, SNR distribution, comb fingerprint within the paired
  same-scene tolerances (~0.3 dB resolution per AG_FW_PLAN §E1). Only shared resources are EDMA
  bandwidth and CM4 idle time — any drift here means a bug, not a tuning issue.
- `angleMotionCycles` (§5.3) < 2.5 ms at W=8; rate ≥ 26.0 Hz, 0 drops, 30-min soak;
  zero MON_TIMING_FAIL; mod-50 bucket check clean (calibration-phase artifact detector).

### 8.4 Margin calibration from the residual distribution (before any detection claims)
Empty/static bench scene, feature ON, `marginDb` high (30), then a 10-min capture with
`marginDb=3, maxPoints=64` → the TLV10 stream *is* the tail of the residual distribution.
Set operating `marginDb` = value where empty-scene raw am-ppf p99 ≤ 10 and tracker-confirmed
false tracks = 0 over 10 min. Repeat in an open outdoor static scene (garage multipath is
documented worst-case, not a fair calibration scene).

### 8.5 Acceptance (the junction/highway fix, numbers not vibes)
Re-record the junction and highway scenes (same geometry class as the proven-miss dataset:
crossers at 330–440 m, snr 26–47, ~4°/2–3 s, zero doppler-channel detections):
1. **Detection**: for ≥70 % of crossing events, TLV10 emits a point within ±1 range bin and
   ±1 azimuth bin of the crosser in ≥50 % of the frames of its crossing window, and the host
   temporal tracker confirms an angle-walk track for the event.
2. **False bound**: empty-scene tracker-confirmed false tracks from the `am` channel = 0 over
   30 min; raw am-ppf p99 ≤ 10 at the calibrated margin.
3. **No doppler regression**: §8.2/§8.3 all green in the same session.
4. **Ground truth discipline**: every detection claim cross-checked against the operator's scene
   log (standing rule: no celebrating detections without ground truth).
New fixtures frozen into `baseline.json` afterwards: T7-junction (ON), T8-empty (ON, margin cal),
T9 corner-reflector az-sanity for the `am` channel (a *static* reflector must NOT appear in TLV10
after warm-up — it is clutter by definition; its disappearance is the clutter-map health check).

---

## 9. RISKS, RANKED

1. **Build-B collision (process, highest)** — Build B edits the same files: empty-band gate lives
   inside `DPU_DopplerProcHWA_extractObjectList` (`dopplerprochwaDDMA.c:629-768/835-1029`) and its
   hypothesis-energy export mirrors the Max-Subband EDMA out (`dopplerprocDDMAcommon.h:221-240`).
   Build C touches extract entry/exit and adds EDMA ch 1 + shadow +57. Code regions are disjoint,
   but: **Build C branches from Build B's merged tree, never in parallel**; EDMA claims recorded in
   the firmware ledger (Build B must not take DMA ch 1 or shadows +57/+58); baselines re-frozen
   after B before C's A/B runs.
2. **CM4 timing (low-medium)** — +1.3–1.9 ms on the DPC core; sized against a ≥19 ms idle window
   but the pend at `objectdetection.c:1941` can in principle be near-zero if MSS is late.
   Mitigation: `angleMotionCycles` stat + the 26 Hz/0-drop gate; single-CLI rollback
   (`isEnabled 0`) restores the byte-identical path — no reflash needed.
3. **HWA/EDMA membank contention (low)** — reading M4/M6 while HWA writes the opposite-parity
   banks is contention-free by bank separation; the documented same-bank hazard
   (`dopplerprocDDMAcommon.h:233`) is avoided by construction (§3). Residual risk = future paramset
   re-banking by Build B; covered by gate §8.3 + the FFT-clip/mem-access monitors at sensorStop
   (`objectdetection.c:2276-2283`).
4. **Clutter-map absorption of slow crossers (algorithmic)** — α=0.02 (τ≈2 s) vs ~2 s/az-bin dwell
   ⇒ up to ~60 % residual decay worst case. Mitigated in-design: detection-shielded EMA (§4.2.2,
   8× slower update above margin−3 dB) and the margin calibrated with shielding active. If field
   data still shows fade-out, α drops to 0.01 (CLI, no reflash).
5. **Periodic-calibration steps (Build A interaction)** — RUN_CALIB gain/phase steps shift H in
   bulk; per-frame median subtraction (§4.2.3) cancels bulk shifts, EMA absorbs per-cell remainder
   within ~1/α frames; mod-50 bucket check is the detector for anything phase-aligned left over.
6. **Angle accuracy (known, pre-existing)** — `antennaCalibParams` are TI sample values (ledger);
   absolute azimuth may be off ~1–2 bins off-boresight. Angle-*walk* tracking is differential and
   tolerant; absolute accuracy improves when C6 per-unit cal lands. Not a Build C blocker.
7. **Memory (negligible)** — ≤222 KB of ≥1.06 MB free L3 even with compression 0.75 (§2); no
   interaction with the cube, no paramset usage (0 added vs ~38/64 used —
   `cslr_soc_defines.h:583`, counts `mmw_resDDM.h:70-115,173-175`, `dopplerprochwaDDMA.h:198-205`,
   `rangeprochwaDDMA.h:255`).
8. **UART pressure at high ppf (low)** — +520 B/frame worst case on top of the 793-pt cap
   dynamics; existing bytes-per-frame watch + drops=0 gate covers it; `maxPoints` is the relief
   valve.
9. **TLV type-10 collision with future TI SDK growth (cosmetic)** — this is a fork; documented in
   `radar/VERSION_*.md` and the daemon treats unknown types as skippable either way.

---

## APPENDIX A — Full file/line change map for the implementing agent

| # | File | Change | Anchor |
|---|---|---|---|
| 1 | `ti/demo/awr2x44P/mmw_ddm/include/mmw_output.h` | enum `MMWDEMO_OUTPUT_MSG_ANGLE_MOTION_POINTS=10`; `MmwDemo_angleMotionPoint` struct | `:60-91` |
| 2 | `ti/demo/awr2x44P/mmw_ddm/mss/mmw_mss.h` | `angleMotionCfg` field in `MmwDemo_datapathCfg`; `MMWDEMO_ANGLEMOTIONCFG_OFFSET` | `:100-114` |
| 3 | `ti/demo/awr2x44P/mmw_ddm/mss/mmw_cli.c` | `angleMotionCfg` table entry + handler; encode margin per threshold pattern | `:1884-1960`, `:475-557`, `:494-503` |
| 4 | `ti/demo/awr2x44P/mmw_ddm/mss/mss_main.c` | copy cfg into pre-start staticCfg; ddmEnabled guard; TLV10 declare+send in `MmwDemo_transmitProcessedOutput` | `:1964-1994`, `:1123-1374` |
| 5 | `ti/datapath/dpc/objectdetection/objdethwaDDMA/objectdetection.h` | `DPC_ObjectDetection_AngleMotionCfg` in StaticCfg; `numAngleMotionPoints`/`angleMotionList` in ExecuteResult; `angleMotionCycles` in Stats | `:380-463`, `:626-666`, `:567-594` |
| 6 | `ti/datapath/dpc/objectdetection/objdethwaDDMA/src/objectdetection.c` | H/C/list L3 allocs in preStartConfig; EDMA ch alloc + DPU hw-res wiring in dopplerConfig; `DPC_ObjDet_angleMotionProcess()` + call site; result fields | allocs after `:3412`; EDMA next to `:3011-3017`; call between `:1924` and `:1941`; result at `:2029-2049` |
| 7 | `ti/datapath/dpu/dopplerprocDDMA/dopplerprochwaDDMA.h` | new `DPU_DopplerProcHWA_HW_Resources` fields: `angleMotionRowBuf`, `edmaAngleMotionOut`, `numZeroDopRows`, `angleMotionEnabled` | near `dopMaxSubBandScratchBuf` |
| 8 | `ti/datapath/dpu/dopplerprocDDMA/src/dopplerprochwaDDMA.c` | PaRAM setup in config; trigger at extract entry (both extract variants, before early-returns) + completion poll at exit | `:557-598`, `:601-606`, `:776-783`, `:804-870` |
| 9 | `ti/demo/awr2x44P/mmw_ddm/mmw_resDDM.h` | `EDMA_ANGLEMOTION_OUT = EDMA_DSS_TPCC_A_EVT_RTIA_DMA_REQ1`, shadow `+57` | `:117-166` |
| 10 | AIRPOC cloud repo `radar/cfg/*.cfg` | add `angleMotionCfg -1 0 0.02 12.0 4 64` line (daemon owner; flip to enabled post-gates) | — |
| 11 | AIRPOC cloud repo `radar/` daemon | §7 spec (owner implements) | — |

Build/flash mechanics unchanged from Build A/B: `seeker_build2.bat` (MSS_AOA_ENABLED=1),
appimage via `uart_uniflash.py`, rollback = `flash_demoDDM.cfg` (AG_FW_PLAN §C), ledger entry in
seeker-ground-station, cloud-repo-only distribution of cfg/daemon changes.
