// ======================================================================= //
//
// Settings.h  -  the "Shogo VR Settings" window (ShogoVR.exe --settings)
//
// Edits ShogoVR\ShogoVR.ini.  Every change is written straight away; the
// game and the headset bridge watch the file and apply it within a second,
// so you can tune things while playing (e.g. from SteamVR's desktop view).
//
// ======================================================================= //

#pragma once
#include "Common.h"
#include "Gui.h"
#include <commctrl.h>
#include <cmath>

enum SettingType { ST_COMBO, ST_SLIDER, ST_CHECK };

struct SettingDef
{
	SettingType		type;
	const wchar_t*	section;
	const wchar_t*	key;
	const wchar_t*	label;
	double			def;
	// sliders
	double			minV, maxV, step, shown;	// shown: multiply for display (e.g. 100 for %)
	const wchar_t*	unit;
	// combos
	std::vector<std::pair<const wchar_t*, double>> options;
	int				column;
};

// Turning is one setting in the file (VRSnapTurn: degrees, 0 = smooth) but
// two controls in the window.
#define TURN_MODE_KEY	L"#TurnMode"

static std::vector<SettingDef> SettingDefs()
{
	std::vector<SettingDef> d;
	auto combo = [&](const wchar_t* sec, const wchar_t* key, const wchar_t* label, double def,
					 std::vector<std::pair<const wchar_t*, double>> opts, int col)
	{ SettingDef s = { ST_COMBO, sec, key, label, def, 0, 0, 0, 1, L"", opts, col }; d.push_back(s); };
	auto slider = [&](const wchar_t* sec, const wchar_t* key, const wchar_t* label, double def,
					  double mn, double mx, double step, double shown, const wchar_t* unit, int col)
	{ SettingDef s = { ST_SLIDER, sec, key, label, def, mn, mx, step, shown, unit, {}, col }; d.push_back(s); };
	auto check = [&](const wchar_t* sec, const wchar_t* key, const wchar_t* label, double def, int col)
	{ SettingDef s = { ST_CHECK, sec, key, label, def, 0, 1, 1, 1, L"", {}, col }; d.push_back(s); };

	// Column 0: controls and comfort
	combo (L"Controls", L"LeftHanded",	L"Dominant hand",		0,	{ { L"Right-handed", 0 }, { L"Left-handed", 1 } }, 0);
	combo (L"Game",	L"VRAimMode",		L"Aim with",			1,	{ { L"Gun hand", 1 }, { L"Head", 0 } }, 0);
	check (L"Game",	L"VRTwoHand",		L"Two-handed aiming (support the gun with your other hand)", 1, 0);
	combo (L"Game",	L"VRMoveDir",		L"Walk towards",		0,	{ { L"Where the gun points", 0 }, { L"Where you look", 1 }, { L"Where the other hand points", 2 } }, 0);
	combo (L"Game",	TURN_MODE_KEY,		L"Turning",				0,	{ { L"Snap turn", 0 }, { L"Smooth turn", 1 } }, 0);
	combo (L"Game",	L"VRSnapTurn",		L"Snap angle",			45,	{ { L"15\u00B0", 15 }, { L"30\u00B0", 30 }, { L"45\u00B0", 45 }, { L"60\u00B0", 60 }, { L"90\u00B0", 90 } }, 0);
	slider(L"Game",	L"VRTurnSpeed",		L"Smooth turn speed",	150, 45, 360, 15, 1, L"\u00B0/s", 0);
	slider(L"Comfort", L"Vignette",		L"Comfort vignette",	0,	0, 1, 0.05, 100, L"%", 0);
	check (L"Game",	L"VRHeadPosition",	L"Lean and move with your head (6DoF)", 1, 0);
	slider(L"Game",	L"VRIPD",			L"Stereo depth (eye distance)", 64, 50, 80, 1, 1, L" mm", 0);
	check (L"Game",	L"VRAimMarker",		L"Show aim dot",		1,	0);
	check (L"Game",	L"VRBody",			L"Show your body",		1,	0);
	check (L"Game",	L"VRBodyArms",		L"Show the body's arms (they can't follow your hands)", 0, 0);
	check (L"Game",	L"VREasyLadders",	L"Easy ladders (the stick climbs up and down)", 1, 0);

	// Column 1: HUD, screens and picture
	slider(L"Game",	L"VRHudWidth",		L"HUD width",			60,	30, 110, 1, 1, L"\u00B0", 1);
	slider(L"Game",	L"VRHudDepth",		L"HUD distance",		3.5, 1, 10, 0.5, 1, L" m", 1);
	combo (L"Game",	L"VRHudStrip",		L"Sharp HUD panel",		-1,	{ { L"Automatic", -1 }, { L"Always", 1 }, { L"Never", 0 } }, 1);
	slider(L"Game",	L"VRMenuWidth",		L"Menu screen size",	80,	40, 140, 5, 1, L"\u00B0", 1);
	combo (L"Game",	L"VRGunModel",		L"Gun in your hand",	0,	{ { L"First-person model (detailed)", 0 }, { L"Third-person model (simple)", 1 } }, 1);
	check (L"Game",	L"VRGunArms",		L"Show Sanjuro's arms on the first-person gun", 1, 1);
	slider(L"Game",	L"VRGunScale",		L"Gun size (on foot)",	1.5, 0.5, 3, 0.1, 1, L"\u00D7", 1);
	slider(L"Game",	L"VRGunScaleMCA",	L"Gun size (in a mech)", 1, 0.5, 3, 0.1, 1, L"\u00D7", 1);
	slider(L"Picture", L"Sharpen",		L"Sharpening",			0.5, 0, 1, 0.05, 100, L"%", 1);
	check (L"Picture", L"Upscale",		L"Upscale to the headset's resolution", 1, 1);
	check (L"Window", L"KeepFocus",		L"Keep Shogo in focus while in VR", 1, 1);
	check (L"Launch", L"SkipMovies",	L"Skip the intro movies", 0, 1);
	combo (L"Launch", L"VramMB",		L"Video memory (dgVoodoo)", 2048, { { L"Leave as it is", 0 }, { L"1 GB", 1024 }, { L"2 GB", 2048 }, { L"3 GB", 3072 }, { L"4 GB", 4096 } }, 1);
	check (L"Picture", L"DirectCapture",	L"Take the picture straight from the game's renderer", 1, 1);
	check (L"Picture", L"Spectator",	L"Left-eye window on the desktop (for recording)", 1, 1);
	return d;
}

