/*
  IrrigationController.cpp
  Seeed XIAO ESP32-C6  ⟶  Zigbee Router (ZCZR)
  - Up to 8 Zigbee "switch" endpoints (EP 11..18), each drives a GPIO relay
  - Dead-man's switch: if ANY zone stays ON > X hours, ALL zones OFF
  - Static endpoints (no heap)
  - LED:
      * Blinks while connecting
      * Solid when connected
      * Pulses on each switch change, then returns to solid
*/

#ifndef ZIGBEE_MODE_ZCZR
#error "Build as Zigbee Router: add -D ZIGBEE_MODE_ZCZR and use Zigbee ZCZR partitions."
#endif

#include <Arduino.h>
#include "Zigbee.h"  // Arduino Zigbee API (ZigbeeCore / ZigbeePowerOutlet)

// ---------------- Config ----------------
constexpr uint8_t  MAX_ZONES            = 8;
constexpr uint8_t  FIRST_ENDPOINT       = 11;     // EPs 11..18
constexpr bool     RELAY_ACTIVE_LOW     = false;  // set true for LOW-active relay modules
constexpr float    DEFAULT_DEADMAN_HR   = 2.0f;   // <=0 disables safety
constexpr bool     ZB_ERASE_NVS_ON_BOOT = false;  // set true ONCE if you changed partitions

// XIAO ESP32C6 builtin LED is typically GPIO 15; set -1 to disable LED features
constexpr int      LED_PIN              = 15;
constexpr uint32_t CONNECT_BLINK_MS     = 300;    // connecting blink rate
constexpr uint32_t PULSE_MS             = 180;    // pulse duration on command

// Replace with your actual relay GPIOs:
static const uint8_t DEFAULT_ZONE_PINS[MAX_ZONES] = { 2, 3, 4, 5, 6, 7, 8, 9 };

// Pre-create static endpoints (heap-free).
static ZigbeePowerOutlet EP_STORE[MAX_ZONES] = {
  ZigbeePowerOutlet(FIRST_ENDPOINT + 0), ZigbeePowerOutlet(FIRST_ENDPOINT + 1),
  ZigbeePowerOutlet(FIRST_ENDPOINT + 2), ZigbeePowerOutlet(FIRST_ENDPOINT + 3),
  ZigbeePowerOutlet(FIRST_ENDPOINT + 4), ZigbeePowerOutlet(FIRST_ENDPOINT + 5),
  ZigbeePowerOutlet(FIRST_ENDPOINT + 6), ZigbeePowerOutlet(FIRST_ENDPOINT + 7),
};

class IrrigationController {
public:
  IrrigationController(uint8_t num_zones,
                       const uint8_t *pins,
                       float deadman_hours = DEFAULT_DEADMAN_HR,
                       const char *mfg = "DIY",
                       const char *model = "IrrigationController")
  : zones_(clampZones(num_zones)),
    deadman_ms_(hoursToMs(deadman_hours)),
    mfg_(mfg), model_(model),
    led_pin_(LED_PIN),
    led_mode_(LED_CONNECTING),
    led_last_ms_(0),
    led_pulse_start_(0),
    connected_(false) {
    for (uint8_t i = 0; i < zones_; ++i) {
      pins_[i] = pins[i];
      last_state_[i] = false;
      on_since_ms_[i] = 0;
      eps_[i] = &EP_STORE[i];
    }
  }

  void begin() {
    // Bring up serial early for USB-CDC
    Serial.begin(115200);
    unsigned long start = millis();
    while (!Serial && (millis() - start) < 2000) { /* wait up to ~2s */ }
    Serial.println("\n[Irrigation] Booting…");

    // LED init
    if (ledEnabled()) {
      pinMode(led_pin_, OUTPUT);
      setLed(false);
      setLedMode(LED_CONNECTING);
    }

    // GPIO init: ensure OFF on boot
    for (uint8_t i = 0; i < zones_; ++i) {
      pinMode(pins_[i], OUTPUT);
      drivePin(i, /*on=*/false);
    }

    // Endpoint setup & registration
    for (uint8_t i = 0; i < zones_; ++i) {
      ZigbeePowerOutlet *ep = eps_[i];
      if (!ep) continue;
      ep->setManufacturerAndModel(mfg_, model_);
      ep->setPowerSource(ZB_POWER_SOURCE_MAINS);
      if (!Zigbee.addEndpoint(ep)) {
        Serial.printf("[Irrigation] Failed to add EP %u\n", FIRST_ENDPOINT + i);
      } else {
        Serial.printf("[Irrigation] Added EP %u\n", FIRST_ENDPOINT + i);
      }
    }

    // Start Zigbee stack (Router). Optionally wipe NVS once if toggled above.
    Serial.println("[Irrigation] Starting Zigbee (router)...");
    if (!Zigbee.begin(ZIGBEE_ROUTER, ZB_ERASE_NVS_ON_BOOT)) {
      Serial.println("[Irrigation] Zigbee begin() failed; rebooting…");
      delay(750);
      ESP.restart();
    }

    // Join loop: blink while connecting, solid when connected
    Serial.print("[Irrigation] Joining");
    uint32_t t0 = millis();
    while (!Zigbee.connected()) {
      Serial.print(".");
      ledTask();               // maintain connecting blink
      delay(50);
      if (millis() - t0 > 30000) {
        Serial.println("\n[Irrigation] Still trying in background.");
        break;
      }
    }
    connected_ = Zigbee.connected();
    if (connected_) {
      setLedMode(LED_SOLID);
      Serial.println("\n[Irrigation] Ready (joined).");
    }
  }

