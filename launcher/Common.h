// ======================================================================= //
//
// Common.h  -  shared by ShogoVR.exe (launcher) and ShogoVR-Setup.exe
//
//   * A simple payload format: files appended to the end of an .exe,
//     followed by an index and a footer.  The setup program carries the mod
//     files this way, so it can be a single download.
//   * Finding the Shogo installation: Steam libraries, GOG Galaxy's
//     registry entries and a few common folders.  A folder counts as Shogo
//     when it holds Client.exe and SHOGO.REZ.
//
// ======================================================================= //

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <tlhelp32.h>

#define SHOGOVR_PRODUCT			L"Shogo VR"
#define SHOGOVR_UNINSTALL_KEY	L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ShogoVR"
#define SHOGOVR_MOD_FOLDER		L"ShogoVR"
#define SHOGOVR_VERSION_STR		L"1.0"

// ======================================================================= //
//  Small helpers
// ======================================================================= //

static std::wstring ExePath()
{
	wchar_t buf[4096];
	DWORD n = GetModuleFileNameW(nullptr, buf, 4096);
	return std::wstring(buf, n);
}

static std::wstring DirOf(const std::wstring& path)
{
	size_t slash = path.find_last_of(L"\\/");
	return (slash == std::wstring::npos) ? std::wstring() : path.substr(0, slash);
}

static std::wstring JoinPath(const std::wstring& a, const std::wstring& b)
{
	if (a.empty()) return b;
	wchar_t last = a[a.size() - 1];
	return (last == L'\\' || last == L'/') ? a + b : a + L"\\" + b;
}

static bool FileExists(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool DirExists(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
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

static bool ReadWholeFile(const std::wstring& path, std::vector<char>& data)
{
	HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (f == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size;
	if (!GetFileSizeEx(f, &size) || size.QuadPart > 0x7fffffff) { CloseHandle(f); return false; }
	data.resize((size_t)size.QuadPart);
	DWORD got = 0;
	bool ok = data.empty() || (ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr) && got == data.size());
	CloseHandle(f);
	return ok;
}

static bool WriteWholeFile(const std::wstring& path, const char* data, size_t size)
{
	HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return false;
	DWORD put = 0;
	bool ok = size == 0 || (WriteFile(f, data, (DWORD)size, &put, nullptr) && put == size);
	CloseHandle(f);
	return ok;
}

static std::wstring RegReadString(HKEY root, const std::wstring& key, const std::wstring& value, REGSAM extra = 0)
{
	HKEY h = nullptr;
	if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ | extra, &h) != ERROR_SUCCESS) return L"";
	wchar_t buf[4096];
	DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
	std::wstring out;
	if (RegQueryValueExW(h, value.c_str(), nullptr, &type, (BYTE*)buf, &size) == ERROR_SUCCESS &&
		(type == REG_SZ || type == REG_EXPAND_SZ))
	{
		buf[size / sizeof(wchar_t)] = 0;
		out = buf;
	}
	RegCloseKey(h);
	return out;
}

// ======================================================================= //
//  Payload: [stub exe][file data...][index][footer]
// ======================================================================= //

static const char k_PakMagic[16] = "SHOGOVR-PAK-v1";	// 14 chars + NULs

#pragma pack(push, 1)
struct PakFooter
{
	char		magic[16];
	uint64_t	stubSize;		// bytes of the original program before the payload
	uint64_t	indexOffset;
	uint64_t	indexSize;
};
#pragma pack(pop)

struct PakFile
{
	std::string			name;	// e.g. "ShogoVR/CShell.dll" (forward slashes)
	std::vector<char>	data;
};

// Reads the files carried at the end of an exe.  Returns false if there are
// none (stubSize is then the whole file).
static bool PakRead(const std::vector<char>& exe, uint64_t& stubSize, std::vector<PakFile>& files)
{
	files.clear();
	stubSize = exe.size();
	if (exe.size() < sizeof(PakFooter)) return false;

	PakFooter foot;
	memcpy(&foot, exe.data() + exe.size() - sizeof(PakFooter), sizeof(foot));
	if (memcmp(foot.magic, k_PakMagic, sizeof(k_PakMagic)) != 0) return false;
	if (foot.stubSize > exe.size() || foot.indexOffset + foot.indexSize + sizeof(PakFooter) != exe.size()) return false;

	const char* p   = exe.data() + foot.indexOffset;
	const char* end = p + foot.indexSize;
	auto take = [&](void* out, size_t n) { if (p + n > end) return false; memcpy(out, p, n); p += n; return true; };

	uint32_t count = 0;
	if (!take(&count, 4) || count > 1000) return false;
	for (uint32_t i = 0; i < count; ++i)
	{
		uint32_t nameLen = 0;
		uint64_t off = 0, size = 0;
		if (!take(&nameLen, 4) || nameLen > 1024 || p + nameLen > end) return false;
		PakFile f;
		f.name.assign(p, nameLen);
		p += nameLen;
		if (!take(&off, 8) || !take(&size, 8)) return false;
		if (off < foot.stubSize || off + size > foot.indexOffset) return false;
		f.data.assign(exe.data() + off, exe.data() + off + size);
		files.push_back(std::move(f));
	}
	stubSize = foot.stubSize;
	return true;
}

