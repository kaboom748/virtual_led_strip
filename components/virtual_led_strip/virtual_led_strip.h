#pragma once

#include "esphome/core/component.h"
#include "esphome/components/light/addressable_light.h"
#include "esphome/components/socket/socket.h"

#include <memory>
#include <vector>

namespace esphome {
namespace virtual_led_strip {

/// An addressable light with no pin. The browser is the strip.
///
/// Same shape as ESPHome's real drivers -- AddressableLight is LightOutput +
/// Component -- so effects, transitions, Home Assistant and every automation
/// work unchanged, and swapping to neopixelbus or esp32_rmt_led_strip touches
/// only the platform line.
///
/// One socket path for both targets: ESPHome's socket component covers ESP8266
/// through its lwip_tcp implementation. Beware that on lwip_tcp, ListenSocket
/// and Socket are distinct types -- a plain socket_ip() cannot listen() there.
class VirtualLedStrip final : public light::AddressableLight {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  /// HARDWARE, comme neopixelbus et esp32_rmt_led_strip -- et pour la meme
  /// raison: LightState est a HARDWARE - 1.0f, il s'initialise donc APRES nous
  /// et appelle setup_state(), init_internal() sur chaque effet, puis restaure
  /// l'etat depuis la flash avec un call.perform(). A AFTER_WIFI (200) notre
  /// tampon etait encore vide 599 niveaux plus tot: pointeur dans le neant,
  /// plantage, boot loop, safe mode. Invisible sur le banc host: il n'a pas de
  /// flash a relire, donc rien a restaurer.
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  void write_state(light::LightState *state) override;
  int32_t size() const override { return this->num_leds_; }
  void clear_effect_data() override;
  light::LightTraits get_traits() override {
    auto traits = light::LightTraits();
    if (this->is_rgbw_) {
      traits.set_supported_color_modes({light::ColorMode::RGB_WHITE, light::ColorMode::WHITE});
    } else {
      traits.set_supported_color_modes({light::ColorMode::RGB});
    }
    return traits;
  }

  void set_num_leds(uint16_t num_leds) { this->num_leds_ = num_leds; }
  void set_port(uint16_t port) { this->port_ = port; }
  void set_rgbw(bool rgbw) { this->is_rgbw_ = rgbw; }
  void set_max_refresh_rate(uint32_t interval_us) { this->max_refresh_us_ = interval_us; }

 protected:
  light::ESPColorView get_view_internal(int32_t index) const override;

  void start_server_();
  void accept_client_();
  void read_request_();
  void handle_request_();
  void pump_html_();
  void start_stream_();
  void drop_stream_(const char *why);
  void promote_();
  bool flush_();
  void heartbeat_(uint32_t now);
  void encode_(uint32_t ts);

  uint16_t num_leds_{0};
  uint16_t port_{8083};
  bool is_rgbw_{false};
  uint8_t bpp_{3};
  uint32_t max_refresh_us_{0};
  uint32_t last_refresh_{0};

  /// The live buffer. ESPColorView hands out pointers into it, so the colour
  /// correction -- brightness scaling then powf(v, gamma_correct) -- is applied
  /// on write. What sits here is the PWM duty the LED would carry, not the
  /// colour the effect asked for. The browser must undo neither: it converts
  /// duty (linear light) to sRGB, which is a different curve entirely.
  std::vector<uint8_t> buf_;
  /// Base of the delta encoding. Only advances on a frame actually committed to
  /// the wire, never on one superseded before being sent -- otherwise the client
  /// would rebuild deltas against a frame it never saw.
  std::vector<uint8_t> prev_;
  std::vector<uint8_t> effect_;

  /// The frame being written, possibly across several loop passes.
  std::vector<uint8_t> out_;
  size_t out_pos_{0};

  bool dirty_{false};
  uint32_t dirty_ts_{0};
  uint16_t seq_{0};
  uint32_t dropped_{0};
  uint32_t last_beat_{0};

  std::unique_ptr<socket::ListenSocket> server_;
  std::unique_ptr<socket::Socket> pending_client_;
  std::unique_ptr<socket::Socket> stream_client_;

  // Chrome envoie ~450 octets d'en-tetes. A 160, le buffer se remplissait avant
  // le CRLF CRLF final: la requete n'etait jamais reconnue complete, handle_
  // request_ jamais appele, et le client largue comme "idle" au bout de 2 s --
  // page blanche. Invisible sur ESP8266, dont la requete transferee tombait
  // sous 160. La boucle de lecture continue de scruter la fin des en-tetes meme
  // buffer plein, donc 512 est une marge, pas une limite dure.
  // 512 est une marge: on n'a besoin que de la premiere ligne (le chemin), et la
  // boucle de lecture continue de scruter la fin des en-tetes meme buffer plein.
  char request_[512]{};
  size_t request_len_{0};
  uint8_t nl_{0};
  uint32_t pending_since_{0};
  bool html_sending_{false};
  bool server_started_{false};
  size_t html_pos_{0};
};

}  // namespace virtual_led_strip
}  // namespace esphome
