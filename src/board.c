// SPDX-License-Identifier: GPL-2.0-or-later
// copyright (C) 2025 sekigon-gonnoc

#include <zephyr/sys/util_macro.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/logging/log.h>
#include <zephyr/input/input.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/usb.h>
#include <zmk/event_manager.h>
#include <zmk/hid.h>
#include <zmk/keys.h>

LOG_MODULE_REGISTER(split_power_mgmt, CONFIG_ZMK_LOG_LEVEL);

#define SLEEP1_TIMEOUT_MS 5000   // 5 seconds to sleep1 from active
#define SLEEP2_TIMEOUT_MS 15000  // 15 seconds to sleep2 from sleep1  
#define SLEEP3_TIMEOUT_MS 30000  // 30 seconds to sleep3 from sleep2
#define ACTIVE_CONN_INTERVAL CONFIG_ZMK_SPLIT_BLE_PREF_INT
#define SLEEP1_CONN_INTERVAL (CONFIG_ZMK_SPLIT_BLE_PREF_INT*2)
#define SLEEP2_CONN_INTERVAL (CONFIG_ZMK_SPLIT_BLE_PREF_INT*4)
#define SLEEP3_CONN_INTERVAL (CONFIG_ZMK_SPLIT_BLE_PREF_INT*8)
#define CONN_LATENCY CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY
#define SLEEP1_CONN_LATENCY ((CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY+1)/2)
#define SLEEP2_CONN_LATENCY ((CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY+3)/4)  
#define SLEEP3_CONN_LATENCY ((CONFIG_ZMK_SPLIT_BLE_PREF_LATENCY+7)/8) 
#define SUPERVISION_TIMEOUT CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT

enum power_mode {
    POWER_MODE_ACTIVE,
    POWER_MODE_SLEEP1,
    POWER_MODE_SLEEP2,
    POWER_MODE_SLEEP3,
};

static struct k_work_delayable power_mode_work;
static enum power_mode current_mode = POWER_MODE_ACTIVE;
static int64_t last_activity_time = 0;
static struct bt_conn *split_conn = NULL;

