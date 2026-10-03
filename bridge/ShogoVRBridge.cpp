// ======================================================================= //
//
// ShogoVRBridge.cpp  -  Shogo VR mod, stage 2/3
//
// A small OpenXR application that puts Shogo into your headset:
//
//   1. Every headset frame it reads your head pose and hands it to the
//      game through shared memory (see ShogoVRShared.h).  The game turns
//      its camera to match and renders a side-by-side stereo frame.
//
//   2. It captures the game window from the desktop (DXGI Desktop
//      Duplication), splits it into the left/right eye images and submits
//      them to the headset, telling the runtime the exact pose and field
//      of view each image was rendered with.  The runtime (SteamVR, Oculus,
//      ...) then handles lens distortion and reprojection.
//
// No OpenXR loader library is needed: the bridge reads the active OpenXR
// runtime from the registry and talks to it directly.
//
// Keys (work while the game has focus):  Ctrl+Shift+R = recentre
// Close with Ctrl+C or by closing the console window.
//
// ======================================================================= //

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"
#include "openxr/openxr_loader_negotiation.h"

#include "ShogoVRShared.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#endif

template <typename T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Everything printed also goes to ShogoVRBridge.log next to the bridge (with
// the time), except the once-a-second status line - handy for sending in.
#include <cstdarg>
#include <ctime>
static FILE* g_log = nullptr;
static void OpenLog()
{
	wchar_t exe[2048];
	DWORD n = GetModuleFileNameW(nullptr, exe, 2048);
	std::wstring path(exe, n);
	size_t slash = path.find_last_of(L"\\/");
	path = path.substr(0, slash == std::wstring::npos ? 0 : slash + 1) + L"ShogoVRBridge.log";
	g_log = _wfopen(path.c_str(), L"w");
}
static int LogPrintf(const char* fmt, ...)
{
	char buf[4096];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fputs(buf, stdout);
	if (g_log && buf[0] != '\r')
	{
		const char* p = buf;
		while (*p == '\n') { fputc('\n', g_log); ++p; }
		if (*p)
		{
			time_t t = time(nullptr);
			char ts[32];
			strftime(ts, sizeof(ts), "[%H:%M:%S] ", localtime(&t));
			fputs(ts, g_log);
			fputs(p, g_log);
		}
		fflush(g_log);
	}
	return n;
}
#define printf LogPrintf

static volatile bool g_quit = false;
static HANDLE g_quitEvent = nullptr;		// set by ShogoVR.exe when the game has exited

static HANDLE g_gameProcess = nullptr;	// set when the game started us: close when it ends

// True if the process runs with administrator rights ("elevated").
static bool IsElevated(HANDLE process)
{
	HANDLE token = nullptr;
	bool elevated = false;
	if (OpenProcessToken(process, TOKEN_QUERY, &token))
	{
		TOKEN_ELEVATION e = {};
		DWORD size = 0;
		if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size)) elevated = e.TokenIsElevated != 0;
		CloseHandle(token);
	}
	return elevated;
}

static void CheckQuitEvent()
{
	if (g_quitEvent && WaitForSingleObject(g_quitEvent, 0) == WAIT_OBJECT_0) g_quit = true;
	if (g_gameProcess && WaitForSingleObject(g_gameProcess, 0) == WAIT_OBJECT_0) g_quit = true;
}


// ======================================================================= //
//  OpenXR function table (loaded straight from the runtime)
// ======================================================================= //

#define SHOGOVR_XR_FUNCS(X) \
	X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrResultToString) X(xrGetSystem) \
	X(xrGetSystemProperties) X(xrEnumerateViewConfigurationViews) X(xrGetD3D11GraphicsRequirementsKHR) \
	X(xrCreateSession) X(xrDestroySession) X(xrCreateReferenceSpace) X(xrDestroySpace) \
	X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain) \
	X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) \
	X(xrReleaseSwapchainImage) X(xrPollEvent) X(xrBeginSession) X(xrEndSession) \
	X(xrRequestExitSession) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame) X(xrLocateViews) \
	X(xrLocateSpace) X(xrStringToPath) X(xrCreateActionSet) X(xrDestroyActionSet) X(xrCreateAction) \
	X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) X(xrSyncActions) \
	X(xrGetActionStateBoolean) X(xrGetActionStateVector2f) X(xrCreateActionSpace)

#define SHOGOVR_DECLARE_FN(name) static PFN_##name name = nullptr;
SHOGOVR_XR_FUNCS(SHOGOVR_DECLARE_FN)
static PFN_xrGetInstanceProcAddr					xrGetInstanceProcAddr_ = nullptr;
static PFN_xrCreateInstance							xrCreateInstance = nullptr;
static PFN_xrEnumerateInstanceExtensionProperties	xrEnumerateInstanceExtensionProperties = nullptr;

static XrInstance g_instance = XR_NULL_HANDLE;

static bool XrOk(XrResult r, const char* what)
{
	if (XR_SUCCEEDED(r)) return true;
	char buf[XR_MAX_RESULT_STRING_SIZE] = "";
	if (xrResultToString && g_instance != XR_NULL_HANDLE) xrResultToString(g_instance, r, buf);
	printf("\n[OpenXR] %s failed: %s (%d)\n", what, buf[0] ? buf : "error", (int)r);
	return false;
}

// Reads the "library_path" string out of the runtime's JSON manifest.
static bool ExtractLibraryPath(const std::string& json, std::string& out)
{
	size_t k = json.find("\"library_path\"");
	if (k == std::string::npos) return false;
	size_t q = json.find('"', json.find(':', k) + 1);
	if (q == std::string::npos) return false;

	out.clear();
	for (size_t i = q + 1; i < json.size(); ++i)
	{
		char c = json[i];
		if (c == '"') return true;
		if (c == '\\' && i + 1 < json.size())
		{
			char e = json[++i];
			if (e == 'n') c = '\n'; else if (e == 't') c = '\t'; else c = e;	// \\ \/ \"
		}
		out += c;
	}
	return false;
}

static std::wstring Utf8ToWide(const std::string& s)
{
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

static std::string WideToUtf8(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

static bool LoadActiveRuntime()
{
	// 1. Which runtime?  (XR_RUNTIME_JSON overrides the registry, like the real loader.)
	std::wstring jsonPath;
	wchar_t env[2048];
	DWORD envLen = GetEnvironmentVariableW(L"XR_RUNTIME_JSON", env, 2048);
	if (envLen > 0 && envLen < 2048)
	{
		jsonPath = env;
	}
	else
	{
		HKEY key = nullptr;
		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", 0, KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS)
		{
			wchar_t buf[2048]; DWORD size = sizeof(buf); DWORD type = 0;
			if (RegQueryValueExW(key, L"ActiveRuntime", nullptr, &type, (BYTE*)buf, &size) == ERROR_SUCCESS &&
				(type == REG_SZ || type == REG_EXPAND_SZ))
			{
				buf[2047] = 0;
				wchar_t expanded[2048];
				if (ExpandEnvironmentStringsW(buf, expanded, 2048)) jsonPath = expanded; else jsonPath = buf;
			}
			RegCloseKey(key);
		}
	}

	if (jsonPath.empty())
	{
		printf("No OpenXR runtime is set as active.\n"
			   "In SteamVR: Settings > OpenXR > 'Set SteamVR as OpenXR Runtime'.\n");
		return false;
	}
	printf("OpenXR runtime manifest: %s\n", WideToUtf8(jsonPath).c_str());

	// 2. Read the manifest and find the runtime DLL.
	HANDLE f = CreateFileW(jsonPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (f == INVALID_HANDLE_VALUE) { printf("Can't open the runtime manifest.\n"); return false; }
	std::string json;
	char chunk[4096]; DWORD got = 0;
	while (ReadFile(f, chunk, sizeof(chunk), &got, nullptr) && got > 0) json.append(chunk, got);
	CloseHandle(f);

	std::string lib;
	if (!ExtractLibraryPath(json, lib)) { printf("The runtime manifest has no library_path.\n"); return false; }

	std::wstring libPath = Utf8ToWide(lib);
	bool bAbsolute = (libPath.size() > 2 && libPath[1] == L':') || (libPath.size() > 1 && libPath[0] == L'\\' && libPath[1] == L'\\');
	if (!bAbsolute)
	{
		size_t slash = jsonPath.find_last_of(L"\\/");
		libPath = jsonPath.substr(0, slash == std::wstring::npos ? 0 : slash + 1) + libPath;
	}

	HMODULE runtime = LoadLibraryExW(libPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!runtime) { printf("Can't load the runtime DLL: %s (error %lu)\n", WideToUtf8(libPath).c_str(), GetLastError()); return false; }

	// 3. Negotiate, exactly like the official loader does.
	PFN_xrNegotiateLoaderRuntimeInterface negotiate =
		(PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(runtime, "xrNegotiateLoaderRuntimeInterface");
	if (!negotiate) { printf("The runtime DLL doesn't export xrNegotiateLoaderRuntimeInterface.\n"); return false; }

	XrNegotiateLoaderInfo loaderInfo = {};
	loaderInfo.structType			= XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
	loaderInfo.structVersion		= XR_LOADER_INFO_STRUCT_VERSION;
	loaderInfo.structSize			= sizeof(loaderInfo);
	loaderInfo.minInterfaceVersion	= 1;
	loaderInfo.maxInterfaceVersion	= XR_CURRENT_LOADER_RUNTIME_VERSION;
	loaderInfo.minApiVersion		= XR_MAKE_VERSION(1, 0, 0);
	loaderInfo.maxApiVersion		= XR_MAKE_VERSION(1, 0x3ff, 0xfff);

	XrNegotiateRuntimeRequest request = {};
	request.structType		= XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
	request.structVersion	= XR_RUNTIME_INFO_STRUCT_VERSION;
	request.structSize		= sizeof(request);

	if (XR_FAILED(negotiate(&loaderInfo, &request)) || !request.getInstanceProcAddr)
	{
		printf("Runtime negotiation failed.\n");
		return false;
	}

	xrGetInstanceProcAddr_ = request.getInstanceProcAddr;
	xrGetInstanceProcAddr_(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&xrCreateInstance);
	xrGetInstanceProcAddr_(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&xrEnumerateInstanceExtensionProperties);
	return xrCreateInstance && xrEnumerateInstanceExtensionProperties;
}

static bool LoadInstanceFunctions()
{
	bool ok = true;
#define SHOGOVR_LOAD_FN(name) \
	if (XR_FAILED(xrGetInstanceProcAddr_(g_instance, #name, (PFN_xrVoidFunction*)&name)) || !name) \
	{ printf("Runtime is missing %s\n", #name); ok = false; }
	SHOGOVR_XR_FUNCS(SHOGOVR_LOAD_FN)
#undef SHOGOVR_LOAD_FN
	return ok;
}


// ======================================================================= //
//  Small maths helpers (OpenXR space: right-handed, x right, y up, -z forward)
// ======================================================================= //

static XrQuaternionf QMul(const XrQuaternionf& a, const XrQuaternionf& b)
{
	XrQuaternionf r;
	r.w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z;
	r.x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y;
	r.y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x;
	r.z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w;
	return r;
}

static XrVector3f QRotate(const XrQuaternionf& q, const XrVector3f& v)
{
	// v' = v + 2w(u x v) + 2(u x (u x v)),  u = q.xyz
	XrVector3f u = { q.x, q.y, q.z };
	XrVector3f t = { 2.0f*(u.y*v.z - u.z*v.y), 2.0f*(u.z*v.x - u.x*v.z), 2.0f*(u.x*v.y - u.y*v.x) };
	XrVector3f r;
	r.x = v.x + q.w*t.x + (u.y*t.z - u.z*t.y);
	r.y = v.y + q.w*t.y + (u.z*t.x - u.x*t.z);
	r.z = v.z + q.w*t.z + (u.x*t.y - u.y*t.x);
	return r;
}

static XrQuaternionf QYaw(float angle)	// rotation about +Y
{
	XrQuaternionf q = { 0.0f, std::sin(angle * 0.5f), 0.0f, std::cos(angle * 0.5f) };
	return q;
}

static float YawOf(const XrQuaternionf& q)
{
	XrVector3f fwd = QRotate(q, XrVector3f{ 0.0f, 0.0f, -1.0f });
	return std::atan2(-fwd.x, -fwd.z);
}

static XrPosef IdentityPose()
{
	XrPosef p;
	p.orientation = XrQuaternionf{ 0.0f, 0.0f, 0.0f, 1.0f };
	p.position = XrVector3f{ 0.0f, 0.0f, 0.0f };
	return p;
}


// ======================================================================= //
//  Shared memory with the game
// ======================================================================= //

struct BridgeData
{
	long	poseId;
	float	fwd[3], up[3], pos[3];
	float	yaw, pitch;
	float	needTanX, needTanY;
	long	flags;
	long	ctrlFlags;
	long	buttons;
	float	moveX, moveY, turnX, turnY;
	float	rFwd[3], rUp[3], rPos[3];
	float	lFwd[3], lUp[3], lPos[3];
};

struct GameInfo
{
	bool		alive;
	HWND		hwnd;
	long		usedPoseId;
	long		viewMode;
	long		swapEyes;
	float		renderTanX, renderTanY;
	float		eyeSeparation;
	long		recenterRequest;
	long		renderWidth, renderHeight;
	long		clientWidth, clientHeight;
	long		surfaceWidth, surfaceHeight;
	long		layoutFlags, eyeHeight;
	long		hudX, hudY, hudW, hudH;
	float		hudAngle, hudDistance, screenAngle;
	DWORD		captureHandle;
	long		captureWidth, captureHeight, captureFormat, captureFrame;
};

class SharedLink
{
public:
	bool Open()
	{
		m_mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ShogoVRShared), SHOGOVR_SHARED_NAME);
		if (!m_mapping) { printf("Can't create shared memory (error %lu)\n", GetLastError()); return false; }
		bool bExisted = (GetLastError() == ERROR_ALREADY_EXISTS);

		m_p = (ShogoVRShared*)MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ShogoVRShared));
		if (!m_p) { printf("Can't map shared memory (error %lu)\n", GetLastError()); return false; }

		if (!bExisted || m_p->magic != SHOGOVR_MAGIC)
		{
			m_p->magic		= SHOGOVR_MAGIC;
			m_p->version	= SHOGOVR_VERSION;
			m_p->structSize	= sizeof(ShogoVRShared);
		}
		else if (m_p->version != SHOGOVR_VERSION || m_p->structSize != sizeof(ShogoVRShared))
		{
			printf("The game's CShell.dll is from a different version of the mod - rebuild both.\n");
			return false;
		}
		return true;
	}

	void Close()
	{
		if (m_p) { m_p->bridgeFlags = 0; UnmapViewOfFile(m_p); m_p = nullptr; }
		if (m_mapping) { CloseHandle(m_mapping); m_mapping = nullptr; }
	}

	void WriteBridge(const BridgeData& d)
	{
		InterlockedIncrement(&m_p->bridgeSeq);			// odd: writing
		MemoryBarrier();
		m_p->bridgeHeartbeat = m_p->bridgeHeartbeat + 1;
		m_p->bridgeFlags = d.flags;
		m_p->poseId = d.poseId;
		for (int i = 0; i < 3; ++i)
		{
			m_p->headForward[i]	= d.fwd[i];		m_p->headUp[i]	= d.up[i];		m_p->headPos[i]	= d.pos[i];
			m_p->rightForward[i]	= d.rFwd[i];	m_p->rightUp[i]	= d.rUp[i];		m_p->rightPos[i]	= d.rPos[i];
			m_p->leftForward[i]	= d.lFwd[i];	m_p->leftUp[i]	= d.lUp[i];		m_p->leftPos[i]	= d.lPos[i];
		}
		m_p->headYaw = d.yaw;
		m_p->headPitch = d.pitch;
		m_p->needTanX = d.needTanX;
		m_p->needTanY = d.needTanY;
		m_p->controllerFlags = d.ctrlFlags;
		m_p->buttons = d.buttons;
		m_p->moveX = d.moveX;	m_p->moveY = d.moveY;
		m_p->turnX = d.turnX;	m_p->turnY = d.turnY;
		MemoryBarrier();
		InterlockedIncrement(&m_p->bridgeSeq);			// even: done
	}

	void SetFlags(long flags)
	{
		InterlockedIncrement(&m_p->bridgeSeq);
		MemoryBarrier();
		m_p->bridgeHeartbeat = m_p->bridgeHeartbeat + 1;
		m_p->bridgeFlags = flags;
		MemoryBarrier();
		InterlockedIncrement(&m_p->bridgeSeq);
	}

	GameInfo ReadGame()
	{
		GameInfo g = {};
		ShogoVRShared s = {};
		for (int tries = 0; tries < 1000; ++tries)
		{
			long a = m_p->gameSeq;
			if (a & 1) { YieldProcessor(); continue; }
			MemoryBarrier();
			s.gameHeartbeat		= m_p->gameHeartbeat;
			s.gameHwnd			= m_p->gameHwnd;
			s.usedPoseId		= m_p->usedPoseId;
			s.viewMode			= m_p->viewMode;
			s.swapEyes			= m_p->swapEyes;
			s.renderTanX		= m_p->renderTanX;
			s.renderTanY		= m_p->renderTanY;
			s.eyeSeparation		= m_p->eyeSeparation;
			s.recenterRequest	= m_p->recenterRequest;
			s.renderWidth		= m_p->renderWidth;
			s.renderHeight		= m_p->renderHeight;
			s.clientWidth		= m_p->clientWidth;
			s.clientHeight		= m_p->clientHeight;
			s.surfaceWidth		= m_p->surfaceWidth;
			s.surfaceHeight		= m_p->surfaceHeight;
			s.layoutFlags		= m_p->layoutFlags;
			s.eyeHeight			= m_p->eyeHeight;
			s.hudX				= m_p->hudX;
			s.hudY				= m_p->hudY;
			s.hudW				= m_p->hudW;
			s.hudH				= m_p->hudH;
			s.hudAngle			= m_p->hudAngle;
			s.hudDistance		= m_p->hudDistance;
			s.screenAngle		= m_p->screenAngle;
			s.captureHandle		= (DWORD)m_p->captureHandle;
			s.captureWidth		= m_p->captureWidth;
			s.captureHeight		= m_p->captureHeight;
			s.captureFormat		= m_p->captureFormat;
			s.captureFrame		= m_p->captureFrame;
			MemoryBarrier();
			if (m_p->gameSeq == a) break;
		}

		// Alive = heartbeat moved within the last 1.5 seconds.
		ULONGLONG now = GetTickCount64();
		if (s.gameHeartbeat != m_lastBeat) { m_lastBeat = s.gameHeartbeat; m_lastBeatTime = now; }
		g.alive = (m_lastBeatTime != 0) && (now - m_lastBeatTime < 1500);

		g.hwnd				= (HWND)(ULONG_PTR)s.gameHwnd;
		g.usedPoseId		= s.usedPoseId;
		g.viewMode			= s.viewMode;
		g.swapEyes			= s.swapEyes;
		g.renderTanX		= s.renderTanX;
		g.renderTanY		= s.renderTanY;
		g.eyeSeparation		= s.eyeSeparation;
		g.recenterRequest	= s.recenterRequest;
		g.renderWidth		= s.renderWidth;
		g.renderHeight		= s.renderHeight;
		g.clientWidth		= s.clientWidth;
		g.clientHeight		= s.clientHeight;
		g.surfaceWidth		= s.surfaceWidth;
		g.surfaceHeight		= s.surfaceHeight;
		g.layoutFlags		= s.layoutFlags;
		g.eyeHeight			= s.eyeHeight;
		g.hudX = s.hudX;	g.hudY = s.hudY;	g.hudW = s.hudW;	g.hudH = s.hudH;
		g.hudAngle			= s.hudAngle;
		g.hudDistance		= s.hudDistance;
		g.screenAngle		= s.screenAngle;
		g.captureHandle		= s.captureHandle;
		g.captureWidth		= s.captureWidth;
		g.captureHeight		= s.captureHeight;
		g.captureFormat		= s.captureFormat;
		g.captureFrame		= s.captureFrame;
		if (!g.hwnd || !IsWindow(g.hwnd)) g.alive = false;
		return g;
	}

private:
	HANDLE			m_mapping = nullptr;
	ShogoVRShared*	m_p = nullptr;
	long			m_lastBeat = 0;
	ULONGLONG		m_lastBeatTime = 0;
};


// ======================================================================= //
//  Desktop capture of the game window
// ======================================================================= //

class WindowCapture
{
public:
	void Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGIAdapter1* adapter)
	{
		m_dev = dev; m_ctx = ctx; m_adapter = adapter;
	}

	void Shutdown()
	{
		ReleaseDuplication();
		SafeRelease(m_srv);
		SafeRelease(m_tex);
		m_w = m_h = 0;
	}

	// Returns true if a new image of the window was captured this call.
	bool Update(HWND hwnd)
	{
		if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) return false;

		HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
		if (mon != m_monitor) { ReleaseDuplication(); m_monitor = mon; }
		if (!m_dup && !CreateDuplication()) return false;

		// Where is the game's picture on that monitor?  It must be entirely
		// on screen: a cropped picture would split into the wrong left and
		// right halves, so rather keep showing the last good frame (the
		// window watchdog moves the window back).
		RECT rc;
		GetClientRect(hwnd, &rc);
		POINT tl = { rc.left, rc.top };
		ClientToScreen(hwnd, &tl);
		LONG left   = tl.x;
		LONG top    = tl.y;
		LONG right  = tl.x + (rc.right - rc.left);
		LONG bottom = tl.y + (rc.bottom - rc.top);
		if (right - left < 16 || bottom - top < 16) return false;
		if (left < m_outRect.left || top < m_outRect.top || right > m_outRect.right || bottom > m_outRect.bottom)
		{
			m_error = "the Shogo window is partly off-screen - moving it back";
			return false;
		}
		m_error = nullptr;

		UINT w = (UINT)(right - left), h = (UINT)(bottom - top);
		if (w != m_w || h != m_h || m_fmt != DXGI_FORMAT_B8G8R8A8_TYPELESS)
		{
			if (!CreateTexture(w, h)) return false;
		}

		DXGI_OUTDUPL_FRAME_INFO info;
		IDXGIResource* res = nullptr;
		HRESULT hr = m_dup->AcquireNextFrame(0, &info, &res);
		if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
		if (FAILED(hr))
		{
			// Mode change, fullscreen switch, UAC prompt...  rebuild next time.
			ReleaseDuplication();
			return false;
		}

		bool bNew = false;
		if (info.LastPresentTime.QuadPart != 0)		// 0 = only the mouse moved
		{
			ID3D11Texture2D* desk = nullptr;
			if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desk)))
			{
				D3D11_BOX box;
				box.left   = (UINT)(left - m_outRect.left);
				box.top    = (UINT)(top - m_outRect.top);
				box.right  = box.left + w;
				box.bottom = box.top + h;
				box.front  = 0;
				box.back   = 1;
				m_ctx->CopySubresourceRegion(m_tex, 0, 0, 0, 0, desk, 0, &box);
				desk->Release();
				bNew = true;
				m_hasImage = true;
			}
		}
		res->Release();
		m_dup->ReleaseFrame();
		return bNew;
	}

	ID3D11Texture2D*			Texture()	{ return m_tex; }
	ID3D11ShaderResourceView*	SRV()		{ return m_srv; }
	UINT	Width() const		{ return m_w; }
	UINT	Height() const		{ return m_h; }
	bool	HasImage() const	{ return m_hasImage && m_tex; }
	bool	IsBGRA() const		{ return m_fmt == DXGI_FORMAT_B8G8R8A8_TYPELESS; }
	bool	IsDirect() const	{ return m_shared != nullptr; }
	bool	SharedFailed(DWORD handle) const { return handle == m_sharedFailed; }
	const char* SharedError() const { return m_sharedError; }

	// Direct capture: the game's renderer copies each finished frame into a
	// shared texture (the game's VRCapture.cpp).  Returns true for a new frame.
	bool UpdateShared(DWORD handle)
	{
		if (!handle) { CloseShared(); return false; }
		if (handle != m_sharedHandle)
		{
			CloseShared();
			ID3D11Texture2D* t = nullptr;
			if (FAILED(m_dev->OpenSharedResource((HANDLE)(ULONG_PTR)handle, __uuidof(ID3D11Texture2D), (void**)&t)) || !t)
			{
				m_sharedFailed = handle;
				m_sharedError = "the game renders on a different graphics card than the headset uses";
				return false;
			}
			D3D11_TEXTURE2D_DESC d;
			t->GetDesc(&d);
			DXGI_FORMAT typeless, view;
			switch (d.Format)
			{
			case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
				typeless = DXGI_FORMAT_B8G8R8A8_TYPELESS; view = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; break;
			case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				typeless = DXGI_FORMAT_R8G8B8A8_TYPELESS; view = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; break;
			default:
				t->Release();
				m_sharedFailed = handle;
				m_sharedError = "the game's picture format isn't supported";
				return false;
			}
			if (FAILED(t->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&m_sharedMutex)))
			{
				t->Release();
				m_sharedFailed = handle;
				m_sharedError = "the shared picture has no lock";
				return false;
			}
			m_shared = t;
			m_sharedHandle = handle;
			m_sharedTypeless = typeless;
			m_sharedView = view;
			ReleaseDuplication();			// screen capture no longer needed
		}

		D3D11_TEXTURE2D_DESC d;
		m_shared->GetDesc(&d);
		if (!m_tex || m_w != d.Width || m_h != d.Height || m_fmt != m_sharedTypeless)
			if (!CreateTexture(d.Width, d.Height, m_sharedTypeless, m_sharedView)) return false;

		if (m_sharedMutex->AcquireSync(1, 0) != S_OK) return false;		// no new frame yet
		m_ctx->CopyResource(m_tex, m_shared);
		m_sharedMutex->ReleaseSync(0);
		m_hasImage = true;
		return true;
	}

	void CloseShared()
	{
		SafeRelease(m_sharedMutex);
		SafeRelease(m_shared);
		m_sharedHandle = 0;
	}
	const char* LastError() const { return m_error; }

