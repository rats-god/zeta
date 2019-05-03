/**
 * Copyright (c) 2018, 2019 Adrian Siekierka
 *
 * This file is part of Zeta.
 *
 * Zeta is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Zeta is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Zeta.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#ifdef USE_GETOPT
#define _POSIX_C_SOURCE 2
#include <unistd.h>
#endif

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#ifdef USE_OPENGL
#ifdef USE_OPENGL_ES
#include <SDL2/SDL_opengles.h>
#else
#include <SDL2/SDL_opengl.h>
#endif
#endif
#include "zzt.h"
#include "audio_stream.h"
#include "posix_vfs.h"
#include "render_software.h"
#ifdef ENABLE_SCREENSHOTS
#include "screenshot_writer.h"
#endif

static const u8 sdl_to_pc_scancode[] = {
/*  0*/	0,
/*  1*/	0, 0, 0,
/*  4*/	0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, /* A-I */
/* 13*/	0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, /* J-R */
/* 22*/	0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,       /* S-Z */
/* 30*/	2, 3, 4, 5, 6, 7, 8, 9, 10, 11, /* 1-0 */
/* 40*/	0x1C, 0x01, 0x0E, 0x0F, 0x39,
/* 45*/	0x0C, 0x0D, 0x1A, 0x1B, 0x2B,
/* 50*/	0x2B, 0x27, 0x28, 0x29,
/* 54*/	0x33, 0x34, 0x35, 0x3A,
	0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,
	0x37, 0x46, 0, 0x52, 0x47, 0x49, 0x53, 0x4F, 0x51,
	0x4D, 0x4B, 0x50, 0x48, 0x45
};

static const int sdl_to_pc_scancode_max = sizeof(sdl_to_pc_scancode) - 1;

static SDL_Texture *create_texture_from_array(SDL_Renderer *renderer, int access, unsigned char *data, int height) {
	SDL_Texture *texture;
	SDL_Rect rect;
	Uint32 texdata[8 * height];
	Uint32* tptr;
	int ch, cx, cy, ctmp;

	rect.w = 8;
	rect.h = height;
	texture = SDL_CreateTexture(renderer,
		SDL_PIXELFORMAT_RGBA32,
		access,
		16*rect.w, 16*rect.h);

	for (ch = 0; ch < 256; ch++) {
		rect.x = (ch & 0x0F) * rect.w;
		rect.y = (ch >> 4) * rect.h;
		tptr = texdata;
		for (cy = 0; cy < height; cy++, data++) {
			ctmp = data[0];
			for (cx = 0; cx < 8; cx++, tptr++, ctmp <<= 1) {
				*tptr = ((ctmp >> 7) & 1) * 0xFFFFFFFF;
			}
		}
		SDL_UpdateTexture(texture, &rect, texdata, 32);
	}

	return texture;
}

long zeta_time_ms() {
	return SDL_GetTicks();
}

void cpu_ext_log(const char *s) {
	fprintf(stderr, "%s\n", s);
}

int zeta_has_feature(int feature) {
	return 1;
}

static SDL_AudioDeviceID audio_device;
static SDL_AudioSpec audio_spec;
static SDL_mutex *audio_mutex;

static void audio_callback(void *userdata, Uint8 *stream, int len) {
	SDL_LockMutex(audio_mutex);
	audio_stream_generate_u8(zeta_time_ms(), stream, len);
	SDL_UnlockMutex(audio_mutex);
}

void speaker_on(double freq) {
	SDL_LockMutex(audio_mutex);
	audio_stream_append_on(zeta_time_ms(), freq);
	SDL_UnlockMutex(audio_mutex);
}

void speaker_off() {
	SDL_LockMutex(audio_mutex);
	audio_stream_append_off(zeta_time_ms());
	SDL_UnlockMutex(audio_mutex);
}

static SDL_mutex *zzt_thread_lock;
static SDL_cond *zzt_thread_cond;
static u8 zzt_vram_copy[80*25*2];
static u8 zzt_thread_running;
static atomic_int zzt_renderer_waiting = 0;
static u8 video_blink = 1;

