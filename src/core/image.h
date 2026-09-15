// PNG decoding and RGBA to BGRA texture copies for the picture reads.
#pragma once

#include <stdint.h>
#include <vector>

namespace xls {

struct RgbaImage {
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> pixels; // width * height * 4, RGBA.
};

bool DecodePng(const uint8_t* data, size_t size, RgbaImage& out);

// Scales (nearest) into a dwHeight x dwHeight square and writes B8G8R8A8 rows of pitch bytes.
void BlitToTexture(const RgbaImage& image, uint8_t* texture, uint32_t pitch, uint32_t height);

// Fills a texture with a flat colour, for a picture that could not be loaded.
void FillTexture(uint8_t* texture, uint32_t pitch, uint32_t height, uint32_t bgra);

}
