/* user/ceres/include/ceres_textures.h — Seamless Geological Textures */
#ifndef CERES_TEXTURES_H
#define CERES_TEXTURES_H

#include <stdint.h>

#define TEX_ROCK_W 32
#define TEX_ROCK_H 32
#define TEX_ROCK_PIXELS (32 * 32)

/* Zero-static texture unpacker: decompresses 4-bit Flash into caller-owned buffer */
void ceres_textures_unpack(uint16_t *dst);

#endif /* CERES_TEXTURES_H */
