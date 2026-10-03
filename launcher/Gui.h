// ======================================================================= //
//
// Gui.h  -  the shared look of the Shogo VR launcher and installer
//
//   * Images built into the .exe (PNG/JPEG resources), decoded and scaled
//     with Windows' own image codecs (WIC).
//   * A dark header band with the "In Memory of Monolith Productions" art,
//     fonts that follow the system's, and red primary buttons.
//   * The attribution / legal text shown by both programs.
//
// ======================================================================= //

#pragma once
#include "Common.h"
#include <commctrl.h>
#include <wincodec.h>

#define IDR_HEADER_PNG		101		// "In Memory of Monolith Productions"
#define IDR_BANNER_JPG		102		// Shogo VR banner

namespace gui
{
	const COLORREF kHeaderBg	= RGB(12, 12, 14);
	const COLORREF kBodyBg		= RGB(250, 250, 250);
	const COLORREF kText		= RGB(25, 25, 28);
	const COLORREF kMuted		= RGB(95, 95, 100);
	const COLORREF kRed			= RGB(178, 26, 30);		// Shogo red
	const COLORREF kRedDark		= RGB(130, 16, 20);
	const COLORREF kNoticeBg	= RGB(255, 244, 220);

	inline int& DpiRef() { static int dpi = 96; return dpi; }
	inline void InitDpi()
	{
		HDC dc = GetDC(nullptr);
		DpiRef() = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
		if (dc) ReleaseDC(nullptr, dc);
	}
	inline int S(int v) { return MulDiv(v, DpiRef(), 96); }

	inline HFONT MakeFont(int pt, bool bold, bool italic = false)
	{
		NONCLIENTMETRICSW ncm = { sizeof(ncm) };
		SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
		LOGFONTW lf = ncm.lfMessageFont;
		lf.lfHeight = -MulDiv(pt, DpiRef(), 72);
		lf.lfWeight = bold ? FW_BOLD : FW_NORMAL;
		lf.lfItalic = italic ? TRUE : FALSE;
		lf.lfQuality = CLEARTYPE_QUALITY;
		return CreateFontIndirectW(&lf);
	}

	// A built-in image, scaled to fit (w x h) keeping its shape, as a 32-bit
	// premultiplied bitmap ready for AlphaBlend.
	struct Image
	{
		HBITMAP	bmp = nullptr;
		int		w = 0, h = 0;
		void Free() { if (bmp) DeleteObject(bmp); bmp = nullptr; w = h = 0; }
	};

