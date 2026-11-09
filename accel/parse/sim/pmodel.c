// C model of acc_parse (step 2 of 4.0): from what the ARM would give the
// block for each CABAC slice (descriptor + slice data, written by refcmd from
// <prefix>.sl), it reads every macroblock (CABAC, macroblock syntax, motion
// vector and direct prediction, intra modes, dequantisation, DC transforms)
// and writes the engine commands of each macroblock (A5 + A3 + coefficients,
// accel/doc/comandi.md). They must be identical, word for word, to the ones
// the ARM writes today (<prefix>.cmd, from FFmpeg with the player's patch).
//
// Progressive frames, 4:2:0, 8 bit, CABAC only (what the player sends to the
// FPGA). The logic follows FFmpeg's H.264 decoder (libavcodec/h264_cabac.c,
// h264_mvpred.h, h264_direct.c, h264_parse.c, h264idct_template.c; LGPL 2.1
// or later, Michael Niedermayer and others), with its caches, so that every
// word matches; MBAFF and field paths are left out.
//
// Usage: pmodel <prefix> [name]
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "cab_tables.h"

// ---------------------------------------------------------------- constants

#define MB_TYPE_INTRA4x4   (1 << 0)
#define MB_TYPE_INTRA16x16 (1 << 1)
#define MB_TYPE_INTRA_PCM  (1 << 2)
#define MB_TYPE_16x16      (1 << 3)
#define MB_TYPE_16x8       (1 << 4)
#define MB_TYPE_8x16       (1 << 5)
#define MB_TYPE_8x8        (1 << 6)
#define MB_TYPE_DIRECT2    (1 << 8)
#define MB_TYPE_SKIP       (1 << 11)
#define MB_TYPE_P0L0       (1 << 12)
#define MB_TYPE_P1L0       (1 << 13)
#define MB_TYPE_P0L1       (1 << 14)
#define MB_TYPE_P1L1       (1 << 15)
#define MB_TYPE_L0         (MB_TYPE_P0L0 | MB_TYPE_P1L0)
#define MB_TYPE_L1         (MB_TYPE_P0L1 | MB_TYPE_P1L1)
#define MB_TYPE_L0L1       (MB_TYPE_L0 | MB_TYPE_L1)
#define MB_TYPE_8x8DCT     0x01000000

#define IS_INTRA4x4(a)   ((a) & MB_TYPE_INTRA4x4)
#define IS_INTRA16x16(a) ((a) & MB_TYPE_INTRA16x16)
#define IS_INTRA_PCM(a)  ((a) & MB_TYPE_INTRA_PCM)
#define IS_INTRA(a)      ((a) & 7)
#define IS_INTER(a)      ((a) & (MB_TYPE_16x16 | MB_TYPE_16x8 | MB_TYPE_8x16 | MB_TYPE_8x8))
#define IS_SKIP(a)       ((a) & MB_TYPE_SKIP)
#define IS_DIRECT(a)     ((a) & MB_TYPE_DIRECT2)
#define IS_16X16(a)      ((a) & MB_TYPE_16x16)
#define IS_16X8(a)       ((a) & MB_TYPE_16x8)
#define IS_8X16(a)       ((a) & MB_TYPE_8x16)
#define IS_8X8(a)        ((a) & MB_TYPE_8x8)
#define IS_SUB_8X8(a)    ((a) & MB_TYPE_16x16)
#define IS_SUB_8X4(a)    ((a) & MB_TYPE_16x8)
#define IS_SUB_4X8(a)    ((a) & MB_TYPE_8x16)
#define IS_DIR(a, part, list) ((a) & (MB_TYPE_P0L0 << ((part) + 2 * (list))))
#define USES_LIST(a, list) ((a) & ((MB_TYPE_P0L0 | MB_TYPE_P1L0) << (2 * (list))))
#define IS_8x8DCT(a)     ((a) & MB_TYPE_8x8DCT)

#define LIST_NOT_USED -1
#define PART_NOT_AVAILABLE -2
#define LUMA_DC_BLOCK_INDEX 48
#define CHROMA_DC_BLOCK_INDEX 49

#define DC_PRED 2
#define LEFT_DC_PRED 9
#define TOP_DC_PRED 10
#define DC_128_PRED 11
#define LEFT_DC_PRED8x8 4
#define TOP_DC_PRED8x8 5
#define DC_128_PRED8x8 6
#define ALZHEIMER_DC_L0T_PRED8x8 7

static const uint8_t scan8[16 * 3 + 3] = {
	4 +  1 * 8, 5 +  1 * 8, 4 +  2 * 8, 5 +  2 * 8,
	6 +  1 * 8, 7 +  1 * 8, 6 +  2 * 8, 7 +  2 * 8,
	4 +  3 * 8, 5 +  3 * 8, 4 +  4 * 8, 5 +  4 * 8,
	6 +  3 * 8, 7 +  3 * 8, 6 +  4 * 8, 7 +  4 * 8,
	4 +  6 * 8, 5 +  6 * 8, 4 +  7 * 8, 5 +  7 * 8,
	6 +  6 * 8, 7 +  6 * 8, 6 +  7 * 8, 7 +  7 * 8,
	4 +  8 * 8, 5 +  8 * 8, 4 +  9 * 8, 5 +  9 * 8,
	6 +  8 * 8, 7 +  8 * 8, 6 +  9 * 8, 7 +  9 * 8,
	4 + 11 * 8, 5 + 11 * 8, 4 + 12 * 8, 5 + 12 * 8,
	6 + 11 * 8, 7 + 11 * 8, 6 + 12 * 8, 7 + 12 * 8,
	4 + 13 * 8, 5 + 13 * 8, 4 + 14 * 8, 5 + 14 * 8,
	6 + 13 * 8, 7 + 13 * 8, 6 + 14 * 8, 7 + 14 * 8,
	0 +  0 * 8, 0 +  5 * 8, 0 + 10 * 8
};

