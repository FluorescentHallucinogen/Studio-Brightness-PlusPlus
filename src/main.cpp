#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <dbt.h>
#include <commctrl.h>
#include <initguid.h>
#include <sensorsapi.h>
#include <sensors.h>
#include <devpkey.h>
#include <setupapi.h>
#include <shlwapi.h>
#include <portabledevicetypes.h>
#include <wrl/client.h>
#include <thread>
#include <chrono>
#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <set>
#include <vector>
#include <cwchar>
#include <atomic>
#include <mutex>
#include <gdiplus.h>

#include "hid.h"
#include "resource.h"
#include "Settings.h"
#include "OSDWindow.h"
#include "TrayPopup.h"
#include "Log.h"
#include "LogWindow.h"
#include "version.h"
#include "Updater.h"
#include "HdrMonitor.h"
#include "NvHdr.h"
#include "PresetConfirm.h"
#include "orientation.h"
#include "als.h"
#include "Dpi.h"

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "sensorsapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "gdiplus.lib")

using Microsoft::WRL::ComPtr;
using namespace Gdiplus;

/* ---------- globals ---------- */
static HINSTANCE  g_hInst              = nullptr;
static HWND       g_hMain              = nullptr;
constexpr UINT    WMAPP_NOTIFYCALLBACK = WM_APP + 1;
constexpr wchar_t kWndClass[]          = L"StudioBrightnessClass";

constexpr wchar_t kReleaseUrl[] = L"https://github.com/LitteRabbit-37/Studio-Brightness-PlusPlus/releases";
constexpr wchar_t kAppVersion[] = SBPP_VERSION_STR;

// Update state, filled by the background update-check thread.
constexpr UINT     WMAPP_UPDATE_READY = WM_APP + 2;
constexpr UINT_PTR ID_UPDATE_TIMER    = 0xA001;
constexpr UINT_PTR ID_HDR_TIMER       = 0xA002;
constexpr UINT_PTR ID_REAPPLY_TIMER   = 0xA003;
constexpr UINT_PTR ID_OPTIONS_TIMER   = 0xA004; // Options dialog: keeps its brightness controls current
constexpr UINT_PTR ID_TYPING_TIMER    = 0xA005; // Options dialog: a pause in typing into the percent field
static std::atomic<bool> g_updateAvailable{false};
static std::atomic<bool> g_updateChecking{false};
static std::mutex        g_updateMutex;
static UpdateInfo        g_updateInfo;

// HDR state of Apple displays, tracked live. Brightness control is unavailable while HDR is on.
static std::atomic<bool> g_hdrActive{false};

/* ---------- system tray icon ---------- */
// Re-add the tray icon when the shell (re)creates the taskbar: Explorer restart, or we
// started before the taskbar was ready at boot. Registered in WinMain, handled in WndProc.
static UINT g_wmTaskbarCreated = 0;

/* ---------- multi-display state ---------- */
static std::vector<DisplayDevice> g_displays;
static std::mutex                 g_displayMutex;
// Set when an Apple HID interface arrives, so the worker re-scans even while displays are
// already connected: a second display plugged in later, or one whose HID shows up late at boot.
static std::atomic<bool>          g_rescanRequested{false};
// Set by ID_REAPPLY_TIMER once the displays are back on; the worker sends the brightness again.
static std::atomic<bool>          g_reapplyRequested{false};

/* ---------- Per-display color-preset persistence (HKCU\...\Presets\{ContainerId}) ---------- */
static std::wstring guidToString(const GUID &g) {
	wchar_t buf[64] = {};
	StringFromGUID2(g, buf, 64);
	return buf;
}
// Revert a display's color preset to a previous index, located by ContainerId so it still works
// after the HID re-enumeration a preset switch triggers (the DisplayDevice object gets replaced).
static bool tryRevertPreset(const GUID &cid, int prevIdx) {
	std::lock_guard<std::mutex> lock(g_displayMutex);
	for (auto &dev : g_displays)
		if (memcmp(&dev.containerId, &cid, sizeof(GUID)) == 0 && dev.hPreset != INVALID_HANDLE_VALUE) {
			if (dev.setActivePreset(prevIdx) == 0) {
				Log::Info(L"Color preset reverted to %d on %s", prevIdx, dev.name.c_str());
				return true;
			}
		}
	return false;
}
static void RevertPresetByContainer(const GUID &cid, int prevIdx) {
	if (prevIdx < 0) return;
	Log::Info(L"Reverting color preset to %d", prevIdx);
	if (tryRevertPreset(cid, prevIdx))
		return;
	// The switch re-enumerates the display's HID interface (a disconnect), so the device may not
	// be back in g_displays yet, especially on the early display-dropped revert. Keep trying in
	// the background until the worker has re-added it.
	std::thread([cid, prevIdx]() {
		for (int i = 0; i < 40; ++i) { // ~20 s
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			if (tryRevertPreset(cid, prevIdx))
				return;
		}
		Log::Warn(L"Preset revert gave up: the display did not come back within 20 s");
	}).detach();
}

[[maybe_unused]] static bool loadPresetForContainer(const GUID &cid, int *outIdx) {
	HKEY hKey;
	bool ok = false;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\StudioBrightnessPlusPlus\\Presets", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
		DWORD val = 0, size = sizeof(val), type = 0;
		if (RegQueryValueExW(hKey, guidToString(cid).c_str(), nullptr, &type, (BYTE *)&val, &size) == ERROR_SUCCESS && type == REG_DWORD) {
			if (outIdx) *outIdx = (int)val;
			ok = true;
		}
		RegCloseKey(hKey);
	}
	return ok;
}
[[maybe_unused]] static void savePresetForContainer(const GUID &cid, int idx) {
	HKEY hKey;
	DWORD disp;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\StudioBrightnessPlusPlus\\Presets", 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, &disp) == ERROR_SUCCESS) {
		DWORD val = (DWORD)idx;
		RegSetValueExW(hKey, guidToString(cid).c_str(), 0, REG_DWORD, (BYTE *)&val, sizeof(val));
		RegCloseKey(hKey);
	}
}

// Cache of enumerated presets (so we don't re-write the 0xFF20 cursor on every reconnect) and a
// once-per-run default-reset guard. Switching a calibrated preset re-enumerates the display's HID
// descriptor (a disconnect), so re-applying on each reconnect would loop. Keyed by ContainerId.
static std::map<std::wstring, std::vector<ColorPreset>> g_presetCache;
static std::set<std::wstring>                           g_presetRestored;

// GDI+
static ULONG_PTR gdiplusToken;

// --- Apple-style auto-brightness transition (hysteresis + asymmetric perceptual ramp) ---
// The two ramp durations are settings, edited in Options: g_settings.rampBrightenMs / rampDimMs.
constexpr float  kRelLuxHysteresis = 0.20f;   // re-target only when |dLux|/lastTargetLux >= 20%

static double brightToPerc(ULONG v) { return std::log2((double)v + 1.0); }
static ULONG  percToBright(double l) { return (ULONG)std::llround(std::exp2(l) - 1.0); }
static double nowMs() {
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

static void unregisterHotkeys(HWND h) {
	UnregisterHotKey(h, ID_HOTKEY_UP);
	UnregisterHotKey(h, ID_HOTKEY_DOWN);
}
static bool registerHotkeys(HWND h) {
	unregisterHotkeys(h);
	if (!g_settings.enableCustomHotkeys)
		return true;
	bool ok = true;
	if (g_settings.hkUp.vk)
		ok = ok && RegisterHotKey(h, ID_HOTKEY_UP, g_settings.hkUp.mods, g_settings.hkUp.vk);
	if (g_settings.hkDown.vk)
		ok = ok && RegisterHotKey(h, ID_HOTKEY_DOWN, g_settings.hkDown.mods, g_settings.hkDown.vk);
	return ok;
}

/* ---------- prototypes ---------- */
INT_PTR CALLBACK OptionsDlgProc(HWND, UINT, WPARAM, LPARAM);

/* ---------- systray helpers ---------- */
BOOL AddNotificationIcon(HWND h) {
	NOTIFYICONDATA nid{sizeof(nid)};
	nid.hWnd             = h;
	nid.uID              = 1;
	nid.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE;
	nid.uCallbackMessage = WMAPP_NOTIFYCALLBACK;
	nid.hIcon            = LoadIcon(g_hInst, MAKEINTRESOURCE(IDI_MYICON));
	wcscpy_s(nid.szTip, L"Studio Brightness ++");
	Shell_NotifyIcon(NIM_ADD, &nid);
	nid.uVersion = NOTIFYICON_VERSION_4;
	return Shell_NotifyIcon(NIM_SETVERSION, &nid);
}
BOOL DeleteNotificationIcon() {
	NOTIFYICONDATA nid{sizeof(nid)};
	nid.hWnd = g_hMain;
	nid.uID  = 1;
	return Shell_NotifyIcon(NIM_DELETE, &nid);
}

/* ---------- update helpers ---------- */
static void ShowUpdateBalloon(const wchar_t *title, const wchar_t *text) {
	NOTIFYICONDATA nid{sizeof(nid)};
	nid.hWnd        = g_hMain;
	nid.uID         = 1;
	nid.uFlags      = NIF_INFO;
	nid.dwInfoFlags = NIIF_INFO;
	wcscpy_s(nid.szInfoTitle, title);
	wcscpy_s(nid.szInfo, text);
	Shell_NotifyIcon(NIM_MODIFY, &nid);
}

// Re-read the HDR state of Apple displays into g_hdrActive, logging any transition.
// On the SDR-to-HDR transition, auto-rescue: a reference-mode preset is incompatible with HDR
// (the panel can blank), so force each display back to a main "Apple XDR Display" preset the
// moment HDR turns on. This closes the "pick a reference mode, then enable HDR" trap, and it is
// the same recovery a user would do by hand from a second machine.
static void RefreshHdrState() {
	bool now  = HdrAnyAppleDisplayActive();
	bool prev = g_hdrActive.exchange(now);
	if (now != prev) {
		Log::Info(L"HDR %s", now ? L"enabled (brightness is controlled by Windows)" : L"disabled");
		NvapiLogHdrState(now ? L"HDR on" : L"HDR off");
		HdrLogAppleOutputs(now ? L"HDR on" : L"HDR off");
		if (now) {
			PresetConfirm::Cancel(); // a pending keep/revert prompt is superseded by the rescue
			std::lock_guard<std::mutex> lock(g_displayMutex);
			for (auto &dev : g_displays) {
				if (!dev.hasPresets() || !dev.presetsClassifiable())
					continue;
				const ColorPreset *ap = dev.activePreset();
				if (!ap || ap->isHdrCompatible())
					continue;
				int tgt = dev.firstHdrCompatiblePreset();
				if (tgt >= 0 && tgt != dev.activePresetIndex) {
					Log::Info(L"HDR enabled with \"%s\" active on %s: switching to preset %d for HDR compatibility",
					          ap->name.c_str(), dev.name.c_str(), tgt);
					if (dev.setActivePreset(tgt) != 0)
						Log::Warn(L"HDR rescue preset switch failed on %s", dev.name.c_str());
				}
			}
		}
	}
}

static void StartUpdateCheck(bool manual) {
	bool expected = false;
	if (!g_updateChecking.compare_exchange_strong(expected, true)) return; // a check is already running
	std::thread([manual]() {
		UpdateInfo info = CheckForUpdate(g_settings.updateChannel, kAppVersion);
		{
			std::lock_guard<std::mutex> lk(g_updateMutex);
			g_updateInfo = info;
		}
		g_updateAvailable.store(info.available);
		g_updateChecking.store(false);
		if (g_hMain) PostMessageW(g_hMain, WMAPP_UPDATE_READY, manual ? 1 : 0, 0);
	}).detach();
}

static void StartUpdateInstall() {
	UpdateInfo info;
	{
		std::lock_guard<std::mutex> lk(g_updateMutex);
		info = g_updateInfo;
	}
	if (!info.available) return;
	ShowUpdateBalloon(L"Studio Brightness++", L"Downloading the update...");
	std::thread([info]() {
		bool ok = BeginInstallUpdate(info);
		if (g_hMain) {
			if (ok) PostMessageW(g_hMain, WM_CLOSE, 0, 0);       // exit so the installer can replace files
			else    PostMessageW(g_hMain, WMAPP_UPDATE_READY, 3, 0);
		}
	}).detach();
}

/* ---------- ALS via ISensorEvents ---------- */
class AlsSensorListener : public ISensorEvents {
public:
	AlsSensorListener() : refCount_(1) {}

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
		if (riid == IID_IUnknown || riid == __uuidof(ISensorEvents)) {
			*ppv = static_cast<ISensorEvents *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refCount_); }
	STDMETHODIMP_(ULONG) Release() override {
		ULONG c = InterlockedDecrement(&refCount_);
		if (c == 0) delete this;
		return c;
	}

	// ISensorEvents
	STDMETHODIMP OnStateChanged(ISensor *, SensorState) override { return S_OK; }
	STDMETHODIMP OnDataUpdated(ISensor *, ISensorDataReport *pReport) override {
		if (!pReport) return S_OK;
		PROPVARIANT v;
		PropVariantInit(&v);
		if (SUCCEEDED(pReport->GetSensorValue(SENSOR_DATA_TYPE_LIGHT_LEVEL_LUX, &v))) {
			float lux = 100.f;
			if (v.vt == VT_R4)
				lux = v.fltVal;
			else if (v.vt == VT_R8)
				lux = static_cast<float>(v.dblVal);
			lastLux_.store(lux, std::memory_order_relaxed);
			alive_.store(true, std::memory_order_relaxed);
		}
		PropVariantClear(&v);
		return S_OK;
	}
	STDMETHODIMP OnEvent(ISensor *, REFGUID, IPortableDeviceValues *) override { return S_OK; }
	STDMETHODIMP OnLeave(REFSENSOR_ID) override { return S_OK; }

	float    lux()     const { return lastLux_.load(std::memory_order_relaxed); }
	bool     alive()   const { return alive_.load(std::memory_order_relaxed); }

	GUID     containerId = {};
	ComPtr<ISensor> sensor;

private:
	LONG              refCount_;
	std::atomic<float> lastLux_{100.f};
	std::atomic<bool>  alive_{false};
};

