//---------------------------------------------------------------------------
// BuiltinPlugins.cpp — data-driven resource/plugin overrides.
//
// KiriKiri games ship Windows PE plugins (`*.dll`, `*.tpm`) and link them from
// TJS (@loadplugin / Plugins.link). A PE cannot be dlopen'd here, so the engine
// answers those loads itself. Which loads to answer, what to substitute, and
// which archive files are encrypted are all described by one flat file:
//
//     <KRKRSDL2_DATA_PATH>/overrides.txt
//
// Format — one record per line, whitespace-separated, `#` starts a comment:
//
//     <fileName>  <fileHash>  <replacementFile>  <needDecryption>
//
//   fileName         plugin / storage file name to match (or `-`). Matched on
//                    the base name, case-insensitively.
//   fileHash         XP3 per-file hash to match (or `-`), e.g. 0x1a2b3c4d.
//   replacementFile  `-`  -> keep the file as the game shipped it. For a plugin
//                            that means "supplied by the engine, do not load
//                            the PE" (a no-op link; the game's own scripts
//                            carry the behaviour, or the engine does).
//                    else -> path, relative to KRKRSDL2_DATA_PATH, whose bytes
//                            replace the game's file (a Linux .so for a plugin,
//                            or any other resource).
//   needDecryption   1 -> apply the game's archive decryption to this file.
//
// A line may set just the parts it needs and `-` the rest, e.g. a plugin:
//     cxdec.tpm    -   -   0
// or an encrypted archive file:
//     -   0x89abcdef   -   1
//
// Why a table rather than code: it is trivial to maintain, it covers resources
// other than plugins, and the same list documents what each game needs.
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "XP3Archive.h"
#include "DebugIntf.h"
#include "SysInitIntf.h"
#include "CharacterSet.h"
#include "PluginImpl.h"   /* iTVPFunctionExporter, TVPGetFunctionExporter */
#include "BuiltinShims.h" /* compiled-in shims: TVPParseBuiltinRef, TVPInstallBuiltinShim */

#include <SDL_loadso.h>   /* SDL_LoadObject / SDL_LoadFunction */
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>

//---------------------------------------------------------------------------
// Override database
//---------------------------------------------------------------------------
namespace {

struct OverrideRec {
	ttstr fileName;             // lower-cased base name (Unicode), empty if unused
	unsigned long fileHash;     // 0 if not matched by hash
	bool hasHash;
	std::string replacement;    // "" means `-` (keep as-is)
	bool needDecryption;
};

std::vector<OverrideRec> g_overrides;
bool g_overrides_loaded = false;

// Reduce a name to its base component as UTF-16. The engine speaks UTF-16
// internally and TJS2's ToLowerCase() does full Unicode case folding, so file
// names compare as codepoints (Shift-JIS names are already decoded to UTF-16 by
// the storage layer) rather than bytes.
ttstr UnicodeBaseName(const ttstr &in)
{
	const tjs_char *s = in.c_str();
	const tjs_char *leaf = s;
	for (const tjs_char *p = s; *p; p++)
		if (*p == TJS_W('/') || *p == TJS_W('\\')) leaf = p + 1;
	ttstr n(leaf);
	n.ToLowerCase();
	return n;
}

// Where overrides.txt is looked for, in order:
//
//   1. $KRKRSDL2_DATA_PATH/overrides.txt — the engine's own data dir, set by
//      the core from the frontend's system directory when one is available,
//      else the packaged location.
//   2. <first non-empty entry of $KRKRSDL2_OVERRIDE_PATH>/overrides.txt — an
//      extra search path, colon-separated. The core points this at the
//      frontend's system ("BIOS") directory, which is where a *user* override
//      belongs: it sits outside the immutable packaged install, survives a
//      package upgrade, and is the directory users already know to put
//      per-system files in.
//   3. the packaged default, /usr/share/krkrsdl2/overrides.txt.
//
// A file found in an earlier location wins outright rather than merging: a
// half-overridden table is harder to reason about than a whole one, and the
// per-record `-` fields already express "leave this alone".
static bool ReadWholeFile(const std::string &path, std::string *out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out->append(buf, n);
	fclose(f);
	return true;
}

std::string OverridesPath()
{
	// 1. The engine's data dir.
	const char *env = getenv("KRKRSDL2_DATA_PATH");
#ifdef _WIN32
	// Windows has no /usr/share. The core always sets DATA_PATH from the
	// frontend's system directory, so this fallback is only reached when a
	// frontend reports none; point it somewhere a Windows install could
	// plausibly have put the table rather than at a POSIX path.
	std::string base = env && *env ? env : "krkrsdl2";
#else
	std::string base = env && *env ? env : "/usr/share/krkrsdl2";
#endif
	if (!base.empty() && base[base.size() - 1] != '/') base += '/';
	std::string packaged = base + "overrides.txt";

	// 2. Extra search path (the frontend's system/BIOS directory).
	const char *extra = getenv("KRKRSDL2_OVERRIDE_PATH");
	if (extra && *extra)
	{
		std::string dirs(extra);
		size_t pos = 0;
		while (pos <= dirs.size())
		{
			// Windows uses ';' as the path-list separator; ':' is part of
			// every absolute path there (the drive colon). Splitting a Windows
			// path on ':' yields "C" and "\dir\...", so overrides.txt is never
			// found — and without it no plugin is recognised as a builtin, so
			// the engine tries to dlopen a Windows PE and dies.
#ifdef _WIN32
			const char listsep = ';';
#else
			const char listsep = ':';
#endif
			size_t sep = dirs.find(listsep, pos);
			std::string dir = dirs.substr(
				pos, sep == std::string::npos ? std::string::npos : sep - pos);
			pos = (sep == std::string::npos) ? dirs.size() + 1 : sep + 1;
			if (dir.empty())
				continue;
			if (dir[dir.size() - 1] != '/') dir += '/';
			std::string candidate = dir + "overrides.txt";
			std::string unused;
			if (ReadWholeFile(candidate, &unused))
			{
				TVPAddImportantLog(ttstr(TJS_W("(info) overrides.txt: using ")) +
					ttstr(candidate.c_str()));
				return candidate;
			}
		}
	}

	// 3. The data dir's own copy.
	return packaged;
}

const char *SkipWs(const char *p) { while (*p == ' ' || *p == '\t') p++; return p; }
std::string NextField(const char **pp)
{
	const char *p = SkipWs(*pp);
	std::string out;
	while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') out += *p++;
	*pp = p;
	return out;
}

void LoadOverrides()
{
	g_overrides_loaded = true;
	std::string path = OverridesPath();
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) {
		TVPAddImportantLog(ttstr(TJS_W("(info) overrides.txt not found; using defaults")));
		return;
	}
	std::string data;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
	fclose(f);