// Power mode transition handler
static void power_mode_transition(struct k_work *work) {
    if (!split_conn) {
        return;
    }
    
    // Stay in active mode when USB power is connected
    if (zmk_usb_is_powered()) {
        LOG_DBG("USB power detected, staying in active mode");
        if (current_mode != POWER_MODE_ACTIVE) {
            // Return to active mode
            struct bt_le_conn_param param = {
                .interval_min = ACTIVE_CONN_INTERVAL,
                .interval_max = ACTIVE_CONN_INTERVAL,
                .latency = CONN_LATENCY,
                .timeout = SUPERVISION_TIMEOUT,
            };
            
            int err = bt_conn_le_param_update(split_conn, &param);
            if (err == 0) {
                current_mode = POWER_MODE_ACTIVE;
                LOG_INF("Returned to active mode due to USB power");
            }
        }
        
        // Periodic check while USB power is present
        k_work_schedule(&power_mode_work, K_MSEC(5000));
        return;
    }
    
    int64_t idle_time = k_uptime_get() - last_activity_time;
    enum power_mode target_mode;
    
    // Determine target mode based on idle time
    if (idle_time >= SLEEP3_TIMEOUT_MS) {
        target_mode = POWER_MODE_SLEEP3;
    } else if (idle_time >= SLEEP2_TIMEOUT_MS) {
        target_mode = POWER_MODE_SLEEP2;
    } else if (idle_time >= SLEEP1_TIMEOUT_MS) {
        target_mode = POWER_MODE_SLEEP1;
    } else {
        target_mode = POWER_MODE_ACTIVE;
    }
    
    // Only update if different from current mode
    if (target_mode == current_mode) {
        // Schedule next transition
        int32_t next_timeout;
        switch (current_mode) {
        case POWER_MODE_ACTIVE:
            next_timeout = SLEEP1_TIMEOUT_MS - idle_time;
            break;
        case POWER_MODE_SLEEP1:
            next_timeout = SLEEP2_TIMEOUT_MS - idle_time;
            break;
        case POWER_MODE_SLEEP2:
            next_timeout = SLEEP3_TIMEOUT_MS - idle_time;
            break;
        default:
            return; // No further transitions from SLEEP3
        }
        
        if (next_timeout > 0) {
            k_work_schedule(&power_mode_work, K_MSEC(next_timeout));
        }
        return;
    }
    
    // Configure connection parameters
    struct bt_le_conn_param param;
    const char *mode_name;
    
    switch (target_mode) {
    case POWER_MODE_ACTIVE:
        param.interval_min = param.interval_max = ACTIVE_CONN_INTERVAL;
        param.latency = CONN_LATENCY;
        mode_name = "active";
        break;
    case POWER_MODE_SLEEP1:
        param.interval_min = param.interval_max = SLEEP1_CONN_INTERVAL;
        param.latency = SLEEP1_CONN_LATENCY;
        mode_name = "sleep1";
        break;
    case POWER_MODE_SLEEP2:
        param.interval_min = param.interval_max = SLEEP2_CONN_INTERVAL;
        param.latency = SLEEP2_CONN_LATENCY;
        mode_name = "sleep2";
        break;
    case POWER_MODE_SLEEP3:
        param.interval_min = param.interval_max = SLEEP3_CONN_INTERVAL;
        param.latency = SLEEP3_CONN_LATENCY;
        mode_name = "sleep3";
        break;
    }
    
    param.timeout = SUPERVISION_TIMEOUT;
    
    LOG_INF("Entering %s mode - updating connection parameters", mode_name);
    
    int err = bt_conn_le_param_update(split_conn, &param);
    if (err == 0) {
        current_mode = target_mode;
        LOG_INF("%s mode activated", mode_name);
        
        // Schedule next transition
        int32_t next_timeout;
        switch (current_mode) {
        case POWER_MODE_ACTIVE:
            next_timeout = SLEEP1_TIMEOUT_MS - idle_time;
            break;
        case POWER_MODE_SLEEP1:
            next_timeout = SLEEP2_TIMEOUT_MS - idle_time;
            break;
        case POWER_MODE_SLEEP2:
            next_timeout = SLEEP3_TIMEOUT_MS - idle_time;
            break;
        default:
            return; // No further transitions from SLEEP3
        }
        
        if (next_timeout > 0) {
            k_work_schedule(&power_mode_work, K_MSEC(next_timeout));
        }
    } else {
        LOG_WRN("Failed to update connection parameters for %s mode: %d", mode_name, err);
    }
}

// Reset activity timer on user input
static void reset_idle_timer(void) {
    LOG_DBG("Activity detected - resetting idle timer");
    last_activity_time = k_uptime_get();
    k_work_cancel_delayable(&power_mode_work);
    
    if (current_mode != POWER_MODE_ACTIVE) {
        // Return to active mode immediately
        power_mode_transition(&power_mode_work.work);
    } else {
        // Schedule transition to SLEEP1 from active mode
        k_work_schedule(&power_mode_work, K_MSEC(SLEEP1_TIMEOUT_MS));
    }
}

static int position_state_changed_listener(const zmk_event_t *eh) {
    reset_idle_timer();
    return ZMK_EV_EVENT_BUBBLE;
}


ZMK_LISTENER(split_power_mgmt_position, position_state_changed_listener);
ZMK_SUBSCRIPTION(split_power_mgmt_position, zmk_position_state_changed);

static bool is_split_peripheral_conn(struct bt_conn *conn) {
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) != 0) {
        return false;
    }
    
    return (info.role == BT_CONN_ROLE_CENTRAL && info.type == BT_CONN_TYPE_LE);
}

static void power_mgmt_bt_conn_connected_cb(struct bt_conn *conn, uint8_t err) {
    if (err || !is_split_peripheral_conn(conn)) {
        return;
    }
    
    LOG_INF("Split peripheral connection detected");
    if (split_conn) {
        bt_conn_unref(split_conn);
    }
    split_conn = bt_conn_ref(conn);
    
    last_activity_time = k_uptime_get();
    k_work_schedule(&power_mode_work, K_MSEC(SLEEP1_TIMEOUT_MS));
}

