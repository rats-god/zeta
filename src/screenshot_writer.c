/**
 * Copyright (c) 2018, 2019, 2020 Adrian Siekierka
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"
#include "screenshot_writer.h"

#define POS_MUL (scr_width <= 40 ? 2 : 1)

#ifdef USE_LIBPNG
#include <png.h>

static int write_screenshot_png(FILE *output, u8 *buffer, u32 *palette, int flags, int width, int height) {
	png_image image;
	u8 colormap[48];

	memset(&image, 0, sizeof(image));
	image.version = PNG_IMAGE_VERSION;
	image.width = width;
	image.height = height;
	image.format = PNG_FORMAT_RGB_COLORMAP;
	image.colormap_entries = 16;

	for (int i = 0; i < 16; i++) {
		colormap[i*3 + 0] = palette[i] >> 16;
		colormap[i*3 + 1] = palette[i] >> 8;
		colormap[i*3 + 2] = palette[i] >> 0;
	}

	return png_image_write_to_stdio(&image, output, 0, buffer, 0, colormap) > 0;
}
#endif

static int write_screenshot_bmp(FILE *output, u8 *buffer, u32 *palette, int flags, int width, int height) {
	int bmp_size = 54 + (4*16) + (width * height / 2);

	// header
	fputc('B', output);
	fputc('M', output);
	fput32le(output, bmp_size);
	fput32le(output, 0);
	fput32le(output, 54 + (4*16));

	fput32le(output, 40);
	fput32le(output, width);
	fput32le(output, height);
	fput16le(output, 1);
	fput16le(output, 4);
	fput32le(output, 0);
	fput32le(output, (width * height) / 2);

	// 13-inch monitor at 640x350
	fput32le(output, width * 1000 / 262);
	fput32le(output, height * 1000 / 270);

	fput32le(output, 16);
	fput32le(output, 0);

	// color table
	for (int i = 0; i < 16; i++) {
		fputc((palette[i]) & 0xFF, output);
		fputc((palette[i] >> 8) & 0xFF, output);
		fputc((palette[i] >> 16) & 0xFF, output);
		fputc(0, output);
	}

	// pixel data
	for (int y = 0; y < height; y++) {
		int pos = (height - 1 - y) * width;
		for (int x = 0; x < width; x += 2, pos += 2) {
			fputc((buffer[pos] << 4) | buffer[pos + 1], output);
		}
	}

	return 0;
}

int write_screenshot(FILE *output, int type, int scr_width, int flags, u8 *video, u8 *charset, int char_width, int char_height, u32 *palette) {
	void *buffer;

	int paletted = (type == SCREENSHOT_TYPE_BMP || type == SCREENSHOT_TYPE_PNG);

	buffer = malloc(char_width * char_height * scr_width * POS_MUL * 25 * (paletted ? sizeof(u8) : sizeof(u32)));
	if (buffer == NULL) {
		return -1;
	}

	if (paletted) {
		render_software_paletted(buffer, scr_width, -1, flags, video, charset, char_width, char_height);
	} else {
		render_software_rgb(buffer, scr_width, -1, flags, video, charset, char_width, char_height, palette);
	}

	int result;

	switch (type) {
		case SCREENSHOT_TYPE_BMP:
			result = write_screenshot_bmp(output, (u8*) buffer, palette, flags, char_width * scr_width * POS_MUL, char_height * 25);
			break;
#ifdef USE_LIBPNG
		case SCREENSHOT_TYPE_PNG:
			result = write_screenshot_png(output, (u8*) buffer, palette, flags, char_width * scr_width * POS_MUL, char_height * 25);
			break;
#endif
		default:
			result = -1;
			break;
	}

	free(buffer);
	return result;
}