static std::vector<AlsSensorListener *> g_alsListeners;
static std::mutex                       g_alsMutex;
static std::atomic<float>               g_lastKnownLux{100.f}; // last good lux; survives ALS re-init

// Extract ContainerId from a sensor device path by looking up its parent chain.
// The sensor path typically contains the HID instance ID which shares a ContainerId
// with the display's USB composite device.
static GUID getContainerIdFromDevicePath(const std::wstring &devPath) {
	GUID cid = {};

	if (!StrStrIW(devPath.c_str(), L"VID_05AC"))
		return cid;

	// Sensor device path: \\?\HID#VID_05AC&PID_xxxx&MI_08#inst#{guid}\{sub}
	// Instance ID:         HID\VID_05AC&PID_xxxx&MI_08\inst
	// Match by converting instId separators (\->#) and checking if it appears in devPath.
	static const GUID GUID_DEVCLASS_SENSOR = {0x5175D334, 0xC371, 0x4806,
	                                           {0xB3, 0xBA, 0x71, 0xFD, 0x53, 0xC9, 0x25, 0x8D}};

	HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_SENSOR, nullptr, 0, DIGCF_PRESENT);
	if (set == INVALID_HANDLE_VALUE) return cid;

	SP_DEVINFO_DATA devInfo{sizeof(devInfo)};
	for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &devInfo); ++i) {
		wchar_t instId[512] = {};
		if (!SetupDiGetDeviceInstanceIdW(set, &devInfo, instId, 512, nullptr))
			continue;

		// Convert backslashes to # to match device path format
		std::wstring normalized = instId;
		for (auto &ch : normalized)
			if (ch == L'\\') ch = L'#';

		// Check if this instance ID matches the sensor device path
		if (!StrStrIW(devPath.c_str(), normalized.c_str()))
			continue;

		DWORD type = 0, size = 0;
		SetupDiGetDevicePropertyW(set, &devInfo, &DEVPKEY_Device_ContainerId,
		                          &type, nullptr, 0, &size, 0);
		if (size == sizeof(GUID)) {
			SetupDiGetDevicePropertyW(set, &devInfo, &DEVPKEY_Device_ContainerId,
			                          &type, (PBYTE)&cid, sizeof(GUID), nullptr, 0);
			break;
		}
	}

	SetupDiDestroyDeviceInfoList(set);
	return cid;
}

static void initAlsSensors() {
	ComPtr<ISensorManager> mgr;
	if (FAILED(CoCreateInstance(CLSID_SensorManager, nullptr, CLSCTX_INPROC_SERVER,
	                            IID_PPV_ARGS(&mgr)))) {
		Log::Warn(L"ALS: Failed to create SensorManager");
		return;
	}

	ComPtr<ISensorCollection> col;
	if (FAILED(mgr->GetSensorsByType(SENSOR_TYPE_AMBIENT_LIGHT, &col))) {
		Log::Info(L"ALS: no ambient light sensor via the Windows Sensor API");
		return;
	}

	ULONG count = 0;
	col->GetCount(&count);
	Log::Info(L"ALS: %lu ambient light sensor(s) via the Windows Sensor API", count);

	std::lock_guard<std::mutex> lock(g_alsMutex);

	for (ULONG i = 0; i < count; ++i) {
		ComPtr<ISensor> sensor;
		if (FAILED(col->GetAt(i, &sensor)))
			continue;

		// Get sensor ID for logging
		SENSOR_ID sid = {};
		sensor->GetID(&sid);

		// Get device path for ContainerId correlation
		PROPVARIANT pvPath;
		PropVariantInit(&pvPath);

		std::wstring sensorInfo = L"(unknown)";
		if (SUCCEEDED(sensor->GetProperty(SENSOR_PROPERTY_FRIENDLY_NAME, &pvPath))) {
			if (pvPath.vt == VT_LPWSTR && pvPath.pwszVal)
				sensorInfo = pvPath.pwszVal;
		}
		PropVariantClear(&pvPath);

		// Try to get device path for ContainerId matching
		GUID cid = {};
		PropVariantInit(&pvPath);
		if (SUCCEEDED(sensor->GetProperty(SENSOR_PROPERTY_DEVICE_PATH, &pvPath))) {
			if (pvPath.vt == VT_LPWSTR && pvPath.pwszVal) {
				std::wstring devPath = pvPath.pwszVal;
				cid = getContainerIdFromDevicePath(devPath);
				Log::Info(L"ALS: Sensor %lu device path: %s", i, devPath.c_str());
			}
		}
		PropVariantClear(&pvPath);

		auto *listener       = new AlsSensorListener();
		listener->sensor     = sensor;
		listener->containerId = cid;

		HRESULT hr = sensor->SetEventSink(listener);
		if (SUCCEEDED(hr)) {
			// Request ~500ms update interval
			ComPtr<IPortableDeviceValues> params;
			if (SUCCEEDED(CoCreateInstance(CLSID_PortableDeviceValues, nullptr,
			                               CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&params)))) {
				params->SetUnsignedIntegerValue(SENSOR_PROPERTY_CURRENT_REPORT_INTERVAL, 500);
				sensor->SetProperties(params.Get(), nullptr);
			}
			Log::Info(L"ALS: Registered listener for sensor %lu: %s", i, sensorInfo.c_str());
			g_alsListeners.push_back(listener);
		} else {
			Log::Warn(L"ALS: SetEventSink failed for sensor %lu (0x%08X)", i, hr);
			listener->Release();
		}
	}
}

static void cleanupAlsSensors() {
	std::lock_guard<std::mutex> lock(g_alsMutex);
	for (auto *l : g_alsListeners) {
		if (l->sensor)
			l->sensor->SetEventSink(nullptr);
		l->Release();
	}
	g_alsListeners.clear();
}

// Get lux from the best available sensor for a given display.
// If the display has an ALS (matched by ContainerId), use it.
// Otherwise use the master sensor (index 0 or user-selected).
// `live`, when given, says whether the value is a sensor's current reading or only the last
// one that was known.
static float getAmbientLux(const DisplayDevice &dev, bool *live = nullptr) {
	std::lock_guard<std::mutex> lock(g_alsMutex);
	if (live) *live = true;

	// Prefer the raw-HID Apple ALS (the same source Boot Camp uses) matched to this display.
	{
		GUID zeroG = {};
		float lx;
		if (memcmp(&dev.containerId, &zeroG, sizeof(GUID)) != 0 && als_get_lux(&dev.containerId, &lx)) {
			g_lastKnownLux.store(lx, std::memory_order_relaxed);
			return lx;
		}
	}

	// Try to find a sensor matching this display's ContainerId
	GUID zero = {};
	if (memcmp(&dev.containerId, &zero, sizeof(GUID)) != 0) {
		for (auto *l : g_alsListeners) {
			if (memcmp(&l->containerId, &dev.containerId, sizeof(GUID)) == 0 && l->alive()) {
				float lx = l->lux();
				g_lastKnownLux.store(lx, std::memory_order_relaxed);
				return lx;
			}
		}
	}

	// Raw-HID master (any display's ALS) before the Sensor-API master.
	{
		float lx;
		if (als_get_lux(nullptr, &lx)) {
			g_lastKnownLux.store(lx, std::memory_order_relaxed);
			return lx;
		}
	}

	// Fallback: use first alive sensor (master)
	for (auto *l : g_alsListeners) {
		if (l->alive()) {
			float lx = l->lux();
			g_lastKnownLux.store(lx, std::memory_order_relaxed);
			return lx;
		}
	}

	// No live sensor yet (e.g. just after an ALS re-init): use the last known value, not a hard default.
	if (live) *live = false;
	return g_lastKnownLux.load(std::memory_order_relaxed);
}

/* ---------- mapping lux to brightness ---------- */
static ULONG mapLuxToBrightness(float lux, const DisplayDevice &dev) {
	float scale = std::clamp(lux, 2.f, 5000.f) / dev.baseLux;
	float tgt   = dev.baseBrightness * scale;
	return static_cast<ULONG>(std::clamp(tgt, (float)dev.minBrightness, (float)dev.maxBrightness));
}