static void power_mgmt_bt_conn_disconnected_cb(struct bt_conn *conn, uint8_t reason) {
    if (conn != split_conn) {
        return;
    }
    
    LOG_INF("Split peripheral disconnected (reason: %d)", reason);
    
    k_work_cancel_delayable(&power_mode_work);
    bt_conn_unref(split_conn);
    split_conn = NULL;
    current_mode = POWER_MODE_ACTIVE;
}

static struct bt_conn_cb power_mgmt_bt_conn_callbacks = {
    .connected = power_mgmt_bt_conn_connected_cb,
    .disconnected = power_mgmt_bt_conn_disconnected_cb,
};

static void mouse_input_callback(struct input_event *evt) {
    reset_idle_timer();
}

static int split_power_mgmt_init(void) {
    LOG_INF("Initializing split power management");
    
    k_work_init_delayable(&power_mode_work, power_mode_transition);
    
    bt_conn_cb_register(&power_mgmt_bt_conn_callbacks);
    
    if (split_conn) {
        last_activity_time = k_uptime_get();
        k_work_schedule(&power_mode_work, K_MSEC(SLEEP1_TIMEOUT_MS));
        LOG_INF("Split power management initialized with existing connection");
    } else {
        LOG_INF("Split power management initialized - waiting for connection");
    }
    
    return 0;
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET_OR_NULL(DT_NODELABEL(trackball)) , mouse_input_callback);