typedef struct { uint32_t type; int8_t pred_mode; uint8_t cbp; } IMbInfo;
typedef struct { uint32_t type; uint8_t partition_count; } PMbInfo;

static const IMbInfo i_mb_type_info[26] = {
	{ MB_TYPE_INTRA4x4,  -1,  0 },
	{ MB_TYPE_INTRA16x16, 2,   0 }, { MB_TYPE_INTRA16x16, 1,   0 }, { MB_TYPE_INTRA16x16, 0,   0 }, { MB_TYPE_INTRA16x16, 3,   0 },
	{ MB_TYPE_INTRA16x16, 2,  16 }, { MB_TYPE_INTRA16x16, 1,  16 }, { MB_TYPE_INTRA16x16, 0,  16 }, { MB_TYPE_INTRA16x16, 3,  16 },
	{ MB_TYPE_INTRA16x16, 2,  32 }, { MB_TYPE_INTRA16x16, 1,  32 }, { MB_TYPE_INTRA16x16, 0,  32 }, { MB_TYPE_INTRA16x16, 3,  32 },
	{ MB_TYPE_INTRA16x16, 2,  15 }, { MB_TYPE_INTRA16x16, 1,  15 }, { MB_TYPE_INTRA16x16, 0,  15 }, { MB_TYPE_INTRA16x16, 3,  15 },
	{ MB_TYPE_INTRA16x16, 2,  31 }, { MB_TYPE_INTRA16x16, 1,  31 }, { MB_TYPE_INTRA16x16, 0,  31 }, { MB_TYPE_INTRA16x16, 3,  31 },
	{ MB_TYPE_INTRA16x16, 2,  47 }, { MB_TYPE_INTRA16x16, 1,  47 }, { MB_TYPE_INTRA16x16, 0,  47 }, { MB_TYPE_INTRA16x16, 3,  47 },
	{ MB_TYPE_INTRA_PCM,  -1,  0 },
};
static const PMbInfo p_mb_type_info[4] = {
	{ MB_TYPE_16x16 | MB_TYPE_P0L0, 1 },
	{ MB_TYPE_16x8  | MB_TYPE_P0L0 | MB_TYPE_P1L0, 2 },
	{ MB_TYPE_8x16  | MB_TYPE_P0L0 | MB_TYPE_P1L0, 2 },
	{ MB_TYPE_8x8   | MB_TYPE_P0L0 | MB_TYPE_P1L0, 4 },
};
static const PMbInfo p_sub_mb_type_info[4] = {
	{ MB_TYPE_16x16 | MB_TYPE_P0L0, 1 },
	{ MB_TYPE_16x8  | MB_TYPE_P0L0, 2 },
	{ MB_TYPE_8x16  | MB_TYPE_P0L0, 2 },
	{ MB_TYPE_8x8   | MB_TYPE_P0L0, 4 },
};
static const PMbInfo b_mb_type_info[23] = {
	{ MB_TYPE_DIRECT2 | MB_TYPE_L0L1, 1 },
	{ MB_TYPE_16x16 | MB_TYPE_P0L0, 1 },
	{ MB_TYPE_16x16 | MB_TYPE_P0L1, 1 },
	{ MB_TYPE_16x16 | MB_TYPE_P0L0 | MB_TYPE_P0L1, 1 },
	{ MB_TYPE_16x8 | MB_TYPE_P0L0 | MB_TYPE_P1L0, 2 },
	{ MB_TYPE_8x16 | MB_TYPE_P0L0 | MB_TYPE_P1L0, 2 },
	{ MB_TYPE_16x8 | MB_TYPE_P0L1 | MB_TYPE_P1L1, 2 },
	{ MB_TYPE_8x16 | MB_TYPE_P0L1 | MB_TYPE_P1L1, 2 },
	{ MB_TYPE_16x8 | MB_TYPE_P0L0 | MB_TYPE_P1L1, 2 },
	{ MB_TYPE_8x16 | MB_TYPE_P0L0 | MB_TYPE_P1L1, 2 },
	{ MB_TYPE_16x8 | MB_TYPE_P0L1 | MB_TYPE_P1L0, 2 },
	{ MB_TYPE_8x16 | MB_TYPE_P0L1 | MB_TYPE_P1L0, 2 },
	{ MB_TYPE_16x8 | MB_TYPE_P0L0 | MB_TYPE_P1L0 | MB_TYPE_P1L1, 2 },
	{ MB_TYPE_8x16 | MB_TYPE_P0L0 | MB_TYPE_P1L0 | MB_TYPE_P1L1, 2 },
	{ MB_TYPE_16x8 | MB_TYPE_P0L1 | MB_TYPE_P1L0 | MB_TYPE_P1L1, 2 },
	{ MB_TYPE_8x16 | MB_TYPE_P0L1 | MB_TYPE_P1L0 | MB_TYPE_P1L1, 2 },
	{ MB_TYPE_16x8 | MB_TYPE_P0L0 | MB_TYPE_P0L1 | MB_TYPE_P1L0, 2 },
	{ MB_TYPE_8x16 | MB_TYPE_P0L0 | MB_TYPE_P0L1 | MB_TYPE_P1L0, 2 },
	{ MB_TYPE_16x8 | MB_TYPE_P0L0 | MB_TYPE_P0L1 | MB_TYPE_P1L1, 2 },
	{ MB_TYPE_8x16 | MB_TYPE_P0L0 | MB_TYPE_P0L1 | MB_TYPE_P1L1, 2 },
	{ MB_TYPE_16x8 | MB_TYPE_P0L0 | MB_TYPE_P0L1 | MB_TYPE_P1L0 | MB_TYPE_P1L1, 2 },
	{ MB_TYPE_8x16 | MB_TYPE_P0L0 | MB_TYPE_P0L1 | MB_TYPE_P1L0 | MB_TYPE_P1L1, 2 },
	{ MB_TYPE_8x8 | MB_TYPE_P0L0 | MB_TYPE_P0L1 | MB_TYPE_P1L0 | MB_TYPE_P1L1, 4 },
};
static const PMbInfo b_sub_mb_type_info[13] = {
	{ MB_TYPE_DIRECT2, 1 },
	{ MB_TYPE_16x16 | MB_TYPE_P0L0, 1 },
	{ MB_TYPE_16x16 | MB_TYPE_P0L1, 1 },
	{ MB_TYPE_16x16 | MB_TYPE_P0L0 | MB_TYPE_P0L1, 1 },
	{ MB_TYPE_16x8 | MB_TYPE_P0L0 | MB_TYPE_P1L0, 2 },
	{ MB_TYPE_8x16 | MB_TYPE_P0L0 | MB_TYPE_P1L0, 2 },
	{ MB_TYPE_16x8 | MB_TYPE_P0L1 | MB_TYPE_P1L1, 2 },
	{ MB_TYPE_8x16 | MB_TYPE_P0L1 | MB_TYPE_P1L1, 2 },
	{ MB_TYPE_16x8 | MB_TYPE_P0L0 | MB_TYPE_P0L1 | MB_TYPE_P1L0 | MB_TYPE_P1L1, 2 },
	{ MB_TYPE_8x16 | MB_TYPE_P0L0 | MB_TYPE_P0L1 | MB_TYPE_P1L0 | MB_TYPE_P1L1, 2 },
	{ MB_TYPE_8x8 | MB_TYPE_P0L0 | MB_TYPE_P1L0, 4 },
	{ MB_TYPE_8x8 | MB_TYPE_P0L1 | MB_TYPE_P1L1, 4 },
	{ MB_TYPE_8x8 | MB_TYPE_P0L0 | MB_TYPE_P0L1 | MB_TYPE_P1L0 | MB_TYPE_P1L1, 4 },
};