// Writes stub + files + index + footer to outPath.
static bool PakWrite(const std::wstring& outPath, const std::vector<char>& exe, uint64_t stubSize,
					 const std::vector<PakFile>& files)
{
	std::vector<char> out(exe.begin(), exe.begin() + (size_t)stubSize);
	std::vector<char> index;
	auto put = [&](std::vector<char>& v, const void* d, size_t n) { v.insert(v.end(), (const char*)d, (const char*)d + n); };

	uint32_t count = (uint32_t)files.size();
	put(index, &count, 4);
	for (const PakFile& f : files)
	{
		uint64_t off = out.size(), size = f.data.size();
		put(out, f.data.data(), f.data.size());
		uint32_t nameLen = (uint32_t)f.name.size();
		put(index, &nameLen, 4);
		put(index, f.name.data(), nameLen);
		put(index, &off, 8);
		put(index, &size, 8);
	}

	PakFooter foot = {};
	memcpy(foot.magic, k_PakMagic, sizeof(k_PakMagic));
	foot.stubSize = stubSize;
	foot.indexOffset = out.size();
	foot.indexSize = index.size();
	put(out, index.data(), index.size());
	put(out, &foot, sizeof(foot));
	return WriteWholeFile(outPath, out.data(), out.size());
}

static const PakFile* PakFind(const std::vector<PakFile>& files, const char* name)
{
	for (const PakFile& f : files) if (_stricmp(f.name.c_str(), name) == 0) return &f;
	return nullptr;
}

// ======================================================================= //
//  Finding Shogo
// ======================================================================= //

static bool IsShogoDir(const std::wstring& dir)
{
	return !dir.empty() && FileExists(JoinPath(dir, L"Client.exe")) && FileExists(JoinPath(dir, L"SHOGO.REZ"));
}

