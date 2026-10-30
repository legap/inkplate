// Diagnostics for Inkplate 6: reports PSRAM, I2C devices and display init over serial.

#if !defined(ARDUINO_INKPLATE6) && !defined(ARDUINO_INKPLATE6V2)
#error "Select 'e-radionica Inkplate 6' or 'Soldered Inkplate 6' as the board."
#endif

#include "Inkplate.h"
#include <Wire.h>

Inkplate display(INKPLATE_1BIT);

void scanI2C() {
    Serial.println("I2C scan:");
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) Serial.printf("  found 0x%02X\n", addr);
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Inkplate diag ===");
    Serial.printf("Chip: %s rev %d, %d MHz\n", ESP.getChipModel(), ESP.getChipRevision(), ESP.getCpuFreqMHz());
    Serial.printf("psramFound: %d, PSRAM size: %u, free: %u\n", psramFound(), ESP.getPsramSize(), ESP.getFreePsram());
    Serial.printf("Heap free: %u\n", ESP.getFreeHeap());

    Serial.println("display.begin()...");
    display.begin();
    Serial.println("display.begin() returned");

    scanI2C();

    Serial.printf("Temperature: %d C\n", display.readTemperature());
    Serial.printf("Battery: %.2f V\n", display.readBattery());

    display.clearDisplay();
    display.setCursor(10, 10);
    display.setTextSize(4);
    display.print("Hello World!");
    Serial.println("display.display()...");
    display.display();
    Serial.println("done");
}

void loop() {
}
