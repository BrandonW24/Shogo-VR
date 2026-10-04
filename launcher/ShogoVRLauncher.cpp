// ======================================================================= //
//
// ShogoVRLauncher.cpp  ->  ShogoVR.exe
//
// One click to play: starts the headset bridge (minimized), starts Shogo
// with its own archives plus the ShogoVR mod folder, and closes the bridge
// again when the game exits.
//
// Lives in <Shogo>\ShogoVR\.
//   ShogoVR.exe               play
//   ShogoVR.exe --settings    the Shogo VR Settings window
//   ShogoVR.exe --uninstall   remove the mod
// Extra game options can go in ShogoVR.ini:
//     [Launch]
//     ExtraArgs=+EnableRetailLevels 1
//
// ======================================================================= //

#include "Common.h"
#include "Gui.h"
#include "Settings.h"
#include "Bindings.h"
#include <shellapi.h>
#include <cwctype>

static void Message(const std::wstring& text, UINT icon = MB_ICONINFORMATION)
{
	MessageBoxW(nullptr, text.c_str(), SHOGOVR_PRODUCT, MB_OK | icon);
}

// ShogoVR_launch.log: what the launcher did, for bug reports.
static std::wstring g_launchLog;
static void LaunchLog(const std::wstring& line)
{
	if (g_launchLog.empty()) return;
	FILE* f = _wfopen(g_launchLog.c_str(), L"a, ccs=UTF-8");
	if (!f) return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	fwprintf(f, L"[%02u:%02u:%02u] %ls\n", t.wHour, t.wMinute, t.wSecond, line.c_str());
	fclose(f);
}

static std::wstring ErrorText(DWORD err)
{
	wchar_t* buf = nullptr;
	FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				   nullptr, err, 0, (LPWSTR)&buf, 0, nullptr);
	std::wstring text = buf ? buf : L"";
	if (buf) LocalFree(buf);
	while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) text.pop_back();
	return text + L" (error " + std::to_wstring(err) + L")";
}

static bool ContainsNoCase(std::wstring hay, std::wstring needle)
{
	for (auto& c : hay) c = (wchar_t)towlower(c);
	for (auto& c : needle) c = (wchar_t)towlower(c);
	return hay.find(needle) != std::wstring::npos;
}

static bool g_assumeYes = false;		// --yes: answer the cleanup question automatically (testing)

// A Shogo from an earlier session still running - typically one that crashed
// and is being held open by Windows Error Reporting, or one that hung while
// quitting?  A new Shogo started next to it can crash at once, so offer to
// close it first.
static bool CloseLeftoverGames(const std::wstring& gameDir)
{
	std::vector<DWORD> pids = FindGameProcesses(gameDir);
	if (pids.empty()) return true;
	LaunchLog(L"Found Shogo still running from an earlier session (" + std::to_wstring(pids.size()) + L" process(es))");
	if (!g_assumeYes &&
		MessageBoxW(nullptr, L"Shogo from an earlier session is still running in the background - it probably didn't close "
							 L"properly (after a crash, Windows can keep it open for a while).\n\nClose it and start Shogo VR?",
					SHOGOVR_PRODUCT, MB_YESNO | MB_ICONWARNING) != IDYES)
	{
		LaunchLog(L"Left it running");
		return false;
	}
	bool bAll = true;
	for (DWORD pid : pids)
	{
		HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
		if (h && TerminateProcess(h, 1))
		{
			WaitForSingleObject(h, 5000);
			LaunchLog(L"Closed the earlier Shogo (process " + std::to_wstring(pid) + L")");
		}
		else
		{
			bAll = false;
			LaunchLog(L"Couldn't close process " + std::to_wstring(pid) + L": " + ErrorText(GetLastError()));
		}
		if (h) CloseHandle(h);
	}
	if (!bAll)
	{
		Message(L"Couldn't close the earlier Shogo (it may be running as administrator). Please end Client.exe in "
				L"Task Manager (Details tab), then start Shogo VR again.", MB_ICONWARNING);
		return false;
	}
	Sleep(1500);		// let Windows and the graphics driver let go of it
	return true;
}

// ----------------------------------------------------------------------- //
//  Starting the game through Shogo's own launcher (Shogo.exe)
// ----------------------------------------------------------------------- //