  void loop() {
    // Keep LED state machine responsive
    ledTask();

    // Detect link state changes (e.g., dropped & rejoin)
    const bool now_connected = Zigbee.connected();
    if (now_connected != connected_) {
      connected_ = now_connected;
      setLedMode(connected_ ? LED_SOLID : LED_CONNECTING);
    }

    // Handle endpoint states + safety
    const uint32_t now = millis();
    for (uint8_t i = 0; i < zones_; ++i) {
      ZigbeePowerOutlet *ep = eps_[i];
      if (!ep) continue;

      const bool s = ep->getPowerOutletState();
      if (s != last_state_[i]) {
        drivePin(i, s);
        last_state_[i] = s;
        on_since_ms_[i] = s ? now : 0;
        Serial.printf("[Irrigation] Zone %u -> %s\n", i + 1, s ? "ON" : "OFF");
        pulseLed(); // blink on command/switch change
      }

      // Dead-man safety per zone
      if (deadman_ms_ > 0 && s && on_since_ms_[i] > 0 && (now - on_since_ms_[i]) > deadman_ms_) {
        Serial.printf("[Irrigation] Dead-man (zone %u) triggered. ALL OFF.\n", i + 1);
        allOff();
        for (uint8_t z = 0; z < zones_; ++z) on_since_ms_[z] = 0;
        pulseLed(); // visual feedback on safety trip
        break;
      }
    }

    delay(20);
  }

  void setDeadmanHours(float hours) { deadman_ms_ = hoursToMs(hours); }

  void allOff() {
    for (uint8_t i = 0; i < zones_; ++i) {
      ZigbeePowerOutlet *ep = eps_[i];
      if (!ep) continue;
      if (ep->getPowerOutletState()) { ep->setState(false); } // reflect to HA
      if (last_state_[i]) {
        drivePin(i, false);
        last_state_[i] = false;
      }
    }
  }

private:
  // -------- LED helpers --------
  enum LedMode : uint8_t { LED_CONNECTING, LED_SOLID, LED_PULSE };

  bool ledEnabled() const { return led_pin_ >= 0; }
  void setLed(bool on) {
    if (!ledEnabled()) return;
    digitalWrite(led_pin_, on ? LOW : HIGH); // builtin LED is active-HIGH on XIAO C6
  }
  void setLedMode(LedMode m) {
    if (!ledEnabled()) return;
    led_mode_ = m;
    switch (m) {
      case LED_CONNECTING:
        setLed(false);
        led_last_ms_ = millis();
        break;
      case LED_SOLID:
        setLed(true);
        break;
      case LED_PULSE:
        // momentarily turn LED off (or toggle) then restore to SOLID
        setLed(false);
        led_pulse_start_ = millis();
        break;
    }
  }
  void pulseLed() {
    // Only “blink then return to solid” when in solid/connected mode
    if (!ledEnabled()) return;
    if (led_mode_ == LED_SOLID) setLedMode(LED_PULSE);
  }
  void ledTask() {
    if (!ledEnabled()) return;
    const uint32_t now = millis();
    if (led_mode_ == LED_CONNECTING) {
      if (now - led_last_ms_ >= CONNECT_BLINK_MS) {
        digitalWrite(led_pin_, !digitalRead(led_pin_));
        led_last_ms_ = now;
      }
    } else if (led_mode_ == LED_PULSE) {
      if (now - led_pulse_start_ >= PULSE_MS) {
        setLedMode(LED_SOLID);
      }
    }
  }

  // -------- Utility & IO --------
  static uint8_t  clampZones(uint8_t z) { return z == 0 ? 1 : (z > MAX_ZONES ? MAX_ZONES : z); }
  static uint32_t hoursToMs(float h) {
    if (h <= 0.0f) return 0;
    const double ms = (double)h * 3600000.0;
    return (ms > 4294967295.0) ? 4294967295UL : (uint32_t)ms;
  }
  inline void drivePin(uint8_t idx, bool on) {
    const bool level = RELAY_ACTIVE_LOW ? !on : on;
    digitalWrite(pins_[idx], level ? HIGH : LOW);
  }

  // -------- State --------
  uint8_t  zones_;
  uint32_t deadman_ms_;
  const char *mfg_;
  const char *model_;

  uint8_t  pins_[MAX_ZONES];
  bool     last_state_[MAX_ZONES];
  uint32_t on_since_ms_[MAX_ZONES];
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
  /*num_zones=*/4,
  /*gpio_pins=*/DEFAULT_ZONE_PINS,
  /*deadman_hours=*/2.0f
);

void setup() { controller.begin(); }
void loop()  { controller.loop();  }
