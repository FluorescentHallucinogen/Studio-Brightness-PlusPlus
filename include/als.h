//----------------  als.h  ----------------
// Raw-HID ambient light sensor for Apple displays, reimplemented from the path used
// by BootCampService.exe (class HidAlsSensor): open the HID interface, poll
// HidD_GetInputReport, read HID Usage Page 0x20 (Sensors) / Usage 0x04D1 (Illuminance).
//
// This is the same technique as orientation.cpp, and is the source Boot Camp uses for
// its "Automatic Brightness". It is independent of the Windows Sensor API (ISensor),
// which never instantiates for displays whose ALS interface is claimed by Apple's null
// driver or rejected by hidclass.
#ifndef ALS_INCLUDED
#define ALS_INCLUDED

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <vector>
#include <string>
#include <climits>
#include <cstdint>

constexpr USAGE HID_UP_SENSOR_PAGE  = 0x0020; // Sensors
constexpr USAGE HID_USG_ILLUMINANCE = 0x04D1; // Data Field: Illuminance (lux)

struct AlsDevice {
	HANDLE               hDev        = INVALID_HANDLE_VALUE;
	PHIDP_PREPARSED_DATA prep        = nullptr;
	USHORT               inputLen    = 0;
	UCHAR                reportId    = 0;
	USAGE                uIllum      = 0;
	bool                 overlapped  = false;
	GUID                 containerId = {};
	std::wstring         devicePath;
	std::wstring         label;                 // short "pid_xxxx&mi_xx" tag for logging
	LONG                 lastRaw     = LONG_MIN;

	AlsDevice() = default;
	~AlsDevice() { close(); }
	AlsDevice(const AlsDevice &)            = delete;
	AlsDevice &operator=(const AlsDevice &) = delete;
	AlsDevice(AlsDevice &&o) noexcept { moveFrom(o); }
	AlsDevice &operator=(AlsDevice &&o) noexcept { if (this != &o) { close(); moveFrom(o); } return *this; }

	bool isOpen() const { return hDev != INVALID_HANDLE_VALUE; }
	void close();
	bool readReport(std::vector<uint8_t> &buf);
	// Reads illuminance. `rawOut` = logical value (as reported); `luxOut` = scaled lux
	// (unit exponent applied when the descriptor declares one). Returns true on a read.
	bool readLux(LONG *rawOut, float *luxOut);

private:
	void moveFrom(AlsDevice &o);
};

/* Discover + open every Apple display interface exposing an Illuminance input.
   Logs every page-0x20 input cap it sees (so a hidden/absent ALS is visible). */
std::vector<AlsDevice> als_enumerate();

/* Watcher: init once (UI thread), tick from a ~250 ms WM_TIMER, shutdown at exit.
   Logs raw lux only when it changes. */
void als_watch_init();
void als_watch_tick();
void als_watch_shutdown();

/* Thread-safe accessor for the auto-brightness path (worker thread).
   If `containerId` is non-null, returns the reading for that display; otherwise the
   first available (master). Returns false if no raw-HID ALS reading is available. */
bool als_get_lux(const GUID *containerId, float *lux);

#endif // ALS_INCLUDED
