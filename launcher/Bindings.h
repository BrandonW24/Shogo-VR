// ======================================================================= //
//
// Bindings.h  -  the launcher's "Controller Buttons" tab
//
// What each controller button does, per controller type.  Saved in
// ShogoVR.ini ([Bindings.touch] GunTrigger=fire ...) and read by the headset
// bridge, which applies changes straight away - even mid-game.  Bindings are
// relative to the gun hand, so left-handed mode (VR Settings) mirrors them.
// The tables must match the bridge's (k_defaultBindings in ShogoVRBridge.cpp).
//
// ======================================================================= //

#pragma once
#include "Common.h"
#include "Gui.h"

enum { BIND_CTRLS = 4, BIND_INPUTS = 6, BIND_FNS = 10 };
static const wchar_t* kBindCtrlKeys[BIND_CTRLS]  = { L"touch", L"index", L"vive", L"wmr" };
static const wchar_t* kBindCtrlNames[BIND_CTRLS] = { L"Meta Quest / Rift (Touch)", L"Valve Index", L"HTC Vive", L"Windows Mixed Reality" };
static const wchar_t* kBindInputKeys[BIND_INPUTS] = { L"Trigger", L"Grip", L"Primary", L"Secondary", L"StickClick", L"Menu" };
static const wchar_t* kBindFnKeys[BIND_FNS] = { L"none", L"fire", L"jump", L"crouch", L"transform", L"next_weapon",
												L"weapon_list", L"mission_log", L"menu", L"recenter" };
static const wchar_t* kBindFnNames[BIND_FNS] = { L"(nothing)", L"Fire", L"Jump", L"Crouch  (in a mech: tap to transform)",
												 L"Transform (mech)", L"Next weapon", L"Weapon list", L"Mission log", L"Menu",
												 L"Recentre the view" };
// [controller][0 = gun hand, 1 = other hand][input]
static const int kBindDefaults[BIND_CTRLS][2][BIND_INPUTS] = {
	{ { 1, 5, 2, 3, 0, 8 }, { 0, 6, 7, 6, 9, 8 } },		// Touch
	{ { 1, 5, 2, 3, 0, 0 }, { 0, 6, 7, 8, 9, 0 } },		// Index
	{ { 1, 2, 0, 0, 0, 5 }, { 0, 3, 0, 0, 9, 8 } },		// Vive
	{ { 1, 3, 2, 0, 0, 5 }, { 0, 6, 7, 0, 9, 8 } },		// Windows MR
};

// The button's name on that controller and hand (nullptr: it has none).
static const wchar_t* BindButtonName(int ctrl, int input, bool leftHand)
{
	static const wchar_t* touchL[BIND_INPUTS] = { L"Trigger", L"Grip", L"X button", L"Y button", L"Thumbstick click", L"Menu button" };
	static const wchar_t* touchR[BIND_INPUTS] = { L"Trigger", L"Grip", L"A button", L"B button", L"Thumbstick click", nullptr };
	static const wchar_t* index[BIND_INPUTS]  = { L"Trigger", L"Grip", L"A button", L"B button", L"Thumbstick click", nullptr };
	static const wchar_t* vive[BIND_INPUTS]   = { L"Trigger", L"Grip", nullptr, nullptr, L"Trackpad click", L"Menu button" };
	static const wchar_t* wmr[BIND_INPUTS]    = { L"Trigger", L"Grip", L"Trackpad click", nullptr, L"Thumbstick click", L"Menu button" };
	switch (ctrl)
	{
	case 0:  return leftHand ? touchL[input] : touchR[input];
	case 1:  return index[input];
	case 2:  return vive[input];
	default: return wmr[input];
	}
}

