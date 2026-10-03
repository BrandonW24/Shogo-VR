// ======================================================================= //
//
// ShogoVRSetup.cpp  ->  ShogoVR-InstallerBuilder.exe / ShogoVR-Setup.exe
//
// One program, two jobs, decided by what's packed inside it:
//
//   Builder (no CShell.dll inside yet):  takes the CShell.dll you built in
//     Visual Studio and writes ShogoVR-Setup.exe - itself plus that DLL.
//     Command line: --build <CShell.dll> [<output.exe>]
//
//   Installer (CShell.dll inside):  finds Shogo (Steam, GOG or a folder you
//     pick), copies the mod into <Shogo>\ShogoVR, adds Desktop and Start
//     menu shortcuts and an entry in Windows' "Installed apps", and offers
//     to start it.
//     Command line: --install <Shogo folder> [--quiet]
//
// ======================================================================= //

#include "Common.h"
#include "Gui.h"
#include <shellapi.h>
#include <commdlg.h>

static bool g_quiet = false;

static void Say(const std::wstring& text, UINT icon = MB_ICONINFORMATION)
{
	if (g_quiet) { wprintf(L"%ls\n", text.c_str()); return; }
	MessageBoxW(nullptr, text.c_str(), SHOGOVR_PRODUCT L" Setup", MB_OK | icon);
}

static bool Ask(const std::wstring& text, UINT icon = MB_ICONQUESTION)
{
	if (g_quiet) return true;
	return MessageBoxW(nullptr, text.c_str(), SHOGOVR_PRODUCT L" Setup", MB_YESNO | icon) == IDYES;
}

// ======================================================================= //
//  Builder
// ======================================================================= //

// A real Shogo client DLL: 32-bit Windows DLL exporting GetClientShellFunctions.
static bool LooksLikeShogoClientDll(const std::vector<char>& d, std::wstring& why)
{
	if (d.size() < 0x200 || d[0] != 'M' || d[1] != 'Z') { why = L"it isn't a Windows program file."; return false; }
	uint32_t peOff = 0;
	memcpy(&peOff, d.data() + 0x3C, 4);
	if (peOff + 24 > d.size() || memcmp(d.data() + peOff, "PE\0\0", 4) != 0) { why = L"it isn't a Windows program file."; return false; }
	uint16_t machine = 0, characteristics = 0;
	memcpy(&machine, d.data() + peOff + 4, 2);
	memcpy(&characteristics, d.data() + peOff + 22, 2);
	if (machine != 0x14C) { why = L"it isn't a 32-bit (Win32/x86) build - build the Release | Win32 configuration."; return false; }
	if (!(characteristics & 0x2000)) { why = L"it isn't a DLL."; return false; }
	static const char k_export[] = "GetClientShellFunctions";
	if (std::search(d.begin(), d.end(), k_export, k_export + sizeof(k_export) - 1) == d.end())
	{
		why = L"it doesn't look like a Shogo CShell.dll.";
		return false;
	}
	return true;
}

static std::wstring PickDll()
{
	wchar_t file[4096] = L"CShell.dll";
	OPENFILENAMEW ofn = { sizeof(ofn) };
	ofn.lpstrFilter = L"Shogo client DLL (CShell.dll)\0CShell.dll\0DLL files (*.dll)\0*.dll\0";
	ofn.lpstrFile = file;
	ofn.nMaxFile = 4096;
	ofn.lpstrTitle = L"Choose the CShell.dll you built (ClientShellDLL\\Release\\CShell.dll)";
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
	return GetOpenFileNameW(&ofn) ? std::wstring(file) : std::wstring();
}