SYS_INIT(split_power_mgmt_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static bool gs_lshift = false;
static bool gs_rshift = false;

static void proc_regist_keycode(const struct zmk_keycode_state_changed *ev,
                                uint32_t regist_ifshift, bool is_shift_ifshift,
                                uint32_t regist, bool is_shift) {
    bool shift_now = gs_lshift || gs_rshift;

    if (ev->state) {
        /* press */
        if (shift_now) {
            if (!is_shift_ifshift) {
                zmk_hid_unregister_mods(MOD_LSFT | MOD_RSFT);
            }
            zmk_hid_keyboard_press(regist_ifshift);
        } else {
            if (is_shift) {
                zmk_hid_register_mods(MOD_LSFT);
            }
            zmk_hid_keyboard_press(regist);
        }
    } else {
        /* release */
        if (shift_now && !is_shift_ifshift) {
            if (gs_lshift) zmk_hid_register_mods(MOD_LSFT);
            if (gs_rshift) zmk_hid_register_mods(MOD_RSFT);
        }

        zmk_hid_keyboard_release(regist_ifshift);
        zmk_hid_keyboard_release(regist);

        if (!shift_now && is_shift) {
            zmk_hid_unregister_mods(MOD_LSFT);
        }

        if (shift_now && !is_shift_ifshift) {
            if (gs_lshift) zmk_hid_unregister_mods(MOD_LSFT);
            if (gs_rshift) zmk_hid_unregister_mods(MOD_RSFT);
        }
    }
}

static int us_printed_on_jis_keycode_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (!ev) return ZMK_EV_EVENT_BUBBLE;

    uint32_t keycode = ev->keycode;

    switch (keycode) {
    case LEFT_SHIFT:
        gs_lshift = ev->state;
        return ZMK_EV_EVENT_BUBBLE;
    case RIGHT_SHIFT:
        gs_rshift = ev->state;
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (keycode == CAPSLOCK) {
        if (ev->state) {
            zmk_hid_keyboard_press(INT1);
        } else {
            zmk_hid_keyboard_release(INT1);
        }
        return ZMK_EV_EVENT_HANDLED;
    }

    switch (keycode) {
        case N2:
            proc_regist_keycode(ev, LBRC, false, N2, false);
            return ZMK_EV_EVENT_HANDLED;
        case N6:
            proc_regist_keycode(ev, EQUAL, false, N6, false);
            return ZMK_EV_EVENT_HANDLED;
        case N7:
            proc_regist_keycode(ev, N6, true, N7, false);
            return ZMK_EV_EVENT_HANDLED;
        case N8:
            proc_regist_keycode(ev, QUOT, true, N8, false);
            return ZMK_EV_EVENT_HANDLED;
        case N9:
            proc_regist_keycode(ev, N8, true, N9, false);
            return ZMK_EV_EVENT_HANDLED;
        case N0:
            proc_regist_keycode(ev, N9, true, N0, false);
            return ZMK_EV_EVENT_HANDLED;
        case MINUS:
            proc_regist_keycode(ev, INT1, true, MINUS, false);
            return ZMK_EV_EVENT_HANDLED;
        case EQUAL:
            proc_regist_keycode(ev, SEMICOLON, true, MINUS, true);
            return ZMK_EV_EVENT_HANDLED;
        case LBRC:
            proc_regist_keycode(ev, RBRC, true, RBRC, false);
            return ZMK_EV_EVENT_HANDLED;
        case RBRC:
            proc_regist_keycode(ev, NON_US_HASH, true, NON_US_HASH, false);
            return ZMK_EV_EVENT_HANDLED;
        case BSLH:
            proc_regist_keycode(ev, INT3, true, INT1, false);
            return ZMK_EV_EVENT_HANDLED;
        case SEMICOLON:
            proc_regist_keycode(ev, QUOT, false, SEMICOLON, false);
            return ZMK_EV_EVENT_HANDLED;
        case QUOT:
            proc_regist_keycode(ev, N2, true, N7, true);
            return ZMK_EV_EVENT_HANDLED;
        case GRAVE:
            proc_regist_keycode(ev, EQUAL, true, LBRC, true);
            return ZMK_EV_EVENT_HANDLED;
        case TILD:
            proc_regist_keycode(ev, EQUAL, true, EQUAL, true);
            return ZMK_EV_EVENT_HANDLED;
        case AT:
            proc_regist_keycode(ev, LBRC, false, LBRC, false);
            return ZMK_EV_EVENT_HANDLED;
        case CARET:
            proc_regist_keycode(ev, EQUAL, false, EQUAL, false);
            return ZMK_EV_EVENT_HANDLED;
        case AMPERSAND:
            proc_regist_keycode(ev, N6, true, N6, true);
            return ZMK_EV_EVENT_HANDLED;
        case ASTERISK:
            proc_regist_keycode(ev, QUOT, true, QUOT, true);
            return ZMK_EV_EVENT_HANDLED;
        case LPAR:
            proc_regist_keycode(ev, N8, true, N8, true);
            return ZMK_EV_EVENT_HANDLED;
        case RPAR:
            proc_regist_keycode(ev, N9, true, N9, true);
            return ZMK_EV_EVENT_HANDLED;
        case UNDER:
            proc_regist_keycode(ev, INT1, true, INT1, true);
            return ZMK_EV_EVENT_HANDLED;
        case PLUS:
            proc_regist_keycode(ev, SEMICOLON, true, SEMICOLON, true);
            return ZMK_EV_EVENT_HANDLED;
        case LBKT:
            proc_regist_keycode(ev, RBRC, true, RBRC, true);
            return ZMK_EV_EVENT_HANDLED;
        case RBKT:
            proc_regist_keycode(ev, NON_US_HASH, true, NON_US_HASH, true);
            return ZMK_EV_EVENT_HANDLED;
        case PIPE:
            proc_regist_keycode(ev, INT3, true, INT3, true);
            return ZMK_EV_EVENT_HANDLED;
        case COLON:
            proc_regist_keycode(ev, QUOT, false, QUOT, false);
            return ZMK_EV_EVENT_HANDLED;
        case COMMA:
            proc_regist_keycode(ev, COMMA, false, COMMA, false);
            return ZMK_EV_EVENT_HANDLED;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(us_printed_on_jis_listener, us_printed_on_jis_keycode_listener);
ZMK_SUBSCRIPTION(us_printed_on_jis_listener, zmk_keycode_state_changed);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */
