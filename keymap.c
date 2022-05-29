#include <stdint.h>

#include "tusb.h"

// This is a mapping from USB HID keyboard codes to (mostly) MSX International keyboard matrix.
// Some useful keys (END, PAGEUP and PAGEDOWN) are mapped to MSX keys that do not have a USB
// HID equivalent.
//
// MSX was chosen because, of the 8-bit era keyboard layouts, MSX is very close to that of a
// modern keyboard. In particular, the shifted versions of MSX keys are the same as the shifted
// versions of keys on a modern keyboard. For example, shifted "4" on an MSX keyboard would
// yield a currency symbol, shifted "5" per-cent, and so on.

const uint8_t g_key_map[0x68] = {
  0xFF,
  0xFF,
  0xFF,
  0xFF,
  0x26, // HID_KEY_A                         0x04
  0x27, // HID_KEY_B                         0x05
  0x30, // HID_KEY_C                         0x06
  0x31, // HID_KEY_D                         0x07
  0x32, // HID_KEY_E                         0x08
  0x33, // HID_KEY_F                         0x09
  0x34, // HID_KEY_G                         0x0A
  0x35, // HID_KEY_H                         0x0B
  0x36, // HID_KEY_I                         0x0C
  0x37, // HID_KEY_J                         0x0D
  0x40, // HID_KEY_K                         0x0E
  0x41, // HID_KEY_L                         0x0F
  0x42, // HID_KEY_M                         0x10
  0x43, // HID_KEY_N                         0x11
  0x44, // HID_KEY_O                         0x12
  0x45, // HID_KEY_P                         0x13
  0x46, // HID_KEY_Q                         0x14
  0x47, // HID_KEY_R                         0x15
  0x50, // HID_KEY_S                         0x16
  0x51, // HID_KEY_T                         0x17
  0x52, // HID_KEY_U                         0x18
  0x53, // HID_KEY_V                         0x19
  0x54, // HID_KEY_W                         0x1A
  0x55, // HID_KEY_X                         0x1B
  0x56, // HID_KEY_Y                         0x1C
  0x57, // HID_KEY_Z                         0x1D
  0x01, // HID_KEY_1                         0x1E
  0x02, // HID_KEY_2                         0x1F
  0x03, // HID_KEY_3                         0x20
  0x04, // HID_KEY_4                         0x21
  0x05, // HID_KEY_5                         0x22
  0x06, // HID_KEY_6                         0x23
  0x07, // HID_KEY_7                         0x24
  0x10, // HID_KEY_8                         0x25
  0x11, // HID_KEY_9                         0x26
  0x00, // HID_KEY_0                         0x27
  0x77, // HID_KEY_ENTER                     0x28
  0x72, // HID_KEY_ESCAPE                    0x29
  0x75, // HID_KEY_BACKSPACE                 0x2A
  0x73, // HID_KEY_TAB                       0x2B
  0x80, // HID_KEY_SPACE                     0x2C
  0x12, // HID_KEY_MINUS                     0x2D
  0x13, // HID_KEY_EQUAL                     0x2E
  0x15, // HID_KEY_BRACKET_LEFT              0x2F
  0x16, // HID_KEY_BRACKET_RIGHT             0x30
  0x14, // HID_KEY_BACKSLASH                 0x31
  0xFF, // HID_KEY_EUROPE_1                  0x32
  0x17, // HID_KEY_SEMICOLON                 0x33
  0x20, // HID_KEY_APOSTROPHE                0x34
  0x21, // HID_KEY_GRAVE                     0x35
  0x22, // HID_KEY_COMMA                     0x36
  0x23, // HID_KEY_PERIOD                    0x37
  0x24, // HID_KEY_SLASH                     0x38
  0x63, // HID_KEY_CAPS_LOCK                 0x39
  0x65, // HID_KEY_F1                        0x3A
  0x66, // HID_KEY_F2                        0x3B
  0x67, // HID_KEY_F3                        0x3C
  0x70, // HID_KEY_F4                        0x3D
  0x71, // HID_KEY_F5                        0x3E
  0xFF, // HID_KEY_F6                        0x3F
  0xFF, // HID_KEY_F7                        0x40
  0xFF, // HID_KEY_F8                        0x41
  0xFF, // HID_KEY_F9                        0x42
  0xFF, // HID_KEY_F10                       0x43
  0xFF, // HID_KEY_F11                       0x44
  0xFF, // HID_KEY_F12                       0x45
  0xFF, // HID_KEY_PRINT_SCREEN              0x46
  0xFF, // HID_KEY_SCROLL_LOCK               0x47
  0xFF, // HID_KEY_PAUSE                     0x48
  0x82, // HID_KEY_INSERT                    0x49
  0x81, // HID_KEY_HOME                      0x4A
  0x76, // HID_KEY_PAGE_UP                   0x4B
  0x83, // HID_KEY_DELETE                    0x4C
  0x25, // HID_KEY_END                       0x4D
  0x74, // HID_KEY_PAGE_DOWN                 0x4E
  0x87, // HID_KEY_ARROW_RIGHT               0x4F
  0x84, // HID_KEY_ARROW_LEFT                0x50
  0x86, // HID_KEY_ARROW_DOWN                0x51
  0x85, // HID_KEY_ARROW_UP                  0x52
  0xFF, // HID_KEY_NUM_LOCK                  0x53
  0x92, // HID_KEY_KEYPAD_DIVIDE             0x54
  0x90, // HID_KEY_KEYPAD_MULTIPLY           0x55
  0xA5, // HID_KEY_KEYPAD_SUBTRACT           0x56
  0x91, // HID_KEY_KEYPAD_ADD                0x57
  0x77, // HID_KEY_KEYPAD_ENTER              0x58
  0x94, // HID_KEY_KEYPAD_1                  0x59
  0x95, // HID_KEY_KEYPAD_2                  0x5A
  0x96, // HID_KEY_KEYPAD_3                  0x5B
  0x97, // HID_KEY_KEYPAD_4                  0x5C
  0xA0, // HID_KEY_KEYPAD_5                  0x5D
  0xA1, // HID_KEY_KEYPAD_6                  0x5E
  0xA2, // HID_KEY_KEYPAD_7                  0x5F
  0xA3, // HID_KEY_KEYPAD_8                  0x60
  0xA4, // HID_KEY_KEYPAD_9                  0x61
  0x93, // HID_KEY_KEYPAD_0                  0x62
  0xA7, // HID_KEY_KEYPAD_DECIMAL            0x63
  0xFF, // HID_KEY_EUROPE_2                  0x64
  0xFF, // HID_KEY_APPLICATION               0x65
  0xFF, // HID_KEY_POWER                     0x66
  0x13, // HID_KEY_KEYPAD_EQUAL              0x67
};

void MapModifierKeys(uint8_t* rows, int modifiers) {
  if (modifiers & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) {
    rows[6] |= 0x1;
  }

  if (modifiers & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) {
    rows[6] |= 0x2;
  }

  // ALT to MSX GRAPH
  if (modifiers & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT)) {
    rows[6] |= 0x4;
  }

  // Windows to MSX CODE
  if (modifiers & (KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_RIGHTGUI)) {
    rows[6] |= 0x10;
  }
}