static long first_timer_tick;
static double timer_time;

static Uint32 sdl_timer_thread(Uint32 interval, void *param) {
	if (!zzt_thread_running) return 0;
	long curr_timer_tick = zeta_time_ms();

	atomic_fetch_add(&zzt_renderer_waiting, 1);
	SDL_LockMutex(zzt_thread_lock);
	atomic_fetch_sub(&zzt_renderer_waiting, 1);
	zzt_mark_timer();

	timer_time += SYS_TIMER_TIME;
	long duration = curr_timer_tick - first_timer_tick;
	long tick_time = ((long) (timer_time + SYS_TIMER_TIME)) - duration;

	while (tick_time <= 0) {
		zzt_mark_timer();
		timer_time += SYS_TIMER_TIME;
		tick_time = ((long) (timer_time + SYS_TIMER_TIME)) - duration;
	}

	SDL_CondBroadcast(zzt_thread_cond);
	SDL_UnlockMutex(zzt_thread_lock);
	return tick_time;
}

static void sdl_timer_init() {
	first_timer_tick = zeta_time_ms();
	timer_time = 0;
	SDL_AddTimer((int) SYS_TIMER_TIME, sdl_timer_thread, (void*)NULL);
}

// try to keep a budget of ~5ms per call

static int zzt_thread_func(void *ptr) {
	int opcodes = 1000;
	while (zzt_thread_running) {
		if (SDL_LockMutex(zzt_thread_lock) == 0) {
			while (zzt_renderer_waiting > 0) {
				SDL_CondWait(zzt_thread_cond, zzt_thread_lock);
			}
			long duration = zeta_time_ms();
			int rcode = zzt_execute(opcodes);
			duration = zeta_time_ms() - duration;
			if (rcode == STATE_CONTINUE) {
				if (duration < 2) {
					opcodes = (opcodes * 20 / 19);
				} else if (duration > 4) {
					opcodes = (opcodes * 19 / 20);
				}
			}
			SDL_CondBroadcast(zzt_thread_cond);
			if (rcode == STATE_WAIT) {
				SDL_CondWaitTimeout(zzt_thread_cond, zzt_thread_lock, 20);
			} else if (rcode == STATE_END) {
				zzt_thread_running = 0;
			}
			SDL_UnlockMutex(zzt_thread_lock);
		}
	}

	return 0;
}

#define KEYMOD_ALT(keymod) ((keymod) & (KMOD_LALT | KMOD_RALT))
#define KEYMOD_CTRL(keymod) ((keymod) & (KMOD_LCTRL | KMOD_RCTRL))
#define KEYMOD_SHIFT(keymod) ((keymod) & (KMOD_LSHIFT | KMOD_RSHIFT))

static void update_keymod(SDL_Keymod keymod) {
	if (KEYMOD_SHIFT(keymod)) zzt_kmod_set(0x01); else zzt_kmod_clear(0x01);
	if (KEYMOD_CTRL(keymod)) zzt_kmod_set(0x04); else zzt_kmod_clear(0x04);
	if (KEYMOD_ALT(keymod)) zzt_kmod_set(0x08); else zzt_kmod_clear(0x08);
}

static char as_shifted(char kcode) {
	if (kcode >= 'a' && kcode <= 'z') {
		return kcode - 32;
	} else switch(kcode) {
		case '1': return '!';
		case '2': return '@';
		case '3': return '#';
		case '4': return '$';
		case '5': return '%';
		case '6': return '^';
		case '7': return '&';
		case '8': return '*';
		case '9': return '(';
		case '0': return ')';
		case '-': return '_';
		case '=': return '+';
		case '[': return '{';
		case ']': return '}';
		case ';': return ':';
		case '\'': return '"';
		case '\\': return '|';
		case ',': return '<';
		case '.': return '>';
		case '/': return '?';
		case '`': return '~';
		default: return kcode;
	}
}

static SDL_Window *window;
static SDL_Renderer *renderer;
static int charw, charh;

// used in software mode
static SDL_Texture *playfieldtex = NULL;
static u32 software_palette[16];