static const uint8_t dequant4_init[6][3] = {
	{ 10, 13, 16 }, { 11, 14, 18 }, { 13, 16, 20 }, { 14, 18, 23 }, { 16, 20, 25 }, { 18, 23, 29 },
};
static const uint8_t dequant8_init_scan[16] = { 0, 3, 4, 3, 3, 1, 5, 1, 4, 5, 2, 5, 3, 1, 5, 1 };
static const uint8_t dequant8_init[6][6] = {
	{ 20, 18, 32, 19, 25, 24 }, { 22, 19, 35, 21, 28, 26 }, { 26, 23, 42, 24, 33, 31 },
	{ 28, 25, 45, 26, 35, 33 }, { 32, 28, 51, 30, 40, 38 }, { 36, 32, 58, 34, 46, 43 },
};
static const uint8_t chroma_qp_tab[52] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
	29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36, 36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39
};
static const uint8_t zigzag_scan4[16] = {
	0 + 0 * 4, 1 + 0 * 4, 0 + 1 * 4, 0 + 2 * 4, 1 + 1 * 4, 2 + 0 * 4, 3 + 0 * 4, 2 + 1 * 4,
	1 + 2 * 4, 0 + 3 * 4, 1 + 3 * 4, 2 + 2 * 4, 3 + 1 * 4, 3 + 2 * 4, 2 + 3 * 4, 3 + 3 * 4,
};
static const uint8_t zigzag_direct[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};
static const uint8_t chroma_dc_scan[4] = { 0, 16, 32, 48 };
static uint8_t zz4[16], zz8[64];  // FFmpeg's transposed scans

// ---------------------------------------------------------------- input

typedef struct {
	int pic, slot, mb_w, mb_h, mb_x, mb_y, slice_num, type, qp, idc, direct_spatial;
	int ref_count[2], list_count, t8x8, constrained_intra, deblock, alpha, beta, cqp_off[2];
	int use_weight, direct_8x8_inference, x264_gt33, l1_long;
	int slots[2][32], dist_scale[32], map_col[2][48], implicit[32][32][2];
	uint8_t sm4[6][16], sm8[6][64];
	uint8_t *data;
	unsigned nb;
} Desc;

static int rd32(FILE *f, int32_t *v) { return fread(v, 4, 1, f) == 1; }

static int read_desc(FILE *f, Desc *d)
{
	int32_t v, *p;
	if (!rd32(f, &v)) return 0;
	if (v == 0x50494331) return 2; // stream mode: the ARM's commands of the picture just decoded
	if (v != 0x534C0001) { fprintf(stderr, "pmodel: bad record %08x\n", v); exit(1); }
	int32_t h[25];
	if (fread(h, 4, 25, f) != 25) return 0;
	p = h;
	d->pic = *p++; d->slot = *p++; d->mb_w = *p++; d->mb_h = *p++; d->mb_x = *p++; d->mb_y = *p++;
	d->slice_num = *p++; d->type = *p++; d->qp = *p++; d->idc = *p++; d->direct_spatial = *p++;
	d->ref_count[0] = *p++; d->ref_count[1] = *p++; d->list_count = *p++; d->t8x8 = *p++;
	d->constrained_intra = *p++; d->deblock = *p++; d->alpha = *p++; d->beta = *p++;
	d->cqp_off[0] = *p++; d->cqp_off[1] = *p++; d->use_weight = *p++; d->direct_8x8_inference = *p++;
	d->x264_gt33 = *p++; d->l1_long = *p++;
	for (int l = 0; l < 2; l++) for (int i = 0; i < 32; i++) { rd32(f, &v); d->slots[l][i] = v; }
	for (int i = 0; i < 32; i++) { rd32(f, &v); d->dist_scale[i] = v; }
	for (int l = 0; l < 2; l++) for (int i = 0; i < 48; i++) { rd32(f, &v); d->map_col[l][i] = v; }
	for (int a = 0; a < 32; a++) for (int b = 0; b < 32; b++) { rd32(f, &v); d->implicit[a][b][0] = (int16_t)(v & 0xFFFF); d->implicit[a][b][1] = (int16_t)(v >> 16); }
	for (int i = 0; i < 6; i++) for (int j = 0; j < 16; j++) { rd32(f, &v); d->sm4[i][j] = v; }
	for (int i = 0; i < 6; i++) for (int j = 0; j < 64; j++) { rd32(f, &v); d->sm8[i][j] = v; }
	rd32(f, &v);
	d->nb = v;
	free(d->data);
	d->data = malloc(d->nb + 8);
	unsigned pad = (d->nb + 3) & ~3u;
	if (fread(d->data, 1, pad, f) != pad) return 0;
	return 1;
}

