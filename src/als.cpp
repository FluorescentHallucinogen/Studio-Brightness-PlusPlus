//----------------  als.cpp  ----------------
#include "als.h"
#include "Log.h"

#define _WIN32_DCOM
#include <initguid.h>
#include <devpropdef.h>
#include <setupapi.h>
#include <shlwapi.h>
#include <mutex>
#include <cstring>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "shlwapi.lib")

// DEVPKEY_Device_ContainerId : {8c7ed206-3f8a-4827-b3ab-ae9e1faefc6c}, 2
DEFINE_DEVPROPKEY(ALS_DEVPKEY_Device_ContainerId,
                  0x8c7ed206, 0x3f8a, 0x4827,
                  0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c, 2);

static const wchar_t kAppleVid[] = L"vid_05ac";

static inline bool icontains(const wchar_t *hay, const wchar_t *needle) {
	return StrStrIW(hay, needle) != nullptr;
}

// "vid_05ac&pid_1116&mi_08" from a device interface path, for readable logs.
static std::wstring shortLabel(const wchar_t *path) {
	const wchar_t *p = StrStrIW(path, L"vid_");
	if (!p) p = path;
	std::wstring s;
	for (const wchar_t *q = p; *q && *q != L'#'; ++q) s += *q;
	return s;
}

/* ============================ AlsDevice ============================ */

void AlsDevice::moveFrom(AlsDevice &o) {
	hDev = o.hDev; prep = o.prep; inputLen = o.inputLen; reportId = o.reportId;
	uIllum = o.uIllum; overlapped = o.overlapped; containerId = o.containerId;
	devicePath = std::move(o.devicePath); label = std::move(o.label); lastRaw = o.lastRaw;
	o.hDev = INVALID_HANDLE_VALUE; o.prep = nullptr;
}

void AlsDevice::close() {
	if (prep) { HidD_FreePreparsedData(prep); prep = nullptr; }
	if (hDev != INVALID_HANDLE_VALUE) { CloseHandle(hDev); hDev = INVALID_HANDLE_VALUE; }
}

bool AlsDevice::readReport(std::vector<uint8_t> &buf) {
	if (hDev == INVALID_HANDLE_VALUE || inputLen == 0) return false;
	buf.assign(inputLen, 0);
	buf[0] = reportId;
	if (HidD_GetInputReport(hDev, buf.data(), (ULONG)buf.size()))
		return true;
	if (!overlapped) return false;
	std::vector<uint8_t> rb(inputLen, 0);
	OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!ov.hEvent) return false;
	bool ok = false; DWORD got = 0;
	BOOL r = ReadFile(hDev, rb.data(), inputLen, &got, &ov);
	if (r || GetLastError() == ERROR_IO_PENDING) {
		if (WaitForSingleObject(ov.hEvent, 150) == WAIT_OBJECT_0 &&
		    GetOverlappedResult(hDev, &ov, &got, FALSE) && got > 0) {
			buf.assign(rb.begin(), rb.end());
			ok = true;
		} else {
			CancelIo(hDev);
		}
	}
	CloseHandle(ov.hEvent);
	return ok;
}

bool AlsDevice::readLux(LONG *rawOut, float *luxOut) {
	if (!prep || !uIllum) return false;
	std::vector<uint8_t> buf;
	if (!readReport(buf)) return false;
	ULONG v = 0;
	if (HidP_GetUsageValue(HidP_Input, HID_UP_SENSOR_PAGE, 0, uIllum, &v, prep,
	                       reinterpret_cast<PCHAR>(buf.data()), (ULONG)buf.size()) != HIDP_STATUS_SUCCESS)
		return false;
	LONG raw = (LONG)v;
	LONG scaled = 0;
	bool okS = HidP_GetScaledUsageValue(HidP_Input, HID_UP_SENSOR_PAGE, 0, uIllum, &scaled, prep,
	                                    reinterpret_cast<PCHAR>(buf.data()), (ULONG)buf.size()) == HIDP_STATUS_SUCCESS;
	if (rawOut) *rawOut = raw;
	if (luxOut) *luxOut = okS ? (float)scaled : (float)raw;
	return true;
}

/* ============================ ContainerId ============================ */

