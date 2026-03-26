// OBS
#include <obs-module.h>
// Auto-generated shaders
#include "spv_gray_lap_spv.h"
#include "spv_dilate_spv.h"
#include "spv_diff_spv.h"
#include "spv_reduce_spv.h"
// Opengl
#include <glad/gl.h>
// types
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
// src
#include "gates.h"
#include "dhash.h"

#define TAG "[i-cant-read-ocr-gates] "

/* ═══════════════════════════════════════════════════════════════════════
   Pipeline struct (private definition)
   ═══════════════════════════════════════════════════════════════════════ */
struct GatePipeline {
	GateConfig cfg;
	uint32_t width, height;
	uint32_t num_wg, wg_x, wg_y;
	GLuint prog_gray_lap;
	GLuint prog_dilate;
	GLuint prog_diff;
	GLuint prog_reduce;
	GLuint tex_gray_cur, tex_gray_prev; /* GL_R32F */
	GLuint tex_lap;                     /* GL_R32F */
	GLuint tex_mask_cur, tex_mask_prev; /* GL_R8   */
	GLuint tex_mask_dil;                /* GL_R8   */
	GLuint tex_small;                   /* GL_RGBA8, 9x8 for dHash */
	GLuint ssbo_lap_partial, ssbo_lap_result;
	GLuint ssbo_diff_partial, ssbo_diff_result;
	GLuint ubo_gray_lap;
	GLuint ubo_dilate;
	GLuint ubo_diff;
	GLuint ubo_reduce;
	GLuint fbo_src;
	GLuint fbo_small;
	uint64_t prev_hash; // Updated on successful ocr
	bool has_prev;      // Updated every frame
	float norm_lap_var_thr;
	float norm_diff_thr;
	uint8_t stability_frames; // Updated every frame, reset to 0 on diff > norm_diff_thr
	float norm_black_thr;
	float norm_lap_edge;
	float norm_noise_floor;
};

/* ═══════════════════════════════════════════════════════════════════════
   GL helpers
   ═══════════════════════════════════════════════════════════════════════ */

static GLuint compile_spirv(const uint32_t *words, unsigned int word_count)
{
	blog(LOG_DEBUG, TAG "compiling SPIR-V shader with %u words (%u bytes)", word_count, word_count * 4);
	GLuint shader = glCreateShader(GL_COMPUTE_SHADER);

	/* Upload pre-compiled SPIR-V binary */
	blog(LOG_DEBUG, TAG "uploading SPIR-V binary to shader %u", shader);
	glShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, words, (GLsizei)(word_count * sizeof(uint32_t)));

	/*
     * Specialise — selects the entry point and bakes in
     * specialisation constants (none here, but the call is required).
     */
	blog(LOG_DEBUG, TAG "specialising shader %u", shader);

	glSpecializeShader(shader, "main", /* entry point name */
			   0, NULL, NULL);

	blog(LOG_DEBUG, TAG "checking compile status for shader %u", shader);
	GLint ok = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[2048];
		glGetShaderInfoLog(shader, sizeof log, NULL, log);
		blog(LOG_ERROR, TAG "SPIR-V specialise failed:\n%s", log);
		glDeleteShader(shader);
		return 0;
	}

	blog(LOG_DEBUG, TAG "linking program for shader %u", shader);
	GLuint prog = glCreateProgram();
	glAttachShader(prog, shader);
	glLinkProgram(prog);
	glDeleteShader(shader);

	blog(LOG_DEBUG, TAG "checking link status for program %u", prog);
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[2048];
		glGetProgramInfoLog(prog, sizeof log, NULL, log);
		blog(LOG_ERROR, TAG "program link failed:\n%s", log);
		glDeleteProgram(prog);
		return 0;
	}
	blog(LOG_DEBUG, TAG "SPIR-V program %u compiled and linked successfully", prog);
	return prog;
}

static GLuint make_tex(GLsizei w, GLsizei h, GLenum fmt)
{
	GLuint t;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexStorage2D(GL_TEXTURE_2D, 1, fmt, w, h);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); /* LINEAR for small blit */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	return t;
}

static GLuint make_ssbo(size_t bytes)
{
	GLuint b;
	glGenBuffers(1, &b);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, b);
	glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)bytes, NULL, GL_DYNAMIC_COPY);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	return b;
}

