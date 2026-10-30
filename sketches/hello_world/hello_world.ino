// Hello World for Inkplate 6, based on the InkplateLibrary example (GPL-3.0).
// The upstream example checks for ARDUINO_ESP32_DEV, but board package 4.x
// defines ARDUINO_INKPLATE6 for the e-radionica board, so the check is widened here.

#if !defined(ARDUINO_INKPLATE6) && !defined(ARDUINO_INKPLATE6V2)
#error "Select 'e-radionica Inkplate 6' or 'Soldered Inkplate 6' as the board."
#endif

#include "Inkplate.h"
Inkplate display(INKPLATE_1BIT);

void setup() {
    display.begin();
    display.clearDisplay();
    display.setCursor(10, 10);
    display.setTextSize(4);
    display.print("Hello World!");
    display.display();
}

void loop() {
}