struct BindingsUI
{
	std::wstring			ini;
	HWND					parent = nullptr;
	HFONT					font = nullptr, head = nullptr;
	int						left = 0, top = 0, width = 0;
	int						ctrl = 0;
	bool					visible = false;
	HWND					ctrlLabel = nullptr, ctrlCombo = nullptr, info = nullptr;
	std::vector<HWND>		rows;			// labels, combos and column headings (rebuilt per controller)
	std::vector<int>		rowSlot;		// per combo: hand * BIND_INPUTS + input
	std::vector<HWND>		rowCombo;
};

enum { IDB_CTRL = 5000, IDB_ROW = 5100 };

static std::wstring BindSection(int ctrl) { return std::wstring(L"Bindings.") + kBindCtrlKeys[ctrl]; }
static std::wstring BindKey(int hand, int input) { return std::wstring(hand == 0 ? L"Gun" : L"Off") + kBindInputKeys[input]; }

static int BindRead(const BindingsUI& b, int hand, int input)
{
	wchar_t val[32] = L"";
	GetPrivateProfileStringW(BindSection(b.ctrl).c_str(), BindKey(hand, input).c_str(), L"", val, 32, b.ini.c_str());
	for (int f = 0; f < BIND_FNS; ++f) if (val[0] && _wcsicmp(val, kBindFnKeys[f]) == 0) return f;
	return kBindDefaults[b.ctrl][hand][input];
}

// (Re)creates the two columns of buttons for the selected controller.
static void BuildBindingRows(BindingsUI& b)
{
	using gui::S;
	for (HWND h : b.rows) DestroyWindow(h);
	b.rows.clear();
	b.rowSlot.clear();
	b.rowCombo.clear();

	bool leftHanded = GetPrivateProfileIntW(L"Controls", L"LeftHanded", 0, b.ini.c_str()) != 0;
	HINSTANCE inst = GetModuleHandleW(nullptr);
	auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD st, int x, int y, int w, int h, int id, HFONT f) -> HWND
	{
		HWND c = CreateWindowExW(0, cls, text, WS_CHILD | (b.visible ? WS_VISIBLE : 0) | st, x, y, w, h, b.parent,
								 (HMENU)(INT_PTR)id, inst, nullptr);
		SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
		b.rows.push_back(c);
		return c;
	};

	const int colGap = S(28), rowH = S(34);
	const int colW = (b.width - colGap) / 2, labelW = colW * 36 / 100, comboW = colW - labelW;
	int rowsTop = b.top + S(96);
	for (int hand = 0; hand < 2; ++hand)
	{
		bool leftHand = (hand == 0) == leftHanded;		// the gun hand is the left one when left-handed
		int x = b.left + hand * (colW + colGap);
		std::wstring heading = std::wstring(hand == 0 ? L"GUN HAND" : L"OTHER HAND") + (leftHand ? L"  (left)" : L"  (right)");
		make(L"STATIC", heading.c_str(), SS_LEFT | SS_NOPREFIX, x, rowsTop, colW, S(20), 0, b.head);
		int y = rowsTop + S(28);
		for (int input = 0; input < BIND_INPUTS; ++input)
		{
			const wchar_t* name = BindButtonName(b.ctrl, input, leftHand);
			if (!name) continue;
			// The label right before its combo box: screen readers announce it as the combo's name.
			std::wstring label = std::wstring(name) + L":";
			make(L"STATIC", label.c_str(), SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, x, y, labelW, S(26), 0, b.font);
			HWND combo = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x + labelW, y + S(1), comboW, S(260),
							  IDB_ROW + (int)b.rowCombo.size(), b.font);
			for (int f = 0; f < BIND_FNS; ++f) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)kBindFnNames[f]);
			SendMessageW(combo, CB_SETCURSEL, BindRead(b, hand, input), 0);
			b.rowCombo.push_back(combo);
			b.rowSlot.push_back(hand * BIND_INPUTS + input);
			y += rowH;
		}
	}
}

