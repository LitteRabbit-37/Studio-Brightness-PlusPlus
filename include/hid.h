#ifndef HID_INCLUDED
#define HID_INCLUDED

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int hid_init ();
void hid_deinit ();
int hid_getBrightness (ULONG *val);
int hid_setBrightness (ULONG val);
int hid_getBrightnessRange(ULONG* minVal, ULONG* maxVal);


#endif
