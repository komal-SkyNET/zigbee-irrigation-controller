# Hunter X2 Controller

## Multimeter voltage readings

- AC1 & AC2: ~27.5V
- AC2 & REM: 5 VDC
- AC1 & REM: 17 VAC 
- AC2 & COMMON: 27.5 VAC
- AC1 & COMMON: 0 VAC


After using 24VAC as floating power supply's GND - i.e, AC2 connected to RED of charger cable

- GND pin on ESP32 & AC2: 5VDC (similar to REM vs AC2)
- Shows that ESP signal logic is raised to right potential for Hunter REM to understand

