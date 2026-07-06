// SPDX-License-Identifier: GPL-2.0-or-later
//
// zmk-input-inertia (amgskobo, 2026-02-15以降) は zmk_endpoint_send_mouse_report
// (単数形) を呼ぶが、ZMK v0.3 は zmk_endpoints_send_mouse_report (複数形) のみ提供する。
// アップストリームのAPI名変更前後の橋渡し。

#include <zephyr/sys/util_macro.h>

#if IS_ENABLED(CONFIG_ZMK_POINTING)

#include <zmk/endpoints.h>

int zmk_endpoint_send_mouse_report(void) { return zmk_endpoints_send_mouse_report(); }

#endif /* CONFIG_ZMK_POINTING */