static int Build(const std::vector<char>& self, uint64_t stubSize, std::vector<PakFile> files,
				 std::wstring dllPath, std::wstring outPath)
{
	if (dllPath.empty())
	{
		std::wstring here = JoinPath(DirOf(ExePath()), L"CShell.dll");
		if (FileExists(here) && Ask(L"Build the Shogo VR installer using this CShell.dll?\n\n" + here)) dllPath = here;
		else
		{
			Say(L"This builds the one-click ShogoVR-Setup.exe.\n\n"
				L"Next, choose the CShell.dll you built in Visual Studio "
				L"(ClientShellDLL\\Release\\CShell.dll).");
			dllPath = PickDll();
			if (dllPath.empty()) return 1;
		}
	}

	std::vector<char> dll;
	std::wstring why;
	if (!ReadWholeFile(dllPath, dll)) { Say(L"Couldn't read " + dllPath, MB_ICONERROR); return 1; }
	if (!LooksLikeShogoClientDll(dll, why)) { Say(L"That file can't be used: " + why + L"\n\n" + dllPath, MB_ICONERROR); return 1; }

	// The Shogo source licence requires the creators' names and email
	// addresses with every release: they come from AUTHORS.txt.
	std::vector<char> authors;
	std::wstring authorsPath = JoinPath(DirOf(dllPath), L"AUTHORS.txt");
	if (!FileExists(authorsPath)) authorsPath = JoinPath(DirOf(ExePath()), L"AUTHORS.txt");
	if (!ReadWholeFile(authorsPath, authors) || authors.size() < 3)
	{
		Say(L"Please create AUTHORS.txt next to CShell.dll (or next to this program) with the name and email address "
			L"of each creator, one per line - for example:\n\n    Jane Doe <jane@example.com>\n\n"
			L"Shogo's source licence requires them in every release; the installer, launcher and the game's "
			L"opening screen show them.", MB_ICONWARNING);
		return 1;
	}
	files.erase(std::remove_if(files.begin(), files.end(),
		[](const PakFile& x) { return _stricmp(x.name.c_str(), "ShogoVR/AUTHORS.txt") == 0; }), files.end());
	PakFile fa;
	fa.name = "ShogoVR/AUTHORS.txt";
	fa.data.swap(authors);
	files.push_back(std::move(fa));

	PakFile f;
	f.name = "ShogoVR/CShell.dll";
	f.data.swap(dll);
	files.push_back(std::move(f));

	if (outPath.empty()) outPath = JoinPath(DirOf(ExePath()), L"ShogoVR-Setup.exe");
	if (!PakWrite(outPath, self, stubSize, files)) { Say(L"Couldn't write " + outPath, MB_ICONERROR); return 1; }

	Say(L"Created the installer:\n" + outPath + L"\n\nThat single file is all anyone needs to install Shogo VR.");
	return 0;
}

// ======================================================================= //
//  Installer
// ======================================================================= //

static std::wstring PickFolder()
{
	std::wstring out;
	IFileOpenDialog* dlg = nullptr;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void**)&dlg))) return out;
	DWORD opts = 0;
	dlg->GetOptions(&opts);
	dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
	dlg->SetTitle(L"Choose your Shogo folder (the one with Client.exe and SHOGO.REZ)");
	if (SUCCEEDED(dlg->Show(nullptr)))
	{
		IShellItem* item = nullptr;
		if (SUCCEEDED(dlg->GetResult(&item)))
		{
			PWSTR p = nullptr;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out = p; CoTaskMemFree(p); }
			item->Release();
		}
	}
	dlg->Release();
	return out;
}

static std::wstring FindOrAskShogo(const std::wstring& given)
{
	if (!given.empty())
	{
		std::wstring d = ResolveShogoDir(given);
		if (d.empty()) Say(L"No Shogo installation found in:\n" + given, MB_ICONERROR);
		return d;
	}

	std::wstring found = LocateShogo();
	if (!found.empty() && Ask(L"Found Shogo here:\n\n" + found + L"\n\nInstall Shogo VR there?\n(Choose No to pick a different folder.)"))
		return found;

	for (;;)
	{
		std::wstring picked = PickFolder();
		if (picked.empty()) return L"";
		std::wstring d = ResolveShogoDir(picked);
		if (!d.empty()) return d;

		bool exe = FileExists(JoinPath(picked, L"Client.exe")), rez = FileExists(JoinPath(picked, L"SHOGO.REZ"));
		std::wstring why = (exe && !rez) ? L"That folder has Client.exe but no SHOGO.REZ - the install looks incomplete."
						 : (!exe && rez) ? L"That folder has SHOGO.REZ but no Client.exe - the install looks incomplete."
						 : L"That isn't a Shogo folder. Pick the folder that holds Client.exe and SHOGO.REZ "
						   L"(usually called \"Shogo Mobile Armor Division\").";
		if (!Ask(why + L"\n\nTry another folder?", MB_ICONWARNING)) return L"";
	}
}

static bool SystemScalingAbove100()
{
	HDC dc = GetDC(nullptr);
	int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
	if (dc) ReleaseDC(nullptr, dc);
	return dpi > 96;
}

