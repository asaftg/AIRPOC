# Build B datapath sources (SEEKER-modified TI SDK files)

Snapshot of the SDK-tree files modified for Build B (agv2). SDK destinations:

| File | SDK path (mmwave_mcuplus_sdk_04_07_02_01/ti/) |
|---|---|
| dopplerprochwaDDMA.h | datapath/dpu/dopplerprocDDMA/ |
| dopplerprochwaDDMAinternal.h | datapath/dpu/dopplerprocDDMA/include/ |
| dopplerprochwaDDMA.c | datapath/dpu/dopplerprocDDMA/src/ |
| dopplerprocDDMAlib.mak | datapath/dpu/dopplerprocDDMA/ |
| objectdetection.c | datapath/dpc/objectdetection/objdethwaDDMA/src/ |
| mmw_resDDM.h | demo/awr2x44P/mmw_ddm/ |

Rebuild: copy these over the SDK tree, then run ../seeker_build2.bat
(rebuilds the dopplerprocDDMA lib for M4+C66, then gmake mmwDemoDDM).
All edits carry SEEKER markers.
