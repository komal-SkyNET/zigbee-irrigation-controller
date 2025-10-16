#include "Zigbee.h"
#include "HunterRoam.h"
#include "esp_task_wdt.h" // Include for the Watchdog Timer

/********************* Configuration **************************/
#define SMARTPORT_PIN D5
#define BUTTON_PIN    BOOT_PIN // Using the default BOOT button for factory reset
#define LED_PIN       LED_BUILTIN
#define LED_ON        LOW      // For many boards, the built-in LED is active-low (LOW turns it on)
#define LED_OFF       HIGH
#define NUM_ZONES     8                // Number of irrigation zones (accepts 1-48 zones). 
#define SAFETY_TIMEOUT_MINUTES 60      // Safety shut-off time in minutes
#define WDT_TIMEOUT_SECONDS 30         // Watchdog Timer: reboot if the main loop freezes for this long. Increased for stability.

#if NUM_ZONES < 1
    #error "NUM_ZONES must be at least 1"
#endif
#if NUM_ZONES > 48
    #error "NUM_ZONES cannot exceed 48 (Hunter controller limit)"
#endif
/***********************************************/

// Helper function to generate zone name dynamically
String getZoneName(uint8_t index) {
    return "Zone " + String(index + 1);
}

// Helper function to get Zigbee endpoint for a zone
// Note: Endpoint IDs start at 10 (not to be confused with zone numbers which start at 1)
// Zone 1 = zigbeeEndpoint 10, Zone 2 = zigbeeEndpoint 11, etc.
uint8_t getZoneEndpoint(uint8_t index) {
    return 10 + index;  // index is 0-based, so Zone 1 (index 0) gets endpoint 10
}

/********************* Hardware Instances *********************/
HunterRoam hunter(SMARTPORT_PIN);
ZigbeeLight* valves[NUM_ZONES];

// Array to track the software safety timer for each zone.
// Its only purpose is to sync the Zigbee state if the hardware timer shuts a valve off.
static unsigned long zoneSafetyOffTime[NUM_ZONES] = {0};

// States for the LED indicator
enum LedState { UNKNOWN, BLINKING, ZONE_ACTIVE, CONNECTED_IDLE };

/********************* Core Logic *****************************/

/**
 * @brief Handles a state change request
 */
void handleZoneChange(uint8_t index, bool requestedState) {
    uint8_t zoneNumber = index + 1; // The HunterRoam library is 1-based

    if (requestedState) {
        Serial.printf("Received ON request for zone %d (%s) with %d-minute safety timer\n", zoneNumber, getZoneName(index).c_str(), SAFETY_TIMEOUT_MINUTES);
        byte err = hunter.startZone(zoneNumber, SAFETY_TIMEOUT_MINUTES);

        if (err != 0) {
            Serial.printf("ERROR starting zone %d: %s\n",
                          zoneNumber, hunter.errorHint(err).c_str());
        } else {
            Serial.printf("Successfully started zone %d\n", zoneNumber);
            // Start the software safety timer to keep Zigbee state in sync.
            zoneSafetyOffTime[index] = millis() + (SAFETY_TIMEOUT_MINUTES * 60 * 1000UL);
        }
    } else {
        Serial.printf("Received OFF request for zone %d (%s)\n", zoneNumber, getZoneName(index).c_str());
        byte err = hunter.stopZone(zoneNumber);

        if (err != 0) {
            Serial.printf("ERROR stopping zone %d: %s\n",
                          zoneNumber, hunter.errorHint(err).c_str());
        } else {
            Serial.printf("Successfully stopped zone %d\n", zoneNumber);
            // Clear the software safety timer as HA/coordinator has shut the zone off normally.
            zoneSafetyOffTime[index] = 0;
        }
    }
}

/********************* Zigbee Callbacks ***********************/
#define MAKE_ZONE_CALLBACK(N) \
void onZone##N(bool state){ handleZoneChange(N, state); }

