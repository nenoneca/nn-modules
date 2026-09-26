/* Internals shared between nn_camera.c and its diag/ helpers.
 * NOT a public API — the published surface is include/nn_camera/nn_camera.h
 * and diag/nn_camera_diag.h. */
#pragma once
#include <stdint.h>

int      nn_camera__cap_fd(void);        /* capture device fd, -1 before init */
void     nn_camera__diag_arm(void);      /* borrow the next dequeued frame    */
void     nn_camera__diag_disarm(void);
uint32_t nn_camera__diag_len(void);      /* 0 until a frame has been copied   */
const uint8_t *nn_camera__diag_buf(void);
esp_err_t nn_camera__m2m_shutdown(uint8_t *dst, size_t dst_len);