private:
	bool CreateDuplication()
	{
		m_error = nullptr;
		IDXGIOutput* out = nullptr;
		for (UINT i = 0; m_adapter->EnumOutputs(i, &out) != DXGI_ERROR_NOT_FOUND; ++i)
		{
			DXGI_OUTPUT_DESC desc;
			out->GetDesc(&desc);
			if (desc.Monitor == m_monitor)
			{
				IDXGIOutput1* out1 = nullptr;
				if (SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput1), (void**)&out1)))
				{
					HRESULT hr = out1->DuplicateOutput(m_dev, &m_dup);
					if (FAILED(hr))
					{
						m_error = (hr == E_ACCESSDENIED) ? "Windows refused desktop capture right now (secure desktop / UAC prompt?)"
								: (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) ? "too many apps are capturing this screen"
								: "desktop capture isn't available on this screen";
						m_dup = nullptr;
					}
					else
					{
						m_outRect = desc.DesktopCoordinates;
					}
					out1->Release();
				}
				out->Release();
				return m_dup != nullptr;
			}
			out->Release();
		}
		m_error = "the game window is on a monitor connected to a different graphics card than the headset";
		return false;
	}

	void ReleaseDuplication()
	{
		SafeRelease(m_dup);
	}

	bool CreateTexture(UINT w, UINT h, DXGI_FORMAT typeless = DXGI_FORMAT_B8G8R8A8_TYPELESS,
					   DXGI_FORMAT srgbView = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
	{
		SafeRelease(m_srv);
		SafeRelease(m_tex);
		m_hasImage = false;

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = typeless;
		m_fmt = typeless;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &m_tex))) return false;

		D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
		sd.Format = srgbView;							// game/desktop pixels are sRGB
		sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MipLevels = 1;
		if (FAILED(m_dev->CreateShaderResourceView(m_tex, &sd, &m_srv))) return false;

		m_w = w; m_h = h;
		return true;
	}

	ID3D11Device*				m_dev = nullptr;
	ID3D11DeviceContext*		m_ctx = nullptr;
	IDXGIAdapter1*				m_adapter = nullptr;
	IDXGIOutputDuplication*		m_dup = nullptr;
	HMONITOR					m_monitor = nullptr;
	RECT						m_outRect = {};
	ID3D11Texture2D*			m_tex = nullptr;
	ID3D11ShaderResourceView*	m_srv = nullptr;
	UINT						m_w = 0, m_h = 0;
	DXGI_FORMAT					m_fmt = DXGI_FORMAT_B8G8R8A8_TYPELESS;
	ID3D11Texture2D*			m_shared = nullptr;
	IDXGIKeyedMutex*			m_sharedMutex = nullptr;
	DWORD						m_sharedHandle = 0, m_sharedFailed = 0;
	DXGI_FORMAT					m_sharedTypeless = DXGI_FORMAT_UNKNOWN, m_sharedView = DXGI_FORMAT_UNKNOWN;
	const char*					m_sharedError = "";
	bool						m_hasImage = false;
	const char*					m_error = nullptr;
};


// ======================================================================= //
//  Copying an eye's half into a swapchain image
// ======================================================================= //

// Image processing on the way to the headset:
//   ps_cas : contrast-adaptive sharpening (after AMD FidelityFX CAS, MIT
//            licence) at the game's resolution, never mixing the two eyes.
//   ps_up  : Catmull-Rom bicubic resampling of one eye's half into the
//            headset image - an exact copy at 1:1, a sharp upscale above it.
// Both work in linear light (sRGB views decode/encode automatically).
static const char* g_blitShader =
	"cbuffer P : register(b0) { float4 uvRect; float4 texInfo; float4 casInfo; };\n"
	"Texture2D tex : register(t0);\n"
	"SamplerState smp : register(s0);\n"
	"struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
	"V vs(uint id : SV_VertexID) {\n"
	"  V o; float2 t = float2((id << 1) & 2, id & 2);\n"
	"  o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);\n"
	"  o.uv = t; return o; }\n"
	"\n"
	"float3 LD(int2 p, int x0, int x1, int yMax) {\n"
	"  return tex.Load(int3(clamp(p.x, x0, x1), clamp(p.y, 0, yMax), 0)).rgb; }\n"
	"\n"
	"float3 LD2(int2 p, int x0, int x1, int y0, int y1) {\n"
	"  return tex.Load(int3(clamp(p.x, x0, x1), clamp(p.y, y0, y1), 0)).rgb; }\n"
	"\n"
	"float4 ps_cas(V i) : SV_Target {\n"
	"  int2 p = int2(i.pos.xy);\n"
	"  int halfW = (int)casInfo.y;\n"
	"  int x0 = (p.x >= halfW) ? halfW : 0;\n"
	"  int x1 = min(x0 + halfW - 1, (int)texInfo.x - 1);\n"
	"  int eyeH = (int)casInfo.w;\n"
	"  int y0 = (p.y >= eyeH) ? eyeH : 0;\n"
	"  int y1 = (p.y >= eyeH) ? (int)texInfo.y - 1 : eyeH - 1;\n"
	"  float3 b = LD2(p + int2(0, -1), x0, x1, y0, y1);\n"
	"  float3 d = LD2(p + int2(-1, 0), x0, x1, y0, y1);\n"
	"  float3 e = LD2(p, x0, x1, y0, y1);\n"
	"  float3 f = LD2(p + int2(1, 0), x0, x1, y0, y1);\n"
	"  float3 h = LD2(p + int2(0, 1), x0, x1, y0, y1);\n"
	"  float3 mn = min(min(min(d, e), min(f, b)), h);\n"
	"  float3 mx = max(max(max(d, e), max(f, b)), h);\n"
	"  float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-5)));\n"
	"  float3 w = amp * (-1.0 / lerp(8.0, 5.0, saturate(casInfo.x)));\n"
	"  float3 c = (b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w);\n"
	"  return float4(saturate(c), 1); }\n"
	"\n"
	"float3 SL(float2 uv) {\n"
	"  uv = float2(clamp(uv.x, texInfo.z, texInfo.w), min(uv.y, casInfo.z));\n"
	"  return tex.SampleLevel(smp, uv, 0).rgb; }\n"
	"\n"
	"float4 ps_up(V i) : SV_Target {\n"
	"  float2 texSize = texInfo.xy;\n"
	"  float2 uv = uvRect.xy + i.uv * uvRect.zw;\n"
	"  float2 sp = uv * texSize;\n"
	"  float2 t1 = floor(sp - 0.5) + 0.5;\n"
	"  float2 f = sp - t1;\n"
	"  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));\n"
	"  float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
	"  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));\n"
	"  float2 w3 = f * f * (-0.5 + 0.5 * f);\n"
	"  float2 w12 = w1 + w2;\n"
	"  float2 t0 = (t1 - 1.0) / texSize;\n"
	"  float2 t3 = (t1 + 2.0) / texSize;\n"
	"  float2 t12 = (t1 + w2 / w12) / texSize;\n"
	"  float3 c = 0;\n"
	"  c += SL(float2(t0.x,  t0.y))  * w0.x  * w0.y;\n"
	"  c += SL(float2(t12.x, t0.y))  * w12.x * w0.y;\n"
	"  c += SL(float2(t3.x,  t0.y))  * w3.x  * w0.y;\n"
	"  c += SL(float2(t0.x,  t12.y)) * w0.x  * w12.y;\n"
	"  c += SL(float2(t12.x, t12.y)) * w12.x * w12.y;\n"
	"  c += SL(float2(t3.x,  t12.y)) * w3.x  * w12.y;\n"
	"  c += SL(float2(t0.x,  t3.y))  * w0.x  * w3.y;\n"
	"  c += SL(float2(t12.x, t3.y))  * w12.x * w3.y;\n"
	"  c += SL(float2(t3.x,  t3.y))  * w3.x  * w3.y;\n"
	"  if (casInfo.w > 0.001) {\n"
	"    float r = length((i.uv - 0.5) * 2.0);\n"
	"    float inner = lerp(1.25, 0.35, casInfo.w);\n"
	"    c *= 1.0 - smoothstep(inner, inner + 0.3, r); }\n"
	"  return float4(saturate(c), 1); }\n"
	"\n"
	"// HUD strip: nearest pixel (keeps Shogo's pixel-art text crisp), black becomes see-through\n"
	"float4 ps_hud(V i) : SV_Target {\n"
	"  float2 uv = uvRect.xy + i.uv * uvRect.zw;\n"
	"  int2 p = int2(uv * texInfo.xy);\n"
	"  float3 c = tex.Load(int3(p, 0)).rgb;\n"
	"  float a = saturate(max(c.r, max(c.g, c.b)) * 8.0);\n"
	"  return float4(c * a, a); }\n";