	inline Image LoadImageResource(int id, int maxW, int maxH)
	{
		Image img;
		HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
		HGLOBAL mem = res ? LoadResource(nullptr, res) : nullptr;
		void* data = mem ? LockResource(mem) : nullptr;
		DWORD size = res ? SizeofResource(nullptr, res) : 0;
		if (!data || !size) return img;

		IWICImagingFactory* factory = nullptr;
		IWICStream* stream = nullptr;
		IWICBitmapDecoder* decoder = nullptr;
		IWICBitmapFrameDecode* frame = nullptr;
		IWICFormatConverter* conv = nullptr;
		IWICBitmapScaler* scaler = nullptr;
		bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&factory)) &&
				  SUCCEEDED(factory->CreateStream(&stream)) &&
				  SUCCEEDED(stream->InitializeFromMemory((BYTE*)data, size)) &&
				  SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) &&
				  SUCCEEDED(decoder->GetFrame(0, &frame)) &&
				  SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
				  SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));
		UINT sw = 0, sh = 0;
		if (ok) conv->GetSize(&sw, &sh);
		if (ok && sw && sh)
		{
			double scale = std::min((double)maxW / sw, (double)maxH / sh);
			int tw = std::max(1, (int)(sw * scale + 0.5)), th = std::max(1, (int)(sh * scale + 0.5));
			ok = SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
				 SUCCEEDED(scaler->Initialize(conv, tw, th, WICBitmapInterpolationModeFant));
			if (ok)
			{
				BITMAPINFO bi = {};
				bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
				bi.bmiHeader.biWidth = tw;
				bi.bmiHeader.biHeight = -th;			// top-down
				bi.bmiHeader.biPlanes = 1;
				bi.bmiHeader.biBitCount = 32;
				bi.bmiHeader.biCompression = BI_RGB;
				void* bits = nullptr;
				img.bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
				if (img.bmp && SUCCEEDED(scaler->CopyPixels(nullptr, tw * 4, tw * th * 4, (BYTE*)bits))) { img.w = tw; img.h = th; }
				else img.Free();
			}
		}
		if (scaler) scaler->Release();
		if (conv) conv->Release();
		if (frame) frame->Release();
		if (decoder) decoder->Release();
		if (stream) stream->Release();
		if (factory) factory->Release();
		return img;
	}

	inline void DrawImage(HDC dc, const Image& img, int x, int y)
	{
		if (!img.bmp) return;
		HDC mem = CreateCompatibleDC(dc);
		HGDIOBJ old = SelectObject(mem, img.bmp);
		BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
		AlphaBlend(dc, x, y, img.w, img.h, mem, 0, 0, img.w, img.h, bf);
		SelectObject(mem, old);
		DeleteDC(mem);
	}

	inline void FillRectColor(HDC dc, const RECT& rc, COLORREF c)
	{
		HBRUSH b = CreateSolidBrush(c);
		FillRect(dc, &rc, b);
		DeleteObject(b);
	}

	// The red (primary) or plain (secondary) owner-drawn button.
	inline void DrawButton(const DRAWITEMSTRUCT* di, bool primary, HFONT font)
	{
		bool pressed = (di->itemState & ODS_SELECTED) != 0;
		bool disabled = (di->itemState & ODS_DISABLED) != 0;
		bool focus = (di->itemState & ODS_FOCUS) != 0;
		RECT rc = di->rcItem;
		COLORREF fill = primary ? (disabled ? RGB(200, 150, 150) : (pressed ? kRedDark : kRed)) : (pressed ? RGB(225, 225, 228) : RGB(238, 238, 240));
		COLORREF edge = primary ? kRedDark : RGB(190, 190, 195);
		HBRUSH b = CreateSolidBrush(fill);
		HPEN p = CreatePen(PS_SOLID, 1, edge);
		HGDIOBJ ob = SelectObject(di->hDC, b), op = SelectObject(di->hDC, p);
		RoundRect(di->hDC, rc.left, rc.top, rc.right, rc.bottom, S(6), S(6));
		SelectObject(di->hDC, ob); SelectObject(di->hDC, op);
		DeleteObject(b); DeleteObject(p);

		wchar_t text[128];
		GetWindowTextW(di->hwndItem, text, 128);
		SetBkMode(di->hDC, TRANSPARENT);
		SetTextColor(di->hDC, primary ? RGB(255, 255, 255) : (disabled ? RGB(160, 160, 165) : kText));
		HGDIOBJ of = SelectObject(di->hDC, font);
		DrawTextW(di->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		SelectObject(di->hDC, of);
		if (focus && !primary) { InflateRect(&rc, -S(3), -S(3)); DrawFocusRect(di->hDC, &rc); }
	}

	// The creators, from AUTHORS.txt (the Shogo source licence requires their
	// names and email addresses with every release).
	inline std::wstring ReadAuthors(const std::wstring& path)
	{
		std::vector<char> raw;
		if (!ReadWholeFile(path, raw) || raw.empty()) return L"";
		std::string s(raw.begin(), raw.end());
		if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s = s.substr(3);
		std::wstring w = Utf8ToWide(s);
		while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r' || w.back() == L' ')) w.pop_back();
		return w;
	}

	inline std::wstring Crlf(const std::wstring& in)
	{
		std::wstring out;
		for (wchar_t c : in) { if (c == L'\n') out += L"\r\n"; else if (c != L'\r') out += c; }
		return out;
	}

	// The attribution / legal page, shared by the installer and the launcher.
	inline std::wstring AttributionText(const std::wstring& authors)
	{
		std::wstring t;
		t += L"SHOGO VR\n";
		t += L"An unofficial VR mod for Shogo: Mobile Armor Division, unaffiliated with Monolith Productions or any of its affiliates and subsidiaries.\n\n";
		t += L"THIS LEVEL IS NOT MADE BY OR SUPPORTED BY Monolith Productions, or any of its affiliates and subsidiaries.\n\n";
		t += L"Created by:\n" + (authors.empty() ? std::wstring(L"(see AUTHORS.txt)") : authors) + L"\n\n";
		t += L"IN MEMORY OF MONOLITH PRODUCTIONS\n";
		t += L"Shogo: Mobile Armor Division was created by Monolith Productions in 1998. Thank you to everyone at Monolith "
			 L"for the game, for the LithTech engine, and for releasing Shogo's source code to the community in 1999 so that "
			 L"players could keep building on it. This mod exists because of that generosity.\n\n";
		t += L"WHAT THIS IS\n";
		t += L"Shogo VR is free. It may not be sold or used commercially in any way. You need your own copy of Shogo "
			 L"(Steam or GOG); no game files are included. Shogo itself is never changed: the mod lives in its own "
			 L"ShogoVR folder and can be removed at any time from Windows' Installed apps.\n\n";
		t += L"The VR game code (CShell.dll) is built from Monolith's Shogo v2.2 source release under its licence, which "
			 L"allows modifications to be shared free of charge but does not allow the source itself to be redistributed - "
			 L"so this mod is shared as compiled software. As that licence requires, Monolith Productions has an "
			 L"irrevocable, royalty-free right to use and distribute this mod.\n\n";
		t += L"Shogo: Mobile Armor Division, its logo and artwork belong to their respective owners and are used here "
			 L"only to identify the game this mod is for.\n\n";
		t += L"THIRD-PARTY SOFTWARE\n";
		t += L"- Khronos OpenXR headers: Apache License 2.0. OpenXR is a trademark of The Khronos Group Inc.\n";
		t += L"- The sharpening filter follows AMD FidelityFX CAS (MIT licence).\n";
		t += L"- dgVoodoo 2 is not included; if you use it, it's your own copy.\n";
		t += L"SteamVR, Meta Quest, Valve Index, HTC Vive and Windows Mixed Reality are trademarks of their owners.\n\n";
		t += L"NO WARRANTY\n";
		t += L"Provided as is, without warranty of any kind. Use at your own risk.";
		return Crlf(t);
	}
}
