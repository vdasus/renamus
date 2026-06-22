// Renamus - visual group rename plugin for far2l
//
// Collects the names of the selected files on the active panel, opens them in
// the built-in editor (one name per line), and after the editor is closed
// renames every file whose line was changed. Inspired by the FAR plugin VisRen.
//
// Self-contained: depends only on the far2l SDK / WinPort headers. The
// WINPORT(MoveFile)/WINPORT(GetLastError) symbols are resolved from the far2l
// host process at load time.

#define _FAR_NO_NAMELESS_UNIONS
#include <farplug-wide.h>

#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

#include <cstdio>
#include <cstdlib>
#include <cstdarg>

#include <windows.h>

#include "Version.hpp"

// ---------------------------------------------------------------------------
// Minimal UTF-8 <-> wchar_t conversion (far2l wchar_t is 32-bit UTF-32).
// ---------------------------------------------------------------------------

static std::string WideToUtf8(const wchar_t *s)
{
	std::string out;
	for (; s && *s; ++s) {
		unsigned int c = (unsigned int)*s;
		if (c < 0x80) {
			out.push_back((char)c);
		} else if (c < 0x800) {
			out.push_back((char)(0xC0 | (c >> 6)));
			out.push_back((char)(0x80 | (c & 0x3F)));
		} else if (c < 0x10000) {
			out.push_back((char)(0xE0 | (c >> 12)));
			out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
			out.push_back((char)(0x80 | (c & 0x3F)));
		} else {
			out.push_back((char)(0xF0 | (c >> 18)));
			out.push_back((char)(0x80 | ((c >> 12) & 0x3F)));
			out.push_back((char)(0x80 | ((c >> 6) & 0x3F)));
			out.push_back((char)(0x80 | (c & 0x3F)));
		}
	}
	return out;
}

static std::wstring Utf8ToWide(const std::string &s)
{
	std::wstring out;
	size_t i = 0, n = s.size();
	while (i < n) {
		unsigned char b = (unsigned char)s[i];
		unsigned int c;
		int extra;
		if (b < 0x80) { c = b; extra = 0; }
		else if ((b >> 5) == 0x6) { c = b & 0x1F; extra = 1; }
		else if ((b >> 4) == 0xE) { c = b & 0x0F; extra = 2; }
		else if ((b >> 3) == 0x1E) { c = b & 0x07; extra = 3; }
		else { c = b; extra = 0; } // invalid lead byte: pass through
		i++;
		for (int k = 0; k < extra && i < n; k++, i++)
			c = (c << 6) | ((unsigned char)s[i] & 0x3F);
		out.push_back((wchar_t)c);
	}
	return out;
}

// ---------------------------------------------------------------------------

static struct PluginStartupInfo Info;
static struct FarStandardFunctions FSF;