class Blitter
{
public:
	bool Init(ID3D11Device* dev, ID3D11DeviceContext* ctx)
	{
		m_dev = dev; m_ctx = ctx;
		size_t len = strlen(g_blitShader);
		ID3DBlob *vsb = nullptr, *casb = nullptr, *upb = nullptr, *hudb = nullptr, *err = nullptr;
		HRESULT hr = D3DCompile(g_blitShader, len, "blit", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err);
		if (SUCCEEDED(hr)) hr = D3DCompile(g_blitShader, len, "blit", nullptr, nullptr, "ps_cas", "ps_4_0", 0, 0, &casb, &err);
		if (SUCCEEDED(hr)) hr = D3DCompile(g_blitShader, len, "blit", nullptr, nullptr, "ps_up", "ps_4_0", 0, 0, &upb, &err);
		if (SUCCEEDED(hr)) hr = D3DCompile(g_blitShader, len, "blit", nullptr, nullptr, "ps_hud", "ps_4_0", 0, 0, &hudb, &err);
		if (FAILED(hr))
		{
			if (err) { printf("Shader error: %s\n", (const char*)err->GetBufferPointer()); err->Release(); }
			SafeRelease(vsb); SafeRelease(casb); SafeRelease(upb); SafeRelease(hudb);
			return false;
		}
		dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &m_vs);
		dev->CreatePixelShader(casb->GetBufferPointer(), casb->GetBufferSize(), nullptr, &m_psCas);
		dev->CreatePixelShader(upb->GetBufferPointer(), upb->GetBufferSize(), nullptr, &m_psUp);
		dev->CreatePixelShader(hudb->GetBufferPointer(), hudb->GetBufferSize(), nullptr, &m_psHud);
		vsb->Release(); casb->Release(); upb->Release(); hudb->Release();

		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = 48; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		dev->CreateBuffer(&bd, nullptr, &m_cb);

		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;		// bilinear taps, combined into bicubic
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		dev->CreateSamplerState(&sd, &m_smp);
		D3D11_BLEND_DESC blendDesc = {};
		blendDesc.RenderTarget[0].BlendEnable = TRUE;
		blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;				// premultiplied alpha
		blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		dev->CreateBlendState(&blendDesc, &m_blendPremul);
		return m_vs && m_psCas && m_psUp && m_psHud && m_cb && m_smp && m_blendPremul;
	}

	void Shutdown()
	{
		SafeRelease(m_vs); SafeRelease(m_psCas); SafeRelease(m_psUp); SafeRelease(m_psHud); SafeRelease(m_cb); SafeRelease(m_smp);
		SafeRelease(m_blendPremul);
		SafeRelease(m_sharpRtv); SafeRelease(m_sharpSrv); SafeRelease(m_sharpTex);
	}

	// Sharpens the whole side-by-side picture into an internal texture and
	// returns it (or the input unchanged if that fails).
	ID3D11ShaderResourceView* Sharpen(ID3D11ShaderResourceView* src, UINT w, UINT h, UINT halfW, UINT eyeH, float amount)
	{
		if (!EnsureSharpTarget(w, h)) return src;
		float cb[12] = { 0, 0, 1, 1,  (float)w, (float)h, 0, 1,  amount, (float)halfW, 0, (float)eyeH };
		Run(m_psCas, m_sharpRtv, w, h, src, cb);
		return m_sharpSrv;
	}

	// Resamples the part [u0, u0+uw] x [0, vh] of src into rtv (dstW x dstH).
	void Draw(ID3D11RenderTargetView* rtv, UINT dstW, UINT dstH, ID3D11ShaderResourceView* src,
			  UINT srcW, UINT srcH, float u0, float uw, float vh = 1.0f, float vignette = 0.0f)
	{
		float halfTexel = 0.5f / (float)srcW;
		float vMax = vh - 0.5f / (float)srcH;
		float cb[12] = { u0, 0, uw, vh,  (float)srcW, (float)srcH, u0 + halfTexel, u0 + uw - halfTexel,  0, 0, vMax, vignette };
		Run(m_psUp, rtv, dstW, dstH, src, cb);
	}

	// Resamples the rectangle [u0,u0+uw] x [v0,v0+vh] of src to fill rtv,
	// never sampling outside [uMin,uMax] x [.., vMax] (the eye's own area).
	void DrawRegion(ID3D11RenderTargetView* rtv, UINT dstW, UINT dstH, ID3D11ShaderResourceView* src, UINT srcW, UINT srcH,
					float u0, float v0, float uw, float vh, float uMin, float uMax, float vMax)
	{
		float cb[12] = { u0, v0, uw, vh,  (float)srcW, (float)srcH, uMin + 0.5f / srcW, uMax - 0.5f / srcW,
						 0, 0, vMax - 0.5f / srcH, 0 };
		Run(m_psUp, rtv, dstW, dstH, src, cb);
	}

	// The HUD strip, blended over whatever is in rtv, in the given rectangle.
	void DrawHudAt(ID3D11RenderTargetView* rtv, UINT rtW, UINT rtH, float x, float y, float w, float h,
				   ID3D11ShaderResourceView* src, UINT srcW, UINT srcH, UINT hx, UINT hy, UINT hw, UINT hh)
	{
		float cb[12] = { (float)hx / srcW, (float)hy / srcH, (float)hw / srcW, (float)hh / srcH,
						 (float)srcW, (float)srcH, 0, 1,  0, 0, 1, 0 };
		D3D11_VIEWPORT vp = { x, y, w, h, 0.0f, 1.0f };
		Run(m_psHud, rtv, rtW, rtH, src, cb, &vp, m_blendPremul);
	}

	// Copies a pixel rectangle of src into rtv with nearest-pixel scaling and
	// black made transparent (premultiplied alpha) - for the HUD strip.
	void DrawHud(ID3D11RenderTargetView* rtv, UINT dstW, UINT dstH, ID3D11ShaderResourceView* src,
				 UINT srcW, UINT srcH, UINT x, UINT y, UINT w, UINT h)
	{
		float cb[12] = { (float)x / srcW, (float)y / srcH, (float)w / srcW, (float)h / srcH,
						 (float)srcW, (float)srcH, 0, 1,  0, 0, 1, 0 };
		Run(m_psHud, rtv, dstW, dstH, src, cb);
	}

private:
	bool EnsureSharpTarget(UINT w, UINT h)
	{
		if (m_sharpTex && w == m_sharpW && h == m_sharpH) return true;
		SafeRelease(m_sharpRtv); SafeRelease(m_sharpSrv); SafeRelease(m_sharpTex);

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &m_sharpTex))) return false;

		D3D11_RENDER_TARGET_VIEW_DESC rd = {};
		rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
		sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MipLevels = 1;
		if (FAILED(m_dev->CreateRenderTargetView(m_sharpTex, &rd, &m_sharpRtv)) ||
			FAILED(m_dev->CreateShaderResourceView(m_sharpTex, &sd, &m_sharpSrv))) return false;

		m_sharpW = w; m_sharpH = h;
		return true;
	}

	void Run(ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv, UINT w, UINT h, ID3D11ShaderResourceView* src, const float cb[12],
			 const D3D11_VIEWPORT* pvp = nullptr, ID3D11BlendState* blend = nullptr)
	{
		m_ctx->UpdateSubresource(m_cb, 0, nullptr, cb, 0, 0);
		D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
		m_ctx->RSSetViewports(1, pvp ? pvp : &vp);
		float blendFactor[4] = { 0, 0, 0, 0 };
		m_ctx->OMSetBlendState(blend, blendFactor, 0xffffffff);
		m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
		m_ctx->IASetInputLayout(nullptr);
		m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		m_ctx->VSSetShader(m_vs, nullptr, 0);
		m_ctx->PSSetShader(ps, nullptr, 0);
		m_ctx->PSSetConstantBuffers(0, 1, &m_cb);
		m_ctx->PSSetShaderResources(0, 1, &src);
		m_ctx->PSSetSamplers(0, 1, &m_smp);
		m_ctx->Draw(3, 0);
		ID3D11ShaderResourceView* nullSrv = nullptr;
		m_ctx->PSSetShaderResources(0, 1, &nullSrv);
		ID3D11RenderTargetView* nullRtv = nullptr;
		m_ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
		if (blend) m_ctx->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
	}

	ID3D11Device*				m_dev = nullptr;
	ID3D11DeviceContext*		m_ctx = nullptr;
	ID3D11VertexShader*			m_vs = nullptr;
	ID3D11PixelShader*			m_psCas = nullptr;
	ID3D11PixelShader*			m_psUp = nullptr;
	ID3D11PixelShader*			m_psHud = nullptr;
	ID3D11BlendState*			m_blendPremul = nullptr;
	ID3D11Buffer*				m_cb = nullptr;
	ID3D11SamplerState*			m_smp = nullptr;
	ID3D11Texture2D*			m_sharpTex = nullptr;
	ID3D11RenderTargetView*		m_sharpRtv = nullptr;
	ID3D11ShaderResourceView*	m_sharpSrv = nullptr;
	UINT						m_sharpW = 0, m_sharpH = 0;
};


// ======================================================================= //
//  The bridge
// ======================================================================= //

// ======================================================================= //
//  Spectator window: what your left eye sees, flat, on the desktop - for
//  recording or streaming.  Ctrl+Shift+M shows/hides it.
// ======================================================================= //

class Spectator
{
public:
	bool IsOpen() const { return m_hwnd != nullptr; }
	bool WantsClose() const { return m_wantClose; }

	bool Open(ID3D11Device* dev)
	{
		if (m_hwnd) return true;
		m_dev = dev;
		m_wantClose = false;
		WNDCLASSW wc = {};
		wc.lpfnWndProc = Proc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wc.hbrBackground = nullptr;			// painted by Direct3D
		wc.lpszClassName = L"ShogoVRSpectator";
		RegisterClassW(&wc);
		RECT rc = { 0, 0, 1280, 720 };
		AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
		m_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Shogo VR - left eye (for recording)", WS_OVERLAPPEDWINDOW,
								 CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
		if (!m_hwnd) return false;
		SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);

