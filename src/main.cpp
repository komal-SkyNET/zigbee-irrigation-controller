#include "Zigbee.h"
#include "HunterRoam.h"
#include "esp_zigbee_cluster.h"   // for esp_zb_zcl_set_attribute_val & cluster IDs

/********************* Configuration **************************/
#define SMARTPORT_PIN 5
#define BUTTON_PIN    BOOT_PIN
#define LED_PIN       LED_BUILTIN

#define NUM_ZONES 4

struct ZoneConfig {
  const char* name;
  uint8_t endpoint;       // Zigbee endpoint ID
  uint8_t wateringTime;   // minutes
};

// Define your zones/endpoints/durations here
ZoneConfig zones[NUM_ZONES] = {
  {"Front Yard", 10, 2},
  {"Backyard",   11, 5},
  {"Garden Bed", 12, 15},
  {"Side Lawn",  13, 1}
};

/********************* HunterRoam **************************/
HunterRoam hunter(SMARTPORT_PIN);

/********************* Zigbee Endpoints **************************/
ZigbeeDimmableLight* valves[NUM_ZONES];

/********************* State & Timers **************************/
static volatile bool updating[NUM_ZONES] = {false};  // guard only for app-initiated HA updates
unsigned long valveEndTimes[NUM_ZONES]   = {0};      // millis when zone should turn off
unsigned long valveDurations[NUM_ZONES]  = {0};      // duration (ms) of current run

/********************* Utilities **************************/
// Write Level attribute directly (no callback fired)
static void writeLevelAttr(uint8_t endpoint, uint8_t level /*0..255*/) {
  esp_zb_lock_acquire(portMAX_DELAY);
  esp_zb_zcl_set_attribute_val(
    endpoint,
    ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
    ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
    ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID,
    &level,
    false
  );
  esp_zb_lock_release();
}

// Called when HA toggles a zone (via Zigbee on/off)
static void handleZoneChange(uint8_t index, bool requestedState) {
  if (updating[index]) {
    // We’re in the middle of an app-initiated cluster update; ignore to avoid loops.
    return;
  }

  if (requestedState) {
    // Turn ON: start zone for configured duration
    const uint8_t minutes = zones[index].wateringTime;
    byte err = hunter.startZone(index + 1, minutes);
    if (err != 0) {
      Serial.printf("Error starting zone %u (%s): %s\n",
                    (unsigned)(index + 1), zones[index].name, hunter.errorHint(err).c_str());
      // Reflect failure back to HA
      updating[index] = true;
      valves[index]->setLightState(false);
      updating[index] = false;
      writeLevelAttr(zones[index].endpoint, 0);
      return;
    }

    Serial.printf("Started zone %u (%s) for %u minutes\n",
                  (unsigned)(index + 1), zones[index].name, (unsigned)minutes);

    valveDurations[index] = (unsigned long)minutes * 60UL * 1000UL;
    valveEndTimes[index]  = millis() + valveDurations[index];

    // Immediately set “remaining time %” to 100% (255)
    writeLevelAttr(zones[index].endpoint, 255);
  } else {
    // Turn OFF: stop zone
    byte err = hunter.stopZone(index + 1);
    if (err != 0) {
      Serial.printf("Error stopping zone %u (%s): %s\n",
                    (unsigned)(index + 1), zones[index].name, hunter.errorHint(err).c_str());
      // Keep HA showing ON if stop failed
      updating[index] = true;
      valves[index]->setLightState(true);
      updating[index] = false;
      writeLevelAttr(zones[index].endpoint, 255);
      return;
    }

    Serial.printf("Stopped zone %u (%s)\n", (unsigned)(index + 1), zones[index].name);
    valveEndTimes[index]  = 0;
    valveDurations[index] = 0;
    writeLevelAttr(zones[index].endpoint, 0);
  }
}

/********************* Plain C callbacks (function pointers) **************************/
#define MAKE_ZONE_CALLBACK(N) \
  void onZone##N(bool state, uint8_t /*level*/) { handleZoneChange(N, state); }

MAKE_ZONE_CALLBACK(0)
MAKE_ZONE_CALLBACK(1)
MAKE_ZONE_CALLBACK(2)
MAKE_ZONE_CALLBACK(3)

/********************* Setup **************************/
void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Create endpoints
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    valves[i] = new ZigbeeDimmableLight(zones[i].endpoint);
    valves[i]->setManufacturerAndModel("SkynetIrrigation", zones[i].name);
    Zigbee.addEndpoint(valves[i]);
  }

  // Attach function-pointer callbacks (signature must be void(bool,uint8_t))
  valves[0]->onLightChange(onZone0);
  valves[1]->onLightChange(onZone1);
  valves[2]->onLightChange(onZone2);
  valves[3]->onLightChange(onZone3);

  // Start Zigbee
  if (!Zigbee.begin()) {
    Serial.println("Zigbee failed to start! Rebooting...");
    ESP.restart();
  }
  while (!Zigbee.connected()) {
    delay(200);
  }
  Serial.println("Connected to Zigbee network!");

  // Initialize Level attribute to 0 (no remaining time) for all zones
  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    writeLevelAttr(zones[i].endpoint, 0);
  }
}

/********************* Loop **************************/
void loop() {
  const unsigned long now = millis();

  // Factory reset if button held ~3s
  static bool btnPressed = false;
  static unsigned long btnStart = 0;
  if (digitalRead(BUTTON_PIN) == LOW) {
    if (!btnPressed) { btnPressed = true; btnStart = now; }
    else if ((now - btnStart) > 3000UL) {
      Serial.println("Factory reset Zigbee and rebooting...");
      delay(250);
      Zigbee.factoryReset();
    }
  } else {
    btnPressed = false;
  }

  // Update remaining-time % about once per second, and handle auto-off
  static unsigned long nextLevelUpdate = 0;
  const bool doLevelUpdate = (now >= nextLevelUpdate);
  if (doLevelUpdate) nextLevelUpdate = now + 1000UL;

  for (uint8_t i = 0; i < NUM_ZONES; i++) {
    if (valveEndTimes[i] != 0) {
      // Check auto-off
      if ((long)(now - valveEndTimes[i]) >= 0) {
        // Time’s up: stop hardware, update HA, zero the level
        byte err = hunter.stopZone(i + 1);
        if (err == 0) {
          Serial.printf("Auto-turned off zone %u (%s)\n",
                        (unsigned)(i + 1), zones[i].name);
          valveEndTimes[i]  = 0;
          valveDurations[i] = 0;

          // Tell HA (guarded to avoid callback loop)
          updating[i] = true;
          valves[i]->setLight(false, 0);   // triggers HA OFF & callback (ignored by guard)
          updating[i] = false;

          writeLevelAttr(zones[i].endpoint, 0);
        } else {
          Serial.printf("Error auto-stopping zone %u (%s): %s\n",
                        (unsigned)(i + 1), zones[i].name, hunter.errorHint(err).c_str());
        }
      } else if (doLevelUpdate && valveDurations[i] != 0) {
        // Refresh remaining-time % (0..255), written directly to attribute (no callback)
        unsigned long timeLeft = valveEndTimes[i] - now;
        uint8_t percent = (uint8_t)((timeLeft * 255UL) / valveDurations[i]); // safe since valveDurations>0
        if (percent > 255) percent = 255; // clamp (shouldn’t happen)
        writeLevelAttr(zones[i].endpoint, percent);
      }
    }
  }

  // Tiny delay to keep loop stable without starving Zigbee stack
  delay(20);
}
