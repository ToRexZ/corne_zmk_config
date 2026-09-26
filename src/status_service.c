/*
 * Keyboard status over a custom GATT service.
 *
 * One read-only characteristic describing what the host cannot otherwise see:
 * the active BLE profile, every profile's bond/connection state and peer
 * address, and the highest active layer. The value is rebuilt on every read,
 * so it is never stale, and reading requires an encrypted (bonded) link.
 *
 * Layout (little endian, version 1) - keep in sync with corne-status:
 *
 *   u8   version            1
 *   u8   profile_count      ZMK_BLE_PROFILE_COUNT
 *   u8   active_profile     index
 *   u8   flags              bit0: active profile connected
 *   u8   highest_layer      index
 *   u8   reserved[3]
 *   u32  layer_state        bitmask of active layers
 *   char layer_name[16]     NUL padded
 *   char profile_name[16]   active profile's name, NUL padded
 *   profile_count x {
 *     u8 flags              bit0: bonded, bit1: connected, bit2: active
 *     u8 addr_type          bt_addr_le_t.type
 *     u8 addr[6]            bt_addr_t.val (least significant byte first)
 *   }
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>

#include <zmk/ble.h>
#include <zmk/keymap.h>

#define STATUS_VERSION 1
#define STATUS_NAME_LEN 16

#define PROFILE_BONDED BIT(0)
#define PROFILE_CONNECTED BIT(1)
#define PROFILE_ACTIVE BIT(2)

#define STATUS_ACTIVE_CONNECTED BIT(0)

static const struct bt_uuid_128 status_service_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0xc4830391, 0xecf5, 0x4bcb, 0x8835, 0xce362675f96e));
static const struct bt_uuid_128 status_char_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0xc4830392, 0xecf5, 0x4bcb, 0x8835, 0xce362675f96e));

struct status_profile {
    uint8_t flags;
    uint8_t addr_type;
    uint8_t addr[6];
} __packed;

struct status_value {
    uint8_t version;
    uint8_t profile_count;
    uint8_t active_profile;
    uint8_t flags;
    uint8_t highest_layer;
    uint8_t reserved[3];
    uint32_t layer_state;
    char layer_name[STATUS_NAME_LEN];
    char profile_name[STATUS_NAME_LEN];
    struct status_profile profiles[ZMK_BLE_PROFILE_COUNT];
} __packed;

static void fill_status(struct status_value *v) {
    memset(v, 0, sizeof(*v));

    int active = zmk_ble_active_profile_index();

    v->version = STATUS_VERSION;
    v->profile_count = ZMK_BLE_PROFILE_COUNT;
    v->active_profile = (uint8_t)active;
    if (zmk_ble_active_profile_is_connected()) {
        v->flags |= STATUS_ACTIVE_CONNECTED;
    }

    zmk_keymap_layer_index_t layer = zmk_keymap_highest_layer_active();
    v->highest_layer = (uint8_t)layer;
    v->layer_state = sys_cpu_to_le32((uint32_t)zmk_keymap_layer_state());

    const char *layer_name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(layer));
    if (layer_name) {
        strncpy(v->layer_name, layer_name, STATUS_NAME_LEN - 1);
    }

    const char *profile_name = zmk_ble_active_profile_name();
    if (profile_name) {
        strncpy(v->profile_name, profile_name, STATUS_NAME_LEN - 1);
    }

    for (uint8_t i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        struct status_profile *p = &v->profiles[i];
        if (!zmk_ble_profile_is_open(i)) {
            p->flags |= PROFILE_BONDED;
            const bt_addr_le_t *addr = zmk_ble_profile_address(i);
            if (addr) {
                p->addr_type = addr->type;
                memcpy(p->addr, addr->a.val, sizeof(p->addr));
            }
        }
        if (zmk_ble_profile_is_connected(i)) {
            p->flags |= PROFILE_CONNECTED;
        }
        if (i == active) {
            p->flags |= PROFILE_ACTIVE;
        }
    }
}

static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                           uint16_t len, uint16_t offset) {
    // Rebuilt per read: a long read arrives as several offset reads, and the
    // few bytes that could change between them are harmless.
    struct status_value v;
    fill_status(&v);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &v, sizeof(v));
}

BT_GATT_SERVICE_DEFINE(corne_status_svc, BT_GATT_PRIMARY_SERVICE(&status_service_uuid),
                       BT_GATT_CHARACTERISTIC(&status_char_uuid.uuid, BT_GATT_CHRC_READ,
                                              BT_GATT_PERM_READ_ENCRYPT, read_status, NULL,
                                              NULL));
