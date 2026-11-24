/*
 * Based on ZMK's input_processor_temp_layer.c (MIT, The ZMK Contributors).
 *
 * Differences from upstream:
 *  - While an excluded position (e.g. mouse button) is held, the layer never times out.
 *  - Releasing an excluded position restarts the timeout.
 */

#define DT_DRV_COMPAT zmk_input_processor_temp_layer_keep

#include <zephyr/devicetree.h>

#include <zephyr/sys/util.h>

/* The keymap only exists on the central (or non-split) side */
#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) &&                                                     \
    (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/input_processor.h>
#include <zmk/keymap.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1,
             "Only one zmk,input-processor-temp-layer-keep instance is supported");

static const uint16_t excluded_positions[] = DT_INST_PROP(0, excluded_positions);
#define NUM_EXCLUDED ARRAY_SIZE(excluded_positions)
BUILD_ASSERT(NUM_EXCLUDED <= 32, "Up to 32 excluded positions are supported");

#define REQUIRE_PRIOR_IDLE_MS DT_INST_PROP(0, require_prior_idle_ms)

static K_MUTEX_DEFINE(lock);

/* Protected by lock. layer is a layer ID (same as &mo's param). */
static struct {
    uint8_t layer;
    uint32_t timeout_ms;
    bool is_active;
    uint32_t held_mask; /* bit i = excluded_positions[i] pressed while layer active */
    int64_t last_tapped_timestamp;
} st;

static void activate_work_cb(struct k_work *work);
static void deactivate_work_cb(struct k_work *work);
static K_WORK_DEFINE(activate_work, activate_work_cb);
static K_WORK_DELAYABLE_DEFINE(deactivate_work, deactivate_work_cb);

static int excluded_index(uint32_t position) {
    for (size_t i = 0; i < NUM_EXCLUDED; i++) {
        if (excluded_positions[i] == position) {
            return i;
        }
    }
    return -1;
}

/* Must be called with lock held */
static void set_active(bool activate) {
    if (st.is_active == activate) {
        return;
    }
    st.is_active = activate;
    if (activate) {
        zmk_keymap_layer_activate(st.layer);
        LOG_DBG("temp-layer-keep: layer %d activated", st.layer);
    } else {
        st.held_mask = 0;
        k_work_cancel_delayable(&deactivate_work);
        zmk_keymap_layer_deactivate(st.layer);
        LOG_DBG("temp-layer-keep: layer %d deactivated", st.layer);
    }
}

static void activate_work_cb(struct k_work *work) {
    k_mutex_lock(&lock, K_FOREVER);
    set_active(true);
    k_mutex_unlock(&lock);
}

static void deactivate_work_cb(struct k_work *work) {
    k_mutex_lock(&lock, K_FOREVER);
    if (st.held_mask == 0) {
        set_active(false);
    } else {
        LOG_DBG("temp-layer-keep: excluded key held, keeping layer");
    }
    k_mutex_unlock(&lock);
}

static int on_layer_state_changed(const zmk_event_t *eh) {
    k_mutex_lock(&lock, K_FOREVER);
    if (st.is_active && !zmk_keymap_layer_active(st.layer)) {
        /* Layer was turned off by something else */
        st.is_active = false;
        st.held_mask = 0;
        k_work_cancel_delayable(&deactivate_work);
    }
    k_mutex_unlock(&lock);
    return ZMK_EV_EVENT_BUBBLE;
}

static int on_position_state_changed(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    int idx = excluded_index(ev->position);

    k_mutex_lock(&lock, K_FOREVER);
    if (ev->state) {
        if (st.is_active) {
            if (idx < 0) {
                set_active(false);
            } else {
                st.held_mask |= BIT(idx);
            }
        } else if (idx < 0) {
            /* Typing started before a pending activation ran: drop it */
            k_work_cancel(&activate_work);
        }
    } else if (idx >= 0 && (st.held_mask & BIT(idx))) {
        st.held_mask &= ~BIT(idx);
        if (st.is_active && st.held_mask == 0 && st.timeout_ms > 0) {
            k_work_reschedule(&deactivate_work, K_MSEC(st.timeout_ms));
        }
    }
    k_mutex_unlock(&lock);
    return ZMK_EV_EVENT_BUBBLE;
}

static int on_keycode_state_changed(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev->state) {
        k_mutex_lock(&lock, K_FOREVER);
        st.last_tapped_timestamp = ev->timestamp;
        k_mutex_unlock(&lock);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int temp_layer_keep_listener(const zmk_event_t *eh) {
    if (as_zmk_layer_state_changed(eh) != NULL) {
        return on_layer_state_changed(eh);
    }
    if (as_zmk_position_state_changed(eh) != NULL) {
        return on_position_state_changed(eh);
    }
    if (as_zmk_keycode_state_changed(eh) != NULL) {
        return on_keycode_state_changed(eh);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(processor_temp_layer_keep, temp_layer_keep_listener);
ZMK_SUBSCRIPTION(processor_temp_layer_keep, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(processor_temp_layer_keep, zmk_position_state_changed);
ZMK_SUBSCRIPTION(processor_temp_layer_keep, zmk_keycode_state_changed);

static int temp_layer_keep_handle_event(const struct device *dev, struct input_event *event,
                                        uint32_t param1, uint32_t param2,
                                        struct zmk_input_processor_state *state) {
    if (param1 >= ZMK_KEYMAP_LAYERS_LEN) {
        LOG_ERR("Invalid layer: %u", param1);
        return -EINVAL;
    }

    k_mutex_lock(&lock, K_FOREVER);

    st.layer = param1;
    st.timeout_ms = param2;

    if (!st.is_active &&
        (st.last_tapped_timestamp + REQUIRE_PRIOR_IDLE_MS) <= k_uptime_get()) {
        k_work_submit(&activate_work);
    }

    if (param2 > 0) {
        /* If an excluded key is held, the callback keeps the layer; release reschedules */
        k_work_reschedule(&deactivate_work, K_MSEC(param2));
    }

    k_mutex_unlock(&lock);

    return ZMK_INPUT_PROC_CONTINUE;
}

static int temp_layer_keep_init(const struct device *dev) {
    st.last_tapped_timestamp = -REQUIRE_PRIOR_IDLE_MS;
    return 0;
}

static const struct zmk_input_processor_driver_api temp_layer_keep_driver_api = {
    .handle_event = temp_layer_keep_handle_event,
};

DEVICE_DT_INST_DEFINE(0, temp_layer_keep_init, NULL, NULL, NULL, POST_KERNEL,
                      CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &temp_layer_keep_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY */
