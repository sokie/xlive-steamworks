#include "core/image.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#pragma warning(push)
#pragma warning(disable : 4244 4456 4457 4996)
#include "third-party/stb_image.h"
#pragma warning(pop)

#include <string.h>

namespace xls {

bool DecodePng(const uint8_t* data, size_t size, RgbaImage& out)
{
	int width = 0;
	int height = 0;
	int channels = 0;
	stbi_uc* pixels = stbi_load_from_memory(data, (int)size, &width, &height, &channels, 4);
	if (!pixels) {
		return false;
	}
	out.width = (uint32_t)width;
	out.height = (uint32_t)height;
	out.pixels.assign(pixels, pixels + (size_t)width * height * 4);
	stbi_image_free(pixels);
	return true;
}

void BlitToTexture(const RgbaImage& image, uint8_t* texture, uint32_t pitch, uint32_t height)
{
	if (!texture || !image.width || !image.height) {
		return;
	}
	uint32_t width = pitch / 4 < height ? pitch / 4 : height;
	for (uint32_t y = 0; y < height; y++) {
		uint32_t sourceY = (uint32_t)((uint64_t)y * image.height / height);
		uint8_t* row = texture + (size_t)y * pitch;
		for (uint32_t x = 0; x < width; x++) {
			uint32_t sourceX = (uint32_t)((uint64_t)x * image.width / height);
			const uint8_t* source = &image.pixels[((size_t)sourceY * image.width + sourceX) * 4];
			row[x * 4 + 0] = source[2];
			row[x * 4 + 1] = source[1];
			row[x * 4 + 2] = source[0];
			row[x * 4 + 3] = source[3];
		}
	}
}

void FillTexture(uint8_t* texture, uint32_t pitch, uint32_t height, uint32_t bgra)
{
	if (!texture) {
		return;
	}
	uint32_t width = pitch / 4 < height ? pitch / 4 : height;
	for (uint32_t y = 0; y < height; y++) {
		uint32_t* row = (uint32_t*)(texture + (size_t)y * pitch);
		for (uint32_t x = 0; x < width; x++) {
			row[x] = bgra;
		}
	}
}

}
