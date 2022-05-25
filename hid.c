#include "tusb.h"

#define MAX_REPORT  4

static struct
{
  uint8_t report_count;
  tuh_hid_report_info_t report_info[MAX_REPORT];
} g_hid_info[CFG_TUH_HID];

static void ProcessKeyboardReport(hid_keyboard_report_t const *report)
{
    (void) report;
}

static void ProcessMouseReport(hid_mouse_report_t const * report)
{
    (void) report;
}

static void ProcessGenericReport(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
  (void) dev_addr;

  uint8_t const rpt_count = g_hid_info[instance].report_count;
  tuh_hid_report_info_t* rpt_info_arr = g_hid_info[instance].report_info;
  tuh_hid_report_info_t* rpt_info = NULL;

  if ( rpt_count == 1 && rpt_info_arr[0].report_id == 0)
  {
    // Simple report without report ID as 1st byte
    rpt_info = &rpt_info_arr[0];
  }else
  {
    // Composite report, 1st byte is report ID, data starts from 2nd byte
    uint8_t const rpt_id = report[0];

    // Find report id in the arrray
    for(uint8_t i=0; i<rpt_count; i++)
    {
      if (rpt_id == rpt_info_arr[i].report_id )
      {
        rpt_info = &rpt_info_arr[i];
        break;
      }
    }

    report++;
    len--;
  }

  if (!rpt_info)
  {
    return;
  }

  // For complete list of Usage Page & Usage checkout src/class/hid/hid.h. For examples:
  // - Keyboard                     : Desktop, Keyboard
  // - Mouse                        : Desktop, Mouse
  // - Gamepad                      : Desktop, Gamepad
  // - Consumer Control (Media Key) : Consumer, Consumer Control
  // - System Control (Power key)   : Desktop, System Control
  // - Generic (vendor)             : 0xFFxx, xx
  if ( rpt_info->usage_page == HID_USAGE_PAGE_DESKTOP )
  {
    switch (rpt_info->usage)
    {
      case HID_USAGE_DESKTOP_KEYBOARD:
        // Assume keyboard follow boot report layout
        ProcessKeyboardReport( (hid_keyboard_report_t const*) report );
        break;

      case HID_USAGE_DESKTOP_MOUSE:
        // Assume mouse follow boot report layout
        ProcessMouseReport( (hid_mouse_report_t const*) report );
        break;

      default:
        break;
    }
  }
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len)
{
  // Interface protocol (hid_interface_protocol_enum_t)
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  // By default host stack will use activate boot protocol on supported interface.
  // Therefore for this simple example, we only need to parse generic report descriptor (with built-in parser)
  if ( itf_protocol == HID_ITF_PROTOCOL_NONE )
  {
    g_hid_info[instance].report_count = tuh_hid_parse_report_descriptor(g_hid_info[instance].report_info, MAX_REPORT, desc_report, desc_len);
  }

  // request to receive report
  // tuh_hid_report_received_cb() will be invoked when report is available
  if ( !tuh_hid_receive_report(dev_addr, instance) )
  {
    assert(false);
  }
}

// Invoked when device with hid interface is un-mounted
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len)
{
  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  switch (itf_protocol)
  {
    case HID_ITF_PROTOCOL_KEYBOARD:
      ProcessKeyboardReport( (hid_keyboard_report_t const*) report );
      break;

    case HID_ITF_PROTOCOL_MOUSE:
      ProcessMouseReport( (hid_mouse_report_t const*) report );
      break;

    default:
      // Generic report requires matching ReportID and contents with previous parsed report info
      ProcessGenericReport(dev_addr, instance, report, len);
      break;
  }

  // continue to request to receive report
  if ( !tuh_hid_receive_report(dev_addr, instance) )
  {
    assert(false);
  }
}