static GUID queryContainerIdFromDevinfo(HDEVINFO set, PSP_DEVINFO_DATA devInfo) {
	GUID cid = {}; DEVPROPTYPE type = 0; DWORD size = 0;
	SetupDiGetDevicePropertyW(set, devInfo, &ALS_DEVPKEY_Device_ContainerId, &type, nullptr, 0, &size, 0);
	if (size == sizeof(GUID))
		SetupDiGetDevicePropertyW(set, devInfo, &ALS_DEVPKEY_Device_ContainerId, &type, (PBYTE)&cid, sizeof(GUID), nullptr, 0);
	return cid;
}

/* ============================ Enumeration ============================ */

std::vector<AlsDevice> als_enumerate() {
	std::vector<AlsDevice> result;

	GUID hidGuid; HidD_GetHidGuid(&hidGuid);
	HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, 0, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set == INVALID_HANDLE_VALUE) {
		Log::Error(L"als: SetupDiGetClassDevsW failed (%lu)", GetLastError());
		return result;
	}

	SP_DEVICE_INTERFACE_DATA ifd{sizeof(ifd)};
	for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifd); ++i) {
		DWORD need = 0;
		SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
		if (!need) continue;
		std::vector<BYTE> ibuf(need);
		auto det = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(ibuf.data());
		det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
		SP_DEVINFO_DATA devInfo{sizeof(devInfo)};
		if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, &devInfo)) continue;

		const wchar_t *path = det->DevicePath;
		if (!icontains(path, kAppleVid)) continue;

		HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
		                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
		if (h == INVALID_HANDLE_VALUE) continue;

		PHIDP_PREPARSED_DATA prep = nullptr;
		if (!HidD_GetPreparsedData(h, &prep)) { CloseHandle(h); continue; }

		HIDP_CAPS caps{};
		if (HidP_GetCaps(prep, &caps) != HIDP_STATUS_SUCCESS) { HidD_FreePreparsedData(prep); CloseHandle(h); continue; }

		USAGE uIllum = 0; UCHAR reportId = 0;
		USHORT n = caps.NumberInputValueCaps;
		if (n) {
			std::vector<HIDP_VALUE_CAPS> v(n);
			if (HidP_GetValueCaps(HidP_Input, v.data(), &n, prep) == HIDP_STATUS_SUCCESS) {
				for (USHORT k = 0; k < n; ++k) {
					if (v[k].UsagePage != HID_UP_SENSOR_PAGE || v[k].IsRange) continue;
					if (v[k].NotRange.Usage == HID_USG_ILLUMINANCE) { uIllum = v[k].NotRange.Usage; reportId = v[k].ReportID; break; }
				}
			}
		}

		if (!uIllum) { HidD_FreePreparsedData(prep); CloseHandle(h); continue; }

		AlsDevice d;
		d.hDev = h; d.prep = prep; d.inputLen = caps.InputReportByteLength;
		d.reportId = reportId; d.uIllum = uIllum; d.overlapped = true;
		d.devicePath = path;
		d.label = shortLabel(path);
		d.containerId = queryContainerIdFromDevinfo(set, &devInfo);

		LONG raw = 0; float lux = 0.f;
		bool okRead = d.readLux(&raw, &lux);
		Log::Info(L"als: illuminance sensor %s (reportId=0x%02X) testRead=%s raw=%ld lux=%.1f",
		          d.label.c_str(), reportId, okRead ? L"ok" : L"FAILED", okRead ? raw : -1, okRead ? lux : -1.f);
		result.push_back(std::move(d));
	}
	SetupDiDestroyDeviceInfoList(set);
	Log::Info(L"als: %zu illuminance sensor(s) found (raw HID)", result.size());
	return result;
}

/* ============================ Watcher + snapshot ============================ */

struct AlsSnap { GUID container; float lux; bool valid; };

static std::vector<AlsDevice> g_als;
static std::vector<AlsSnap>   g_alsSnap;      // parallel to g_als; read by als_get_lux
static std::mutex             g_alsSnapMtx;