// used in OpenGL mode
#ifdef USE_OPENGL
static SDL_Texture *chartex = NULL;
static SDL_GLContext gl_context;
#endif

// used for marking render data updates
static SDL_mutex *render_data_update_mutex;

static int charset_update_requested = 0;
static int charset_char_height;
static u8* charset_update_data = NULL;

static int palette_update_requested = 0;
static u32* palette_update_data = NULL;

#define AREA_WITHOUT_SCALE 1

static void calc_render_area(SDL_Rect *rect, int w, int h, int *scale_out, int flags) {
	int iw = 80*charw;
	int ih = 25*charh;

	int scale = 1;
	while (((scale+1)*iw <= w) && ((scale+1)*ih <= h)) scale++;
	if (scale_out != NULL) *scale_out = scale;

	w /= scale;
	h /= scale;

	if (flags & AREA_WITHOUT_SCALE) scale = 1;

	rect->x = ((w - iw) * scale) / 2;
	rect->y = ((h - ih) * scale) / 2;
	rect->w = iw * scale;
	rect->h = ih * scale;
}

#ifdef USE_OPENGL
/* static void oglguard() {
	GLenum err;

	while ((err = glGetError()) != GL_NO_ERROR) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "OpenGL error: %d", err);
	}
} */

static void prepare_render_opengl() {
	SDL_Rect rect;
	int w, h, scale;
	SDL_GL_GetDrawableSize(window, &w, &h);
	calc_render_area(&rect, w, h, &scale, AREA_WITHOUT_SCALE);

	glViewport(0, 0, scale * (rect.w + (rect.x * 2)), scale * (rect.h + (rect.y * 2)));
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
#ifdef USE_OPENGL_ES
	glOrthof(-(rect.x), rect.w + (rect.x), rect.h + (rect.y), -(rect.y), -1, 1);
#else
	glOrtho(-(rect.x), rect.w + (rect.x), rect.h + (rect.y), -(rect.y), -1, 1);
#endif
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

#define GLVX(i,width) ((i)*charw*(80/width))
#define GLVY(i) ((i)*charh)
#define GLTX(chr,i) ( ( ((chr)&0xF)+(i) )/16.0*1.0 )
#define GLTY(chr,i) ( ( ((chr)>>4)+(i) )/16.0*1.0 )

static unsigned short *ogl_buf_pos;
static unsigned short *ogl_buf_pos40;
static unsigned char *ogl_buf_col;
static float *ogl_buf_tex;
static unsigned char *ogl_buf_colcache;
static float *ogl_buf_texcache;

#ifdef USE_OPENGL_ES
#define GL_COMPONENT_POINTS 6
#else
#define GL_COMPONENT_POINTS 4
#endif

static void update_opengl_colcache(u32* pal) {
	int size = GL_COMPONENT_POINTS * 4;
	for (int i = 0; i < size; i++) {
		for (int bpos = i*size; bpos < (i+1)*size; bpos+=4) {
			ogl_buf_colcache[bpos] = (pal[i] >> 16) & 0xFF;
			ogl_buf_colcache[bpos + 1] = (pal[i] >> 8) & 0xFF;
			ogl_buf_colcache[bpos + 2] = (pal[i] >> 0) & 0xFF;
			ogl_buf_colcache[bpos + 3] = 0xFF;
		}
	}
}

static void init_opengl() {
	ogl_buf_pos = malloc((80 * 25) * GL_COMPONENT_POINTS * 2 * sizeof(short));
	ogl_buf_pos40 = malloc((40 * 25) * GL_COMPONENT_POINTS * 2 * sizeof(short));
	ogl_buf_col = malloc(2 * (80 * 25) * GL_COMPONENT_POINTS * 4 * sizeof(char));
	ogl_buf_tex = malloc((80 * 25) * GL_COMPONENT_POINTS * 2 * sizeof(float));
	ogl_buf_colcache = malloc(16 * 4 * GL_COMPONENT_POINTS * sizeof(char));
	ogl_buf_texcache = malloc(256 * 2 * GL_COMPONENT_POINTS * sizeof(float));

	memset(ogl_buf_colcache, 0, 16 * 4 * GL_COMPONENT_POINTS * sizeof(char));

#ifndef USE_OPENGL_ES
	for (int tpos = 0; tpos < 256 * 8; tpos += 8) {
		u8 chr = tpos >> 3;
		ogl_buf_texcache[tpos] = GLTX(chr,0);
		ogl_buf_texcache[tpos+1] = GLTY(chr,0);
		ogl_buf_texcache[tpos+2] = GLTX(chr,1);
		ogl_buf_texcache[tpos+3] = GLTY(chr,0);
		ogl_buf_texcache[tpos+4] = GLTX(chr,1);
		ogl_buf_texcache[tpos+5] = GLTY(chr,1);
		ogl_buf_texcache[tpos+6] = GLTX(chr,0);
		ogl_buf_texcache[tpos+7] = GLTY(chr,1);
	}

	for (int i = 0; i < 2000; i++) {
		int x = i % 80;
		int y = i / 80;
		ogl_buf_pos[i * 8 + 0] = GLVX(x,80);
		ogl_buf_pos[i * 8 + 1] = GLVY(y);
		ogl_buf_pos[i * 8 + 2] = GLVX(x+1,80);
		ogl_buf_pos[i * 8 + 3] = GLVY(y);
		ogl_buf_pos[i * 8 + 4] = GLVX(x+1,80);
		ogl_buf_pos[i * 8 + 5] = GLVY(y+1);
		ogl_buf_pos[i * 8 + 6] = GLVX(x,80);
		ogl_buf_pos[i * 8 + 7] = GLVY(y+1);
	}

	for (int i = 0; i < 1000; i++) {
		int x = i % 40;
		int y = i / 40;
		ogl_buf_pos40[i * 8 + 0] = GLVX(x,40);
		ogl_buf_pos40[i * 8 + 1] = GLVY(y);
		ogl_buf_pos40[i * 8 + 2] = GLVX(x+1,40);
		ogl_buf_pos40[i * 8 + 3] = GLVY(y);
		ogl_buf_pos40[i * 8 + 4] = GLVX(x+1,40);
		ogl_buf_pos40[i * 8 + 5] = GLVY(y+1);
		ogl_buf_pos40[i * 8 + 6] = GLVX(x,40);
		ogl_buf_pos40[i * 8 + 7] = GLVY(y+1);
	}
#else
	for (int tpos = 0; tpos < 256 * 12; tpos += 12) {
		u8 chr = tpos / 12;
		ogl_buf_texcache[tpos] = GLTX(chr,0);
		ogl_buf_texcache[tpos+1] = GLTY(chr,0);
		ogl_buf_texcache[tpos+2] = GLTX(chr,1);
		ogl_buf_texcache[tpos+3] = GLTY(chr,0);
		ogl_buf_texcache[tpos+4] = GLTX(chr,1);
		ogl_buf_texcache[tpos+5] = GLTY(chr,1);
		ogl_buf_texcache[tpos+6] = GLTX(chr,0);
		ogl_buf_texcache[tpos+7] = GLTY(chr,0);
		ogl_buf_texcache[tpos+8] = GLTX(chr,1);
		ogl_buf_texcache[tpos+9] = GLTY(chr,1);
		ogl_buf_texcache[tpos+10] = GLTX(chr,0);
		ogl_buf_texcache[tpos+11] = GLTY(chr,1);
	}

	for (int i = 0; i < 2000; i++) {
		int x = i % 80;
		int y = i / 80;
		ogl_buf_pos[i * 12 + 0] = GLVX(x,80);
		ogl_buf_pos[i * 12 + 1] = GLVY(y);
		ogl_buf_pos[i * 12 + 2] = GLVX(x+1,80);
		ogl_buf_pos[i * 12 + 3] = GLVY(y);
		ogl_buf_pos[i * 12 + 4] = GLVX(x+1,80);
		ogl_buf_pos[i * 12 + 5] = GLVY(y+1);
		ogl_buf_pos[i * 12 + 6] = GLVX(x,80);
		ogl_buf_pos[i * 12 + 7] = GLVY(y);
		ogl_buf_pos[i * 12 + 8] = GLVX(x+1,80);
		ogl_buf_pos[i * 12 + 9] = GLVY(y+1);
		ogl_buf_pos[i * 12 + 10] = GLVX(x,80);
		ogl_buf_pos[i * 12 + 11] = GLVY(y+1);
	}

	for (int i = 0; i < 1000; i++) {
		int x = i % 40;
		int y = i / 40;
		ogl_buf_pos40[i * 12 + 0] = GLVX(x,40);
		ogl_buf_pos40[i * 12 + 1] = GLVY(y);
		ogl_buf_pos40[i * 12 + 2] = GLVX(x+1,40);
		ogl_buf_pos40[i * 12 + 3] = GLVY(y);
		ogl_buf_pos40[i * 12 + 4] = GLVX(x+1,40);
		ogl_buf_pos40[i * 12 + 5] = GLVY(y+1);
		ogl_buf_pos40[i * 12 + 6] = GLVX(x,40);
		ogl_buf_pos40[i * 12 + 7] = GLVY(y);
		ogl_buf_pos40[i * 12 + 8] = GLVX(x+1,40);
		ogl_buf_pos40[i * 12 + 9] = GLVY(y+1);
		ogl_buf_pos40[i * 12 + 10] = GLVX(x,40);
		ogl_buf_pos40[i * 12 + 11] = GLVY(y+1);
	}
#endif
}

static void deinit_opengl() {
	free(ogl_buf_texcache);
	free(ogl_buf_colcache);
	free(ogl_buf_tex);
	free(ogl_buf_col);
	free(ogl_buf_pos40);
	free(ogl_buf_pos);
}

static void render_opengl(long curr_time, int regen_visuals) {
	u8 blink_local = video_blink && ((curr_time % 466) >= 233);
	float texw, texh;
	int width = (zzt_video_mode() & 2) ? 80 : 40;

	prepare_render_opengl();

	// generate visual data
	int vpos = 0;
	if (regen_visuals) for (int y = 0; y < 25; y++) {
		for (int x = 0; x < width; x++, vpos += 2) {
			u8 chr = zzt_vram_copy[vpos];
			u8 col = zzt_vram_copy[vpos+1];
			u8 bgcol = col >> 4;
			u8 fgcol = col & 0xF;

			if (video_blink) {
				if (bgcol >= 0x8) {
					bgcol &= 0x7;
					if (blink_local) fgcol = bgcol;
				}
			}

			int bpos_s = vpos * 2 * GL_COMPONENT_POINTS;
			memcpy(ogl_buf_col + bpos_s, ogl_buf_colcache + (4*GL_COMPONENT_POINTS*bgcol), 4*GL_COMPONENT_POINTS*sizeof(char));
			memcpy(ogl_buf_col + bpos_s + (8000 * GL_COMPONENT_POINTS), ogl_buf_colcache + (4*GL_COMPONENT_POINTS*fgcol), 4*GL_COMPONENT_POINTS*sizeof(char));

			int tpos_s = bpos_s >> 1;
			memcpy(ogl_buf_tex + tpos_s, ogl_buf_texcache + chr*2*GL_COMPONENT_POINTS, 2*GL_COMPONENT_POINTS*sizeof(float));
		}
	}

	// pass 1: background colors
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_CULL_FACE);

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);

	glVertexPointer(2, GL_SHORT, 0, width == 40 ? ogl_buf_pos40 : ogl_buf_pos);
	glColorPointer(4, GL_UNSIGNED_BYTE, 0, ogl_buf_col);