// Generate callbacks for all 48 possible zones (Hunter controller limit)
// Only NUM_ZONES callbacks will be used and attached in setup()
MAKE_ZONE_CALLBACK(0)
MAKE_ZONE_CALLBACK(1)
MAKE_ZONE_CALLBACK(2)
MAKE_ZONE_CALLBACK(3)
MAKE_ZONE_CALLBACK(4)
MAKE_ZONE_CALLBACK(5)
MAKE_ZONE_CALLBACK(6)
MAKE_ZONE_CALLBACK(7)
MAKE_ZONE_CALLBACK(8)
MAKE_ZONE_CALLBACK(9)
MAKE_ZONE_CALLBACK(10)
MAKE_ZONE_CALLBACK(11)
MAKE_ZONE_CALLBACK(12)
MAKE_ZONE_CALLBACK(13)
MAKE_ZONE_CALLBACK(14)
MAKE_ZONE_CALLBACK(15)
MAKE_ZONE_CALLBACK(16)
MAKE_ZONE_CALLBACK(17)
MAKE_ZONE_CALLBACK(18)
MAKE_ZONE_CALLBACK(19)
MAKE_ZONE_CALLBACK(20)
MAKE_ZONE_CALLBACK(21)
MAKE_ZONE_CALLBACK(22)
MAKE_ZONE_CALLBACK(23)
MAKE_ZONE_CALLBACK(24)
MAKE_ZONE_CALLBACK(25)
MAKE_ZONE_CALLBACK(26)
MAKE_ZONE_CALLBACK(27)
MAKE_ZONE_CALLBACK(28)
MAKE_ZONE_CALLBACK(29)
MAKE_ZONE_CALLBACK(30)
MAKE_ZONE_CALLBACK(31)
MAKE_ZONE_CALLBACK(32)
MAKE_ZONE_CALLBACK(33)
MAKE_ZONE_CALLBACK(34)
MAKE_ZONE_CALLBACK(35)
MAKE_ZONE_CALLBACK(36)
MAKE_ZONE_CALLBACK(37)
MAKE_ZONE_CALLBACK(38)
MAKE_ZONE_CALLBACK(39)
MAKE_ZONE_CALLBACK(40)
MAKE_ZONE_CALLBACK(41)
MAKE_ZONE_CALLBACK(42)
MAKE_ZONE_CALLBACK(43)
MAKE_ZONE_CALLBACK(44)
MAKE_ZONE_CALLBACK(45)
MAKE_ZONE_CALLBACK(46)
MAKE_ZONE_CALLBACK(47)

// Array of function pointers for dynamic callback attachment
void (*zoneCallbacks[48])(bool) = {
    onZone0, onZone1, onZone2, onZone3, onZone4, onZone5, onZone6, onZone7,
    onZone8, onZone9, onZone10, onZone11, onZone12, onZone13, onZone14, onZone15,
    onZone16, onZone17, onZone18, onZone19, onZone20, onZone21, onZone22, onZone23,
    onZone24, onZone25, onZone26, onZone27, onZone28, onZone29, onZone30, onZone31,
    onZone32, onZone33, onZone34, onZone35, onZone36, onZone37, onZone38, onZone39,
    onZone40, onZone41, onZone42, onZone43, onZone44, onZone45, onZone46, onZone47
};

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
 * @brief Checks if any zone is currently running by checking the safety timer array.
 * @return true if at least one zone is active, false otherwise.
 */