// A HID sensor powers up in "No Events" and streams nothing until a host writes its
// Reporting State feature (usage 0x0316) = All Events (2). The Windows sensor mapper does
// this automatically; when we read the raw HID interface ourselves we must do it too, or
// every input read times out and no lux is ever produced. Same issue/fix as the WinUSB path.
static bool alsFeatureReportId(PHIDP_PREPARSED_DATA prep, USAGE usage, UCHAR *rid) {
	HIDP_CAPS caps{};
	if (HidP_GetCaps(prep, &caps) != HIDP_STATUS_SUCCESS || caps.NumberFeatureValueCaps == 0) return false;
	USHORT n = caps.NumberFeatureValueCaps;
	std::vector<HIDP_VALUE_CAPS> v(n);
	if (HidP_GetValueCaps(HidP_Feature, v.data(), &n, prep) != HIDP_STATUS_SUCCESS) return false;
	for (auto &c : v) {
		USAGE u = c.IsRange ? c.Range.UsageMin : c.NotRange.Usage;
		if (c.UsagePage == HID_UP_SENSOR_PAGE && u == usage) { if (rid) *rid = c.ReportID; return true; }
	}
	return false;
}

static void alsEnableReporting(AlsDevice &d) {
	if (d.hDev == INVALID_HANDLE_VALUE || !d.prep) return;
	HIDP_CAPS caps{};
	if (HidP_GetCaps(d.prep, &caps) != HIDP_STATUS_SUCCESS || caps.FeatureReportByteLength == 0) return;
	UCHAR rid = 0;
	if (!alsFeatureReportId(d.prep, 0x0316, &rid)) return;   // 0x0316 = Property: Reporting State
	std::vector<uint8_t> buf(caps.FeatureReportByteLength, 0);
	buf[0] = rid;
	HidD_GetFeature(d.hDev, buf.data(), (ULONG)buf.size());  // read-modify-write; ok if this fails
	HidP_SetUsageValue(HidP_Feature, HID_UP_SENSOR_PAGE, 0, 0x0316, 2, d.prep,   // 2 = All Events
	                   reinterpret_cast<PCHAR>(buf.data()), (ULONG)buf.size());
	UCHAR ridInt = 0;
	if (alsFeatureReportId(d.prep, 0x030E, &ridInt) && ridInt == rid)            // 0x030E = Report Interval
		HidP_SetUsageValue(HidP_Feature, HID_UP_SENSOR_PAGE, 0, 0x030E, 500, d.prep,
		                   reinterpret_cast<PCHAR>(buf.data()), (ULONG)buf.size());
	if (HidD_SetFeature(d.hDev, buf.data(), (ULONG)buf.size()))
		Log::Info(L"als: reporting enabled (%s)", d.label.c_str());
	else
		Log::Warn(L"als: enable reporting failed gle=%lu (%s)", GetLastError(), d.label.c_str());
}

void als_watch_init() {
	g_als = als_enumerate();
	for (auto &d : g_als) alsEnableReporting(d);
	std::lock_guard<std::mutex> lk(g_alsSnapMtx);
	g_alsSnap.clear();
	for (auto &d : g_als) g_alsSnap.push_back({d.containerId, 0.f, false});
}

void als_watch_tick() {
	for (size_t i = 0; i < g_als.size(); ++i) {
		LONG raw = 0; float lux = 0.f;
		if (!g_als[i].readLux(&raw, &lux)) continue;
		{
			std::lock_guard<std::mutex> lk(g_alsSnapMtx);
			if (i < g_alsSnap.size()) { g_alsSnap[i].lux = lux; g_alsSnap[i].valid = true; }
		}
		if (raw == g_als[i].lastRaw) continue;             // log only on change
		Log::Info(L"als: illuminance raw=%ld lux=%.1f (%s)", raw, lux, g_als[i].label.c_str());
		g_als[i].lastRaw = raw;
	}
}

void als_watch_shutdown() {
	{
		std::lock_guard<std::mutex> lk(g_alsSnapMtx);
		g_alsSnap.clear();
	}
	g_als.clear();
}

bool als_get_lux(const GUID *containerId, float *lux) {
	if (!lux) return false;
	std::lock_guard<std::mutex> lk(g_alsSnapMtx);
	static const GUID zero = {};
	if (containerId && memcmp(containerId, &zero, sizeof(GUID)) != 0) {
		for (const auto &s : g_alsSnap)
			if (s.valid && memcmp(&s.container, containerId, sizeof(GUID)) == 0) { *lux = s.lux; return true; }
		return false;   // no ContainerId-matched raw ALS
	}
	for (const auto &s : g_alsSnap)
		if (s.valid) { *lux = s.lux; return true; }   // master
	return false;
}
