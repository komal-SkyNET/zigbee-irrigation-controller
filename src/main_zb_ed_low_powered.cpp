/*
  CODE-B: IrrigationController (Zigbee End Device, battery-friendly)
  Board: Seeed XIAO ESP32-C6

  What it does
  - Presents up to 8 HA "switch" endpoints (EP 11..18) using ZigbeePowerOutlet
  - Optimized for battery: End Device role + RX off when idle (library handles sleepy polling) 
  - LED patterns:
      • Blinking while joining
      • Solid when joined
      • Quick pulse on each switch state change (then back to solid)

  Build (PlatformIO) — important:
    - Define ED mode:          -D ZIGBEE_MODE_ED
    - Use ED partition scheme: board_build.partitions = partitions.csv  (ED / non-ZCZR)
    - Keep USB CDC on boot if you want logs:
        -D ARDUINO_USB_MODE=1
        -D ARDUINO_USB_CDC_ON_BOOT=1

  Notes:
    - HA/Zigbee2MQTT sends commands; as a sleepy end device they’re delivered on poll. 
      Expect latency ~= your parent-poll interval (managed by the stack).
    - For mains-powered nodes, prefer Router mode (your earlier CODE-A). 
*/

#ifndef ZIGBEE_MODE_ED
#error "Build in Zigbee END DEVICE mode: add -D ZIGBEE_MODE_ED and use an ED partition scheme."
#endif

#include <Arduino.h>
#include "Zigbee.h"                // ZigbeeCore / ZigbeeEP / ZigbeePowerOutlet (Arduino Zigbee)
#include "esp_zigbee_core.h"       // for esp_zb_cfg_t (optional custom cfg)

// ---------------- Config ----------------
constexpr uint8_t  MAX_ZONES          = 8;
constexpr uint8_t  FIRST_ENDPOINT     = 11;     // EPs 11..18
constexpr bool     RELAY_ACTIVE_LOW   = false;  // if you wire any local loads later
constexpr int      LED_PIN            = 15;     // XIAO ESP32C6 builtin LED (set -1 to disable)
constexpr uint32_t CONNECT_BLINK_MS   = 300;
constexpr uint32_t PULSE_MS           = 180;

// Battery-friendly network config (keep-alive influences parent buffering & polls)
// Keep this modest to balance latency & power (e.g., 6000–10000 ms).
constexpr uint16_t ED_KEEPALIVE_MS    = 7000;   // used when custom cfg path compiles

// Replace with your actual pins if you will drive local signals later.
// (Not required for an ED that only receives commands.)
static const uint8_t DEFAULT_ZONE_PINS[MAX_ZONES] = { 2, 3, 4, 5, 6, 7, 8, 9 };

// Pre-create static endpoints (no heap)
static ZigbeePowerOutlet EP_STORE[MAX_ZONES] = {
  ZigbeePowerOutlet(FIRST_ENDPOINT + 0), ZigbeePowerOutlet(FIRST_ENDPOINT + 1),
  ZigbeePowerOutlet(FIRST_ENDPOINT + 2), ZigbeePowerOutlet(FIRST_ENDPOINT + 3),
  ZigbeePowerOutlet(FIRST_ENDPOINT + 4), ZigbeePowerOutlet(FIRST_ENDPOINT + 5),
  ZigbeePowerOutlet(FIRST_ENDPOINT + 6), ZigbeePowerOutlet(FIRST_ENDPOINT + 7),
};

// ---------------- Device ----------------
class IrrigationController {
public:
  IrrigationController(uint8_t num_zones,
                       const uint8_t *pins,
                       const char *mfg = "Komal-SkyNET",
                       const char *model = "IrrigationController-ED")
  : zones_(clampZones(num_zones)),
    mfg_(mfg), model_(model),
    led_pin_(LED_PIN),
    led_mode_(LED_CONNECTING),
    led_last_ms_(0),
    led_pulse_start_(0),
    connected_(false) {
    for (uint8_t i = 0; i < zones_; ++i) {
      pins_[i] = pins[i];
      last_state_[i] = false;
      eps_[i] = &EP_STORE[i];
    }
  }

