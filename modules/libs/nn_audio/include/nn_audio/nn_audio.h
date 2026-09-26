/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * nn_audio — P4 microphone capture + AAC-LC encode.
 *
 * Captures mono 16-bit PCM from the ES8311 codec over I2S (or a synthetic sine
 * for HW-free testing), AAC-LC encodes it (esp_audio_codec, ADTS frames), and
 * hands each encoded frame to a callback with the device capture timestamp.
 * The P4 app wraps each frame in nn_aud_hdr_t (magic 'A' + ts) and nn_link_sends
 * it to the C6.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Called (from the capture task) for each encoded AAC frame.
 *   aac    : one ADTS frame
 *   len    : its length
 *   ts_ms  : device timestamp (esp_timer ms) at capture
 *   seq    : monotonically increasing audio frame number */
typedef void (*nn_audio_frame_cb_t)(const uint8_t *aac, size_t len,
                                    uint32_t ts_ms, uint16_t seq, void *ctx);

void      nn_audio_set_frame_cb(nn_audio_frame_cb_t cb, void *ctx);

/* Bring up I2S + ES8311 + the AAC encoder.  Returns an error (and leaves audio
 * disabled) if the codec doesn't probe — video is unaffected. */
esp_err_t nn_audio_init(void);

/* Start the capture+encode task. */
esp_err_t nn_audio_start(void);

/* "<src> <rate>Hz aac=<bitrate> frames=<n> bytes=<n>" status. */
void      nn_audio_status(char *out, size_t n);

/* Register the `aud` console command. */
void      nn_audio_cli_register(void);

#ifdef __cplusplus
}
#endif
