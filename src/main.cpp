#include "Zigbee.h"
#include "HunterRoam.h"

/********************* Configuration **************************/
#define SMARTPORT_PIN 5
#define BUTTON_PIN BOOT_PIN
#define NUM_ZONES 4
#define LED_PIN LED_BUILTIN

struct ZoneConfig {
    const char* name;
    uint8_t endpoint;
    uint8_t wateringTime; // minutes
};

// Zone definitions
ZoneConfig zones[NUM_ZONES] = {
    {"Front Yard", 10, 7},
    {"Backyard",   11, 10},
    {"Garden Bed", 12, 5},
    {"Side Lawn",  13, 8}
};

/********************* HunterRoam **************************/
HunterRoam hunter(SMARTPORT_PIN);

/********************* Zigbee **************************/
ZigbeeLight* valves[NUM_ZONES];

/********************* State & Timers **************************/
unsigned long valveEndTimes[NUM_ZONES] = {0};
bool currentState[NUM_ZONES] = {false, false, false, false};  // our mirror of on/off
volatile bool updating[NUM_ZONES] = {false, false, false, false}; // re-entrancy guard

/********************* Helper Function **************************/
void handleZoneChange(uint8_t index, bool requestedState){
    // Prevent re-entrant loops if this was triggered by our own setLight()
    if (updating[index]) return;

    // If the state is already what’s requested, do nothing
    if (currentState[index] == requestedState) return;

    if (requestedState) {
        byte err = hunter.startZone(index + 1, zones[index].wateringTime);
        if (err != 0) {
            Serial.printf("Error starting zone %d (%s): %s\n",
                          index + 1, zones[index].name, hunter.errorHint(err).c_str());
            // Reflect failure: ensure UI is OFF without triggering recursion
            updating[index] = true;
            valves[index]->setLight(false);
            updating[index] = false;
            currentState[index] = false;
            valveEndTimes[index] = 0;
            return;
        }

        // Success: mark ON, set auto-off deadline
        currentState[index] = true;
        valveEndTimes[index] = millis() + zones[index].wateringTime * 60UL * 1000UL;

        // Update Zigbee attribute without re-entering callback
        updating[index] = true;
        valves[index]->setLight(true);
        updating[index] = false;

        Serial.printf("Started zone %d (%s) for %d minutes\n",
                      index + 1, zones[index].name, zones[index].wateringTime);

    } else {
        byte err = hunter.stopZone(index + 1);
        if (err != 0) {
            Serial.printf("Error stopping zone %d (%s): %s\n",
                          index + 1, zones[index].name, hunter.errorHint(err).c_str());
            // Keep UI consistent with actual state (still ON)
            updating[index] = true;
            valves[index]->setLight(true);
            updating[index] = false;
            currentState[index] = true;
            return;
        }

        currentState[index] = false;
        valveEndTimes[index] = 0;

        updating[index] = true;
        valves[index]->setLight(false);
        updating[index] = false;

        Serial.printf("Stopped zone %d (%s)\n", index + 1, zones[index].name);
    }
}

/********************* Macro for Callbacks **************************/
#define MAKE_ZONE_CALLBACK(N) \
void onZone##N(bool state){ handleZoneChange(N, state); }

MAKE_ZONE_CALLBACK(0)
MAKE_ZONE_CALLBACK(1)
MAKE_ZONE_CALLBACK(2)
MAKE_ZONE_CALLBACK(3)

/********************* Setup **************************/
void setup(){
    Serial.begin(115200);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // Create endpoints
    for (uint8_t i = 0; i < NUM_ZONES; i++) {
        valves[i] = new ZigbeeLight(zones[i].endpoint);
        valves[i]->setManufacturerAndModel("SkynetIrrigation", zones[i].name);
        currentState[i] = false;
        updating[i] = false;
        valveEndTimes[i] = 0;
    }

    // Attach callbacks
    valves[0]->onLightChange(onZone0);
    valves[1]->onLightChange(onZone1);
    valves[2]->onLightChange(onZone2);
    valves[3]->onLightChange(onZone3);

    // Register endpoints
    for (uint8_t i = 0; i < NUM_ZONES; i++) {
        Zigbee.addEndpoint(valves[i]);
    }

    // Start Zigbee
    if (!Zigbee.begin()) {
        Serial.println("Zigbee failed to start! Rebooting...");
        ESP.restart();
    }

    while (!Zigbee.connected()) delay(200);
    Serial.println("Connected to Zigbee network!");
}

/********************* Loop **************************/
void loop(){
    unsigned long now = millis();

    // Non-blocking factory reset
    static bool buttonPressed = false;
    static unsigned long buttonPressStart = 0;
    if (digitalRead(BUTTON_PIN) == LOW){
        if (!buttonPressed){
            buttonPressed = true;
            buttonPressStart = now;
        } else if (now - buttonPressStart > 3000){
            Serial.println("Factory reset Zigbee and rebooting...");
            delay(1000);
            Zigbee.factoryReset();
        }
    } else {
        buttonPressed = false;
    }

    // Auto-off timers (rollover-safe)
    for (uint8_t i = 0; i < NUM_ZONES; i++) {
        if (valveEndTimes[i] != 0 && (long)(now - valveEndTimes[i]) >= 0) {
            byte err = hunter.stopZone(i + 1);
            if (err == 0) {
                currentState[i] = false;
                valveEndTimes[i] = 0;

                // Update Zigbee attribute without re-triggering callback
                updating[i] = true;
                valves[i]->setLight(false);
                updating[i] = false;

                Serial.printf("Auto-turned off zone %d (%s)\n", i + 1, zones[i].name);
            } else {
                Serial.printf("Error auto-stopping zone %d (%s): %s\n",
                              i + 1, zones[i].name, hunter.errorHint(err).c_str());
                // leave state as-is on error
            }
        }
    }

    delay(50); // small delay for loop stability
}
