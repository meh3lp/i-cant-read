// OBS
#include <obs-module.h>
#include <graphics/graphics.h>
// Sockets
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
// Opengl
#include <glad/gl.h>
// types
#include <string.h>
// src
#include "gates.h"
#include "settings.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static GatePipeline *g_gate = NULL;
static int g_sock = -1;

#define GATE_SOCK "/tmp/i_cant_read_gate.sock"
#define TAG "[i-cant-read-ocr-gates] "

static void render_cb(void *param)
{
	blog(LOG_DEBUG, TAG "rendered callback");
	(void)param;
	if (!g_gate)
		return;

	gs_texture_t *tex = NULL;

	tex = obs_get_main_texture();
	if (!tex)
		return;

	uint32_t width = 0;
	uint32_t height = 0;

	width = gs_texture_get_width(tex);
	height = gs_texture_get_height(tex);
	blog(LOG_DEBUG, TAG "got main texture %ux%u", width, height);

	GLuint gl_tex = *(GLuint *)gs_texture_get_obj(tex);
	if (!gl_tex) {
		blog(LOG_ERROR, TAG "failed to get GL texture from main texture");
	}
	blog(LOG_DEBUG, TAG "got GL texture ID %u", gl_tex);

	float lap_var, diff, hamming;
	if (!gate_run(g_gate, gl_tex, &lap_var, &diff, &hamming))
		return;

	/* ── write frame to /dev/shm/i-cant-read.png ─────────────────────── */
	blog(LOG_DEBUG, TAG "gates passed, dumping frame to /dev/shm/i-cant-read.png for python to pick up");
	enum gs_color_format fmt = gs_texture_get_color_format(tex);
	gs_stagesurf_t *surf = gs_stagesurface_create(width, height, fmt);
	if (surf) {
		gs_stage_texture(surf, tex);

		uint8_t *data = NULL;
		uint32_t linesize = 0;

		if (gs_stagesurface_map(surf, &data, &linesize)) {
			size_t row = (size_t)width * 4;
			uint8_t *rgba = bmalloc((size_t)width * height * 4);
			for (uint32_t y = 0; y < height; y++) {
				const uint8_t *src = data + y * linesize;
				uint8_t *dst = rgba + y * row;

				if (fmt == GS_BGRA) {
					for (uint32_t x = 0; x < width; x++) {
						dst[x * 4 + 0] = src[x * 4 + 2]; /* R ← B */
						dst[x * 4 + 1] = src[x * 4 + 1]; /* G */
						dst[x * 4 + 2] = src[x * 4 + 0]; /* B ← R */
						// dst[x * 4 + 3] = src[x * 4 + 3]; /* A */
						dst[x * 4 + 3] = 255; // force opaque alpha
					}
				} else {
					memcpy(dst, src, row); /* already RGBA */
					for (uint32_t x = 0; x < width; x++)
						dst[x * 4 + 3] = 255; /* ignore texture alpha */
				}
			}

			uint8_t *packed = NULL;
			int need_pack = ((uint32_t)linesize != width * 4u);

			if (need_pack) {
				packed = bmalloc((size_t)width * height * 4);
				for (uint32_t y = 0; y < height; y++)
					memcpy(packed + y * width * 4, rgba + y * linesize, width * 4);
			}

			if (!stbi_write_png("/dev/shm/i-cant-read.png", (int)width, (int)height, 4,
					    need_pack ? packed : rgba, need_pack ? (int)(width * 4) : (int)linesize))
				blog(LOG_WARNING, TAG "stbi_write_png failed");

			if (packed)
				bfree(packed);
			bfree(rgba);

			gs_stagesurface_unmap(surf);
		} else {
			blog(LOG_WARNING, TAG "gs_stagesurface_map failed");
		}

		gs_stagesurface_destroy(surf);
	} else {
		blog(LOG_WARNING, TAG "gs_stagesurface_create failed");
	}

	/* Frame passed all gates: notify Python with a single byte */
	blog(LOG_DEBUG, TAG "frame passed gates (lap_var=%.6f diff=%.6f hamming=%u), sending signal to Python", lap_var,
	     diff, (unsigned)hamming);
	if (g_sock >= 0) {
		uint8_t sig = 1;
		send(g_sock, &sig, 1, MSG_DONTWAIT);
	}
}

