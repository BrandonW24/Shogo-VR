// ======================================================================= //
//
// MODULE  : VRCapture.cpp
//
// PURPOSE : Direct capture from the renderer.
//
//           With dgVoodoo, Shogo's DirectDraw/Direct3D 6 calls end up as a
//           Direct3D 11 swap chain in this process.  Every swap chain shares
//           one function table, so replacing its Present entry lets us see
//           each finished frame just before it goes to the window: we copy
//           it into a shared texture that the headset bridge opens.
//
//           A keyed mutex keeps the two sides apart without ever waiting:
//           the game copies only when the bridge has finished with the last
//           frame (key 0), the bridge reads only a new one (key 1).
//
//           Anything unexpected (no Direct3D 11, a Direct3D 12 output, a
//           different graphics card) simply means no shared texture, and the
//           bridge goes on capturing the screen as before.
//
// ======================================================================= //

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include "ShogoVRShared.h"
#include "VRCapture.h"

typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *Present1Fn)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT (WINAPI *CreateDeviceAndSwapChainFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
													  UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
													  ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

static void**			s_vtbl			= NULL;		// swap chain function table we patched
static void**			s_vtbl1			= NULL;
static PresentFn		s_origPresent	= NULL;
static Present1Fn		s_origPresent1	= NULL;
static int				s_inHook		= 0;

static HWND				s_hwnd			= NULL;
static ShogoVRShared*	s_pShared		= NULL;
static int				s_bEnabled		= 1;

static ID3D11Device*	s_dev			= NULL;
static ID3D11Texture2D*	s_tex			= NULL;
static IDXGIKeyedMutex*	s_mutex			= NULL;
static HANDLE			s_handle		= NULL;
static UINT				s_w = 0, s_h = 0;
static DXGI_FORMAT		s_fmt			= DXGI_FORMAT_UNKNOWN;
static LONG				s_frames		= 0;
static LONG				s_seen			= 0;		// frames the hook saw from the game's window

static HHOOK			s_sizeHook		= NULL;		// lets the window grow beyond the screen
static DWORD			s_hookThread	= 0;

static void Publish()
{
	if (!s_pShared) return;
	s_pShared->captureHandle	= s_tex ? (long)(LONG_PTR)s_handle : 0;
	s_pShared->captureWidth		= s_tex ? (long)s_w : 0;
	s_pShared->captureHeight	= s_tex ? (long)s_h : 0;
	s_pShared->captureFormat	= s_tex ? (long)s_fmt : 0;
}

static void ReleaseShared()
{
	if (s_mutex) { s_mutex->Release(); s_mutex = NULL; }
	if (s_tex) { s_tex->Release(); s_tex = NULL; }
	if (s_dev) { s_dev->Release(); s_dev = NULL; }
	s_handle = NULL;
	s_w = s_h = 0;
	s_fmt = DXGI_FORMAT_UNKNOWN;
	Publish();
}

static void CaptureFrom(IDXGISwapChain* pSC)
{
	if (!s_bEnabled || !s_pShared || !pSC) return;

	DXGI_SWAP_CHAIN_DESC scd;
	if (FAILED(pSC->GetDesc(&scd))) return;
	if (s_hwnd && scd.OutputWindow != s_hwnd) return;		// only the game's own window
	++s_seen;

	ID3D11Texture2D* pBack = NULL;
	if (FAILED(pSC->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBack)) || !pBack) return;	// not Direct3D 11

	ID3D11Device* pDev = NULL;
	pBack->GetDevice(&pDev);
	D3D11_TEXTURE2D_DESC td;
	pBack->GetDesc(&td);

	if (pDev && (!s_tex || pDev != s_dev || td.Width != s_w || td.Height != s_h || td.Format != s_fmt))
	{
		ReleaseShared();
		D3D11_TEXTURE2D_DESC sd = td;
		sd.MipLevels = 1;
		sd.ArraySize = 1;
		sd.SampleDesc.Count = 1;
		sd.SampleDesc.Quality = 0;
		sd.Usage = D3D11_USAGE_DEFAULT;
		sd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		sd.CPUAccessFlags = 0;
		sd.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
		if (SUCCEEDED(pDev->CreateTexture2D(&sd, NULL, &s_tex)) && s_tex)
		{
			IDXGIResource* pRes = NULL;
			if (SUCCEEDED(s_tex->QueryInterface(__uuidof(IDXGIResource), (void**)&pRes)) && pRes)
			{
				pRes->GetSharedHandle(&s_handle);
				pRes->Release();
			}
			s_tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&s_mutex);
			if (s_handle && s_mutex)
			{
				s_dev = pDev;
				s_dev->AddRef();
				s_w = td.Width;
				s_h = td.Height;
				s_fmt = td.Format;
				Publish();
			}
			else
			{
				ReleaseShared();
			}
		}
	}

	if (s_tex && s_mutex && pDev == s_dev)
	{
		// Only when the bridge has taken the previous frame - never wait.
		if (s_mutex->AcquireSync(0, 0) == S_OK)
		{
			ID3D11DeviceContext* pCtx = NULL;
			s_dev->GetImmediateContext(&pCtx);
			if (pCtx)
			{
				if (td.SampleDesc.Count > 1) pCtx->ResolveSubresource(s_tex, 0, pBack, 0, td.Format);
				else pCtx->CopyResource(s_tex, pBack);
				pCtx->Release();
			}
			s_mutex->ReleaseSync(1);
			s_pShared->captureFrame = ++s_frames;
		}
	}

	if (pDev) pDev->Release();
	pBack->Release();
}