// Accepts the folder itself, a folder one level above it (e.g. steamapps\common)
// or one level below it (e.g. the game's Custom folder).
static std::wstring ResolveShogoDir(const std::wstring& picked)
{
	if (IsShogoDir(picked)) return picked;

	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(JoinPath(picked, L"*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
		{
			if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
			{
				std::wstring sub = JoinPath(picked, fd.cFileName);
				if (IsShogoDir(sub)) { FindClose(h); return sub; }
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}

	std::wstring parent = DirOf(picked);
	if (IsShogoDir(parent)) return parent;
	return L"";
}

// Steam: the main install plus every library listed in libraryfolders.vdf.
static std::vector<std::wstring> SteamLibraries()
{
	std::vector<std::wstring> libs;
	std::wstring steam = RegReadString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
	if (steam.empty()) steam = RegReadString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", KEY_WOW64_32KEY);
	if (steam.empty()) return libs;
	std::replace(steam.begin(), steam.end(), L'/', L'\\');
	libs.push_back(steam);

	std::vector<char> vdf;
	if (!ReadWholeFile(JoinPath(steam, L"steamapps\\libraryfolders.vdf"), vdf)) return libs;
	std::string text(vdf.begin(), vdf.end());

	size_t pos = 0;
	while ((pos = text.find("\"path\"", pos)) != std::string::npos)
	{
		pos += 6;
		size_t q1 = text.find('"', pos);
		if (q1 == std::string::npos) break;
		std::string val;
		size_t i = q1 + 1;
		for (; i < text.size() && text[i] != '"'; ++i)
		{
			if (text[i] == '\\' && i + 1 < text.size()) ++i;	// "\\" -> "\"
			val += text[i];
		}
		pos = i + 1;
		std::wstring lib = Utf8ToWide(val);
		bool dup = false;
		for (auto& l : libs) if (_wcsicmp(l.c_str(), lib.c_str()) == 0) dup = true;
		if (!lib.empty() && !dup) libs.push_back(lib);
	}
	return libs;
}

// GOG Galaxy: every installed game has a subkey with a "path" value.  Check
// them all rather than guessing Shogo's product id.
static std::vector<std::wstring> GogInstalls()
{
	std::vector<std::wstring> out;
	HKEY games = nullptr;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GOG.com\\Games", 0, KEY_READ | KEY_WOW64_32KEY, &games) != ERROR_SUCCESS)
		return out;
	wchar_t name[256];
	for (DWORD i = 0; ; ++i)
	{
		DWORD len = 256;
		if (RegEnumKeyExW(games, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
		std::wstring p = RegReadString(games, name, L"path");
		if (!p.empty()) out.push_back(p);
	}
	RegCloseKey(games);
	return out;
}

static std::wstring LocateShogo()
{
	// Already installed?  Then the same place again.
	std::wstring prev = RegReadString(HKEY_CURRENT_USER, SHOGOVR_UNINSTALL_KEY, L"InstallLocation");
	if (IsShogoDir(prev)) return prev;

	for (const std::wstring& lib : SteamLibraries())
	{
		std::wstring common = JoinPath(lib, L"steamapps\\common");
		std::wstring p = JoinPath(common, L"Shogo Mobile Armor Division");
		if (IsShogoDir(p)) return p;
		p = ResolveShogoDir(common);		// any folder under common that holds Shogo
		if (!p.empty()) return p;
	}
	for (const std::wstring& p : GogInstalls()) if (IsShogoDir(p)) return p;

	const wchar_t* candidates[] = {
		L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Shogo Mobile Armor Division",
		L"C:\\Program Files\\Steam\\steamapps\\common\\Shogo Mobile Armor Division",
		L"C:\\GOG Games\\Shogo", L"C:\\GOG Games\\Shogo Mobile Armor Division",
		L"C:\\Games\\Shogo", L"C:\\Program Files (x86)\\Monolith\\Shogo", L"C:\\Shogo" };
	for (const wchar_t* c : candidates) if (IsShogoDir(c)) return c;
	return L"";
}

// The -rez list Client.exe needs: the base game's archives (it refuses to
// start without them), official map packs, then the VR mod folder last so
// its CShell.dll wins.
static std::wstring BuildGameArgs(const std::wstring& gameDir, const std::wstring& extra)
{
	std::wstring args;
	auto addRez = [&](const std::wstring& rel)
	{
		if (FileExists(JoinPath(gameDir, rel)) || DirExists(JoinPath(gameDir, rel)))
		{
			if (!args.empty()) args += L" ";
			args += L"-rez \"" + rel + L"\"";
		}
	};
	// The same shape as Shogo's own launcher: window title, the base game,
	// the Custom folder, the map packs, single player.
	args = L"-windowtitle Shogo";
	addRez(L"SHOGO.REZ");
	addRez(L"SOUND.REZ");
	addRez(L"Custom");

	std::vector<std::wstring> packs;
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(JoinPath(gameDir, L"SHOGOP*.REZ").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do { packs.push_back(fd.cFileName); } while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	std::sort(packs.begin(), packs.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
	for (auto& p : packs) addRez(p);

	args += L" +multiplayer 0";
	addRez(SHOGOVR_MOD_FOLDER);
	if (!extra.empty()) args += L" " + extra;
	return args;
}

// ======================================================================= //
//  Shortcuts (Desktop / Start menu)
// ======================================================================= //

#include <shobjidl.h>
#include <objbase.h>
#include <knownfolders.h>

static std::wstring KnownFolder(REFKNOWNFOLDERID id)
{
	PWSTR p = nullptr;
	std::wstring out;
	if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
	if (p) CoTaskMemFree(p);
	return out;
}

static std::wstring ShortcutPath(REFKNOWNFOLDERID folder, const wchar_t* name = L"Shogo VR.lnk")
{
	std::wstring dir = KnownFolder(folder);
	return dir.empty() ? dir : JoinPath(dir, name);
}

static bool CreateShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& workDir,
						   const std::wstring& icon, const std::wstring& description, const std::wstring& args = L"")
{
	IShellLinkW* link = nullptr;
	if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&link))) return false;
	link->SetPath(target.c_str());
	if (!args.empty()) link->SetArguments(args.c_str());
	link->SetWorkingDirectory(workDir.c_str());
	link->SetDescription(description.c_str());
	if (!icon.empty()) link->SetIconLocation(icon.c_str(), 0);

	IPersistFile* file = nullptr;
	bool ok = false;
	if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&file)))
	{
		ok = SUCCEEDED(file->Save(lnk.c_str(), TRUE));
		file->Release();
	}
	link->Release();
	return ok;
}

