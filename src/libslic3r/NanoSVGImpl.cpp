// The single nanosvg parser implementation. It lives in libslic3r because libslic3r itself
// parses SVG (NSVGUtils, Format/svg.cpp), so every executable and module that links libslic3r
// gets it, including the ones without the GUI. The rasterizer stays in slic3r/GUI/BitmapCache.cpp.
#include <cstdio>
#include <cstring>
#include <cmath>
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