	size_t pos = 0, records = 0;
	while (pos <= data.size()) {
		size_t eol = data.find('\n', pos);
		std::string line = data.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
		pos = (eol == std::string::npos) ? data.size() + 1 : eol + 1;

		const char *p = line.c_str();
		p = SkipWs(p);
		if (!*p || *p == '#' || *p == '\r') continue;

		std::string fname  = NextField(&p);
		std::string fhash  = NextField(&p);
		std::string repl   = NextField(&p);
		std::string dec    = NextField(&p);
		if (fname.empty() || fhash.empty()) continue;

		OverrideRec r;
		r.hasHash = false; r.fileHash = 0; r.needDecryption = false;
		if (fname != "-") {
			// The table is UTF-8; convert to the engine's UTF-16 and fold case.
			tjs_string u16;
			TVPUtf8ToUtf16(u16, fname);
			r.fileName = UnicodeBaseName(ttstr(u16.c_str()));
		}
		if (fhash != "-") {
			r.hasHash = true;
			r.fileHash = (unsigned long)strtoul(fhash.c_str(), nullptr, 0);
		}
		if (repl != "-") r.replacement = repl;
		r.needDecryption = (dec == "1" || dec == "true" || dec == "yes");
		g_overrides.push_back(r);
		records++;
	}
	TVPAddImportantLog(ttstr(TJS_W("(info) overrides.txt: ")) + ttstr((tjs_int)records) +
		TJS_W(" record(s)"));
}

// A record matches only when every criterion it specifies is satisfied by the
// query. If the query cannot supply a criterion the record asks for, it does
// NOT match — absent information must never be treated as a wildcard, or a
// trivial file (a blank storage, a one-line config) could collide with another
// game's entry and be replaced or decrypted by mistake.
//   * record with name+hash -> query must supply both and match both
//   * record with name only -> query must match the name
//   * record with hash only -> query must match the hash
bool RecordMatches(const OverrideRec &r,
                   const ttstr *qname, bool qhasHash, unsigned long qhash)
{
	if (!r.fileName.IsEmpty()) {
		if (!qname || r.fileName != *qname) return false;
	}
	if (r.hasHash) {
		if (!qhasHash || r.fileHash != qhash) return false;
	}
	return true;
}