// Optional diagnostics: set RENAMUS_DEBUG to a writable path to append a trace.
static void DBG(const char *fmt, ...)
{
	const char *path = getenv("RENAMUS_DEBUG");
	if (!path || !*path)
		return;
	FILE *f = fopen(path, "a");
	if (!f)
		return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::wstring GetPanelDir()
{
	int size = Info.Control(PANEL_ACTIVE, FCTL_GETPANELDIR, 0, 0);
	if (size <= 0)
		return std::wstring();
	std::vector<wchar_t> buf(size);
	Info.Control(PANEL_ACTIVE, FCTL_GETPANELDIR, size, (LONG_PTR)buf.data());
	return std::wstring(buf.data());
}

// Join a directory and a (possibly relative) name into a full path.
static std::wstring JoinPath(const std::wstring &dir, const std::wstring &name)
{
	if (!name.empty() && name[0] == L'/')
		return name; // already absolute
	std::wstring full = dir;
	if (!full.empty() && full.back() != L'/')
		full += L'/';
	full += name;
	return full;
}

static void ShowError(const wchar_t *line1, const wchar_t *line2)
{
	const wchar_t *items[] = {L"Renamus " RENAMUS_VERSION_W, line1, line2, L"OK"};
	Info.Message(Info.ModuleNumber, FMSG_WARNING, NULL, items, line2 ? 4 : 3, 1);
}

// Report a header line followed by a (possibly truncated) list of detail lines.
static void ShowReport(const wchar_t *header, const std::vector<std::wstring> &lines)
{
	static const size_t MAX_LINES = 15;
	std::vector<const wchar_t *> items;
	items.push_back(L"Renamus " RENAMUS_VERSION_W);
	items.push_back(header);
	std::wstring more;
	const size_t shown = std::min(lines.size(), MAX_LINES);
	for (size_t i = 0; i < shown; i++)
		items.push_back(lines[i].c_str());
	if (lines.size() > MAX_LINES) {
		more = L"... and " + std::to_wstring(lines.size() - MAX_LINES) + L" more";
		items.push_back(more.c_str());
	}
	items.push_back(L"OK");
	Info.Message(Info.ModuleNumber, FMSG_WARNING, NULL, items.data(),
		(int)items.size(), 1);
}

// ---------------------------------------------------------------------------
// Core
// ---------------------------------------------------------------------------

static void DoRename()
{
	struct PanelInfo pi;
	if (!Info.Control(PANEL_ACTIVE, FCTL_GETPANELINFO, 0, (LONG_PTR)&pi))
		return;

	const std::wstring dir = GetPanelDir();

	// Collect the names of the selected files (skip the ".." entry).
	std::vector<std::wstring> names;
	if (pi.SelectedItemsNumber > 0) {
		for (int i = 0; i < pi.SelectedItemsNumber; i++) {
			int size = Info.Control(PANEL_ACTIVE, FCTL_GETSELECTEDPANELITEM, i, 0);
			if (size <= 0)
				continue;
			std::vector<char> raw(size);
			PluginPanelItem *ppi = (PluginPanelItem *)raw.data();
			Info.Control(PANEL_ACTIVE, FCTL_GETSELECTEDPANELITEM, i, (LONG_PTR)ppi);
			const wchar_t *n = ppi->FindData.lpwszFileName;
			if (n && wcscmp(n, L"..") != 0)
				names.push_back(n);
		}
	}

	if (names.empty()) {
		ShowError(L"No files selected", NULL);
		return;
	}

	// Create a temp file holding one name per line (UTF-8).
	wchar_t tmpW[MAX_PATH];
	if (!FSF.MkTemp(tmpW, ARRAYSIZE(tmpW), L"renamus"))
		return;
	const std::string tmpMB = WideToUtf8(tmpW);

	{
		std::ofstream out(tmpMB.c_str(), std::ios::binary | std::ios::trunc);
		if (!out) {
			ShowError(L"Cannot create temp file", tmpW);
			return;
		}
		for (const auto &n : names)
			out << WideToUtf8(n.c_str()) << '\n';
	}

	DBG("DoRename v%s: names=%zu dir=%s tmp=%s", RENAMUS_VERSION, names.size(),
		WideToUtf8(dir.c_str()).c_str(), tmpMB.c_str());

	// Edit the list. The editor call is modal: it returns once closed.
	int ed = Info.Editor(tmpW, L"Renamus " RENAMUS_VERSION_W, 0, 0, -1, -1,
		EF_DISABLEHISTORY, 1, 1, CP_UTF8);
	DBG("DoRename: editor returned %d", ed);

	// Read the edited names back.
	std::vector<std::wstring> edited;
	{
		std::ifstream in(tmpMB.c_str(), std::ios::binary);
		std::string line;
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			edited.push_back(Utf8ToWide(line));
		}
	}
	::remove(tmpMB.c_str());

	// Drop a single trailing empty line the editor may have appended.
	if (edited.size() == names.size() + 1 && edited.back().empty())
		edited.pop_back();

	if (edited.size() != names.size()) {
		ShowError(L"Line count changed - nothing renamed.",
			L"Do not add or remove lines.");
		return;
	}

	int renamed = 0;
	std::vector<std::wstring> failures;
	for (size_t i = 0; i < names.size(); i++) {
		if (edited[i].empty() || edited[i] == names[i])
			continue;
		const std::wstring oldFull = JoinPath(dir, names[i]);
		const std::wstring newFull = JoinPath(dir, edited[i]);
		BOOL ok = MoveFile(oldFull.c_str(), newFull.c_str());
		DWORD err = ok ? 0 : GetLastError();
		DBG("MoveFile '%s' -> '%s' = %d (err %u)", WideToUtf8(oldFull.c_str()).c_str(),
			WideToUtf8(newFull.c_str()).c_str(), (int)ok, err);
		if (ok) {
			renamed++;
		} else {
			const wchar_t *why = (err == ERROR_ALREADY_EXISTS) ? L"target exists"
				: (err == ERROR_PATH_NOT_FOUND || err == ERROR_FILE_NOT_FOUND) ? L"path not found"
				: L"error";
			failures.push_back(names[i] + L" -> " + edited[i] + L"  (" + why + L")");
		}
	}

	Info.Control(PANEL_ACTIVE, FCTL_UPDATEPANEL, 1, 0);
	Info.Control(PANEL_ACTIVE, FCTL_REDRAWPANEL, 0, 0);

	if (!failures.empty()) {
		wchar_t header[64];
		FSF.snprintf(header, ARRAYSIZE(header), L"%d renamed, %d failed:",
			renamed, (int)failures.size());
		ShowReport(header, failures);
	}
}

// ---------------------------------------------------------------------------
// Plugin exports
// ---------------------------------------------------------------------------

SHAREDSYMBOL int WINAPI EXP_NAME(GetMinFarVersion)()
{
	return FARMANAGERVERSION;
}

SHAREDSYMBOL void WINAPI EXP_NAME(SetStartupInfo)(const struct PluginStartupInfo *psi)
{
	Info = *psi;
	FSF = *psi->FSF;
	Info.FSF = &FSF;
}

SHAREDSYMBOL HANDLE WINAPI EXP_NAME(OpenPlugin)(int OpenFrom, INT_PTR Item)
{
	DoRename();
	return INVALID_HANDLE_VALUE;
}

SHAREDSYMBOL void WINAPI EXP_NAME(GetPluginInfo)(struct PluginInfo *pi)
{
	pi->StructSize = sizeof(*pi);
	pi->Flags = 0;
	static const wchar_t *menu[] = {L"Renamus"};
	pi->PluginMenuStrings = menu;
	pi->PluginMenuStringsNumber = ARRAYSIZE(menu);
	pi->SysID = 0x52454E41; // 'RENA'
}