// ======================================================================= //

struct SettingsUI
{
	std::wstring				ini;
	std::vector<SettingDef>		defs;
	std::vector<HWND>			ctrl;		// one control per setting
	std::vector<HWND>			valueLbl;	// slider value labels
	std::vector<HWND>			all;		// every window the panel made (to show/hide)
	bool						loading = false;
};

enum { ID_BASE = 1000, ID_DEFAULTS = 2000, ID_PLAY, ID_CLOSE };

static double IniRead(const SettingsUI& ui, const SettingDef& s)
{
	if (wcscmp(s.key, TURN_MODE_KEY) == 0)
	{
		wchar_t buf[64];
		GetPrivateProfileStringW(L"Game", L"VRSnapTurn", L"45", buf, 64, ui.ini.c_str());
		return _wtof(buf) <= 0.0 ? 1.0 : 0.0;
	}
	wchar_t def[64], buf[64];
	swprintf(def, 64, L"%g", s.def);
	GetPrivateProfileStringW(s.section, s.key, def, buf, 64, ui.ini.c_str());
	return _wtof(buf);
}

static void IniWrite(const SettingsUI& ui, const wchar_t* section, const wchar_t* key, double v)
{
	wchar_t buf[64];
	swprintf(buf, 64, L"%g", v);
	WritePrivateProfileStringW(section, key, buf, ui.ini.c_str());
}

static int ComboIndexFor(const SettingDef& s, double v)
{
	int best = 0;
	double bestDiff = 1e9;
	for (size_t i = 0; i < s.options.size(); ++i)
	{
		double diff = std::fabs(s.options[i].second - v);
		if (diff < bestDiff) { bestDiff = diff; best = (int)i; }
	}
	return best;
}

static double SliderValue(const SettingDef& s, int pos) { return s.minV + pos * s.step; }