// Same as Properties > Compatibility > Change high DPI settings > "Application".
static void ApplyDpiOverride(const std::wstring& clientExe)
{
	HKEY key = nullptr;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers",
						0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
	wchar_t cur[1024] = L"";
	DWORD size = sizeof(cur) - sizeof(wchar_t), type = 0;
	RegQueryValueExW(key, clientExe.c_str(), nullptr, &type, (BYTE*)cur, &size);
	std::wstring flags = (type == REG_SZ) ? std::wstring(cur) : std::wstring();
	if (flags.find(L"HIGHDPIAWARE") == std::wstring::npos)
	{
		flags = flags.empty() ? std::wstring(L"~ HIGHDPIAWARE") : flags + L" HIGHDPIAWARE";
		RegSetValueExW(key, clientExe.c_str(), 0, REG_SZ, (const BYTE*)flags.c_str(), (DWORD)((flags.size() + 1) * sizeof(wchar_t)));
	}
	RegCloseKey(key);
}

static void WriteUninstallEntry(const std::wstring& gameDir, const std::wstring& modDir, size_t totalBytes)
{
	HKEY key = nullptr;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, SHOGOVR_UNINSTALL_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
	auto setStr = [&](const wchar_t* name, const std::wstring& v)
	{ RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t))); };
	auto setDword = [&](const wchar_t* name, DWORD v) { RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&v, 4); };

	std::wstring launcher = JoinPath(modDir, L"ShogoVR.exe");
	setStr(L"DisplayName", L"Shogo VR (mod for Shogo: Mobile Armor Division)");
	setStr(L"Publisher", L"Shogo VR mod");
	setStr(L"DisplayIcon", JoinPath(modDir, L"ShogoVR.exe"));
	setStr(L"InstallLocation", gameDir);
	setStr(L"UninstallString", L"\"" + launcher + L"\" --uninstall");
	setStr(L"QuietUninstallString", L"\"" + launcher + L"\" --uninstall --quiet");
	setDword(L"NoModify", 1);
	setDword(L"NoRepair", 1);
	setDword(L"EstimatedSize", (DWORD)(totalBytes / 1024));
	RegCloseKey(key);
}

struct InstallOptions
{
	bool desktop = true;
	bool startMenu = true;
	bool dpiFix = false;
	bool vram = true;
};

// Copies the mod into <gameDir>\ShogoVR, adds shortcuts and the Installed
// apps entry.  On success `summary` says what was done.
static bool DoInstall(const std::vector<PakFile>& files, const std::wstring& gameDir, const InstallOptions& o,
					  std::wstring& summary, std::wstring& error)
{
	std::wstring modDir = JoinPath(gameDir, SHOGOVR_MOD_FOLDER);
	if (IsBridgeRunning()) { error = L"Shogo VR is running. Please close it (and Shogo) and try again."; return false; }

	CreateDirectoryW(modDir.c_str(), nullptr);
	if (!DirExists(modDir)) { error = L"Couldn't create the folder:\n" + modDir; return false; }

	size_t total = 0;
	for (const PakFile& f : files)
	{
		std::wstring rel = Utf8ToWide(f.name);
		std::replace(rel.begin(), rel.end(), L'/', L'\\');
		std::wstring dest = JoinPath(gameDir, rel);
		CreateDirectoryW(DirOf(dest).c_str(), nullptr);
		if (!WriteWholeFile(dest, f.data.data(), f.data.size()))
		{
			error = L"Couldn't write:\n" + dest + L"\n\nIf Shogo is in Program Files, run the installer as administrator.";
			return false;
		}
		total += f.data.size();
	}

	std::wstring launcher = JoinPath(modDir, L"ShogoVR.exe");
	std::wstring icon = launcher;		// carries the Shogo icon
	if (o.desktop) CreateShortcut(ShortcutPath(FOLDERID_Desktop), launcher, modDir, icon, L"Shogo VR - unofficial VR mod for Shogo");
	else DeleteFileW(ShortcutPath(FOLDERID_Desktop).c_str());
	if (o.startMenu)
	{
		CreateShortcut(ShortcutPath(FOLDERID_Programs), launcher, modDir, icon, L"Shogo VR - unofficial VR mod for Shogo");
		CreateShortcut(ShortcutPath(FOLDERID_Programs, L"Shogo VR Settings.lnk"), launcher, modDir, icon,
					   L"Shogo VR settings: hands, turning, comfort, HUD, picture", L"--settings");
	}
	WriteUninstallEntry(gameDir, modDir, total);

	std::wstring vramFile;
	int nVram = o.vram ? FixDgVoodooVram(gameDir, 2048, true, &vramFile) : 0;
	if (o.dpiFix) ApplyDpiOverride(JoinPath(gameDir, L"Client.exe"));

	summary = L"Shogo VR is installed in:\n" + modDir + L"\n\n";
	summary += L"Start it with the \"Shogo VR\" shortcut" + std::wstring(o.desktop ? L" on your Desktop or" : L" in") + L" the Start menu. ";
	summary += L"It starts the game through Shogo's own launcher, so do this once: open Shogo.exe, click Advanced..., put -rez ShogoVR "
			   L"in the Command-Line box, tick \"Always specify these command-line parameters\" and click OK.";
	if (nVram > 0) summary += L"\n\nRaised dgVoodoo's video memory to 2 GB (the original settings are saved as " + vramFile + L".shogovr-backup).";
	std::wstring runtime = RegReadString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", KEY_WOW64_64KEY);
	if (runtime.empty())
		summary += L"\n\nOne more step: no OpenXR runtime is set yet. In SteamVR, open Settings > OpenXR and choose \"Set SteamVR as OpenXR Runtime\".";
	return true;
}

