/* Button icons that follow the controller in use: the game's button glyph sheet changed at upload (glyph_icons.c). */
#pragma once
#include <stdint.h>

enum { GLYPH_ICONS_NINTENDO = 0, GLYPH_ICONS_XBOX = 1, GLYPH_ICONS_PLAYSTATION = 2 };
/* the sheet (BC4 level 0, linear: deswizzled) is DeviceFont80's button glyph sheet */
int glyph_icons_is_sheet(int w, int h, int levels, int bc4_unorm, uint8_t *bc4);
/* changes the sheet for a controller style in place; 1 if it changed anything */
int glyph_icons_apply(uint8_t *bc4, int style);
/* the style for the controller in use (nnpad_sdl.c: [general] button_icons of controllers.ini, or the last pad's type) */
int glyph_icons_style(void);