		IDXGIDevice* dxDev = nullptr;
		IDXGIAdapter* adapter = nullptr;
		IDXGIFactory2* factory = nullptr;
		bool ok = SUCCEEDED(m_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxDev)) &&
				  SUCCEEDED(dxDev->GetAdapter(&adapter)) &&
				  SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory));
		if (ok)
		{
			DXGI_SWAP_CHAIN_DESC1 sd = {};
			sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			sd.SampleDesc.Count = 1;
			sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			sd.BufferCount = 2;
			sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			ok = SUCCEEDED(factory->CreateSwapChainForHwnd(m_dev, m_hwnd, &sd, nullptr, nullptr, &m_sc));
			if (ok) factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
		}
		SafeRelease(factory); SafeRelease(adapter); SafeRelease(dxDev);
		if (!ok) { Close(); return false; }
		ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
		return true;
	}

	void Close()
	{
		SafeRelease(m_rtv);
		SafeRelease(m_sc);
		if (m_hwnd) DestroyWindow(m_hwnd);
		m_hwnd = nullptr;
		m_w = m_h = 0;
	}

	static void Pump()
	{
		MSG m;
		while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
	}

	ID3D11RenderTargetView* Begin(UINT& w, UINT& h)
	{
		if (!m_sc || IsIconic(m_hwnd)) return nullptr;
		RECT rc;
		GetClientRect(m_hwnd, &rc);
		w = std::max(1L, rc.right);
		h = std::max(1L, rc.bottom);
		if (w != m_w || h != m_h)
		{
			SafeRelease(m_rtv);
			if (FAILED(m_sc->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) return nullptr;
			m_w = w; m_h = h;
		}
		if (!m_rtv)
		{
			ID3D11Texture2D* bb = nullptr;
			if (FAILED(m_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return nullptr;
			D3D11_RENDER_TARGET_VIEW_DESC rd = {};
			rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
			rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			m_dev->CreateRenderTargetView(bb, &rd, &m_rtv);
			bb->Release();
		}
		return m_rtv;
	}

	void Present() { if (m_sc) m_sc->Present(0, 0); }		// never waits: the headset comes first

private:
	static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
	{
		if (msg == WM_CLOSE)
		{
			Spectator* s = (Spectator*)GetWindowLongPtrW(h, GWLP_USERDATA);
			if (s) s->m_wantClose = true;
			return 0;
		}
		return DefWindowProcW(h, msg, wp, lp);
	}

	ID3D11Device*			m_dev = nullptr;
	HWND					m_hwnd = nullptr;
	IDXGISwapChain1*		m_sc = nullptr;
	ID3D11RenderTargetView*	m_rtv = nullptr;
	UINT					m_w = 0, m_h = 0;
	bool					m_wantClose = false;
};

// Controller actions (see SetupActions)
enum { A_MOVE, A_TURN, A_FIRE, A_JUMP, A_CROUCH, A_MENU, A_LOG, A_WEAPONS, A_NEXT, A_ALT, A_RECENTER, A_TRANSFORM, A_RAIM, A_LAIM, A_COUNT };
struct ActionDef { const char* name; XrActionType type; const char* label; };
struct BindDef { int action; std::string path; };
struct ProfileLayout { std::string profile; std::vector<BindDef> binds; };
struct HandLayout
{
	XrActionSet	set = XR_NULL_HANDLE;
	XrAction	act[A_COUNT] = {};
	XrSpace		gunSpace = XR_NULL_HANDLE;
	XrSpace		offSpace = XR_NULL_HANDLE;
};

struct EyeSwapchain
{
	XrSwapchain								handle = XR_NULL_HANDLE;
	std::vector<XrSwapchainImageD3D11KHR>	images;
	std::vector<ID3D11RenderTargetView*>	rtvs;
	UINT									w = 0, h = 0;
};

struct PoseRecord
{
	long	id;
	XrPosef	pose;	// raw pose, in the LOCAL space we submit in
};

class Bridge
{
public:
	bool Init();
	void Run();
	void Shutdown();

private:
	bool CreateInstanceAndSystem();
	bool CreateDevice();
	bool CreateSession();
	void DestroySwapchains();
	bool CreateSwapchains(UINT w, UINT h);
	bool MakeSwapchain(EyeSwapchain& e, UINT w, UINT h);
	void FreeSwapchain(EyeSwapchain& e);
	bool CopyHud(const GameInfo& g);
	bool CopyScreen(const GameInfo& g);
	bool CaptureFrame(const GameInfo& g);
	void DrawSpectator(const GameInfo& g);
	void PollEvents();
	void Frame();
	void PublishPose(XrTime time);
	void Recenter();
	bool CopyEyes(const GameInfo& g);
	void PrintStatus(const GameInfo& g);
	bool SetupActions();
	void PollInput(XrTime time, BridgeData& d);
	void ToGameSpace(const XrPosef& raw, float fwd[3], float up[3], float pos[3]);
	void HandleWindow(const GameInfo& g);
	void FixGameWindow(const GameInfo& g, bool bBringToFront);
	void FixDpiScaling(const GameInfo& g);
	struct WindowPlan
	{
		int		mode;				// 0 normal window, 1 borderless, 2 shrunk to fit
		LONG	style, exStyle;
		LONG	clientW, clientH;
		LONG	x, y, outerW, outerH;
	};
	bool PlanWindow(const GameInfo& g, WindowPlan& p);
	void WatchWindow(const GameInfo& g);
	void KeepFocus(const GameInfo& g);
	void LoadSettings(bool bFirst = true);
	void SaveSettings();
	void CheckSettingsFile();

	SharedLink		m_link;
	WindowCapture	m_capture;
	Blitter			m_blit;

	XrSystemId		m_system = XR_NULL_SYSTEM_ID;
	XrSession		m_session = XR_NULL_HANDLE;
	XrSpace			m_localSpace = XR_NULL_HANDLE;
	XrSpace			m_viewSpace = XR_NULL_HANDLE;
	XrSessionState	m_state = XR_SESSION_STATE_UNKNOWN;
	bool			m_running = false;
	bool			m_exitRequested = false;

	IDXGIAdapter1*			m_adapter = nullptr;
	ID3D11Device*			m_dev = nullptr;
	ID3D11DeviceContext*	m_ctx = nullptr;

	int64_t			m_format = 0;
	bool			m_copyPath = false;	// true = CopySubresourceRegion, false = shader blit
	EyeSwapchain	m_eyes[2];
	bool			m_eyesHaveImage = false;
	EyeSwapchain	m_hud;						// the HUD strip panel
	bool			m_hudHasImage = false;
	EyeSwapchain	m_screen;					// the flat floating screen (menus, cutscenes)
	bool			m_screenHasImage = false;

	// Pose bookkeeping
	XrQuaternionf	m_centerInv = { 0.0f, 0.0f, 0.0f, 1.0f };
	XrVector3f		m_centerPos = { 0.0f, 0.0f, 0.0f };
	bool			m_centered = false;
	XrPosef			m_lastRaw = IdentityPose();
	bool			m_haveRaw = false;
	long			m_poseId = 0;
	PoseRecord		m_history[256] = {};
	float			m_needTanX = 1.0f, m_needTanY = 1.0f;

	// What the last captured image was rendered with
	GameInfo		m_shown = {};
	bool			m_screenFrozen = false;
	XrPosef			m_screenPose = IdentityPose();

	// Controllers
	HandLayout		m_hands[2];					// [0] right-handed, [1] left-handed layout
	bool			m_actionsReady = false;
	bool			m_leftHanded = false;
	bool			m_directCapture = true;		// take frames straight from the game's renderer when it offers them
	bool			m_usingDirect = false;
	bool			m_oversize = false;			// window as big as the game's resolution, even beyond the screen
	bool			m_spectatorOn = false;		// off unless wanted (Ctrl+Shift+M / settings)
	bool			m_spectatorKeyDown = false;
	Spectator		m_spectator;
	float			m_motion = 0.0f;			// 0..1 how hard the sticks move/turn you
	float			m_vignetteNow = 0.0f;		// smoothed
	float			m_vignette = 0.0f;			// comfort vignette strength setting 0..1
	ULONGLONG		m_lastIniCheck = 0;
	FILETIME		m_iniTime = {};
	bool			m_recenterBtnDown = false;

	// Game window handling
	bool			m_gameWasAlive = false;
	ULONGLONG		m_gameAliveSince = 0;
	bool			m_windowFixedOnce = false;
	long			m_lastRenderW = 0, m_lastRenderH = 0;
	bool			m_dpiWarned = false;
	bool			m_dpiVirtualized = false;
	bool			m_focusKeyDown = false;
	bool			m_dpiKeyDown = false;
	ULONGLONG		m_lastAliveTime = 0;
	bool			m_borderless = false;		// we removed the window's frame
	LONG			m_savedStyle = 0, m_savedExStyle = 0;
	ULONGLONG		m_lastWatch = 0, m_badSince = 0, m_lastCorrection = 0;
	int				m_corrections = 0;
	bool			m_watchGaveUp = false;
	bool			m_keepFocus = true;			// take focus back from SteamVR / consoles
	ULONGLONG		m_focusLostSince = 0, m_lastFocusFix = 0;
	long			m_lastSurfW = -1, m_lastSurfH = -1;

	// Picture quality
	float			m_sharpen = 0.5f;			// 0 = off .. 1 = strong
	bool			m_upscale = true;			// upscale eye images towards the headset's resolution
	uint32_t		m_recW = 0, m_recH = 0;		// runtime's recommended eye image size
	bool			m_sharpKeyDown = false, m_upKeyDown = false;
	bool			m_blitOk = false;
	std::wstring	m_iniPath;

	long			m_lastRecenterRequest = 0;
	bool			m_recenterKeyDown = false;
	ULONGLONG		m_lastStatus = 0;
	int				m_capturedFrames = 0;
	int				m_submittedFrames = 0;
};

bool Bridge::Init()
{
	if (!m_link.Open()) return false;
	if (!LoadActiveRuntime()) return false;
	if (!CreateInstanceAndSystem()) return false;
	if (!CreateDevice()) return false;
	LoadSettings();
	m_blitOk = m_blit.Init(m_dev, m_ctx);
	if (!m_blitOk) printf("(Image processing unavailable - sharpening and upscaling are off.)\n");
	m_capture.Init(m_dev, m_ctx, m_adapter);
	if (!CreateSession()) return false;
	if (!SetupActions()) printf("Controllers unavailable - keyboard and mouse still work.\n");
	return true;
}

bool Bridge::CreateInstanceAndSystem()
{
	// The runtime must support D3D11.
	uint32_t count = 0;
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
	std::vector<XrExtensionProperties> exts(count, { XR_TYPE_EXTENSION_PROPERTIES });
	xrEnumerateInstanceExtensionProperties(nullptr, count, &count, exts.data());
	bool bD3D11 = false;
	for (auto& e : exts) if (strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) bD3D11 = true;
	if (!bD3D11) { printf("This OpenXR runtime doesn't support Direct3D 11.\n"); return false; }

	const char* enabled[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
	XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
	strcpy_s(ci.applicationInfo.applicationName, "Shogo VR");
	ci.applicationInfo.applicationVersion = 1;
	strcpy_s(ci.applicationInfo.engineName, "LithTech 1.0 + ShogoVR bridge");
	ci.applicationInfo.engineVersion = 1;
	ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	ci.enabledExtensionCount = 1;
	ci.enabledExtensionNames = enabled;
	if (!XrOk(xrCreateInstance(&ci, &g_instance), "xrCreateInstance")) return false;
	if (!LoadInstanceFunctions()) return false;

	XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
	if (XR_SUCCEEDED(xrGetInstanceProperties(g_instance, &ip))) printf("Runtime: %s\n", ip.runtimeName);

	// Wait for a headset.
	XrSystemGetInfo si = { XR_TYPE_SYSTEM_GET_INFO };
	si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	bool bWaitingMsg = false;
	for (;;)
	{
		XrResult r = xrGetSystem(g_instance, &si, &m_system);
		if (XR_SUCCEEDED(r)) break;
		if (r != XR_ERROR_FORM_FACTOR_UNAVAILABLE) return XrOk(r, "xrGetSystem");
		if (!bWaitingMsg) { printf("Waiting for the headset to connect...\n"); bWaitingMsg = true; }
		CheckQuitEvent();
		if (g_quit) return false;
		Sleep(1000);
	}

	XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };
	if (XR_SUCCEEDED(xrGetSystemProperties(g_instance, m_system, &sp))) printf("Headset: %s\n", sp.systemName);
	return true;
}

bool Bridge::CreateDevice()
{
	XrGraphicsRequirementsD3D11KHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
	if (!XrOk(xrGetD3D11GraphicsRequirementsKHR(g_instance, m_system, &req), "xrGetD3D11GraphicsRequirementsKHR")) return false;

	IDXGIFactory1* factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return false;
	for (UINT i = 0; factory->EnumAdapters1(i, &m_adapter) != DXGI_ERROR_NOT_FOUND; ++i)
	{
		DXGI_ADAPTER_DESC1 d;
		m_adapter->GetDesc1(&d);
		if (memcmp(&d.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0)
		{
			printf("Graphics card: %s\n", WideToUtf8(d.Description).c_str());
			break;
		}
		SafeRelease(m_adapter);
	}
	factory->Release();
	if (!m_adapter) { printf("Couldn't find the graphics card the headset is connected to.\n"); return false; }

	D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
	HRESULT hr = D3D11CreateDevice(m_adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
								   levels, 4, D3D11_SDK_VERSION, &m_dev, nullptr, &m_ctx);
	if (FAILED(hr))
	{
		hr = D3D11CreateDevice(m_adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
							   levels + 1, 3, D3D11_SDK_VERSION, &m_dev, nullptr, &m_ctx);
	}
	if (FAILED(hr)) { printf("Couldn't create a Direct3D 11 device (0x%08lx).\n", (unsigned long)hr); return false; }
	return true;
}

bool Bridge::CreateSession()
{
	XrGraphicsBindingD3D11KHR binding = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
	binding.device = m_dev;

	XrSessionCreateInfo ci = { XR_TYPE_SESSION_CREATE_INFO };
	ci.next = &binding;
	ci.systemId = m_system;
	if (!XrOk(xrCreateSession(g_instance, &ci, &m_session), "xrCreateSession")) return false;

	XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	rs.poseInReferenceSpace = IdentityPose();
	rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if (!XrOk(xrCreateReferenceSpace(m_session, &rs, &m_localSpace), "xrCreateReferenceSpace(LOCAL)")) return false;
	rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if (!XrOk(xrCreateReferenceSpace(m_session, &rs, &m_viewSpace), "xrCreateReferenceSpace(VIEW)")) return false;

	// Pick a swapchain format.  BGRA sRGB lets us copy the desktop pixels
	// straight in; anything else goes through the shader.
	uint32_t count = 0;
	xrEnumerateSwapchainFormats(m_session, 0, &count, nullptr);
	std::vector<int64_t> formats(count);
	xrEnumerateSwapchainFormats(m_session, count, &count, formats.data());

	const int64_t prefs[] = { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
							  DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM };
	m_format = 0;
	for (int64_t p : prefs)
	{
		for (int64_t f : formats) if (f == p) { m_format = p; break; }
		if (m_format) break;
	}
	if (!m_format) { printf("The runtime offers no 8-bit colour swapchain format we can use.\n"); return false; }
	m_copyPath = (m_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);

	// The headset's preferred eye image size (we upscale towards it).
	uint32_t nViews = 0;
	xrEnumerateViewConfigurationViews(g_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &nViews, nullptr);
	std::vector<XrViewConfigurationView> views(nViews, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
	if (nViews && XR_SUCCEEDED(xrEnumerateViewConfigurationViews(g_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
																 nViews, &nViews, views.data())))
	{
		m_recW = views[0].recommendedImageRectWidth;
		m_recH = views[0].recommendedImageRectHeight;
		printf("Headset's eye resolution: %u x %u\n", m_recW, m_recH);
	}
	printf("Swapchain format %lld (%s)\n", (long long)m_format, m_copyPath ? "direct copy" : "shader copy");
	return true;
}

// ======================================================================= //
//  Controllers
// ======================================================================= //

static XrPath MakePath(const char* s)
{
	XrPath p = XR_NULL_PATH;
	xrStringToPath(g_instance, s, &p);
	return p;
}

static XrAction MakeAction(XrActionSet set, XrActionType type, const char* name, const char* label)
{
	XrActionCreateInfo ci = { XR_TYPE_ACTION_CREATE_INFO };
	ci.actionType = type;
	strcpy_s(ci.actionName, name);
	strcpy_s(ci.localizedActionName, label);
	XrAction a = XR_NULL_HANDLE;
	XrOk(xrCreateAction(set, &ci, &a), name);
	return a;
}

static const ActionDef k_actions[A_COUNT] =
{
	{ "move",			XR_ACTION_TYPE_VECTOR2F_INPUT,	"Move / strafe" },
	{ "turn",			XR_ACTION_TYPE_VECTOR2F_INPUT,	"Turn (left/right), change weapon (up/down)" },
	{ "fire",			XR_ACTION_TYPE_BOOLEAN_INPUT,	"Fire" },
	{ "jump",			XR_ACTION_TYPE_BOOLEAN_INPUT,	"Jump / menu select" },
	{ "crouch",			XR_ACTION_TYPE_BOOLEAN_INPUT,	"Crouch / menu back" },
	{ "menu",			XR_ACTION_TYPE_BOOLEAN_INPUT,	"Menu" },
	{ "mission_log",	XR_ACTION_TYPE_BOOLEAN_INPUT,	"Mission log" },
	{ "weapon_list",	XR_ACTION_TYPE_BOOLEAN_INPUT,	"Weapon list" },
	{ "next_weapon",	XR_ACTION_TYPE_BOOLEAN_INPUT,	"Next weapon" },
	{ "alt",			XR_ACTION_TYPE_BOOLEAN_INPUT,	"Spare button" },
	{ "recenter",		XR_ACTION_TYPE_BOOLEAN_INPUT,	"Recentre view" },
	{ "transform",		XR_ACTION_TYPE_BOOLEAN_INPUT,	"Transform (mech vehicle mode)" },
	{ "gun_hand",		XR_ACTION_TYPE_POSE_INPUT,		"Gun hand (aim)" },
	{ "off_hand",		XR_ACTION_TYPE_POSE_INPUT,		"Other hand" },
};

static const char* k_touch = "/interaction_profiles/oculus/touch_controller";

// The right-handed layouts.  The left-handed ones are made from these by
// MirrorPath().
static std::vector<ProfileLayout> RightHandedLayouts()
{
	const char* L = "/user/hand/left/input/";
	const char* R = "/user/hand/right/input/";
	auto P = [](const char* hand, const char* comp) { return std::string(hand) + comp; };
	std::vector<ProfileLayout> out;

	// Meta Quest / Rift (Touch)
	out.push_back({ k_touch, {
		{ A_MOVE, P(L, "thumbstick") }, { A_TURN, P(R, "thumbstick") }, { A_FIRE, P(R, "trigger/value") },
		{ A_JUMP, P(R, "a/click") }, { A_CROUCH, P(R, "b/click") }, { A_MENU, P(L, "menu/click") },
		{ A_LOG, P(L, "x/click") }, { A_WEAPONS, P(L, "y/click") }, { A_WEAPONS, P(L, "squeeze/value") },
		{ A_NEXT, P(R, "squeeze/value") },
		{ A_ALT, P(L, "trigger/value") }, { A_RECENTER, P(L, "thumbstick/click") },
		{ A_RAIM, P(R, "aim/pose") }, { A_LAIM, P(L, "aim/pose") } } });

	// Valve Index
	out.push_back({ "/interaction_profiles/valve/index_controller", {
		{ A_MOVE, P(L, "thumbstick") }, { A_TURN, P(R, "thumbstick") }, { A_FIRE, P(R, "trigger/value") },
		{ A_JUMP, P(R, "a/click") }, { A_CROUCH, P(R, "b/click") }, { A_MENU, P(L, "b/click") },
		{ A_LOG, P(L, "a/click") }, { A_WEAPONS, P(L, "squeeze/value") }, { A_NEXT, P(R, "squeeze/value") },
		{ A_ALT, P(L, "trigger/value") }, { A_RECENTER, P(L, "thumbstick/click") },
		{ A_RAIM, P(R, "aim/pose") }, { A_LAIM, P(L, "aim/pose") } } });

	// HTC Vive wands (trackpads instead of sticks)
	out.push_back({ "/interaction_profiles/htc/vive_controller", {
		{ A_MOVE, P(L, "trackpad") }, { A_TURN, P(R, "trackpad") }, { A_FIRE, P(R, "trigger/value") },
		{ A_JUMP, P(R, "squeeze/click") }, { A_CROUCH, P(L, "squeeze/click") }, { A_MENU, P(L, "menu/click") },
		{ A_NEXT, P(R, "menu/click") }, { A_ALT, P(L, "trigger/value") }, { A_RECENTER, P(L, "trackpad/click") },
		{ A_RAIM, P(R, "aim/pose") }, { A_LAIM, P(L, "aim/pose") } } });

	// Windows Mixed Reality
	out.push_back({ "/interaction_profiles/microsoft/motion_controller", {
		{ A_MOVE, P(L, "thumbstick") }, { A_TURN, P(R, "thumbstick") }, { A_FIRE, P(R, "trigger/value") },
		{ A_JUMP, P(R, "trackpad/click") }, { A_CROUCH, P(R, "squeeze/click") }, { A_MENU, P(L, "menu/click") },
		{ A_LOG, P(L, "trackpad/click") }, { A_WEAPONS, P(L, "squeeze/click") }, { A_NEXT, P(R, "menu/click") },
		{ A_ALT, P(L, "trigger/value") }, { A_RECENTER, P(L, "thumbstick/click") },
		{ A_RAIM, P(R, "aim/pose") }, { A_LAIM, P(L, "aim/pose") } } });

	// Anything else (basic fallback)
	out.push_back({ "/interaction_profiles/khr/simple_controller", {
		{ A_FIRE, P(R, "select/click") }, { A_JUMP, P(L, "select/click") }, { A_MENU, P(L, "menu/click") },
		{ A_RAIM, P(R, "aim/pose") }, { A_LAIM, P(L, "aim/pose") } } });
	return out;
}

// Left-handed version of a binding: swap the hands.  On Touch controllers the
// buttons differ per hand (A/B right, X/Y left), and only the left one has a
// menu button, so the menu stays where it is.
static std::string MirrorPath(const std::string& profile, const std::string& path)
{
	const std::string L = "/user/hand/left/", R = "/user/hand/right/";
	bool bWasLeft  = path.compare(0, L.size(), L) == 0;
	bool bWasRight = path.compare(0, R.size(), R) == 0;
	bool bTouch = (profile == k_touch);
	if (bTouch && path == L + "input/menu/click") return path;

	std::string p = path;
	if (bWasLeft)		p = R + path.substr(L.size());
	else if (bWasRight)	p = L + path.substr(R.size());

	if (bTouch)
	{
		auto swapComp = [&](const char* from, const char* to)
		{
			std::string f = std::string("/input/") + from + "/";
			size_t pos = p.find(f);
			if (pos != std::string::npos) p.replace(pos, f.size(), std::string("/input/") + to + "/");
		};
		if (bWasLeft)  { swapComp("x", "a"); swapComp("y", "b"); }	// now on the right hand
		if (bWasRight) { swapComp("a", "x"); swapComp("b", "y"); }	// now on the left hand
	}
	return p;
}

bool Bridge::SetupActions()
{
	// Two action sets: [0] right-handed, [1] left-handed (mirrored).  Both are
	// attached; each frame we read the one that matches LeftHanded.
	static const char* k_setNames[2]  = { "gameplay_right", "gameplay_left" };
	static const char* k_setLabels[2] = { "Shogo (right-handed)", "Shogo (left-handed)" };
	for (int h = 0; h < 2; ++h)
	{
		XrActionSetCreateInfo si = { XR_TYPE_ACTION_SET_CREATE_INFO };
		strcpy_s(si.actionSetName, k_setNames[h]);
		strcpy_s(si.localizedActionSetName, k_setLabels[h]);
		if (!XrOk(xrCreateActionSet(g_instance, &si, &m_hands[h].set), "xrCreateActionSet")) return false;
		for (int a = 0; a < A_COUNT; ++a)
			m_hands[h].act[a] = MakeAction(m_hands[h].set, k_actions[a].type, k_actions[a].name, k_actions[a].label);
	}

	// One suggestion per controller type, carrying both layouts (a second
	// call for the same type would replace the first).
	for (const ProfileLayout& layout : RightHandedLayouts())
	{
		std::vector<std::string> paths;		// keep the strings alive while building
		std::vector<XrActionSuggestedBinding> sb;
		for (const BindDef& b : layout.binds)
		{
			sb.push_back({ m_hands[0].act[b.action], MakePath(b.path.c_str()) });
			sb.push_back({ m_hands[1].act[b.action], MakePath(MirrorPath(layout.profile, b.path).c_str()) });
		}
		XrInteractionProfileSuggestedBinding ps = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		ps.interactionProfile = MakePath(layout.profile.c_str());
		ps.countSuggestedBindings = (uint32_t)sb.size();
		ps.suggestedBindings = sb.data();
		XrResult r = xrSuggestInteractionProfileBindings(g_instance, &ps);
		if (XR_FAILED(r)) printf("(Couldn't set up bindings for %s - error %d)\n", layout.profile.c_str(), (int)r);
	}

	for (int h = 0; h < 2; ++h)
	{
		XrActionSpaceCreateInfo asi = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
		asi.poseInActionSpace = IdentityPose();
		asi.action = m_hands[h].act[A_RAIM];
		if (!XrOk(xrCreateActionSpace(m_session, &asi, &m_hands[h].gunSpace), "xrCreateActionSpace(gun hand)")) return false;
		asi.action = m_hands[h].act[A_LAIM];
		if (!XrOk(xrCreateActionSpace(m_session, &asi, &m_hands[h].offSpace), "xrCreateActionSpace(other hand)")) return false;
	}

	XrActionSet sets[2] = { m_hands[0].set, m_hands[1].set };
	XrSessionActionSetsAttachInfo ai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	ai.countActionSets = 2;
	ai.actionSets = sets;
	if (!XrOk(xrAttachSessionActionSets(m_session, &ai), "xrAttachSessionActionSets")) return false;

	m_actionsReady = true;
	return true;
}

static bool GetBool(XrSession session, XrAction a)
{
	if (a == XR_NULL_HANDLE) return false;
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	gi.action = a;
	XrActionStateBoolean st = { XR_TYPE_ACTION_STATE_BOOLEAN };
	if (XR_FAILED(xrGetActionStateBoolean(session, &gi, &st))) return false;
	return st.isActive && st.currentState;
}

static XrVector2f GetVec2(XrSession session, XrAction a)
{
	XrVector2f v = { 0.0f, 0.0f };
	if (a == XR_NULL_HANDLE) return v;
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	gi.action = a;
	XrActionStateVector2f st = { XR_TYPE_ACTION_STATE_VECTOR2F };
	if (XR_SUCCEEDED(xrGetActionStateVector2f(session, &gi, &st)) && st.isActive) v = st.currentState;
	return v;
}

// Converts a raw tracking-space pose into what the game wants: relative to
// the recentre point, in LithTech's left-handed space (z flipped).
void Bridge::ToGameSpace(const XrPosef& raw, float fwd[3], float up[3], float pos[3])
{
	XrQuaternionf q = QMul(m_centerInv, raw.orientation);
	XrVector3f d = { raw.position.x - m_centerPos.x, raw.position.y - m_centerPos.y, raw.position.z - m_centerPos.z };
	XrVector3f p = QRotate(m_centerInv, d);
	XrVector3f f = QRotate(q, XrVector3f{ 0.0f, 0.0f, -1.0f });
	XrVector3f u = QRotate(q, XrVector3f{ 0.0f, 1.0f, 0.0f });
	fwd[0] = f.x;	fwd[1] = f.y;	fwd[2] = -f.z;
	up[0]  = u.x;	up[1]  = u.y;	up[2]  = -u.z;
	pos[0] = p.x;	pos[1] = p.y;	pos[2] = -p.z;
}

void Bridge::PollInput(XrTime time, BridgeData& d)
{
	m_motion = 0.0f;
	if (!m_actionsReady || m_state != XR_SESSION_STATE_FOCUSED) return;

	const HandLayout& H = m_hands[m_leftHanded ? 1 : 0];
	XrActiveActionSet active = { H.set, XR_NULL_PATH };
	XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;
	if (xrSyncActions(m_session, &sync) != XR_SUCCESS) return;	// not focused etc.

	d.ctrlFlags |= SHOGOVR_CTRL_ACTIVE;

	XrVector2f mv = GetVec2(m_session, H.act[A_MOVE]);
	XrVector2f tv = GetVec2(m_session, H.act[A_TURN]);
	d.moveX = mv.x;	d.moveY = mv.y;
	d.turnX = tv.x;	d.turnY = tv.y;

	// How much you're being moved around - drives the comfort vignette.
	float moveAmt = std::sqrt(mv.x * mv.x + mv.y * mv.y);
	float turnAmt = std::fabs(tv.x) > std::fabs(tv.y) ? std::fabs(tv.x) : 0.0f;
	m_motion = std::min(1.0f, std::max(moveAmt > 0.15f ? moveAmt : 0.0f, turnAmt > 0.2f ? turnAmt : 0.0f));

	static const int k_btn[][2] = {
		{ A_FIRE, SHOGOVR_BTN_FIRE }, { A_JUMP, SHOGOVR_BTN_JUMP }, { A_CROUCH, SHOGOVR_BTN_CROUCH },
		{ A_MENU, SHOGOVR_BTN_MENU }, { A_LOG, SHOGOVR_BTN_LOG }, { A_WEAPONS, SHOGOVR_BTN_WEAPONS },
		{ A_NEXT, SHOGOVR_BTN_NEXT_WEAPON }, { A_ALT, SHOGOVR_BTN_ALT }, { A_TRANSFORM, SHOGOVR_BTN_TRANSFORM } };
	for (auto& b : k_btn) if (GetBool(m_session, H.act[b[0]])) d.buttons |= b[1];

	// Recentre: click the movement stick (handled here, the game doesn't need it).
	bool rc = GetBool(m_session, H.act[A_RECENTER]);
	if (rc && !m_recenterBtnDown) Recenter();
	m_recenterBtnDown = rc;

	// Hands: the game's "right" hand is always the gun hand.
	const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
	XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
	if (XR_SUCCEEDED(xrLocateSpace(H.gunSpace, m_localSpace, time, &loc)) && (loc.locationFlags & need) == need)
	{
		ToGameSpace(loc.pose, d.rFwd, d.rUp, d.rPos);
		d.ctrlFlags |= SHOGOVR_CTRL_RIGHT_POSE;
	}
	loc = { XR_TYPE_SPACE_LOCATION };
	if (XR_SUCCEEDED(xrLocateSpace(H.offSpace, m_localSpace, time, &loc)) && (loc.locationFlags & need) == need)
	{
		ToGameSpace(loc.pose, d.lFwd, d.lUp, d.lPos);
		d.ctrlFlags |= SHOGOVR_CTRL_LEFT_POSE;
	}
}

// ======================================================================= //
//  The game window: size, focus and Windows display scaling
// ======================================================================= //

// Windows 10+ DPI helpers, loaded at run time so the bridge still starts on older systems.
typedef HANDLE	(WINAPI *GetWindowDpiAwarenessContextFn)(HWND);
typedef int		(WINAPI *GetAwarenessFromDpiAwarenessContextFn)(HANDLE);
typedef UINT	(WINAPI *GetDpiForWindowFn)(HWND);
typedef UINT	(WINAPI *GetDpiForSystemFn)();
typedef BOOL	(WINAPI *AdjustWindowRectExForDpiFn)(LPRECT, DWORD, BOOL, DWORD, UINT);
typedef HRESULT	(WINAPI *GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);

template <typename T> static T User32Fn(const char* name)
{
	HMODULE u = GetModuleHandleW(L"user32.dll");
	return u ? (T)GetProcAddress(u, name) : nullptr;
}

// Is Windows stretching the game's picture (DPI virtualisation)?  That makes
// it blurry and makes window sizes ambiguous between the game and us.
static bool IsDpiVirtualized(HWND hwnd, int& scalePercent)
{
	scalePercent = 100;
	auto getCtx = User32Fn<GetWindowDpiAwarenessContextFn>("GetWindowDpiAwarenessContext");
	auto getAw  = User32Fn<GetAwarenessFromDpiAwarenessContextFn>("GetAwarenessFromDpiAwarenessContext");
	auto getSys = User32Fn<GetDpiForSystemFn>("GetDpiForSystem");
	HMODULE shcore = LoadLibraryW(L"shcore.dll");
	auto getMon = shcore ? (GetDpiForMonitorFn)GetProcAddress(shcore, "GetDpiForMonitor") : nullptr;
	if (!getCtx || !getAw || !getMon) return false;

	UINT dx = 96, dy = 96;
	if (FAILED(getMon(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy))) return false;
	scalePercent = (int)(dx * 100 / 96);

	int awareness = getAw(getCtx(hwnd));	// 0 unaware, 1 system aware, 2 per-monitor aware
	if (awareness == 0) return dx != 96;
	if (awareness == 1) return getSys && dx != getSys();
	return false;
}

static void BringToFront(HWND hwnd)
{
	HWND fg = GetForegroundWindow();
	if (fg == hwnd) return;

	// Windows only lets the foreground app hand over focus, so briefly share
	// its input state (the usual trick), then switch.
	DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
	DWORD me = GetCurrentThreadId();
	bool attached = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
	BringWindowToTop(hwnd);
	SetForegroundWindow(hwnd);
	if (attached) AttachThreadInput(me, fgThread, FALSE);
}

static bool ClientFullyOnMonitor(HWND hwnd)
{
	RECT rc;
	GetClientRect(hwnd, &rc);
	POINT tl = { 0, 0 };
	ClientToScreen(hwnd, &tl);
	MONITORINFO mi = { sizeof(mi) };
	GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
	return tl.x >= mi.rcMonitor.left && tl.y >= mi.rcMonitor.top &&
		   tl.x + rc.right <= mi.rcMonitor.right && tl.y + rc.bottom <= mi.rcMonitor.bottom;
}

// Works out where the game window should be and how big, so that its inside
// is exactly the game's resolution (pixel-for-pixel = sharpest capture).
bool Bridge::PlanWindow(const GameInfo& g, WindowPlan& p)
{
	HWND hwnd = g.hwnd;
	if (!hwnd || !IsWindow(hwnd) || g.renderWidth <= 0 || g.renderHeight <= 0) return false;

	LONG curStyle = GetWindowLongW(hwnd, GWL_STYLE);
	LONG curEx    = GetWindowLongW(hwnd, GWL_EXSTYLE);
	LONG style    = m_borderless ? m_savedStyle   : curStyle;	// plan from the game's own frame
	LONG exStyle  = m_borderless ? m_savedExStyle : curEx;

	// A borderless/fullscreen game window (no frame of its own) is left alone.
	bool bFramed = ((style & WS_CAPTION) == WS_CAPTION) || (style & WS_THICKFRAME);
	if (!bFramed) return false;

	MONITORINFO mi = { sizeof(mi) };
	GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
	RECT mon = mi.rcMonitor, work = mi.rcWork;
	LONG monW = mon.right - mon.left, monH = mon.bottom - mon.top;
	LONG workW = work.right - work.left, workH = work.bottom - work.top;

	auto adjustForDpi = User32Fn<AdjustWindowRectExForDpiFn>("AdjustWindowRectExForDpi");
	auto dpiForWindow = User32Fn<GetDpiForWindowFn>("GetDpiForWindow");
	auto outerSize = [&](LONG st, LONG ex, LONG cw, LONG ch, LONG& ow, LONG& oh)
	{
		RECT rc = { 0, 0, cw, ch };
		if (adjustForDpi && dpiForWindow) adjustForDpi(&rc, (DWORD)st, FALSE, (DWORD)ex, dpiForWindow(hwnd));
		else AdjustWindowRectEx(&rc, (DWORD)st, FALSE, (DWORD)ex);
		ow = rc.right - rc.left;
		oh = rc.bottom - rc.top;
	};

	LONG cw = g.renderWidth, ch = g.renderHeight, ow = 0, oh = 0;
	outerSize(style, exStyle, cw, ch, ow, oh);

	if (ow <= workW && oh <= workH)
	{
		// Fits as a normal window: keep its position if it's fully on screen.
		p.mode = 0;
		p.style = style;
		p.exStyle = exStyle;
		RECT wr;
		GetWindowRect(hwnd, &wr);
		p.x = wr.left;
		p.y = wr.top;
		bool bWasBorderless = m_borderless;
		if (bWasBorderless || p.x < work.left || p.y < work.top || p.x + ow > work.right || p.y + oh > work.bottom)
		{
			p.x = work.left + (workW - ow) / 2;
			p.y = work.top + (workH - oh) / 2;
		}
	}
	else if ((cw <= monW && ch <= monH) || m_oversize)
	{
		// The frame and taskbar don't leave room - drop the frame instead of
		// shrinking the picture, and centre it on the monitor.
		p.mode = 1;
		p.style = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP;
		p.exStyle = exStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
		ow = cw;
		oh = ch;
		p.x = m_oversize ? mon.left : mon.left + (monW - cw) / 2;		// oversize: from the top-left corner
		p.y = m_oversize ? mon.top  : mon.top + (monH - ch) / 2;
	}
	else
	{
		// Bigger than the monitor itself: shrink (this costs sharpness).
		p.mode = 2;
		p.style = style;
		p.exStyle = exStyle;
		float sx = (float)(workW - (ow - cw)) / (float)cw;
		float sy = (float)(workH - (oh - ch)) / (float)ch;
		float sc = std::min(sx, sy);
		cw = (LONG)(cw * sc);
		ch = (LONG)(ch * sc);
		outerSize(style, exStyle, cw, ch, ow, oh);
		p.x = work.left + (workW - ow) / 2;
		p.y = work.top + (workH - oh) / 2;
	}

	p.clientW = cw;
	p.clientH = ch;
	p.outerW = ow;
	p.outerH = oh;
	return true;
}

void Bridge::FixGameWindow(const GameInfo& g, bool bBringToFront)
{
	HWND hwnd = g.hwnd;
	if (!hwnd || !IsWindow(hwnd)) return;

	if (IsIconic(hwnd))
	{
		// Shogo minimizes itself when it loses focus, and its renderer rebuilds
		// itself when it comes back.  Restoring it without focus, or at its
		// old (tiny) size, is what made it draw a shrunken picture.  So only
		// restore it when we're also giving it focus, and set the size it
		// restores to first, so the renderer rebuilds at full size.
		if (!bBringToFront) return;

		WindowPlan p;
		if (!m_dpiVirtualized && PlanWindow(g, p))
		{
			LONG curStyle = GetWindowLongW(hwnd, GWL_STYLE);
			LONG curEx    = GetWindowLongW(hwnd, GWL_EXSTYLE);
			if (p.mode == 1 && !m_borderless) { m_savedStyle = curStyle & ~WS_MINIMIZE; m_savedExStyle = curEx; }
			if ((curStyle & ~(WS_MINIMIZE | WS_VISIBLE)) != (p.style & ~(WS_MINIMIZE | WS_VISIBLE)) || curEx != p.exStyle)
			{
				SetWindowLongW(hwnd, GWL_STYLE, (p.style & ~WS_MINIMIZE) | (curStyle & (WS_MINIMIZE | WS_VISIBLE)));
				SetWindowLongW(hwnd, GWL_EXSTYLE, p.exStyle);
			}
			m_borderless = (p.mode == 1);

			// The restore position is in "workspace" coordinates (relative
			// to the primary monitor's work area).
			MONITORINFO pmi = { sizeof(pmi) };
			POINT origin = { 0, 0 };
			GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &pmi);
			WINDOWPLACEMENT wp = { sizeof(wp) };
			GetWindowPlacement(hwnd, &wp);
			wp.rcNormalPosition.left	= p.x - pmi.rcWork.left;
			wp.rcNormalPosition.top		= p.y - pmi.rcWork.top;
			wp.rcNormalPosition.right	= wp.rcNormalPosition.left + p.outerW;
			wp.rcNormalPosition.bottom	= wp.rcNormalPosition.top + p.outerH;
			wp.showCmd = SW_RESTORE;
			wp.flags = 0;
			SetWindowPlacement(hwnd, &wp);
			printf("\nRestored the minimized Shogo window at %ldx%ld.\n", p.clientW, p.clientH);
		}
		else
		{
			ShowWindowAsync(hwnd, SW_RESTORE);
		}
		BringToFront(hwnd);
		return;
	}

	// With Windows scaling the game, its idea of pixels and ours differ, so
	// don't guess sizes - Ctrl+Shift+D fixes the scaling itself.
	WindowPlan p;
	if (!m_dpiVirtualized && !IsZoomed(hwnd) && PlanWindow(g, p))
	{
		LONG curStyle = GetWindowLongW(hwnd, GWL_STYLE);
		LONG curEx    = GetWindowLongW(hwnd, GWL_EXSTYLE);
		bool bStyleChange = (curStyle != p.style) || (curEx != p.exStyle);

		if (p.mode == 1 && !m_borderless)
		{
			m_savedStyle = curStyle;
			m_savedExStyle = curEx;
		}
		if (bStyleChange)
		{
			SetWindowLongW(hwnd, GWL_STYLE, p.style);
			SetWindowLongW(hwnd, GWL_EXSTYLE, p.exStyle);
		}
		m_borderless = (p.mode == 1);

		bool bNeeded = bStyleChange || g.clientWidth != p.clientW || g.clientHeight != p.clientH || (!m_oversize && !ClientFullyOnMonitor(hwnd));
		if (bNeeded)
		{
			SetWindowPos(hwnd, nullptr, p.x, p.y, p.outerW, p.outerH,
						 SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS | (bStyleChange ? SWP_FRAMECHANGED : 0));

			if (p.mode == 1)
				printf("\nMade the Shogo window borderless so its full %ldx%ld fits your screen pixel-for-pixel (was %ldx%ld).\n",
					   p.clientW, p.clientH, g.clientWidth, g.clientHeight);
			else if (p.mode == 2)
				printf("\nShogo's resolution (%ldx%ld) is larger than your monitor - shrank the window to %ldx%ld.\n"
					   "Choose a resolution no bigger than your monitor for a sharper picture.\n",
					   g.renderWidth, g.renderHeight, p.clientW, p.clientH);
			else
				printf("\nPut the Shogo window back to %ldx%ld, fully on screen (was %ldx%ld).\n",
					   p.clientW, p.clientH, g.clientWidth, g.clientHeight);
		}
	}

	if (bBringToFront) BringToFront(hwnd);
}

// Keeps the window right after loading screens and anything else that
// moves or resizes it.  Waits a moment before acting (so it never fights a
// window you're dragging), and gives up if the game keeps undoing it.
void Bridge::WatchWindow(const GameInfo& g)
{
	if (!g.alive || m_dpiVirtualized || m_watchGaveUp || !m_windowFixedOnce) return;

	ULONGLONG now = GetTickCount64();
	if (now - m_lastWatch < 250) return;
	m_lastWatch = now;

	HWND hwnd = g.hwnd;
	if (IsZoomed(hwnd) || (GetAsyncKeyState(VK_LBUTTON) & 0x8000))
	{
		m_badSince = 0;		// maximized by you, or maybe being dragged: leave it
		return;
	}

	WindowPlan p;
	if (!PlanWindow(g, p)) return;

	if (IsIconic(hwnd) || g.clientWidth <= 0 || g.clientHeight <= 0)
	{
		m_badSince = 0;		// minimized: KeepFocus decides whether to bring it back
		return;
	}

	LONG curStyle = GetWindowLongW(hwnd, GWL_STYLE);
	bool bBad = g.clientWidth != p.clientW || g.clientHeight != p.clientH ||
				(!m_oversize && !ClientFullyOnMonitor(hwnd)) || (p.mode == 1 && curStyle != p.style);
	if (!bBad)
	{
		m_badSince = 0;
		if (now - m_lastCorrection > 10000) m_corrections = 0;
		return;
	}

	if (!m_badSince) { m_badSince = now; return; }
	if (now - m_badSince < 1000 || now - m_lastCorrection < 3000) return;

	if (++m_corrections > 5)
	{
		m_watchGaveUp = true;
		printf("\nShogo keeps changing its window, so the bridge will stop correcting it.\n"
			   "Press Ctrl+Shift+F to try again.\n");
		return;
	}

	FixGameWindow(g, false);
	m_lastCorrection = now;
	m_badSince = 0;
}

void Bridge::FixDpiScaling(const GameInfo& g)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(g.hwnd, &pid);
	HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!proc) { printf("\nCouldn't look up Shogo's program file (error %lu).\n", GetLastError()); return; }

	wchar_t path[2048];
	DWORD len = 2048;
	BOOL ok = QueryFullProcessImageNameW(proc, 0, path, &len);
	CloseHandle(proc);
	if (!ok) { printf("\nCouldn't look up Shogo's program file.\n"); return; }

	// Same setting as Properties > Compatibility > Change high DPI settings >
	// "Override high DPI scaling behavior: Application".
	HKEY key = nullptr;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers",
						0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
	{
		printf("\nCouldn't open the Windows compatibility settings.\n");
		return;
	}

	wchar_t cur[1024] = L"";
	DWORD size = sizeof(cur) - sizeof(wchar_t), type = 0;
	RegQueryValueExW(key, path, nullptr, &type, (BYTE*)cur, &size);
	std::wstring flags = (type == REG_SZ) ? std::wstring(cur) : std::wstring();

	if (flags.find(L"HIGHDPIAWARE") != std::wstring::npos)
	{
		printf("\nShogo is already set to handle display scaling itself - restart Shogo if it's still blurry.\n");
	}
	else
	{
		flags = flags.empty() ? std::wstring(L"~ HIGHDPIAWARE") : flags + L" HIGHDPIAWARE";
		RegSetValueExW(key, path, 0, REG_SZ, (const BYTE*)flags.c_str(), (DWORD)((flags.size() + 1) * sizeof(wchar_t)));
		printf("\nDone: Windows will no longer stretch %s.\nRestart Shogo for this to take effect.\n", WideToUtf8(path).c_str());
	}
	RegCloseKey(key);
}

static std::wstring ProcessNameOfWindow(HWND hwnd)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!proc) return L"";
	wchar_t path[1024];
	DWORD len = 1024;
	std::wstring name;
	if (QueryFullProcessImageNameW(proc, 0, path, &len))
	{
		name = path;
		size_t slash = name.find_last_of(L"\\/");
		if (slash != std::wstring::npos) name = name.substr(slash + 1);
		for (auto& c : name) c = (wchar_t)towlower(c);
	}
	CloseHandle(proc);
	return name;
}

