#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace esphome::zboss_ncp {

class ZbossNcpComponent : public PollingComponent {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  // Before WiFi: the Zigbee stack starts at once, the TCP server waits for
  // the network itself.
  float get_setup_priority() const override { return setup_priority::BUS; }

  void set_use_wifi(bool use_wifi) { this->use_wifi_ = use_wifi; }
  // Both in host byte order (as Python's ipaddress gives them).
  void add_allowed_network(uint32_t network, uint32_t netmask) {
    this->allowed_.push_back({network, netmask});
  }

  void set_channel_sensor(sensor::Sensor *s) { this->channel_sensor_ = s; }
  void set_pan_id_sensor(text_sensor::TextSensor *s) { this->pan_id_sensor_ = s; }
  void set_ext_pan_id_sensor(text_sensor::TextSensor *s) { this->ext_pan_id_sensor_ = s; }
  void set_version_sensor(text_sensor::TextSensor *s) { this->version_sensor_ = s; }
  void set_host_connected_sensor(binary_sensor::BinarySensor *s) { this->host_connected_sensor_ = s; }
  void set_network_lost_sensor(binary_sensor::BinarySensor *s) { this->network_lost_sensor_ = s; }
  void set_safe_mode_sensor(binary_sensor::BinarySensor *s) { this->safe_mode_sensor_ = s; }

 protected:
  struct AllowedNet {
    uint32_t network;
    uint32_t netmask;
  };
  static bool network_up_hook_();
  static bool peer_allowed_hook_(uint32_t ipv4_be);
  static void task_(void *arg);

  bool use_wifi_{false};
  bool coex_reapplied_{false};
  std::vector<AllowedNet> allowed_;

  sensor::Sensor *channel_sensor_{nullptr};
  text_sensor::TextSensor *pan_id_sensor_{nullptr};
  text_sensor::TextSensor *ext_pan_id_sensor_{nullptr};
  text_sensor::TextSensor *version_sensor_{nullptr};
  binary_sensor::BinarySensor *host_connected_sensor_{nullptr};
  binary_sensor::BinarySensor *network_lost_sensor_{nullptr};
  binary_sensor::BinarySensor *safe_mode_sensor_{nullptr};
};

}  // namespace esphome::zboss_ncp
