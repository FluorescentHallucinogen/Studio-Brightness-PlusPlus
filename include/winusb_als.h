//----------------  winusb_als.h  ----------------
// Ambient light sensor for the Studio Display XDR read over WinUSB.
//
// On the XDR, MI_08 is a vendor-class USB interface (not HID), so hidusb/hidclass
// never bind it. After AppleAlsWinUsb.inf binds winusb.sys to USB\...&MI_08, this
// module opens that interface, reads the interrupt-IN endpoint directly, and parses
// the illuminance value (macOS shows it as a HID AmbientLight sensor, value 0..20000).
#ifndef WINUSB_ALS_INCLUDED
#define WINUSB_ALS_INCLUDED

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

void winusb_als_init();       // open + enumerate pipes (UI thread, once)
void winusb_als_tick();       // poll the endpoint from the ~250 ms timer
void winusb_als_shutdown();

// Latest ambient-light value. If `containerId` is non-null it must match the WinUSB
// device's container; pass null for the master value. Returns false until a value is
// available. Thread-safe (called from the auto-brightness worker thread).
bool winusb_als_get_lux(const GUID *containerId, float *lux);

#endif // WINUSB_ALS_INCLUDED