// While you're in the headset, give Shogo its focus back if SteamVR, the
// Oculus software or a console window (like this one) took it - but never
// if you deliberately switched to another program.
void Bridge::KeepFocus(const GameInfo& g)
{
	if (!m_keepFocus || !g.alive || !m_running || !m_windowFixedOnce) return;
	if (m_state != XR_SESSION_STATE_FOCUSED && m_state != XR_SESSION_STATE_VISIBLE) return;

	ULONGLONG now = GetTickCount64();
	HWND hwnd = g.hwnd;
	HWND fg = GetForegroundWindow();
	bool bMinimized = IsIconic(hwnd) != 0;
	if (fg == hwnd && !bMinimized)
	{
		m_focusLostSince = 0;
		return;
	}

	std::wstring who = fg ? ProcessNameOfWindow(fg) : L"";
	DWORD fgPid = 0;
	if (fg) GetWindowThreadProcessId(fg, &fgPid);

	static const wchar_t* s_takers[] = {
		L"vrmonitor.exe", L"vrcompositor.exe", L"vrserver.exe", L"vrdashboard.exe", L"vrwebhelper.exe",
		L"vrstartup.exe", L"oculusdash.exe", L"ovrserver_x64.exe", L"oculusclient.exe",
		L"windowsterminal.exe", L"openconsole.exe", L"conhost.exe", L"cmd.exe", L"powershell.exe",
		L"virtualdesktop.streamer.exe" };
	bool bTaker = !fg || fgPid == GetCurrentProcessId();
	for (const wchar_t* t : s_takers) if (who == t) bTaker = true;
	if (bMinimized && who == L"explorer.exe") bTaker = true;	// Shogo minimized itself onto the desktop

	if (!bTaker)
	{
		m_focusLostSince = 0;		// you chose another program - leave it be
		return;
	}

	if (!m_focusLostSince) { m_focusLostSince = now; return; }
	if (now - m_focusLostSince < 1500 || now - m_lastFocusFix < 3000) return;

	printf("\nGave focus back to Shogo%s%s%s.\n", who.empty() ? "" : " (taken by ",
		   WideToUtf8(who).c_str(), who.empty() ? "" : ")");
	FixGameWindow(g, true);
	m_lastFocusFix = now;
	m_focusLostSince = 0;
}

