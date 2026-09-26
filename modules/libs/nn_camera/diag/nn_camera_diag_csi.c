/* MIPI-CSI2 host link statistics: ECC / CRC / frame-sync / lane-sync errors.
 *
 * The ESP32-P4's CSI host (a Synopsys CSI-2 host controller) latches every
 * protocol error into a set of interrupt-status registers.  These are the
 * link-integrity counters you want when deciding whether an image artifact
 * comes from the wire or from processing downstream of it:
 *
 *   ECC_CORRECTED      packet header ECC caught and CORRECTED a bit flip
 *   DATA_ID            unrecognised virtual-channel / data-type in a header
 *   CRC_FRAME_FATAL    frame-level CRC mismatch
 *   PLD_CRC_FATAL      payload CRC mismatch  (corrupt pixel data on the wire)
 *   BNDRY_FRAME_FATAL  frame start/end mismatched  -> FRAME SYNC error
 *   SEQ_FRAME_FATAL    frame numbers out of sequence -> FRAME SYNC error
 *   PKT_FATAL          malformed packet / short packet
 *   PHY_FATAL          per-lane SoT sync failure     -> LANE (line) SYNC error
 *   PHY                per-lane SoT / escape / control errors
 *
 * NOTE ON MASKING: the *_INT_ST_* registers reflect status AFTER the
 * corresponding *_INT_MSK_* mask.  esp_video never programs these masks, so a
 * masked bit would read 0 forever and we would wrongly conclude "no errors".
 * This module therefore UNMASKS everything on first use before reporting.
 *
 * The status bits are read-to-clear, so each call reports errors SINCE THE
 * PREVIOUS CALL; running totals are accumulated here.
 */
#include <stdio.h>
#include <string.h>
#include "soc/soc.h"
#include "soc/mipi_csi_host_reg.h"
#include <nn_osal/log.h>
NN_OSAL_LOG_MODULE(nn_cam_csi);

#include "nn_camera_diag.h"

static struct {
    uint32_t phy_fatal, pkt_fatal, phy, bndry, seq, crc_frame, pld_crc, data_id, ecc;
    uint32_t reads;
} s_tot;

static bool s_unmasked;

static void csi_unmask_all(void)
{
    /* 0 = do not mask, so every error reaches the status registers. */
    REG_WRITE(CSI_HOST_INT_MSK_PHY_FATAL_REG,         0);
    REG_WRITE(CSI_HOST_INT_MSK_PKT_FATAL_REG,         0);
    REG_WRITE(CSI_HOST_INT_MSK_PHY_REG,               0);
    REG_WRITE(CSI_HOST_INT_MSK_BNDRY_FRAME_FATAL_REG, 0);
    REG_WRITE(CSI_HOST_INT_MSK_SEQ_FRAME_FATAL_REG,   0);
    REG_WRITE(CSI_HOST_INT_MSK_CRC_FRAME_FATAL_REG,   0);
    REG_WRITE(CSI_HOST_INT_MSK_PLD_CRC_FATAL_REG,     0);
    REG_WRITE(CSI_HOST_INT_MSK_DATA_ID_REG,           0);
    REG_WRITE(CSI_HOST_INT_MSK_ECC_CORRECTED_REG,     0);
    s_unmasked = true;
}

void nn_camera_diag_csi_reset(void)
{
    if (!s_unmasked) csi_unmask_all();
    /* read-to-clear everything, then zero the totals */
    (void)REG_READ(CSI_HOST_INT_ST_PHY_FATAL_REG);
    (void)REG_READ(CSI_HOST_INT_ST_PKT_FATAL_REG);
    (void)REG_READ(CSI_HOST_INT_ST_PHY_REG);
    (void)REG_READ(CSI_HOST_INT_ST_BNDRY_FRAME_FATAL_REG);
    (void)REG_READ(CSI_HOST_INT_ST_SEQ_FRAME_FATAL_REG);
    (void)REG_READ(CSI_HOST_INT_ST_CRC_FRAME_FATAL_REG);
    (void)REG_READ(CSI_HOST_INT_ST_PLD_CRC_FATAL_REG);
    (void)REG_READ(CSI_HOST_INT_ST_DATA_ID_REG);
    (void)REG_READ(CSI_HOST_INT_ST_ECC_CORRECTED_REG);
    memset(&s_tot, 0, sizeof s_tot);
}