// ---------------------------------------------------------------- pictures

// one picture's macroblock data, FFmpeg's layout (mb_stride = mb_w + 1,
// b_stride = 4 mb_w); the arrays start before index 0 (top neighbours)
typedef struct {
	int mb_w, mb_h, mb_stride, b_stride, valid;
	uint32_t *mb_type_base, *mb_type;
	uint16_t *slice_table_base, *slice_table;
	uint8_t *qscale_table_base, *qscale_table;
	int16_t (*mv_base[2])[2], (*motion_val[2])[2];
	int8_t *ref_base[2], *ref_index[2];
} Pic;

static Pic pics[256];

static void pic_alloc(Pic *p, int mb_w, int mb_h)
{
	if (p->valid && p->mb_w == mb_w && p->mb_h == mb_h) return;
	if (p->valid)
	{
		free(p->mb_type_base); free(p->slice_table_base); free(p->qscale_table_base);
		for (int l = 0; l < 2; l++) { free(p->mv_base[l]); free(p->ref_base[l]); }
	}
	p->mb_w = mb_w;
	p->mb_h = mb_h;
	p->mb_stride = mb_w + 1;
	p->b_stride = mb_w * 4;
	int n = (mb_h + 4) * p->mb_stride, off = 2 * p->mb_stride + 1;
	p->mb_type_base = calloc(n, 4);
	p->mb_type = p->mb_type_base + off;
	p->slice_table_base = malloc(n * 2);
	p->slice_table = p->slice_table_base + off;
	p->qscale_table_base = calloc(n, 1);
	p->qscale_table = p->qscale_table_base + off;
	for (int l = 0; l < 2; l++)
	{
		int nb = 16 * (mb_h + 4) * (mb_w + 4);
		p->mv_base[l] = calloc(nb, 4);
		p->motion_val[l] = p->mv_base[l] + 4 * (mb_w * 4) + 4;
		p->ref_base[l] = calloc(4 * n, 1);
		p->ref_index[l] = p->ref_base[l] + 4 * off;
	}
	p->valid = 1;
}

// ---------------------------------------------------------------- state

static Desc D;
static Pic *cur;
static int mb_stride, b_stride, mb_w, mb_h;

// per picture tables, by macroblock
static uint8_t (*non_zero_count)[48];
static uint16_t *cbp_table;
static uint8_t *chroma_pred_mode_table;
static int8_t (*intra4x4_pred_mode)[8];
static uint8_t (*mvd_table[2])[8][2];
static uint8_t *direct_table;
static int ntab;

// slice context
static struct {
	int mb_x, mb_y, mb_xy, slice_num, slice_type_nos, qscale, chroma_qp[2], last_qscale_diff;
	int ref_count[2], list_count, direct_spatial_mv_pred, deblocking_filter;
	int topleft_mb_xy, top_mb_xy, topright_mb_xy, left_mb_xy[2];
	int topleft_type, top_type, topright_type, left_type[2];
	const uint8_t *left_block;
	int topleft_partition;
	int top_samples_available, left_samples_available, topleft_samples_available, topright_samples_available;
	int intra16x16_pred_mode, chroma_pred_mode, cbp, top_cbp, left_cbp, neighbor_transform_size;
	int8_t intra4x4_pred_mode_cache[5 * 8];
	uint8_t non_zero_count_cache[15 * 8];
	int16_t mv_cache[2][5 * 8][2];
	uint8_t mvd_cache[2][5 * 8][2];
	int8_t ref_cache[2][5 * 8];
	uint8_t direct_cache[5 * 8];
	uint16_t sub_mb_type[4];
	int16_t mb[16 * 48];
	int16_t mb_luma_dc[16];
	const uint8_t *intra_pcm_ptr;
	uint8_t cabac_state[1024];
} sl;

// pps tables
static uint32_t dequant4[6][52][16], dequant8[6][52][64];
static uint8_t cqp_table[2][52];

// ---------------------------------------------------------------- CABAC engine (standard, 9.3)

static const uint8_t *bs;
static uint64_t bitpos, nbits;
static unsigned range, offset;

static int rdbit(void)
{
	unsigned b = bitpos < nbits ? (bs[bitpos >> 3] >> (7 - (bitpos & 7))) & 1 : 0;
	bitpos++;
	return b;
}

static void engine_init(void)
{
	range = 510;
	offset = 0;
	for (int i = 0; i < 9; i++) offset = offset << 1 | rdbit();
}

static int get_cabac(uint8_t *st)
{
	int p = *st >> 1, mps = *st & 1, bin;
	unsigned rlps = cab_rlps[p][(range >> 6) & 3];
	range -= rlps;
	if (offset >= range)
	{
		bin = !mps;
		offset -= range;
		range = rlps;
		if (p == 0) mps = !mps;
		p = cab_tlps[p];
	}
	else
	{
		bin = mps;
		p = cab_tmps[p];
	}
	*st = (uint8_t)(p << 1 | mps);
	while (range < 256)
	{
		range <<= 1;
		offset = offset << 1 | rdbit();
	}
	return bin;
}