void Bridge::HandleWindow(const GameInfo& g)
{
	ULONGLONG now = GetTickCount64();
	if (g.alive && !m_gameWasAlive)
	{
		// Windows doesn't let a normal program resize, restyle or focus the
		// window of a program running as administrator.  If Shogo runs as
		// administrator and we don't, step aside: the game starts a bridge
		// with its own rights within a few seconds.
		DWORD gamePid = 0;
		if (g.hwnd) GetWindowThreadProcessId(g.hwnd, &gamePid);
		HANDLE gameProc = gamePid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, gamePid) : nullptr;
		bool gameElevated = gameProc && IsElevated(gameProc);
		if (gameProc) CloseHandle(gameProc);
		if (gameElevated && !IsElevated(GetCurrentProcess()))
		{
			printf("\nShogo is running as administrator, and Windows won't let this bridge (not administrator)\n"
				   "manage its window. Closing - the game starts its own bridge with matching rights.\n");
			g_quit = true;
			return;
		}

		// Back after a loading screen (the game pauses while it loads), or
		// a fresh start of Shogo?  Only a fresh start gets the full welcome.
		bool bFreshStart = (m_lastAliveTime == 0) || (now - m_lastAliveTime > 30000);
		if (bFreshStart)
		{
			m_gameAliveSince = now;
			m_windowFixedOnce = false;
			m_dpiWarned = false;
			m_borderless = false;
		}
		m_watchGaveUp = false;
		m_corrections = 0;
		m_badSince = 0;
	}
	m_gameWasAlive = g.alive;
	if (!g.alive) return;
	m_lastAliveTime = now;

	// Report when the game draws at a different size than its resolution
	// (the game resets its renderer by itself when that happens).
	if (g.surfaceWidth > 0 && (g.surfaceWidth != m_lastSurfW || g.surfaceHeight != m_lastSurfH))
	{
		if (g.surfaceWidth != g.renderWidth || g.surfaceHeight != g.renderHeight)
			printf("\nShogo is drawing at %ldx%ld, not its %ldx%ld resolution (window %ldx%ld).\n",
				   g.surfaceWidth, g.surfaceHeight, g.renderWidth, g.renderHeight, g.clientWidth, g.clientHeight);
		else if (m_lastSurfW > 0)
			printf("\nShogo is drawing at its full %ldx%ld again.\n", g.surfaceWidth, g.surfaceHeight);
		m_lastSurfW = g.surfaceWidth;
		m_lastSurfH = g.surfaceHeight;
	}

	if (!m_dpiWarned)
	{
		int pct = 100;
		m_dpiVirtualized = IsDpiVirtualized(g.hwnd, pct);
		if (m_dpiVirtualized)
		{
			printf("\nWindows is stretching Shogo for your %d%% display scaling, which makes it blurry\n"
				   "and confuses the window size.  Press Ctrl+Shift+D to fix it, then restart Shogo.\n", pct);
		}
		m_dpiWarned = true;
	}

	// Once the game has settled (1.5 s after it connects), and whenever its
	// resolution changes: fix the window size, and bring it to the front once.
	bool bModeChanged = m_windowFixedOnce && (g.renderWidth != m_lastRenderW || g.renderHeight != m_lastRenderH);
	if (bModeChanged)
	{
		printf("\nShogo's resolution changed from %ldx%ld to %ldx%ld.\n", m_lastRenderW, m_lastRenderH, g.renderWidth, g.renderHeight);
	}
	if ((!m_windowFixedOnce && now - m_gameAliveSince > 1500) || bModeChanged)
	{
		FixGameWindow(g, !m_windowFixedOnce);
		m_windowFixedOnce = true;
		m_lastRenderW = g.renderWidth;
		m_lastRenderH = g.renderHeight;
	}

	// Hotkeys: Ctrl+Shift+F = fix window + focus game, Ctrl+Shift+D = fix display scaling.
	bool ctrlShift = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
	bool fKey = ctrlShift && (GetAsyncKeyState('F') & 0x8000);
	if (fKey && !m_focusKeyDown)
	{
		m_watchGaveUp = false;
		m_corrections = 0;
		FixGameWindow(g, true);
	}
	m_focusKeyDown = fKey;

	WatchWindow(g);
	KeepFocus(g);
	bool dKey = ctrlShift && (GetAsyncKeyState('D') & 0x8000);
	if (dKey && !m_dpiKeyDown) FixDpiScaling(g);
	m_dpiKeyDown = dKey;
}

void Bridge::FreeSwapchain(EyeSwapchain& e)
{
	for (auto* r : e.rtvs) if (r) r->Release();
	e.rtvs.clear();
	e.images.clear();
	if (e.handle != XR_NULL_HANDLE) { xrDestroySwapchain(e.handle); e.handle = XR_NULL_HANDLE; }
	e.w = e.h = 0;
}