/* ---------- Central Brightness Setter ---------- */
static void SetBrightness(DisplayDevice &dev, ULONG val, bool isUserAction, bool showOSD) {
	if (g_hdrActive.load()) return;   // brightness writes are no-ops under HDR; Windows owns it
	if (dev.activePresetLocksBrightness()) return; // reference modes fix brightness (macOS locks it too)
	ULONG safeVal = std::clamp(val, dev.minBrightness, dev.maxBrightness);
	if (safeVal != dev.currentBrightness) {
		int rc = dev.setBrightness(safeVal);
		if (rc != 0) {
			Log::Warn(L"setBrightness failed on %s (rc=%d)", dev.name.c_str(), rc);
			return;
		}
		dev.currentBrightness = safeVal;

		if (isUserAction) {
			dev.baseBrightness = safeVal;
			if (safeVal != dev.minBrightness && safeVal != dev.maxBrightness)
				dev.baseLux = std::max(1.f, getAmbientLux(dev));
			// Stop any auto ramp and drop the hysteresis anchor so auto re-syncs to the user.
			dev.rampDurationMs = 0.0;
			dev.lastTargetLux  = 0.f;
		}

		if (showOSD && g_settings.showOSD)
			OSDWindow::Show((int)dev.currentBrightness, (int)dev.maxBrightness);
	}
}

// Map a brightness value from one display's range to another using nit calibration.
// This ensures perceived brightness matches across displays with different nit ceilings.
static ULONG mapBrightnessAcrossDisplays(ULONG val, const DisplayDevice &from, const DisplayDevice &to) {
	if (from.maxNits == to.maxNits && from.maxBrightness == to.maxBrightness && from.minBrightness == to.minBrightness)
		return val;
	float fromRange = (float)(from.maxBrightness - from.minBrightness);
	if (fromRange <= 0.f) return val;
	float pct  = (float)(val - from.minBrightness) / fromRange;
	float nits = pct * from.maxNits;
	float toPct = std::clamp(nits / to.maxNits, 0.f, 1.f);
	return to.minBrightness + (ULONG)(toPct * (float)(to.maxBrightness - to.minBrightness));
}

// Apply brightness to all connected displays (linked mode, proportional nit mapping)
static void SetBrightnessLinked(ULONG val, const DisplayDevice &refDev, bool isUserAction, bool showOSD) {
	for (auto &dev : g_displays) {
		ULONG mapped = mapBrightnessAcrossDisplays(val, refDev, dev);
		SetBrightness(dev, mapped, isUserAction, showOSD);
	}
}

// The value an Apple display reports is only the last one it was sent. After a boot, a restart
// or a sleep its panel can come back at the firmware default while still reporting that value,
// so reading it tells nothing (#28). Send the brightness again regardless: one unit off first,
// so the display sees a real change like a slider move, then the value itself.
static void ReapplyBrightness(const wchar_t *reason) {
	if (g_hdrActive.load() || g_displays.empty()) return;

	if (g_settings.linkedMode && g_displays.size() > 1) {
		ULONG idx = std::min((ULONG)(g_displays.size() - 1), g_settings.activeDisplayIndex);
		const auto &ref = g_displays[idx];
		SetBrightnessLinked(ref.currentBrightness, ref, true, false);
	}

	for (auto &dev : g_displays) {
		if (dev.maxBrightness <= dev.minBrightness || dev.activePresetLocksBrightness())
			continue;
		ULONG val   = std::clamp(dev.currentBrightness, dev.minBrightness, dev.maxBrightness);
		ULONG nudge = (val < dev.maxBrightness) ? val + 1 : val - 1;
		if (dev.setBrightness(nudge) != 0 || dev.setBrightness(val) != 0) {
			Log::Warn(L"Reapplying brightness failed on %s (%s)", dev.name.c_str(), reason);
			continue;
		}
		dev.currentBrightness = val;
		Log::Info(L"Reapplied brightness %lu to %s (%s)", val, dev.name.c_str(), reason);
	}
}

// Apply brightness change based on current mode (linked or single display)
static void ApplyBrightness(ULONG val, bool isUserAction, bool showOSD) {
	std::lock_guard<std::mutex> lock(g_displayMutex);
	if (g_displays.empty()) return;

	if (g_settings.linkedMode || g_displays.size() == 1) {
		ULONG idx = std::min((ULONG)(g_displays.size() - 1), g_settings.activeDisplayIndex);
		SetBrightnessLinked(val, g_displays[idx], isUserAction, showOSD);
	} else {
		ULONG idx = std::min((ULONG)(g_displays.size() - 1), g_settings.activeDisplayIndex);
		SetBrightness(g_displays[idx], val, isUserAction, showOSD);
	}
}

/* ---------- helpers: brightness step ---------- */
static void adjustBrightnessByStep(int direction) {
	std::lock_guard<std::mutex> lock(g_displayMutex);
	if (g_displays.empty()) return;

	ULONG idx = std::min((ULONG)(g_displays.size() - 1), g_settings.activeDisplayIndex);
	auto &ref = g_displays[idx];
	ULONG step = (ref.maxBrightness - ref.minBrightness) / g_settings.brightnessSteps;
	if (step < 1) step = 1;

	ULONG newBrightness = ref.currentBrightness;
	if (direction > 0) {
		if (ref.currentBrightness < ref.maxBrightness) {
			ULONG actualStep = std::min(step, ref.maxBrightness - ref.currentBrightness);
			newBrightness    = ref.currentBrightness + actualStep;
		}
	} else if (direction < 0) {
		if (ref.currentBrightness > ref.minBrightness) {
			ULONG actualStep = std::min(step, ref.currentBrightness - ref.minBrightness);
			newBrightness    = ref.currentBrightness - actualStep;
		}
	}

	if (newBrightness != ref.currentBrightness) {
		if (g_settings.linkedMode || g_displays.size() == 1) {
			SetBrightnessLinked(newBrightness, ref, true, true);
		} else {
			SetBrightness(ref, newBrightness, true, true);
		}
	}
}

/* ---------- manual brightness slider (tray popup and Options dialog) ---------- */
// What a brightness slider needs to know about the active display. The tray popup and the
// Options dialog both take it from here, so the two sliders show the same level and lock in
// exactly the same situations.
struct SliderState {
	bool  hasDisplay    = false;
	bool  hdr           = false; // Windows owns brightness while HDR is on
	bool  locked        = false; // reference-mode preset: brightness is fixed (macOS locks it there too)
	ULONG minBrightness = 1000;
	ULONG maxBrightness = 60000;
	ULONG brightness    = 0;

	// What automatic brightness works from: the level last set by hand and the ambient light
	// it was set in, and the ambient light now.
	ULONG baseBrightness = 0;
	float baseLux        = 0.f;
	float lux            = 0.f;
	bool  luxLive        = false; // false: no sensor is reporting, `lux` is only the last known value

	bool usable() const { return hasDisplay && !hdr && !locked; }

	// A level as a position on a 0-100 slider. Rounded to the nearest, so that a level set from
	// a position reads back as that position.
	int percentOf(ULONG level) const {
		int range = (int)(maxBrightness - minBrightness);
		if (range <= 0) range = 1;
		float pct = (float)(level - minBrightness) / (float)range * 100.0f;
		return (int)std::lround(std::clamp(pct, 0.0f, 100.0f));
	}
	int percent() const { return percentOf(brightness); }
	int basePercent() const { return percentOf(baseBrightness); }
};

// Caller holds g_displayMutex.
static SliderState sliderStateLocked() {
	SliderState st;
	st.hdr = g_hdrActive.load();
	if (!g_displays.empty()) {
		ULONG idx = std::min((ULONG)(g_displays.size() - 1), g_settings.activeDisplayIndex);
		const auto &ref  = g_displays[idx];
		st.hasDisplay    = true;
		st.locked        = ref.activePresetLocksBrightness();
		st.minBrightness = ref.minBrightness;
		st.maxBrightness = ref.maxBrightness;
		st.brightness    = ref.currentBrightness;
		st.baseBrightness = ref.baseBrightness;
		st.baseLux        = ref.baseLux;
		st.lux            = getAmbientLux(ref, &st.luxLive);
	}
	return st;
}

static SliderState readSliderState() {
	std::lock_guard<std::mutex> lock(g_displayMutex);
	return sliderStateLocked();
}

// For UI timers. The worker keeps the display lock across HID I/O, which can take seconds on a
// panel that is waking up: give up rather than stall the message pump, and catch up next time.
static bool tryReadSliderState(SliderState &st) {
	std::unique_lock<std::mutex> lock(g_displayMutex, std::try_to_lock);
	if (!lock.owns_lock())
		return false;
	st = sliderStateLocked();
	return true;
}

// Send a 0-100 slider position to the active display, or to all of them in linked mode.
// Returns the brightness that was asked for.
static ULONG applySliderPercent(int pct, ULONG refMin, ULONG refMax) {
	ULONG r   = refMax - refMin;
	ULONG val = refMin + (ULONG)((float)r * (float)pct / 100.0f);
	ApplyBrightness(val, true, false);
	return val;
}

/* ---------- Options Dialog ---------- */
static void setDlgHotkey(HWND d, int ctrlId, const HotkeySpec &hk) {
	BYTE f = 0;
	if (hk.mods & MOD_CONTROL) f |= HOTKEYF_CONTROL;
	if (hk.mods & MOD_SHIFT)   f |= HOTKEYF_SHIFT;
	if (hk.mods & MOD_ALT)     f |= HOTKEYF_ALT;
	WORD w = MAKEWORD((BYTE)hk.vk, f);
	SendDlgItemMessageW(d, ctrlId, HKM_SETHOTKEY, w, 0);
}
static void getDlgHotkey(HWND d, int ctrlId, HotkeySpec &out) {
	WORD w    = (WORD)SendDlgItemMessageW(d, ctrlId, HKM_GETHOTKEY, 0, 0);
	UINT vk   = LOBYTE(w);
	BYTE f    = HIBYTE(w);
	UINT mods = 0;
	if (f & HOTKEYF_CONTROL) mods |= MOD_CONTROL;
	if (f & HOTKEYF_SHIFT)   mods |= MOD_SHIFT;
	if (f & HOTKEYF_ALT)     mods |= MOD_ALT;
	out = {mods, vk};
}

// Enable or disable a control. A control that is being disabled hands the focus on first, or
// the keyboard would be left on a window that ignores it.
static void enableDlgItem(HWND d, int ctrlId, bool enable) {
	HWND c = GetDlgItem(d, ctrlId);
	if (!c || (IsWindowEnabled(c) != FALSE) == enable)
		return;
	if (!enable && GetFocus() == c)
		SendMessageW(d, WM_NEXTDLGCTL, 0, FALSE);
	EnableWindow(c, enable);
}

// The ramp times: an edit box with a spin control, in milliseconds.
static void initRampField(HWND d, int editId, int spinId, ULONG ms) {
	UDACCEL step{0, 100}; // the arrows move by a tenth of a second
	SendDlgItemMessageW(d, editId, EM_SETLIMITTEXT, 5, 0); // as many digits as kMaxRampMs
	SendDlgItemMessageW(d, spinId, UDM_SETRANGE32, kMinRampMs, kMaxRampMs);
	SendDlgItemMessageW(d, spinId, UDM_SETACCEL, 1, (LPARAM)&step);
	SendDlgItemMessageW(d, spinId, UDM_SETPOS32, 0, ms);
}
// Read from the edit box rather than the spin control, so that a typed value outside the range
// is clamped instead of dropped. An empty box keeps the current setting.
static ULONG readRampField(HWND d, int editId, ULONG current) {
	BOOL ok = FALSE;
	UINT ms = GetDlgItemInt(d, editId, &ok, FALSE);
	return ok ? std::clamp((ULONG)ms, kMinRampMs, kMaxRampMs) : current;
}
// Automatic brightness as the dialog shows it: the checkbox available and ticked.
static bool autoBrightnessTicked(HWND d) {
	return IsWindowEnabled(GetDlgItem(d, IDC_AUTO_BRIGHTNESS)) &&
	       IsDlgButtonChecked(d, IDC_AUTO_BRIGHTNESS) == BST_CHECKED;
}
// The ramp times only shape automatic changes (a manual change is applied at once), so they can
// be edited only while automatic brightness is ticked.
static void updateRampFields(HWND d) {
	bool on = autoBrightnessTicked(d);
	for (int ctrlId : {IDC_RAMP_BRIGHTEN, IDC_RAMP_BRIGHTEN_SPIN, IDC_RAMP_DIM, IDC_RAMP_DIM_SPIN})
		enableDlgItem(d, ctrlId, on);
}