static int get_cabac_bypass(void)
{
	offset = offset << 1 | rdbit();
	if (offset >= range) { offset -= range; return 1; }
	return 0;
}

static int get_cabac_terminate(void)
{
	range -= 2;
	if (offset >= range) return 1;
	while (range < 256)
	{
		range <<= 1;
		offset = offset << 1 | rdbit();
	}
	return 0;
}

static void ctx_init(void)
{
	int set = sl.slice_type_nos == 2 ? 0 : 1 + D.idc;
	int q = sl.qscale < 0 ? 0 : sl.qscale > 51 ? 51 : sl.qscale;
	for (int i = 0; i < 1024; i++)
	{
		int pre = ((cab_init[set][i][0] * q) >> 4) + cab_init[set][i][1];
		pre = pre < 1 ? 1 : pre > 126 ? 126 : pre;
		sl.cabac_state[i] = pre <= 63 ? (uint8_t)((63 - pre) << 1) : (uint8_t)(((pre - 64) << 1) | 1);
	}
}

// ---------------------------------------------------------------- helpers

static void fill_rectangle(void *vp, int w, int h, int stride, uint32_t val, int size)
{
	uint8_t *p = vp;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
		{
			if (size == 1) p[x + y * stride] = (uint8_t)val;
			else if (size == 2) memcpy(p + 2 * (x + y * stride), &val, 2);
			else memcpy(p + 4 * (x + y * stride), &val, 4);
		}
}

static uint32_t pack16to32(int a, int b) { return (uint16_t)a | (uint32_t)(uint16_t)b << 16; }
static uint16_t pack8to16(int a, int b) { return (uint8_t)a | (uint16_t)(uint8_t)b << 8; }
static int mid_pred(int a, int b, int c)
{
	if (a > b) { if (c > b) { if (c > a) b = a; else b = c; } }
	else { if (b > c) { if (c > a) b = c; else b = a; } }
	return b;
}
static int iabs(int a) { return a < 0 ? -a : a; }
static uint32_t rn32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int get_chroma_qp(int t, int q) { return cqp_table[t][q]; }
#define FFMIN(a, b) ((a) < (b) ? (a) : (b))

#include "pmodel_mb.h"

// ---------------------------------------------------------------- output (the ARM's h264_mcacc.c / h264_mcdbk.c)

static uint32_t *out;
static size_t nout, capout;
static void ow(uint32_t w)
{
	if (nout == capout) { capout = capout ? 2 * capout : 1 << 16; out = realloc(out, capout * 4); }
	out[nout++] = w;
}

static int mcacc_blk4(uint32_t *o, int16_t *c, int b)
{
	int n = 0, any = 0;
	for (int i = 0; i < 16; i++) any |= c[i];
	if (!any) return 0;
	for (int p = 0; p < 16; p++)
	{
		int v = c[(p & 3) << 2 | p >> 2];
		if (v) o[n++] = (v & 0xFFFF) | p << 16 | b << 22;
	}
	memset(c, 0, 32);
	return n;
}

static void luma_dc_dequant_idct(int16_t *output, int16_t *input, int qmul)
{
	const int stride = 16;
	int temp[16];
	static const uint8_t x_offset[4] = { 0, 2 * 16, 8 * 16, 10 * 16 };
	for (int i = 0; i < 4; i++)
	{
		const int z0 = input[4 * i + 0] + input[4 * i + 1];
		const int z1 = input[4 * i + 0] - input[4 * i + 1];
		const int z2 = input[4 * i + 2] - input[4 * i + 3];
		const int z3 = input[4 * i + 2] + input[4 * i + 3];
		temp[4 * i + 0] = z0 + z3;
		temp[4 * i + 1] = z0 - z3;
		temp[4 * i + 2] = z1 - z2;
		temp[4 * i + 3] = z1 + z2;
	}
	for (int i = 0; i < 4; i++)
	{
		const int offset = x_offset[i];
		const unsigned z0 = temp[4 * 0 + i] + temp[4 * 2 + i];
		const unsigned z1 = temp[4 * 0 + i] - temp[4 * 2 + i];
		const unsigned z2 = temp[4 * 1 + i] - temp[4 * 3 + i];
		const unsigned z3 = temp[4 * 1 + i] + temp[4 * 3 + i];
		output[stride * 0 + offset] = (int)((z0 + z3) * qmul + 128) >> 8;
		output[stride * 1 + offset] = (int)((z1 + z2) * qmul + 128) >> 8;
		output[stride * 4 + offset] = (int)((z1 - z2) * qmul + 128) >> 8;
		output[stride * 5 + offset] = (int)((z0 - z3) * qmul + 128) >> 8;
	}
}

static void chroma_dc_dequant_idct(int16_t *block, int qmul)
{
	const int stride = 32, xStride = 16;
	unsigned a, b, c, d, e;
	a = block[stride * 0 + xStride * 0];
	b = block[stride * 0 + xStride * 1];
	c = block[stride * 1 + xStride * 0];
	d = block[stride * 1 + xStride * 1];
	e = a - b;
	a = a + b;
	b = c - d;
	c = c + d;
	block[stride * 0 + xStride * 0] = (int)((a + c) * qmul) >> 7;
	block[stride * 0 + xStride * 1] = (int)((e + b) * qmul) >> 7;
	block[stride * 1 + xStride * 0] = (int)((a - c) * qmul) >> 7;
	block[stride * 1 + xStride * 1] = (int)((e - b) * qmul) >> 7;
}

