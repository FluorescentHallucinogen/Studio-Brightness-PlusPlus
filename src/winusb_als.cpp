//----------------  winusb_als.cpp  ----------------
#include "winusb_als.h"
#include "Log.h"

#define _WIN32_DCOM
#include <initguid.h>
#include <devpropdef.h>
#include <winusb.h>
#include <setupapi.h>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <cstring>

#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "setupapi.lib")

// Must match AppleAlsWinUsb.inf -> DeviceInterfaceGUIDs.
// {B4A9F2C1-0E7D-49A8-9C33-6D5E8B21A7F0}
static const GUID GUID_ALS_WINUSB =
    {0xB4A9F2C1, 0x0E7D, 0x49A8, {0x9C, 0x33, 0x6D, 0x5E, 0x8B, 0x21, 0xA7, 0xF0}};

// DEVPKEY_Device_ContainerId : {8c7ed206-3f8a-4827-b3ab-ae9e1faefc6c}, 2
DEFINE_DEVPROPKEY(WU_DEVPKEY_Device_ContainerId,
                  0x8c7ed206, 0x3f8a, 0x4827,
                  0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c, 2);

/* ---- device state (set on init, torn down on shutdown) ---- */
static HANDLE                  g_dev       = INVALID_HANDLE_VALUE;
static WINUSB_INTERFACE_HANDLE g_wusb      = nullptr;
static UCHAR                   g_pipeIn    = 0;      // interrupt IN pipe id (0 = none)
static USHORT                  g_maxPacket = 0;
static GUID                    g_container = {};

static std::thread        g_thread;
static std::atomic<bool>  g_stop{false};
static std::atomic<bool>  g_luxValid{false};
static std::atomic<float> g_lux{0.f};

/*++
    Turn the sensor on.

    This is a standard HID sensor: its feature report 1 carries
        [0] report ID
        [1] Property: Reporting State  (usage 0x0316; 1 = NoEvents, 2 = AllEvents)
        [2..3] Property: Report Interval (usage 0x030E, ms, 200..10000)
        [6] Property: Power State      (usage 0x0319; 2 = D0 full power)
    and it powers up in "No Events", so it sends nothing until a host asks it to report.
    The Windows sensor stack and macOS do this automatically; WinUSB does not — it only
    opens the pipe. Without this the interrupt endpoint stays silent forever and every
    ReadPipe returns ERROR_SEM_TIMEOUT (121).

    Read-modify-write rather than building the report by hand, so we don't have to know
    the full field layout.
--*/
static void AlsEnableReporting(WINUSB_INTERFACE_HANDLE h, UCHAR ifaceNum) {
    std::vector<uint8_t> buf(64, 0);
    WINUSB_SETUP_PACKET  sp{};
    ULONG                got = 0;

    sp.RequestType = 0xA1;                    // IN | Class | Interface
    sp.Request     = 0x01;                    // GET_REPORT
    sp.Value       = (USHORT)(0x0300 | 0x01); // Feature, report ID 1
    sp.Index       = ifaceNum;
    sp.Length      = (USHORT)buf.size();

    if (!WinUsb_ControlTransfer(h, sp, buf.data(), (ULONG)buf.size(), &got, nullptr) || got < 2) {
        Log::Warn(L"winusb-als: GET_REPORT(feature 1) failed (gle=%lu got=%lu)", GetLastError(), got);
        return;
    }
    Log::Info(L"winusb-als: feature1 before: len=%lu state=%u interval=%u powerState=%u",
              got, buf[1],
              (got > 3) ? (unsigned)(buf[2] | (buf[3] << 8)) : 0u,
              (got > 6) ? buf[6] : 0u);

    buf[1] = 2;                                  // Reporting State = All Events
    if (got > 3) { buf[2] = 0xF4; buf[3] = 0x01; } // Report Interval = 500 ms
    if (got > 6) { buf[6] = 2; }                 // Power State = D0 (full power)

    sp.RequestType = 0x21;                    // OUT | Class | Interface
    sp.Request     = 0x09;                    // SET_REPORT
    sp.Value       = (USHORT)(0x0300 | 0x01);
    sp.Index       = ifaceNum;
    sp.Length      = (USHORT)got;

    ULONG sent = 0;
    if (WinUsb_ControlTransfer(h, sp, buf.data(), got, &sent, nullptr))
        Log::Info(L"winusb-als: reporting enabled (SET_REPORT feature 1, %lu bytes)", sent);
    else
        Log::Warn(L"winusb-als: SET_REPORT(feature 1) failed (gle=%lu)", GetLastError());
}

static void closeDev() {
	if (g_wusb) { WinUsb_Free(g_wusb); g_wusb = nullptr; }
	if (g_dev != INVALID_HANDLE_VALUE) { CloseHandle(g_dev); g_dev = INVALID_HANDLE_VALUE; }
}