static void UpdateValueLabel(SettingsUI& ui, size_t i)
{
	const SettingDef& s = ui.defs[i];
	if (s.type != ST_SLIDER || !ui.valueLbl[i]) return;
	int pos = (int)SendMessageW(ui.ctrl[i], TBM_GETPOS, 0, 0);
	double v = SliderValue(s, pos) * s.shown;
	wchar_t buf[64];
	if (s.step * s.shown < 1.0 - 1e-9) swprintf(buf, 64, L"%.1f%ls", v, s.unit);
	else swprintf(buf, 64, L"%.0f%ls", v, s.unit);
	SetWindowTextW(ui.valueLbl[i], buf);
}

static int FindDef(const SettingsUI& ui, const wchar_t* key)
{
	for (size_t i = 0; i < ui.defs.size(); ++i) if (wcscmp(ui.defs[i].key, key) == 0) return (int)i;
	return -1;
}

// Snap angle and smooth speed only matter in their own turning mode.
static void UpdateEnabled(SettingsUI& ui)
{
	int iMode = FindDef(ui, TURN_MODE_KEY), iSnap = FindDef(ui, L"VRSnapTurn"), iSpeed = FindDef(ui, L"VRTurnSpeed");
	if (iMode < 0) return;
	bool smooth = SendMessageW(ui.ctrl[iMode], CB_GETCURSEL, 0, 0) == 1;
	if (iSnap >= 0) EnableWindow(ui.ctrl[iSnap], !smooth);
	if (iSpeed >= 0) { EnableWindow(ui.ctrl[iSpeed], smooth); EnableWindow(ui.valueLbl[iSpeed], smooth); }
}

static void LoadControls(SettingsUI& ui)
{
	ui.loading = true;
	for (size_t i = 0; i < ui.defs.size(); ++i)
	{
		const SettingDef& s = ui.defs[i];
		double v = IniRead(ui, s);
		if (s.type == ST_COMBO)
		{
			if (wcscmp(s.key, L"VRSnapTurn") == 0 && v <= 0.0) v = 45;	// smooth: keep a sensible snap angle shown
			SendMessageW(ui.ctrl[i], CB_SETCURSEL, ComboIndexFor(s, v), 0);
		}
		else if (s.type == ST_SLIDER)
		{
			int pos = (int)std::lround((std::min(s.maxV, std::max(s.minV, v)) - s.minV) / s.step);
			SendMessageW(ui.ctrl[i], TBM_SETPOS, TRUE, pos);
			UpdateValueLabel(ui, i);
		}
		else
		{
			SendMessageW(ui.ctrl[i], BM_SETCHECK, v != 0.0 ? BST_CHECKED : BST_UNCHECKED, 0);
		}
	}
	UpdateEnabled(ui);
	ui.loading = false;
}