static int mcacc_dbk(uint32_t w[3])
{
	const int mb_xy = sl.mb_xy;
	const uint8_t *qs = cur->qscale_table;
	int avl, avt;
	if (!sl.deblocking_filter) return 0;
	const int offa = D.alpha, offb = D.beta;
	if (offa < -16 || offa > 15 || offb < -16 || offb > 15) return 0;
	if (sl.deblocking_filter == 2)
	{
		avl = cur->slice_table[mb_xy - 1] == sl.slice_num;
		avt = cur->slice_table[mb_xy - mb_stride] == sl.slice_num;
	}
	else
	{
		avl = cur->slice_table[mb_xy - 1] != 0xFFFF;
		avt = cur->slice_table[mb_xy - mb_stride] != 0xFFFF;
	}
	const int q = qs[mb_xy];
	const int nl = avl ? qs[mb_xy - 1] : q, nt = avt ? qs[mb_xy - mb_stride] : q;
	const int qy0 = (q + nl + 1) >> 1, qy1 = (q + nt + 1) >> 1;
	const int qb0 = (cqp_table[0][q] + cqp_table[0][nl] + 1) >> 1, qb1 = (cqp_table[0][q] + cqp_table[0][nt] + 1) >> 1;
	const int qr0 = (cqp_table[1][q] + cqp_table[1][nl] + 1) >> 1, qr1 = (cqp_table[1][q] + cqp_table[1][nt] + 1) >> 1;
	w[0] = 0xA5000000u | avl | avt << 1 | (offa & 31) << 2 | (offb & 31) << 7;
	w[1] = qy0 | qy1 << 6 | q << 12 | qb0 << 18 | (uint32_t)qb1 << 24;
	w[2] = cqp_table[0][q] | qr0 << 6 | qr1 << 12 | cqp_table[1][q] << 18;
	return 1;
}

static void mcacc_mb(void)
{
	const int mb_type = cur->mb_type[sl.mb_xy];
	int16_t *mb = sl.mb;
	int type, i, n = 0;
	uint32_t mask = 0, coef[400], head[37], dw[3];
	int dbk = sl.deblocking_filter;
	if (IS_INTRA_PCM(mb_type)) type = 4;
	else if (IS_INTRA16x16(mb_type)) type = 3;
	else if (IS_INTRA4x4(mb_type)) type = IS_8x8DCT(mb_type) ? 2 : 1;
	else type = 0;
	if (dbk && !mcacc_dbk(dw)) dw[0] = dw[1] = dw[2] = 0xA0000000u;
	if (dbk) { ow(dw[0]); ow(dw[1]); ow(dw[2]); }
	ow(0xA3000000u | type << 16 | sl.mb_y << 8 | sl.mb_x);
	if (type == 4)
	{
		for (i = 0; i < 384; i += 4)
			ow(sl.intra_pcm_ptr[i] | sl.intra_pcm_ptr[i + 1] << 8 | sl.intra_pcm_ptr[i + 2] << 16 | (uint32_t)sl.intra_pcm_ptr[i + 3] << 24);
		return;
	}
	if (type == 3 && sl.non_zero_count_cache[scan8[LUMA_DC_BLOCK_INDEX]])
		luma_dc_dequant_idct(mb, sl.mb_luma_dc, dequant4[0][sl.qscale][0]);
	if (sl.cbp & 0x30)
	{
		if (sl.non_zero_count_cache[scan8[CHROMA_DC_BLOCK_INDEX + 0]])
			chroma_dc_dequant_idct(mb + 256, dequant4[IS_INTRA(mb_type) ? 1 : 4][sl.chroma_qp[0]][0]);
		if (sl.non_zero_count_cache[scan8[CHROMA_DC_BLOCK_INDEX + 1]])
			chroma_dc_dequant_idct(mb + 512, dequant4[IS_INTRA(mb_type) ? 2 : 5][sl.chroma_qp[1]][0]);
	}
	if (IS_8x8DCT(mb_type))
	{
		for (int q = 0; q < 4; q++)
		{
			int16_t *c = mb + q * 64;
			int m = 0;
			if (!(sl.cbp & (1 << q))) continue;
			for (int p = 0; p < 64; p++)
			{
				int v = c[(p & 7) << 3 | p >> 3];
				if (v) { coef[n++] = (v & 0xFFFF) | p << 16 | (q * 4) << 22; m = 1; }
			}
			mask |= m << (q * 4);
			memset(c, 0, 128);
		}
	}
	else
	{
		int q8 = type == 3 ? 15 : sl.cbp & 15;
		for (int b = 0; b < 16; b++)
		{
			int k;
			if (!(q8 & (1 << (b >> 2)))) continue;
			k = mcacc_blk4(coef + n, mb + b * 16, b);
			if (k) { n += k; mask |= 1 << b; }
		}
	}
	if (sl.cbp & 0x30)
		for (int b = 16; b < 24; b++)
		{
			int k = mcacc_blk4(coef + n, mb + 256 + (b - 16) * 16 + (b >= 20 ? 192 : 0), b);
			if (k) { n += k; mask |= 1 << b; }
		}
	head[0] = mask | (IS_8x8DCT(mb_type) ? 1u : 0u) << 24;
	if (type)
	{
		uint32_t m0 = 0, m1 = 0;
		for (i = 0; i < 8; i++) m0 |= (uint32_t)(sl.intra4x4_pred_mode_cache[scan8[i]] & 15) << (4 * i);
		for (i = 0; i < 8; i++) m1 |= (uint32_t)(sl.intra4x4_pred_mode_cache[scan8[i + 8]] & 15) << (4 * i);
		if (type == 3) m0 = m1 = 0;
		head[1] = m0;
		head[2] = m1;
		head[3] = (sl.topright_samples_available & 0xFFFF) | (sl.topleft_samples_available & 0xFFFF) << 16;
		head[4] = sl.chroma_pred_mode | (type == 3 ? sl.intra16x16_pred_mode : 0) << 4;
		for (i = 0; i < 5; i++) ow(head[i]);
	}
	else
	{
		for (i = 0; i < 4; i++)
		{
			int r0 = USES_LIST(mb_type, 0) ? sl.ref_cache[0][scan8[i * 4]] : -1;
			int r1 = sl.list_count > 1 && USES_LIST(mb_type, 1) ? sl.ref_cache[1][scan8[i * 4]] : -1;
			uint32_t refs = (r0 & 0x3F) | (r0 < 0) << 6 | (uint32_t)((r1 & 0x3F) | (r1 < 0) << 6) << 7;
			if (r0 >= 0 && r1 >= 0 && D.use_weight == 2)
				refs |= (uint32_t)(D.implicit[r0][r1][sl.mb_y & 1] & 0x1FF) << 16;
			else
				refs |= 32u << 16;
			head[1 + i] = refs;
		}
		for (int l = 0; l < 2; l++)
		{
			int ok = l < sl.list_count && USES_LIST(mb_type, l);
			for (i = 0; i < 16; i++)
				head[5 + l * 16 + i] = ok ? (uint16_t)sl.mv_cache[l][scan8[i]][0] | (uint32_t)(uint16_t)sl.mv_cache[l][scan8[i]][1] << 16 : 0;
		}
		for (i = 0; i < 37; i++) ow(head[i]);
	}
	ow(n);
	for (i = 0; i < n; i++) ow(coef[i]);
}

