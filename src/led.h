#ifndef LED_H
#define LED_H

#include <Arduino.h>
#include <ezOutput.h>

class Led {
    private:
        ezOutput _led;
    public:
        Led(byte pin);
        void on();
        void off();
        bool state();
        void toggle();
        void flash(unsigned long lowTime, unsigned long highTime, int count=-1);
        void flashFast(int count=-1);
        void flashSlow(int count=-1);
        void loop();
};
#endif    // LED_H