void nn_camera_diag_csi_dump(void)
{
    if (!s_unmasked) {
        csi_unmask_all();
        printf("csi: interrupt masks cleared (errors now latch); "
               "first read may show stale bits\n");
    }

    const uint32_t main_st = REG_READ(CSI_HOST_INT_ST_MAIN_REG);
    const uint32_t phy_f   = REG_READ(CSI_HOST_INT_ST_PHY_FATAL_REG);
    const uint32_t pkt_f   = REG_READ(CSI_HOST_INT_ST_PKT_FATAL_REG);
    const uint32_t phy     = REG_READ(CSI_HOST_INT_ST_PHY_REG);
    const uint32_t bndry   = REG_READ(CSI_HOST_INT_ST_BNDRY_FRAME_FATAL_REG);
    const uint32_t seq     = REG_READ(CSI_HOST_INT_ST_SEQ_FRAME_FATAL_REG);
    const uint32_t crc_f   = REG_READ(CSI_HOST_INT_ST_CRC_FRAME_FATAL_REG);
    const uint32_t pld     = REG_READ(CSI_HOST_INT_ST_PLD_CRC_FATAL_REG);
    const uint32_t did     = REG_READ(CSI_HOST_INT_ST_DATA_ID_REG);
    const uint32_t ecc     = REG_READ(CSI_HOST_INT_ST_ECC_CORRECTED_REG);

    /* The datasheet marks these RC (read-clear), but on this silicon a read
     * alone leaves them asserted — the SAME pattern comes back on a read taken
     * microseconds later, and even with the camera STOPPED (no CSI traffic at
     * all), which cannot be a real error rate.  Try the other common Synopsys
     * convention as well: write the bits back to clear them.  If the values
     * still persist after this, the readout is not tracking the link and must
     * not be trusted. */
    REG_WRITE(CSI_HOST_INT_ST_PHY_FATAL_REG,         phy_f);
    REG_WRITE(CSI_HOST_INT_ST_PKT_FATAL_REG,         pkt_f);
    REG_WRITE(CSI_HOST_INT_ST_PHY_REG,               phy);
    REG_WRITE(CSI_HOST_INT_ST_BNDRY_FRAME_FATAL_REG, bndry);
    REG_WRITE(CSI_HOST_INT_ST_SEQ_FRAME_FATAL_REG,   seq);
    REG_WRITE(CSI_HOST_INT_ST_CRC_FRAME_FATAL_REG,   crc_f);
    REG_WRITE(CSI_HOST_INT_ST_PLD_CRC_FATAL_REG,     pld);
    REG_WRITE(CSI_HOST_INT_ST_DATA_ID_REG,           did);
    REG_WRITE(CSI_HOST_INT_ST_ECC_CORRECTED_REG,     ecc);

    /* each nonzero status word = at least one event in this interval */
    s_tot.phy_fatal += !!phy_f;  s_tot.pkt_fatal += !!pkt_f;  s_tot.phy       += !!phy;
    s_tot.bndry     += !!bndry;  s_tot.seq       += !!seq;    s_tot.crc_frame += !!crc_f;
    s_tot.pld_crc   += !!pld;    s_tot.data_id   += !!did;    s_tot.ecc       += !!ecc;
    s_tot.reads++;

    printf("csi link stats (since last read | intervals-with-errors since reset, %u reads)\n",
           (unsigned)s_tot.reads);
    printf("  main_status              0x%08X\n", (unsigned)main_st);
    printf("  FRAME SYNC  boundary     0x%08X | %u\n", (unsigned)bndry, (unsigned)s_tot.bndry);
    printf("  FRAME SYNC  sequence     0x%08X | %u\n", (unsigned)seq,   (unsigned)s_tot.seq);
    printf("  LANE  SYNC  phy_fatal    0x%08X | %u\n", (unsigned)phy_f, (unsigned)s_tot.phy_fatal);
    printf("  LANE        phy_err      0x%08X | %u\n", (unsigned)phy,   (unsigned)s_tot.phy);
    printf("  CRC         frame        0x%08X | %u\n", (unsigned)crc_f, (unsigned)s_tot.crc_frame);
    printf("  CRC         payload      0x%08X | %u\n", (unsigned)pld,   (unsigned)s_tot.pld_crc);
    printf("  ECC         corrected    0x%08X | %u\n", (unsigned)ecc,   (unsigned)s_tot.ecc);
    printf("  HEADER      data_id      0x%08X | %u\n", (unsigned)did,   (unsigned)s_tot.data_id);
    printf("  PACKET      pkt_fatal    0x%08X | %u\n", (unsigned)pkt_f, (unsigned)s_tot.pkt_fatal);
    const bool clean = !(phy_f | pkt_f | phy | bndry | seq | crc_f | pld | did | ecc);
    printf("  => link %s in this interval\n", clean ? "CLEAN" : "REPORTED ERRORS");
}