// ---------------------------------------------------------------- picture and slice

static void build_pps_tables(void)
{
	for (int i = 0; i < 6; i++)
		for (int q = 0; q < 52; q++)
		{
			int shift = q / 6 + 2, idx = q % 6;
			for (int x = 0; x < 16; x++)
				dequant4[i][q][(x >> 2) | ((x << 2) & 0xF)] = ((uint32_t)dequant4_init[idx][(x & 1) + ((x >> 2) & 1)] * D.sm4[i][x]) << shift;
			shift = q / 6;
			if (D.t8x8)
				for (int x = 0; x < 64; x++)
					dequant8[i][q][(x >> 3) | ((x & 7) << 3)] =
						((uint32_t)dequant8_init[idx][dequant8_init_scan[((x >> 1) & 12) | (x & 3)]] * D.sm8[i][x]) << shift;
		}
	for (int t = 0; t < 2; t++)
		for (int i = 0; i < 52; i++)
		{
			int j = i + D.cqp_off[t];
			cqp_table[t][i] = chroma_qp_tab[j < 0 ? 0 : j > 51 ? 51 : j];
		}
}

static void tables_alloc(void)
{
	int n = (mb_h + 4) * mb_stride, off = 2 * mb_stride + 1;
	if (n == ntab) return;
	ntab = n;
	static void *bases[7];
	for (int i = 0; i < 7; i++) free(bases[i]);
	bases[0] = calloc(n, 48); non_zero_count = (uint8_t (*)[48])bases[0] + off;
	bases[1] = calloc(n, 2); cbp_table = (uint16_t *)bases[1] + off;
	bases[2] = calloc(n, 1); chroma_pred_mode_table = (uint8_t *)bases[2] + off;
	bases[3] = calloc(n, 8); intra4x4_pred_mode = (int8_t (*)[8])bases[3] + off;
	bases[4] = calloc(n, 16); mvd_table[0] = (uint8_t (*)[8][2])bases[4] + off;
	bases[5] = calloc(n, 16); mvd_table[1] = (uint8_t (*)[8][2])bases[5] + off;
	bases[6] = calloc(n, 4); direct_table = (uint8_t *)bases[6] + 4 * off;
}

static long nmb, nslices;

static void decode_slice(void)
{
	mb_w = D.mb_w;
	mb_h = D.mb_h;
	mb_stride = mb_w + 1;
	b_stride = mb_w * 4;
	tables_alloc();
	build_pps_tables();
	sl.mb_x = D.mb_x;
	sl.mb_y = D.mb_y;
	sl.slice_num = D.slice_num;
	sl.slice_type_nos = D.type;
	sl.qscale = D.qp;
	sl.chroma_qp[0] = get_chroma_qp(0, sl.qscale);
	sl.chroma_qp[1] = get_chroma_qp(1, sl.qscale);
	sl.last_qscale_diff = 0;
	sl.ref_count[0] = D.ref_count[0];
	sl.ref_count[1] = D.ref_count[1];
	sl.list_count = D.list_count;
	sl.direct_spatial_mv_pred = D.direct_spatial;
	sl.deblocking_filter = D.deblock;
	// as ff_h264_slice_context_init: cache cells no macroblock ever fills
	for (int l = 0; l < 2; l++)
		sl.ref_cache[l][scan8[5] + 1] = sl.ref_cache[l][scan8[7] + 1] = sl.ref_cache[l][scan8[13] + 1] = PART_NOT_AVAILABLE;
	bs = D.data;
	nbits = 8ull * D.nb;
	bitpos = 0;
	ctx_init();
	engine_init();
	nslices++;
	for (;;)
	{
		if (decode_mb_cabac() < 0) { fprintf(stderr, "pmodel: error in picture %d at MB %d %d\n", D.pic, sl.mb_x, sl.mb_y); exit(1); }
		mcacc_mb();
		nmb++;
		int eos = get_cabac_terminate();
		if (++sl.mb_x >= mb_w)
		{
			sl.mb_x = 0;
			++sl.mb_y;
		}
		if (eos || sl.mb_y >= mb_h) break;
	}
}

// ---------------------------------------------------------------- compare with the ARM's commands

static FILE *cmdf;
static uint32_t *ref;
static size_t nref, capref;
static long npics, nerr, nwords, nskip;