#ifdef USE_OPENGL_ES
	glDrawArrays(GL_TRIANGLES, 0, width * 25 * 6);
#else
	glDrawArrays(GL_QUADS, 0, width * 25 * 4);
#endif

	// pass 2: foreground colors
	if (chartex != NULL) {
		if (SDL_GL_BindTexture(chartex, &texw, &texh)) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not bind OpenGL texture! %s", SDL_GetError());
		}
		glAlphaFunc(GL_GREATER, 0.5);
		glEnable(GL_ALPHA_TEST);
		glEnable(GL_TEXTURE_2D);

		glEnableClientState(GL_TEXTURE_COORD_ARRAY);

		glColorPointer(4, GL_UNSIGNED_BYTE, 0, ogl_buf_col + (80 * 25 * 4 * GL_COMPONENT_POINTS * sizeof(char)));
		glTexCoordPointer(2, GL_FLOAT, 0, ogl_buf_tex);

#ifdef USE_OPENGL_ES
		glDrawArrays(GL_TRIANGLES, 0, width * 25 * 6);
#else
		glDrawArrays(GL_QUADS, 0, width * 25 * 4);
#endif

		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		glDisableClientState(GL_VERTEX_ARRAY);
		glDisableClientState(GL_COLOR_ARRAY);

		SDL_GL_UnbindTexture(chartex);
	}
}
#endif /* USE_OPENGL */