bool Bridge::MakeSwapchain(EyeSwapchain& e, UINT w, UINT h)
{
	FreeSwapchain(e);

	XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	ci.format = m_format;
	ci.sampleCount = 1;
	ci.width = w;
	ci.height = h;
	ci.faceCount = 1;
	ci.arraySize = 1;
	ci.mipCount = 1;
	if (!XrOk(xrCreateSwapchain(m_session, &ci, &e.handle), "xrCreateSwapchain")) return false;

	uint32_t n = 0;
	xrEnumerateSwapchainImages(e.handle, 0, &n, nullptr);
	e.images.assign(n, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
	xrEnumerateSwapchainImages(e.handle, n, &n, (XrSwapchainImageBaseHeader*)e.images.data());

	e.rtvs.assign(n, nullptr);
	if (m_blitOk)
	{
		for (uint32_t i = 0; i < n; ++i)
		{
			D3D11_RENDER_TARGET_VIEW_DESC rd = {};
			rd.Format = (DXGI_FORMAT)m_format;
			rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			m_dev->CreateRenderTargetView(e.images[i].texture, &rd, &e.rtvs[i]);
		}
	}
	e.w = w; e.h = h;
	return true;
}

void Bridge::DestroySwapchains()
{
	for (auto& e : m_eyes) FreeSwapchain(e);
	FreeSwapchain(m_hud);
	FreeSwapchain(m_screen);
	m_eyesHaveImage = m_hudHasImage = m_screenHasImage = false;
}

bool Bridge::CreateSwapchains(UINT w, UINT h)
{
	for (auto& e : m_eyes) if (!MakeSwapchain(e, w, h)) return false;
	m_eyesHaveImage = false;
	printf("\nEye images: %u x %u\n", w, h);
	return true;
}

// Where the picture comes from: straight from the game's renderer when the
// game offers it (no monitor limit), otherwise a copy of the screen.
bool Bridge::CaptureFrame(const GameInfo& g)
{
	bool bDirect = m_directCapture && g.captureHandle && !m_capture.SharedFailed(g.captureHandle);
	if (bDirect)
	{
		bool bNew = m_capture.UpdateShared(g.captureHandle);
		if (!m_capture.SharedFailed(g.captureHandle))
		{
			if (!m_usingDirect)
			{
				printf("\nCapturing straight from Shogo's renderer at %ldx%ld - your monitor no longer limits the picture.\n",
					   g.captureWidth, g.captureHeight);
				m_usingDirect = true;
			}
			// If the renderer draws at the window's size, the window has to be
			// as big as the game's resolution - even bigger than the screen.
			if (!m_oversize && g.captureWidth > 0 && g.captureHeight > 0 &&
				(g.captureWidth < g.renderWidth || g.captureHeight < g.renderHeight))
			{
				m_oversize = true;
				printf("\nThe renderer draws at the window's size (%ldx%ld), so the Shogo window is being made %ldx%ld, "
					   "even though that's bigger than your screen.\n", g.captureWidth, g.captureHeight, g.renderWidth, g.renderHeight);
				FixGameWindow(g, false);
			}
			return bNew;
		}
		printf("\nDirect capture isn't available (%s) - using screen capture.\n", m_capture.SharedError());
	}
	if (m_usingDirect)
	{
		m_usingDirect = false;
		m_oversize = false;
		m_capture.CloseShared();
		printf("\nBack to screen capture.\n");
	}
	return m_capture.Update(g.hwnd);
}

// The left-eye window: your left eye's picture cropped to the window's
// shape, with the HUD laid over it where it floats in the headset.
void Bridge::DrawSpectator(const GameInfo& g)
{
	if (!m_spectatorOn || !m_blitOk) return;
	if (!m_spectator.IsOpen() && !m_spectator.Open(m_dev)) { m_spectatorOn = false; return; }

	UINT W = 0, H = 0;
	ID3D11RenderTargetView* rtv = m_spectator.Begin(W, H);
	UINT capW = m_capture.Width(), capH = m_capture.Height();
	if (!rtv || capW < 16 || capH < 16) return;

	float black[4] = { 0, 0, 0, 1 };
	m_ctx->ClearRenderTargetView(rtv, black);
	float winA = (float)W / (float)H;

	if (g.viewMode == SHOGOVR_VIEW_FLAT)
	{
		float srcA = (float)capW / (float)capH, uw = 1.0f, vh = 1.0f;
		if (winA > srcA) vh = srcA / winA; else uw = winA / srcA;
		m_blit.DrawRegion(rtv, W, H, m_capture.SRV(), capW, capH, (1.0f - uw) * 0.5f, (1.0f - vh) * 0.5f, uw, vh, 0.0f, 1.0f, 1.0f);
	}
	else
	{
		UINT eyeW = capW / 2;
		UINT eyeH = (g.eyeHeight > 0 && (UINT)g.eyeHeight < capH) ? (UINT)g.eyeHeight : capH;
		float eyeA = (float)eyeW / (float)eyeH, fx = 1.0f, fy = 1.0f;
		if (winA > eyeA) fy = eyeA / winA; else fx = winA / eyeA;

		int half = g.swapEyes ? 1 : 0;				// where the left eye's picture is
		float uEye = (float)eyeW / capW, vEye = (float)eyeH / capH;
		float uw = uEye * fx, vh = vEye * fy;
		float u0 = half * uEye + (uEye - uw) * 0.5f, v0 = (vEye - vh) * 0.5f;
		m_blit.DrawRegion(rtv, W, H, m_capture.SRV(), capW, capH, u0, v0, uw, vh, half * uEye, (half + 1) * uEye, vEye);

		if ((g.layoutFlags & SHOGOVR_LAYOUT_HUD) && g.hudW > 0 && g.hudH > 0 && g.renderTanX > 0.01f &&
			(UINT)(g.hudX + g.hudW) <= capW && (UINT)(g.hudY + g.hudH) <= capH)
		{
			float angle = (g.hudAngle > 0.1f && g.hudAngle < 2.8f) ? g.hudAngle : 1.05f;
			float frac = std::tan(angle * 0.5f) / (g.renderTanX * fx);		// HUD width / window width
			float hw = W * frac, hh = hw * (float)g.hudH / (float)g.hudW;
			m_blit.DrawHudAt(rtv, W, H, (W - hw) * 0.5f, (H - hh) * 0.5f, hw, hh, m_capture.SRV(), capW, capH,
							 g.hudX, g.hudY, g.hudW, g.hudH);
		}
	}
	m_spectator.Present();
}

// The HUD strip: copied pixel-exact (then enlarged by a whole number so the
// headset's own filtering keeps it crisp), with black made see-through.
bool Bridge::CopyHud(const GameInfo& g)
{
	UINT capW = m_capture.Width(), capH = m_capture.Height();
	if (!m_blitOk || g.hudW <= 0 || g.hudH <= 0 || g.hudX < 0 || g.hudY < 0 ||
		(UINT)(g.hudX + g.hudW) > capW || (UINT)(g.hudY + g.hudH) > capH) return false;

	UINT k = (UINT)std::max(1L, std::min(4L, 1536L / g.hudW));
	UINT outW = (UINT)g.hudW * k, outH = (UINT)g.hudH * k;
	if (m_hud.handle == XR_NULL_HANDLE || m_hud.w != outW || m_hud.h != outH)
	{
		if (!MakeSwapchain(m_hud, outW, outH)) return false;
		m_hudHasImage = false;
		printf("\nHUD panel: %ldx%ld pixels, shown at %ux\n", g.hudW, g.hudH, k);
	}

	uint32_t idx = 0;
	XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if (!XrOk(xrAcquireSwapchainImage(m_hud.handle, &ai, &idx), "xrAcquireSwapchainImage(hud)")) return false;
	XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	wi.timeout = XR_INFINITE_DURATION;
	if (!XrOk(xrWaitSwapchainImage(m_hud.handle, &wi), "xrWaitSwapchainImage(hud)")) return false;
	if (m_hud.rtvs[idx]) m_blit.DrawHud(m_hud.rtvs[idx], outW, outH, m_capture.SRV(), capW, capH, g.hudX, g.hudY, g.hudW, g.hudH);
	XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(m_hud.handle, &ri);
	m_hudHasImage = true;
	return true;
}

// The flat game picture (menus, loading screens, cutscenes) for the floating screen.
bool Bridge::CopyScreen(const GameInfo& g)
{
	UINT capW = m_capture.Width(), capH = m_capture.Height();
	if (capW < 16 || capH < 16) return false;

	if (m_screen.handle == XR_NULL_HANDLE || m_screen.w != capW || m_screen.h != capH)
	{
		if (!MakeSwapchain(m_screen, capW, capH)) return false;
		m_screenHasImage = false;
	}

	uint32_t idx = 0;
	XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if (!XrOk(xrAcquireSwapchainImage(m_screen.handle, &ai, &idx), "xrAcquireSwapchainImage(screen)")) return false;
	XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	wi.timeout = XR_INFINITE_DURATION;
	if (!XrOk(xrWaitSwapchainImage(m_screen.handle, &wi), "xrWaitSwapchainImage(screen)")) return false;
	if (m_blitOk && m_screen.rtvs[idx])
	{
		m_blit.Draw(m_screen.rtvs[idx], capW, capH, m_capture.SRV(), capW, capH, 0.0f, 1.0f);
	}
	else if (m_copyPath && m_capture.IsBGRA())
	{
		m_ctx->CopyResource(m_screen.images[idx].texture, m_capture.Texture());
	}
	XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(m_screen.handle, &ri);
	m_screenHasImage = true;
	return true;
}

void Bridge::PollEvents()
{
	XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
	while (xrPollEvent(g_instance, &ev) == XR_SUCCESS)
	{
		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
		{
			const XrEventDataSessionStateChanged& sc = *(XrEventDataSessionStateChanged*)&ev;
			m_state = sc.state;
			if (m_state == XR_SESSION_STATE_READY)
			{
				XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
				bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if (XrOk(xrBeginSession(m_session, &bi), "xrBeginSession"))
				{
					m_running = true;
					printf("\nHeadset session started.\n");
				}
			}
			else if (m_state == XR_SESSION_STATE_STOPPING)
			{
				xrEndSession(m_session);
				m_running = false;
				m_link.SetFlags(0);
				printf("\nHeadset session stopped.\n");
			}
			else if (m_state == XR_SESSION_STATE_EXITING || m_state == XR_SESSION_STATE_LOSS_PENDING)
			{
				g_quit = true;
			}
		}
		else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
		{
			g_quit = true;
		}
		ev = { XR_TYPE_EVENT_DATA_BUFFER };
	}
}

void Bridge::Recenter()
{
	if (!m_haveRaw) return;
	m_centerInv = QYaw(-YawOf(m_lastRaw.orientation));
	m_centerPos = m_lastRaw.position;
	m_centered = true;
	m_screenFrozen = false;		// re-place the floating screen too
	printf("\nRecentred.\n");
}

void Bridge::PublishPose(XrTime time)
{
	// Head pose...
	XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
	if (XR_FAILED(xrLocateSpace(m_viewSpace, m_localSpace, time, &loc))) return;
	if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return;
	m_lastRaw = loc.pose;
	if (!(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) m_lastRaw.position = m_centerPos;
	m_haveRaw = true;
	if (!m_centered) Recenter();

	// ...and how much each eye's picture needs to cover.
	XrViewLocateInfo vi = { XR_TYPE_VIEW_LOCATE_INFO };
	vi.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	vi.displayTime = time;
	vi.space = m_viewSpace;
	XrViewState vs = { XR_TYPE_VIEW_STATE };
	XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
	uint32_t n = 0;
	if (XR_SUCCEEDED(xrLocateViews(m_session, &vi, &vs, 2, &n, views)) && n == 2)
	{
		float tx = 0.0f, ty = 0.0f;
		for (int i = 0; i < 2; ++i)
		{
			tx = std::max(tx, std::max(std::tan(-views[i].fov.angleLeft), std::tan(views[i].fov.angleRight)));
			ty = std::max(ty, std::max(std::tan(views[i].fov.angleUp), std::tan(-views[i].fov.angleDown)));
		}
		if (tx > 0.1f && ty > 0.1f) { m_needTanX = tx; m_needTanY = ty; }
	}

	// Remember the raw pose so we can submit the frame the game renders with it.
	++m_poseId;
	if (m_poseId <= 0) m_poseId = 1;
	PoseRecord& rec = m_history[m_poseId & 255];
	rec.id = m_poseId;
	rec.pose = m_lastRaw;

	// Everything for the game, in its own coordinate system.
	BridgeData d = {};
	d.poseId = m_poseId;
	PollInput(time, d);								// may recentre, so do it first
	ToGameSpace(m_lastRaw, d.fwd, d.up, d.pos);
	d.yaw   = std::atan2(d.fwd[0], d.fwd[2]);
	d.pitch = -std::atan2(d.fwd[1], std::sqrt(d.fwd[0]*d.fwd[0] + d.fwd[2]*d.fwd[2]));
	d.needTanX = m_needTanX;
	d.needTanY = m_needTanY;
	d.flags = (m_running && (m_state == XR_SESSION_STATE_VISIBLE || m_state == XR_SESSION_STATE_FOCUSED))
			  ? SHOGOVR_BRIDGE_HEADSET_ACTIVE : 0;
	if (m_blitOk) d.flags |= SHOGOVR_BRIDGE_LAYERS;		// we can show the HUD strip and flat screens
	m_link.WriteBridge(d);
}

bool Bridge::CopyEyes(const GameInfo& g)
{
	UINT capW = m_capture.Width();
	UINT capH = m_capture.Height();
	UINT eyeW = capW / 2;
	UINT eyeH = capH;
	if (g.eyeHeight > 0 && (UINT)g.eyeHeight < capH) eyeH = (UINT)g.eyeHeight;	// HUD strip below
	if (eyeW < 8 || eyeH < 8) return false;

	// Upscale towards the headset's own resolution (at most 2x) with a sharp
	// bicubic filter, instead of leaving the runtime to stretch a small image.
	float k = 1.0f;
	if (m_upscale && m_blitOk && m_recW > eyeW) k = std::min(2.0f, (float)m_recW / (float)eyeW);
	UINT outW = (UINT)(eyeW * k + 0.5f);
	UINT outH = (UINT)(eyeH * k + 0.5f);

	// Comfort vignette: fades in while the sticks move or turn you.
	m_vignetteNow += (m_motion - m_vignetteNow) * 0.3f;
	float vignette = m_vignette * m_vignetteNow;

	if (!m_blitOk && !m_capture.IsBGRA()) return false;
	bool bShader = m_blitOk && (!m_copyPath || !m_capture.IsBGRA() || k > 1.001f || m_sharpen > 0.01f || m_vignette > 0.01f);
	if (!bShader && !m_copyPath) return false;

	if (m_eyes[0].handle == XR_NULL_HANDLE || m_eyes[0].w != outW || m_eyes[0].h != outH)
	{
		if (!CreateSwapchains(outW, outH)) return false;
	}

	ID3D11ShaderResourceView* src = m_capture.SRV();
	if (bShader && m_sharpen > 0.01f) src = m_blit.Sharpen(src, capW, capH, eyeW, eyeH, m_sharpen);

	for (int eye = 0; eye < 2; ++eye)
	{
		EyeSwapchain& e = m_eyes[eye];
		int half = eye ^ (g.swapEyes ? 1 : 0);		// which half of the window holds this eye

		uint32_t idx = 0;
		XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		if (!XrOk(xrAcquireSwapchainImage(e.handle, &ai, &idx), "xrAcquireSwapchainImage")) return false;
		XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		wi.timeout = XR_INFINITE_DURATION;
		if (!XrOk(xrWaitSwapchainImage(e.handle, &wi), "xrWaitSwapchainImage")) return false;

		if (bShader && e.rtvs[idx])
		{
			float uw = (float)eyeW / (float)capW;
			m_blit.Draw(e.rtvs[idx], outW, outH, src, capW, capH, half * uw, uw, (float)eyeH / (float)capH, vignette);
		}
		else
		{
			D3D11_BOX box = { half * eyeW, 0, 0, half * eyeW + eyeW, eyeH, 1 };
			m_ctx->CopySubresourceRegion(e.images[idx].texture, 0, 0, 0, 0, m_capture.Texture(), 0, &box);
		}

		XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(e.handle, &ri);
	}
	m_eyesHaveImage = true;
	return true;
}

void Bridge::LoadSettings(bool bFirst)
{
	if (m_iniPath.empty())
	{
		// ShogoVR.ini next to the bridge - shared with the game and the
		// Shogo VR Settings window.
		wchar_t exe[2048];
		DWORD n = GetModuleFileNameW(nullptr, exe, 2048);
		std::wstring path(exe, n);
		size_t slash = path.find_last_of(L"\\/");
		m_iniPath = path.substr(0, slash == std::wstring::npos ? 0 : slash + 1) + L"ShogoVR.ini";
	}

	const wchar_t* ini = m_iniPath.c_str();
	wchar_t buf[64];
	GetPrivateProfileStringW(L"Picture", L"Sharpen", L"0.5", buf, 64, ini);
	float sharpen = std::max(0.0f, std::min(1.0f, (float)_wtof(buf)));
	bool upscale = GetPrivateProfileIntW(L"Picture", L"Upscale", 1, ini) != 0;
	bool keepFocus = GetPrivateProfileIntW(L"Window", L"KeepFocus", 1, ini) != 0;
	bool leftHanded = GetPrivateProfileIntW(L"Controls", L"LeftHanded", 0, ini) != 0;
	GetPrivateProfileStringW(L"Comfort", L"Vignette", L"0", buf, 64, ini);
	float vignette = std::max(0.0f, std::min(1.0f, (float)_wtof(buf)));
	bool direct = GetPrivateProfileIntW(L"Picture", L"DirectCapture", 1, ini) != 0;
	bool spectator = GetPrivateProfileIntW(L"Picture", L"Spectator", 0, ini) != 0;

	bool bChanged = sharpen != m_sharpen || upscale != m_upscale || keepFocus != m_keepFocus ||
					leftHanded != m_leftHanded || vignette != m_vignette || direct != m_directCapture || spectator != m_spectatorOn;
	m_sharpen = sharpen;
	m_upscale = upscale;
	m_keepFocus = keepFocus;
	m_leftHanded = leftHanded;
	m_vignette = vignette;
	m_directCapture = direct;
	if (!spectator && m_spectatorOn) m_spectator.Close();
	m_spectatorOn = spectator;

	WIN32_FILE_ATTRIBUTE_DATA fa;
	if (GetFileAttributesExW(ini, GetFileExInfoStandard, &fa)) m_iniTime = fa.ftLastWriteTime;

	if (bFirst || bChanged)
	{
		printf("%s%s-handed, sharpening %.0f%%, upscaling %s, vignette %.0f%%\n",
			   bFirst ? "Settings: " : "\nSettings updated: ", m_leftHanded ? "left" : "right",
			   m_sharpen * 100.0f, m_upscale ? "on" : "off", m_vignette * 100.0f);
	}
}

// The Settings window (or a text editor) changed ShogoVR.ini: apply it now.
void Bridge::CheckSettingsFile()
{
	ULONGLONG now = GetTickCount64();
	if (now - m_lastIniCheck < 1000) return;
	m_lastIniCheck = now;

	WIN32_FILE_ATTRIBUTE_DATA fa;
	if (!m_iniPath.empty() && GetFileAttributesExW(m_iniPath.c_str(), GetFileExInfoStandard, &fa) &&
		CompareFileTime(&fa.ftLastWriteTime, &m_iniTime) != 0)
	{
		LoadSettings(false);
	}
}

void Bridge::SaveSettings()
{
	wchar_t buf[64];
	swprintf(buf, 64, L"%.2f", m_sharpen);
	WritePrivateProfileStringW(L"Picture", L"Sharpen", buf, m_iniPath.c_str());
	WritePrivateProfileStringW(L"Picture", L"Upscale", m_upscale ? L"1" : L"0", m_iniPath.c_str());
	WIN32_FILE_ATTRIBUTE_DATA fa;
	if (GetFileAttributesExW(m_iniPath.c_str(), GetFileExInfoStandard, &fa)) m_iniTime = fa.ftLastWriteTime;
}

void Bridge::Frame()
{
	XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState fs = { XR_TYPE_FRAME_STATE };
	if (!XrOk(xrWaitFrame(m_session, &fwi, &fs), "xrWaitFrame")) { Sleep(10); return; }
	XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
	if (XR_FAILED(xrBeginFrame(m_session, &fbi))) return;

	PublishPose(fs.predictedDisplayTime);

	GameInfo g = m_link.ReadGame();

	// Recentre: Ctrl+Shift+R, or the game's VRRecenter command.
	bool keys = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState('R') & 0x8000);
	if (keys && !m_recenterKeyDown) Recenter();
	m_recenterKeyDown = keys;
	if (g.recenterRequest != m_lastRecenterRequest) { m_lastRecenterRequest = g.recenterRequest; Recenter(); }

	HandleWindow(g);
	CheckSettingsFile();

	// Ctrl+Shift+M: the left-eye window for recording.
	bool mKey = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState('M') & 0x8000);
	Spectator::Pump();
	if ((mKey && !m_spectatorKeyDown) || (m_spectator.WantsClose() && m_spectatorOn))
	{
		m_spectatorOn = !m_spectatorOn;
		if (!m_spectatorOn) m_spectator.Close();
		printf("\nLeft-eye window (for recording) %s.\n", m_spectatorOn ? "on" : "off");
		WritePrivateProfileStringW(L"Picture", L"Spectator", m_spectatorOn ? L"1" : L"0", m_iniPath.c_str());
		WIN32_FILE_ATTRIBUTE_DATA fa;
		if (GetFileAttributesExW(m_iniPath.c_str(), GetFileExInfoStandard, &fa)) m_iniTime = fa.ftLastWriteTime;
	}
	m_spectatorKeyDown = mKey;

	// Picture hotkeys: Ctrl+Shift+S cycles sharpening, Ctrl+Shift+U toggles upscaling.
	bool cs = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
	bool sKey = cs && (GetAsyncKeyState('S') & 0x8000);
	if (sKey && !m_sharpKeyDown)
	{
		m_sharpen = (m_sharpen < 0.01f) ? 0.25f : (m_sharpen < 0.3f) ? 0.5f : (m_sharpen < 0.6f) ? 0.8f : (m_sharpen < 0.9f) ? 1.0f : 0.0f;
		printf("\nSharpening: %.0f%%\n", m_sharpen * 100.0f);
		SaveSettings();
	}
	m_sharpKeyDown = sKey;
	bool uKey = cs && (GetAsyncKeyState('U') & 0x8000);
	if (uKey && !m_upKeyDown)
	{
		m_upscale = !m_upscale;
		printf("\nUpscaling: %s\n", m_upscale ? "on" : "off");
		SaveSettings();
	}
	m_upKeyDown = uKey;

	// New picture from the game?
	if (g.alive && g.viewMode != SHOGOVR_VIEW_OFF && CaptureFrame(g))
	{
		++m_capturedFrames;
		m_shown = g;		// remember what this picture was rendered with
		if (fs.shouldRender)
		{
			if (g.viewMode == SHOGOVR_VIEW_FLAT)
			{
				CopyScreen(g);
			}
			else
			{
				CopyEyes(g);
				if (g.layoutFlags & SHOGOVR_LAYOUT_HUD) CopyHud(g);
			}
			DrawSpectator(g);
		}
	}

	XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerProjectionView pv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
	XrCompositionLayerQuad hudQuad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad screenQuad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	const XrCompositionLayerBaseHeader* layers[2] = {};
	uint32_t layerCount = 0;

	if (fs.shouldRender && g.alive && m_shown.viewMode == SHOGOVR_VIEW_FLAT && m_screenHasImage)
	{
		// Flat game picture on a floating screen, fixed where it appeared.
		if (!m_screenFrozen)
		{
			m_screenPose.orientation = QYaw(YawOf(m_lastRaw.orientation));
			m_screenPose.position = m_lastRaw.position;
			m_screenFrozen = true;
		}
		const float dist = 3.0f;
		float angle = (m_shown.screenAngle > 0.1f && m_shown.screenAngle < 3.0f) ? m_shown.screenAngle : 1.4f;
		float width = 2.0f * dist * std::tan(angle * 0.5f);
		XrVector3f fwd = QRotate(m_screenPose.orientation, XrVector3f{ 0.0f, 0.0f, -dist });

		screenQuad.space = m_localSpace;
		screenQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		screenQuad.subImage.swapchain = m_screen.handle;
		screenQuad.subImage.imageRect.offset = { 0, 0 };
		screenQuad.subImage.imageRect.extent = { (int32_t)m_screen.w, (int32_t)m_screen.h };
		screenQuad.pose.orientation = m_screenPose.orientation;
		screenQuad.pose.position = XrVector3f{ m_screenPose.position.x + fwd.x, m_screenPose.position.y + fwd.y, m_screenPose.position.z + fwd.z };
		screenQuad.size = XrExtent2Df{ width, width * (float)m_screen.h / (float)m_screen.w };
		layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&screenQuad;
		++m_submittedFrames;
	}
	else if (fs.shouldRender && g.alive && m_eyesHaveImage && m_shown.viewMode != SHOGOVR_VIEW_OFF &&
		m_shown.viewMode != SHOGOVR_VIEW_FLAT && m_shown.renderTanX > 0.01f && m_shown.renderTanY > 0.01f)
	{
		// Which head pose was this picture rendered from?
		XrPosef base = m_lastRaw;
		if (m_shown.viewMode == SHOGOVR_VIEW_TRACKED)
		{
			m_screenFrozen = false;		// next floating screen appears where you look then
			const PoseRecord& rec = m_history[m_shown.usedPoseId & 255];
			if (m_shown.usedPoseId != 0 && rec.id == m_shown.usedPoseId) base = rec.pose;
		}
		else
		{
			// Floating screen: keep it where it was when it appeared, upright.
			if (!m_screenFrozen)
			{
				m_screenPose.orientation = QYaw(YawOf(m_lastRaw.orientation));
				m_screenPose.position = m_lastRaw.position;
				m_screenFrozen = true;
			}
			base = m_screenPose;
		}

		XrVector3f right = QRotate(base.orientation, XrVector3f{ 1.0f, 0.0f, 0.0f });
		float half = m_shown.eyeSeparation * 0.5f;
		float ax = std::atan(m_shown.renderTanX);
		float ay = std::atan(m_shown.renderTanY);

		for (int eye = 0; eye < 2; ++eye)
		{
			float s = (eye == 0) ? -half : half;
			pv[eye].pose.orientation = base.orientation;
			pv[eye].pose.position = XrVector3f{ base.position.x + right.x * s, base.position.y + right.y * s, base.position.z + right.z * s };
			pv[eye].fov.angleLeft	= -ax;
			pv[eye].fov.angleRight	=  ax;
			pv[eye].fov.angleUp		=  ay;
			pv[eye].fov.angleDown	= -ay;
			pv[eye].subImage.swapchain = m_eyes[eye].handle;
			pv[eye].subImage.imageRect.offset = { 0, 0 };
			pv[eye].subImage.imageRect.extent = { (int32_t)m_eyes[eye].w, (int32_t)m_eyes[eye].h };
			pv[eye].subImage.imageArrayIndex = 0;
		}

		layer.space = m_localSpace;
		layer.viewCount = 2;
		layer.views = pv;
		layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&layer;
		++m_submittedFrames;

		// The HUD as its own panel in front of your eyes, drawn by the
		// headset at full resolution.
		if ((m_shown.layoutFlags & SHOGOVR_LAYOUT_HUD) && m_hudHasImage && m_shown.hudW > 0 && m_shown.hudH > 0)
		{
			float dist = (m_shown.hudDistance > 0.2f && m_shown.hudDistance < 50.0f) ? m_shown.hudDistance : 3.0f;
			float angle = (m_shown.hudAngle > 0.1f && m_shown.hudAngle < 2.8f) ? m_shown.hudAngle : 0.96f;
			float width = 2.0f * dist * std::tan(angle * 0.5f);

			hudQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			hudQuad.space = m_viewSpace;					// moves with your head
			hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			hudQuad.subImage.swapchain = m_hud.handle;
			hudQuad.subImage.imageRect.offset = { 0, 0 };
			hudQuad.subImage.imageRect.extent = { (int32_t)m_hud.w, (int32_t)m_hud.h };
			hudQuad.pose = IdentityPose();
			hudQuad.pose.position.z = -dist;
			hudQuad.size = XrExtent2Df{ width, width * (float)m_shown.hudH / (float)m_shown.hudW };
			layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&hudQuad;
		}
	}

	XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
	fei.displayTime = fs.predictedDisplayTime;
	fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	fei.layerCount = layerCount;
	fei.layers = layerCount ? layers : nullptr;
	XrOk(xrEndFrame(m_session, &fei), "xrEndFrame");

	PrintStatus(g);
}