static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* pSC, UINT nSync, UINT nFlags)
{
	if (!s_inHook && !(nFlags & DXGI_PRESENT_TEST))
	{
		s_inHook = 1;
		CaptureFrom(pSC);
		s_inHook = 0;
	}
	return s_origPresent(pSC, nSync, nFlags);
}

static HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* pSC, UINT nSync, UINT nFlags, const DXGI_PRESENT_PARAMETERS* pParams)
{
	if (!s_inHook && !(nFlags & DXGI_PRESENT_TEST))
	{
		s_inHook = 1;
		CaptureFrom(pSC);
		s_inHook = 0;
	}
	return s_origPresent1(pSC, nSync, nFlags, pParams);
}

static void PatchSlot(void** vtbl, int nSlot, void* pNew, void** pOld)
{
	DWORD dwOld = 0;
	if (!VirtualProtect(&vtbl[nSlot], sizeof(void*), PAGE_READWRITE, &dwOld)) return;
	if (pOld) *pOld = vtbl[nSlot];
	vtbl[nSlot] = pNew;
	VirtualProtect(&vtbl[nSlot], sizeof(void*), dwOld, &dwOld);
}

int VRCapture_Install()
{
	if (s_vtbl) return 1;

	HMODULE hD3D11 = LoadLibraryA("d3d11.dll");
	if (!hD3D11) return 0;
	CreateDeviceAndSwapChainFn pfnCreate = (CreateDeviceAndSwapChainFn)GetProcAddress(hD3D11, "D3D11CreateDeviceAndSwapChain");
	if (!pfnCreate) return 0;

	// A throwaway swap chain, just to find the function table all swap chains share.
	WNDCLASSA wc;
	ZeroMemory(&wc, sizeof(wc));
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.lpszClassName = "ShogoVRCaptureProbe";
	RegisterClassA(&wc);
	HWND hProbe = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPED, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
	if (!hProbe) return 0;

	DXGI_SWAP_CHAIN_DESC sd;
	ZeroMemory(&sd, sizeof(sd));
	sd.BufferCount = 1;
	sd.BufferDesc.Width = 64;
	sd.BufferDesc.Height = 64;
	sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hProbe;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	IDXGISwapChain* pSC = NULL;
	ID3D11Device* pDev = NULL;
	ID3D11DeviceContext* pCtx = NULL;
	HRESULT hr = pfnCreate(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &pSC, &pDev, NULL, &pCtx);
	if (FAILED(hr)) hr = pfnCreate(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &pSC, &pDev, NULL, &pCtx);

	if (SUCCEEDED(hr) && pSC)
	{
		// IUnknown(3) + IDXGIObject(4) + IDXGIDeviceSubObject(1) -> Present is slot 8.
		void** vtbl = *(void***)pSC;
		PatchSlot(vtbl, 8, (void*)HookPresent, (void**)&s_origPresent);
		s_vtbl = vtbl;

		// IDXGISwapChain1::Present1 is slot 22 (flip-model swap chains may use it).
		IDXGISwapChain1* pSC1 = NULL;
		if (SUCCEEDED(pSC->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&pSC1)) && pSC1)
		{
			void** vtbl1 = *(void***)pSC1;
			PatchSlot(vtbl1, 22, (void*)HookPresent1, (void**)&s_origPresent1);
			s_vtbl1 = vtbl1;
			pSC1->Release();
		}
	}

	if (pSC) pSC->Release();
	if (pCtx) pCtx->Release();
	if (pDev) pDev->Release();
	DestroyWindow(hProbe);
	return s_vtbl ? 1 : 0;
}

