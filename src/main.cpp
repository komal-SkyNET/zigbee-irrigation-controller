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

/********************* Auto-Off Timers **************************/
unsigned long valveEndTimes[NUM_ZONES] = {0,0,0,0};

/********************* Helper Functions **************************/
void handleZoneChange(uint8_t index, bool state){
    if(state){
        byte err = hunter.startZone(index+1, zones[index].wateringTime);
        if(err != 0){
            Serial.printf("Error starting zone %d (%s): %s\n",
                          index+1, zones[index].name, hunter.errorHint(err).c_str());
            valves[index]->setLight(false);
            valveEndTimes[index] = 0;
            return;
        }
        valves[index]->setLight(true);
        valveEndTimes[index] = millis() + zones[index].wateringTime * 60UL * 1000UL;
        Serial.printf("Started zone %d (%s) for %d minutes\n",
                      index+1, zones[index].name, zones[index].wateringTime);
    } else {
        byte err = hunter.stopZone(index+1);
        if(err != 0){
            Serial.printf("Error stopping zone %d (%s): %s\n",
                          index+1, zones[index].name, hunter.errorHint(err).c_str());
            valves[index]->setLight(true);
            return;
        }
        valves[index]->setLight(false);
        valveEndTimes[index] = 0;
        Serial.printf("Stopped zone %d (%s)\n", index+1, zones[index].name);
    }
}

// Plain callbacks for ZigbeeLight
void onZone0(bool state){ handleZoneChange(0,state); }
void onZone1(bool state){ handleZoneChange(1,state); }
void onZone2(bool state){ handleZoneChange(2,state); }
void onZone3(bool state){ handleZoneChange(3,state); }

/********************* Setup **************************/
void setup(){
    Serial.begin(115200);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    // Create endpoints
    valves[0] = new ZigbeeLight(zones[0].endpoint);
    valves[0]->setManufacturerAndModel("SkynetIrrigation", zones[0].name);
    valves[0]->onLightChange(onZone0);
    Zigbee.addEndpoint(valves[0]);

    valves[1] = new ZigbeeLight(zones[1].endpoint);
    valves[1]->setManufacturerAndModel("SkynetIrrigation", zones[1].name);
    valves[1]->onLightChange(onZone1);
    Zigbee.addEndpoint(valves[1]);

    valves[2] = new ZigbeeLight(zones[2].endpoint);
    valves[2]->setManufacturerAndModel("SkynetIrrigation", zones[2].name);
    valves[2]->onLightChange(onZone2);
    Zigbee.addEndpoint(valves[2]);

    valves[3] = new ZigbeeLight(zones[3].endpoint);
    valves[3]->setManufacturerAndModel("SkynetIrrigation", zones[3].name);
    valves[3]->onLightChange(onZone3);
    Zigbee.addEndpoint(valves[3]);

    // Start Zigbee
    if(!Zigbee.begin()){
        Serial.println("Zigbee failed to start! Rebooting...");
        ESP.restart();
    }

    while(!Zigbee.connected()) delay(200);
    Serial.println("Connected to Zigbee network!");
}

/********************* Loop **************************/
void loop(){
    unsigned long now = millis();

    // Non-blocking factory reset
    static bool buttonPressed = false;
    static unsigned long buttonPressStart = 0;
    if(digitalRead(BUTTON_PIN)==LOW){
        if(!buttonPressed){
            buttonPressed = true;
            buttonPressStart = now;
        } else if(now - buttonPressStart > 3000){
            Serial.println("Factory reset Zigbee and rebooting...");
            delay(1000);
            Zigbee.factoryReset();
        }
    } else {
        buttonPressed = false;
    }

    // Auto-off timers (millis rollover safe)
    for(uint8_t i=0;i<NUM_ZONES;i++){
        if(valveEndTimes[i]!=0 && (long)(now - valveEndTimes[i]) >= 0){
            byte err = hunter.stopZone(i+1);
            if(err==0){
                valves[i]->setLight(false);
                Serial.printf("Auto-turned off zone %d (%s)\n", i+1, zones[i].name);
            } else {
                Serial.printf("Error auto-stopping zone %d (%s): %s\n",
                              i+1, zones[i].name, hunter.errorHint(err).c_str());
            }
            valveEndTimes[i] = 0;
        }
    }
    
    delay(50); // small delay for loop stability
}