void Bridge::PrintStatus(const GameInfo& g)
{
	ULONGLONG now = GetTickCount64();
	if (now - m_lastStatus < 1000) return;
	float secs = m_lastStatus ? (now - m_lastStatus) / 1000.0f : 1.0f;
	m_lastStatus = now;

	const char* game = !g.alive ? "waiting for Shogo (start it with -rez ShogoVR)" :
					   g.viewMode == SHOGOVR_VIEW_OFF ? "connected, stereo is off" :
					   g.viewMode == SHOGOVR_VIEW_TRACKED ? "head tracked" : "floating screen";
	const char* cap = m_capture.LastError();
	printf("\r%-46s | capture %5.1f fps | headset %5.1f fps | eye %ux%u %s      ",
		   game, m_capturedFrames / secs, m_submittedFrames / secs, m_eyes[0].w, m_eyes[0].h, cap ? cap : "");
	fflush(stdout);
	m_capturedFrames = m_submittedFrames = 0;
}

void Bridge::Run()
{
	printf("\nRunning.  Hotkeys (work while Shogo has focus too):\n"
		   "  Ctrl+Shift+R  recentre the view (or click the left thumbstick)\n"
		   "  Ctrl+Shift+F  fix the Shogo window size and bring it to the front\n"
		   "  Ctrl+Shift+D  stop Windows display scaling from blurring Shogo\n"
		   "  Ctrl+Shift+S  change sharpening (off / 25 / 50 / 80 / 100%%)\n"
		   "  Ctrl+Shift+U  upscaling on/off\n"
		   "  Ctrl+C        quit\n");
	while (!g_quit || m_running)
	{
		CheckQuitEvent();
		if (g_quit && m_running && !m_exitRequested)
		{
			xrRequestExitSession(m_session);
			m_exitRequested = true;
		}

		PollEvents();

		if (m_running)
		{
			Frame();
		}
		else
		{
			// Headset not showing us yet: still look after the game window.
			GameInfo g = m_link.ReadGame();
			HandleWindow(g);
			CheckSettingsFile();
			Spectator::Pump();
			Sleep(20);
			if (g_quit) break;
		}
	}
}

void Bridge::Shutdown()
{
	m_link.Close();
	DestroySwapchains();
	m_spectator.Close();
	for (auto& h : m_hands)
	{
		if (h.gunSpace != XR_NULL_HANDLE) xrDestroySpace(h.gunSpace);
		if (h.offSpace != XR_NULL_HANDLE) xrDestroySpace(h.offSpace);
		if (h.set != XR_NULL_HANDLE) xrDestroyActionSet(h.set);
	}
	if (m_viewSpace != XR_NULL_HANDLE) xrDestroySpace(m_viewSpace);
	if (m_localSpace != XR_NULL_HANDLE) xrDestroySpace(m_localSpace);
	if (m_session != XR_NULL_HANDLE) xrDestroySession(m_session);
	m_capture.Shutdown();
	m_blit.Shutdown();
	SafeRelease(m_ctx);
	SafeRelease(m_dev);
	SafeRelease(m_adapter);
	if (g_instance != XR_NULL_HANDLE && xrDestroyInstance) xrDestroyInstance(g_instance);
	g_instance = XR_NULL_HANDLE;
}


// ======================================================================= //

static BOOL WINAPI CtrlHandler(DWORD type)
{
	if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT)
	{
		g_quit = true;
		if (type == CTRL_CLOSE_EVENT) Sleep(2000);	// give the loop a moment to shut down cleanly
		return TRUE;
	}
	return FALSE;
}

static void MakeDpiAware()
{
	// Physical pixels for window/monitor coordinates, so capture lines up on scaled displays.
	typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
	HMODULE user32 = GetModuleHandleW(L"user32.dll");
	SetCtxFn setCtx = user32 ? (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext") : nullptr;
	if (!setCtx || !setCtx((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */))
	{
		SetProcessDPIAware();
	}
}

int main(int argc, char** argv)
{
	// "--game-pid N": started by the game itself (no window) - close when it does.
	for (int i = 1; i + 1 < argc; ++i)
		if (strcmp(argv[i], "--game-pid") == 0)
			g_gameProcess = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)strtoul(argv[i + 1], nullptr, 10));

	printf("ShogoVR bridge - stage 2/3\n"
		   "--------------------------\n");
	OpenLog();
	printf("Shogo VR - an unofficial VR mod, unaffiliated with Monolith Productions or any of its affiliates and subsidiaries.\n\n");
	SetConsoleCtrlHandler(CtrlHandler, TRUE);
	MakeDpiAware();

	// Only one bridge at a time (ShogoVR.exe checks this too).
	HANDLE running = CreateMutexA(nullptr, TRUE, "Local\\ShogoVRBridge_Running");
	if (running && GetLastError() == ERROR_ALREADY_EXISTS)
	{
		printf("The ShogoVR bridge is already running.\n");
		Sleep(3000);
		return 0;
	}
	g_quitEvent = CreateEventA(nullptr, TRUE, FALSE, "Local\\ShogoVRBridge_Quit");
	if (g_quitEvent) ResetEvent(g_quitEvent);

	Bridge bridge;
	int ret = 0;
	if (bridge.Init())
	{
		bridge.Run();
	}
	else
	{
		printf("\nThe bridge couldn't start.  See the message above.\n");
		ret = 1;
	}
	bridge.Shutdown();

	HWND console = GetConsoleWindow();
	if (ret && !g_quit && console && IsWindowVisible(console))		// leave the message up, unless ShogoVR.exe asked us to close
	{
		printf("Press Enter to close.\n");
		getchar();
	}
	return ret;
}