const std::vector<OverrideRec> &Overrides()
{
	if (!g_overrides_loaded) LoadOverrides();
	return g_overrides;
}

// Find a record by file name (plugins/resources), with the name it must match.
const OverrideRec *FindByName(const ttstr &name)
{
	ttstr base = UnicodeBaseName(name);
	for (const OverrideRec &r : Overrides())
		if (!r.fileName.IsEmpty() && RecordMatches(r, &base, false, 0)) return &r;
	return nullptr;
}

// Find a record by XP3 file hash. The extraction filter has no file name, so
// only records that specify a hash (and no name) can match here; a record that
// requires a name is deliberately skipped rather than guessed.
const OverrideRec *FindByHash(unsigned long hash)
{
	for (const OverrideRec &r : Overrides())
		if (r.hasHash && RecordMatches(r, nullptr, true, hash)) return &r;
	return nullptr;
}

//---------------------------------------------------------------------------
// Archive encryption filter
//
// The filter runs on every archive this engine reads, so it must not corrupt
// files it does not recognise. The overrides table states `needDecryption` per
// hash; for hashes not listed, the first bytes are inspected and the decision
// cached for the process (a file's later chunks must use the same transform its
// first chunk chose, and all archives of a game share one scheme).
//---------------------------------------------------------------------------
enum class ArchFormat { Unknown, Plain, FateXor };
ArchFormat g_arch_format = ArchFormat::Unknown;

inline bool LooksLikeHeader(const unsigned char *p, unsigned size, bool xored)
{
	if (size < 4) return false;
	unsigned char b[4];
	for (unsigned i = 0; i < 4; i++)
		b[i] = xored ? (unsigned char)(p[i] ^ 0x36) : p[i];

	if (b[0] == 'T' && b[1] == 'L' && b[2] == 'G')    return true;  // TLG5/6
	if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N')   return true;  // PNG
	if (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return true;  // JPEG
	if (b[0] == 'O' && b[1] == 'g' && b[2] == 'g')    return true;  // OggS
	if (b[0] == 'R' && b[1] == 'I' && b[2] == 'F')    return true;  // RIFF
	if (b[0] == 'I' && b[1] == 'D' && b[2] == '3')    return true;  // ID3v2
	if (b[0] == '/' && (b[1] == '/' || b[1] == '*'))  return true;  // TJS comment
	if (b[0] == '<') return true;                                   // XML-ish
	int printable = 0;
	for (unsigned i = 0; i < size && i < 16; i++) {
		unsigned char c = xored ? (unsigned char)(p[i] ^ 0x36) : p[i];
		if ((c >= 0x20 && c < 0x7F) || c == '\r' || c == '\n' || c == '\t') printable++;
	}
	return printable >= 12;
}

void FateXorTransform(unsigned char *buf, unsigned size, uint64_t offset)
{
	for (unsigned i = 0; i < size; i++) buf[i] ^= 0x36;
	// Two extra bit flips, at absolute stream offsets, as in Fate's scheme.
	if (offset <= 0x13 && offset + size > 0x13)     buf[0x13 - offset] ^= 1;
	if (offset <= 0x2EA29 && offset + size > 0x2EA29) buf[0x2EA29 - offset] ^= 3;
}

void BuiltinXP3Filter(tTVPXP3ExtractionFilterInfo *info)
{
	unsigned char *buf = (unsigned char *)info->Buffer;
	const unsigned size = info->BufferSize;
	if (!buf || size == 0) return;

	// Explicit table entry wins.
	const OverrideRec *rec = FindByHash(info->FileHash);
	if (rec) {
		if (rec->needDecryption) FateXorTransform(buf, size, info->Offset);
		return;
	}

	// Otherwise decide once from the file's first bytes.
	if (g_arch_format == ArchFormat::Unknown && info->Offset == 0) {
		if (LooksLikeHeader(buf, size, false))      g_arch_format = ArchFormat::Plain;
		else if (LooksLikeHeader(buf, size, true))  g_arch_format = ArchFormat::FateXor;
		else return; // undecidable: leave the bytes alone
		TVPAddImportantLog(ttstr(TJS_W("(info) XP3 filter: ")) +
			(g_arch_format == ArchFormat::FateXor
				? ttstr(TJS_W("XOR 0x36 (encrypted)"))
				: ttstr(TJS_W("plaintext"))));
	}
	if (g_arch_format == ArchFormat::FateXor)
		FateXorTransform(buf, size, info->Offset);
}

} // namespace

//---------------------------------------------------------------------------
// Public API used by the plugin loader
//---------------------------------------------------------------------------