// The percent field next to the slider shows the slider position as a number, for typing an
// exact level. s_fillingPercent tells our own updates of the box from the user's typing;
// s_percentTyped marks a typed value that has not been sent to the display yet.
static bool s_fillingPercent = false;
static bool s_percentTyped   = false;

static void setPercentField(HWND d, int pct) {
	s_fillingPercent = true;
	SendDlgItemMessageW(d, IDC_SLIDER_PERCENT_SPIN, UDM_SETPOS32, 0, pct);
	s_fillingPercent = false;
}

// Send a level from the slider or the percent field to the display, and show it in both.
// Applied at once, like the tray slider: brightness does not wait for Save.
static void applyOptionsPercent(HWND d, int pct) {
	KillTimer(d, ID_TYPING_TIMER);
	s_percentTyped = false;

	// If the display does not take the new level, nothing is remembered as matched, and the
	// timer puts the controls back where the display really is.
	HWND        slider  = GetDlgItem(d, IDC_BRIGHTNESS_SLIDER);
	ULONG       matched = ~0ul;
	SliderState st      = readSliderState();
	if (st.usable()) {
		ULONG asked = applySliderPercent(pct, st.minBrightness, st.maxBrightness);
		if (readSliderState().brightness == asked)
			matched = asked;
	}
	SetWindowLongPtrW(slider, GWLP_USERDATA, (LONG_PTR)matched);

	if ((int)SendMessageW(slider, TBM_GETPOS, 0, 0) != pct)
		SendMessageW(slider, TBM_SETPOS, TRUE, pct);
	// Rewriting the box moves the caret, so leave it alone when it already says the same.
	BOOL ok    = FALSE;
	UINT shown = GetDlgItemInt(d, IDC_SLIDER_PERCENT, &ok, FALSE);
	if (!ok || (int)shown != pct)
		setPercentField(d, pct);
}

// Apply what was typed into the percent field. A box left empty goes back to the slider's value.
static void commitPercentField(HWND d, bool leaving) {
	KillTimer(d, ID_TYPING_TIMER);
	BOOL ok    = FALSE;
	UINT typed = GetDlgItemInt(d, IDC_SLIDER_PERCENT, &ok, FALSE);
	if (ok && s_percentTyped)
		applyOptionsPercent(d, (int)std::min(typed, 100u));
	else if (!ok && leaving && IsWindowEnabled(GetDlgItem(d, IDC_BRIGHTNESS_SLIDER)))
		setPercentField(d, (int)SendDlgItemMessageW(d, IDC_BRIGHTNESS_SLIDER, TBM_GETPOS, 0, 0));
	s_percentTyped = false;
}

// With automatic brightness on, the level on screen drifts away from the one set by hand. The
// line under the checkbox and a tick mark on the slider keep the two apart: what was set and in
// what ambient light, and where that has led now. Both are blank while automatic brightness is
// off or cannot work, when the two levels are the same thing. (The dialog is as wide as it is
// so that this line fits with six-digit lux values, which is direct sunlight.)
static std::wstring luxText(float lux) {
	wchar_t buf[32];
	swprintf_s(buf, lux < 10.f ? L"%.1f" : L"%.0f", lux);
	return buf;
}
static void showAutoStatus(HWND d, const SliderState &st) {
	bool         on = st.usable() && autoBrightnessTicked(d);
	std::wstring text;
	if (on && !st.luxLive) {
		text = L"No ambient light reading, so there is nothing to follow";
	} else if (on) {
		wchar_t buf[160];
		swprintf_s(buf, L"Set: %d %% at %s lx of ambient light \u00B7 Now: %d %% at %s lx of ambient light",
		           st.basePercent(), luxText(st.baseLux).c_str(), st.percent(), luxText(st.lux).c_str());
		text = buf;
	}
	wchar_t shown[160] = {};
	GetDlgItemTextW(d, IDC_AUTO_STATUS, shown, 160);
	if (text != shown) // rewriting the same text would only make it flicker
		SetDlgItemTextW(d, IDC_AUTO_STATUS, text.c_str());

	// The tick mark for the level that was set: the only tick the slider has (TBM_GETTIC gives -1
	// when there is none). It is painted by drawSliderTick.
	HWND slider = GetDlgItem(d, IDC_BRIGHTNESS_SLIDER);
	int  tick   = on ? st.basePercent() : -1;
	if ((int)SendMessageW(slider, TBM_GETTIC, 0, 0) != tick) {
		SendMessageW(slider, TBM_CLEARTICS, TRUE, 0);
		if (tick >= 0)
			SendMessageW(slider, TBM_SETTIC, 0, tick);
	}
}

// NM_CUSTOMDRAW of the slider. A trackbar that draws any tick also draws one at each end of its
// range, and those two would only compete with the one tick that means something here. So the
// control's own tick drawing is skipped, and the tick for the level that was set is drawn alone.
static LRESULT drawSliderTick(const NMCUSTOMDRAW *cd) {
	if (cd->dwDrawStage == CDDS_PREPAINT)
		return CDRF_NOTIFYITEMDRAW;
	if (cd->dwDrawStage != CDDS_ITEMPREPAINT || cd->dwItemSpec != TBCD_TICS)
		return CDRF_DODEFAULT;

	HWND slider = cd->hdr.hwndFrom;
	int  x      = (int)SendMessageW(slider, TBM_GETTICPOS, 0, 0); // -1: no tick is set
	if (x >= 0) {
		int  dpi = DpiOfWindow(slider);
		RECT thumb;
		SendMessageW(slider, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
		RECT tick{x, thumb.bottom + DpiScale(2, dpi), x + DpiScale(1, dpi), thumb.bottom + DpiScale(7, dpi)};
		FillRect(cd->hdc, &tick, GetSysColorBrush(COLOR_BTNTEXT));
	}
	return CDRF_SKIPDEFAULT;
}

// Bring the brightness controls in line with the active display: which ones are available and
// where the slider sits. Runs when the dialog opens and then on a timer, because
// auto-brightness, the brightness keys, HDR and the display itself all carry on meanwhile.
static void showBrightnessState(HWND d, const SliderState &st, bool opening) {
	// The slider locks exactly when the tray slider does. As in Boot Camp, its labels grey out with it.
	for (int ctrlId : {IDC_BRIGHTNESS_SLIDER, IDC_SLIDER_DIM_LABEL, IDC_SLIDER_BRIGHT_LABEL,
	                   IDC_SLIDER_PERCENT, IDC_SLIDER_PERCENT_SPIN, IDC_SLIDER_PERCENT_UNIT})
		enableDlgItem(d, ctrlId, st.usable());

	// Auto-brightness is unavailable while Windows owns brightness (HDR) and while a
	// reference-mode preset fixes it (macOS locks it there too).
	enableDlgItem(d, IDC_AUTO_BRIGHTNESS, !st.hdr && !st.locked);
	updateRampFields(d);
	showAutoStatus(d, st);

	// Follow the display, but not mid-drag and not after the slider's own change: a position is
	// coarser than the brightness it was mapped to, and mapping that back can nudge the thumb.
	// GWLP_USERDATA remembers the brightness the position was last matched to (see WM_HSCROLL).
	HWND slider = GetDlgItem(d, IDC_BRIGHTNESS_SLIDER);
	if (st.hasDisplay && GetCapture() != slider &&
	    (opening || (ULONG)GetWindowLongPtrW(slider, GWLP_USERDATA) != st.brightness)) {
		SendMessageW(slider, TBM_SETPOS, TRUE, st.percent());
		SetWindowLongPtrW(slider, GWLP_USERDATA, (LONG_PTR)st.brightness);
		if (!s_percentTyped) // not over a number the user is in the middle of typing
			setPercentField(d, st.percent());
	}
}

// A tooltip window for longer, wrapped text. Owned by the dialog, so destroyed with it.
static HWND createWrappedTooltip(HWND d) {
	HWND tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
	                           0, 0, 0, 0, d, nullptr, g_hInst, nullptr);
	if (tip) {
		RECT width{0, 0, 180, 0}; // dialog units, so the wrap width follows the font and the DPI
		MapDialogRect(d, &width);
		SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, width.right);
	}
	return tip;
}