static bool IsBridgeRunning()
{
	HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\ShogoVRBridge_Running");
	if (!m) return false;
	CloseHandle(m);
	return true;
}

// ======================================================================= //
//  dgVoodoo's video memory
// ======================================================================= //

// dgVoodoo pretends to be a graphics card with VRAM = 256 MB by default.
// At high resolutions (e.g. 3840x2160 for VR) the screen buffers alone use a
// big share of that, and LithTech then drops texture detail as levels load -
// everything gets blurrier over time.  Sets it to targetMB (or, with
// bOnlyRaise, raises it to at least that).
// Returns: 0 nothing to do / no dgVoodoo config, 1 changed, -1 couldn't write.
static int FixDgVoodooVram(const std::wstring& gameDir, int targetMB, bool bOnlyRaise = true, std::wstring* pChangedFile = nullptr)
{
	std::vector<std::wstring> confs;
	confs.push_back(JoinPath(gameDir, L"dgVoodoo.conf"));
	wchar_t appData[MAX_PATH] = L"";
	if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH)) confs.push_back(JoinPath(appData, L"dgVoodoo\\dgVoodoo.conf"));

	for (const std::wstring& conf : confs)
	{
		std::vector<char> raw;
		if (!FileExists(conf) || !ReadWholeFile(conf, raw)) continue;
		std::string text(raw.begin(), raw.end());

		// Walk the lines: find VRAM inside [DirectX].
		bool bInDirectX = false;
		size_t pos = 0;
		while (pos < text.size())
		{
			size_t eol = text.find('\n', pos);
			if (eol == std::string::npos) eol = text.size();
			std::string line = text.substr(pos, eol - pos);
			size_t a = line.find_first_not_of(" \t");
			std::string t = (a == std::string::npos) ? "" : line.substr(a);
			if (!t.empty() && t.back() == '\r') t.pop_back();

			if (!t.empty() && t[0] == '[')
			{
				bInDirectX = (_strnicmp(t.c_str(), "[DirectX]", 9) == 0);
			}
			else if (bInDirectX && _strnicmp(t.c_str(), "VRAM", 4) == 0 && t.find('=') != std::string::npos)
			{
				std::string val = t.substr(t.find('=') + 1);
				size_t v0 = val.find_first_not_of(" \t");
				val = (v0 == std::string::npos) ? "" : val.substr(v0);
				int mb = atoi(val.c_str());
				if (val.find("GB") != std::string::npos || val.find("gb") != std::string::npos) mb *= 1024;
				if (bOnlyRaise ? (mb >= targetMB) : (mb == targetMB)) return 0;

				// Keep the original once, then rewrite just this value.
				std::wstring backup = conf + L".shogovr-backup";
				if (!FileExists(backup) && !WriteWholeFile(backup, raw.data(), raw.size())) return -1;
				size_t eq = line.find('=');
				std::string newLine = line.substr(0, eq + 1) + " " + std::to_string(targetMB);
				if (!line.empty() && line.back() == '\r') newLine += "\r";
				text.replace(pos, eol - pos, newLine);
				if (!WriteWholeFile(conf, text.data(), text.size())) return -1;
				if (pChangedFile) *pChangedFile = conf;
				return 1;
			}
			pos = eol + 1;
		}
		return 0;		// dgVoodoo's own config takes priority even without a VRAM line
	}
	return 0;
}