// True when the engine answers this plugin itself (a table entry names it, or
// the historical built-in list does). The PE on disk is never loaded.
bool TVPIsBuiltinPlugin(const ttstr &name)
{
	return FindByName(name) != nullptr;
}

// Supply a table-listed plugin.
//
// A `replacementFile` is resolved in one of two ways:
//   * `@builtin:<name>` -> a shim compiled into the engine (BuiltinShims.h).
//     Preferred: no file, no architecture to match, nothing to install
//     alongside the table.
//   * a plain path -> dlopen'd from the overrides directory. It must be built
//     for the target architecture; an x86-64 object can never load on an
//     aarch64 device.
//
// `-` means the engine answers the link itself and the game's Windows PE is
// never touched.
void TVPInstallBuiltinPlugin(const ttstr &name)
{
	const OverrideRec *r = FindByName(name);
	const char *shim_name = nullptr;
	size_t shim_len = 0;

	if (r && !r->replacement.empty() &&
	    TVPParseBuiltinRef(r->replacement, &shim_name, &shim_len)) {
		// Copy the name out: it is a slice of the record, not terminated.
		std::string shim(shim_name, shim_len);
		if (TVPInstallBuiltinShim(shim.c_str())) {
			TVPAddImportantLog(ttstr(TJS_W("(info) override: '")) + name +
				TJS_W("' -> @builtin:") + ttstr(shim.c_str()));
			return;
		}
		// Recognised as a shim reference but not in the registry: either the
		// feature is compiled out or the table names a shim this build does not
		// have. Say which, and do not fall through to the file path — a name
		// like "@builtin:noop" is not a path and would only produce a
		// confusing "cannot load" message.
		TVPAddImportantLog(ttstr(TJS_W("(warn) override: '")) + name +
			TJS_W("' wants shim '") + ttstr(shim.c_str()) +
			TJS_W("', which this build does not provide"));
		TVPSetXP3ArchiveExtractionFilter(&BuiltinXP3Filter);
		return;
	}

	if (r && !r->replacement.empty()) {
		std::string path = OverridesPath();     // .../<data>/overrides.txt
		size_t slash = path.find_last_of('/');
		path = (slash == std::string::npos ? std::string(".") : path.substr(0, slash + 1))
		       + r->replacement;
		// Load the replacement for real. It exports V2Link/V2Unlink exactly like
		// the plugin it replaces, and must match this binary's architecture.
		void *h = SDL_LoadObject(path.c_str());
		if (h) {
			typedef int (*linkproc)(iTVPFunctionExporter *);
			linkproc v2link = (linkproc)SDL_LoadFunction(h, "V2Link");
			if (v2link) v2link(TVPGetFunctionExporter());
			TVPAddImportantLog(ttstr(TJS_W("(info) override: '")) + name +
				TJS_W("' -> ") + ttstr(r->replacement.c_str()));
			return;
		}
		TVPAddImportantLog(ttstr(TJS_W("(warn) override: cannot load '")) +
			ttstr(path.c_str()) + TJS_W("' (wrong architecture?)"));
	}
	// Engine-handled: install the behaviours the plugin would have provided.
	TVPSetXP3ArchiveExtractionFilter(&BuiltinXP3Filter);
	TVPAddImportantLog(ttstr(TJS_W("(info) Builtin plugin: '")) + name +
		TJS_W("' handled by engine"));
}

// Install the archive filter so it is live even if no plugin is linked.
void TVPInstallBuiltinPlugins(void)
{
	TVPSetXP3ArchiveExtractionFilter(&BuiltinXP3Filter);

	/* The core's own text channel (src/shims/textstream_shim.cpp): the globals a
	 * look-ahead parser calls to hand parsed text to the shell.
	 *
	 * Installed HERE, at start-up, rather than through overrides.txt — and that
	 * is the honest place for it, because no game links this name. A table entry
	 * would have to pretend some title asks for `textstream.dll`; the table is a
	 * record of what a game's own code links, and this is not that. It is the
	 * core talking to itself, so it starts with the engine rather than in
	 * response to a title's plugin request, and it is live before any script
	 * runs — which is when the look-ahead needs it.
	 *
	 * A game is unaffected either way: the globals are only reachable by name,
	 * and a caller checks `typeof vnovelPublishTextRun == "function"` before
	 * using them. */
	if (!TVPInstallBuiltinShim("textstream"))
		TVPAddImportantLog(
			ttstr(TJS_W("(info) text stream shim not present in this build")));
}