// Tooltip explaining the brightness steps, shown when hovering the field's label.
static void addStepsHelp(HWND d) {
	static const wchar_t kText[] =
	    L"The number of steps the whole brightness range is divided into for the brightness shortcuts: "
	    L"the keyboard's native brightness keys and the custom shortcuts set below.\r\n\r\n"
	    L"One press changes brightness by (max - min) / steps, so fewer steps means a bigger change per press.";

	HWND tip = createWrappedTooltip(d);
	if (!tip)
		return;
	SendMessageW(tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000); // long enough to read
	TTTOOLINFOW ti = {sizeof(ti)};
	ti.uFlags      = TTF_IDISHWND | TTF_SUBCLASS;
	ti.hwnd        = d;
	ti.uId         = (UINT_PTR)GetDlgItem(d, IDC_STEPS_LABEL);
	ti.lpszText    = (LPWSTR)kText;
	SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

/* ---------- preset descriptions as tooltips on the dropdown items ---------- */
// The display supplies a description with a preset (the text that also goes to the log). It is
// shown next to the open dropdown for the item the mouse is over or the arrow keys have moved
// to. A combo box does not report which item is hovered, so its list window is subclassed; this
// state lives and dies with it.
struct PresetTips {
	HWND                      tip = nullptr;
	std::vector<std::wstring> descs;                        // by position in the dropdown; empty = no description
	int                       shown     = -1;               // position the tooltip is up for, -1 = hidden
	POINT                     lastMouse = {LONG_MIN, LONG_MIN}; // screen position of the last real mouse move
};
constexpr UINT_PTR kPresetTipsSubclass = 1;

static void hidePresetTip(HWND list, PresetTips *pt) {
	if (pt->shown < 0)
		return;
	TTTOOLINFOW ti = {sizeof(ti)};
	ti.hwnd        = list;
	SendMessageW(pt->tip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
	pt->shown = -1;
}

static void showPresetTip(HWND list, PresetTips *pt, int item) {
	if (item == pt->shown)
		return;
	hidePresetTip(list, pt);
	if (item < 0 || item >= (int)pt->descs.size() || pt->descs[item].empty())
		return;

	TTTOOLINFOW ti = {sizeof(ti)};
	ti.hwnd        = list;
	ti.lpszText    = pt->descs[item].data();
	SendMessageW(pt->tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);

	// Beside the list and level with the item, so that it covers none of the other items.
	RECT lr, ir, tr;
	GetWindowRect(list, &lr);
	SendMessageW(list, LB_GETITEMRECT, item, (LPARAM)&ir);
	MapWindowPoints(list, nullptr, (POINT *)&ir, 2);
	int gap = 2 * GetSystemMetrics(SM_CXEDGE);
	SendMessageW(pt->tip, TTM_TRACKPOSITION, 0, MAKELPARAM(lr.right + gap, ir.top));
	SendMessageW(pt->tip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
	pt->shown = item;

	// Only now does it have the size of this text. If the monitor has no room for it on the
	// right, move it to the left of the list, and failing that under the list.
	GetWindowRect(pt->tip, &tr);
	int         width = tr.right - tr.left;
	MONITORINFO mi    = {sizeof(mi)};
	GetMonitorInfoW(MonitorFromWindow(list, MONITOR_DEFAULTTONEAREST), &mi);
	if (lr.right + gap + width > mi.rcWork.right) {
		int x = lr.left - gap - width, y = ir.top;
		if (x < mi.rcWork.left) {
			x = lr.left;
			y = lr.bottom + gap;
		}
		SendMessageW(pt->tip, TTM_TRACKPOSITION, 0, MAKELPARAM(x, y));
	}
}

static LRESULT CALLBACK PresetListProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
	auto *pt = (PresetTips *)ref;
	switch (m) {
	case WM_MOUSEMOVE: {
		// Windows also sends this when a window appears or goes under a mouse that has not
		// moved, the tooltip included. Only a real move may take over from the arrow keys.
		POINT p{(short)LOWORD(l), (short)HIWORD(l)}, at = p;
		ClientToScreen(h, &at);
		if (at.x == pt->lastMouse.x && at.y == pt->lastMouse.y)
			break;
		pt->lastMouse = at;

		// The open list holds the mouse capture, so moves outside it arrive here as well. And
		// LB_ITEMFROMPOINT answers with the nearest item, so check the point is really on it.
		RECT rc;
		GetClientRect(h, &rc);
		int item = -1;
		if (PtInRect(&rc, p)) {
			int hit = LOWORD(SendMessageW(h, LB_ITEMFROMPOINT, 0, l));
			if (SendMessageW(h, LB_GETITEMRECT, hit, (LPARAM)&rc) != LB_ERR && PtInRect(&rc, p))
				item = hit;
		}
		showPresetTip(h, pt, item);
		break;
	}
	case WM_MOUSEWHEEL:
	case WM_VSCROLL:
		hidePresetTip(h, pt); // the items move under the mouse; the next move picks it up again
		break;
	case WM_SHOWWINDOW:
	case WM_WINDOWPOSCHANGED: {
		// The dropdown opening or closing, by either route.
		UINT flags = m == WM_WINDOWPOSCHANGED ? ((const WINDOWPOS *)l)->flags : (w ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
		if (flags & SWP_HIDEWINDOW)
			hidePresetTip(h, pt);
		if (flags & SWP_SHOWWINDOW)
			GetCursorPos(&pt->lastMouse); // a mouse that just sits where the list opens is not hovering
		break;
	}
	case WM_NCDESTROY:
		RemoveWindowSubclass(h, PresetListProc, id);
		delete pt;
		break;
	}
	return DefSubclassProc(h, m, w, l);
}

// Keyboard: while the dropdown is open, every arrow-key move comes as CBN_SELCHANGE.
static void showPresetTipForSelection(HWND combo) {
	COMBOBOXINFO cbi = {sizeof(cbi)};
	DWORD_PTR    ref = 0;
	if (!SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) || !GetComboBoxInfo(combo, &cbi) || !cbi.hwndList ||
	    !GetWindowSubclass(cbi.hwndList, PresetListProc, kPresetTipsSubclass, &ref))
		return;
	showPresetTip(cbi.hwndList, (PresetTips *)ref, (int)SendMessageW(combo, CB_GETCURSEL, 0, 0));
}

// `descs` holds one entry per dropdown position.
static void addPresetTips(HWND d, HWND combo, std::vector<std::wstring> descs) {
	if (std::all_of(descs.begin(), descs.end(), [](const std::wstring &s) { return s.empty(); }))
		return; // this display describes none of its presets
	COMBOBOXINFO cbi = {sizeof(cbi)};
	if (!GetComboBoxInfo(combo, &cbi) || !cbi.hwndList)
		return;
	HWND tip = createWrappedTooltip(d);
	if (!tip)
		return;
	TTTOOLINFOW ti = {sizeof(ti)};
	ti.uFlags      = TTF_TRACK | TTF_ABSOLUTE; // shown and placed by showPresetTip
	ti.hwnd        = cbi.hwndList;
	ti.lpszText    = (LPWSTR)L"";
	SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);

	auto *pt = new PresetTips{tip, std::move(descs)};
	if (!SetWindowSubclass(cbi.hwndList, PresetListProc, kPresetTipsSubclass, (DWORD_PTR)pt))
		delete pt;
}

INT_PTR CALLBACK OptionsDlgProc(HWND d, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_INITDIALOG: {
		CheckDlgButton(d, IDC_AUTO_BRIGHTNESS, g_settings.autoAdjustEnabled.load() ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(d, IDC_AUTO_ROTATE, g_settings.autoRotateEnabled.load() ? BST_CHECKED : BST_UNCHECKED);
		// Snapshot the active display's preset state once; the combo fill and the tooltips
		// below key off it. (What gets disabled is kept current by showBrightnessState.)
		bool hdrOn = g_hdrActive.load();
		std::wstring dispName;
		std::vector<ColorPreset> presetsCopy;
		int  active         = -1;
		bool have           = false;
		bool lockBrightness = false;
		{
			std::lock_guard<std::mutex> lock(g_displayMutex);
			if (!g_displays.empty()) {
				ULONG idx      = std::min((ULONG)(g_displays.size() - 1), g_settings.activeDisplayIndex);
				dispName       = g_displays[idx].name;
				presetsCopy    = g_displays[idx].presets;
				active         = g_displays[idx].activePresetIndex;
				lockBrightness = g_displays[idx].activePresetLocksBrightness();
				have           = true;
			}
		}
		CheckDlgButton(d, IDC_SHOW_OSD, g_settings.showOSD ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(d, IDC_RUN_AT_STARTUP, g_settings.runAtStartup ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(d, IDC_ENABLE_HOTKEYS, g_settings.enableCustomHotkeys ? BST_CHECKED : BST_UNCHECKED);
		EnableWindow(GetDlgItem(d, IDC_HOTKEY_UP), g_settings.enableCustomHotkeys);
		EnableWindow(GetDlgItem(d, IDC_HOTKEY_DOWN), g_settings.enableCustomHotkeys);
		setDlgHotkey(d, IDC_HOTKEY_UP, g_settings.hkUp);
		setDlgHotkey(d, IDC_HOTKEY_DOWN, g_settings.hkDown);
		SendDlgItemMessageW(d, IDC_BRIGHTNESS_STEPS_SPIN, UDM_SETRANGE32, kMinBrightnessSteps, kMaxBrightnessSteps);
		SendDlgItemMessageW(d, IDC_BRIGHTNESS_STEPS_SPIN, UDM_SETPOS32, 0, g_settings.brightnessSteps);
		addStepsHelp(d);
		initRampField(d, IDC_RAMP_BRIGHTEN, IDC_RAMP_BRIGHTEN_SPIN, g_settings.rampBrightenMs.load());
		initRampField(d, IDC_RAMP_DIM, IDC_RAMP_DIM_SPIN, g_settings.rampDimMs.load());

		// The brightness slider: the control Boot Camp Control Panel uses, 0 to 100, with Dim and
		// Bright at its sides. Boot Camp's has a tick mark at each end; here the only tick is the
		// one for the level that was set (see drawSliderTick).
		SendDlgItemMessageW(d, IDC_BRIGHTNESS_SLIDER, TBM_SETRANGEMIN, FALSE, 0);
		SendDlgItemMessageW(d, IDC_BRIGHTNESS_SLIDER, TBM_SETRANGEMAX, FALSE, 100);
		s_percentTyped = false;
		SendDlgItemMessageW(d, IDC_SLIDER_PERCENT, EM_SETLIMITTEXT, 3, 0);
		SendDlgItemMessageW(d, IDC_SLIDER_PERCENT_SPIN, UDM_SETRANGE32, 0, 100);
		showBrightnessState(d, readSliderState(), true);
		SetTimer(d, ID_OPTIONS_TIMER, 200, nullptr);

		// Color preset combo for the active display. Under HDR only the main "Apple XDR Display"
		// presets stay listed (the only ones compatible with HDR): switching between those is
		// valid, anything else can blank the panel.
		{
			std::vector<ColorPreset> shown;
			for (const auto &p : presetsCopy)
				if (!hdrOn || p.isHdrCompatible())
					shown.push_back(p);
			std::wstring label = !have ? std::wstring(L"(no display detected)")
			                   : hdrOn ? std::wstring(shown.empty()
			                                 ? L"Disable HDR to change the color preset"
			                                 : L"HDR is on: only the Apple XDR presets are available")
			                           : (L"Display: " + dispName);
			SetDlgItemTextW(d, IDC_PRESET_DISPLAY_LABEL, label.c_str());
			HWND combo = GetDlgItem(d, IDC_PRESET_COMBO);
			SendMessageW(combo, CB_RESETCONTENT, 0, 0);
			if (have && !shown.empty()) {
				int                       sel = -1;
				std::vector<std::wstring> descs(shown.size()); // by dropdown position, for the tooltips
				for (size_t i = 0; i < shown.size(); ++i) {
					int pos = (int)SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)shown[i].name.c_str());
					SendMessageW(combo, CB_SETITEMDATA, pos, (LPARAM)shown[i].index);
					if ((int)shown[i].index == active)
						sel = pos;
					if (pos >= 0 && pos < (int)descs.size())
						descs[pos] = shown[i].desc;
				}
				SendMessageW(combo, CB_SETCURSEL, sel, 0); // -1 = no selection (active preset filtered out)
				EnableWindow(combo, TRUE);
				addPresetTips(d, combo, std::move(descs));
			} else {
				SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"(no presets)");
				SendMessageW(combo, CB_SETCURSEL, 0, 0);
				EnableWindow(combo, FALSE);
			}

			// Experimental escape hatch: ask Windows to turn HDR off from here (it may refuse).
			ShowWindow(GetDlgItem(d, IDC_HDR_OFF_BTN), hdrOn ? SW_SHOW : SW_HIDE);

			// Explanatory tooltips. Rect-based (TTF_SUBCLASS on the dialog) so they work on
			// disabled controls; an enabled control eats the mouse, so only tooltip disabled ones.
			const wchar_t *note = hdrOn          ? L"Brightness is controlled by Windows while HDR is on"
			                    : lockBrightness ? L"Brightness is fixed by the current color preset"
			                                     : nullptr;
			if (note) {
				HWND tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr, WS_POPUP | TTS_ALWAYSTIP,
				                           0, 0, 0, 0, d, nullptr, g_hInst, nullptr);
				if (tip) {
					std::vector<int> targets{ IDC_AUTO_BRIGHTNESS, IDC_BRIGHTNESS_SLIDER };
					if (!IsWindowEnabled(combo))
						targets.push_back(IDC_PRESET_COMBO);
					for (int ctrlId : targets) {
						RECT rc; GetWindowRect(GetDlgItem(d, ctrlId), &rc);
						POINT tl{ rc.left, rc.top }, br{ rc.right, rc.bottom };
						ScreenToClient(d, &tl); ScreenToClient(d, &br);
						TTTOOLINFOW ti = { sizeof(ti) };
						ti.uFlags   = TTF_SUBCLASS;
						ti.hwnd     = d;
						ti.uId      = (UINT_PTR)ctrlId;
						ti.rect     = { tl.x, tl.y, br.x, br.y };
						ti.lpszText = (LPWSTR)note;
						SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
					}
				}
			}
		}
		return TRUE;
	}
	case WM_NOTIFY: {
		const NMHDR *nm = (const NMHDR *)lp;
		if (nm->idFrom != IDC_BRIGHTNESS_SLIDER || nm->code != NM_CUSTOMDRAW)
			break;
		SetWindowLongPtrW(d, DWLP_MSGRESULT, drawSliderTick((const NMCUSTOMDRAW *)lp));
		return TRUE;
	}
	case WM_HSCROLL: {
		HWND slider = GetDlgItem(d, IDC_BRIGHTNESS_SLIDER);
		if ((HWND)lp != slider)
			break;
		applyOptionsPercent(d, (int)SendMessageW(slider, TBM_GETPOS, 0, 0)); // as the thumb moves
		return TRUE;
	}
	case WM_VSCROLL:
		// The arrows of the percent field: no need to wait for a pause as with typing.
		if ((HWND)lp != GetDlgItem(d, IDC_SLIDER_PERCENT_SPIN))
			break;
		commitPercentField(d, false);
		return TRUE;
	case WM_TIMER: {
		if (wp == ID_TYPING_TIMER) {
			commitPercentField(d, false);
			return TRUE;
		}
		if (wp != ID_OPTIONS_TIMER)
			break;
		SliderState st;
		if (tryReadSliderState(st))
			showBrightnessState(d, st, false);
		return TRUE;
	}
	case WM_SETCURSOR: {
		// The steps label has a tooltip: say so with the help cursor.
		if ((HWND)wp != GetDlgItem(d, IDC_STEPS_LABEL))
			break;
		SetCursor(LoadCursorW(nullptr, IDC_HELP));
		SetWindowLongPtrW(d, DWLP_MSGRESULT, TRUE);
		return TRUE;
	}
	case WM_DESTROY:
		KillTimer(d, ID_OPTIONS_TIMER);
		KillTimer(d, ID_TYPING_TIMER);
		break;
	case WM_COMMAND: {
		WORD id   = LOWORD(wp);
		WORD code = HIWORD(wp);
		if (id == IDC_AUTO_BRIGHTNESS && code == BN_CLICKED) {
			showBrightnessState(d, readSliderState(), false); // ramp fields, status line, tick mark
			return TRUE;
		}
		if (id == IDC_PRESET_COMBO && code == CBN_SELCHANGE) {
			showPresetTipForSelection((HWND)lp);
			return TRUE;
		}
		if (id == IDC_SLIDER_PERCENT && code == EN_CHANGE && !s_fillingPercent) {
			// Wait for a pause before applying: "50" should not flash through 5 % on the way.
			s_percentTyped = true;
			SetTimer(d, ID_TYPING_TIMER, 600, nullptr);
			return TRUE;
		}
		if (id == IDC_SLIDER_PERCENT && code == EN_KILLFOCUS) {
			commitPercentField(d, true);
			return TRUE;
		}
		if (id == IDC_ENABLE_HOTKEYS && code == BN_CLICKED) {
			BOOL en = IsDlgButtonChecked(d, IDC_ENABLE_HOTKEYS) == BST_CHECKED;
			EnableWindow(GetDlgItem(d, IDC_HOTKEY_UP), en);
			EnableWindow(GetDlgItem(d, IDC_HOTKEY_DOWN), en);
			return TRUE;
		}
		if (id == IDC_HDR_OFF_BTN && code == BN_CLICKED) {
			Log::Info(L"User asked to turn HDR off from Options");
			if (HdrTurnOffForAppleDisplays() > 0) {
				SetDlgItemTextW(d, IDC_PRESET_DISPLAY_LABEL,
				                L"HDR is turning off. Reopen Options to change the color preset.");
				EnableWindow(GetDlgItem(d, IDC_HDR_OFF_BTN), FALSE);
			} else {
				MessageBoxW(d,
				            L"Windows did not accept the change. Please turn HDR off in Windows Settings > System > Display.",
				            L"Studio Brightness++", MB_ICONINFORMATION);
			}
			return TRUE;
		}
		if (id == IDC_RESET_SHORTCUTS && code == BN_CLICKED) {
			CheckDlgButton(d, IDC_ENABLE_HOTKEYS, BST_UNCHECKED);
			EnableWindow(GetDlgItem(d, IDC_HOTKEY_UP), FALSE);
			EnableWindow(GetDlgItem(d, IDC_HOTKEY_DOWN), FALSE);
			SendDlgItemMessageW(d, IDC_HOTKEY_UP, HKM_SETHOTKEY, 0, 0);
			SendDlgItemMessageW(d, IDC_HOTKEY_DOWN, HKM_SETHOTKEY, 0, 0);
			SendDlgItemMessageW(d, IDC_BRIGHTNESS_STEPS_SPIN, UDM_SETPOS32, 0, kDefaultBrightnessSteps);
			SendDlgItemMessageW(d, IDC_RAMP_BRIGHTEN_SPIN, UDM_SETPOS32, 0, kDefaultRampBrightenMs);
			SendDlgItemMessageW(d, IDC_RAMP_DIM_SPIN, UDM_SETPOS32, 0, kDefaultRampDimMs);
			return TRUE;
		}
		if (id == IDOK) {
			commitPercentField(d, true); // Enter right after typing a level
			g_settings.autoAdjustEnabled.store(IsDlgButtonChecked(d, IDC_AUTO_BRIGHTNESS) == BST_CHECKED);
			g_settings.autoRotateEnabled.store(IsDlgButtonChecked(d, IDC_AUTO_ROTATE) == BST_CHECKED);
			g_settings.showOSD            = (IsDlgButtonChecked(d, IDC_SHOW_OSD) == BST_CHECKED);
			g_settings.runAtStartup        = (IsDlgButtonChecked(d, IDC_RUN_AT_STARTUP) == BST_CHECKED);
			g_settings.enableCustomHotkeys = (IsDlgButtonChecked(d, IDC_ENABLE_HOTKEYS) == BST_CHECKED);
			if (g_settings.enableCustomHotkeys) {
				getDlgHotkey(d, IDC_HOTKEY_UP, g_settings.hkUp);
				getDlgHotkey(d, IDC_HOTKEY_DOWN, g_settings.hkDown);
			} else {
				g_settings.hkUp   = {0, 0};
				g_settings.hkDown = {0, 0};
			}
			if (!registerHotkeys(g_hMain))
				MessageBoxW(d, L"Failed to register one or more hotkeys.", L"StudioBrightnessPlusPlus", MB_ICONERROR);

			ULONG steps            = (ULONG)SendDlgItemMessageW(d, IDC_BRIGHTNESS_STEPS_SPIN, UDM_GETPOS32, 0, 0);
			g_settings.brightnessSteps = std::clamp(steps, kMinBrightnessSteps, kMaxBrightnessSteps);
			g_settings.rampBrightenMs.store(readRampField(d, IDC_RAMP_BRIGHTEN, g_settings.rampBrightenMs.load()));
			g_settings.rampDimMs.store(readRampField(d, IDC_RAMP_DIM, g_settings.rampDimMs.load()));
			g_settings.Save();
			g_settings.SetStartup(g_settings.runAtStartup);
			orient_set_enabled(g_settings.autoRotateEnabled.load());

			// Apply the chosen color preset to the active display, then prompt to keep or auto-revert.
			// Not persisted: a preset resets to the default at startup, changed only by hand here.
			{
				HWND combo = GetDlgItem(d, IDC_PRESET_COMBO);
				int  sel   = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
				if (sel != CB_ERR && IsWindowEnabled(combo)) {
					int  hwIdx    = (int)SendMessageW(combo, CB_GETITEMDATA, sel, 0);
					GUID cid      = {};
					int  prevIdx  = -1;
					bool switched = false;
					{
						std::lock_guard<std::mutex> lock(g_displayMutex);
						if (!g_displays.empty()) {
							ULONG idx = std::min((ULONG)(g_displays.size() - 1), g_settings.activeDisplayIndex);
							auto &dev = g_displays[idx];
							if (dev.hPreset != INVALID_HANDLE_VALUE && hwIdx != dev.activePresetIndex) {
								prevIdx = dev.activePresetIndex;
								// Log the intent BEFORE the write (the file log flushes per line):
								// if this switch takes the GPU driver down, the log still shows
								// exactly what ran.
								const ColorPreset *from  = dev.activePreset();
								const wchar_t     *toName = L"?";
								for (const auto &p : dev.presets)
									if ((int)p.index == hwIdx) { toName = p.name.c_str(); break; }
								Log::Info(L"Switching color preset on %s: %d (%s) -> %d (%s), HDR=%d",
								          dev.name.c_str(), prevIdx, from ? from->name.c_str() : L"?",
								          hwIdx, toName, g_hdrActive.load() ? 1 : 0);
								if (dev.setActivePreset(hwIdx) == 0) {
									cid      = dev.containerId;
									switched = true;
								} else {
									Log::Warn(L"Preset switch failed on %s", dev.name.c_str());
								}
							}
						}
					}
					if (switched) {
						NvapiLogHdrState(L"post preset switch");
						PresetConfirm::Show(g_hInst, 10, [cid, prevIdx]() { RevertPresetByContainer(cid, prevIdx); });
					}
				}
			}

			EndDialog(d, IDOK);
			return TRUE;
		}
		if (id == IDCANCEL) {
			EndDialog(d, IDCANCEL);
			return TRUE;
		}
		break;
	}
	}
	return FALSE;
}