bool isAnyZoneActive() {
    for (uint8_t i = 0; i < NUM_ZONES; i++) {
        // A non-zero value means the timer is set, so the zone is running.
        if (zoneSafetyOffTime[i] != 0) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Manages the status LED based on connection and zone status.
 * - Blinking: Disconnected from Zigbee network.
 * - Solid OFF: Connected to Zigbee, but no zones are running.
 * - Solid ON: Connected to Zigbee and at least one zone is running.
 */
void handleLedIndicator() {
    static LedState currentLedState = UNKNOWN;
    static unsigned long ledTimer = 0;

    if (Zigbee.connected()) {
        if (isAnyZoneActive()) {
            // A zone is active, LED should be solid ON
            if (currentLedState != ZONE_ACTIVE) {
                digitalWrite(LED_PIN, LED_ON);
                currentLedState = ZONE_ACTIVE;
            }
        } else {
            // Connected but idle, LED should be OFF
            if (currentLedState != CONNECTED_IDLE) {
                digitalWrite(LED_PIN, LED_OFF);
                currentLedState = CONNECTED_IDLE;
            }
        }
    } else {
        // Not connected, LED should blink
        if (currentLedState != BLINKING) {
            // Transitioning to blinking state, ensures the timer is reset.
            currentLedState = BLINKING;
        }

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

/**
 * @brief Checks if any zone's safety timer has expired and updates Zigbee state if so.
 */
void handleSafetyTimeout() {
    for (uint8_t i = 0; i < NUM_ZONES; i++) {
        // Check if a timer is active for this zone and if its time has come.
        // millis() >= ... is a rollover-safe way to check.
        if (zoneSafetyOffTime[i] != 0 && millis() >= zoneSafetyOffTime[i]) {
            Serial.printf("Safety timer expired for zone %d. Updating Zigbee state to OFF.\n", i + 1);
            valves[i]->setLight(false); // This will trigger the callback and sync everything.
            // Do NOT clear the timer here. If the stop command fails,
            // we want this check to run again on the next loop to re-attempt the shutdown.
        }
    }
}

/********************* Setup **********************************/
void setup() {
    Serial.begin(115200);
    delay(1000); // Add a small delay on boot for hardware to stabilize
    
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LED_OFF);

    // Initialize the Watchdog Timer.
    Serial.printf("Initializing Watchdog Timer with %d second timeout.\n", WDT_TIMEOUT_SECONDS);
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WDT_TIMEOUT_SECONDS * 1000,
        .trigger_panic = true,
    };
    esp_task_wdt_init(&wdt_config);
    esp_task_wdt_add(NULL); // Add this current task to the watchdog

    // Create and register Zigbee endpoints for each valve
    for (uint8_t i = 0; i < NUM_ZONES; i++) {
        valves[i] = new ZigbeeLight(getZoneEndpoint(i));
        valves[i]->setManufacturerAndModel("SkynetIrrigation", "Controller");
        Zigbee.addEndpoint(valves[i]);
    }

    // Attach callbacks dynamically based on NUM_ZONES
    for (uint8_t i = 0; i < NUM_ZONES; i++) {
        valves[i]->onLightChange(zoneCallbacks[i]);
    }

    // Temporarily remove our task from the watchdog before starting Zigbee,
    // as Zigbee.begin() can block for a long time if the hub is not nearby.
    Serial.println("Pausing watchdog for Zigbee initialization...");
    esp_task_wdt_delete(NULL); // NULL means the current task

    // Attempt to start Zigbee. If it fails, the library state is indeterminate,
    // and the safest action is to reboot and try again from a clean slate.
    if (!Zigbee.begin()) {
        Serial.println("Failed to start Zigbee. Rebooting to try again...");
        delay(1000); // Brief delay to allow the serial message to send
        ESP.restart();
    }
    
    // Re-add our task to the watchdog now that the blocking section is finished.
    Serial.println("Resuming watchdog monitoring.");
    esp_task_wdt_add(NULL); // NULL means the current task

    Serial.println("Zigbee started. Waiting for connection...");
}

/********************* Main Loop ******************************/
void loop() {
    // 1. "Pet" the watchdog to show the main loop is running correctly.
    esp_task_wdt_reset();

    // 2. Handle initial valve/zone shutdown on first connect.
    handleInitialShutdown();

    // 3. Update the status LED.
    handleLedIndicator();

    // 4. Check if the factory reset button is being pressed.
    handleFactoryResetButton();

    // 5. Check if any safety timers have expired and sync state.
    handleSafetyTimeout();

    // 6. Add a small delay to reduce CPU usage and power consumption.
    // This allows the processor to rest instead of running a tight loop.
    delay(20);
}
