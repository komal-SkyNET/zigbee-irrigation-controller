#include "Zigbee.h"
#include "HunterRoam.h"

/********************* Configuration **************************/
#define SMARTPORT_PIN 5
#define BUTTON_PIN    BOOT_PIN // Using the default BOOT button for factory reset
#define LED_PIN       LED_BUILTIN
#define LED_ON        LOW      // For many boards, the built-in LED is active-low (LOW turns it on)
#define LED_OFF       HIGH
#define NUM_ZONES     4
#define SAFETY_TIMEOUT_MINUTES 60 // Safety shut-off time in minutes

// The ZoneConfig is simplified. Home Assistant will manage names and all timing.
// We only need to define the Zigbee endpoint and a model name for identification.
struct ZoneConfig {
    const char* modelName;
    uint8_t endpoint;
};

ZoneConfig zones[NUM_ZONES] = {
    {"Zone 1", 10},
    {"Zone 2", 11},
    {"Zone 3", 12},
    {"Zone 4", 13}
};

/********************* Hardware Instances *********************/
HunterRoam hunter(SMARTPORT_PIN);
ZigbeeLight* valves[NUM_ZONES];

/********************* Core Logic *****************************/

/**
 * @brief Handles a state change request from the Zigbee coordinator (e.g., Home Assistant).
 *
 * This function is now stateless. It directly translates the requested state
 * (on/off) into a hardware command for the Hunter controller. It does not
 * track timers or current state, as Home Assistant is the source of truth.
 *
 * @param index The index (0-3) of the zone to control.
 * @param requestedState The desired state: true for ON, false for OFF.
 */
void handleZoneChange(uint8_t index, bool requestedState) {
    uint8_t zoneNumber = index + 1; // The HunterRoam library is 1-based

    if (requestedState) {
        Serial.printf("Received ON request for zone %d (%s) with %d-minute safety timer\n", zoneNumber, zones[index].modelName, SAFETY_TIMEOUT_MINUTES);
        
        // Start the zone with the defined safety timeout. This acts as a dead man's
        // switch in case the 'off' command from Home Assistant is never received.
        byte err = hunter.startZone(zoneNumber, SAFETY_TIMEOUT_MINUTES);

        if (err != 0) {
            Serial.printf("ERROR starting zone %d: %s\n",
                          zoneNumber, hunter.errorHint(err).c_str());
            // We don't need to revert the state in Zigbee. HA will see the
            // command failed or the state didn't change.
        } else {
            Serial.printf("Successfully started zone %d\n", zoneNumber);
        }
    } else {
        Serial.printf("Received OFF request for zone %d (%s)\n", zoneNumber, zones[index].modelName);
        byte err = hunter.stopZone(zoneNumber);

        if (err != 0) {
            Serial.printf("ERROR stopping zone %d: %s\n",
                          zoneNumber, hunter.errorHint(err).c_str());
        } else {
            Serial.printf("Successfully stopped zone %d\n", zoneNumber);
        }
    }
}

/********************* Zigbee Callbacks ***********************/
// This macro is an efficient way to generate a unique callback
// function for each zone, which then calls our handler.
#define MAKE_ZONE_CALLBACK(N) \
void onZone##N(bool state){ handleZoneChange(N, state); }

// Generate onZone0, onZone1, onZone2, onZone3
MAKE_ZONE_CALLBACK(0)
MAKE_ZONE_CALLBACK(1)
MAKE_ZONE_CALLBACK(2)
MAKE_ZONE_CALLBACK(3)

/********************* Setup **********************************/
void setup() {
    Serial.begin(115200);

    // The initial shutdown logic is now handled in the main loop once a
    // Zigbee connection is established. This is safer and more reliable.
    
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LED_OFF); // Start with LED off

    // Create and register Zigbee endpoints for each valve
    for (uint8_t i = 0; i < NUM_ZONES; i++) {
        valves[i] = new ZigbeeLight(zones[i].endpoint);
        valves[i]->setManufacturerAndModel("SkynetIrrigation", "Controller");
        Zigbee.addEndpoint(valves[i]);
    }

    // Attach the unique callback to each endpoint
    valves[0]->onLightChange(onZone0);
    valves[1]->onLightChange(onZone1);
    valves[2]->onLightChange(onZone2);
    valves[3]->onLightChange(onZone3);

    // Start the Zigbee stack. The LED will blink until a connection is made.
    if (!Zigbee.begin()) {
        Serial.println("Zigbee failed to start! Rebooting...");
        ESP.restart();
    }

    Serial.println("Zigbee started. Waiting for connection...");
}

/********************* Main Loop ******************************/
// To make the LED gestures more reliable, we'll use a simple state machine.
enum LedState { UNKNOWN, BLINKING, SOLID };
static LedState currentLedState = UNKNOWN;

void loop() {
    // --- 1. Initial Shutdown on First Connect ---
    // On the first successful connection to the Zigbee network after a reboot,
    // we ensure all valves are set to OFF. This prevents a valve from being
    // stuck on after a power cycle.
    static bool initialShutdownComplete = false;
    if (Zigbee.connected() && !initialShutdownComplete) {
        Serial.println("First connect: Setting all zones to OFF as a safety measure.");
        for (uint8_t i = 0; i < NUM_ZONES; i++) {
            // This triggers the onZoneN callback, which calls handleZoneChange(i, false)
            // to issue the actual hunter.stopZone() command.
            valves[i]->setLight(false);
        }
        initialShutdownComplete = true; // Ensure this logic only runs once.
    }

    // --- 2. LED Status Indicator ---
    // This logic is non-blocking to ensure the rest of the code runs smoothly.
    static unsigned long ledTimer = 0;
    if (Zigbee.connected()) {
        // We are connected. The LED should be solid ON.
        // We only write to the pin if the state is not already SOLID to be efficient.
        if (currentLedState != SOLID) {
            digitalWrite(LED_PIN, LED_ON);
            currentLedState = SOLID;
        }
    } else {
        // We are not connected. The LED should blink.
        currentLedState = BLINKING;
        if (millis() - ledTimer > 500) { // 500ms blink interval
            digitalWrite(LED_PIN, !digitalRead(LED_PIN));
            ledTimer = millis();
        }
    }

    // --- 3. Factory Reset Button ---
    // This is a non-blocking check. Using a 'while' loop here would freeze the device,
    // stopping Zigbee communication and all other tasks until the button is released.
    // The current approach checks the button on each pass of the main loop.
    static unsigned long buttonPressStartTime = 0;
    static bool isButtonBeingHeld = false;

    if (digitalRead(BUTTON_PIN) == LOW) { // Button is currently pressed
        if (!isButtonBeingHeld) {
            // This is the first moment we see the button is pressed.
            // We record the start time and set a flag. This block only runs once per press.
            isButtonBeingHeld = true;
            buttonPressStartTime = millis();
            Serial.println("Button pressed. Hold for 5 seconds for factory reset.");
        } else if (millis() - buttonPressStartTime > 5000) { // Check if held for 5s
            Serial.println("Factory reset triggered. Rebooting...");
            Zigbee.factoryReset(); // This function reboots the device.
        }
    } else { // Button is not pressed
        // If the button was being held, this is the moment it's released.
        // We reset the flag so we can detect the next press.
        if (isButtonBeingHeld) {
            Serial.println("Button released.");
            isButtonBeingHeld = false;
        }
    }
}