/* ---------- helpers for tray menu display list ---------- */
static bool activeDisplayPresetLocked() {
	std::lock_guard<std::mutex> lock(g_displayMutex);
	if (g_displays.empty()) return false;
	ULONG idx = std::min((ULONG)(g_displays.size() - 1), g_settings.activeDisplayIndex);
	return g_displays[idx].activePresetLocksBrightness();
}

static std::wstring buildDisplayStatusLine() {
	std::lock_guard<std::mutex> lock(g_displayMutex);
	if (g_displays.empty())
		return L"\U0001F534 No Display Detected";

	if (g_displays.size() == 1)
		return std::wstring(L"\U0001F7E2 ") + g_displays[0].name + L" Connected";

	// Multiple displays
	std::wstring line = L"\U0001F7E2 ";
	line += std::to_wstring(g_displays.size());
	line += L" Displays Connected";
	return line;
}

/* ---------- hidden window & WndProc ---------- */
// A panel takes a moment to light up after Windows turns it on, so reapply 3 s and 6 s later
// rather than once right away. A new event restarts the count.
static int g_reapplyPassesLeft = 0;
static void scheduleReapply(HWND h) {
	g_reapplyPassesLeft = 2;
	SetTimer(h, ID_REAPPLY_TIMER, 3000, nullptr);
}