  void begin() {
    // Serial (optional)
    Serial.begin(115200);
    unsigned long t0 = millis();
    while (!Serial && (millis() - t0) < 2000) {}

    // LED init
    if (ledEnabled()) { pinMode(led_pin_, OUTPUT); setLed(false); setLedMode(LED_CONNECTING); }

    // (Optional) configure GPIOs if you’ll wire local outputs later
    for (uint8_t i = 0; i < zones_; ++i) { pinMode(pins_[i], OUTPUT); drivePin(i, /*on=*/false); }

    // Register endpoints: identify as BATTERY device for HA/Z2M
    for (uint8_t i = 0; i < zones_; ++i) {
      auto *ep = eps_[i];
      if (!ep) continue;
      ep->setManufacturerAndModel(mfg_, model_);
      ep->setPowerSource(ZB_POWER_SOURCE_BATTERY, 100);  // report as battery-powered (100% init) :contentReference[oaicite:1]{index=1}
      if (!Zigbee.addEndpoint(ep)) {
        Serial.printf("[ED] Failed to add EP %u\n", FIRST_ENDPOINT + i);
      }
      // after Zigbee.addEndpoint(ep) loop:
      for (uint8_t i = 0; i < zones_; ++i) { eps_[i]->setState(false); } // reflect OFF to ZCL/HA
    }

    // Power behavior: allow radio sleep when idle (sleepy ED) :contentReference[oaicite:2]{index=2}
    Zigbee.setRxOnWhenIdle(false);

    // Start Zigbee stack as END DEVICE
    // Path A: simple (portable) — rely on library defaults
    bool ok = Zigbee.begin(ZIGBEE_END_DEVICE);
    // Path B: optional custom keepalive if headers are available
#ifdef ESP_ZB_VERSION
    if (!ok) { /* fallback if previous failed */ }
    else {
      // Re-start with explicit ZED cfg (keep_alive tuned) if desired:
      esp_zb_cfg_t cfg = ESP_ZB_ZED_CONFIG();          // SDK macro for ED role config :contentReference[oaicite:3]{index=3}
      cfg.nwk_cfg.zed_cfg.keep_alive = ED_KEEPALIVE_MS; // ms; affects parent data buffering/polling :contentReference[oaicite:4]{index=4}
      Zigbee.begin(&cfg);
    }
#endif

    if (!Zigbee.started()) {
      Serial.println("[ED] Zigbee begin() failed; rebooting…");
      delay(600);
      ESP.restart();
    }

    // Fast join loop: blink while joining; solid when joined
    Serial.print("[ED] Joining");
    uint32_t start = millis();
    while (!Zigbee.connected()) {
      Serial.print(".");
      ledTask();
      delay(50);
      if (millis() - start > ZigbeeTimeout()) {
        Serial.println("\n[ED] Still trying in background.");
        break;
      }
    }
    connected_ = Zigbee.connected();
    if (connected_) { setLedMode(LED_SOLID); Serial.println("\n[ED] Ready (joined)."); }
  }

  void loop() {
    ledTask();

    // Detect link state changes (rejoin scenarios)
    const bool now_connected = Zigbee.connected();
    if (now_connected != connected_) {
      connected_ = now_connected;
      setLedMode(connected_ ? LED_SOLID : LED_CONNECTING);
    }

    // Reflect endpoint state changes (HA → device)
    for (uint8_t i = 0; i < zones_; ++i) {
      auto *ep = eps_[i];
      if (!ep) continue;
      const bool s = ep->getPowerOutletState();       // server-side On/Off state :contentReference[oaicite:5]{index=5}
      if (s != last_state_[i]) {
        drivePin(i, s);                               // optional local action (no-op if not wired)
        last_state_[i] = s;
        pulseLed();                                   // feedback on each command
        Serial.printf("[ED] Zone %u -> %s\n", i + 1, s ? "ON" : "OFF");
      }
    }

    delay(20);
  }

private:
  // -------- LED helpers --------
  enum LedMode : uint8_t { LED_CONNECTING, LED_SOLID, LED_PULSE };

  bool ledEnabled() const { return led_pin_ >= 0; }
  void setLed(bool on) {
    if (!ledEnabled()) return;
    digitalWrite(led_pin_, on ? HIGH : LOW);
  }
  void setLedMode(LedMode m) {
    if (!ledEnabled()) return;
    led_mode_ = m;
    switch (m) {
      case LED_CONNECTING: setLed(false); led_last_ms_ = millis(); break;
      case LED_SOLID:      setLed(true);  break;
      case LED_PULSE:      setLed(false); led_pulse_start_ = millis(); break;
    }
  }
  void pulseLed() { if (ledEnabled() && led_mode_ == LED_SOLID) setLedMode(LED_PULSE); }
  void ledTask() {
    if (!ledEnabled()) return;
    const uint32_t now = millis();
    if (led_mode_ == LED_CONNECTING) {
      if (now - led_last_ms_ >= CONNECT_BLINK_MS) {
        digitalWrite(led_pin_, !digitalRead(led_pin_));
        led_last_ms_ = now;
      }
    } else if (led_mode_ == LED_PULSE) {
      if (now - led_pulse_start_ >= PULSE_MS) setLedMode(LED_SOLID);
    }
  }

  // -------- Utility & IO --------
  static uint8_t clampZones(uint8_t z) { return z == 0 ? 1 : (z > MAX_ZONES ? MAX_ZONES : z); }
  static uint32_t ZigbeeTimeout() { return 30000; } // ms
  inline void drivePin(uint8_t idx, bool on) {
    const bool level = RELAY_ACTIVE_LOW ? !on : on;
    if (idx < zones_) digitalWrite(pins_[idx], level ? HIGH : LOW);
  }

  // -------- State --------
  uint8_t  zones_;
  const char *mfg_;
  const char *model_;

  uint8_t  pins_[MAX_ZONES];
  bool     last_state_[MAX_ZONES];
  ZigbeePowerOutlet *eps_[MAX_ZONES];

  // LED state
  int       led_pin_;
  LedMode   led_mode_;
  uint32_t  led_last_ms_;
  uint32_t  led_pulse_start_;
  bool      connected_;
};

// ---------------- Instantiate & Arduino glue ----------------
IrrigationController controller(
  /*num_zones=*/4,                   // up to 8
  /*gpio_pins=*/DEFAULT_ZONE_PINS
);

void setup() { controller.begin(); }
void loop()  { controller.loop();  }
