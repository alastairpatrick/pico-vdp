#ifndef PINS_H
#define PINS_H

// Remember .pio files might have pin numbers hard coded.
#define TX_PIN 0
#define RX_PIN 1
#define SYNC_PINS 2
#define VSYNC_PIN (SYNC_PINS+0)
#define HSYNC_PIN (SYNC_PINS+1)
#define VIDEO_PINS 4
#define RED_PINS (VIDEO_PINS+0)
#define GREEN_PINS (VIDEO_PINS+3)
#define BLUE_PINS (VIDEO_PINS+6)
#define RD_PIN 12
#define PWM_PIN 13
#define DATA_PINS 14
#define SUPPLY_PIN 24
#define LED_PIN 25
#define CS0_PIN 22
#define CS1_PIN 26
#define CS2_PIN 27

#endif  // PINS_H
