//---------------------------------------------------------------------------
// FontDiscovery.cpp — where the engine's font files come from.
//
// Historically the engine found fonts by shelling out to `find` over a list of
// hardcoded Linux paths, then registered every file it saw by face name. That
// neither worked off Linux nor resolved the Windows face names KiriKiri games
// ask for (MS Gothic and friends): discovery was a directory listing and
// matching was exact-name only.
//
// This module owns discovery only; matching/aliasing stays in FreeType.cpp
// where the face lists live. Two backends:
//
//   * fontconfig (Linux/BSD) — the system's own font database, so user font
//     directories, aliases and substitutions are honoured with no directory
//     list to maintain. Compiled in when libfontconfig is available
//     (KRKRSDL2_HAVE_FONTCONFIG); fontconfig is already a dependency of the
//     Recalbox image, so this is the normal path there.
//
//   * directory scan — a portable walker (dirent / Win32) over an ordered,
//     overridable list of directories. Used on hosts without fontconfig, and
//     whenever KRKRSDL2_FONT_PATH selects an explicit list.
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "FontDiscovery.h"

#include <vector>
#include <string>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#if defined(KRKRSDL2_HAVE_FONTCONFIG)
#include <fontconfig/fontconfig.h>
#endif

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

namespace {

bool HasFontExtension(const std::string &name)
{
	if (name.size() < 5) return false;
	std::string lower = name;
	for (char &c : lower) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
	for (const char *ext : {".ttf", ".ttc", ".otf"})
		if (lower.compare(lower.size() - 4, 4, ext) == 0) return true;
	return false;
}

/* Recursively collect font files under `dir`. Symlinked directories are not
 * followed: font trees can be self-referential. */
void ScanDirImpl(const std::string &dir, std::vector<std::string> &out)
{
#ifdef _WIN32
	WIN32_FIND_DATAA fd;
	std::string pat = dir + "\\*";
	HANDLE h = FindFirstFileA(pat.c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		if (fd.cFileName[0] == '.') continue;
		std::string full = dir + "\\" + fd.cFileName;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { ScanDirImpl(full, out); continue; }
		if (HasFontExtension(fd.cFileName)) out.push_back(full);
	} while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	DIR *dp = opendir(dir.c_str());
	if (!dp) return;
	struct dirent *ent;
	while ((ent = readdir(dp)) != NULL) {
		if (ent->d_name[0] == '.') continue;
		std::string full = dir + "/" + ent->d_name;
		struct stat st;
		if (stat(full.c_str(), &st) != 0) continue;
		if (S_ISDIR(st.st_mode)) { ScanDirImpl(full, out); continue; }
		if (HasFontExtension(ent->d_name)) out.push_back(full);
	}
	closedir(dp);
#endif
}

/* Split a colon-separated directory list. */
std::vector<std::string> SplitPathList(const char *p)
{
	std::vector<std::string> dirs;
	while (p && *p) {
		const char *sep = strchr(p, ':');
		std::string one(p, sep ? (size_t)(sep - p) : strlen(p));
		if (!one.empty()) dirs.push_back(one);
		if (!sep) break;
		p = sep + 1;
	}
	return dirs;
}

} // namespace

//---------------------------------------------------------------------------
std::vector<std::string> TVPListFontFiles()
{
	std::vector<std::string> out;

	/* An explicit path list wins over every backend: it is how a packaged
	 * install points at its own font directory, and how tests isolate the
	 * environment from the host's fonts. */
	const char *override = getenv("KRKRSDL2_FONT_PATH");
	if (override && *override) {
		std::vector<std::string> dirs = SplitPathList(override);
		for (size_t i = 0; i < dirs.size(); i++) ScanDirImpl(dirs[i], out);
		return out;
	}

#if defined(KRKRSDL2_HAVE_FONTCONFIG)
	/* Build the font database and install it as the current config, then list
	 * it. FcInit() alone does not necessarily materialise the font list, and a
	 * config that is not "current" is ignored by FcFontList. */
	FcInit();
	FcConfig *boot = FcInitLoadConfigAndFonts();
	if (boot) FcConfigSetCurrent(boot);
	FcConfig *cfg = FcConfigGetCurrent();
	const FcChar8 *cfgfile = cfg ? FcConfigGetFilename(cfg, NULL) : NULL;
	fprintf(stderr, "krkr fonts: fontconfig config='%s'\n", cfgfile ? (const char *)cfgfile : "(none)");
	if (cfg) {
		/* Enumerate the font sets the config knows about directly, rather than
		 * asking FcFontList to match a NULL pattern (which does not reliably
		 * enumerate every font). Application fonts include those added via
		 * FcConfigAppFontAddFile; system fonts are the installed set. */
		FcFontSet *sets[2] = { FcConfigGetFonts(cfg, FcSetSystem),
		                       FcConfigGetFonts(cfg, FcSetApplication) };
		for (int s = 0; s < 2; s++) {
			if (!sets[s]) continue;
			for (int i = 0; i < sets[s]->nfont; i++) {
				FcChar8 *file = NULL;
				if (FcPatternGetString(sets[s]->fonts[i], FC_FILE, 0, &file) == FcResultMatch && file)
					out.push_back(std::string((const char *)file));
			}
		}
	}
	if (!out.empty()) {
		fprintf(stderr, "krkr fonts: fontconfig reported %zu font file(s)\n", out.size());
		return out;
	}
	fprintf(stderr, "krkr fonts: fontconfig empty; falling back to a directory scan\n");
#endif

	/* Portable fallback: per-user dirs, then the platform's system dirs.
	 * Windows and macOS both keep fonts as ordinary files in well-known
	 * directories, so a directory walk finds them without needing GDI or
	 * CoreText enumeration. */
	std::vector<std::string> dirs;
	const char *home = getenv("HOME");
#if defined(_WIN32)
	/* Windows has no HOME; use the profile locations. */
	const char *localappdata = getenv("LOCALAPPDATA");
	if (localappdata && *localappdata)
		dirs.push_back(std::string(localappdata) + "\\Microsoft\\Windows\\Fonts");
	const char *appdata = getenv("APPDATA");
	if (appdata && *appdata)
		dirs.push_back(std::string(appdata) + "\\Microsoft\\Windows\\Fonts");
	char windir[MAX_PATH];
	if (GetWindowsDirectoryA(windir, sizeof(windir)))
		dirs.push_back(std::string(windir) + "\\Fonts");
#else
	if (home && *home) {
		dirs.push_back(std::string(home) + "/.fonts");
		dirs.push_back(std::string(home) + "/.local/share/fonts");
#if defined(__APPLE__)
		dirs.push_back(std::string(home) + "/Library/Fonts");
#endif
	}
#if defined(__APPLE__)
	dirs.push_back("/Library/Fonts");
	dirs.push_back("/Network/Library/Fonts");
	dirs.push_back("/System/Library/Fonts");
#else
	dirs.push_back("/usr/share/fonts");
	dirs.push_back("/usr/local/share/fonts");
#endif
#endif
	for (size_t i = 0; i < dirs.size(); i++) ScanDirImpl(dirs[i], out);
	return out;
}