LRESULT CALLBACK HiddenWndProc(HWND h, UINT m, WPARAM wParam, LPARAM lParam) {
	if (g_wmTaskbarCreated && m == g_wmTaskbarCreated) {
		AddNotificationIcon(h);
		return 0;
	}
	if (m == WM_DISPLAYCHANGE) {
		RefreshHdrState();
		scheduleReapply(h);
		return 0;
	}
	if (m == WM_POWERBROADCAST && wParam == PBT_POWERSETTINGCHANGE) {
		// Console display state: 0 off, 1 on, 2 dimmed. Windows sends the current state right
		// after registering, so only a change from off or dimmed to on counts.
		static DWORD lastState = 1;
		auto *ps = (const POWERBROADCAST_SETTING *)lParam;
		if (ps && IsEqualGUID(ps->PowerSetting, GUID_CONSOLE_DISPLAY_STATE) && ps->DataLength >= sizeof(DWORD)) {
			DWORD state = *(const DWORD *)ps->Data;
			if (state == 1 && lastState != 1)
				scheduleReapply(h);
			lastState = state;
		}
		return TRUE;
	}
	if (m == WM_TIMER && wParam == ID_REAPPLY_TIMER) {
		g_reapplyRequested.store(true);
		if (--g_reapplyPassesLeft <= 0)
			KillTimer(h, ID_REAPPLY_TIMER);
		return 0;
	}
	if (m == WM_DEVICECHANGE && wParam == DBT_DEVICEARRIVAL) {
		// Only Apple interfaces: a full scan logs every interface, so a mouse or a USB stick
		// should not trigger one. Removals are caught by the worker's liveness check.
		auto *hdr = (const DEV_BROADCAST_HDR *)lParam;
		if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE &&
		    StrStrIW(((const DEV_BROADCAST_DEVICEINTERFACE_W *)hdr)->dbcc_name, L"vid_05ac"))
			g_rescanRequested.store(true);
		return TRUE;
	}
	if (m == WM_HOTKEY) {
		if (wParam == ID_HOTKEY_UP) {
			adjustBrightnessByStep(+1);
			return 0;
		}
		if (wParam == ID_HOTKEY_DOWN) {
			adjustBrightnessByStep(-1);
			return 0;
		}
	}
	if (m == WM_INPUT) {
		UINT dwSize = 0;
		GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &dwSize, sizeof(RAWINPUTHEADER));
		if (dwSize) {
			std::vector<BYTE> lpb(dwSize);
			if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb.data(), &dwSize, sizeof(RAWINPUTHEADER)) == dwSize) {
				RAWINPUT *raw = (RAWINPUT *)lpb.data();
				if (raw->header.dwType == RIM_TYPEHID) {
					const RAWHID &hid = raw->data.hid;
					for (ULONG i = 0; i < hid.dwCount; ++i) {
						const BYTE *report = hid.bRawData + i * hid.dwSizeHid;
						if (hid.dwSizeHid >= 3) {
							USHORT usage = report[1] | (report[2] << 8);
							if (usage == 0x006F)
								adjustBrightnessByStep(+1);
							else if (usage == 0x0070)
								adjustBrightnessByStep(-1);
						}
					}
				}
			}
		}
		return 0;
	}
	if (m == WMAPP_UPDATE_READY) {
		if (wParam == 3) {
			ShowUpdateBalloon(L"Studio Brightness++", L"The update could not be installed.");
		} else if (g_updateAvailable.load()) {
			std::wstring v;
			{
				std::lock_guard<std::mutex> lk(g_updateMutex);
				v = g_updateInfo.version;
			}
			std::wstring text = L"Version " + v + L" is available. Click here to install.";
			ShowUpdateBalloon(L"Update available", text.c_str());
		} else if (wParam == 1) {
			ShowUpdateBalloon(L"Studio Brightness++", L"You are running the latest version.");
		}
		return 0;
	}
	if (m == WM_TIMER && wParam == ID_UPDATE_TIMER) {
		StartUpdateCheck(false);
		return 0;
	}
	if (m == WM_TIMER && wParam == ID_HDR_TIMER) {
		RefreshHdrState();
		return 0;
	}
	if (m == WMAPP_NOTIFYCALLBACK) {
		if (LOWORD(lParam) == WM_LBUTTONUP) {
			SliderState st = readSliderState();
			if (st.hdr) {
				TrayPopup::Show(h, 0, nullptr, true, L"Brightness controlled by Windows (HDR)");
				return 0;
			}
			if (!st.hasDisplay) return 0;
			if (st.locked) { // reference-mode preset: brightness is fixed (macOS locks it there too)
				TrayPopup::Show(h, 0, nullptr, true, L"Brightness fixed by the color preset");
				return 0;
			}

			TrayPopup::Show(h, st.percent(), [refMin = st.minBrightness, refMax = st.maxBrightness](int newPct) {
				applySliderPercent(newPct, refMin, refMax);
			});
			return 0;
		}

		if (LOWORD(lParam) == NIN_BALLOONUSERCLICK) {
			if (g_updateAvailable.load()) StartUpdateInstall();
			return 0;
		}

		if (LOWORD(lParam) == WM_RBUTTONUP) {
			HMENU hMenu = CreatePopupMenu();

			std::wstring statusLine = buildDisplayStatusLine();
			AppendMenuW(hMenu, MF_STRING | MF_DISABLED, 0, statusLine.c_str());

			// Display list with selection (only when multiple)
			{
				std::lock_guard<std::mutex> lock(g_displayMutex);
				if (g_displays.size() > 1) {
					ULONG activeIdx = std::min((ULONG)(g_displays.size() - 1), g_settings.activeDisplayIndex);
					for (size_t i = 0; i < g_displays.size(); ++i) {
						std::wstring item = L"    \U0001F7E2 " + g_displays[i].name;
						UINT flags = MF_STRING;
						if (g_settings.linkedMode) {
							flags |= MF_CHECKED | MF_GRAYED;
						} else {
							flags |= (i == activeIdx) ? MF_CHECKED : MF_UNCHECKED;
						}
						AppendMenuW(hMenu, flags, IDM_SELECT_DISPLAY + i, item.c_str());
					}
					AppendMenuW(hMenu, MF_STRING | (g_settings.linkedMode ? MF_CHECKED : 0),
					            IDM_LINKED_MODE, L"Linked Displays");
				}
			}

			AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
			UINT autoFlags = MF_STRING | (g_settings.autoAdjustEnabled.load() ? MF_CHECKED : 0u)
			                 | ((g_hdrActive.load() || activeDisplayPresetLocked())
			                        ? (UINT)(MF_GRAYED | MF_DISABLED) : 0u);
			AppendMenuW(hMenu, autoFlags, IDM_TOGGLE_AUTO, L"Automatic Brightness");
			UINT rotFlags = MF_STRING | (g_settings.autoRotateEnabled.load() ? MF_CHECKED : 0u);
			AppendMenuW(hMenu, rotFlags, IDM_TOGGLE_ROTATE, L"Automatic Rotate");
			AppendMenuW(hMenu, MF_STRING, IDM_OPTIONS, L"Options...");
			AppendMenuW(hMenu, MF_STRING, IDM_SHOW_LOGS, L"Logs...");
			if (g_updateAvailable.load()) {
				std::wstring lbl;
				{
					std::lock_guard<std::mutex> lk(g_updateMutex);
					lbl = L"Install update " + g_updateInfo.version;
				}
				AppendMenuW(hMenu, MF_STRING, IDM_INSTALL_UPDATE, lbl.c_str());
			} else {
				AppendMenuW(hMenu, MF_STRING, IDM_CHECK_UPDATE, L"Check update");
			}
			HMENU hChannel = CreatePopupMenu();
			AppendMenuW(hChannel, MF_STRING | (g_settings.updateChannel == 0 ? MF_CHECKED : 0),
			            IDM_CHANNEL_STABLE, L"Stable only");
			AppendMenuW(hChannel, MF_STRING | (g_settings.updateChannel == 1 ? MF_CHECKED : 0),
			            IDM_CHANNEL_BETA, L"Include betas");
			AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hChannel, L"Update channel");
			wchar_t versionLabel[40];
			swprintf_s(versionLabel, L"Version %s", kAppVersion);
			AppendMenuW(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, IDM_VERSION_INFO, versionLabel);
			AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
			AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Quit");

			POINT pt;
			GetCursorPos(&pt);
			SetForegroundWindow(h);
			int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, h, nullptr);
			DestroyMenu(hMenu);

			if (cmd == IDM_TOGGLE_AUTO) {
				g_settings.autoAdjustEnabled.store(!g_settings.autoAdjustEnabled.load());
				g_settings.Save();
			} else if (cmd == IDM_TOGGLE_ROTATE) {
				g_settings.autoRotateEnabled.store(!g_settings.autoRotateEnabled.load());
				g_settings.Save();
				orient_set_enabled(g_settings.autoRotateEnabled.load());
			} else if (cmd == IDM_LINKED_MODE) {
				g_settings.linkedMode = !g_settings.linkedMode;
				g_settings.Save();
			} else if (cmd >= IDM_SELECT_DISPLAY && cmd < IDM_SELECT_DISPLAY + 16) {
				g_settings.activeDisplayIndex = (ULONG)(cmd - IDM_SELECT_DISPLAY);
				g_settings.Save();
			} else if (cmd == IDM_OPTIONS) {
				DialogBoxParamW(g_hInst, MAKEINTRESOURCE(IDD_OPTIONS), h, OptionsDlgProc, 0);
			} else if (cmd == IDM_SHOW_LOGS) {
				LogWindow::Show();
			} else if (cmd == IDM_CHECK_UPDATE) {
				StartUpdateCheck(true);
			} else if (cmd == IDM_INSTALL_UPDATE) {
				StartUpdateInstall();
			} else if (cmd == IDM_CHANNEL_STABLE) {
				g_settings.updateChannel = 0;
				g_settings.Save();
			} else if (cmd == IDM_CHANNEL_BETA) {
				g_settings.updateChannel = 1;
				g_settings.Save();
			} else if (cmd == IDM_EXIT) {
				PostMessage(h, WM_CLOSE, 0, 0);
			}
			return 0;
		}
	}
	if (m == WM_DESTROY) {
		cleanupAlsSensors();
		{
			std::lock_guard<std::mutex> lock(g_displayMutex);
			g_displays.clear(); // destructors close handles
		}
		DeleteNotificationIcon();
		unregisterHotkeys(h);
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProc(h, m, wParam, lParam);
}

bool RegisterHiddenClass() {
	WNDCLASSEXW wc{sizeof(wc)};
	wc.lpfnWndProc   = HiddenWndProc;
	wc.hInstance     = g_hInst;
	wc.hIcon         = LoadIcon(g_hInst, MAKEINTRESOURCE(IDI_MYICON));
	wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = kWndClass;
	ATOM a = RegisterClassExW(&wc);
	if (!a) {
		wchar_t buf[128];
		wsprintf(buf, L"RegisterClassExW failed (%lu)", GetLastError());
		MessageBoxW(nullptr, buf, L"StudioBrightnessPlusPlus", MB_ICONERROR | MB_TOPMOST);
		return false;
	}
	return true;
}

/* ---------- sensor polling thread ---------- */
// Both watchers read with synchronous HID I/O: HidD_GetInputReport blocks, and the fallback
// waits up to 150 ms per device. On a WM_TIMER that stalls the message pump several times a
// second, and a panel waking from sleep can block a HID call for seconds. Polling goes here
// instead and publishes into the snapshots the UI and the worker read.
// Joined at shutdown, unlike the worker: the watchers free the handles it is still reading.
static std::thread g_sensorThread;
static HANDLE      g_sensorStop = nullptr;

static void startSensorThread() {
	g_sensorStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!g_sensorStop) {
		Log::Warn(L"Sensor thread not started: CreateEvent failed (%lu)", GetLastError());
		return;
	}
	g_sensorThread = std::thread([] {
		// Wait, not sleep, so shutdown does not cost a whole poll interval.
		while (WaitForSingleObject(g_sensorStop, 250) == WAIT_TIMEOUT) {
			orient_watch_tick();
			als_watch_tick();
		}
	});
}

