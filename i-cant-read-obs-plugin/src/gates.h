#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BLACK_MASK_BIT (1 << 0)
#define LAP_MASK_BIT   (1 << 1)
#define SWAP_RB_BIT    (1 << 2)

/*
 * Thresholds use Python-side units:
 *   pixel values / luma : 0–255
 *   lap_var             : (0–255)^2  — same scale as torch.Tensor.var() on float bgr
 *   diff                : 0–255
 */
typedef struct {
	float text_lap_threshold;    /* lap.var() threshold                  */
	float diff_threshold;        /* mean abs-diff threshold; 0 = disabled */
	float change_diff_threshold; /* mean abs-diff threshold for text change gate; 0 = disabled */
	int stability_frames;        /* frames below diff_threshold to pass   */
	float black_threshold;       /* luma cutoff for black mask  */
	float luma_noise_floor;      /* per-pixel diff noise floor           */
	float lap_edge_threshold;    /* |lap| edge binarisation threshold    */
	int lap_dilate_kernel;       /* dilation kernel size (odd int ≥ 1)   */
	int modes;                   /* bitfield: which gates to apply (BLACK_MASK_BIT, LAP_MASK_BIT, SWAP_RB_BIT) */
} GateConfig;

typedef struct GatePipeline GatePipeline; /* opaque */

/* Both must be called from inside obs_enter_graphics() */
GatePipeline *gate_create(uint32_t width, uint32_t height, const GateConfig *cfg);
void gate_destroy(GatePipeline *gp);

/*
 * Run all enabled gates on gl_tex_id (result of gs_texture_get_obj).
 * Must be called from inside obs_enter_graphics().
 * Returns true only when every enabled gate passes.
 * out_* pointers may be NULL.
 */
bool gate_run(GatePipeline *p, unsigned int gl_tex_id, float *out_lap_var, float *out_stability_diff,
	      float *out_change_diff);

void gate_update_config(GatePipeline *p, const GateConfig *cfg);
