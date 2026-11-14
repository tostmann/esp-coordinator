#include "zboss_ncp.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/def.h>

#include <atomic>
#include <cinttypes>
#include <cstdio>

#include "zboss_ncp_api.h"

namespace esphome::zboss_ncp {

static const char *const TAG = "zboss_ncp";

// The NCP's tasks ask through these; they must not touch ESPHome objects.
static std::atomic<bool> s_network_up{false};
static ZbossNcpComponent *s_instance = nullptr;

// Upstream ran app_main() on the 8 KB main task (priority 1); same here. The
// ZBOSS task runs at 5 and must stay above this one.
static constexpr uint32_t NCP_TASK_STACK = 8192;
static constexpr UBaseType_t NCP_TASK_PRIORITY = 1;
static constexpr uint32_t RADIO_READY_TIMEOUT_MS = 5000;

bool ZbossNcpComponent::network_up_hook_() { return s_network_up.load(std::memory_order_relaxed); }

bool ZbossNcpComponent::peer_allowed_hook_(uint32_t ipv4_be) {
  // allowed_ is filled before setup() and never changes afterwards.
  if (s_instance == nullptr || s_instance->allowed_.empty())
    return true;
  const uint32_t ip = lwip_ntohl(ipv4_be);
  for (const auto &net : s_instance->allowed_) {
    if ((ip & net.netmask) == (net.network & net.netmask))
      return true;
  }
  return false;
}

void ZbossNcpComponent::task_(void * /*arg*/) {
  zboss_ncp_main();  // returns only when the NCP failed to start
  vTaskDelete(nullptr);
}

void ZbossNcpComponent::setup() {
  s_instance = this;
  s_network_up.store(network::is_connected(), std::memory_order_relaxed);

  ncp_host_hooks_t hooks{};
  hooks.network_up = &ZbossNcpComponent::network_up_hook_;
  hooks.peer_allowed = &ZbossNcpComponent::peer_allowed_hook_;
  hooks.use_wifi = this->use_wifi_;
  ncp_host_set_hooks(&hooks);

  ESP_LOGI(TAG, "setup: starting NCP task at %" PRIu32 " ms", millis());
  if (xTaskCreate(&ZbossNcpComponent::task_, "zboss_ncp", NCP_TASK_STACK, nullptr, NCP_TASK_PRIORITY, nullptr) !=
      pdPASS) {
    ESP_LOGE(TAG, "Could not start the NCP task");
    this->mark_failed();
    return;
  }

  // Upstream's order: Zigbee stack up and coexistence on, then WiFi starts.
  // This component sets up before WiFi (priority BUS), so holding setup()
  // here keeps ESPHome from starting WiFi while the 802.15.4 radio
  // initialises (that race hangs in wifi_hw_start). Normally ~50 ms.
  const uint32_t start = millis();
  while (!zboss_ncp_radio_ready() && millis() - start < RADIO_READY_TIMEOUT_MS) {
    App.feed_wdt();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (zboss_ncp_radio_ready()) {
    ESP_LOGI(TAG, "Zigbee radio ready after %" PRIu32 " ms (at %" PRIu32 " ms)", millis() - start, millis());
  } else {
    ESP_LOGW(TAG, "Zigbee stack not up after %" PRIu32 " ms; continuing", millis() - start);
  }
  if (this->version_sensor_ != nullptr)
    this->version_sensor_->publish_state(zboss_ncp_version());
}

void ZbossNcpComponent::loop() {
  const bool up = network::is_connected();
  s_network_up.store(up, std::memory_order_relaxed);
  if (up && this->use_wifi_ && !this->coex_reapplied_) {
    ncp_host_reapply_coex();
    this->coex_reapplied_ = true;
  }
}

void ZbossNcpComponent::update() {
  ncp_status_t st;
  ncp_get_status(&st);

  if (this->host_connected_sensor_ != nullptr)
    this->host_connected_sensor_->publish_state(st.host_connected);
  if (this->network_lost_sensor_ != nullptr)
    this->network_lost_sensor_->publish_state(st.network_lost);
  if (this->safe_mode_sensor_ != nullptr)
    this->safe_mode_sensor_->publish_state(st.safe_mode);
  if (!st.valid || !st.joined)
    return;
  if (this->channel_sensor_ != nullptr && st.channel != 0xFF)
    this->channel_sensor_->publish_state(st.channel);
  if (this->pan_id_sensor_ != nullptr) {
    char buf[7];
    snprintf(buf, sizeof(buf), "0x%04X", st.pan_id);
    this->pan_id_sensor_->publish_state(buf);
  }
  if (this->ext_pan_id_sensor_ != nullptr) {
    // ZBOSS keeps it little-endian; show it as Z2M and ZHA do (big-endian).
    char buf[24];
    char *p = buf;
    for (int i = 7; i >= 0; i--)
      p += snprintf(p, 4, i ? "%02x:" : "%02x", st.ext_pan_id[i]);
    this->ext_pan_id_sensor_->publish_state(buf);
  }
}

void ZbossNcpComponent::dump_config() {
  ESP_LOGCONFIG(TAG,
                "ZBOSS NCP coordinator:\n"
                "  Firmware: %s\n"
                "  TCP port: %d\n"
                "  WiFi coexistence: %s",
                zboss_ncp_version(), CONFIG_NCP_TCP_PORT, YESNO(this->use_wifi_));
  if (this->allowed_.empty()) {
    ESP_LOGCONFIG(TAG, "  Allowed hosts: any");
  }
  for (const auto &net : this->allowed_) {
    ESP_LOGCONFIG(TAG, "  Allowed: %u.%u.%u.%u/%u.%u.%u.%u", (unsigned) (net.network >> 24) & 0xFF,
                  (unsigned) (net.network >> 16) & 0xFF, (unsigned) (net.network >> 8) & 0xFF,
                  (unsigned) net.network & 0xFF, (unsigned) (net.netmask >> 24) & 0xFF,
                  (unsigned) (net.netmask >> 16) & 0xFF, (unsigned) (net.netmask >> 8) & 0xFF,
                  (unsigned) net.netmask & 0xFF);
  }
}

}  // namespace esphome::zboss_ncp