static void read_result(GLuint ssbo, float *a, float *b)
{
	float buf[2] = {0.0f, 0.0f};
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
	glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, 2 * sizeof(float), buf);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	*a = buf[0];
	*b = buf[1];
}

static void swap_prev(GatePipeline *p)
{
	GLuint tmp;
	tmp = p->tex_gray_prev;
	p->tex_gray_prev = p->tex_gray_cur;
	p->tex_gray_cur = tmp;
	{
		GLuint eff_cur = (p->cfg.mode == GATE_MODE_LAPCARVE) ? p->tex_mask_dil : p->tex_mask_cur;
		tmp = p->tex_mask_prev;
		p->tex_mask_prev = eff_cur;
		if (p->cfg.mode == GATE_MODE_LAPCARVE)
			p->tex_mask_dil = tmp;
		else
			p->tex_mask_cur = tmp;
	}
	p->has_prev = true;
}

/* ═══════════════════════════════════════════════════════════════════════
   Public API
   ═══════════════════════════════════════════════════════════════════════ */
GatePipeline *gate_create(uint32_t w, uint32_t h, const GateConfig *cfg)
{
	blog(LOG_INFO, TAG "creating pipeline for %ux%u mode=%d", w, h, cfg->mode);
	GatePipeline *p = bzalloc(sizeof *p);
	blog(LOG_DEBUG, TAG "initializing pipeline");
	p->cfg = *cfg;
	p->width = w;
	p->height = h;
	p->norm_lap_var_thr = cfg->text_lap_threshold / (255.0f * 255.0f);
	p->norm_diff_thr = cfg->diff_threshold / 255.0f;
	p->norm_black_thr = cfg->black_threshold / 255.0f;
	p->norm_lap_edge = cfg->lap_edge_threshold / 255.0f;
	p->norm_noise_floor = cfg->luma_noise_floor / 255.0f;
	p->wg_x = (w + 15) / 16;
	p->wg_y = (h + 15) / 16;
	p->num_wg = p->wg_x * p->wg_y;
	p->prog_gray_lap = compile_spirv(spv_gray_lap_data, spv_gray_lap_size);
	p->prog_dilate = compile_spirv(spv_dilate_data, spv_dilate_size);
	p->prog_diff = compile_spirv(spv_diff_data, spv_diff_size);
	p->prog_reduce = compile_spirv(spv_reduce_data, spv_reduce_size);
	if (!p->prog_gray_lap || !p->prog_dilate || !p->prog_diff || !p->prog_reduce) {
		gate_destroy(p);
		return NULL;
	}
	p->tex_gray_cur = make_tex((GLsizei)w, (GLsizei)h, GL_R32F);
	p->tex_gray_prev = make_tex((GLsizei)w, (GLsizei)h, GL_R32F);
	p->tex_lap = make_tex((GLsizei)w, (GLsizei)h, GL_R32F);
	p->tex_mask_cur = make_tex((GLsizei)w, (GLsizei)h, GL_R8);
	p->tex_mask_prev = make_tex((GLsizei)w, (GLsizei)h, GL_R8);
	p->tex_mask_dil = make_tex((GLsizei)w, (GLsizei)h, GL_R8);
	p->tex_small = make_tex(9, 8, GL_RGBA8);
	{
		size_t pair = 2 * sizeof(float);
		p->ssbo_lap_partial = make_ssbo(p->num_wg * pair);
		p->ssbo_lap_result = make_ssbo(pair);
		p->ssbo_diff_partial = make_ssbo(p->num_wg * pair);
		p->ssbo_diff_result = make_ssbo(pair);
	}
	blog(LOG_DEBUG, TAG "pre-allocating UBOs and FBOs");
	glGenBuffers(1, &p->ubo_gray_lap);
	glGenBuffers(1, &p->ubo_dilate);
	glGenBuffers(1, &p->ubo_diff);
	glGenBuffers(1, &p->ubo_reduce);
	/* pre-allocate; contents uploaded per-frame in gate_run */
	glBindBuffer(GL_UNIFORM_BUFFER, p->ubo_gray_lap);
	glBufferData(GL_UNIFORM_BUFFER, 32, NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, p->ubo_dilate);
	glBufferData(GL_UNIFORM_BUFFER, 16, NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, p->ubo_diff);
	glBufferData(GL_UNIFORM_BUFFER, 16, NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, p->ubo_reduce);
	glBufferData(GL_UNIFORM_BUFFER, 16, NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
	glGenFramebuffers(1, &p->fbo_src);
	glGenFramebuffers(1, &p->fbo_small);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, p->fbo_small);
	glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, p->tex_small, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	p->has_prev = false;
	p->prev_hash = 0;
	blog(LOG_INFO, TAG "pipeline created %ux%u mode=%d num_wg=%u", w, h, cfg->mode, p->num_wg);
	return p;
}