static GUID containerOf(HDEVINFO set, PSP_DEVINFO_DATA di) {
	GUID cid = {}; DEVPROPTYPE t = 0; DWORD sz = 0;
	SetupDiGetDevicePropertyW(set, di, &WU_DEVPKEY_Device_ContainerId, &t, nullptr, 0, &sz, 0);
	if (sz == sizeof(GUID))
		SetupDiGetDevicePropertyW(set, di, &WU_DEVPKEY_Device_ContainerId, &t, (PBYTE)&cid, sizeof(GUID), nullptr, 0);
	return cid;
}

/* ---- background reader: blocks on ReadPipe, publishes lux via atomics ----
   Report layout (Studio Display XDR MI_08, decoded from a bright/dark capture):
   illuminance = uint32 LE at byte offset 2. It is monotonic with room light
   (~3000 dark .. ~21000 bright) and lives at the same offset in both report IDs
   (0x01 = low/mid range, 0x02 = bright range). Bytes 6-9 move inversely and are
   the sensor's gain/integration, not lux. */
static void readLoop() {
	std::vector<uint8_t> lastReport;
	ULONGLONG lastLogTick = 0;
	ULONGLONG lastErrTick = 0;
	ULONG cap = g_maxPacket ? g_maxPacket : 64;

	while (!g_stop.load(std::memory_order_relaxed)) {
		std::vector<uint8_t> buf(cap, 0);
		ULONG got = 0;
		BOOL  ok  = WinUsb_ReadPipe(g_wusb, g_pipeIn, buf.data(), cap, &got, nullptr);

		if (!ok || got == 0) {
			DWORD     gle = ok ? 0 : GetLastError();
			ULONGLONG now = GetTickCount64();

			// Rate-limited so a silent sensor doesn't flood the log, but we can still tell
			// "device sent nothing" (gle=121 ERROR_SEM_TIMEOUT) from a real pipe failure.
			if (now - lastErrTick >= 5000) {
				Log::Info(L"winusb-als: no data (ReadPipe ok=%d gle=%lu got=%lu)",
				          ok ? 1 : 0, gle, got);
				lastErrTick = now;
			}
			// Anything other than a plain timeout: try to recover a halted pipe.
			if (!ok && gle != ERROR_SEM_TIMEOUT)
				WinUsb_ResetPipe(g_wusb, g_pipeIn);
			continue;
		}
		buf.resize(got);

		if (got >= 6) {
			uint32_t v = (uint32_t)buf[2] | ((uint32_t)buf[3] << 8) |
			             ((uint32_t)buf[4] << 16) | ((uint32_t)buf[5] << 24);
			g_lux.store((float)v, std::memory_order_relaxed);
			g_luxValid.store(true, std::memory_order_relaxed);
		}

		ULONGLONG now = GetTickCount64();
		if (buf != lastReport && now - lastLogTick >= 2000) {
			Log::Info(L"winusb-als: illuminance=%.0f", g_lux.load());
			lastReport = buf;
			lastLogTick = now;
		}
	}
}