// The window may need to be bigger than the screen (when the renderer draws
// at the window's size).  Windows can cap that through WM_GETMINMAXINFO; this
// lifts the cap.  A thread hook rather than replacing the window procedure,
// so it can always be removed cleanly.
static LRESULT CALLBACK SizeHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode == HC_ACTION && lParam)
	{
		CWPRETSTRUCT* p = (CWPRETSTRUCT*)lParam;
		if (p->message == WM_GETMINMAXINFO && p->hwnd == s_hwnd && p->lParam)
		{
			MINMAXINFO* pMMI = (MINMAXINFO*)p->lParam;
			pMMI->ptMaxTrackSize.x = pMMI->ptMaxSize.x = 16384;
			pMMI->ptMaxTrackSize.y = pMMI->ptMaxSize.y = 16384;
		}
	}
	return CallNextHookEx(s_sizeHook, nCode, wParam, lParam);
}

void VRCapture_SetGame(void* hwnd, void* pSharedBlock, int bEnabled)
{
	ShogoVRShared* pShared = (ShogoVRShared*)pSharedBlock;
	s_hwnd = (HWND)hwnd;
	if (pShared != s_pShared)
	{
		s_pShared = pShared;
		Publish();
	}
	if (!bEnabled && s_tex) ReleaseShared();
	s_bEnabled = bEnabled;

	if (s_hwnd && s_vtbl && !s_sizeHook)
	{
		s_hookThread = GetWindowThreadProcessId(s_hwnd, NULL);
		s_sizeHook = SetWindowsHookExA(WH_CALLWNDPROCRET, SizeHookProc, NULL, s_hookThread);
	}
}

void VRCapture_Uninstall()
{
	if (s_sizeHook) { UnhookWindowsHookEx(s_sizeHook); s_sizeHook = NULL; }

	// Put the original functions back (only if they're still ours).
	if (s_vtbl && s_origPresent && s_vtbl[8] == (void*)HookPresent) PatchSlot(s_vtbl, 8, (void*)s_origPresent, NULL);
	if (s_vtbl1 && s_origPresent1 && s_vtbl1[22] == (void*)HookPresent1) PatchSlot(s_vtbl1, 22, (void*)s_origPresent1, NULL);
	s_vtbl = s_vtbl1 = NULL;

	ReleaseShared();
	s_pShared = NULL;
}

int VRCapture_Frames()
{
	return (int)s_frames;
}

int VRCapture_Seen()
{
	return (int)s_seen;
}