struct TopWindowSearch { DWORD pid; HWND hwnd; };
static BOOL CALLBACK FindTopWindowProc(HWND h, LPARAM lp)
{
	TopWindowSearch* f = (TopWindowSearch*)lp;
	DWORD pid = 0;
	GetWindowThreadProcessId(h, &pid);
	if (pid == f->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { f->hwnd = h; return FALSE; }
	return TRUE;
}

static BOOL CALLBACK FindLaunchButtonProc(HWND h, LPARAM lp)
{
	wchar_t cls[32] = L"", text[128] = L"";
	GetClassNameW(h, cls, 32);
	if (_wcsicmp(cls, L"Button") != 0) return TRUE;
	GetWindowTextW(h, text, 128);
	std::wstring t;
	for (const wchar_t* c = text; *c; ++c) if (*c != L'&') t += *c;		// "&Launch Shogo" -> "Launch Shogo"
	if (ContainsNoCase(t, L"launch shogo") || _wcsicmp(t.c_str(), L"Launch") == 0 || _wcsicmp(t.c_str(), L"Play") == 0)
	{
		*(HWND*)lp = h;
		return FALSE;
	}
	return TRUE;
}

static bool FileTimeAfter(const std::wstring& path, const FILETIME& since)
{
	WIN32_FILE_ATTRIBUTE_DATA fa;
	return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && CompareFileTime(&fa.ftLastWriteTime, &since) > 0;
}

// Opens Shogo's own launcher and presses "Launch Shogo" - exactly the way the
// game starts when the player does it by hand, with the options saved in that
// launcher (Advanced: -rez ShogoVR, other mods), the same rights and the same
// compatibility settings.  Returns the exit code, or -1 if Shogo.exe can't be
// used (then the caller starts Client.exe directly).
static int PlayViaShogoLauncher(const std::wstring& gameDir, const std::wstring& modDir, int vramMB)
{
	std::wstring shogoExe = JoinPath(gameDir, L"Shogo.exe");
	std::wstring gameLog = JoinPath(modDir, L"ShogoVR_game.log");

	for (int attempt = 0; attempt < 2; ++attempt)
	{
		FILETIME launchTime;
		GetSystemTimeAsFileTime(&launchTime);

		// 1. Shogo's launcher: one that's already open, or a new one.
		std::vector<DWORD> open = FindProcessesByPath(shogoExe);
		DWORD pid = open.empty() ? 0 : open[0];
		if (!pid)
		{
			std::wstring cmd = L"\"" + shogoExe + L"\"";
			STARTUPINFOW si = { sizeof(si) };
			PROCESS_INFORMATION pi = {};
			if (CreateProcessW(shogoExe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, gameDir.c_str(), &si, &pi))
			{
				pid = pi.dwProcessId;
				CloseHandle(pi.hThread);
				CloseHandle(pi.hProcess);
				LaunchLog(L"Opened Shogo's own launcher (Shogo.exe)");
			}
			else
			{
				DWORD err = GetLastError();
				if (err == ERROR_ELEVATION_REQUIRED && !IsProcessElevated(GetCurrentProcess()))
				{
					// Shogo.exe runs as administrator: Windows only lets a program with the
					// same rights press its button, so come back with them (one prompt,
					// the same one Shogo.exe itself would show).
					LaunchLog(L"Shogo's launcher runs as administrator - restarting the Shogo VR launcher with the same rights");
					std::wstring self = ExePath();
					std::wstring params = std::wstring(L"--play") + (g_assumeYes ? L" --yes" : L"");
					HINSTANCE r = ShellExecuteW(nullptr, L"runas", self.c_str(), params.c_str(), modDir.c_str(), SW_SHOWNORMAL);
					if ((INT_PTR)r > 32) return 0;
					LaunchLog(L"Not restarted with administrator rights (" + ErrorText(GetLastError()) + L")");
				}
				else LaunchLog(L"Couldn't open Shogo's launcher: " + ErrorText(err));
				return -1;
			}
		}

		// 2. Press "Launch Shogo".
		HWND button = nullptr;
		for (int i = 0; i < 80 && !button; ++i)			// up to 20 seconds
		{
			TopWindowSearch f = { pid, nullptr };
			EnumWindows(FindTopWindowProc, (LPARAM)&f);
			if (f.hwnd)
			{
				HWND b = nullptr;
				EnumChildWindows(f.hwnd, FindLaunchButtonProc, (LPARAM)&b);
				if (b && IsWindowEnabled(b)) button = b;
			}
			if (!button) Sleep(250);
		}
		if (button)
		{
			Sleep(300);
			PostMessageW(button, BM_CLICK, 0, 0);
			LaunchLog(L"Pressed \"Launch Shogo\"");
		}
		else LaunchLog(L"Couldn't find the \"Launch Shogo\" button - leaving Shogo's launcher open for you to press it");

		// 3. Wait for the game to appear.
		DWORD gamePid = 0;
		ULONGLONG t0 = GetTickCount64(), limit = button ? 60000 : 600000;
		while (GetTickCount64() - t0 < limit)
		{
			std::vector<DWORD> g = FindGameProcesses(gameDir);
			if (!g.empty()) { gamePid = g[0]; break; }
			if (FindProcessesByPath(shogoExe).empty() && GetTickCount64() - t0 > 3000) break;	// launcher closed, no game
			Sleep(200);
		}
		if (!gamePid)
		{
			LaunchLog(L"Shogo didn't start from its launcher");
			if (attempt == 0) { Sleep(1500); continue; }
			return -1;
		}
		LaunchLog(L"Shogo is running (process " + std::to_wstring(gamePid) + L")");

		// 4. Until it ends: did the VR mod load?
		HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, gamePid);
		ULONGLONG started = GetTickCount64();
		bool modSeen = false, warned = false;
		for (;;)
		{
			if (h) { if (WaitForSingleObject(h, 500) == WAIT_OBJECT_0) break; }
			else
			{
				std::vector<DWORD> g = FindGameProcesses(gameDir);
				if (std::find(g.begin(), g.end(), gamePid) == g.end()) break;
				Sleep(500);
			}
			if (!modSeen && FileTimeAfter(gameLog, launchTime))
			{
				modSeen = true;
				LaunchLog(L"The VR mod is running");
			}
			if (!modSeen && !warned && GetTickCount64() - started > 15000)
			{
				warned = true;
				LaunchLog(L"Shogo is running without the VR mod (-rez ShogoVR is missing from Shogo's launcher)");
				if (!g_assumeYes)
					MessageBoxW(nullptr, L"Shogo started without the VR mod.\n\nIn Shogo's own launcher click Advanced..., put -rez ShogoVR "
										 L"in the Command-Line box, tick \"Always specify these command-line parameters\", click OK - "
										 L"then quit Shogo and start Shogo VR again.",
								SHOGOVR_PRODUCT, MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
			}
		}
		DWORD code = 0;
		bool haveCode = h && GetExitCodeProcess(h, &code);
		if (h) CloseHandle(h);
		ULONGLONG secs = (GetTickCount64() - started) / 1000;
		if (!modSeen && FileTimeAfter(gameLog, launchTime)) modSeen = true;
		if (!modSeen && !warned && secs >= 5)
		{
			LaunchLog(L"Shogo ran without the VR mod (-rez ShogoVR is missing from Shogo's launcher)");
			if (!g_assumeYes)
				MessageBoxW(nullptr, L"Shogo ran without the VR mod.\n\nIn Shogo's own launcher click Advanced..., put -rez ShogoVR "
									 L"in the Command-Line box, tick \"Always specify these command-line parameters\", click OK, "
									 L"then start Shogo VR again.", SHOGOVR_PRODUCT, MB_OK | MB_ICONWARNING);
		}
		wchar_t hex[16];
		swprintf(hex, 16, L"0x%08X", code);
		LaunchLog(L"Shogo closed after " + std::to_wstring(secs) + L" s" + (haveCode ? L" (exit code " + std::wstring(hex) + L")" : L""));

		bool bCrashedEarly = secs < 10 && (!haveCode || code != 0);
		if (!bCrashedEarly) return 0;
		if (attempt == 0)
		{
			LaunchLog(L"That looks like a crash at startup - waiting a moment and trying again");
			Sleep(3000);
			CloseLeftoverGames(gameDir);
			continue;
		}
		std::wstring why = L"Shogo closed right after starting" + (haveCode ? L" (exit code " + std::wstring(hex) + L")" : std::wstring()) + L".";
		if (vramMB > 2048)
			why += L"\n\nVideo memory is set to " + std::to_wstring(vramMB / 1024) + L" GB in VR Settings. Some old games crash with more than 2 GB - try 2 GB.";
		why += L"\n\nA log of what happened is in ShogoVR_launch.log in the ShogoVR folder.";
		LaunchLog(L"Gave up: " + why);
		Message(why, MB_ICONWARNING);
		return 1;
	}
	return 1;
}

static void QuitBridge(bool bStarted, PROCESS_INFORMATION& bridge)
{
	if (!bStarted) return;
	HANDLE quit = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\ShogoVRBridge_Quit");
	if (quit) { SetEvent(quit); CloseHandle(quit); }
	WaitForSingleObject(bridge.hProcess, 10000);
	CloseHandle(bridge.hProcess);
	CloseHandle(bridge.hThread);
}

// Offer Shogo's own launcher - once started that way (with -rez ShogoVR),
// the game remembers its command line and this shortcut reuses it.
static void OfferShogoLauncher(const std::wstring& gameDir, const std::wstring& why, bool bKnownGood = false)
{
	std::wstring shogoExe = JoinPath(gameDir, L"Shogo.exe");
	bool bHave = FileExists(shogoExe);
	std::wstring text = why + L"\n\n";
	if (bHave && bKnownGood)
		text += L"Shogo starts fine from its own launcher (Shogo.exe) on this PC, and the headset bridge starts by "
				L"itself that way too. A log of what happened is in ShogoVR_launch.log in the ShogoVR folder.\n\n"
				L"Open Shogo's launcher now?";
	else if (bHave)
		text += L"If Shogo starts fine from its own launcher (Shogo.exe), start it that way once, with -rez ShogoVR "
				L"under Advanced. Shogo VR then remembers exactly how the game is started on this PC, and this "
				L"shortcut will work from then on (the headset bridge starts by itself either way).\n\n"
				L"Open Shogo's launcher now?";
	else
		text += L"A log of what happened is in ShogoVR_launch.log in the ShogoVR folder.";
	if (bHave)
	{
		if (MessageBoxW(nullptr, text.c_str(), SHOGOVR_PRODUCT, MB_YESNO | MB_ICONWARNING) == IDYES)
			ShellExecuteW(nullptr, L"open", shogoExe.c_str(), nullptr, gameDir.c_str(), SW_SHOWNORMAL);
	}
	else
	{
		Message(text, MB_ICONWARNING);
	}
}

// Where's the game?  Normally right above this program's folder; otherwise
// look for it (e.g. when this program was copied somewhere else).
static bool ResolveDirs(const std::wstring& exeDir, std::wstring& gameDir, std::wstring& modDir)
{
	gameDir = DirOf(exeDir);
	modDir = exeDir;
	if (IsShogoDir(gameDir) && FileExists(JoinPath(modDir, L"CShell.dll"))) return true;
	std::wstring found = LocateShogo();
	if (found.empty() || !FileExists(JoinPath(JoinPath(found, SHOGOVR_MOD_FOLDER), L"CShell.dll"))) return false;
	gameDir = found;
	modDir = JoinPath(found, SHOGOVR_MOD_FOLDER);
	return true;
}

static int Play(const std::wstring& gameDir, const std::wstring& modDir)
{

	g_launchLog = JoinPath(modDir, L"ShogoVR_launch.log");
	DeleteFileW(g_launchLog.c_str());
	LaunchLog(L"Shogo VR launcher - game folder: " + gameDir);

	std::wstring ini = JoinPath(modDir, L"ShogoVR.ini");

	// 0. Enough (emulated) video memory, so textures don't get blurrier over time.
	int vramMB = GetPrivateProfileIntW(L"Launch", L"VramMB", 2048, ini.c_str());
	if (GetPrivateProfileIntW(L"Launch", L"FixVram", 1, ini.c_str()) == 0) vramMB = 0;		// older setting: leave alone
	if (vramMB >= 256)
	{
		int r = FixDgVoodooVram(gameDir, vramMB, false);
		if (r != 0) LaunchLog(r > 0 ? L"Set dgVoodoo's video memory to " + std::to_wstring(vramMB) + L" MB"
									: L"Couldn't change dgVoodoo's video memory (no permission?)");
	}

	// Picture quality (Picture tab): dgVoodoo's render resolution, smooth
	// edges and texture filtering.
	{
		int scale = GetPrivateProfileIntW(L"Launch", L"RenderScale", 2, ini.c_str());
		int msaa = GetPrivateProfileIntW(L"Launch", L"Antialiasing", 2, ini.c_str());
		int aniso = GetPrivateProfileIntW(L"Launch", L"Anisotropic", 16, ini.c_str());
		struct { const char* key; std::string value; } opts[] = {
			{ "Resolution",		scale >= 2 ? std::to_string(scale) + "x" : std::string("unforced") },
			{ "Antialiasing",	msaa >= 2 ? std::to_string(msaa) + "x" : std::string("appdriven") },
			{ "Filtering",		aniso >= 2 ? std::to_string(aniso) : std::string("appdriven") },
		};
		for (auto& o : opts)
		{
			int r = SetDgVoodooDirectX(gameDir, o.key, o.value);
			std::wstring k(o.key, o.key + strlen(o.key)), v(o.value.begin(), o.value.end());
			if (r > 0) LaunchLog(L"dgVoodoo " + k + L" = " + v);
			else if (r < 0) LaunchLog(L"Couldn't change dgVoodoo's " + k + L" (no permission?)");
		}
	}

	// 1. The headset bridge: the game starts it itself, so it always has the
	// game's own rights.  (Windows doesn't let a normal program manage the
	// window of a game running as administrator - which is how Shogo runs on
	// some PCs.)
	PROCESS_INFORMATION bridge = {};
	bool bStartedBridge = false;
	LaunchLog(IsBridgeRunning() ? L"A headset bridge is already running"
								: L"The game will start the headset bridge itself");

	// 2. The game's command line.  Two candidates: the one Shogo was last
	// started with on this PC (remembered by the mod - proven to work), and
	// the standard one.  A remembered line is only trusted if it clearly
	// loads Shogo's own archive.
	wchar_t learned[4096] = L"";
	GetPrivateProfileStringW(L"Launch", L"GameArgs", L"", learned, 4096, ini.c_str());
	std::wstring remembered = (learned[0] == L'!' && learned[1]) ? std::wstring(learned + 1) : std::wstring();
	if (!remembered.empty())
	{
		size_t first = remembered.find_first_not_of(L' ');
		bool bStartsWell = first != std::wstring::npos && (remembered[first] == L'-' || remembered[first] == L'+');
		if (!bStartsWell || !ContainsNoCase(remembered, L"-rez") || !ContainsNoCase(remembered, L"shogo.rez"))
		{
			LaunchLog(L"Not using the remembered command line (it doesn't load SHOGO.REZ properly): " + remembered);
			remembered.clear();
		}
	}

	wchar_t extraBuf[2048] = L"";
	GetPrivateProfileStringW(L"Launch", L"ExtraArgs", L"", extraBuf, 2048, ini.c_str());
	bool bSkipMovies = GetPrivateProfileIntW(L"Launch", L"SkipMovies", 0, ini.c_str()) != 0;
	auto finish = [&](std::wstring args)
	{
		if (!ContainsNoCase(args, L"shogovr")) args += L" -rez \"ShogoVR\"";
		if (extraBuf[0]) args += std::wstring(L" ") + extraBuf;
		if (bSkipMovies && !ContainsNoCase(args, L"DisableMovies")) args += L" +DisableMovies 1";
		return args + L" +VRLauncher 1";		// lets the game know this shortcut started it
	};

	std::vector<std::pair<std::wstring, std::wstring>> attempts;	// (description, arguments)
	if (!remembered.empty()) attempts.push_back({ L"the command line Shogo was last started with", finish(remembered) });
	attempts.push_back({ L"the standard command line", finish(BuildGameArgs(gameDir, L"")) });

	// 3. Start it through the Windows shell, like Shogo's own launcher - that
	// honours compatibility settings such as "Run as administrator".  If it
	// crashes straight away, try the other command line once.
	std::wstring client = JoinPath(gameDir, L"Client.exe");
	if (!CloseLeftoverGames(gameDir)) return 1;

	// Preferred: through Shogo's own launcher - the way that works on every
	// PC where the game itself works.  Starting Client.exe directly is the
	// fallback (no Shogo.exe, or chosen in VR Settings).
	int method = GetPrivateProfileIntW(L"Launch", L"Method", 0, ini.c_str());
	if (method == 0 && FileExists(JoinPath(gameDir, L"Shogo.exe")))
	{
		int r = PlayViaShogoLauncher(gameDir, modDir, vramMB);
		if (r >= 0) return r;
		LaunchLog(L"Falling back to starting Client.exe directly");
	}

	// Each command line gets two tries: a crash right at startup is often a
	// one-off (the graphics driver or Windows still busy with a previous run).
	ULONGLONG secs = 0;
	DWORD code = 0;
	const size_t kTries = 2;
	for (size_t t = 0; t < attempts.size() * kTries; ++t)
	{
		size_t attempt = t / kTries;
		// Exactly the way Shogo's own launcher does it: the program's name
		// alone ("Client.exe"), not its full path.  The 1998 engine reads the
		// whole command line, and a folder name like "Shogo - Mobile Armor
		// Division" (GOG) has a lone "-" in it that it mistakes for an option.
		std::wstring cmd = L"Client.exe " + attempts[attempt].second;
		LaunchLog(L"Starting with " + attempts[attempt].first + L": " + cmd);
		HANDLE hGame = nullptr;
		STARTUPINFOW si = { sizeof(si) };
		PROCESS_INFORMATION pi = {};
		if (CreateProcessW(client.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, gameDir.c_str(), &si, &pi))
		{
			CloseHandle(pi.hThread);
			hGame = pi.hProcess;
		}
		else
		{
			DWORD err = GetLastError();
			if (err == ERROR_ELEVATION_REQUIRED)
			{
				// It needs administrator rights: go through the Windows shell
				// (which asks), using the short 8.3 path so the folder name
				// can't confuse the engine.
				wchar_t shortPath[MAX_PATH * 2] = L"";
				std::wstring file = client;
				if (GetShortPathNameW(client.c_str(), shortPath, MAX_PATH * 2) && shortPath[0]) file = shortPath;
				LaunchLog(L"Shogo needs administrator rights - starting it through the Windows shell as " + file);
				SHELLEXECUTEINFOW sei = { sizeof(sei) };
				sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
				sei.lpVerb = L"open";
				sei.lpFile = file.c_str();
				sei.lpParameters = attempts[attempt].second.c_str();
				sei.lpDirectory = gameDir.c_str();
				sei.nShow = SW_SHOWNORMAL;
				if (ShellExecuteExW(&sei) && sei.hProcess) hGame = sei.hProcess;
				else err = GetLastError();
			}
			if (!hGame)
			{
				std::wstring errText = ErrorText(err);
				LaunchLog(L"Couldn't start Shogo: " + errText);
				QuitBridge(bStartedBridge, bridge);
				OfferShogoLauncher(gameDir, L"Couldn't start Shogo:\n" + errText, learned[0] == L'!');
				return 1;
			}
		}

		ULONGLONG started = GetTickCount64();
		WaitForSingleObject(hGame, INFINITE);
		code = 0;
		GetExitCodeProcess(hGame, &code);
		CloseHandle(hGame);
		secs = (GetTickCount64() - started) / 1000;
		wchar_t hex[16];
		swprintf(hex, 16, L"0x%08X", code);
		LaunchLog(L"Shogo closed after " + std::to_wstring(secs) + L" s (exit code " + hex + L")");

		bool bCrashedEarly = secs < 10 && code != 0;
		if (!bCrashedEarly || t + 1 >= attempts.size() * kTries) break;
		if ((t + 1) % kTries != 0)
		{
			LaunchLog(L"That looks like a crash at startup - waiting a moment and trying again");
			Sleep(3000);
			CloseLeftoverGames(gameDir);		// in case the crashed one is still held open
		}
		else
		{
			LaunchLog(L"Still crashing - trying " + attempts[attempt + 1].first);
		}
	}

	// 4. Game over: close the bridge we started.
	QuitBridge(bStartedBridge, bridge);

	if (secs < 10)
	{
		wchar_t hex[16];
		swprintf(hex, 16, L"0x%08X", code);
		std::wstring why = L"Shogo closed right after starting (exit code " + std::wstring(hex) + L").";
		if (vramMB > 2048)
			why += L"\n\nVideo memory is set to " + std::to_wstring(vramMB / 1024) + L" GB in VR Settings. Some old "
				   L"games crash with more than 2 GB - try 2 GB.";
		LaunchLog(L"Gave up: " + why);
		OfferShogoLauncher(gameDir, why, learned[0] == L'!');
	}
	return 0;
}

static bool CanWriteTo(const std::wstring& dir)
{
	std::wstring probe = JoinPath(dir, L"~shogovr-write-test.tmp");
	HANDLE f = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
	if (f == INVALID_HANDLE_VALUE) return false;
	CloseHandle(f);
	return true;
}

static int Uninstall(const std::wstring& modDir, bool bQuiet)
{
	if (!bQuiet && MessageBoxW(nullptr,
			L"Remove Shogo VR?\n\nThis deletes the ShogoVR folder and its shortcuts. Shogo itself is not changed.",
			SHOGOVR_PRODUCT, MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;

	// Steam's folder usually needs administrator rights to change.
	if (!CanWriteTo(modDir))
	{
		std::wstring self = ExePath();
		ShellExecuteW(nullptr, L"runas", self.c_str(), L"--uninstall --quiet", modDir.c_str(), SW_SHOWNORMAL);
		return 0;
	}

	if (IsBridgeRunning())
	{
		HANDLE quit = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\ShogoVRBridge_Quit");
		if (quit) { SetEvent(quit); CloseHandle(quit); }
		Sleep(2000);
	}

	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	DeleteFileW(ShortcutPath(FOLDERID_Desktop).c_str());
	DeleteFileW(ShortcutPath(FOLDERID_Programs).c_str());
	DeleteFileW(ShortcutPath(FOLDERID_Programs, L"Shogo VR Settings.lnk").c_str());
	CoUninitialize();
	RegDeleteKeyW(HKEY_CURRENT_USER, SHOGOVR_UNINSTALL_KEY);

	// This program is inside the folder, so remove the folder just after we exit.
	std::wstring cmd = L"cmd.exe /c ping 127.0.0.1 -n 3 > nul & rmdir /s /q \"" + modDir + L"\"";
	STARTUPINFOW si = { sizeof(si) };
	PROCESS_INFORMATION pi = {};
	std::wstring parent = DirOf(modDir);
	if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, parent.c_str(), &si, &pi))
	{
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}

	if (!bQuiet) Message(L"Shogo VR has been removed.");
	return 0;
}

// ======================================================================= //
//  The launcher window
// ======================================================================= //

enum { IDC_TABS = 3000, IDC_PLAY, IDC_CLOSE, IDC_OPEN_SHOGO, IDC_OPEN_FOLDER, IDC_ABOUT_TEXT, IDC_RESET };
enum { PAGE_PLAY = 0, PAGE_SETTINGS, PAGE_PICTURE, PAGE_BUTTONS, PAGE_ABOUT };

struct LauncherUI
{
	std::wstring	gameDir, modDir, authors, runtime, modInfo;
	bool			ok = false;
	int				page = PAGE_PLAY;
	bool			play = false;
	gui::Image		header, banner;
	HFONT			font = nullptr, bold = nullptr, big = nullptr, small = nullptr, head = nullptr;
	HWND			tabs = nullptr, playBtn = nullptr, closeBtn = nullptr, openShogo = nullptr, openFolder = nullptr;
	HWND			about = nullptr, resetBtn = nullptr;
	SettingsUI		settings;
	SettingsUI		picture;
	BindingsUI		bindings;
	int				W = 0, H = 0, headerH = 0, tabsH = 0, footH = 0;
	RECT			pageRect = {};
};
static LauncherUI* g_L = nullptr;

static std::wstring RuntimeName()
{
	std::wstring rt = RegReadString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", KEY_WOW64_64KEY);
	if (rt.empty()) return L"";
	if (ContainsNoCase(rt, L"steamxr")) return L"SteamVR";
	if (ContainsNoCase(rt, L"oculus")) return L"Meta Quest Link (Oculus)";
	if (ContainsNoCase(rt, L"mixedreality")) return L"Windows Mixed Reality";
	if (ContainsNoCase(rt, L"virtualdesktop")) return L"Virtual Desktop";
	size_t slash = rt.find_last_of(L"\\/");
	return slash == std::wstring::npos ? rt : rt.substr(slash + 1);
}

static void ShowPage(LauncherUI& L, int page)
{
	L.page = page;
	ShowSettingsPanel(L.settings, page == PAGE_SETTINGS);
	ShowSettingsPanel(L.picture, page == PAGE_PICTURE);
	ShowBindingsPanel(L.bindings, page == PAGE_BUTTONS);
	ShowWindow(L.resetBtn, (page == PAGE_SETTINGS || page == PAGE_PICTURE || page == PAGE_BUTTONS) ? SW_SHOW : SW_HIDE);
	ShowWindow(L.about, page == PAGE_ABOUT ? SW_SHOW : SW_HIDE);
	ShowWindow(L.openShogo, page == PAGE_PLAY ? SW_SHOW : SW_HIDE);
	ShowWindow(L.openFolder, page == PAGE_PLAY ? SW_SHOW : SW_HIDE);
	TabCtrl_SetCurSel(L.tabs, page);
	InvalidateRect(GetParent(L.tabs), nullptr, TRUE);
}

static void PaintLauncher(HWND hwnd, HDC dc)
{
	LauncherUI& L = *g_L;
	using namespace gui;
	RECT rc;
	GetClientRect(hwnd, &rc);

	// Header band with "In Memory of Monolith Productions".
	RECT band = { 0, 0, rc.right, L.headerH };
	FillRectColor(dc, band, kHeaderBg);
	DrawImage(dc, L.header, (rc.right - L.header.w) / 2, (L.headerH - L.header.h) / 2);
	RECT stripe = { 0, L.headerH - S(3), rc.right, L.headerH };
	FillRectColor(dc, stripe, kRed);

	RECT body = { 0, L.headerH, rc.right, rc.bottom };
	FillRectColor(dc, body, kBodyBg);
	SetBkMode(dc, TRANSPARENT);

	if (L.page == PAGE_PLAY)
	{
		int y = L.pageRect.top + S(8);
		DrawImage(dc, L.banner, (rc.right - L.banner.w) / 2, y);
		y += L.banner.h + S(10);

		// The notice, every time you launch.
		RECT note = { S(24), y, rc.right - S(24), y + S(52) };
		FillRectColor(dc, note, kNoticeBg);
		RECT bar = { note.left, note.top, note.left + S(4), note.bottom };
		FillRectColor(dc, bar, kRed);
		RECT t1 = { note.left + S(14), note.top + S(6), note.right - S(10), note.top + S(28) };
		SelectObject(dc, L.bold);
		SetTextColor(dc, kText);
		DrawTextW(dc, L"Shogo VR is an unofficial VR mod, unaffiliated with Monolith Productions or any of its affiliates and subsidiaries.",
				  -1, &t1, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
		RECT t2 = { note.left + S(14), note.top + S(27), note.right - S(10), note.bottom - S(4) };
		SelectObject(dc, L.small);
		SetTextColor(dc, kMuted);
		std::wstring by = L"Free fan project" + (L.authors.empty() ? std::wstring() : L" by " + L.authors.substr(0, L.authors.find(L'\n'))) +
						  L".  Not made by or supported by Monolith Productions.";
		DrawTextW(dc, by.c_str(), -1, &t2, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER | DT_NOPREFIX);
		y = note.bottom + S(10);

		// Ready checks.
		auto line = [&](bool good, const std::wstring& text)
		{
			RECT r = { S(28), y, rc.right - S(260), y + S(20) };
			HBRUSH dot = CreateSolidBrush(good ? RGB(30, 150, 60) : kRed);
			HGDIOBJ ob = SelectObject(dc, dot), op = SelectObject(dc, GetStockObject(NULL_PEN));
			Ellipse(dc, r.left, r.top + S(5), r.left + S(11), r.top + S(16));
			SelectObject(dc, ob); SelectObject(dc, op);
			DeleteObject(dot);
			SelectObject(dc, L.font);
			r.left += S(20);
			SetTextColor(dc, kText);
			DrawTextW(dc, text.c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
			y += S(21);
		};
		line(L.ok, L.ok ? L"Shogo: " + L.gameDir : L"Shogo with the VR mod wasn't found - please run ShogoVR-Setup.exe");
		line(!L.runtime.empty(), L.runtime.empty() ? L"No OpenXR runtime set - in SteamVR: Settings > OpenXR > Set SteamVR as OpenXR Runtime"
												   : L"Headset runtime: " + L.runtime);
		line(L.ok, L.modInfo);
	}

	// Footer.
	RECT line = { 0, rc.bottom - L.footH, rc.right, rc.bottom - L.footH + 1 };
	FillRectColor(dc, line, RGB(220, 220, 224));
	if (L.page == PAGE_SETTINGS || L.page == PAGE_PICTURE || L.page == PAGE_BUTTONS) return;		// "Reset to defaults" lives there
	RECT foot = { S(20), rc.bottom - L.footH, rc.right / 2, rc.bottom };
	SelectObject(dc, L.small);
	SetTextColor(dc, kMuted);
	std::wstring footText = L"Shogo VR " SHOGOVR_VERSION_STR L"  \u00B7  unofficial fan-made VR mod  \u00B7  free";
	DrawTextW(dc, footText.c_str(), -1, &foot, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
}

static LRESULT CALLBACK LauncherProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	LauncherUI& L = *g_L;
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
		HDC mem = CreateCompatibleDC(dc);							// double-buffered: no flicker
		HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
		HGDIOBJ old = SelectObject(mem, bmp);
		PaintLauncher(hwnd, mem);
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
		HDC dc = (HDC)wp;
		SetBkColor(dc, gui::kBodyBg);
		SetTextColor(dc, (HWND)lp == L.about ? gui::kText : gui::kText);
		return (LRESULT)body;
	}
	case WM_DRAWITEM:
	{
		const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lp;
		gui::DrawButton(di, di->CtlID == IDC_PLAY, di->CtlID == IDC_PLAY ? L.bold : L.font);
		return TRUE;
	}
	case WM_NOTIFY:
	{
		const NMHDR* nm = (const NMHDR*)lp;
		if (nm->idFrom == IDC_TABS && nm->code == TCN_SELCHANGE) ShowPage(L, TabCtrl_GetCurSel(L.tabs));
		return 0;
	}
	case WM_COMMAND:
		if (SettingsHandleCommand(L.settings, wp)) return 0;
		if (SettingsHandleCommand(L.picture, wp)) return 0;
		if (BindingsHandleCommand(L.bindings, wp)) return 0;
		switch (LOWORD(wp))
		{
		case IDC_PLAY:			if (L.ok) { L.play = true; DestroyWindow(hwnd); } return 0;
		case IDC_CLOSE:			DestroyWindow(hwnd); return 0;
		case IDC_RESET:
			if (L.page == PAGE_BUTTONS) ResetBindings(L.bindings);
			else if (L.page == PAGE_PICTURE) ResetDefaults(L.picture);
			else ResetDefaults(L.settings);
			return 0;
		case IDC_OPEN_SHOGO:
		{
			std::wstring exe = JoinPath(L.gameDir, L"Shogo.exe");
			if (FileExists(exe)) ShellExecuteW(hwnd, L"open", exe.c_str(), nullptr, L.gameDir.c_str(), SW_SHOWNORMAL);
			else Message(L"Shogo.exe wasn't found in " + L.gameDir, MB_ICONWARNING);
			return 0;
		}
		case IDC_OPEN_FOLDER:	ShellExecuteW(hwnd, L"open", L.modDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return 0;
		}
		return 0;
	case WM_HSCROLL:
		SettingsHandleScroll(L.settings, lp);
		SettingsHandleScroll(L.picture, lp);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

// Returns true if the player pressed Play.
static bool RunLauncherWindow(const std::wstring& exeDir, int startPage)
{
	using gui::S;
	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES };
	InitCommonControlsEx(&icc);
	gui::InitDpi();

	LauncherUI L;
	g_L = &L;
	L.ok = ResolveDirs(exeDir, L.gameDir, L.modDir);
	if (!L.ok) { L.gameDir = DirOf(exeDir); L.modDir = exeDir; }
	L.authors = gui::ReadAuthors(JoinPath(L.modDir, L"AUTHORS.txt"));
	L.runtime = RuntimeName();
	{
		WIN32_FILE_ATTRIBUTE_DATA fa;
		SYSTEMTIME st;
		if (GetFileAttributesExW(JoinPath(L.modDir, L"CShell.dll").c_str(), GetFileExInfoStandard, &fa) &&
			FileTimeToSystemTime(&fa.ftLastWriteTime, &st))
		{
			wchar_t buf[128];
			swprintf(buf, 128, L"VR mod installed (Shogo VR %ls, game code from %04u-%02u-%02u)", SHOGOVR_VERSION_STR, st.wYear, st.wMonth, st.wDay);
			L.modInfo = buf;
		}
		else L.modInfo = L"The VR mod's CShell.dll is missing - please run ShogoVR-Setup.exe";
	}

	L.font = gui::MakeFont(9, false);
	L.bold = gui::MakeFont(10, true);
	L.small = gui::MakeFont(8, false);
	L.head = gui::MakeFont(9, true);

	L.W = S(940);
	L.headerH = S(96);
	L.tabsH = S(30);
	L.footH = S(58);
	L.header = gui::LoadImageResource(IDR_HEADER_PNG, L.W - S(80), L.headerH - S(18));
	int pageTop = L.headerH + L.tabsH + S(4);
	int pageH = S(436);
	L.H = pageTop + pageH + L.footH;
	L.pageRect = { 0, pageTop, L.W, pageTop + pageH };
	L.banner = gui::LoadImageResource(IDR_BANNER_JPG, L.W - S(48), pageH - S(160));

	WNDCLASSW wc = {};
	wc.lpfnWndProc = LauncherProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
	wc.lpszClassName = L"ShogoVRLauncher";
	RegisterClassW(&wc);

	RECT rc = { 0, 0, L.W, L.H };
	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
	AdjustWindowRect(&rc, style, FALSE);
	HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Shogo VR", style, CW_USEDEFAULT, CW_USEDEFAULT,
								rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance, nullptr);
	if (!hwnd) return false;

	auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD st, int x, int y, int w, int h, int id, HFONT f) -> HWND
	{
		HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | st, x, y, w, h, hwnd, (HMENU)(INT_PTR)id, wc.hInstance, nullptr);
		SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
		return c;
	};

	L.tabs = make(WC_TABCONTROLW, L"", WS_CLIPSIBLINGS, S(16), L.headerH + S(6), L.W - S(32), L.tabsH, IDC_TABS, L.font);
	const wchar_t* names[] = { L"  Play  ", L"  VR Settings  ", L"  Picture  ", L"  Controller Buttons  ", L"  About && Legal  " };
	for (int i = 0; i < 5; ++i)
	{
		TCITEMW it = {};
		it.mask = TCIF_TEXT;
		it.pszText = (LPWSTR)names[i];
		TabCtrl_InsertItem(L.tabs, i, &it);
	}
	RECT tabItem = {};
	TabCtrl_GetItemRect(L.tabs, 0, &tabItem);			// just the tabs: the frame's top edge is the separator
	SetWindowPos(L.tabs, nullptr, 0, 0, L.W - S(32), tabItem.bottom + 1, SWP_NOMOVE | SWP_NOZORDER);

	int by = L.H - L.footH + (L.footH - S(36)) / 2;
	L.closeBtn = make(L"BUTTON", L"Close", BS_OWNERDRAW | WS_TABSTOP, L.W - S(20) - S(110), by, S(110), S(36), IDC_CLOSE, L.font);
	L.playBtn = make(L"BUTTON", L"Play Shogo VR", BS_OWNERDRAW | WS_TABSTOP, L.W - S(20) - S(110) - S(12) - S(200), by, S(200), S(36), IDC_PLAY, L.bold);
	EnableWindow(L.playBtn, L.ok);

	int pby = L.pageRect.bottom - S(40);
	L.openShogo = make(L"BUTTON", L"Open Shogo's own launcher", BS_OWNERDRAW | WS_TABSTOP, L.W - S(24) - S(220), pby - S(46), S(220), S(32), IDC_OPEN_SHOGO, L.font);
	L.openFolder = make(L"BUTTON", L"Open the ShogoVR folder", BS_OWNERDRAW | WS_TABSTOP, L.W - S(24) - S(220), pby - S(8), S(220), S(32), IDC_OPEN_FOLDER, L.font);

	CreateSettingsPanel(L.settings, hwnd, L.font, L.head, S(24), L.pageRect.top + S(8), L.W - S(48), L.modDir, 0);
	CreateSettingsPanel(L.picture, hwnd, L.font, L.head, S(24), L.pageRect.top + S(8), L.W - S(48), L.modDir, 1);
	CreateBindingsPanel(L.bindings, hwnd, L.font, L.head, S(24), L.pageRect.top + S(12), L.W - S(48), L.modDir);
	L.resetBtn = make(L"BUTTON", L"Reset to defaults", BS_OWNERDRAW | WS_TABSTOP, S(20), by, S(160), S(36), IDC_RESET, L.font);

	L.about = make(L"EDIT", gui::AttributionText(L.authors).c_str(),
				   ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER,
				   S(24), L.pageRect.top + S(8), L.W - S(48), pageH - S(16), IDC_ABOUT_TEXT, L.font);
	SendMessageW(L.about, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(10), S(10)));

	ShowPage(L, startPage);
	ShowWindow(hwnd, SW_SHOW);
	UpdateWindow(hwnd);

	MSG m;
	while (GetMessageW(&m, nullptr, 0, 0) > 0)
	{
		if (!IsDialogMessageW(hwnd, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
	}
	L.header.Free();
	L.banner.Free();
	for (HFONT f : { L.font, L.bold, L.small, L.head }) DeleteObject(f);
	g_L = nullptr;
	return L.play;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	bool bUninstall = false, bQuiet = false, bSettings = false, bPlay = false;
	for (int i = 1; i < argc; ++i)
	{
		if (_wcsicmp(argv[i], L"--uninstall") == 0) bUninstall = true;
		if (_wcsicmp(argv[i], L"--quiet") == 0) bQuiet = true;
		if (_wcsicmp(argv[i], L"--settings") == 0) bSettings = true;
		if (_wcsicmp(argv[i], L"--play") == 0) bPlay = true;
		if (_wcsicmp(argv[i], L"--yes") == 0) g_assumeYes = true;
	}
	LocalFree(argv);

	std::wstring exeDir = DirOf(ExePath());
	if (bUninstall) return Uninstall(exeDir, bQuiet);

	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	bool go = bPlay || RunLauncherWindow(exeDir, bSettings ? PAGE_SETTINGS : PAGE_PLAY);
	int ret = 0;
	if (go)
	{
		std::wstring gameDir, modDir;
		if (!ResolveDirs(exeDir, gameDir, modDir))
		{
			Message(L"Couldn't find Shogo with the VR mod installed.\n\nPlease run ShogoVR-Setup.exe.", MB_ICONERROR);
			ret = 1;
		}
		else ret = Play(gameDir, modDir);
	}
	CoUninitialize();
	return ret;
}
