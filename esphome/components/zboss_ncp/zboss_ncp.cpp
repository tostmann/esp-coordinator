#include "zboss_ncp.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

#include <driver/gpio.h>
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
// Set by the NCP task if zboss_ncp_main() returns (the NCP failed to start).
static std::atomic<bool> s_ncp_stopped{false};
// Set by the NCP (NCP_RESET, restore, boot guard) to reboot the device.
static std::atomic<bool> s_restart_requested{false};
static ZbossNcpComponent *s_instance = nullptr;

// Upstream ran app_main() on the 8 KB main task (priority 1); same here. The
// ZBOSS task runs at 5 and must stay above this one.
static constexpr uint32_t NCP_TASK_STACK = 8192;
static constexpr UBaseType_t NCP_TASK_PRIORITY = 1;
static constexpr uint32_t RADIO_READY_TIMEOUT_MS = 5000;
// External antenna chosen but no WiFi connection within this time: back to
// the on-board antenna (an empty U.FL connector would cut off OTA and API).
static constexpr uint32_t ANTENNA_FALLBACK_MS = 120000;
// XIAO ESP32-C6 RF switch (not on the header): GPIO3 low enables the switch
// control, GPIO14 selects the antenna (high = U.FL). See main/main.cpp.
static constexpr gpio_num_t ANT_ENABLE_PIN = GPIO_NUM_3;
static constexpr gpio_num_t ANT_SELECT_PIN = GPIO_NUM_14;

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
  s_ncp_stopped.store(true, std::memory_order_relaxed);
  vTaskDelete(nullptr);
}

void ZbossNcpComponent::setup() {
  s_instance = this;
  s_network_up.store(network::is_connected(), std::memory_order_relaxed);

  ncp_host_hooks_t hooks{};
  hooks.network_up = &ZbossNcpComponent::network_up_hook_;
  hooks.peer_allowed = &ZbossNcpComponent::peer_allowed_hook_;
  hooks.use_wifi = this->use_wifi_;
  hooks.restart = []() { s_restart_requested.store(true, std::memory_order_relaxed); };
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

  if (this->antenna_select_ != nullptr) {
    // Until here GPIO3/14 are in their reset state (inputs, the XIAO's
    // pulldown selects the on-board antenna); the saved choice applies now.
    this->antenna_pref_ = global_preferences->make_preference<uint8_t>(fnv1_hash("zboss_ncp_antenna"));
    uint8_t saved = 0;
    this->antenna_pref_.load(&saved);
    this->set_external_antenna(saved == 1, false);
  }
}

void ZbossNcpComponent::apply_antenna_(bool external) {
  gpio_config_t io{};
  io.pin_bit_mask = (1ULL << ANT_ENABLE_PIN) | (1ULL << ANT_SELECT_PIN);
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  if (!external) {
    // As the firmware's default: inputs, the board's pulldown on GPIO14
    // selects the on-board antenna.
    this->cancel_timeout("antenna");
    io.mode = GPIO_MODE_INPUT;
    gpio_config(&io);
    return;
  }
  io.mode = GPIO_MODE_OUTPUT;
  gpio_set_level(ANT_SELECT_PIN, 0);
  gpio_config(&io);
  gpio_set_level(ANT_ENABLE_PIN, 0);  // enable the switch control
  // Seeed's sequence: 100 ms settle, then select the U.FL antenna.
  this->set_timeout("antenna", 100, []() { gpio_set_level(ANT_SELECT_PIN, 1); });
}

void ZbossNcpComponent::set_external_antenna(bool external, bool save) {
  const bool changed = external != this->external_antenna_;
  this->external_antenna_ = external;
  this->apply_antenna_(external);
  this->antenna_unconfirmed_ = external && this->use_wifi_;
  this->antenna_changed_at_ = millis();
  if (save) {
    uint8_t v = external ? 1 : 0;
    this->antenna_pref_.save(&v);
    global_preferences->sync();
  }
  if (changed || !save)
    ESP_LOGI(TAG, "Antenna: %s", external ? "external (U.FL)" : "on-board");
  if (this->antenna_select_ != nullptr)
    this->antenna_select_->publish_state(external ? 1 : 0);
}

void ZbossNcpComponent::loop() {
  if (s_restart_requested.load(std::memory_order_relaxed)) {
    // From this component's loop, so the debug component stores "zboss_ncp"
    // as the reboot source.
    ESP_LOGI(TAG, "NCP requested a reboot");
    App.safe_reboot();
  }
  if (s_ncp_stopped.load(std::memory_order_relaxed) && !this->status_has_error()) {
    // Shown as the component's error status (and in Home Assistant), not only
    // in the log. OTA and the API keep working, so it can be fixed remotely.
    this->status_set_error(LOG_STR("NCP stopped, see the log"));
  }
  const bool up = network::is_connected();
  s_network_up.store(up, std::memory_order_relaxed);
  if (up && this->use_wifi_ && !this->coex_reapplied_) {
    ncp_host_reapply_coex();
    this->coex_reapplied_ = true;
  }
  if (this->antenna_unconfirmed_) {
    if (up) {
      this->antenna_unconfirmed_ = false;
    } else if (millis() - this->antenna_changed_at_ > ANTENNA_FALLBACK_MS) {
      ESP_LOGW(TAG, "No WiFi connection on the external antenna for %" PRIu32 " s: back to the on-board antenna",
               ANTENNA_FALLBACK_MS / 1000);
      this->set_external_antenna(false, true);
    }
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
  if (this->antenna_select_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Antenna: %s (selectable)", this->external_antenna_ ? "external (U.FL)" : "on-board");
  }
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
