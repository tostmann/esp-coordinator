#include "web_info.h"
#include "wifi_coex.h"
#include "coordid.h"        // g_network_lost (Feature 1)
#include "coord_health.h"   // live identity cache (Feature 2)
#include "version.h"        // FW_VERSION_STRING

#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_system.h>     // esp_reset_reason

#include <cstdio>
#include <cstring>
#include <cstdlib>

static const char *TAG = "WEBINFO";

// busware logo, embedded via EMBED_FILES "busware.png" in main/CMakeLists.txt.
extern const uint8_t busware_png_start[] asm("_binary_busware_png_start");
extern const uint8_t busware_png_end[]   asm("_binary_busware_png_end");

// Single static page. Self-contained. `@HOST@` is substituted server-side with
// this device's real IP (so the address + the Z2M example are concrete, no JS).
static const char PAGE[] = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP-Coordinator — connected</title>
<style>
 body{font-family:system-ui,-apple-system,sans-serif;background:#f8fafc;color:#1e293b;margin:0;padding:24px;line-height:1.55}
 .card{max-width:680px;margin:0 auto;background:#fff;border-radius:10px;box-shadow:0 4px 10px -2px rgb(0 0 0/.12);padding:26px}
 .hd{display:flex;align-items:center;gap:16px;border-bottom:1px solid #e2e8f0;padding-bottom:16px;margin-bottom:6px}
 .hd img{height:46px;width:auto}
 h1{font-size:22px;margin:0}.ok{color:#16a34a}
 h2{font-size:16px;color:#475569;margin-top:26px}
 code{background:#e2e8f0;padding:1px 5px;border-radius:3px;font-size:13px}
 pre{background:#1e293b;color:#f8fafc;padding:14px;border-radius:6px;font-size:13px;overflow-x:auto}
 .addr{font-size:18px;font-weight:700;color:#2563eb;font-family:ui-monospace,monospace;word-break:break-all}
 .exp{background:#fffbeb;border-left:5px solid #d97706;border-radius:6px;padding:12px 16px;font-size:13px;color:#78350f;margin:18px 0}
 .note{background:#f0f9ff;border-left:4px solid #0ea5e9;border-radius:4px;padding:10px 14px;font-size:13px;margin-top:14px}
 .warn{background:#fef2f2;border-left:4px solid #ef4444;border-radius:4px;padding:10px 14px;font-size:13px;color:#991b1b;margin-top:14px}
 a{color:#2563eb}.small{font-size:12px;color:#94a3b8;margin-top:22px}
 .st{border-collapse:collapse;margin:6px 0 4px;font-size:14px}
 .st td{padding:4px 16px 4px 0;border-bottom:1px solid #eef2f6}
 .st td:first-child{color:#64748b;white-space:nowrap}
</style></head><body><div class="card">
<div class="hd"><img src="busware.png" alt="busware">
<div><h1><span class="ok">&#10003;</span> Coordinator connected</h1>
<div style="color:#64748b;font-size:14px">Your ESP-Coordinator (ZBOSS NCP) is on your WiFi. &middot; <a href="https://github.com/tostmann/esp-coordinator" target="_blank">GitHub</a></div></div></div>

@STATUS@

<div class="exp"><strong>Experimental WiFi-Coex build.</strong> The single C6 radio is time-shared between WiFi and Zigbee; coexistence quality depends on your RF environment. Use a spare stick / test network and please report back (link below).</div>

<h2>1. This coordinator's address</h2>
<p>Point Zigbee2MQTT here:</p>
<p class="addr">tcp://@HOST@:6638</p>
<div class="note">Tip: set a <b>static DHCP lease</b> for this device so the address stays stable. mDNS name (if your network resolves it): <code>esp-zboss-coord.local:6638</code>.</div>

<h2>2. Zigbee2MQTT configuration</h2>
<p>In <code>zigbee2mqtt/data/configuration.yaml</code>:</p>
<pre>serial:
  port: tcp://@HOST@:6638
  adapter: zboss

advanced:
  transmit_power: 20</pre>
<div class="note"><b>Required Docker image for this build:</b>
<pre style="margin:6px 0 0">image: ghcr.io/tostmann/zigbee2mqtt-esp32:latest</pre>
It carries the longer timeouts the single-radio TCP link needs; the stock image can drop the link under load.</div>
<div class="warn"><b>Backup/restore over TCP is degraded</b> (raw pull can stall &rarr; structured-backup fallback). For a full backup, flash the stable USB build temporarily.</div>

<h2>Change WiFi later</h2>
<p>No reflash: power-cycle the stick and, within the first <b>120&nbsp;s</b>, reconnect it in the web flasher and choose &ldquo;Change&nbsp;Wi-Fi&rdquo;.</p>

<div style="margin-top:22px;background:#ecfdf5;border-left:5px solid #22c55e;border-radius:6px;padding:14px 16px;font-size:14px">
<b>Please report how it went</b> on <a href="https://github.com/tostmann/esp-coordinator/discussions/1" target="_blank">Discussion&nbsp;#1</a>: your AP/environment, Zigbee + WiFi channels, device count, and stability.</div>

<div class="small"><a href="https://github.com/tostmann/esp-coordinator" target="_blank">GitHub</a> &middot; <a href="https://paypal.me/busware" target="_blank">Support &#9749;</a> &middot; experimental wifi-coex build</div>
</div></body></html>)HTML";

static const char HOST_TOKEN[]   = "@HOST@";
static const char STATUS_TOKEN[] = "@STATUS@";

static void send_all(int cs, const char *p, int len) {
    int off = 0;
    while (off < len) {
        int n = ::send(cs, p + off, len - off, 0);
        if (n <= 0) return;
        off += n;
    }
}

// Render the live coordinator-status block (Feature 2). READ-ONLY: the
// g_coord_health cache (published from the ZBOSS task), g_network_lost
// (Feature 1), esp_reset_reason(), FW_VERSION_STRING, and the WiFi RSSI getter.
// CRITICAL: NO zb_* calls here — this runs on the httpd task and a zb_* call
// would reintroduce the v1.3.49 cross-task big-lock vPortExitCritical panic.
static void build_status_html(char *o, size_t n) {
    const struct coord_health &h = g_coord_health;
    int w = 0;
#define WREM ((w < (int)n) ? (size_t)((int)n - w) : (size_t)0)
    if (g_network_lost) {
        w += snprintf(o + w, WREM,
            "<div class=\"warn\" style=\"border-left-width:6px;font-size:14px\">"
            "<b>&#9888; NETWORK LOST &mdash; your Zigbee network is gone.</b> "
            "The coordinator booted factory-blank: its NVRAM was wiped (an "
            "interrupted write) or a corrupt backup was restored. Last known "
            "network: PAN 0x%04X, channel %u. Recover by letting Zigbee2MQTT "
            "form a fresh network and re-pairing your devices, or by restoring "
            "a known-good backup.</div>",
            (unsigned)h.lost_pan, (unsigned)h.lost_channel);
    }

    const char *joined_s = !h.valid ? "(starting&hellip;)" : (h.joined ? "yes" : "no");
    char panbuf[12], chanbuf[12], extbuf[28];
    if (h.valid && h.joined) {
        snprintf(panbuf, sizeof panbuf, "0x%04X", (unsigned)h.pan);
        snprintf(chanbuf, sizeof chanbuf, "%u", (unsigned)h.channel);
        uint8_t e[8];
        if (coord_health_read_extpan(e))
            snprintf(extbuf, sizeof extbuf,
                     "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
                     e[7], e[6], e[5], e[4], e[3], e[2], e[1], e[0]);  // MSB-first display
        else
            strcpy(extbuf, "&mdash;");
    } else {
        strcpy(panbuf, "&mdash;");
        strcpy(chanbuf, "&mdash;");
        strcpy(extbuf, "&mdash;");
    }

    w += snprintf(o + w, WREM,
        "<h2>Coordinator status</h2><table class=\"st\">"
        "<tr><td>Joined</td><td>%s</td></tr>"
        "<tr><td>PAN ID</td><td>%s</td></tr>"
        "<tr><td>Channel</td><td>%s</td></tr>"
        "<tr><td>Ext&nbsp;PAN / IEEE</td><td><code>%s</code></td></tr>"
        "<tr><td>Firmware</td><td>%s</td></tr>"
        "<tr><td>Reset&nbsp;reason</td><td>%d</td></tr>",
        joined_s, panbuf, chanbuf, extbuf, FW_VERSION_STRING, (int)esp_reset_reason());

    int8_t rssi;
    if (wifi_coex_current_rssi(&rssi))
        w += snprintf(o + w, WREM,
            "<tr><td>WiFi&nbsp;RSSI</td><td>%d&nbsp;dBm</td></tr>", (int)rssi);
    w += snprintf(o + w, WREM, "</table>");
#undef WREM
}

// Serve the HTML page with @HOST@ -> device IP and @STATUS@ -> live status block.
// The status block is variable-length (banner present/absent, joined/blank,
// RSSI present/absent), so the body is assembled into ONE heap buffer and the
// Content-Length is its exact strlen (an over-count hangs the browser, an
// under-count truncates). Heap, not stack: the webinfo task stack is only 3584 B.
static void send_html(int cs) {
    char ip[20] = {0};
    wifi_coex_current_ip(ip, sizeof(ip));
    if (!ip[0]) strncpy(ip, "this-device", sizeof(ip) - 1);
    const int iplen = (int)strlen(ip);

    // static (BSS), NOT on the stack: the webinfo task stack is small and a 1.5 KB
    // stack buffer here (on top of serve_client's req[700]) overflows it. The task
    // serves one client at a time (synchronous accept loop), so a static scratch
    // buffer is reentrancy-safe.
    static char status[1536];
    build_status_html(status, sizeof(status));
    const int statuslen = (int)strlen(status);

    const int hosttok = (int)(sizeof(HOST_TOKEN) - 1);
    const int stattok = (int)(sizeof(STATUS_TOKEN) - 1);

    const size_t cap = sizeof(PAGE) + (size_t)statuslen + 64;
    char *body = (char *)malloc(cap);
    if (!body) {
        static const char busy[] =
            "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n"
            "Content-Length: 11\r\n\r\nbusy, retry";
        send_all(cs, busy, (int)(sizeof(busy) - 1));
        return;
    }
    size_t bl = 0;
    for (const char *p = PAGE; *p && bl + 1 < cap; ) {
        if (!strncmp(p, HOST_TOKEN, hosttok)) {
            for (int i = 0; i < iplen && bl + 1 < cap; ++i) body[bl++] = ip[i];
            p += hosttok;
        } else if (!strncmp(p, STATUS_TOKEN, stattok)) {
            for (int i = 0; i < statuslen && bl + 1 < cap; ++i) body[bl++] = status[i];
            p += stattok;
        } else {
            body[bl++] = *p++;
        }
    }
    body[bl] = 0;

    char hdr[160];
    int hh = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %u\r\nConnection: close\r\n\r\n", (unsigned)bl);
    send_all(cs, hdr, hh);
    send_all(cs, body, (int)bl);
    free(body);
}

static void send_png(int cs) {
    const size_t len = (size_t)(busware_png_end - busware_png_start);
    char hdr[160];
    int h = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n"
        "Content-Length: %u\r\nCache-Control: max-age=86400\r\nConnection: close\r\n\r\n",
        (unsigned)len);
    send_all(cs, hdr, h);
    send_all(cs, reinterpret_cast<const char *>(busware_png_start), (int)len);
}

static void serve_client(int cs) {
    // Read the request until end-of-headers (need the request line for routing).
    // CRITICAL: a browser sends >256 B of headers; closing a socket with unread
    // RX data makes the stack send an RST, which discards the in-flight response
    // tail (truncates the page in the browser — curl's tiny request didn't hit
    // it). So consume the request, and below do a lingering close.
    char req[700];
    int total = 0;
    while (total < (int)sizeof(req) - 1) {
        int n = ::recv(cs, req + total, sizeof(req) - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        req[total] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }

    if (strstr(req, "busware.png")) send_png(cs);
    else                            send_html(cs);

    // Lingering close: FIN after the queued response, then drain anything the
    // client still sends until it closes (recv==0) or a short timeout — so the
    // final close never turns into an RST that truncates the response.
    ::shutdown(cs, SHUT_WR);
    struct timeval to = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
    char drain[256];
    while (::recv(cs, drain, sizeof(drain), 0) > 0) { /* discard */ }
    ::close(cs);
}

static void web_info_task(void *) {
    while (!wifi_coex_is_up()) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    int ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls < 0) { ESP_LOGE(TAG, "socket failed"); vTaskDelete(NULL); return; }
    int opt = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(80);
    if (::bind(ls, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0 ||
        ::listen(ls, 2) != 0) {
        ESP_LOGE(TAG, "bind/listen :80 failed (errno %d)", errno);
        ::close(ls);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "info page on http://<ip>/");

    while (true) {
        int cs = ::accept(ls, nullptr, nullptr);
        if (cs < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        int one = 1;
        setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        struct timeval to = {.tv_sec = 3, .tv_usec = 0};
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
        setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
        serve_client(cs);
    }
}

extern "C" void web_info_start(void) {
    static bool started = false;
    if (started) return;
    started = true;
    if (xTaskCreate(web_info_task, "webinfo", 4608, nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "webinfo task create failed");
        started = false;
    }
}
