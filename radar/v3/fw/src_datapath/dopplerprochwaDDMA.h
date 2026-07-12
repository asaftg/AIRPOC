/**
 *   @file  dopplerprochwaDDMA.h
 *
 *   @brief
 *      Implements Data path doppler processing functionality.
 *
 *  \par
 *  NOTE:
 *      (C) Copyright 2018 - 2026 Texas Instruments, Inc.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *    Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 *    Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the
 *    distribution.
 *
 *    Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**************************************************************************
 *************************** Include Files ********************************
 **************************************************************************/
#ifndef DOPPLERPROC_HWA_H
#define DOPPLERPROC_HWA_H

/* Standard Include Files. */
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

/* mmWave SDK Driver/Common Include Files */
// #include <ti/drv/hwa/hwa.h>
#include <drivers/edma.h>
#include <drivers/hwa.h>

/* DPIF Components Include Files */
#include <ti/datapath/dpif/dpif_detmatrix.h>
#include <ti/datapath/dpif/dpif_radarcube.h>
#include <ti/datapath/dpif/dpif_pointcloud.h>

/* mmWave SDK Data Path Include Files */
#include <ti/datapath/dpif/dp_error.h>
#include <ti/datapath/dpu/dopplerprocDDMA/dopplerprocDDMAcommon.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @addtogroup DPU_DOPPLERPROC_ERROR_CODE
 *  Base error code for the dopplerProc DPU is defined in the
 *  \include ti/datapath/dpif/dp_error.h
 @{ */

/**
 * @brief   Error Code: Invalid argument
 */
#define DPU_DOPPLERPROCHWA_EINVAL                  (DP_ERRNO_DOPPLER_PROC_DDMA_BASE-1)

/**
 * @brief   Error Code: Out of memory
 */
#define DPU_DOPPLERPROCHWA_ENOMEM                  (DP_ERRNO_DOPPLER_PROC_DDMA_BASE-2)

/**
 * @brief   Error Code: DPU is in progress
 */
#define DPU_DOPPLERPROCHWA_EINPROGRESS             (DP_ERRNO_DOPPLER_PROC_DDMA_BASE-3)

/**
 * @brief   Error Code: Out of HWA resources
 */
#define DPU_DOPPLERPROCHWA_EHWARES                 (DP_ERRNO_DOPPLER_PROC_DDMA_BASE-4)

/**
 * @brief   Error Code: Semaphore creation failed
 */
#define DPU_DOPPLERPROCHWA_ESEMA                   (DP_ERRNO_DOPPLER_PROC_DDMA_BASE-5)

/**
 * @brief   Error Code: Bad semaphore status
 */
#define DPU_DOPPLERPROCHWA_ESEMASTATUS             (DP_ERRNO_DOPPLER_PROC_DDMA_BASE-6)

/**
 * @brief   Error Code: Configure parameters exceed HWA memory bank size
 */
#define DPU_DOPPLERPROCHWA_EEXCEEDHWAMEM           (DP_ERRNO_DOPPLER_PROC_DDMA_BASE-7)

/**
 * @brief   Error Code: Unsupported radar cube format
 */
#define DPU_DOPPLERPROCHWA_ECUBEFORMAT             (DP_ERRNO_DOPPLER_PROC_DDMA_BASE-8)

/**
 * @brief   Error Code: Unsupported detection matrix format
 */
#define DPU_DOPPLERPROCHWA_EDETMFORMAT             (DP_ERRNO_DOPPLER_PROC_DDMA_BASE-9)

/**
 * @brief   Error Code: Insufficient detection matrix size
 */
#define DPU_DOPPLERPROCHWA_EDETMSIZE               (DP_ERRNO_DOPPLER_PROC_DDMA_BASE-10)

/**
 * @brief   Error Code: Wrong window size
 */
#define DPU_DOPPLERPROCHWA_EWINDSIZE               (DP_ERRNO_DOPPLER_PROC_DDMA_BASE-11)

/**
 * @brief   Error Code: Number of chirps per ping for decompression < 1
 */
#define DPU_DOPPLERPROCHWA_ERROR_NUMCHIRPSPERPING          (DP_ERRNO_DOPPLER_PROC_DDMA_BASE-12)

/**
 * @brief   Error Code: Number of Rx antennas per block should be the same as the number
 *                      of Rx antennas
 */
#define DPU_DOPPLERPROCHWA_ERROR_NUMRXANTPERBLOCK_DECOMPRESSION        (DP_ERRNO_DOPPLER_PROC_DDMA_BASE-13)

/**
 * @brief   Error Code: RangeBinsPerBlock for decompression should be a power of 2
 */
#define DPU_DOPPLERPROCHWA_ERROR_RANGEBINSPERBLOCK_DECOMPRESSION       (DP_ERRNO_DOPPLER_PROC_DDMA_BASE-14)

/**
 * @brief   Error Code: decompression method 0 and 1 are supported
 */
#define DPU_DOPPLERPROCHWA_ERROR_METHOD_DECOMPRESSION                  (DP_ERRNO_DOPPLER_PROC_DDMA_BASE-15)

/**
 * @brief   Error Code: CFAR Config error
 */
#define DPU_DOPPLERPROCHWA_ERROR_METHOD_CFAR                  (DP_ERRNO_DOPPLER_PROC_DDMA_BASE-16)

/**
 * @brief   Error Code: Find Max Idx error
 */
#define DPU_DOPPLERPROCHWA_ERROR_FINDMAX                  (DP_ERRNO_DOPPLER_PROC_DDMA_BASE-17)

/**
@}
*/
/** @brief This limits the detected number of objects per range bin. It helps in detecting more objects at
 * long distance, with limited available memory.
 * With detObjList written over the radar cube, it is essential to keep this macro enabled to avoid
 * overflow and detect objects at far range-bin. */
#define LIMIT_DETECTED_OBJS_PER_RANGEBIN

#ifdef LIMIT_DETECTED_OBJS_PER_RANGEBIN
/** @brief This macro defines the maximum number of objects that can be stored per range bin. */
#define MAX_NUM_OBJ_PER_RANGE_BIN (40U)
#endif


/* Uncommenting the following macro enables the histogram based detection and
 * disables the doppler CFAR stage.
 * For bit-exact testing of this feature, user needs to generate test vectors from
 * Matlab model by enabling the 'useHistogram' option in main script. */

// #define ENABLE_HISTOGRAM_BASED_DOP_AZIM_DETECTION
#ifdef ENABLE_HISTOGRAM_BASED_DOP_AZIM_DETECTION
/* CDF Bin Number which crosses the CDF_CNT_THRES * dopSubBins mark is used for LM engine SW thresholds. */
#define CDF_CNT_THRES                      (0.9f)
#define HIST_SIZE_SELECT                   (6U)
#define HIST_SCALE_SELECT                  (10U)
#define HIST_DOPPLER_DIM_OFFSET_DB         (10U) /* dB */
#endif

/**
 * @brief   Number of HWA memory banks needed
 */
#define DPU_DOPPLERPROCHWA_NUM_HWA_MEMBANKS  8U

#define DOPPLERPROCHWADDMA_NUM_EDMA_INTERRUPTS 3U

#define DOPPLERPROCHWA_DDMA_DECOMP_NUM_HWA_PARAMSETS         2U
#define DOPPLERPROCHWA_DDMA_BFP_DECOMP_NUM_HWA_PARAMSETS     8U
#define DOPPLERPROCHWA_DDMA_DOPPLER_NUM_HWA_PARAMSETS       12U

#ifdef ENABLE_HISTOGRAM_BASED_DOP_AZIM_DETECTION
#define DOPPLERPROCHWA_DDMA_AZIMCFAR_NUM_HWA_PARAMSETS       12U
#else
#define DOPPLERPROCHWA_DDMA_AZIMCFAR_NUM_HWA_PARAMSETS      14U
#endif

/**
 * @brief   16KB of HWA MemBank size for Decompression
 */
#define DECOMP_HWA_MEMBANK_SIZE 16384U


/*! @brief Alignment for memory allocation purpose of detection matrix.
 *         There is CPU access of detection matrix in the implementation.
 */
#define DPU_DOPPLER_DET_MATRIX_BYTE_ALIGNMENT CSL_CACHE_L1D_LINESIZE

/* SEEKER 2026-07-10 (Build B "comb killer"): Empty-band leakage gate.
 * The 6-bit TX phase shifter cannot represent 1/6-turn DDMA steps; the resulting
 * deterministic period-3 phase error leaks ~-31.5dBc energy into the empty
 * sub-bands, which passes CFAR and is assigned a wrong (random-looking) velocity.
 * The gate compares the DDMA-Metric energy of the winning rotation hypothesis
 * against the hypotheses that swap one empty band in; candidates whose margin is
 * below the threshold are rejected at object-list extraction.
 * Default margin threshold (dB) used when the CLI supplies enabled=1 with 0 dB. */
#define SEEKER_EMPTYBAND_MARGIN_DB_DEFAULT   (12.0f)

/* SEEKER 2026-07-11 (agv3): dB -> DDMA-Metric raw-LSB conversion constants,
 * corrected after the agv2 gate margin proved to be a no-op at both 12 dB and
 * 60 dB (junk count identical to gate-off at both settings).
 *
 * Scale chain of the Z-array the gate compares, pinned from the stock
 * paramsets in dopplerprochwaDDMA.c:
 *  1. The HWA LOG2 unit emits log2(|x|) in Q11 (2048 LSB per log2 unit) in
 *     the 24-bit internal domain. Anchor: the demo doppler-CFAR threshold
 *     conversion dB * log2(10)/20 * 2048 (MmwDemo_convertDopplerCfarToThresh)
 *     applied against the azimuth-FFT log2 output written at dstScale=0 and
 *     read back at srcScale=8.
 *  2. The LOG ABS SUM RX stats paramset accumulates numRxAntennas log2 values
 *     in the 36-bit sum statistic; the common FFTSUMDIV register - programmed
 *     to ceilLog2(numRxAntennas) in DPU_DopplerProcHWA_configHWACommon - then
 *     right-shifts the sum, i.e. the stats output is the AVERAGE of the
 *     per-RX log2 values, still Q11. The 32-bit stats write at dstScale=8 is
 *     an identity (LSB-aligned) store, which is why the downstream 16-bit
 *     stride-8 readers (DDMA-Metric, SumTx) see the value intact: the maximum
 *     is 24 * 2048 = 49152 < 65536. Anchor: the range-CFAR threshold
 *     conversion (MmwDemo_convertRangeCfarToThresh) whose
 *     numBands / 2^ceilLog2(numBands) bookkeeping only balances with this
 *     scale.
 *  3. The DDMA METRIC paramset reads those 16-bit values at srcScale=8
 *     (identity), FFT-sums numBandsActive of them (butterflyScaling=0 -> bin 0
 *     is the plain sum), keeping 2048 LSB per log2 PER BAND internally.
 *  4. The metric OUTPUT formatter (32-bit real write, dstScale=0) stores
 *     word = internal << (8 - dstScale) = internal << 8. Anchor: every stock
 *     32-bit write-dstScale-0 / read-srcScale-8 hop in the chain (doppler FFT
 *     -> logAbs/demod, demod -> azimuth FFT, metric -> max-subband) is a
 *     lossless roundtrip; stock demodulation would decay to zero within two
 *     hops otherwise. The gate (and the host) compare those raw 32-bit words.
 *
 * A single-band amplitude ratio of D dB therefore moves the host-read Z by
 *   D * (log2(10)/20) * 2048 * 256 * (numRx / 2^ceil(log2(numRx)))
 * = D * ~87086 raw LSB with 4 RX. agv2 applied D * ~5.3 raw LSB (it assumed a
 * sum over RX where the hardware averages, and a >>8 where the output
 * formatter does <<8): a combined 2^14 error that turned a 12 dB margin into
 * ~0.0007 dB - the root cause of the no-op gate. */

/* HWA log2 magnitude scale in the 24-bit internal domain: Q11 (5.11),
 * 2048 LSB per log2 unit - the same convention as every CFAR threshold
 * conversion in the demo. */
#define SEEKER_EMPTYBAND_LOG2_Q11_SCALE      (2048.0f)

/* Left-shift applied by the HWA output formatter when the DDMA-Metric
 * paramset writes its 24-bit internal Z values as 32-bit real samples with
 * dstScale=0 (word = internal << (8 - dstScale)). */
#define SEEKER_EMPTYBAND_METRIC_OUT_LSHIFT   (8U)

/* Margin-telemetry histogram bin width, as a power of two of raw Z LSBs.
 * 1<<18 = 262144 raw LSB ~ 3.01 dB per bin with 4 RX, so 16 bins cover
 * ~48 dB of margin. */
#define SEEKER_EMPTYBAND_HIST_BIN_SHIFT      (18U)

/* Number of bins in the margin-telemetry histogram. */
#define SEEKER_EMPTYBAND_HIST_NUM_BINS       (16U)

#define NUM_DDMA_BANDS 6U
#if defined(SOC_AWR2944) || defined(SOC_AWR2X44P)
#define NUM_TXANT 4U
#define NUM_TXANT_AZIM 3U
#define NUM_TXANT_ELEV 1U
#elif defined(SOC_AWR2943)
#define NUM_TXANT 3U
#define NUM_TXANT_AZIM 2U
#define NUM_TXANT_ELEV 1U

#endif
#define NUM_RXANT 4U


/*!
 *  @brief   Handle for Doppler Processing DPU.
 */
typedef void*  DPU_DopplerProcHWA_Handle;

/**
 * @brief
 *  dopplerProc DPU initial configuration parameters
 *
 * @details
 *  The structure is used to hold the DPU initial configurations.
 *
 *  \ingroup DPU_DOPPLERPROC_EXTERNAL_DATA_STRUCTURE
 */
typedef struct DPU_DopplerProcHWA_InitCfg_t
{
    /*! @brief HWA Handle */
    HWA_Handle  hwaHandle;

}DPU_DopplerProcHWA_InitParams;

typedef struct DPU_DopplerProcHWADDMA_HwaStateMachineCfg_t{
        /*! @brief Number of HWA paramsets reserved for the Doppler DPU.
         The DPU will use numParamSets consecutively, starting from paramSetStartIdx.\n
    */
    uint8_t     numParamSets;

    /*! @brief HWA paramset Start index.
         Application has to ensure that paramSetStartIdx is such that \n
        [paramSetStartIdx, paramSetStartIdx + 1, ... (paramSetStartIdx + numParamSets - 1)] \n
        is a valid set of HWA paramsets.\n
    */
    uint32_t    paramSetStartIdx;
}DPU_DopplerProcHWADDMA_HwaStateMachineCfg;


/**
 * @brief
 *  dopplerProc DPU HWA configuration parameters
 *
 * @details
 *  The structure is used to hold the HWA configuration parameters
 *  for the Doppler Processing DPU
 *
 *  \ingroup DPU_DOPPLERPROC_EXTERNAL_DATA_STRUCTURE
     */
typedef struct DPU_DopplerProcHWA_HwaCfg_t
{
    /*! @brief Indicates if HWA window is symmetric or non-symmetric.
        Use HWA macro definitions for symmetric/non-symmetric.
    */
    uint8_t     winSym;

    /*!  @brief Doppler FFT window size in bytes.
         This is the number of coefficients to be programmed in the HWA for the windowing
         functionality. The size is a function of numDopplerChirps as follows:\n
         If non-symmetric window is selected: windowSize = numDopplerChirps * sizeof(int32_t) \n
         If symmetric window is selected and numDopplerChirps is even:
         windowSize = numDopplerChirps * sizeof(int32_t) / 2 \n
         If symmetric window is selected and numDopplerChirps is odd:
         windowSize = (numDopplerChirps + 1) * sizeof(int32_t) / 2
    */
    uint32_t    windowSize;

    /*! @brief Pointer to Doppler FFT window coefficients. */
    int32_t     *window;

    /*! @brief HWA window RAM offset in number of samples. */
    uint32_t    winRamOffset;

    /*! @brief Number of HWA paramsets reserved for the Doppler DPU.
         The DPU will use numParamSets consecutively, starting from paramSetStartIdx.\n
    */
    uint8_t     numParamSets;

    /*! @brief HWA paramset Start index.
         Application has to ensure that paramSetStartIdx is such that \n
        [paramSetStartIdx, paramSetStartIdx + 1, ... (paramSetStartIdx + numParamSets - 1)] \n
        is a valid set of HWA paramsets.\n
    */
    uint32_t    paramSetStartIdx;

    DPU_DopplerProcHWADDMA_HwaStateMachineCfg   decompStageHwaStateMachineCfg;
    DPU_DopplerProcHWADDMA_HwaStateMachineCfg   dopplerStageHwaStateMachineCfg;
    DPU_DopplerProcHWADDMA_HwaStateMachineCfg   azimCfarStageHwaStateMachineCfg;

}DPU_DopplerProcHWA_HwaCfg;

/**
 * @brief
 *  dopplerProc DPU EDMA configuration parameters
 *
 * @details
 *  The structure is used to hold the EDMA configuration parameters
 *  for the Doppler Processing DPU
 *
 *  \ingroup DPU_DOPPLERPROC_EXTERNAL_DATA_STRUCTURE
 */
typedef struct DPU_DopplerProc_DDMA_Decompression_EdmaCfg_t
{

    /*! @brief  EDMA configuration for Decompression input data (Radar cube -> HWA memory). */
    DPU_DopplerProc_Edma edmaIn;

    /*! @brief  EDMA configuration for Decompression input data (Radar cube -> HWA memory). */
    DPU_DopplerProc_Edma edmaInSignature;

    /*! @brief  EDMA configuration for Decompression Output data (HWA memory -> detection matrix). */
    DPU_DopplerProc_Edma edmaOut;

    /*! @brief  EDMA configuration for decompression out. */
    Edma_IntrObject *edmaIntrObjDecompOut;

}DPU_DopplerProc_DDMA_Decompression_EdmaCfg;

/**
 * @brief
 *  dopplerProc DPU EDMA configuration parameters
 *
 * @details
 *  The structure is used to hold the EDMA configuration parameters
 *  for the Doppler Processing DPU
 *
 *  \ingroup DPU_DOPPLERPROC_EXTERNAL_DATA_STRUCTURE
 */
typedef struct DPU_DopplerProc_DDMA_Doppler_EdmaCfg_t
{

    /*! @brief  EDMA configuration for Doppler FFT input data (DecompressedBuf cube -> HWA memory). */
    DPU_DopplerProc_Edma edmaIn;

    /*! @brief  EDMA configuration for Doppler FFT input data Signature Channel. */
    DPU_DopplerProc_Edma edmaInSignature;

    /*! @brief  EDMA configuration for DDMA Metric Output data.
     *  This Channel brings the Max Subband Idx Output if
     *  DDMA demodulation is performed on HWA  */
    DPU_DopplerProc_Edma edmaMaxSubbandOut;

    /*! @brief Interrupt Object for DDMA Metric/ Max Subband Idx Out */
    DPU_DopplerProc_EdmaIntrObj edmaIntrObjMaxSubbandOut;

    /*! @brief  EDMA configuration for SumRX  + log Abs + SumTX Output Output data (HWA memory -> detection matrix). */
    DPU_DopplerProc_Edma edmaSumLogAbsOut;

    /*! @brief  SEEKER: EDMA configuration for the full DDMA-Metric hypothesis-energy
     *  array (HWA M0/M2 -> L3 scratch). Chained to edmaMaxSubbandOut; only used when
     *  the empty-band leakage gate is enabled. Channel 0 means "not allocated". */
    DPU_DopplerProc_Edma edmaDdmaMetricOut;

}DPU_DopplerProc_DDMA_Doppler_EdmaCfg;

/**
 * @brief
 *  dopplerProc DPU EDMA configuration parameters
 *
 * @details
 *  The structure is used to hold the EDMA configuration parameters
 *  for the Doppler Processing DPU
 *
 *  \ingroup DPU_DOPPLERPROC_EXTERNAL_DATA_STRUCTURE
 */
typedef struct DPU_DopplerProcHWA_EdmaCfg_t
{

    /*! @brief  EDMA driver handle. */
    EDMA_Handle edmaHandle;

    /*! @brief  EDMA config for decompression stage. */
    DPU_DopplerProc_DDMA_Decompression_EdmaCfg decompEdmaCfg;

    /*! @brief  EDMA config for doppler demod stage. */
    DPU_DopplerProc_DDMA_Doppler_EdmaCfg dopplerEdmaCfg;

    /*! @brief  EDMA config for detected objects antenna samples copy. */
    DPEDMA_ChanCfg  edmaDetObjAntSamples;

}DPU_DopplerProcHWA_EdmaCfg;

typedef struct DetObjParams_t
{
    /*! @brief  Azim FFT Samples */
    cmplx32ImRe_t azimSamples[NUM_TXANT_AZIM * NUM_RXANT] __attribute__((aligned(8)));

    /*! @brief  Elev FFT Samples */
    cmplx32ImRe_t elevSamples[NUM_TXANT_ELEV * NUM_RXANT] __attribute__((aligned(8)));

    /*! @brief  Azimuth Index */
    uint32_t    azimIdx;

    /*! @brief  Doppler Idx relative to sub band */
    uint32_t    dopIdx;

    /*! @brief  Range Gate Idx */
    uint32_t    rangeIdx;

    /*! @brief  Actual doppler Idx */
    uint32_t    dopIdxActual;

    /*! @brief  CFAR noise value */
    uint32_t    dopCfarNoise;

    /*! @brief  Peak samples Azim */
    uint32_t    azimPeakSamples[3];

}DetObjParams;

/**
 * @brief
 *  Doppler DPU HW configuration parameters
 *
 * @details
 *  The structure is used to hold the  HW configuration parameters
 *  for the Doppler DPU
 *
 *  \ingroup DPU_DOPPLERPROC_EXTERNAL_DATA_STRUCTURE
 */
typedef struct DPU_DopplerProcHWA_HW_Resources_t
{
    /*! @brief  EDMA configuration */
    DPU_DopplerProcHWA_EdmaCfg edmaCfg;

    /*! @brief  HWA configuration */
    DPU_DopplerProcHWA_HwaCfg  hwaCfg;

    /*! @brief  Radar Cube (Compressed) */
    DPIF_RadarCube radarCube;

    /*! @brief  Detection matrix */
    DPIF_DetMatrix detMatrix;

    /*! @brief  Local scratch buffer for decompression (will store a
                decompressed radar cube block) */
    uint8_t * decompScratchBuf;

    /*! @brief  Local scratch buffer for decompression */
    uint32_t decompScratchBufferSizeBytes;

#ifndef ENABLE_HISTOGRAM_BASED_DOP_AZIM_DETECTION
    /*! @brief  Maximum number of CFAR peaks that can be detected */
    uint32_t maxCfarPeaksToDetect;

    /*! @brief  Range-dependent CFAR threshold scaling Look-Up Table
     *
     * This LUT stores pre-computed threshold scale adjustments for each range bin when
     * variable CFAR threshold mode is enabled (variableThresholdMode = 1).
     *
     * Array size: numRangeBins elements
     * Units: Threshold adjustment value in log2 5.11 fixed-point format (scale factor: 2048)
     *
     * The LUT is populated by DPU_DopplerProcHWA_configCfarThreshScaleLUT() during
     * configuration and implements the following threshold profile:
     * - First 25% of range bins: Constant offset of +7dB above baseline
     * - Remaining 75% of range bins: Linear taper from +7dB to 0dB
     *
     * During processing, the threshold for range bin i is computed as:
     *   actualThreshold = thresholdScale + cfarThreshScaleLUT[i]
     * where thresholdScale is the baseline threshold set for the far-field.
     *
     * Memory allocation: Must be allocated by application before DPU configuration.
     * The array should persist throughout the lifetime of DPU operation.
     */
    uint32_t * cfarThreshScaleLUT;
#endif

    /*! @brief  Local scratch buffer storing Doppler Max SubBand Idx */
    uint8_t * dopMaxSubBandScratchBuf[2];

    /*! @brief  Size of local scratch buffer storing Doppler Max SubBand Idx (Ping + pong)*/
    uint32_t dopMaxSubBandScratchBufferSizeBytes;

    /*! @brief  SEEKER: scratch buffers (ping/pong) receiving the DDMA-Metric
     *  hypothesis energies Z[numBandsTotal][numDopplerBins/numBandsTotal]
     *  (uint32 each, numDopplerBins entries per buffer). NULL disables the
     *  empty-band leakage gate regardless of the CLI setting. */
    uint32_t * ddmaMetricScratchBuf[2];

    /*! @brief  SEEKER: total size (ping + pong) of ddmaMetricScratchBuf in bytes */
    uint32_t ddmaMetricScratchBufferSizeBytes;

    /*! @brief List of detected objects */
    DetObjParams * detObjList;

    /*! @brief Max size of detected objects list to be stored per range-gate after range-dop processing */
    uint32_t maxObjListPerRGateSize;

    /*! @brief Max number of final detected objects to be stored after dop and range cfar stage intersection. */
    uint32_t finalMaxNumDetObjs;

    /*! @brief List of final detected objects after intersection with rangecfar */
    DetObjParams * finalDetObjList;

    /*! @brief      Detected objects output data */
    DPIF_PointCloudCartesian  *  objOut;

    /*! @brief Max size of detected object output data in bytes */
    uint32_t objOutSizeInBytes;

}DPU_DopplerProcHWA_HW_Resources;


/**
 * @brief
 *  DopplerProc HWA DDMA Decompression hardware resources
 *
 * @details
 *  The structure is used to hold the hardware resources needed for decompression of Range FFT
 *
 *  \ingroup DPU_DOPPLERPROC_EXTERNAL_DATA_STRUCTURE
 */
typedef struct DPU_DopplerProc_DecompressionCfg_t
{
    /*! @brief Flag that indicates if compression is enabled */
    bool  isEnabled;

    /*! @brief Compression Method, 0 indicates EGE and 1 indicates BFP */
    uint8_t  compressionMethod;

    /*! @brief Compression ration, a value between 0 and 1 */
    float  compressionRatio;

    /*! @brief Indicates the number of range bins to be compressed in a single compression block */
    uint16_t  rangeBinsPerBlock;

    /*! @brief Can be greater than 1 only for DPIF_RADARCUBE_FORMAT_2.
               For DPIF_RADARCUBE_FORMAT_1 this should be set to 1 */
    uint16_t  numRxAntennaPerBlock;

    /*! @brief extra compression parameters for BFP, dependent on enabled number of RX antenna*/
    uint8_t bfpCompExtraParamSets;

}DPU_DopplerProc_DecompressionCfg;

/**
 * @brief
 *  CFAR Configuration
 *
 * @details
 *  The structure contains the cfar configuration used in data path
 */
typedef struct DPU_DopplerProc_CfarCfg_t
{
    /*! @brief    CFAR Enabled */
    uint8_t       isEnabled;

    /*! @brief    CFAR threshold scale */
    uint16_t       thresholdScale;

    /*! @brief    CFAR averagining mode 0-CFAR_CA, 1-CFAR_CAGO, 2-CFAR_CASO, 3-CFAR_OS(HWA2.0 only) */
    uint8_t        averageMode;

    /*! @brief    CFAR noise averaging one sided window length */
    uint8_t        winLen;

    /*! @brief    CFAR one sided guard length*/
    uint8_t        guardLen;

    /*! @brief    CFAR cumulative noise sum divisor
                  CFAR_CA:
                        noiseDivShift should account for both left and right noise window
                        ex: noiseDivShift = ceil(log2(2 * winLen))
                  CFAR_CAGO/_CASO:
                        noiseDivShift should account for only one sided noise window
                        ex: noiseDivShift = ceil(log2(winLen))
     */
    uint8_t        noiseDivShift;

    /*! @brief    CFAR 0-cyclic mode disabled, 1-cyclic mode enabled */
    uint8_t        cyclicMode;

    /*! @brief    Peak grouping scheme 1-based on neighboring peaks from detection matrix
     *                                 2-based on on neighboring CFAR detected peaks.
     *            Scheme 2 is not supported on the HWA version (cfarprochwa.h) */
    uint8_t        peakGroupingScheme;

    /*! @brief     Peak grouping, 0- disabled, 1-enabled */
    uint8_t        peakGroupingEn;

    /*! @brief     The ordered statistic K in CFAR_OS */
    uint8_t        osKvalue;

    /*! @brief     Only used in CFAR_OS non-cyclic mode, scaling of K value for edge samples,
     *             0- disabled, 1-enabled */
    uint8_t        osEdgeKscaleEn;

    /*! @brief     Variable CFAR Thresholds based on Range Index,
     *             0- disabled, 1-enabled
     *
     * When enabled, this feature implements range-dependent CFAR thresholds to improve
     * detection performance across different ranges. The threshold varies per range bin:
     * - Near-field bins (first 25% of range): Higher constant threshold (+7dB above far-field)
     * - Mid-to-far field bins: Linearly decreasing threshold from near to far
     * - Far-field bins: Baseline threshold (specified by thresholdScale parameter)
     *
     * This compensates for the higher clutter and SNR typically present in near-field bins
     * while maintaining sensitivity for far-field targets. The threshold adjustment is
     * computed during configuration and stored in cfarThreshScaleLUT, then applied
     * dynamically during processing on a per-range-bin basis.
     *
     * Note: The thresholdScale parameter should be set for the far-field (last range gate).
     * Near-field thresholds are automatically scaled higher based on the algorithm.
     */
    uint8_t        variableThresholdMode;

    /*! @brief     SEEKER empty-band leakage gate configuration (see
     *             @ref DPU_DopplerProc_EmptyBandGateCfg). Appended at the end of the
     *             struct so the legacy cfarCfg CLI blob layout is unchanged; the
     *             cfarCfg CLI handler memsets the whole struct, so the
     *             emptyBandGateCfg CLI command must come AFTER cfarCfg in the
     *             configuration file. */
    struct DPU_DopplerProc_EmptyBandGateCfg_t
    {
        /*! @brief 0 - gate disabled (default, stock behavior), 1 - enabled,
         *  2 - SEEKER agv3 observe-only: margins are measured and reported in
         *  the telemetry but no candidate is ever rejected (zero-risk
         *  calibration mode). */
        uint8_t  enabled;

        /*! @brief explicit pad for deterministic layout across cores */
        uint8_t  reserved;

        /*! @brief margin threshold in dB, CLI-encoded as dB*100 (0 => 12 dB default) */
        uint16_t marginDbEnc;

        /*! @brief SEEKER agv3: optional margin threshold directly in raw
         *  DDMA-Metric LSBs. 0 = use the dB path above; non-zero overrides
         *  it, bypassing the dB->LSB conversion entirely so the gate can be
         *  calibrated live against the queryDemoStatus margin telemetry
         *  without a reflash. */
        uint32_t marginRawOvr;
    } emptyBandGateCfg;

} DPU_DopplerProc_CfarCfg;

/**
 * @brief
 *  Local Max Configuration
 *
 * @details
 *  The structure contains the local max thresholds/config used in data path
 */
typedef struct DPU_DopplerProc_LocalMaxCfg_t
{
    /*! @brief    Azim threshold scale */
    uint16_t azimThreshold;

    /*! @brief    Azim threshold scale */
    uint16_t dopplerThreshold;

} DPU_DopplerProc_LocalMaxCfg;

/**
 * @brief
 *  Doppler DPU static configuration parameters
 *
 * @details
 *  The structure is used to hold the static configuration parameters
 *  for the Doppler DPU. The following conditions must be satisfied:
 *
 *    @verbatim
      numTxAntennas * numRxAntennas * numDopplerChirps * sizeof(cmplx16ImRe_t) <= X
         where X = 16 KB (one HWA memory bank) for HWA 1.0
               X = 32 KB (two HWA memory banks) for HWA 2.0

      numTxAntennas * numRxAntennas * numDopplerBins * sizeof(uint16_t) <= X
         where X = 16 KB (one HWA memory bank) for HWA 1.0
               X = 32 KB (two HWA memory banks) for HWA 2.0
      @endverbatim
 *
 *  \ingroup DPU_DOPPLERPROC_EXTERNAL_DATA_STRUCTURE
 */
typedef struct DPU_DopplerProcHWA_StaticConfig_t
{
    /*! @brief  Number of transmit antennas */
    uint8_t     numTxAntennas;

    /*! @brief  Number of transmit antennas */
    uint8_t     numAzimTxAntennas;

    /*! @brief  Number of receive antennas */
    uint8_t     numRxAntennas;

    /*! @brief  Number of virtual antennas */
    uint8_t     numVirtualAntennas;

    /*! @brief  Number of range bins */
    uint16_t    numRangeBins;

    /*! @brief  Number of Doppler chirps. */
    uint16_t    numChirps;

    /*! @brief  Number of Doppler bins */
    uint16_t    numDopplerFFTBins;

    /*! @brief  Number of Azimuth FFT Bins */
    uint16_t     numAzimFFTBins;

    /*! @brief  Size of input samples (radarcube samples) in bytes */
    uint16_t    sizeOfInputSample;

	/*! @brief  maximum number of detected objects */
    uint32_t    maxNumObj;

    /*! @brief  Log2 of number of Doppler bins */
    uint8_t     log2NumDopplerBins;

    /*! @brief  Total number of subbands */
    uint8_t     numBandsTotal;

    /*! @brief  If range CFAR is disabled, we may disable Sum Tx calculation. */
    uint8_t     isSumTxEnabled;

    /* @brief   Zero Insertion Mask in Azimuth Antenna Array */
    uint64_t     zeroInsrtMaskAzim;

    /*! @brief Decompression Configuration */
    DPU_DopplerProc_DecompressionCfg decompCfg;

#ifndef ENABLE_HISTOGRAM_BASED_DOP_AZIM_DETECTION
    /* CFAR Configuration */
    DPU_DopplerProc_CfarCfg cfarCfg;
#endif
    /* Local Max Configuration */
    DPU_DopplerProc_LocalMaxCfg localMaxCfg;

    /*! @brief Antenna Calibration Configuration, complex float value, hence the factor of 2, IM RE format */
    float antennaCalibParams[NUM_TXANT * NUM_RXANT * 2U];

    uint16_t antennaGeometryCfg[NUM_TXANT * NUM_RXANT];

}DPU_DopplerProcHWA_StaticConfig;

/**
 * @brief
 *  dopplerProc DPU configuration parameters
 *
 * @details
 *  The structure is used to hold the configuration parameters
 *  for the Doppler Processing removal DPU
 *
 *  \ingroup DPU_DOPPLERPROC_EXTERNAL_DATA_STRUCTURE
 */
typedef struct DPU_DopplerProcHWA_Config_t
{
    /*! @brief HW resources. */
    DPU_DopplerProcHWA_HW_Resources  hwRes;

    /*! @brief Static configuration. */
    DPU_DopplerProcHWA_StaticConfig  staticCfg;

}DPU_DopplerProcHWA_Config;


/**
 * @brief
 *  SEEKER agv3: empty-band gate margin telemetry.
 *
 * @details
 *  Per-frame snapshot (plus cumulative counters) of the raw DDMA-Metric
 *  margins min(Z[win]-Z[win+1], Z[win]-Z[win-1]) the gate computes for every
 *  CFAR candidate it evaluates. Lets the host read counts-per-dB separation
 *  between real reflectors and the phase-shifter ghost comb directly off live
 *  returns, and verify which threshold is actually applied. All raw fields
 *  are in DDMA-Metric output LSBs; divide by marginLsbPerDb for dB.
 *
 *  \ingroup DPU_DOPPLERPROC_EXTERNAL_DATA_STRUCTURE
 */
typedef struct DPU_DopplerProc_EmptyBandGateStats_t
{
    /*! @brief 1 when the gate was active for the reported frame; 0 keeps all
     *  other fields meaningless (struct is zeroed while inactive) */
    uint8_t  gateActive;

    /*! @brief 1 when running observe-only (enabled=2): margins measured and
     *  counted, no candidate rejected */
    uint8_t  observeOnly;

    /*! @brief explicit pad for deterministic cross-core layout */
    uint8_t  pad[2];

    /*! @brief margin threshold actually applied, raw LSBs (post dB-conversion
     *  or raw override) - read this back to verify what is live */
    int32_t  appliedMarginRaw;

    /*! @brief raw LSBs per dB for the active antenna configuration:
     *  (log2(10)/20) * 2048 * 256 * numRx / 2^ceil(log2(numRx)) */
    float    marginLsbPerDb;

    /*! @brief histogram bin width = (1 << histBinShift) raw LSBs */
    uint32_t histBinShift;

    /*! @brief LAST FRAME: number of CFAR candidates the gate evaluated */
    uint32_t numCandidates;

    /*! @brief LAST FRAME: candidates below the margin threshold (in observe
     *  mode these are would-be rejections; in gate mode, actual rejections) */
    uint32_t numRejected;

    /*! @brief LAST FRAME: candidates with a negative margin (an empty-band
     *  hypothesis outscored the winner - the ghost-comb signature) */
    uint32_t numNegative;

    /*! @brief LAST FRAME: minimum margin over the candidates, raw LSBs;
     *  INT32_MAX when numCandidates == 0 */
    int32_t  minMarginRaw;

    /*! @brief LAST FRAME: maximum margin, raw LSBs; INT32_MIN when empty */
    int32_t  maxMarginRaw;

    /*! @brief LAST FRAME: margin histogram,
     *  bin = min(margin >> histBinShift, numBins-1); negative margins land in
     *  bin 0 (and are counted in numNegative) */
    uint32_t hist[SEEKER_EMPTYBAND_HIST_NUM_BINS];

    /*! @brief CUMULATIVE since configuration: frames processed with the gate
     *  active */
    uint32_t frameCount;

    /*! @brief CUMULATIVE: candidates evaluated */
    uint32_t totalCandidates;

    /*! @brief CUMULATIVE: candidates below the threshold */
    uint32_t totalRejected;

} DPU_DopplerProc_EmptyBandGateStats;

/**
 * @brief
 *  DPU processing output parameters
 *
 * @details
 *  The structure is used to hold the output parameters DPU processing
 *
 *  \ingroup DPU_DOPPLERPROC_EXTERNAL_DATA_STRUCTURE
 */
typedef struct DPU_DopplerProcHWA_OutParams_t
{
    /*! @brief DPU statistics */
    DPU_DopplerProc_Stats  stats;

    /*! @brief Number of Objects Out */
    uint32_t  numObjOut;

    /*! @brief SEEKER agv3: empty-band gate margin telemetry (zeroed when the
     *  gate is inactive; gateActive flags validity) */
    DPU_DopplerProc_EmptyBandGateStats emptyBandGateStats;

}DPU_DopplerProcHWA_OutParams;


DPU_DopplerProcHWA_Handle DPU_DopplerProcHWA_init(
    DPU_DopplerProcHWA_InitParams *initCfg,
    volatile uint8_t subframeCounter,
    int32_t* errCode
)
#if defined(SUBSYS_M4) && SUBSYS_M4
__attribute__((section (".customCode")));
#else
;
#endif

int32_t DPU_DopplerProcHWA_config(
    DPU_DopplerProcHWA_Handle handle,
    DPU_DopplerProcHWA_Config *cfg, int32_t isFullConfig
);

int32_t DPU_DopplerProcHWA_process(
    DPU_DopplerProcHWA_Handle handle,
    DPU_DopplerProcHWA_Config *cfg,
    DPU_DopplerProcHWA_OutParams *outParams);

int32_t DPU_DopplerProcHWA_deinit(DPU_DopplerProcHWA_Handle handle);


#ifdef __cplusplus
}
#endif

#endif