static void render_software_copy(long curr_time) {
	SDL_Rect dest;
	void *buffer;
	int w, h, pitch;

	int swidth = (zzt_video_mode() & 2) ? 80 : 40;
	int sflags = 0;

	if (palette_update_data == NULL || charset_update_data == NULL) {
		return;
	}

	if (!video_blink) sflags |= RENDER_BLINK_OFF;
	else if ((zeta_time_ms() % 466) >= 233) sflags |= RENDER_BLINK_PHASE;

	SDL_GetWindowSize(window, &w, &h);
	calc_render_area(&dest, w, h, NULL, 0);

	SDL_LockTexture(playfieldtex, NULL, &buffer, &pitch);
	render_software_rgb(
		buffer,
		swidth, pitch / 4, sflags,
		zzt_get_ram(), charset_update_data,
		8, charset_char_height,
		palette_update_data
	);
	SDL_UnlockTexture(playfieldtex);

	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	SDL_RenderCopy(renderer, playfieldtex, NULL, &dest);
}

void zeta_update_charset(int width, int height, u8* data) {
	SDL_LockMutex(render_data_update_mutex);
	charset_char_height = height;
	charset_update_data = data;
	charset_update_requested = 1;
	SDL_UnlockMutex(render_data_update_mutex);
}

void zeta_update_palette(u32* data) {
	SDL_LockMutex(render_data_update_mutex);
	palette_update_data = data;
	palette_update_requested = 1;
	SDL_UnlockMutex(render_data_update_mutex);
}