static void stopSensorThread() {
	if (g_sensorStop) SetEvent(g_sensorStop);
	if (g_sensorThread.joinable()) g_sensorThread.join();
	if (g_sensorStop) { CloseHandle(g_sensorStop); g_sensorStop = nullptr; }
}

/* ---------- background worker thread ---------- */
void startWorker() {
	std::thread([] {
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);

		// Initialize ALS sensors
		initAlsSensors();

		DWORD lastEnumerateTick = 0;
		bool  firstAddDone      = false; // skip the ALS re-bind on the first device add (startup)
		constexpr DWORD kEnumerateCooldownMs = 3000; // re-scan every 3s at most

		for (;;) {
			/* ---------- device (re)connection attempt ---------- */
			{
				std::lock_guard<std::mutex> lock(g_displayMutex);

				// Check existing devices are still alive
				bool anyDead = false;
				for (auto &dev : g_displays) {
					ULONG tmp;
					if (dev.getBrightness(&tmp) != 0) {
						Log::Warn(L"Device %s disconnected", dev.name.c_str());
						dev.close();
						anyDead = true;
					}
				}

				// Remove dead devices
				if (anyDead) {
					g_displays.erase(
					    std::remove_if(g_displays.begin(), g_displays.end(),
					                   [](const DisplayDevice &d) { return !d.isOpen(); }),
					    g_displays.end());
				}

				// Re-scan for new devices only when needed (empty, device lost, or a device change)
				DWORD now = GetTickCount();
				bool needScan = g_displays.empty() || anyDead || g_rescanRequested.load();
				if (needScan && now - lastEnumerateTick >= kEnumerateCooldownMs) {
					lastEnumerateTick = now;
					g_rescanRequested.store(false); // before enumerating, so a change during it re-arms

					auto found = hid_enumerate();
					bool added = false;
					for (auto &newDev : found) {
						bool exists = false;
						for (const auto &existing : g_displays) {
							if (existing.devicePath == newDev.devicePath) {
								exists = true;
								break;
							}
						}
						if (exists) {
							newDev.close();
							continue;
						}

						// Initialize new device. On a reconnect the old ISensor goes stale across the
						// display reconfiguration, so re-bind the ALS to the freshly settled sensor
						// (skip the first add: initAlsSensors already ran at worker startup).
						if (firstAddDone) {
							cleanupAlsSensors();
							initAlsSensors();
						}
						firstAddDone = true;

						newDev.getBrightnessRange(&newDev.minBrightness, &newDev.maxBrightness);
						if (newDev.getBrightness(&newDev.currentBrightness) == 0) {
							newDev.baseBrightness = newDev.currentBrightness;
							// mapLuxToBrightness divides by baseLux, and a sensor reads 0 in a dark room.
							// Unclamped that sends the target to infinity.
							newDev.baseLux = std::max(1.f, getAmbientLux(newDev));
						}
						Log::Info(L"Device %s ready [range %lu-%lu, current %lu]",
						          newDev.name.c_str(), newDev.minBrightness, newDev.maxBrightness,
						          newDev.currentBrightness);

						// Color presets: enumerate ONCE per physical display (cached), reset to the default ONCE per run.
						// Re-enumerating or re-restoring on every reconnect loops, because switching a
						// calibrated preset re-enumerates the display's HID descriptor (a disconnect).
						if (newDev.hPreset != INVALID_HANDLE_VALUE) {
							std::wstring cidKey = guidToString(newDev.containerId);
							auto cit = g_presetCache.find(cidKey);
							if (cit != g_presetCache.end() && !cit->second.empty()) {
								newDev.presets = cit->second; // reuse cached list; skip cursor writes
							} else {
								newDev.enumeratePresets();
								if (!newDev.presets.empty())
									g_presetCache[cidKey] = newDev.presets;
							}
							newDev.getActivePreset(&newDev.activePresetIndex);
							if (g_presetRestored.insert(cidKey).second) { // once per run, per display
								// Reset to the default reference mode (index 0) at startup if not already
								// there. No persistence/restore of the user's choice; a preset is changed
								// only by manual action, and this stops a saved preset from re-blanking
								// some XDR units on every launch. Log first (the file log flushes per
								// line) so a write that blanks the panel still shows in the log.
								if (!newDev.presets.empty() && newDev.activePresetIndex != 0) {
									Log::Info(L"Startup: resetting %s to its default color preset (was %d)",
									          newDev.name.c_str(), newDev.activePresetIndex);
									if (newDev.setActivePreset(0) != 0)
										Log::Warn(L"Default preset reset failed on %s", newDev.name.c_str());
								}
							}
						}

						g_displays.push_back(std::move(newDev));
						added = true;
					}

					// At launch or after a reconnect, the panels may not show what they report.
					if (added)
						ReapplyBrightness(L"display found");
				}

				// Displays powered back on (sleep, monitor standby) or reconfigured: same reason.
				if (g_reapplyRequested.exchange(false))
					ReapplyBrightness(L"displays back on");
			}

			/* ---------- auto-brightness (Apple-style hysteresis + asymmetric perceptual ramp) ---------- */
			if (g_settings.autoAdjustEnabled.load()) {
				std::lock_guard<std::mutex> lock(g_displayMutex);
				for (auto &dev : g_displays) {
					if (dev.maxBrightness <= dev.minBrightness)
						continue; // brightness locked (e.g. a calibrated color preset); nothing to adjust
					if (dev.activePresetLocksBrightness())
						continue; // reference mode active: brightness is fixed (macOS parity)
					float lux = getAmbientLux(dev); // per-device, ContainerId-matched sensor

					bool retarget = (dev.lastTargetLux <= 0.f) ||
					                (std::fabs(lux - dev.lastTargetLux) / dev.lastTargetLux >= kRelLuxHysteresis);
					if (retarget) {
						ULONG goal = mapLuxToBrightness(lux, dev);
						if (goal != dev.rampGoal || dev.rampDurationMs == 0.0) {
							ULONG rampMs = (goal >= dev.currentBrightness) ? g_settings.rampBrightenMs.load()
							                                               : g_settings.rampDimMs.load();
							dev.rampStart      = dev.currentBrightness;
							dev.rampGoal       = goal;
							dev.rampStartMs    = nowMs();
							// At least 1 ms: 0 means "no ramp running" here, and the goal would never be applied.
							dev.rampDurationMs = std::max(1.0, (double)rampMs);
						}
						dev.lastTargetLux = lux;
					}

					if (dev.rampDurationMs > 0.0) {
						double t = (nowMs() - dev.rampStartMs) / dev.rampDurationMs;
						if (t >= 1.0) {
							if (dev.currentBrightness != dev.rampGoal)
								SetBrightness(dev, dev.rampGoal, false, false);
							dev.rampDurationMs = 0.0;
						} else {
							if (t < 0.0) t = 0.0;
							double a = brightToPerc(dev.rampStart), b = brightToPerc(dev.rampGoal);
							ULONG  next = percToBright(a + t * (b - a));
							next = std::clamp(next, dev.minBrightness, dev.maxBrightness);
							if (next != dev.currentBrightness)
								SetBrightness(dev, next, false, false);
						}
					}
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}).detach();
}

/* ---------- WinMain ---------- */
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
	HANDLE hSingleInstance = CreateMutexW(nullptr, TRUE, L"StudioBrightnessPlusPlus_SingleInstance");
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		if (hSingleInstance) CloseHandle(hSingleInstance);
		return 0;
	}

	GdiplusStartupInput gdiplusStartupInput;
	GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	g_hInst = hInst;
	INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES};
	InitCommonControlsEx(&icc);

	g_settings.Load();
	bool realStartup = g_settings.IsStartupEnabled();
	if (g_settings.runAtStartup != realStartup) {
		// Sync the Run key with the saved preference: rewrite the (now path-checked) entry if we
		// should auto-start, or remove a stale/leftover entry if we shouldn't.
		g_settings.SetStartup(g_settings.runAtStartup);
	}

	Log::ResumeIfPending();   // resume a file-log session that was still running before a restart
	Log::Info(L"Studio Brightness++ v%s starting", kAppVersion);

	if (!RegisterHiddenClass()) {
		CloseHandle(hSingleInstance);
		return 1;
	}

	SetLastError(0);
	HWND h  = CreateWindowW(kWndClass, L"", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
	                        400, 200, nullptr, nullptr, hInst, nullptr);
	g_hMain = h;
	g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
	if (!h) {
		wchar_t buf[128];
		wsprintf(buf, L"CreateWindowW failed (%lu)", GetLastError());
		MessageBoxW(nullptr, buf, L"StudioBrightnessPlusPlus", MB_ICONERROR | MB_TOPMOST);
		CloseHandle(hSingleInstance);
		return 1;
	}
	ShowWindow(h, SW_HIDE);

	RAWINPUTDEVICE rid;
	rid.usUsagePage = 0x0C;
	rid.usUsage     = 0x01;
	rid.dwFlags     = RIDEV_INPUTSINK;
	rid.hwndTarget  = h;
	if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
		MessageBoxW(nullptr, L"RegisterRawInputDevices failed", L"StudioBrightnessPlusPlus", MB_ICONERROR);
		CloseHandle(hSingleInstance);
		return 1;
	}

	// HID interface arrivals reach HiddenWndProc as DBT_DEVICEARRIVAL (re-scan for displays).
	DEV_BROADCAST_DEVICEINTERFACE_W hidFilter{};
	hidFilter.dbcc_size       = sizeof(hidFilter);
	hidFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
	HidD_GetHidGuid(&hidFilter.dbcc_classguid);
	if (!RegisterDeviceNotificationW(h, &hidFilter, DEVICE_NOTIFY_WINDOW_HANDLE))
		Log::Warn(L"RegisterDeviceNotification failed (%lu)", GetLastError());
	// Displays turning back on (wake from sleep, monitor standby) reach HiddenWndProc as
	// WM_POWERBROADCAST, so the brightness can be sent again (#28).
	if (!RegisterPowerSettingNotification(h, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE))
		Log::Warn(L"RegisterPowerSettingNotification failed (%lu)", GetLastError());

	if (!AddNotificationIcon(h)) {
		MessageBoxW(nullptr,
		            L"Failed to create the system tray icon.\nUse Task Manager to exit if needed.",
		            L"Studio Brightness ++", MB_ICONWARNING);
	}
	registerHotkeys(h);

	// Before startWorker: the worker anchors baseLux from the first reading it can get, so the
	// raw-HID ALS has to be up and carrying a real sample by then.
	orient_watch_init(g_settings.autoRotateEnabled.load()); // discover Apple orientation sensors (MI_09)
	als_watch_init();                           // discover Apple raw-HID ALS (MI_08 illuminance)
	startSensorThread();                        // poll both off the UI thread
	startWorker();

	// Check for updates shortly after launch, then once a day.
	SetTimer(h, ID_UPDATE_TIMER, 24 * 60 * 60 * 1000, nullptr);
	SetTimer(h, ID_HDR_TIMER, 2000, nullptr);   // poll HDR; also refreshed on WM_DISPLAYCHANGE
	RefreshHdrState();
	NvapiLogHdrState(L"startup");
	HdrLogAppleOutputs(L"startup");
	StartUpdateCheck(false);

	MSG msg;
	while (GetMessage(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	stopSensorThread();   // join first: the watchers below free the handles it reads
	orient_watch_shutdown();
	als_watch_shutdown();
	GdiplusShutdown(gdiplusToken);
	CoUninitialize();
	CloseHandle(hSingleInstance);
	return 0;
}