static int Install(const std::vector<PakFile>& files, const std::wstring& givenDir)
{
	std::wstring gameDir = FindOrAskShogo(givenDir);
	if (gameDir.empty()) return 1;
	std::wstring summary, error;
	InstallOptions o;
	if (!DoInstall(files, gameDir, o, summary, error)) { Say(error, MB_ICONERROR); return 1; }
	Say(summary);
	return 0;
}

// ======================================================================= //
//  The installer window
// ======================================================================= //

enum { IDW_BACK = 4000, IDW_NEXT, IDW_CANCEL, IDW_AGREE, IDW_PATH, IDW_BROWSE, IDW_DESKTOP, IDW_STARTMENU,
	   IDW_DPI, IDW_VRAM, IDW_LAUNCH, IDW_LEGAL };
enum { WP_WELCOME = 0, WP_LEGAL, WP_LOCATION, WP_DONE };

struct WizardUI
{
	const std::vector<PakFile>*	files = nullptr;
	std::wstring	authors, summary, gameDir;
	int				page = WP_WELCOME;
	bool			startNow = false;
	gui::Image		header, banner;
	HFONT			font = nullptr, bold = nullptr, title = nullptr, small = nullptr;
	HWND			hwnd = nullptr, back = nullptr, next = nullptr, cancel = nullptr;
	HWND			legal = nullptr, agree = nullptr, path = nullptr, browse = nullptr;
	HWND			desktop = nullptr, startMenu = nullptr, dpi = nullptr, vram = nullptr, launch = nullptr;
	int				W = 0, H = 0, headerH = 0, footH = 0;
};
static WizardUI* g_W = nullptr;

static bool HasDgVoodooConf(const std::wstring& gameDir)
{
	wchar_t appData[MAX_PATH] = L"";
	GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
	return FileExists(JoinPath(gameDir, L"dgVoodoo.conf")) || (appData[0] && FileExists(JoinPath(appData, L"dgVoodoo\\dgVoodoo.conf")));
}

static void WizardShowPage(WizardUI& w, int page)
{
	w.page = page;
	ShowWindow(w.legal, page == WP_LEGAL ? SW_SHOW : SW_HIDE);
	ShowWindow(w.agree, page == WP_LEGAL ? SW_SHOW : SW_HIDE);
	for (HWND h : { w.path, w.browse, w.desktop, w.startMenu, w.vram }) ShowWindow(h, page == WP_LOCATION ? SW_SHOW : SW_HIDE);
	ShowWindow(w.dpi, page == WP_LOCATION && SystemScalingAbove100() ? SW_SHOW : SW_HIDE);
	ShowWindow(w.launch, page == WP_DONE ? SW_SHOW : SW_HIDE);
	ShowWindow(w.back, (page == WP_LEGAL || page == WP_LOCATION) ? SW_SHOW : SW_HIDE);
	ShowWindow(w.cancel, page == WP_DONE ? SW_HIDE : SW_SHOW);

	const wchar_t* nextText = page == WP_LOCATION ? L"Install" : (page == WP_DONE ? L"Finish" : L"Next");
	SetWindowTextW(w.next, nextText);
	bool canNext = true;
	if (page == WP_LEGAL) canNext = SendMessageW(w.agree, BM_GETCHECK, 0, 0) == BST_CHECKED;
	if (page == WP_LOCATION)
	{
		wchar_t buf[MAX_PATH * 2] = L"";
		GetWindowTextW(w.path, buf, MAX_PATH * 2);
		canNext = !ResolveShogoDir(buf).empty();
		ShowWindow(w.vram, HasDgVoodooConf(buf) ? SW_SHOW : SW_HIDE);
	}
	EnableWindow(w.next, canNext);
	InvalidateRect(w.hwnd, nullptr, TRUE);
}

