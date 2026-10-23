#ifndef MOON_TEXTURES_H
#define MOON_TEXTURES_H

#include <stdint.h>
#include "moon_blitter.h"

#define TEX_REGOLITH_W 128
#define TEX_REGOLITH_H 128
extern uint16_t g_tex_regolith[16384];

#define TEX_CLIFF_W 128
#define TEX_CLIFF_H 64
extern const uint16_t g_tex_cliff[8192];

void moon_textures_init(void);

#endif /* MOON_TEXTURES_H */