void gate_update_config(GatePipeline *p, const GateConfig *cfg)
{
	p->cfg = *cfg;
	p->norm_lap_var_thr = cfg->text_lap_threshold / (255.0f * 255.0f);
	p->norm_diff_thr = cfg->diff_threshold / 255.0f;
	p->norm_black_thr = cfg->black_threshold / 255.0f;
	p->norm_lap_edge = cfg->lap_edge_threshold / 255.0f;
	p->norm_noise_floor = cfg->luma_noise_floor / 255.0f;
}

void gate_destroy(GatePipeline *p)
{
	if (!p)
		return;
	{
		GLuint texs[] = {p->tex_gray_cur,  p->tex_gray_prev, p->tex_lap,  p->tex_mask_cur,
				 p->tex_mask_prev, p->tex_mask_dil,  p->tex_small};
		glDeleteTextures(7, texs);
	}
	{
		GLuint bufs[] = {p->ssbo_lap_partial, p->ssbo_lap_result, p->ssbo_diff_partial, p->ssbo_diff_result};
		glDeleteBuffers(4, bufs);
	}
	GLuint ubos[] = {p->ubo_gray_lap, p->ubo_dilate, p->ubo_diff, p->ubo_reduce};
	glDeleteBuffers(4, ubos);
	glDeleteFramebuffers(1, &p->fbo_src);
	glDeleteFramebuffers(1, &p->fbo_small);
	if (p->prog_gray_lap)
		glDeleteProgram(p->prog_gray_lap);
	if (p->prog_dilate)
		glDeleteProgram(p->prog_dilate);
	if (p->prog_diff)
		glDeleteProgram(p->prog_diff);
	if (p->prog_reduce)
		glDeleteProgram(p->prog_reduce);
	bfree(p);
}

