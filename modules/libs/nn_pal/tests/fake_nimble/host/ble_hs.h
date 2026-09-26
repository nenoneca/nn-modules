/* SPDX-License-Identifier: Apache-2.0 */
/* Fake of NimBLE's host/ble_hs.h — see fake_nimble.h for the contract. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#define BLE_HS_CONN_HANDLE_NONE 0xFFFF
#define BLE_HS_FOREVER          0x7FFFFFFF
#define BLE_HS_ENOTCONN         7

#define BLE_UUID_TYPE_128 128
typedef struct { uint8_t type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint8_t value[16]; } ble_uuid128_t;

/* — GAP — */
#define BLE_GAP_EVENT_CONNECT      0
#define BLE_GAP_EVENT_DISCONNECT   1
#define BLE_GAP_EVENT_ADV_COMPLETE 9
#define BLE_GAP_EVENT_MTU          14
#define BLE_GAP_CONN_MODE_UND 2
#define BLE_GAP_DISC_MODE_GEN 2
#define BLE_ERR_REM_USER_CONN_TERM 0x13

struct ble_gap_event {
    uint8_t type;
    union {
        struct { int status; uint16_t conn_handle; } connect;
        struct { int reason; } disconnect;
        struct { uint16_t conn_handle; uint16_t value; } mtu;
    };
};
typedef int ble_gap_event_fn(struct ble_gap_event *event, void *arg);

struct ble_gap_adv_params { uint8_t conn_mode, disc_mode; };
struct ble_hs_adv_fields {
    uint8_t flags;
    const ble_uuid128_t *uuids128;
    uint8_t num_uuids128;
    unsigned uuids128_is_complete : 1;
    const uint8_t *name;
    uint8_t name_len;
    unsigned name_is_complete : 1;
};
#define BLE_HS_ADV_F_DISC_GEN   0x02
#define BLE_HS_ADV_F_BREDR_UNSUP 0x04

int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *f);
int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *f);
int ble_gap_adv_start(uint8_t own_addr_type, const void *peer, int32_t dur,
                      const struct ble_gap_adv_params *p,
                      ble_gap_event_fn *cb, void *arg);
int ble_gap_adv_stop(void);
int ble_gap_terminate(uint16_t conn_handle, uint8_t reason);

/* — mbuf: flat buffer in disguise — */
struct os_mbuf { uint8_t buf[600]; uint16_t len; };
#define OS_MBUF_PKTLEN(om) ((om)->len)
int os_mbuf_append(struct os_mbuf *om, const void *data, uint16_t len);
struct os_mbuf *ble_hs_mbuf_from_flat(const void *data, uint16_t len);
int ble_hs_mbuf_to_flat(const struct os_mbuf *om, void *buf,
                        uint16_t cap, uint16_t *outlen);

/* — GATT server — */
#define BLE_GATT_ACCESS_OP_READ_CHR  0
#define BLE_GATT_ACCESS_OP_WRITE_CHR 1
#define BLE_ATT_ERR_READ_NOT_PERMITTED    0x02
#define BLE_ATT_ERR_WRITE_NOT_PERMITTED   0x03
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 0x0d
#define BLE_ATT_ERR_UNLIKELY              0x0e
#define BLE_ATT_ERR_INSUFFICIENT_RES      0x11

struct ble_gatt_access_ctxt { uint8_t op; struct os_mbuf *om; };
typedef uint16_t ble_gatt_chr_flags;
#define BLE_GATT_CHR_F_READ     0x0002
#define BLE_GATT_CHR_F_WRITE    0x0008
#define BLE_GATT_CHR_F_NOTIFY   0x0010
#define BLE_GATT_CHR_F_INDICATE 0x0020

typedef int ble_gatt_access_fn(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg);
struct ble_gatt_chr_def {
    const ble_uuid_t *uuid;
    ble_gatt_access_fn *access_cb;
    void *arg;
    ble_gatt_chr_flags flags;
    uint16_t *val_handle;
};
#define BLE_GATT_SVC_TYPE_PRIMARY 1
struct ble_gatt_svc_def {
    uint8_t type;
    const ble_uuid_t *uuid;
    const struct ble_gatt_chr_def *characteristics;
};

int ble_gatts_count_cfg(const struct ble_gatt_svc_def *defs);
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *defs);
int ble_gatts_notify_custom(uint16_t conn_handle, uint16_t chr_val_handle,
                            struct os_mbuf *om);

/* — host config / state — */
struct ble_hs_cfg_s {
    void (*sync_cb)(void);
    void (*reset_cb)(int reason);
};
extern struct ble_hs_cfg_s ble_hs_cfg;
bool ble_hs_synced(void);
int ble_hs_id_infer_auto(int privacy, uint8_t *own_addr_type);

#ifndef strlcpy
size_t strlcpy(char *dst, const char *src, size_t cap);
#endif
