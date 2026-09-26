/* ISP memory-to-memory replay: feed the SAME RAW Bayer frame through the ISP
 * repeatedly and ship every output to the host.
 *
 * WHY: the interleave experiment proved the left-edge band is created by the
 * ISP colour pipeline (RAW 0/4 vs RGB 6/7 banded, p=0.015), but on live sensor
 * input every frame differs, so "intermittent" cannot be separated into
 * input-dependent vs ISP-internal-state-dependent.  Replaying ONE frame N
 * times removes the input as a variable entirely:
 *     outputs differ between runs  -> ISP internal state, proven
 *     outputs identical, banded    -> deterministic function of this input
 *     outputs identical, clean     -> band needs live-timing context
 *
 * DATA PATH (all register facts from hw_ver1 headers, v1.3 silicon):
 *     PSRAM raw frame
 *       -> DW-GDMA chan A (src mem, dst PERIPH_ISP @ MIPI_CSI_BRG_MEM_BASE)
 *       -> ISP (input_data_source = DWGDMA, isp_in_src = 2)
 *       -> CSI bridge
 *       -> DW-GDMA chan B (src PERIPH_CSI @ MIPI_CSI_BRG_MEM_BASE, dst mem)
 *       -> PSRAM rgb frame
 * Per-frame trigger: ISP_DMA_CNTL_REG.ISP_DMA_EN ("write 1 to trigger dma to
 * get 1 frame"); frame size in 64-bit words via ISP_DMA_RAW_DATA_REG.
 * The output side is copied from esp_cam_ctlr_isp_dvp (the in-tree proof that
 * the CSI bridge captures ISP output with the CSI host idle).  The input side
 * has no in-tree user — vendored-IDF patches: isp_core DWGDMA guard removed,
 * dw_gdma_ll dst master port taught the CSI window.
 *
 * PRECONDITION: boot in RAW diag mode (`isp fmt raw`) so the live pipeline's
 * buffers are small; nn_camera__m2m_shutdown() then frees the whole esp_video
 * path before we allocate ours.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "soc/soc.h"
#include "soc/isp_reg.h"
#include "soc/reg_base.h"
#include "driver/isp.h"
#include "esp_private/dw_gdma.h"
#include "esp_private/mipi_csi_share_hw_ctrl.h"
#include "hal/mipi_csi_brg_ll.h"
#include "esp_check.h"
#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_cam_m2m);
#define TAG "nn_cam_m2m"

#include "nn_camera_diag.h"
#include "../src/nn_camera_internal.h"

#if !CONFIG_NN_CAMERA_DIAG_ISP
/* The replay needs the diag build's pipeline-shutdown hook and RAW boot mode;
 * in production it is deliberately absent. */
esp_err_t nn_camera_diag_m2m(const char *host, uint16_t port, int iterations)
{
    (void)host; (void)port; (void)iterations;
    return ESP_ERR_NOT_SUPPORTED;
}
#else


#define M2M_W 1920
#define M2M_H 1296
#define RAW_BYTES  ((size_t)M2M_W * M2M_H)          /* RAW8  */
#define RGB_BYTES  ((size_t)M2M_W * M2M_H * 3)      /* RGB888 */