GateConfig generate_config(void)
{
	obs_data_t *cant_read_settings = cant_read_get_settings();
	GateConfig cfg = {
		/* ── Tune these to match your Python GateConfig ── */
		// .text_lap_threshold = 50.0f,
		.text_lap_threshold = obs_data_get_double(cant_read_settings, "text_lap_threshold"),
		// .diff_threshold = 5.0f,
		.diff_threshold = obs_data_get_double(cant_read_settings, "stability_threshold"),
		.stability_frames = (uint8_t)obs_data_get_int(cant_read_settings, "stability_frames"),
		// .phash_hamming_threshold = 4,
		.phash_hamming_threshold = (uint8_t)obs_data_get_int(cant_read_settings, "hash_threshold"),
		// .black_threshold = 10.0f,
		.black_threshold = (double)obs_data_get_int(cant_read_settings, "black_threshold"),
		// .luma_noise_floor = 3.0f,
		.luma_noise_floor = obs_data_get_double(cant_read_settings, "luma_noise_floor"),
		// .lap_edge_threshold = 10.0f,
		.lap_edge_threshold = obs_data_get_double(cant_read_settings, "lap_carve_edge_threshold"),
		// .lap_dilate_kernel = 5,
		.lap_dilate_kernel = obs_data_get_int(cant_read_settings, "lap_dilate_kernel"),
		.mode = GATE_MODE_BLACK,
		/*
         * swap_rb: set to 1 if colors look wrong.
         * OBS on Linux usually stores RGBA, so OpenCV-style BGR
         * conversions need swap_rb=1.  Verify with a test frame.
         */
		.swap_rb = 1,
	};
	return cfg;
}

bool cant_read_gate_init(void)
{
	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		blog(LOG_ERROR, TAG "no video info");
		return false;
	}

	GateConfig cfg = generate_config();
	blog(LOG_INFO, TAG "initializing gate manager with output resolution %ux%u", ovi.output_width,
	     ovi.output_height);
	obs_enter_graphics();
	g_gate = gate_create(ovi.output_width, ovi.output_height, &cfg);
	obs_leave_graphics();

	if (!g_gate) {
		blog(LOG_ERROR, TAG "gate_create failed (GL 4.3 required)");
		return false;
	}

	/* Datagram socket → Python listener */
	g_sock = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (g_sock >= 0) {
		struct sockaddr_un addr;
		memset(&addr, 0, sizeof addr);
		addr.sun_family = AF_UNIX;
		strncpy(addr.sun_path, GATE_SOCK, sizeof addr.sun_path - 1);
		if (connect(g_sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
			blog(LOG_WARNING, TAG "socket connect: %s", strerror(errno));
			close(g_sock);
			g_sock = -1;
		}
	}

	obs_add_main_rendered_callback(render_cb, NULL);

	blog(LOG_INFO, TAG "loaded %ux%u", ovi.output_width, ovi.output_height);
	return true;
}

void cant_read_gate_deinit(void)
{
	obs_remove_main_rendered_callback(render_cb, NULL);

	if (g_gate) {
		obs_enter_graphics();
		gate_destroy(g_gate);
		obs_leave_graphics();
		g_gate = NULL;
	}

	if (g_sock >= 0) {
		close(g_sock);
		g_sock = -1;
	}
}

void cant_read_gate_reload(void)
{
	if (!g_gate)
		return;
	blog(LOG_INFO, TAG "reloading gate config");
	GateConfig cfg = generate_config();
	gate_update_config(g_gate, &cfg);
	blog(LOG_INFO, TAG "gate config updated");
}