static int CreateBindingsPanel(BindingsUI& b, HWND parent, HFONT font, HFONT head, int left, int top, int width, const std::wstring& modDir)
{
	using gui::S;
	b.ini = JoinPath(modDir, L"ShogoVR.ini");
	b.parent = parent;
	b.font = font;
	b.head = head;
	b.left = left;
	b.top = top;
	b.width = width;

	// Start on the controllers the headset bridge last saw.
	wchar_t seen[32] = L"";
	GetPrivateProfileStringW(L"Controls", L"Controller", L"touch", seen, 32, b.ini.c_str());
	for (int c = 0; c < BIND_CTRLS; ++c) if (_wcsicmp(seen, kBindCtrlKeys[c]) == 0) b.ctrl = c;

	HINSTANCE inst = GetModuleHandleW(nullptr);
	b.ctrlLabel = CreateWindowExW(0, L"STATIC", L"Controllers:", WS_CHILD | SS_LEFT | SS_CENTERIMAGE, left, top, S(110), S(26),
								  parent, nullptr, inst, nullptr);
	b.ctrlCombo = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, left + S(110), top + S(1),
								  S(280), S(200), parent, (HMENU)(INT_PTR)IDB_CTRL, inst, nullptr);
	b.info = CreateWindowExW(0, L"STATIC",
		L"Choose what each button does. Changes apply straight away, even while you're playing. The sticks are fixed: "
		L"the other hand's stick moves you; the gun hand's stick turns you (left/right) and changes weapon (up/down). "
		L"Left-handed mode (VR Settings) swaps the hands.",
		WS_CHILD | SS_LEFT | SS_NOPREFIX, left, top + S(36), width, S(48), parent, nullptr, inst, nullptr);
	for (HWND h : { b.ctrlLabel, b.ctrlCombo, b.info }) SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
	for (int c = 0; c < BIND_CTRLS; ++c) SendMessageW(b.ctrlCombo, CB_ADDSTRING, 0, (LPARAM)kBindCtrlNames[c]);
	SendMessageW(b.ctrlCombo, CB_SETCURSEL, b.ctrl, 0);
	BuildBindingRows(b);
	return S(96) + S(28) + 6 * S(34);
}

static void ShowBindingsPanel(BindingsUI& b, bool show)
{
	if (show && !b.visible) { b.visible = true; BuildBindingRows(b); }		// picks up a left-handed change
	b.visible = show;
	for (HWND h : { b.ctrlLabel, b.ctrlCombo, b.info }) ShowWindow(h, show ? SW_SHOW : SW_HIDE);
	for (HWND h : b.rows) ShowWindow(h, show ? SW_SHOW : SW_HIDE);
}

static void ResetBindings(BindingsUI& b)
{
	WritePrivateProfileStringW(BindSection(b.ctrl).c_str(), nullptr, nullptr, b.ini.c_str());	// back to the defaults
	BuildBindingRows(b);
}

// Forward the parent's WM_COMMAND here.  True if it was ours.
static bool BindingsHandleCommand(BindingsUI& b, WPARAM wp)
{
	int id = LOWORD(wp), code = HIWORD(wp);
	if (id == IDB_CTRL)
	{
		if (code == CBN_SELCHANGE)
		{
			b.ctrl = (int)SendMessageW(b.ctrlCombo, CB_GETCURSEL, 0, 0);
			BuildBindingRows(b);
		}
		return true;
	}
	if (id >= IDB_ROW && id < IDB_ROW + (int)b.rowCombo.size())
	{
		if (code == CBN_SELCHANGE)
		{
			size_t r = (size_t)(id - IDB_ROW);
			int fn = (int)SendMessageW(b.rowCombo[r], CB_GETCURSEL, 0, 0);
			int hand = b.rowSlot[r] / BIND_INPUTS, input = b.rowSlot[r] % BIND_INPUTS;
			WritePrivateProfileStringW(BindSection(b.ctrl).c_str(), BindKey(hand, input).c_str(), kBindFnKeys[fn], b.ini.c_str());
		}
		return true;
	}
	return false;
}