// Write one control's value to the file (straight away - the game applies it live).
static void SaveControl(SettingsUI& ui, size_t i)
{
	if (ui.loading) return;
	const SettingDef& s = ui.defs[i];

	if (wcscmp(s.key, TURN_MODE_KEY) == 0 || wcscmp(s.key, L"VRSnapTurn") == 0)
	{
		int iMode = FindDef(ui, TURN_MODE_KEY), iSnap = FindDef(ui, L"VRSnapTurn");
		bool smooth = SendMessageW(ui.ctrl[iMode], CB_GETCURSEL, 0, 0) == 1;
		int snapSel = (int)SendMessageW(ui.ctrl[iSnap], CB_GETCURSEL, 0, 0);
		IniWrite(ui, L"Game", L"VRSnapTurn", smooth ? 0.0 : ui.defs[iSnap].options[snapSel].second);
		UpdateEnabled(ui);
		return;
	}

	double v = 0;
	if (s.type == ST_COMBO)		v = s.options[(size_t)SendMessageW(ui.ctrl[i], CB_GETCURSEL, 0, 0)].second;
	else if (s.type == ST_SLIDER)	v = SliderValue(s, (int)SendMessageW(ui.ctrl[i], TBM_GETPOS, 0, 0));
	else							v = SendMessageW(ui.ctrl[i], BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
	IniWrite(ui, s.section, s.key, v);
}

static void ResetDefaults(SettingsUI& ui)
{
	for (const SettingDef& s : ui.defs)
		if (wcscmp(s.key, TURN_MODE_KEY) != 0) IniWrite(ui, s.section, s.key, s.def);
	LoadControls(ui);
}

// Builds the settings controls inside `parent`, two columns, starting at
// (left, top), `width` wide.  Returns the height used.
static int CreateSettingsPanel(SettingsUI& ui, HWND parent, HFONT font, HFONT headFont, int left, int top, int width,
							   const std::wstring& modDir)
{
	using gui::S;
	ui.ini = JoinPath(modDir, L"ShogoVR.ini");
	ui.defs = SettingDefs();
	ui.ctrl.assign(ui.defs.size(), nullptr);
	ui.valueLbl.assign(ui.defs.size(), nullptr);
	ui.all.clear();

	HINSTANCE inst = GetModuleHandleW(nullptr);
	auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD st, int x, int y, int w, int h, int id, HFONT f) -> HWND
	{
		HWND c = CreateWindowExW(0, cls, text, WS_CHILD | st, x, y, w, h, parent, (HMENU)(INT_PTR)id, inst, nullptr);
		SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
		ui.all.push_back(c);
		return c;
	};

	const int colGap = S(28), rowH = S(27);
	const int colW = (width - colGap) / 2;
	const int valW = S(58), labelW = (colW - valW) * 45 / 100, ctrlW = colW - valW - labelW;
	make(L"STATIC", L"CONTROLS && COMFORT", SS_LEFT, left, top, colW, S(20), 0, headFont);
	make(L"STATIC", L"HUD, SCREENS && PICTURE", SS_LEFT, left + colW + colGap, top, colW, S(20), 0, headFont);
	int rowsTop = top + S(28);

	int rowIdx[2] = { 0, 0 };
	for (size_t i = 0; i < ui.defs.size(); ++i)
	{
		const SettingDef& s = ui.defs[i];
		int x = left + s.column * (colW + colGap);
		int y = rowsTop + rowIdx[s.column]++ * rowH;
		int id = ID_BASE + (int)i;

		if (s.type == ST_CHECK)
		{
			ui.ctrl[i] = make(L"BUTTON", s.label, BS_AUTOCHECKBOX | WS_TABSTOP, x, y + S(3), colW, S(22), id, font);
			continue;
		}
		make(L"STATIC", s.label, SS_LEFT | SS_CENTERIMAGE, x, y, labelW, S(25), 0, font);
		if (s.type == ST_COMBO)
		{
			ui.ctrl[i] = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x + labelW, y + S(1), ctrlW + valW, S(200), id, font);
			for (auto& o : s.options) SendMessageW(ui.ctrl[i], CB_ADDSTRING, 0, (LPARAM)o.first);
		}
		else
		{
			ui.ctrl[i] = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, x + labelW, y + S(1), ctrlW, S(24), id, font);
			SendMessageW(ui.ctrl[i], TBM_SETRANGE, TRUE, MAKELPARAM(0, (int)std::lround((s.maxV - s.minV) / s.step)));
			ui.valueLbl[i] = make(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, x + labelW + ctrlW + S(6), y, valW, S(25), 0, font);
		}
	}
	LoadControls(ui);
	return S(28) + std::max(rowIdx[0], rowIdx[1]) * rowH;
}

static void ShowSettingsPanel(SettingsUI& ui, bool show)
{
	for (HWND h : ui.all) ShowWindow(h, show ? SW_SHOW : SW_HIDE);
}

// Forward the parent's WM_COMMAND / WM_HSCROLL here.  True if it was ours.
static bool SettingsHandleCommand(SettingsUI& ui, WPARAM wp)
{
	int id = LOWORD(wp), code = HIWORD(wp);
	if (id < ID_BASE || id >= ID_BASE + (int)ui.defs.size()) return false;
	size_t i = (size_t)(id - ID_BASE);
	if ((ui.defs[i].type == ST_COMBO && code == CBN_SELCHANGE) || (ui.defs[i].type == ST_CHECK && code == BN_CLICKED))
		SaveControl(ui, i);
	return true;
}

static bool SettingsHandleScroll(SettingsUI& ui, LPARAM lp)
{
	for (size_t i = 0; i < ui.ctrl.size(); ++i)
	{
		if ((HWND)lp == ui.ctrl[i])
		{
			UpdateValueLabel(ui, i);
			SaveControl(ui, i);
			return true;
		}
	}
	return false;
}
