#pragma once
//
// web_info — a tiny on-device HTTP info page for the wifi-coex Mode B testground.
// After the device joins WiFi, the Improv "Visit Device" URL (http://<ip>/) lands
// here: a single static page that tells the user exactly how to add this
// coordinator to Zigbee2MQTT (the tcp://<ip>:6638 address, the zboss adapter, the
// required Docker image, and how to re-configure WiFi). The page is self-contained
// (no external assets) and fills its own address client-side from location.hostname,
// so it works the same whether reached by IP or mDNS name.
//
// Raw-socket server (no esp_http_server dependency), one tiny task. Started only in
// NORMAL Mode B (app::start_int) — it waits for wifi_coex_is_up() then binds :80.
//
#ifdef __cplusplus
extern "C" {
#endif

void web_info_start(void);  // idempotent; starts the :80 info server task once

#ifdef __cplusplus
}
#endif