static void PaintWizard(HWND hwnd, HDC dc)
{
	WizardUI& w = *g_W;
	using namespace gui;
	RECT rc;
	GetClientRect(hwnd, &rc);
	RECT band = { 0, 0, rc.right, w.headerH };
	FillRectColor(dc, band, kHeaderBg);
	DrawImage(dc, w.header, (rc.right - w.header.w) / 2, (w.headerH - w.header.h) / 2);
	RECT stripe = { 0, w.headerH - S(3), rc.right, w.headerH };
	FillRectColor(dc, stripe, kRed);
	RECT body = { 0, w.headerH, rc.right, rc.bottom };
	FillRectColor(dc, body, kBodyBg);
	SetBkMode(dc, TRANSPARENT);

	auto text = [&](HFONT f, COLORREF c, const std::wstring& t, RECT r, UINT flags)
	{
		SelectObject(dc, f);
		SetTextColor(dc, c);
		DrawTextW(dc, t.c_str(), -1, &r, flags | DT_NOPREFIX);
	};
	int x = S(28), y = w.headerH + S(16), right = rc.right - S(28);
	static const wchar_t* steps[] = { L"Welcome", L"About & legal", L"Install location", L"Done" };
	std::wstring crumb;
	for (int i = 0; i < 4; ++i) crumb += (i ? L"   \u203A   " : L"") + std::wstring(steps[i]);
	text(w.small, kMuted, L"Step " + std::to_wstring(w.page + 1) + L" of 4:  " + std::wstring(steps[w.page]), { x, y, right, y + S(18) }, DT_LEFT | DT_SINGLELINE);
	y += S(22);

	if (w.page == WP_WELCOME)
	{
		text(w.title, kText, L"Welcome to Shogo VR", { x, y, right, y + S(30) }, DT_LEFT | DT_SINGLELINE);
		y += S(34);
		DrawImage(dc, w.banner, (rc.right - w.banner.w) / 2, y);
		y += w.banner.h + S(10);
		text(w.font, kText, L"Play Shogo: Mobile Armor Division in a PC VR headset - stereo 3D, head and motion-controller "
			 L"tracking, a sharp floating HUD and comfort settings. This installs Shogo VR, an unofficial VR mod, "
			 L"unaffiliated with Monolith Productions or any of its affiliates and subsidiaries.\n\n"
			 L"You need your own copy of Shogo (Steam or GOG) and a PC VR headset with SteamVR. Your game files are not "
			 L"changed: the mod goes into its own folder and can be removed from Windows' Installed apps.",
			 { x, y, right, rc.bottom - w.footH - S(6) }, DT_LEFT | DT_WORDBREAK);
	}
	else if (w.page == WP_LEGAL)
	{
		text(w.title, kText, L"About & legal", { x, y, right, y + S(30) }, DT_LEFT | DT_SINGLELINE);
	}
	else if (w.page == WP_LOCATION)
	{
		text(w.title, kText, L"Where is Shogo installed?", { x, y, right, y + S(30) }, DT_LEFT | DT_SINGLELINE);
		text(w.font, kText, L"Shogo VR goes into a ShogoVR folder inside your Shogo folder (the one with Client.exe and SHOGO.REZ).",
			 { x, w.headerH + S(74), right, w.headerH + S(94) }, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
		wchar_t buf[MAX_PATH * 2] = L"";
		GetWindowTextW(w.path, buf, MAX_PATH * 2);
		std::wstring found = ResolveShogoDir(buf);
		int sy = w.headerH + S(138);
		HBRUSH dot = CreateSolidBrush(found.empty() ? kRed : RGB(30, 150, 60));
		HGDIOBJ ob = SelectObject(dc, dot), op = SelectObject(dc, GetStockObject(NULL_PEN));
		Ellipse(dc, x, sy + S(5), x + S(11), sy + S(16));
		SelectObject(dc, ob); SelectObject(dc, op);
		DeleteObject(dot);
		RECT st = { x + S(20), sy, right, sy + S(22) };
		if (!found.empty()) text(w.font, kText, L"Shogo found: " + found, st, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS);
		else text(w.font, kRed, L"That isn't a Shogo folder - choose the folder with Client.exe and SHOGO.REZ.", st, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
	}
	else
	{
		text(w.title, kText, L"Shogo VR is installed", { x, y, right, y + S(30) }, DT_LEFT | DT_SINGLELINE);
		y += S(40);
		text(w.font, kText, w.summary, { x, y, right, rc.bottom - w.footH - S(46) }, DT_LEFT | DT_WORDBREAK);
	}

	RECT line = { 0, rc.bottom - w.footH, rc.right, rc.bottom - w.footH + 1 };
	FillRectColor(dc, line, RGB(220, 220, 224));
	text(w.small, kMuted, L"Shogo VR " SHOGOVR_VERSION_STR L"  \u00B7  unofficial fan-made VR mod  \u00B7  free",
		 { S(20), rc.bottom - w.footH, rc.right / 2, rc.bottom }, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
}

static LRESULT CALLBACK WizardProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	WizardUI& w = *g_W;
	switch (msg)
	{
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		RECT rc;
		GetClientRect(hwnd, &rc);
		HDC mem = CreateCompatibleDC(dc);
		HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
		HGDIOBJ old = SelectObject(mem, bmp);
		PaintWizard(hwnd, mem);
		BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
		SelectObject(mem, old);
		DeleteObject(bmp);
		DeleteDC(mem);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_CTLCOLORSTATIC:
	{
		static HBRUSH body = CreateSolidBrush(gui::kBodyBg);
		SetBkColor((HDC)wp, gui::kBodyBg);
		SetTextColor((HDC)wp, gui::kText);
		return (LRESULT)body;
	}
	case WM_DRAWITEM:
	{
		const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lp;
		gui::DrawButton(di, di->CtlID == IDW_NEXT, di->CtlID == IDW_NEXT ? w.bold : w.font);
		return TRUE;
	}
	case WM_COMMAND:
		switch (LOWORD(wp))
		{
		case IDW_AGREE:
			WizardShowPage(w, w.page);
			return 0;
		case IDW_PATH:
			if (HIWORD(wp) == EN_CHANGE) WizardShowPage(w, w.page);
			return 0;
		case IDW_BROWSE:
		{
			std::wstring picked = PickFolder();
			if (!picked.empty())
			{
				std::wstring d = ResolveShogoDir(picked);
				SetWindowTextW(w.path, (d.empty() ? picked : d).c_str());
			}
			return 0;
		}
		case IDW_BACK:
			if (w.page > WP_WELCOME) WizardShowPage(w, w.page - 1);
			return 0;
		case IDW_CANCEL:
			if (MessageBoxW(hwnd, L"Cancel the Shogo VR setup?", SHOGOVR_PRODUCT L" Setup", MB_YESNO | MB_ICONQUESTION) == IDYES) DestroyWindow(hwnd);
			return 0;
		case IDW_NEXT:
			if (w.page == WP_WELCOME || w.page == WP_LEGAL) { WizardShowPage(w, w.page + 1); return 0; }
			if (w.page == WP_LOCATION)
			{
				wchar_t buf[MAX_PATH * 2] = L"";
				GetWindowTextW(w.path, buf, MAX_PATH * 2);
				w.gameDir = ResolveShogoDir(buf);
				InstallOptions o;
				o.desktop = SendMessageW(w.desktop, BM_GETCHECK, 0, 0) == BST_CHECKED;
				o.startMenu = SendMessageW(w.startMenu, BM_GETCHECK, 0, 0) == BST_CHECKED;
				o.dpiFix = IsWindowVisible(w.dpi) && SendMessageW(w.dpi, BM_GETCHECK, 0, 0) == BST_CHECKED;
				o.vram = SendMessageW(w.vram, BM_GETCHECK, 0, 0) == BST_CHECKED;
				std::wstring error;
				SetCursor(LoadCursor(nullptr, IDC_WAIT));
				if (!DoInstall(*w.files, w.gameDir, o, w.summary, error))
				{
					MessageBoxW(hwnd, error.c_str(), SHOGOVR_PRODUCT L" Setup", MB_OK | MB_ICONERROR);
					return 0;
				}
				WizardShowPage(w, WP_DONE);
				return 0;
			}
			w.startNow = SendMessageW(w.launch, BM_GETCHECK, 0, 0) == BST_CHECKED;
			DestroyWindow(hwnd);
			return 0;
		}
		return 0;
	case WM_CLOSE:
		if (w.page == WP_DONE) { DestroyWindow(hwnd); return 0; }
		SendMessageW(hwnd, WM_COMMAND, IDW_CANCEL, 0);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static int RunInstallWizard(const std::vector<PakFile>& files)
{
	using gui::S;
	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
	InitCommonControlsEx(&icc);
	gui::InitDpi();

	WizardUI w;
	g_W = &w;
	w.files = &files;
	if (const PakFile* a = PakFind(files, "ShogoVR/AUTHORS.txt"))
	{
		std::string s(a->data.begin(), a->data.end());
		w.authors = Utf8ToWide(s);
		while (!w.authors.empty() && (w.authors.back() == L'\n' || w.authors.back() == L'\r')) w.authors.pop_back();
	}

	w.font = gui::MakeFont(9, false);
	w.bold = gui::MakeFont(10, true);
	w.title = gui::MakeFont(15, true);
	w.small = gui::MakeFont(8, false);
	w.W = S(760);
	w.headerH = S(96);
	w.footH = S(58);
	w.H = S(600);
	w.header = gui::LoadImageResource(IDR_HEADER_PNG, w.W - S(80), w.headerH - S(18));
	w.banner = gui::LoadImageResource(IDR_BANNER_JPG, w.W - S(56), S(250));

	WNDCLASSW wc = {};
	wc.lpfnWndProc = WizardProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
	wc.lpszClassName = L"ShogoVRSetup";
	RegisterClassW(&wc);
	RECT rc = { 0, 0, w.W, w.H };
	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
	AdjustWindowRect(&rc, style, FALSE);
	w.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Shogo VR Setup", style, CW_USEDEFAULT, CW_USEDEFAULT,
							 rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
	if (!w.hwnd) return 1;

	auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD st, int x, int y, int cw, int ch, int id, HFONT f) -> HWND
	{
		HWND c = CreateWindowExW(0, cls, text, WS_CHILD | st, x, y, cw, ch, w.hwnd, (HMENU)(INT_PTR)id, wc.hInstance, nullptr);
		SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
		return c;
	};
	int x = S(28), cw = w.W - S(56), top = w.headerH + S(100);
	int by = w.H - w.footH + (w.footH - S(36)) / 2;
	w.cancel = make(L"BUTTON", L"Cancel", BS_OWNERDRAW | WS_TABSTOP | WS_VISIBLE, w.W - S(20) - S(100), by, S(100), S(36), IDW_CANCEL, w.font);
	w.next = make(L"BUTTON", L"Next", BS_OWNERDRAW | WS_TABSTOP | WS_VISIBLE, w.W - S(20) - S(100) - S(10) - S(130), by, S(130), S(36), IDW_NEXT, w.bold);
	w.back = make(L"BUTTON", L"Back", BS_OWNERDRAW | WS_TABSTOP, w.W - S(20) - S(100) - S(10) - S(130) - S(10) - S(100), by, S(100), S(36), IDW_BACK, w.font);

	w.legal = make(L"EDIT", gui::AttributionText(w.authors).c_str(), ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER,
				   x, w.headerH + S(76), cw, w.H - w.footH - w.headerH - S(76) - S(44), IDW_LEGAL, w.font);
	SendMessageW(w.legal, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(10), S(10)));
	w.agree = make(L"BUTTON", L"I understand that Shogo VR is an unofficial mod, not made by or supported by Monolith Productions.",
				   BS_AUTOCHECKBOX | WS_TABSTOP, x, w.H - w.footH - S(34), cw, S(24), IDW_AGREE, w.font);

	std::wstring detected = LocateShogo();
	w.path = make(L"EDIT", detected.c_str(), ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, x, top, cw - S(120), S(26), IDW_PATH, w.font);
	w.browse = make(L"BUTTON", L"Browse...", BS_OWNERDRAW | WS_TABSTOP, x + cw - S(110), top - S(2), S(110), S(30), IDW_BROWSE, w.font);
	int oy = w.headerH + S(176);
	w.desktop = make(L"BUTTON", L"Create a Desktop shortcut", BS_AUTOCHECKBOX | WS_TABSTOP, x, oy, cw, S(24), IDW_DESKTOP, w.font);
	w.startMenu = make(L"BUTTON", L"Create Start menu shortcuts (Shogo VR, Shogo VR Settings)", BS_AUTOCHECKBOX | WS_TABSTOP, x, oy + S(30), cw, S(24), IDW_STARTMENU, w.font);
	w.vram = make(L"BUTTON", L"Give Shogo enough video memory in dgVoodoo (2 GB - recommended for VR resolutions)", BS_AUTOCHECKBOX | WS_TABSTOP, x, oy + S(60), cw, S(24), IDW_VRAM, w.font);
	w.dpi = make(L"BUTTON", L"Stop Windows display scaling from blurring Shogo (recommended)", BS_AUTOCHECKBOX | WS_TABSTOP, x, oy + S(90), cw, S(24), IDW_DPI, w.font);
	for (HWND h : { w.desktop, w.startMenu, w.vram, w.dpi }) SendMessageW(h, BM_SETCHECK, BST_CHECKED, 0);
	w.launch = make(L"BUTTON", L"Start Shogo VR now", BS_AUTOCHECKBOX | WS_TABSTOP, x, w.H - w.footH - S(36), cw, S(24), IDW_LAUNCH, w.font);
	SendMessageW(w.launch, BM_SETCHECK, BST_CHECKED, 0);

	WizardShowPage(w, WP_WELCOME);
	ShowWindow(w.hwnd, SW_SHOW);
	UpdateWindow(w.hwnd);
	MSG m;
	while (GetMessageW(&m, nullptr, 0, 0) > 0)
	{
		if (!IsDialogMessageW(w.hwnd, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
	}
	w.header.Free();
	w.banner.Free();
	for (HFONT f : { w.font, w.bold, w.title, w.small }) DeleteObject(f);
	g_W = nullptr;

	if (w.startNow && !w.gameDir.empty())
	{
		std::wstring modDir = JoinPath(w.gameDir, SHOGOVR_MOD_FOLDER);
		ShellExecuteW(nullptr, L"open", JoinPath(modDir, L"ShogoVR.exe").c_str(), nullptr, modDir.c_str(), SW_SHOWNORMAL);
	}
	return w.summary.empty() ? 1 : 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	std::wstring mode, arg1, arg2;
	for (int i = 1; i < argc; ++i)
	{
		std::wstring a = argv[i];
		if (a == L"--quiet") g_quiet = true;
		else if (a == L"--build" || a == L"--install") mode = a;
		else if (arg1.empty()) arg1 = a;
		else if (arg2.empty()) arg2 = a;
	}
	LocalFree(argv);

	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	std::vector<char> self;
	uint64_t stubSize = 0;
	std::vector<PakFile> files;
	if (!ReadWholeFile(ExePath(), self)) { Say(L"Couldn't read the setup program itself.", MB_ICONERROR); return 1; }
	PakRead(self, stubSize, files);

	bool bHasDll = PakFind(files, "ShogoVR/CShell.dll") != nullptr;
	bool bHasRest = PakFind(files, "ShogoVR/ShogoVR.exe") && PakFind(files, "ShogoVR/ShogoVRBridge.exe");
	if (!bHasRest) { Say(L"This setup program is damaged (its files are missing). Please download it again.", MB_ICONERROR); return 1; }

	int ret;
	if (!bHasDll || mode == L"--build")
	{
		if (bHasDll)
		{
			// Rebuilding from a finished installer: swap in the new DLL.
			files.erase(std::remove_if(files.begin(), files.end(),
				[](const PakFile& f) { return _stricmp(f.name.c_str(), "ShogoVR/CShell.dll") == 0; }), files.end());
		}
		ret = Build(self, stubSize, files, arg1, arg2);
	}
	else if (mode == L"--install" || g_quiet)
	{
		ret = Install(files, arg1);
	}
	else
	{
		ret = RunInstallWizard(files);
	}

	CoUninitialize();
	return ret;
}
