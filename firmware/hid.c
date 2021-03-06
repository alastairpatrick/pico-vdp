#include "tusb.h"

#include "sys80.h"

#define MAX_REPORT  4
#define NUM_HID_CODES 128

extern const uint8_t g_key_map[0x68];
static const char g_ascii_map[NUM_HID_CODES][2] =  { HID_KEYCODE_TO_ASCII };
static hid_keyboard_report_t g_last_kbd_report;
static int g_led_state;
static int g_kbd_dev_addr, g_kbd_instance;

void MapModifierKeys(uint8_t* rows, int modifiers);

static struct {
  uint8_t report_count;
  tuh_hid_report_info_t report_info[MAX_REPORT];
} g_hid_info[CFG_TUH_HID];

static void ProcessKeyboardReport(hid_keyboard_report_t const *report) {
  uint8_t rows[KEYBOARD_ROWS] = {0};
  for (int i = 0; i < count_of(report->keycode); ++i) {
    // Map to kayboard matrix.
    int hid_code = report->keycode[i];
    if (hid_code >= count_of(g_key_map)) {
      continue;
    }

    int mapped_code = g_key_map[hid_code];
    int col = mapped_code & 0xF;
    int row = mapped_code >> 4;
    if (row >= KEYBOARD_ROWS) {
      continue;
    }

    rows[row] |= 1 << col;
  }

  MapModifierKeys(rows, report->modifier);

  for (int i = 0; i < KEYBOARD_ROWS; ++i) {
    g_sys80_regs.kbd_rows[i] = rows[i];
  }

  g_last_kbd_report = *report;
}

static void ProcessMouseReport(hid_mouse_report_t const * report) {
  g_sys80_regs.mouse_buttons = report->buttons;
  g_sys80_regs.mouse_x += report->x;
  g_sys80_regs.mouse_y += report->y;
}

static void ProcessGenericReport(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len) {
  (void) dev_addr;

  uint8_t const rpt_count = g_hid_info[instance].report_count;
  tuh_hid_report_info_t* rpt_info_arr = g_hid_info[instance].report_info;
  tuh_hid_report_info_t* rpt_info = NULL;

  if (rpt_count == 1 && rpt_info_arr[0].report_id == 0) {
    // Simple report without report ID as 1st byte
    rpt_info = &rpt_info_arr[0];
  } else {
    // Composite report, 1st byte is report ID, data starts from 2nd byte
    uint8_t const rpt_id = report[0];

    // Find report id in the arrray
    for (uint8_t i=0; i<rpt_count; i++) {
      if (rpt_id == rpt_info_arr[i].report_id) {
        rpt_info = &rpt_info_arr[i];
        break;
      }
    }

    report++;
    len--;
  }

  if (!rpt_info) {
    return;
  }

  // For complete list of Usage Page & Usage checkout src/class/hid/hid.h. For examples:
  // - Keyboard                     : Desktop, Keyboard
  // - Mouse                        : Desktop, Mouse
  // - Gamepad                      : Desktop, Gamepad
  // - Consumer Control (Media Key) : Consumer, Consumer Control
  // - System Control (Power key)   : Desktop, System Control
  // - Generic (vendor)             : 0xFFxx, xx
  if (rpt_info->usage_page == HID_USAGE_PAGE_DESKTOP) {
    switch (rpt_info->usage) {
      case HID_USAGE_DESKTOP_KEYBOARD:
        // Assume keyboard follow boot report layout
        ProcessKeyboardReport((hid_keyboard_report_t const*) report);
        break;

      case HID_USAGE_DESKTOP_MOUSE:
        // Assume mouse follow boot report layout
        ProcessMouseReport((hid_mouse_report_t const*) report);
        break;

      default:
        break;
    }
  }
}

static void UpdateLEDs() {
  int leds = g_sys80_regs.ay[0][8].value;
  uint8_t led_state = 0;
  if (leds & LED_CAPS_LOCK_MASK) {
    led_state |= KEYBOARD_LED_CAPSLOCK;
  }
  if (leds & LED_NUM_LOCK_MASK) {
    led_state |= KEYBOARD_LED_NUMLOCK;
  }
  if (leds & LED_SCROLL_LOCK_MASK) {
    led_state |= KEYBOARD_LED_SCROLLLOCK;
  }

  if (g_kbd_dev_addr && led_state != g_led_state) {
    g_led_state = led_state;
    tuh_hid_set_report(g_kbd_dev_addr, g_kbd_instance, 0, HID_REPORT_TYPE_OUTPUT, &led_state, sizeof(led_state));
  }
}

void UpdateKeyboard() {
  UpdateLEDs();
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len) {
  // Interface protocol (hid_interface_protocol_enum_t)
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  // By default host stack will use activate boot protocol on supported interface.
  // Therefore for this simple example, we only need to parse generic report descriptor (with built-in parser)
  if (itf_protocol == HID_ITF_PROTOCOL_NONE) {
    g_hid_info[instance].report_count = tuh_hid_parse_report_descriptor(g_hid_info[instance].report_info, MAX_REPORT, desc_report, desc_len);
  }

  // request to receive report
  // tuh_hid_report_received_cb() will be invoked when report is available
  if (!tuh_hid_receive_report(dev_addr, instance)) {
    assert(false);
  }

  if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
    g_kbd_dev_addr = dev_addr;
    g_kbd_instance = instance;
    g_led_state = 0;
  }
}

// Invoked when device with hid interface is un-mounted
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
  if (g_kbd_dev_addr == dev_addr && g_kbd_instance == instance) {
    g_kbd_dev_addr = g_kbd_instance = 0;
    g_led_state = 0;
  }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len) {
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  switch (itf_protocol) {
    case HID_ITF_PROTOCOL_KEYBOARD:
      ProcessKeyboardReport((hid_keyboard_report_t const*) report);
      break;

    case HID_ITF_PROTOCOL_MOUSE:
      ProcessMouseReport((hid_mouse_report_t const*) report);
      break;

    default:
      // Generic report requires matching ReportID and contents with previous parsed report info
      ProcessGenericReport(dev_addr, instance, report, len);
      break;
  }

  // continue to request to receive report
  if (!tuh_hid_receive_report(dev_addr, instance)) {
    assert(false);
  }
}