void winusb_als_init() {
	HDEVINFO set = SetupDiGetClassDevsW(&GUID_ALS_WINUSB, nullptr, nullptr,
	                                    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set == INVALID_HANDLE_VALUE) { Log::Warn(L"winusb-als: SetupDiGetClassDevs failed (%lu)", GetLastError()); return; }

	SP_DEVICE_INTERFACE_DATA ifd{sizeof(ifd)};
	std::wstring path;
	for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_ALS_WINUSB, i, &ifd); ++i) {
		DWORD need = 0;
		SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
		if (!need) continue;
		std::vector<BYTE> buf(need);
		auto det = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
		det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
		SP_DEVINFO_DATA di{sizeof(di)};
		if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, &di)) {
			path = det->DevicePath;
			g_container = containerOf(set, &di);
			break;
		}
	}
	SetupDiDestroyDeviceInfoList(set);

	if (path.empty()) { Log::Info(L"winusb-als: no WinUSB ALS interface present (install AppleAlsWinUsb.inf)"); return; }

	// WinUSB requires the handle be opened for overlapped I/O.
	g_dev = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
	                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
	                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
	if (g_dev == INVALID_HANDLE_VALUE) { Log::Warn(L"winusb-als: CreateFile failed (%lu)", GetLastError()); return; }
	if (!WinUsb_Initialize(g_dev, &g_wusb)) { Log::Warn(L"winusb-als: WinUsb_Initialize failed (%lu)", GetLastError()); closeDev(); return; }

	USB_INTERFACE_DESCRIPTOR id{};
	if (!WinUsb_QueryInterfaceSettings(g_wusb, 0, &id)) { Log::Warn(L"winusb-als: QueryInterfaceSettings failed (%lu)", GetLastError()); closeDev(); return; }

	// Keep this: it is the only way to tell whether the device is still presenting the
	// same interface/endpoint as in a known-good run (iface #8 class=0x03 endpoints=1,
	// pipe 0x8A interrupt maxPacket=42 interval=7).
	Log::Info(L"winusb-als: interface #%u class=0x%02X alt=%u endpoints=%u",
	          id.bInterfaceNumber, id.bInterfaceClass, id.bAlternateSetting, id.bNumEndpoints);

	for (UCHAR e = 0; e < id.bNumEndpoints; ++e) {
		WINUSB_PIPE_INFORMATION pipe{};
		if (!WinUsb_QueryPipe(g_wusb, 0, e, &pipe)) continue;
		Log::Info(L"winusb-als:   pipe[%u] id=0x%02X type=%d maxPacket=%u interval=%u",
		          e, pipe.PipeId, (int)pipe.PipeType, pipe.MaximumPacketSize, pipe.Interval);
		if (pipe.PipeType == UsbdPipeTypeInterrupt && (pipe.PipeId & 0x80) && !g_pipeIn) {
			g_pipeIn    = pipe.PipeId;
			g_maxPacket = pipe.MaximumPacketSize;
		}
	}
	// NOT diagnostic — do not remove. Fetching the HID report descriptor over the control
	// pipe is what primes the sensor. WinUSB performs no HID enumeration of its own, and
	// without this request the firmware never starts pushing reports on the interrupt
	// endpoint (dropping it during a cleanup silently killed the stream: "ready" was
	// logged but no illuminance ever arrived).
	{
		WINUSB_SETUP_PACKET sp{};
		sp.RequestType = 0x81;    // IN | Standard | Interface
		sp.Request     = 0x06;    // GET_DESCRIPTOR
		sp.Value       = 0x2200;  // HID report descriptor
		sp.Index       = id.bInterfaceNumber;
		sp.Length      = 256;
		std::vector<uint8_t> rd(256, 0);
		ULONG got = 0;
		if (WinUsb_ControlTransfer(g_wusb, sp, rd.data(), (ULONG)rd.size(), &got, nullptr))
			Log::Info(L"winusb-als: sensor primed (report descriptor %lu bytes)", got);
		else
			Log::Warn(L"winusb-als: priming control transfer failed (gle=%lu)", GetLastError());
	}

	// Ask the sensor to actually start reporting (it powers up in "No Events").
	AlsEnableReporting(g_wusb, id.bInterfaceNumber);

	if (!g_pipeIn) { Log::Warn(L"winusb-als: no interrupt-IN pipe found; not streaming"); closeDev(); return; }

	// Pipe hygiene: a halted/stalled interrupt pipe left over from a previous session makes
	// every ReadPipe fail forever. Let WinUSB clear stalls automatically (OFF by default)
	// and reset the pipe once up front.
	UCHAR autoClear = TRUE;
	WinUsb_SetPipePolicy(g_wusb, g_pipeIn, AUTO_CLEAR_STALL, sizeof(autoClear), &autoClear);
	if (!WinUsb_ResetPipe(g_wusb, g_pipeIn))
		Log::Warn(L"winusb-als: ResetPipe failed (gle=%lu)", GetLastError());

	ULONG timeout = 250; // ms; so the read loop can observe g_stop between reports
	WinUsb_SetPipePolicy(g_wusb, g_pipeIn, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);

	g_stop.store(false);
	g_thread = std::thread(readLoop);
	Log::Info(L"winusb-als: ready — reading XDR ambient light sensor over WinUSB");
}

void winusb_als_tick() {
	// No-op: the WinUSB endpoint is read on its own background thread (readLoop).
}

void winusb_als_shutdown() {
	g_stop.store(true);
	if (g_wusb && g_pipeIn) WinUsb_AbortPipe(g_wusb, g_pipeIn); // unblock ReadPipe
	if (g_thread.joinable()) g_thread.join();
	g_luxValid.store(false);
	g_pipeIn = 0;
	closeDev();
}

bool winusb_als_get_lux(const GUID *containerId, float *lux) {
	if (!lux || !g_luxValid.load(std::memory_order_relaxed)) return false;
	if (containerId) {
		static const GUID zero = {};
		if (memcmp(containerId, &zero, sizeof(GUID)) != 0 &&
		    memcmp(containerId, &g_container, sizeof(GUID)) != 0)
			return false;     // asked for a specific display that isn't ours
	}
	*lux = g_lux.load(std::memory_order_relaxed);
	return true;
}