#include "frontend_posix.c"

int main(int argc, char **argv) {
	int scancodes_lifted[sdl_to_pc_scancode_max + 1];
	int slc = 0;
	int use_opengl = 0;

	SDL_AudioSpec requested_audio_spec;

	SDL_Event event;
	int scode, kcode;

	SDL_Thread* zzt_thread;
	u8 windowed = 1;

	init_posix_vfs("");

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init failed! %s", SDL_GetError());
		return 1;
	}

	render_data_update_mutex = SDL_CreateMutex();
	zzt_thread_lock = SDL_CreateMutex();
	zzt_thread_cond = SDL_CreateCond();
	audio_mutex = SDL_CreateMutex();

	if (posix_zzt_init(argc, argv) < 0) {
		fprintf(stderr, "Could not load ZZT!\n");
		SDL_Quit();
		return 1;
	}

	SDL_zero(requested_audio_spec);
	requested_audio_spec.freq = 48000;
	requested_audio_spec.format = AUDIO_U8;
	requested_audio_spec.channels = 1;
	requested_audio_spec.samples = 4096;
	requested_audio_spec.callback = audio_callback;

	audio_device = SDL_OpenAudioDevice(NULL, 0, &requested_audio_spec, &audio_spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if (audio_device == 0) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not open audio device! %s", SDL_GetError());
	}

	charw = 8;
	charh = 14;

#ifdef USE_OPENGL
	use_opengl = 1;
	if (use_opengl) {
		init_opengl();

#ifdef USE_OPENGL_ES
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#else
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 4);
#endif
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);

		window = SDL_CreateWindow("Zeta", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			80*charw, 25*charh, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
		if (window == NULL) {
			use_opengl = 0;
		} else if ((gl_context = SDL_GL_CreateContext(window)) == NULL) {
			SDL_DestroyWindow(window);
			use_opengl = 0;
		} else {
			SDL_GL_SetSwapInterval(1);
		}
	}