// the next picture of the reference: its macroblock words only (A1, A2 and A4 left out)
static int magic_read; // stream mode: 'PIC1' already read by read_desc
static int ref_next(void)
{
	uint32_t hd[2];
	if (magic_read) { hd[0] = 0x50494331u; magic_read = 0; if (fread(hd + 1, 4, 1, cmdf) != 1) return 0; }
	else if (fread(hd, 4, 2, cmdf) != 2) return 0;
	if (hd[0] != 0x50494331u) { fprintf(stderr, "pmodel: bad .cmd\n"); exit(1); }
	uint32_t *w = malloc(hd[1] * 4 + 4);
	if (fread(w, 4, hd[1], cmdf) != hd[1]) return 0;
	nref = 0;
	if (capref < hd[1]) { capref = hd[1]; ref = realloc(ref, capref * 4); }
	size_t i = 2; // A1
	while (i < hd[1])
	{
		uint32_t t = w[i] >> 24;
		if (t == 0xA2)
		{
			int n0 = w[i] & 63, n1 = (w[i] >> 6) & 63, two = (w[i] >> 12) & 1;
			i += 1 + 3 * (n0 + (two ? n1 : 0));
		}
		else if (t == 0xA4) i++;
		else if (t == 0xA5 || t == 0xA0) { for (int k = 0; k < 3; k++) ref[nref++] = w[i++]; }
		else if (t == 0xA3)
		{
			int type = (w[i] >> 16) & 7;
			size_t n = type == 4 ? 97 : 1 + 1 + (type ? 4 : 36);
			if (type != 4) n += 1 + w[i + n];
			for (size_t k = 0; k < n; k++) ref[nref++] = w[i++];
		}
		else { fprintf(stderr, "pmodel: unknown word %08x in .cmd\n", w[i]); exit(1); }
	}
	free(w);
	return 1;
}

static void end_picture(void)
{
	if (!cur) return;
	if (!ref_next()) { fprintf(stderr, "pmodel: .cmd has fewer pictures\n"); exit(1); }
	npics++;
	nwords += nref;
	size_t n = nout < nref ? nout : nref;
	for (size_t i = 0; i < n; i++)
		if (out[i] != ref[i])
		{
			nerr++;
			if (nerr <= 10)
			{
				// the macroblock: last A3 at or before i
				size_t j = i;
				while (j > 0 && (out[j] >> 24) != 0xA3) j--;
				fprintf(stderr, "pmodel: picture %d (slot %d), word %zu (MB %d,%d, +%zu): model %08x, ARM %08x\n",
				        D.pic, D.slot, i, out[j] & 0xFF, (out[j] >> 8) & 0xFF, i - j, out[i], ref[i]);
				if (getenv("PMDBG"))
					for (size_t k = j; k < j + 42 && k < n; k++)
						fprintf(stderr, "  +%2zu  %08x  %08x%s\n", k - j, out[k], ref[k], out[k] != ref[k] ? "  <" : "");
			}
			break;
		}
	if (nout != nref && n == nout && n == nref) {}
	else if (nout != nref)
	{
		int first = 1;
		for (size_t i = 0; i < n; i++) if (out[i] != ref[i]) { first = 0; break; }
		if (first) { nerr++; if (nerr <= 10) fprintf(stderr, "pmodel: picture %d: model %zu words, ARM %zu\n", D.pic, nout, nref); }
	}
	nout = 0;
	cur = NULL;
}

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: pmodel <prefix> [name]\n"); return 2; }
	const char *name = argc > 2 ? argv[2] : argv[1];
	char fn[1024];
	FILE *f;
	int stream = !strcmp(argv[1], "-");
	if (stream)
		f = cmdf = stdin; // refcmd file - : slices and commands in decoding order
	else
	{
		snprintf(fn, sizeof(fn), "%s.sl", argv[1]);
		f = fopen(fn, "rb");
		snprintf(fn, sizeof(fn), "%s.cmd", argv[1]);
		cmdf = fopen(fn, "rb");
	}
	if (!f || !cmdf) { fprintf(stderr, "pmodel: cannot open %s.sl/.cmd\n", argv[1]); return 1; }
	static char b1[1 << 20], b2[1 << 20];
	setvbuf(f, b1, _IOFBF, sizeof(b1));
	if (!stream) setvbuf(cmdf, b2, _IOFBF, sizeof(b2));
	for (int i = 0; i < 16; i++) zz4[i] = (zigzag_scan4[i] >> 2) | ((zigzag_scan4[i] << 2) & 0xF);
	for (int i = 0; i < 64; i++) zz8[i] = (zigzag_direct[i] >> 3) | ((zigzag_direct[i] & 7) << 3);
	int last_pic = -1;
	int r;
	while ((r = read_desc(f, &D)))
	{
		if (r == 2)
		{
			// the picture's commands: compare now (a picture with no CABAC slice is skipped)
			if (cur) { magic_read = 1; end_picture(); }
			else
			{
				uint32_t n;
				if (fread(&n, 4, 1, f) != 1) break;
				fseek(f, 0, SEEK_CUR);
				for (uint32_t i = 0; i < n; i++) { uint32_t w; if (fread(&w, 4, 1, f) != 1) break; }
				nskip++;
			}
			continue;
		}
		if (D.pic != last_pic)
		{
			if (stream && cur) { fprintf(stderr, "pmodel: picture %d has no commands\n", last_pic); exit(1); }
			end_picture();
			last_pic = D.pic;
			cur = &pics[D.slot & 255];
			pic_alloc(cur, D.mb_w, D.mb_h);
			int n = (D.mb_h + 4) * (D.mb_w + 1);
			for (int i = 0; i < n; i++) cur->slice_table_base[i] = 0xFFFF;
		}
		decode_slice();
	}
	if (!stream) end_picture();
	else if (cur) { fprintf(stderr, "pmodel: last picture has no commands\n"); nerr++; }
	if (nskip) fprintf(stderr, "pmodel: %ld pictures without CABAC slices\n", nskip);
	printf("%s: modello acc_parse %ld immagini, %ld slice, %ld macroblocchi, %ld parole, errori %ld\n",
	       name, npics, nslices, nmb, nwords, nerr);
	return nerr != 0;
}
