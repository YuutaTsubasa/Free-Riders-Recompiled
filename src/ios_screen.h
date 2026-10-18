#pragma once

#include "launcher_screen_fit.h"

#include <string>

namespace sfr::launcher {

// The edges of the launcher's window that the Dynamic Island or notch, the
// rounded corners and the home indicator cover, in points (UIKit's safe area).
Insets ios_safe_area();

// The file of a system font by its PostScript name (CoreText's answer), or
// empty when the system has no such font.
std::string ios_font_file(const char* postscript_name);

}  // namespace sfr::launcher