#else
	use_opengl = 0;
#endif

	if (!use_opengl) {
#ifdef USE_OPENGL
		fprintf(stderr, "Could not initialize OpenGL (%s), using software renderer...", SDL_GetError());
#endif
		window = SDL_CreateWindow("Zeta", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			80*charw, 25*charh, 0);
		SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
	} else {
		SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
	}

	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

	if (!use_opengl) {
//		int endian_test = 0xFF000000;
//		int pformat = ((((char*)&endian_test)[0]) != 0) ? SDL_PIXELFORMAT_ARGB32 : SDL_PIXELFORMAT_BGRA32;
#ifdef BIG_ENDIAN
		int pformat = SDL_PIXELFORMAT_ARGB32;
#else
		int pformat = SDL_PIXELFORMAT_BGRA32;
#endif
		playfieldtex = SDL_CreateTexture(renderer, pformat, SDL_TEXTUREACCESS_STREAMING, 80*charw, 25*charh);
	}

	long curr_time;
	u8 cont_loop = 1;

	zzt_thread_running = 1;
	zzt_thread = SDL_CreateThread(zzt_thread_func, "ZZT Executor", (void*)NULL);
	if (zzt_thread == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Could not create ZZT thread! %s", SDL_GetError());
		return 1;
	}

	if (audio_device != 0) {
		audio_stream_init(zeta_time_ms(), audio_spec.freq);
		audio_stream_set_volume(audio_stream_get_max_volume() >> 1);
		SDL_PauseAudioDevice(audio_device, 0);
	}

	sdl_timer_init();

	int should_render;

	while (cont_loop) {
		if (!zzt_thread_running) { cont_loop = 0; break; }

		atomic_fetch_add(&zzt_renderer_waiting, 1);
		SDL_LockMutex(zzt_thread_lock);
		atomic_fetch_sub(&zzt_renderer_waiting, 1);
		u8* ram = zzt_get_ram();
		should_render = memcmp(ram + 0xB8000, zzt_vram_copy, 80*25*2);
		if (should_render) memcpy(zzt_vram_copy, ram + 0xB8000, 80*25*2);
		zzt_mark_frame();

		// do KEYUPs before KEYDOWNS - fixes key loss issues w/ Windows
		while (slc > 0) {
			zzt_keyup(scancodes_lifted[--slc]);
		}

		while (SDL_PollEvent(&event)) {
			switch (event.type) {
				case SDL_KEYDOWN:
					if (windowed && (event.key.keysym.sym == 'q' || event.key.keysym.scancode == SDL_SCANCODE_ESCAPE)) {
						if (SDL_GetRelativeMouseMode() != 0) {
							SDL_SetRelativeMouseMode(0);
							break;
						}
					}
#ifdef ENABLE_SCREENSHOTS
					if (event.key.keysym.sym == SDLK_F12) {
						int i = -1;
						FILE *file;
						char filename[24];

						int sflags = 0;
						int swidth = (zzt_video_mode() & 2) ? 80 : 40;

						if (charset_update_data == NULL || palette_update_data == NULL) {
							break;
						}

						if (!video_blink) sflags |= RENDER_BLINK_OFF;
						else if ((zeta_time_ms() % 466) >= 233) sflags |= RENDER_BLINK_PHASE;

						while ((++i) <= 9999) {
							snprintf(filename, 23, "screen%d.bmp", i);
							file = fopen(filename, "rb");
							if (!file) {
								file = fopen(filename, "wb");
								if (write_screenshot(
									file, SCREENSHOT_TYPE_BMP,
									swidth, sflags,
									zzt_get_ram(), charset_update_data,
									8, charset_char_height,
									palette_update_data
								) < 0) {
									fprintf(stderr, "Could not write screenshot!\n");
								}
								break;
							} else {
								fclose(file);
							}
						}

						if (i > 9999) {
							fprintf(stderr, "Could not take screenshot!\n");
						}
						break;
					}
#endif
					if (event.key.keysym.scancode == SDL_SCANCODE_RETURN && KEYMOD_ALT(event.key.keysym.mod)) {
						// Alt+ENTER
						if (windowed) {
							SDL_DisplayMode mode;
							SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(window), &mode);
							SDL_SetWindowSize(window, mode.w, mode.h);
							SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN);
							// force focus
							SDL_SetRelativeMouseMode(1);
						} else {
							SDL_SetWindowFullscreen(window, 0);
							SDL_SetWindowSize(window, 80*charw, 25*charh);
							// drop focus
							SDL_SetRelativeMouseMode(0);
						}
						windowed = 1 - windowed;
						break;
					}
					update_keymod(event.key.keysym.mod);
					scode = event.key.keysym.scancode;
					kcode = event.key.keysym.sym;
					if (kcode < 0 || kcode >= 127) kcode = 0;
					if (scode >= 0 && scode <= sdl_to_pc_scancode_max) {
						if (KEYMOD_SHIFT(event.key.keysym.mod)) kcode = as_shifted(kcode);
						zzt_key(kcode, sdl_to_pc_scancode[scode]);
					}
					break;
				case SDL_KEYUP:
					update_keymod(event.key.keysym.mod);
					scode = event.key.keysym.scancode;
					if (scode >= 0 && scode <= sdl_to_pc_scancode_max) {
						scancodes_lifted[slc++] = sdl_to_pc_scancode[scode];
					}
					break;
				case SDL_MOUSEBUTTONDOWN:
					if (SDL_GetRelativeMouseMode() == 0) {
						if (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) {
							SDL_SetRelativeMouseMode(1);
						}
					} else {
						zzt_mouse_set(event.button.button);
					}
					break;
				case SDL_MOUSEBUTTONUP:
					zzt_mouse_clear(event.button.button);
					break;
				case SDL_MOUSEMOTION:
					if (SDL_GetRelativeMouseMode() != 0) {
						zzt_mouse_axis(0, event.motion.xrel);
						zzt_mouse_axis(1, event.motion.yrel);
					}
					break;
				case SDL_QUIT:
					cont_loop = 0;
					break;
			}
		}

		SDL_CondBroadcast(zzt_thread_cond);
		SDL_UnlockMutex(zzt_thread_lock);

		SDL_LockMutex(render_data_update_mutex);

		if (charset_update_requested) {
#ifdef USE_OPENGL
			if (use_opengl) {
				if (chartex != NULL) SDL_DestroyTexture(chartex);
				chartex = create_texture_from_array(renderer, SDL_TEXTUREACCESS_STATIC, charset_update_data, charset_char_height);
				SDL_SetTextureBlendMode(chartex, SDL_BLENDMODE_BLEND);
			}
#endif
			charset_update_requested = 0;
		}

		if (palette_update_requested) {
			if (use_opengl) {
#ifdef USE_OPENGL
				update_opengl_colcache(palette_update_data);
#endif
			} else {
				for (int i = 0; i < 16; i++) {
					software_palette[i] = palette_update_data[i] | 0xFF000000;
				}
			}
			palette_update_requested = 0;
		}

		SDL_UnlockMutex(render_data_update_mutex);

		curr_time = zeta_time_ms();
		if (use_opengl) {
#ifdef USE_OPENGL
			render_opengl(curr_time, should_render);
			SDL_GL_SwapWindow(window);
#endif
		} else {
			render_software_copy(curr_time);
			SDL_RenderPresent(renderer);
		}
	}

	zzt_thread_running = 0;
#ifdef USE_OPENGL
	if (use_opengl) {
		deinit_opengl();
	}

	if (chartex != NULL) SDL_DestroyTexture(chartex);
#endif
	if (playfieldtex != NULL) SDL_DestroyTexture(playfieldtex);
	SDL_DestroyRenderer(renderer);
	if (audio_device != 0) {
		SDL_CloseAudioDevice(audio_device);
	}
	SDL_Quit();
	return 0;
}
