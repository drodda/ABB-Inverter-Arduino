#include "led.h"


Led::Led(byte pin): _led(pin) {}

void Led::on() {
    _led.low();
}

void Led::off() {
    _led.high();
}

bool Led::state() {
    return !(_led.getState());
}

void Led::toggle() {
    _led.toggle();
}

void Led::flash(unsigned long lowTime, unsigned long highTime, int count){
    off();
    if(count != -1) {
        count = 2 * count;
    }
    _led.blink(lowTime, highTime, 0, count);
}

void Led::flashFast(int count) {
    flash(100, 100, count);
}

void Led::flashSlow(int count){
    flash(500, 500, count);
}

void Led::loop() {
    _led.loop();
}

