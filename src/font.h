// Palm OS fonts (ROM 'afnx' resources) for the Window Manager.
#pragma once
#include "gfx.h"
#include <string>

namespace font {
int width1x(const std::string& s);   // advance width of s in the current font, standard (1x) units
int height1x();
// Draws s with its top-left corner at native pixel (nx, ny); paint_bg also fills the cells.
void draw(Surface* s, int nx, int ny, const std::string& text, uint16_t fg, uint16_t bg, bool paint_bg);
}
