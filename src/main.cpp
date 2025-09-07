#include "Zigbee.h"
#include "HunterRoam.h"
#include "esp_task_wdt.h" // Include for the Watchdog Timer

/********************* Configuration **************************/
#define SMARTPORT_PIN 5
#define BUTTON_PIN    BOOT_PIN // Using the default BOOT button for factory reset
#define LED_PIN       LED_BUILTIN
#define LED_ON        LOW      // For many boards, the built-in LED is active-low (LOW turns it on)
#define LED_OFF       HIGH
#define NUM_ZONES     4
#define SAFETY_TIMEOUT_MINUTES 60 // Safety shut-off time in minutes
#define WDT_TIMEOUT_SECONDS 5     // Watchdog Timer: reboot if the main loop freezes for this long

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

// Define the states for the LED indicator in the global scope
enum LedState { UNKNOWN, BLINKING, SOLID };

/********************* Core Logic *****************************/

/**
 * @brief Handles a state change request from the Zigbee coordinator (e.g., Home Assistant).
 */
void handleZoneChange(uint8_t index, bool requestedState) {
    uint8_t zoneNumber = index + 1; // The HunterRoam library is 1-based

    if (requestedState) {
        Serial.printf("Received ON request for zone %d (%s) with %d-minute safety timer\n", zoneNumber, zones[index].modelName, SAFETY_TIMEOUT_MINUTES);
        byte err = hunter.startZone(zoneNumber, SAFETY_TIMEOUT_MINUTES);

        if (err != 0) {
            Serial.printf("ERROR starting zone %d: %s\n",
                          zoneNumber, hunter.errorHint(err).c_str());
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
#define MAKE_ZONE_CALLBACK(N) \
void onZone##N(bool state){ handleZoneChange(N, state); }

// Generate onZone0, onZone1, onZone2, onZone3
MAKE_ZONE_CALLBACK(0)
MAKE_ZONE_CALLBACK(1)
MAKE_ZONE_CALLBACK(2)
MAKE_ZONE_CALLBACK(3)

/********************* Helper Functions for Main Loop *********/

/**
 * @brief On first connect after boot, turns all valves off as a safety measure.
 */
void handleInitialShutdown() {
    static bool initialShutdownComplete = false;
    if (Zigbee.connected() && !initialShutdownComplete) {
        Serial.println("First connect: Setting all zones to OFF as a safety measure.");
        for (uint8_t i = 0; i < NUM_ZONES; i++) {
            valves[i]->setLight(false);
        }
        initialShutdownComplete = true;
    }
}

/**
 * @brief Manages the status LED (blinking for disconnected, solid for connected).
 */
void handleLedIndicator() {
    static LedState currentLedState = UNKNOWN;
    static unsigned long ledTimer = 0;

    if (Zigbee.connected()) {
        if (currentLedState != SOLID) {
            digitalWrite(LED_PIN, LED_ON);
            currentLedState = SOLID;
        }
    } else {
        currentLedState = BLINKING;
        if (millis() - ledTimer > 500) {
            digitalWrite(LED_PIN, !digitalRead(LED_PIN));
            ledTimer = millis();
        }
    }
}

/**
 * @brief Handles the non-blocking check for the factory reset button.
 */
void handleFactoryResetButton() {
    static unsigned long buttonPressStartTime = 0;
    static bool isButtonBeingHeld = false;

    if (digitalRead(BUTTON_PIN) == LOW) {
        if (!isButtonBeingHeld) {
            isButtonBeingHeld = true;
            buttonPressStartTime = millis();
            Serial.println("Button pressed. Hold for 5 seconds for factory reset.");
        } else if (millis() - buttonPressStartTime > 5000) {
            Serial.println("Factory reset triggered. Rebooting...");
            Zigbee.factoryReset();
        }
    } else {
        if (isButtonBeingHeld) {
            Serial.println("Button released.");
            isButtonBeingHeld = false;
        }
    }
}

/********************* Setup **********************************/
void setup() {
    Serial.begin(115200);
    
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LED_OFF);

    // Initialize the Watchdog Timer. This is a safety feature to automatically
    // reboot the device if the software freezes.
    Serial.printf("Initializing Watchdog Timer with %d second timeout.\n", WDT_TIMEOUT_SECONDS);
    // The API for esp_task_wdt_init has changed. The timeout member is now 'timeout_ms'.
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WDT_TIMEOUT_SECONDS * 1000,
        .trigger_panic = true,
    };
    esp_task_wdt_init(&wdt_config);
    esp_task_wdt_add(NULL); // Add this current task to the watchdog

    // Create and register Zigbee endpoints for each valve
    for (uint8_t i = 0; i < NUM_ZONES; i++) {
        valves[i] = new ZigbeeLight(zones[i].endpoint);
        valves[i]->setManufacturerAndModel("SkynetIrrigation", "Controller");
        Zigbee.addEndpoint(valves[i]);
    }

    // Attach callbacks
    valves[0]->onLightChange(onZone0);
    valves[1]->onLightChange(onZone1);
    valves[2]->onLightChange(onZone2);
    valves[3]->onLightChange(onZone3);

    if (!Zigbee.begin()) {
        Serial.println("Zigbee failed to start! Rebooting...");
        ESP.restart();
    }

    Serial.println("Zigbee started. Waiting for connection...");
}

/********************* Main Loop ******************************/
void loop() {
    // --- TEST CODE for Watchdog Timer ---
    // To test the watchdog, uncomment the following line. This creates an
    // infinite loop, which will prevent esp_task_wdt_reset() from being called.
    // After WDT_TIMEOUT_SECONDS, the device should automatically reboot.
    // REMEMBER TO COMMENT THIS LINE OUT AGAIN AFTER TESTING!
    // while(true);

    // 1. "Pet" the watchdog to show the main loop is running correctly.
    esp_task_wdt_reset();

    // 2. Handle initial shutdown on first connect.
    handleInitialShutdown();

    // 3. Update the status LED.
    handleLedIndicator();

    // 4. Check if the factory reset button is being pressed.
    handleFactoryResetButton();

    // 5. Add a small delay to reduce CPU usage and power consumption.
    // This allows the processor to rest instead of running a tight loop.
    delay(20);
}