esp_err_t nn_camera_diag_m2m(const char *host, uint16_t port, int iterations)
{
    esp_err_t ret = ESP_OK;
    if (iterations < 1) iterations = 1;
    if (iterations > 32) iterations = 32;

    /* 1. take the live pipeline down and keep its last RAW frame */
    uint8_t *raw = heap_caps_aligned_alloc(128, RAW_BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *out = NULL;
    if (!raw) return ESP_ERR_NO_MEM;
    esp_err_t r = nn_camera__m2m_shutdown(raw, RAW_BYTES);
    if (r != ESP_OK) {
        NN_LOG_ERR("pipeline shutdown/snap failed: %d (boot with `isp fmt raw`)", r);
        free(raw);
        return r;
    }
    esp_cache_msync(raw, RAW_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    /* Ground truth FIRST: ship the snapped input before any replay hardware is
     * touched, so the host can diff outputs against it even if we crash. */
    {
        uint8_t hdr[NN_CAM_DIAG_HDR_SIZE];
        memset(hdr, 0, sizeof hdr);
        memcpy(hdr, NN_CAM_DIAG_MAGIC, 4);
        strncpy((char *)hdr + 4, "m2m_input", 11);
        uint32_t *f = (uint32_t *)(hdr + 16);
        f[0] = M2M_W; f[1] = M2M_H; f[2] = 0x31384142 /* 'BA81' */;
        f[3] = M2M_W; f[4] = M2M_H; f[5] = (uint32_t)RAW_BYTES;
        r = nn_camera_diag_net_send(host, port, hdr, sizeof hdr, raw, RAW_BYTES);
        NN_LOG_INF("m2m input shipped -> %s", esp_err_to_name(r));
    }

    out = heap_caps_aligned_alloc(128, RGB_BYTES, MALLOC_CAP_SPIRAM);
    if (!out) { free(raw); return ESP_ERR_NO_MEM; }

    /* 2. our own ISP processor, fed from memory */
    isp_proc_handle_t proc = NULL;
    esp_isp_processor_cfg_t cfg = {
        .clk_src = ISP_CLK_SRC_DEFAULT,
        .input_data_source = ISP_INPUT_DATA_SOURCE_DWGDMA,
        .input_data_color_type = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB888,
        .h_res = M2M_W,
        .v_res = M2M_H,
        .yuv_range = COLOR_RANGE_FULL,
        .yuv_std = ISP_YUV_CONV_STD_BT601,
        .clk_hz = 120 * 1000 * 1000,
        .bayer_order = COLOR_RAW_ELEMENT_ORDER_BGGR,
        .has_line_start_packet = false,
        .has_line_end_packet = false,
    };
    ESP_GOTO_ON_ERROR(esp_isp_new_processor(&cfg, &proc), fail, TAG, "new ISP");
    ESP_GOTO_ON_ERROR(esp_isp_enable(proc), fail, TAG, "enable ISP");

    /* 3. CSI bridge for the output (isp_dvp recipe) */
    int brg_id = 0;
    ESP_GOTO_ON_ERROR(mipi_csi_brg_claim(MIPI_CSI_BRG_USER_ISP_DVP, &brg_id), fail, TAG, "claim brg");
    csi_brg_dev_t *brg = MIPI_CSI_BRG_LL_GET_HW(brg_id);
    mipi_csi_brg_ll_set_intput_data_h_pixel_num(brg, M2M_W);
    mipi_csi_brg_ll_set_intput_data_v_row_num(brg, M2M_H);
    mipi_csi_brg_ll_set_burst_len(brg, 512);

    /* 4. output DMA: CSI bridge -> memory (verbatim from esp_cam_ctlr_isp_dvp) */
    dw_gdma_channel_handle_t out_ch = NULL, in_ch = NULL;
    dw_gdma_channel_alloc_config_t out_alloc = {
        .src = { .block_transfer_type = DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS,
                 .role = DW_GDMA_ROLE_PERIPH_CSI,
                 .handshake_type = DW_GDMA_HANDSHAKE_HW,
                 .num_outstanding_requests = 5,
                 .status_fetch_addr = MIPI_CSI_BRG_MEM_BASE },
        .dst = { .block_transfer_type = DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS,
                 .role = DW_GDMA_ROLE_MEM,
                 .handshake_type = DW_GDMA_HANDSHAKE_HW,
                 .num_outstanding_requests = 5 },
        .flow_controller = DW_GDMA_FLOW_CTRL_SRC,
        .chan_priority = 1,
    };
    ESP_GOTO_ON_ERROR(dw_gdma_new_channel(&out_alloc, &out_ch), fail, TAG, "out chan");
    /* NO event callbacks anywhere in this rig.  Registering the full_trans_done
     * callback enables the channel's interrupts, and with this experimental
     * data path something storms an ISR hard enough to trip the CPU1 interrupt
     * watchdog (twice, with ISP+bridge ints masked).  Completion is detected by
     * POLLING the tail of the poisoned output buffer instead — slower, but no
     * interrupt path at all. */

    /* 5. input DMA: memory -> ISP FIFO */
    dw_gdma_channel_alloc_config_t in_alloc = {
        .src = { .block_transfer_type = DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS,
                 .role = DW_GDMA_ROLE_MEM,
                 .handshake_type = DW_GDMA_HANDSHAKE_HW,
                 .num_outstanding_requests = 5 },
        .dst = { .block_transfer_type = DW_GDMA_BLOCK_TRANSFER_CONTIGUOUS,
                 .role = DW_GDMA_ROLE_PERIPH_ISP,
                 .handshake_type = DW_GDMA_HANDSHAKE_HW,
                 .num_outstanding_requests = 5,
                 .status_fetch_addr = MIPI_CSI_BRG_MEM_BASE },
        .flow_controller = DW_GDMA_FLOW_CTRL_DST,   /* the ISP paces the reads */
        .chan_priority = 2,
    };
    ESP_GOTO_ON_ERROR(dw_gdma_new_channel(&in_alloc, &in_ch), fail, TAG, "in chan");

    /* 6. ISP DMA-input parameters: frame length in 64-bit words + burst.
     * ISP_DMA_BURST_LEN default 128 (x8 B = 1 KB per request);
     * ISP_DMA_DATA_TYPE default 42 = 0x2A = MIPI RAW8, exactly our input. */
    REG_WRITE(ISP_DMA_RAW_DATA_REG, (RAW_BYTES / 8) | (1UL << 31));   /* NUM_TOTAL + SET */
    /* BURST_LEN must MATCH the DW-GDMA burst (dma_msize): both sides count one
     * handshake as one burst, and a mismatch (default 128 words vs our 16-beat
     * bursts) floods the ISP input FIFO — the exact "fifo overflow" storm the
     * first attempt produced.  INTERVAL spaces the requests out; the replay has
     * no realtime requirement, so pace it far below the ISP's ~80 MB/s
     * consumption rate: 128 B per 512 cycles @80 MHz = 20 MB/s.  At 160 MB/s
     * (interval 64) the input FIFO overflowed mid-frame and every dropped byte
     * slipped the line phase — the output was horizontal noise with the right
     * mean and no scene correlation (-0.07 vs the input). */
    uint32_t cntl = REG_READ(ISP_DMA_CNTL_REG);
    cntl &= ~(ISP_DMA_BURST_LEN_M | ISP_DMA_INTERVAL_M);
    cntl |= (16u << ISP_DMA_BURST_LEN_S) | (512u << ISP_DMA_INTERVAL_S);
    REG_WRITE(ISP_DMA_CNTL_REG, cntl | ISP_DMA_UPDATE_REG);
    cntl = REG_READ(ISP_DMA_CNTL_REG);
    NN_LOG_INF("m2m: ISP_DMA_CNTL=0x%08X RAW_NUM=%u words", (unsigned)cntl, (unsigned)(RAW_BYTES / 8));

    /* Silence every ISP and bridge interrupt for the replay.  esp_isp_enable
     * leaves whatever int-enable state the driver defaults to, and with no
     * handler consuming the statuses the first frame raised an interrupt storm
     * that tripped the CPU1 interrupt watchdog (panic PC pointed into the ISP
     * register block).  The replay is fully polled — the only signal we need is
     * the OUTPUT DMA's full_trans_done, which is DW-GDMA's own interrupt. */
    REG_WRITE(ISP_INT_ENA_REG, 0);
    REG_WRITE(ISP_INT_CLR_REG, 0xFFFFFFFF);
    REG_WRITE(DR_REG_CSI_BRG_BASE + 0x28, 0);            /* CSI_BRG_INT_ENA */
    REG_WRITE(DR_REG_CSI_BRG_BASE + 0x20, 0xFFFFFFFF);   /* CSI_BRG_INT_CLR */

    mipi_csi_brg_ll_enable(brg, true);

    for (int it = 0; it < iterations; it++) {
        memset(out, 0x5A, RGB_BYTES);               /* poison: detect unwritten */
        esp_cache_msync(out, RGB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

        dw_gdma_block_transfer_config_t out_xfer = {
            .src = { .addr = MIPI_CSI_BRG_MEM_BASE,
                     .burst_mode = DW_GDMA_BURST_MODE_FIXED,
                     .burst_items = DW_GDMA_BURST_ITEMS_512,
                     .burst_len = 16, .width = DW_GDMA_TRANS_WIDTH_64 },
            .dst = { .addr = (uint32_t)out,
                     .burst_mode = DW_GDMA_BURST_MODE_INCREMENT,
                     .burst_items = DW_GDMA_BURST_ITEMS_512,
                     .burst_len = 16, .width = DW_GDMA_TRANS_WIDTH_64 },
            .size = RGB_BYTES * 8 / 64,             /* in 64-bit beats */
        };
        ESP_GOTO_ON_ERROR(dw_gdma_channel_config_transfer(out_ch, &out_xfer), fail, TAG, "out xfer");
        ESP_GOTO_ON_ERROR(dw_gdma_channel_enable_ctrl(out_ch, true), fail, TAG, "out en");

        dw_gdma_block_transfer_config_t in_xfer = {
            .src = { .addr = (uint32_t)raw,
                     .burst_mode = DW_GDMA_BURST_MODE_INCREMENT,
                     .burst_items = DW_GDMA_BURST_ITEMS_512,
                     .burst_len = 16, .width = DW_GDMA_TRANS_WIDTH_64 },
            .dst = { .addr = MIPI_CSI_BRG_MEM_BASE,
                     .burst_mode = DW_GDMA_BURST_MODE_FIXED,
                     .burst_items = DW_GDMA_BURST_ITEMS_512,
                     .burst_len = 16, .width = DW_GDMA_TRANS_WIDTH_64 },
            .size = RAW_BYTES * 8 / 64,
        };
        ESP_GOTO_ON_ERROR(dw_gdma_channel_config_transfer(in_ch, &in_xfer), fail, TAG, "in xfer");
        ESP_GOTO_ON_ERROR(dw_gdma_channel_enable_ctrl(in_ch, true), fail, TAG, "in en");

        REG_WRITE(ISP_INT_CLR_REG, 0xFFFFFFFF);
        REG_WRITE(DR_REG_CSI_BRG_BASE + 0x20, 0xFFFFFFFF);
        REG_WRITE(ISP_DMA_CNTL_REG, cntl | ISP_DMA_EN);   /* pull ONE frame */

        /* completion probe sits 20 ROWS above the bottom: the last ~5 rows are
         * legitimately never written (the ISP's spatial filters hold vertical
         * context and, with no frame-end signal in DMA mode, the tail stays in
         * the pipe), so the literal buffer end is the wrong completion signal. */
        const size_t probe_off = RGB_BYTES - (size_t)20 * M2M_W * 3;
        volatile uint8_t *probe = out + probe_off;
        bool done = false;
        for (int w = 0; w < 500 && !done; w++) {          /* up to 5 s */
            vTaskDelay(pdMS_TO_TICKS(10));
            esp_cache_msync((void *)(out + probe_off), 128, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            done = (probe[0] != 0x5A || probe[13] != 0x5A || probe[37] != 0x5A);
        }
        /* Always quiesce BOTH channels before the next reconfig — iterating
         * with the out channel still in its post-transfer state store-faulted
         * in the DMA register block on the second pass. */
        /* abort, not just disable: a channel stopped mid-transfer holds its AXI
         * state and the next register write to it bus-faults (MTVAL 0x50081100,
         * twice).  Abort is the documented recovery for exactly this. */
        dw_gdma_channel_enable_ctrl(in_ch, false);
        dw_gdma_channel_enable_ctrl(out_ch, false);
        dw_gdma_channel_abort(in_ch);
        dw_gdma_channel_abort(out_ch);
        vTaskDelay(pdMS_TO_TICKS(50));
        NN_LOG_INF("m2m iter %d: ISP_INT_RAW=0x%08X BRG_INT_RAW=0x%08X %s",
                   it, (unsigned)REG_READ(ISP_INT_RAW_REG),
                   (unsigned)REG_READ(DR_REG_CSI_BRG_BASE + 0x18),
                   done ? "done" : "TIMEOUT");
        if (!done) ret = ESP_ERR_TIMEOUT;
        esp_cache_msync(out, RGB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        size_t poison = 0;
        for (size_t k = 0; k < RGB_BYTES; k += 997) poison += (out[k] == 0x5A);
        NN_LOG_INF("m2m iter %d: ~%.1f%% of output still poison", it,
                   100.0 * poison / (RGB_BYTES / 997));

        char desc[12];
        snprintf(desc, sizeof desc, "m2m_%02d", it);
        uint8_t hdr[NN_CAM_DIAG_HDR_SIZE];
        memset(hdr, 0, sizeof hdr);
        memcpy(hdr, NN_CAM_DIAG_MAGIC, 4);
        strncpy((char *)hdr + 4, desc, 11);
        uint32_t *f = (uint32_t *)(hdr + 16);
        f[0] = M2M_W; f[1] = M2M_H; f[2] = 0x33424752 /* 'RGB3' */;
        f[3] = M2M_W * 3; f[4] = M2M_H; f[5] = (uint32_t)RGB_BYTES;
        r = nn_camera_diag_net_send(host, port, hdr, sizeof hdr, out, RGB_BYTES);
        NN_LOG_INF("m2m iter %d: sent -> %s", it, esp_err_to_name(r));
    }

    NN_LOG_INF("m2m done (%d iteration(s)); pipeline is DOWN — reboot to restore", iterations);

fail:
    if (in_ch)  dw_gdma_del_channel(in_ch);
    if (out_ch) dw_gdma_del_channel(out_ch);
    /* proc/bridge intentionally left; device is expected to be rebooted */
    free(out);
    free(raw);
    return ret;
}
#endif /* CONFIG_NN_CAMERA_DIAG_ISP */