// Sets one option in the [DirectX] section of dgVoodoo's config - only that
// value is rewritten (the line is added if it's missing), and the original
// file is kept once as .shogovr-backup.  1 = changed, 0 = already so or no
// dgVoodoo config, -1 = couldn't write it.
static int SetDgVoodooDirectX(const std::wstring& gameDir, const char* key, const std::string& value)
{
	std::vector<std::wstring> confs;
	confs.push_back(JoinPath(gameDir, L"dgVoodoo.conf"));
	wchar_t appData[MAX_PATH] = L"";
	if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH)) confs.push_back(JoinPath(appData, L"dgVoodoo\\dgVoodoo.conf"));
	size_t keyLen = strlen(key);

	for (const std::wstring& conf : confs)
	{
		std::vector<char> raw;
		if (!FileExists(conf) || !ReadWholeFile(conf, raw)) continue;
		std::string text(raw.begin(), raw.end());
		std::string nl = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";

		bool bInDirectX = false;
		size_t afterHeader = std::string::npos, pos = 0;
		while (pos <= text.size())
		{
			size_t eol = text.find('\n', pos);
			if (eol == std::string::npos) eol = text.size();
			std::string line = text.substr(pos, eol - pos);
			size_t a = line.find_first_not_of(" \t");
			std::string t = (a == std::string::npos) ? "" : line.substr(a);
			if (!t.empty() && t.back() == '\r') t.pop_back();

			if (!t.empty() && t[0] == '[')
			{
				if (bInDirectX) break;		// end of [DirectX] without the key
				bInDirectX = (_strnicmp(t.c_str(), "[DirectX]", 9) == 0);
				if (bInDirectX) afterHeader = eol + 1;
			}
			else if (bInDirectX && _strnicmp(t.c_str(), key, keyLen) == 0 && t.size() > keyLen &&
					 (t[keyLen] == ' ' || t[keyLen] == '\t' || t[keyLen] == '='))
			{
				size_t eqT = t.find('=');
				if (eqT == std::string::npos) { pos = eol + 1; continue; }
				std::string cur = t.substr(eqT + 1);
				size_t v0 = cur.find_first_not_of(" \t");
				cur = (v0 == std::string::npos) ? "" : cur.substr(v0);
				while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\t')) cur.pop_back();
				if (_stricmp(cur.c_str(), value.c_str()) == 0) return 0;

				std::wstring backup = conf + L".shogovr-backup";
				if (!FileExists(backup) && !WriteWholeFile(backup, raw.data(), raw.size())) return -1;
				size_t eq = line.find('=');
				std::string newLine = line.substr(0, eq + 1) + " " + value;
				if (!line.empty() && line.back() == '\r') newLine += "\r";
				text.replace(pos, eol - pos, newLine);
				return WriteWholeFile(conf, text.data(), text.size()) ? 1 : -1;
			}
			if (eol >= text.size()) break;
			pos = eol + 1;
		}
		if (afterHeader == std::string::npos) return 0;		// no [DirectX] section: leave it

		std::wstring backup = conf + L".shogovr-backup";
		if (!FileExists(backup) && !WriteWholeFile(backup, raw.data(), raw.size())) return -1;
		text.insert(std::min(afterHeader, text.size()), std::string(key) + " = " + value + nl);
		return WriteWholeFile(conf, text.data(), text.size()) ? 1 : -1;
	}
	return 0;
}

// ======================================================================= //
//  Running copies of the game
// ======================================================================= //

// Client.exe processes started from this game folder (e.g. one left behind
// by a crash, still held by Windows Error Reporting).
static std::vector<DWORD> FindGameProcesses(const std::wstring& gameDir)
{
	std::vector<DWORD> pids;
	std::wstring want = JoinPath(gameDir, L"Client.exe");
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return pids;
	PROCESSENTRY32W pe = { sizeof(pe) };
	for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
	{
		if (_wcsicmp(pe.szExeFile, L"Client.exe") != 0) continue;
		HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
		if (!h) { pids.push_back(pe.th32ProcessID); continue; }		// can't check its path: count it
		wchar_t path[2048];
		DWORD len = 2048;
		if (QueryFullProcessImageNameW(h, 0, path, &len) && _wcsicmp(path, want.c_str()) == 0) pids.push_back(pe.th32ProcessID);
		CloseHandle(h);
	}
	CloseHandle(snap);
	return pids;
}

// Processes running a given program file (full path).
static std::vector<DWORD> FindProcessesByPath(const std::wstring& exePath)
{
	std::vector<DWORD> pids;
	size_t slash = exePath.find_last_of(L"\\/");
	std::wstring name = slash == std::wstring::npos ? exePath : exePath.substr(slash + 1);
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return pids;
	PROCESSENTRY32W pe = { sizeof(pe) };
	for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
	{
		if (_wcsicmp(pe.szExeFile, name.c_str()) != 0) continue;
		HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
		if (!h) continue;
		wchar_t path[2048];
		DWORD len = 2048;
		if (QueryFullProcessImageNameW(h, 0, path, &len) && _wcsicmp(path, exePath.c_str()) == 0) pids.push_back(pe.th32ProcessID);
		CloseHandle(h);
	}
	CloseHandle(snap);
	return pids;
}

static bool IsProcessElevated(HANDLE process)
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
