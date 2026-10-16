//---------------------------------------------------------------------------
// FontDiscovery.h — list the font files this host provides.
//---------------------------------------------------------------------------
#ifndef __FONT_DISCOVERY_H__
#define __FONT_DISCOVERY_H__

#include <vector>
#include <string>

/* Return absolute paths of font files available on this host, in precedence
 * order. Backend: fontconfig when compiled in, else a portable directory scan.
 * KRKRSDL2_FONT_PATH (colon-separated) overrides both with an explicit list. */
std::vector<std::string> TVPListFontFiles();

#endif // __FONT_DISCOVERY_H__