bool gate_run(GatePipeline *p, unsigned int gl_tex_id, float *out_lap_var, float *out_diff, float *out_hamming)
{
	bool result = false;
	/* ── Save GL state ───────────────────────────────────────────────── */
	blog(LOG_DEBUG, TAG "Saving obs GL state");
	GLint saved_program, saved_active_tex;
	GLint saved_read_fbo, saved_draw_fbo;
	GLint saved_tex[4];
	glGetIntegerv(GL_CURRENT_PROGRAM, &saved_program);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &saved_active_tex);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &saved_read_fbo);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved_draw_fbo);
	for (int i = 0; i < 4; i++) {
		glActiveTexture(GL_TEXTURE0 + i);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved_tex[i]);
	}
	// Start gate run
	blog(LOG_DEBUG, TAG "Running gate on texture %u", gl_tex_id);
	glActiveTexture(GL_TEXTURE0);
	/* ── Pass 1: gray + laplacian + mask + partial lap sums ─────────── */
	blog(LOG_DEBUG, TAG "Pass 1: gray+lap+mask partial sums");
	glUseProgram(p->prog_gray_lap);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, (GLuint)gl_tex_id);
	glUniform1i(0, 0); /* sampler location still works */
	/* bind image outputs (binding= matches layout in gray_lap.comp) */
	glBindImageTexture(0,               // binding = 0 in gray_lap.comp
			   p->tex_gray_cur, // output texture id
			   0,               // mipmap level
			   GL_FALSE,        // layered=false, single texture
			   0,               // layer=0 since not layered
			   GL_WRITE_ONLY,   // Allow shader to write to p->tex_gray_cur
			   GL_R32F          // 32-bit single-channel floats (red channel only)
	);
	glBindImageTexture(1, p->tex_lap, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
	glBindImageTexture(2, p->tex_mask_cur, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
	/* bind SSBO for partial lap sums (binding=0 in gray_lap.comp) */
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, p->ssbo_lap_partial);
	{
		struct {
			int w, h;
			float lap_edge, black;
			int mode, swap_rb;
			float p0, p1;
		} u = {(int)p->width,
		       (int)p->height,
		       p->norm_lap_edge,
		       p->norm_black_thr,
		       p->cfg.mode,
		       p->cfg.swap_rb,
		       0.0f,
		       0.0f};
		glBindBuffer(GL_UNIFORM_BUFFER, p->ubo_gray_lap);
		glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof u, &u);
		glBindBuffer(GL_UNIFORM_BUFFER, 0);
		glBindBufferBase(GL_UNIFORM_BUFFER, 1, p->ubo_gray_lap);
	}
	glDispatchCompute(p->wg_x, p->wg_y, 1);
	glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT |
			GL_SHADER_STORAGE_BARRIER_BIT);
	/* ── Pass 1b: dilation (lapcarve only) ───────────────────────────── */
	if (p->cfg.mode == GATE_MODE_LAPCARVE) {
		blog(LOG_DEBUG, TAG "Pass 1.5: lapcarve");
		glUseProgram(p->prog_dilate);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, p->tex_mask_cur);
		glUniform1i(0, 0);
		/* bind image output (binding=0 in dilate.comp) */
		glBindImageTexture(0, p->tex_mask_dil, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R8);
		{
			struct {
				int radius, w, h, pad;
			} u = {p->cfg.lap_dilate_kernel > 1 ? p->cfg.lap_dilate_kernel / 2 : 1, (int)p->width,
			       (int)p->height, 0};
			glBindBuffer(GL_UNIFORM_BUFFER, p->ubo_dilate);
			glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof u, &u);
			glBindBuffer(GL_UNIFORM_BUFFER, 0);
			glBindBufferBase(GL_UNIFORM_BUFFER, 1, p->ubo_dilate);
		}
		glDispatchCompute(p->wg_x, p->wg_y, 1);
		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
	}
	/* ── Pass 2: reduce lap partial sums ─────────────────────────────── */
	blog(LOG_DEBUG, TAG "Pass 2: reduce lap sums");
	glUseProgram(p->prog_reduce);
	/* reduce.comp uses location=0 plain uniform for count, NOT a UBO */
	glUniform1ui(0, p->num_wg);
	/* bind SSBOs: binding=0 input (partial), binding=1 output (result) */
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, p->ssbo_lap_partial);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, p->ssbo_lap_result);
	glDispatchCompute(1, 1, 1);
	glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
	{
		float lap_sum, lap_sum_sq;
		read_result(p->ssbo_lap_result, &lap_sum, &lap_sum_sq);
		float n = (float)(p->width * p->height);
		float lap_mean = lap_sum / n;
		float lap_var = lap_sum_sq / n - lap_mean * lap_mean;
		if (out_lap_var)
			*out_lap_var = lap_var;
		if (lap_var <= p->norm_lap_var_thr) {
			swap_prev(p);
			blog(LOG_DEBUG, TAG "laplacian variance %.6f below threshold %.6f, skipping further gates",
			     lap_var, p->norm_lap_var_thr);
			goto done; /* result stays false */
		}
	}
	/* ── Pass 3: frame diff ──────────────────────────────────────────── */
	if (p->has_prev) {
		blog(LOG_DEBUG, TAG "Pass 3: frame diff");
		GLuint mask_cur = (p->cfg.mode == GATE_MODE_LAPCARVE) ? p->tex_mask_dil : p->tex_mask_cur;
		float diff_sum, diff_count, mean_diff;
		glUseProgram(p->prog_diff);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, p->tex_gray_cur);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, p->tex_gray_prev);
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, mask_cur);
		glActiveTexture(GL_TEXTURE3);
		glBindTexture(GL_TEXTURE_2D, p->tex_mask_prev);
		/* sampler locations 0-3, scalar uniforms 4-7 (match layout(location=N) in diff.comp) */
		glUniform1i(0, 0);
		glUniform1i(1, 1);
		glUniform1i(2, 2);
		glUniform1i(3, 3);
		glUniform1i(4, (GLint)p->width);
		glUniform1i(5, (GLint)p->height);
		glUniform1i(6, p->cfg.mode);
		glUniform1f(7, p->norm_noise_floor);
		/* bind SSBO for partial diff sums (binding=1 in diff.comp) */
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, p->ssbo_diff_partial);
		glDispatchCompute(p->wg_x, p->wg_y, 1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
		/* reduce diff partial sums (same prog_reduce as lap, plain uniform for count) */
		glUseProgram(p->prog_reduce);
		glUniform1ui(0, p->num_wg);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, p->ssbo_diff_partial);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, p->ssbo_diff_result);
		glDispatchCompute(1, 1, 1);
		glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
		read_result(p->ssbo_diff_result, &diff_sum, &diff_count);
		mean_diff = (diff_count > 0.0f) ? diff_sum / diff_count : 0.0f;
		if (out_diff)
			*out_diff = mean_diff;
		// fail if frame became unstable or if not enough stable frames yet
		if (p->cfg.diff_threshold > 0.0f) {
			if (mean_diff >= p->norm_diff_thr) {
				p->stability_frames = 0;
				blog(LOG_DEBUG,
				     TAG
				     "frame unstable (diff %.6f above threshold %.6f), resetting stability_frames to 0",
				     mean_diff, p->norm_diff_thr);
			}
			if (p->stability_frames < p->cfg.stability_frames) {
				p->stability_frames++;
				blog(LOG_DEBUG, TAG "stability_frames %u/%u", p->stability_frames,
				     p->cfg.stability_frames);
				swap_prev(p);
				goto done; /* result stays false */
			}
		}
	} else {
		blog(LOG_DEBUG, TAG "No previous frame, skipping diff");
	}
	/* ── Gate 3: dHash ───────────────────────────────────────────────── */
	if (p->cfg.phash_hamming_threshold > 0) {
		blog(LOG_DEBUG, TAG "Pass 4: dHash");
		uint8_t pixels[9 * 8 * 4];
		uint8_t gray72[72];
		uint64_t cur_hash;
		int hamming;
		glBindFramebuffer(GL_READ_FRAMEBUFFER, p->fbo_src);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (GLuint)gl_tex_id, 0);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, p->fbo_small);
		glDrawBuffer(GL_COLOR_ATTACHMENT0);
		glBlitFramebuffer(0, 0, (GLint)p->width, (GLint)p->height, 0, 0, 9, 8, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, p->fbo_small);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
		glReadPixels(0, 0, 9, 8, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		for (int i = 0; i < 72; i++) {
			int r = pixels[i * 4 + 0], g = pixels[i * 4 + 1], b = pixels[i * 4 + 2];
			if (p->cfg.swap_rb) {
				int t = r;
				r = b;
				b = t;
			}
			gray72[i] = (uint8_t)((299 * r + 587 * g + 114 * b) / 1000);
		}
		cur_hash = dhash_compute_9x8(gray72);
		hamming = p->prev_hash != 0 ? dhash_hamming64(cur_hash, p->prev_hash) : 64;
		if (out_hamming)
			*out_hamming = (float)hamming;
		if (p->prev_hash != 0 && hamming <= p->cfg.phash_hamming_threshold) {
			swap_prev(p);
			blog(LOG_DEBUG, TAG "dHash Hamming distance %d below threshold %d, failing gate", hamming,
			     p->cfg.phash_hamming_threshold);
			goto done;
		}
		blog(LOG_DEBUG, TAG "Updating dHash (hamming %d above threshold %d)", hamming,
		     p->cfg.phash_hamming_threshold);
		p->prev_hash = cur_hash;
	} else {
		blog(LOG_DEBUG, TAG "dHash disabled, skipping");
	}
	swap_prev(p);
	result = true;
done:
	/* ── Restore GL state ────────────────────────────────────────────── */
	glUseProgram((GLuint)saved_program);
	for (int i = 0; i < 4; i++) {
		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(GL_TEXTURE_2D, (GLuint)saved_tex[i]);
	}
	glActiveTexture((GLenum)saved_active_tex);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)saved_read_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)saved_draw_fbo);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
	blog(LOG_DEBUG, TAG "lap_var=%.6f diff=%.6f hamming=%u result=%s", out_lap_var ? *out_lap_var : 0.0f,
	     out_diff ? *out_diff : 0.0f, out_hamming ? (unsigned int)*out_hamming : 0, result ? "pass" : "fail");
	return result;
}