// pmodel: macroblock reading and prediction (included by pmodel.c). Follows
// FFmpeg's h264_cabac.c, h264_mvpred.h and h264_direct.c for progressive
// frames, 4:2:0, 8 bit (no MBAFF, no fields).

static const uint8_t left_block_options0[32] = {
	0, 1, 2, 3, 7, 10, 8, 11, 3 + 0 * 4, 3 + 1 * 4, 3 + 2 * 4, 3 + 3 * 4, 1 + 4 * 4, 1 + 8 * 4, 1 + 5 * 4, 1 + 9 * 4
};

// ---------------------------------------------------------------- neighbours and caches

static void fill_decode_neighbors(int mb_type)
{
	(void)mb_type;
	const int mb_xy = sl.mb_xy;
	int topleft_xy, top_xy, topright_xy, left_xy;
	sl.topleft_partition = -1;
	top_xy = mb_xy - mb_stride;
	topleft_xy = top_xy - 1;
	topright_xy = top_xy + 1;
	left_xy = mb_xy - 1;
	sl.left_block = left_block_options0;
	sl.topleft_mb_xy = topleft_xy;
	sl.top_mb_xy = top_xy;
	sl.topright_mb_xy = topright_xy;
	sl.left_mb_xy[0] = sl.left_mb_xy[1] = left_xy;
	sl.topleft_type = cur->mb_type[topleft_xy];
	sl.top_type = cur->mb_type[top_xy];
	sl.topright_type = cur->mb_type[topright_xy];
	sl.left_type[0] = sl.left_type[1] = cur->mb_type[left_xy];
	if (cur->slice_table[topleft_xy] != sl.slice_num)
	{
		sl.topleft_type = 0;
		if (cur->slice_table[top_xy] != sl.slice_num) sl.top_type = 0;
		if (cur->slice_table[left_xy] != sl.slice_num) sl.left_type[0] = sl.left_type[1] = 0;
	}
	if (cur->slice_table[topright_xy] != sl.slice_num) sl.topright_type = 0;
}

static void fill_decode_caches(int mb_type)
{
	int topleft_xy = sl.topleft_mb_xy, top_xy = sl.top_mb_xy, topright_xy = sl.topright_mb_xy;
	int left_xy[2] = { sl.left_mb_xy[0], sl.left_mb_xy[1] };
	int topleft_type = sl.topleft_type, top_type = sl.top_type, topright_type = sl.topright_type;
	int left_type[2] = { sl.left_type[0], sl.left_type[1] };
	const uint8_t *left_block = sl.left_block;
	int i;
	uint8_t *nnz, *nnz_cache;

	if (!IS_SKIP(mb_type))
	{
		if (IS_INTRA(mb_type))
		{
			int type_mask = D.constrained_intra ? IS_INTRA(-1) : -1;
			sl.topleft_samples_available = sl.top_samples_available = sl.left_samples_available = 0xFFFF;
			sl.topright_samples_available = 0xEEEA;
			if (!(top_type & type_mask))
			{
				sl.topleft_samples_available = 0xB3FF;
				sl.top_samples_available = 0x33FF;
				sl.topright_samples_available = 0x26EA;
			}
			if (!(left_type[0] & type_mask))
			{
				sl.topleft_samples_available &= 0xDF5F;
				sl.left_samples_available &= 0x5F5F;
			}
			if (!(topleft_type & type_mask)) sl.topleft_samples_available &= 0x7FFF;
			if (!(topright_type & type_mask)) sl.topright_samples_available &= 0xFBFF;
			if (IS_INTRA4x4(mb_type))
			{
				if (IS_INTRA4x4(top_type))
					memcpy(sl.intra4x4_pred_mode_cache + 4 + 8 * 0, intra4x4_pred_mode[top_xy], 4);
				else
					sl.intra4x4_pred_mode_cache[4 + 8 * 0] = sl.intra4x4_pred_mode_cache[5 + 8 * 0] =
					sl.intra4x4_pred_mode_cache[6 + 8 * 0] = sl.intra4x4_pred_mode_cache[7 + 8 * 0] = 2 - 3 * !(top_type & type_mask);
				for (i = 0; i < 2; i++)
				{
					if (IS_INTRA4x4(left_type[0]))
					{
						int8_t *mode = intra4x4_pred_mode[left_xy[0]];
						sl.intra4x4_pred_mode_cache[3 + 8 * 1 + 2 * 8 * i] = mode[6 - left_block[0 + 2 * i]];
						sl.intra4x4_pred_mode_cache[3 + 8 * 2 + 2 * 8 * i] = mode[6 - left_block[1 + 2 * i]];
					}
					else
						sl.intra4x4_pred_mode_cache[3 + 8 * 1 + 2 * 8 * i] =
						sl.intra4x4_pred_mode_cache[3 + 8 * 2 + 2 * 8 * i] = 2 - 3 * !(left_type[0] & type_mask);
				}
			}
		}

		nnz_cache = sl.non_zero_count_cache;
		if (top_type)
		{
			nnz = non_zero_count[top_xy];
			memcpy(&nnz_cache[4 + 8 * 0], &nnz[4 * 3], 4);
			memcpy(&nnz_cache[4 + 8 * 5], &nnz[4 * 5], 4);
			memcpy(&nnz_cache[4 + 8 * 10], &nnz[4 * 9], 4);
		}
		else
		{
			uint8_t top_empty = !IS_INTRA(mb_type) ? 0 : 0x40;
			memset(&nnz_cache[4 + 8 * 0], top_empty, 4);
			memset(&nnz_cache[4 + 8 * 5], top_empty, 4);
			memset(&nnz_cache[4 + 8 * 10], top_empty, 4);
		}
		for (i = 0; i < 2; i++)
		{
			if (left_type[0])
			{
				nnz = non_zero_count[left_xy[0]];
				nnz_cache[3 + 8 * 1 + 2 * 8 * i] = nnz[left_block[8 + 0 + 2 * i]];
				nnz_cache[3 + 8 * 2 + 2 * 8 * i] = nnz[left_block[8 + 1 + 2 * i]];
				nnz_cache[3 + 8 * 6 + 8 * i] = nnz[left_block[8 + 4 + 2 * i]];
				nnz_cache[3 + 8 * 11 + 8 * i] = nnz[left_block[8 + 5 + 2 * i]];
			}
			else
			{
				nnz_cache[3 + 8 * 1 + 2 * 8 * i] = nnz_cache[3 + 8 * 2 + 2 * 8 * i] =
				nnz_cache[3 + 8 * 6 + 2 * 8 * i] = nnz_cache[3 + 8 * 7 + 2 * 8 * i] =
				nnz_cache[3 + 8 * 11 + 2 * 8 * i] = nnz_cache[3 + 8 * 12 + 2 * 8 * i] = !IS_INTRA(mb_type) ? 0 : 64;
			}
		}
		if (top_type) sl.top_cbp = cbp_table[top_xy];
		else sl.top_cbp = IS_INTRA(mb_type) ? 0x7CF : 0x00F;
		if (left_type[0])
			sl.left_cbp = (cbp_table[left_xy[0]] & 0x7F0) |
			              ((cbp_table[left_xy[0]] >> (left_block[0] & (~1))) & 2) |
			              (((cbp_table[left_xy[1]] >> (left_block[2] & (~1))) & 2) << 2);
		else
			sl.left_cbp = IS_INTRA(mb_type) ? 0x7CF : 0x00F;
	}

	if (IS_INTER(mb_type) || (IS_DIRECT(mb_type) && sl.direct_spatial_mv_pred))
	{
		for (int list = 0; list < sl.list_count; list++)
		{
			int8_t *ref_cache = &sl.ref_cache[list][scan8[0]];
			int8_t *ref = cur->ref_index[list];
			int16_t (*mv_cache)[2] = &sl.mv_cache[list][scan8[0]];
			int16_t (*mv)[2] = cur->motion_val[list];
			if (!USES_LIST(mb_type, list)) continue;
			if (USES_LIST(top_type, list))
			{
				const int b_xy = 4 * (top_xy % mb_stride) + 4 * (top_xy / mb_stride) * b_stride + 3 * b_stride;
				memcpy(mv_cache[0 - 1 * 8], mv[b_xy + 0], 16);
				ref_cache[0 - 1 * 8] = ref_cache[1 - 1 * 8] = ref[4 * top_xy + 2];
				ref_cache[2 - 1 * 8] = ref_cache[3 - 1 * 8] = ref[4 * top_xy + 3];
			}
			else
			{
				memset(mv_cache[0 - 1 * 8], 0, 16);
				memset(&ref_cache[0 - 1 * 8], (top_type ? LIST_NOT_USED : PART_NOT_AVAILABLE) & 0xFF, 4);
			}
			if (mb_type & (MB_TYPE_16x8 | MB_TYPE_8x8))
			{
				for (i = 0; i < 2; i++)
				{
					int cache_idx = -1 + i * 2 * 8;
					if (USES_LIST(left_type[0], list))
					{
						const int b_xy = 4 * (left_xy[0] % mb_stride) + 4 * (left_xy[0] / mb_stride) * b_stride + 3;
						const int b8_xy = 4 * left_xy[0] + 1;
						memcpy(mv_cache[cache_idx], mv[b_xy + b_stride * left_block[0 + i * 2]], 4);
						memcpy(mv_cache[cache_idx + 8], mv[b_xy + b_stride * left_block[1 + i * 2]], 4);
						ref_cache[cache_idx] = ref[b8_xy + (left_block[0 + i * 2] & ~1)];
						ref_cache[cache_idx + 8] = ref[b8_xy + (left_block[1 + i * 2] & ~1)];
					}
					else
					{
						memset(mv_cache[cache_idx], 0, 4);
						memset(mv_cache[cache_idx + 8], 0, 4);
						ref_cache[cache_idx] = ref_cache[cache_idx + 8] = left_type[0] ? LIST_NOT_USED : PART_NOT_AVAILABLE;
					}
				}
			}
			else
			{
				if (USES_LIST(left_type[0], list))
				{
					const int b_xy = 4 * (left_xy[0] % mb_stride) + 4 * (left_xy[0] / mb_stride) * b_stride + 3;
					const int b8_xy = 4 * left_xy[0] + 1;
					memcpy(mv_cache[-1], mv[b_xy + b_stride * left_block[0]], 4);
					ref_cache[-1] = ref[b8_xy + (left_block[0] & ~1)];
				}
				else
				{
					memset(mv_cache[-1], 0, 4);
					ref_cache[-1] = left_type[0] ? LIST_NOT_USED : PART_NOT_AVAILABLE;
				}
			}
			if (USES_LIST(topright_type, list))
			{
				const int b_xy = 4 * (topright_xy % mb_stride) + 4 * (topright_xy / mb_stride) * b_stride + 3 * b_stride;
				memcpy(mv_cache[4 - 1 * 8], mv[b_xy], 4);
				ref_cache[4 - 1 * 8] = ref[4 * topright_xy + 2];
			}
			else
			{
				memset(mv_cache[4 - 1 * 8], 0, 4);
				ref_cache[4 - 1 * 8] = topright_type ? LIST_NOT_USED : PART_NOT_AVAILABLE;
			}
			if (ref_cache[2 - 1 * 8] < 0 || ref_cache[4 - 1 * 8] < 0)
			{
				if (USES_LIST(topleft_type, list))
				{
					const int b_xy = 4 * (topleft_xy % mb_stride) + 4 * (topleft_xy / mb_stride) * b_stride + 3 + b_stride +
					                 (sl.topleft_partition & 2 * b_stride);
					const int b8_xy = 4 * topleft_xy + 1 + (sl.topleft_partition & 2);
					memcpy(mv_cache[-1 - 1 * 8], mv[b_xy], 4);
					ref_cache[-1 - 1 * 8] = ref[b8_xy];
				}
				else
				{
					memset(mv_cache[-1 - 1 * 8], 0, 4);
					ref_cache[-1 - 1 * 8] = topleft_type ? LIST_NOT_USED : PART_NOT_AVAILABLE;
				}
			}
			if (mb_type & (MB_TYPE_SKIP | MB_TYPE_DIRECT2)) continue;
			{
				uint8_t (*mvd_cache)[2] = &sl.mvd_cache[list][scan8[0]];
				ref_cache[2 + 8 * 0] = ref_cache[2 + 8 * 2] = PART_NOT_AVAILABLE;
				memset(mv_cache[2 + 8 * 0], 0, 4);
				memset(mv_cache[2 + 8 * 2], 0, 4);
				if (USES_LIST(top_type, list))
					memcpy(mvd_cache[0 - 1 * 8], mvd_table[list][top_xy][0], 8);
				else
					memset(mvd_cache[0 - 1 * 8], 0, 8);
				if (USES_LIST(left_type[0], list))
				{
					memcpy(mvd_cache[-1 + 0 * 8], mvd_table[list][left_xy[0]][6 - left_block[0]], 2);
					memcpy(mvd_cache[-1 + 1 * 8], mvd_table[list][left_xy[0]][6 - left_block[1]], 2);
				}
				else
				{
					memset(mvd_cache[-1 + 0 * 8], 0, 2);
					memset(mvd_cache[-1 + 1 * 8], 0, 2);
				}
				if (USES_LIST(left_type[1], list))
				{
					memcpy(mvd_cache[-1 + 2 * 8], mvd_table[list][left_xy[1]][6 - left_block[2]], 2);
					memcpy(mvd_cache[-1 + 3 * 8], mvd_table[list][left_xy[1]][6 - left_block[3]], 2);
				}
				else
				{
					memset(mvd_cache[-1 + 2 * 8], 0, 2);
					memset(mvd_cache[-1 + 3 * 8], 0, 2);
				}
				memset(mvd_cache[2 + 8 * 0], 0, 2);
				memset(mvd_cache[2 + 8 * 2], 0, 2);
				if (sl.slice_type_nos == 1)
				{
					uint8_t *direct_cache = &sl.direct_cache[scan8[0]];
					fill_rectangle(direct_cache, 4, 4, 8, MB_TYPE_16x16 >> 1, 1);
					if (IS_DIRECT(top_type))
						memset(&direct_cache[-1 * 8], MB_TYPE_DIRECT2 >> 1, 4);
					else if (IS_8X8(top_type))
					{
						int b8_xy = 4 * top_xy;
						direct_cache[0 - 1 * 8] = direct_table[b8_xy + 2];
						direct_cache[2 - 1 * 8] = direct_table[b8_xy + 3];
					}
					else
						memset(&direct_cache[-1 * 8], MB_TYPE_16x16 >> 1, 4);
					if (IS_DIRECT(left_type[0])) direct_cache[-1 + 0 * 8] = MB_TYPE_DIRECT2 >> 1;
					else if (IS_8X8(left_type[0])) direct_cache[-1 + 0 * 8] = direct_table[4 * left_xy[0] + 1 + (left_block[0] & ~1)];
					else direct_cache[-1 + 0 * 8] = MB_TYPE_16x16 >> 1;
					if (IS_DIRECT(left_type[1])) direct_cache[-1 + 2 * 8] = MB_TYPE_DIRECT2 >> 1;
					else if (IS_8X8(left_type[1])) direct_cache[-1 + 2 * 8] = direct_table[4 * left_xy[1] + 1 + (left_block[2] & ~1)];
					else direct_cache[-1 + 2 * 8] = MB_TYPE_16x16 >> 1;
				}
			}
		}
	}
	sl.neighbor_transform_size = !!IS_8x8DCT(top_type) + !!IS_8x8DCT(left_type[0]);
}

// ---------------------------------------------------------------- write back

static int mb2b(int mb_xy) { return 4 * (mb_xy % mb_stride) + 4 * (mb_xy / mb_stride) * b_stride; }

static void write_back_intra_pred_mode(void)
{
	int8_t *i4x4 = intra4x4_pred_mode[sl.mb_xy];
	int8_t *c = sl.intra4x4_pred_mode_cache;
	memcpy(i4x4, c + 4 + 8 * 4, 4);
	i4x4[4] = c[7 + 8 * 3];
	i4x4[5] = c[7 + 8 * 2];
	i4x4[6] = c[7 + 8 * 1];
}

static void write_back_non_zero_count(void)
{
	uint8_t *nnz = non_zero_count[sl.mb_xy], *c = sl.non_zero_count_cache;
	memcpy(&nnz[0], &c[4 + 8 * 1], 4);
	memcpy(&nnz[4], &c[4 + 8 * 2], 4);
	memcpy(&nnz[8], &c[4 + 8 * 3], 4);
	memcpy(&nnz[12], &c[4 + 8 * 4], 4);
	memcpy(&nnz[16], &c[4 + 8 * 6], 4);
	memcpy(&nnz[20], &c[4 + 8 * 7], 4);
	memcpy(&nnz[32], &c[4 + 8 * 11], 4);
	memcpy(&nnz[36], &c[4 + 8 * 12], 4);
}

static void write_back_motion_list(int b_xy, int b8_xy, int mb_type, int list)
{
	int16_t (*mv_dst)[2] = &cur->motion_val[list][b_xy];
	int16_t (*mv_src)[2] = &sl.mv_cache[list][scan8[0]];
	for (int r = 0; r < 4; r++) memcpy(mv_dst + r * b_stride, mv_src + 8 * r, 16);
	uint8_t (*mvd_dst)[2] = mvd_table[list][sl.mb_xy];
	uint8_t (*mvd_src)[2] = &sl.mvd_cache[list][scan8[0]];
	if (IS_SKIP(mb_type))
		memset(mvd_dst, 0, 16);
	else
	{
		memcpy(mvd_dst, mvd_src + 8 * 3, 8);
		memcpy(mvd_dst + 3 + 3, mvd_src + 3 + 8 * 0, 2);
		memcpy(mvd_dst + 3 + 2, mvd_src + 3 + 8 * 1, 2);
		memcpy(mvd_dst + 3 + 1, mvd_src + 3 + 8 * 2, 2);
	}
	int8_t *ref_index = &cur->ref_index[list][b8_xy];
	int8_t *ref_cache = sl.ref_cache[list];
	ref_index[0 + 0 * 2] = ref_cache[scan8[0]];
	ref_index[1 + 0 * 2] = ref_cache[scan8[4]];
	ref_index[0 + 1 * 2] = ref_cache[scan8[8]];
	ref_index[1 + 1 * 2] = ref_cache[scan8[12]];
}

static void write_back_motion(int mb_type)
{
	const int b_xy = 4 * sl.mb_x + 4 * sl.mb_y * b_stride;
	const int b8_xy = 4 * sl.mb_xy;
	if (USES_LIST(mb_type, 0))
		write_back_motion_list(b_xy, b8_xy, mb_type, 0);
	else
		fill_rectangle(&cur->ref_index[0][b8_xy], 2, 2, 2, (uint8_t)LIST_NOT_USED, 1);
	if (USES_LIST(mb_type, 1))
		write_back_motion_list(b_xy, b8_xy, mb_type, 1);
	if (sl.slice_type_nos == 1 && IS_8X8(mb_type))
	{
		uint8_t *dt = &direct_table[4 * sl.mb_xy];
		dt[1] = sl.sub_mb_type[1] >> 1;
		dt[2] = sl.sub_mb_type[2] >> 1;
		dt[3] = sl.sub_mb_type[3] >> 1;
	}
}

// ---------------------------------------------------------------- motion vector prediction

static int fetch_diagonal_mv(const int16_t **C, int i, int list, int part_width)
{
	const int topright_ref = sl.ref_cache[list][i - 8 + part_width];
	if (topright_ref != PART_NOT_AVAILABLE)
	{
		*C = sl.mv_cache[list][i - 8 + part_width];
		return topright_ref;
	}
	*C = sl.mv_cache[list][i - 8 - 1];
	return sl.ref_cache[list][i - 8 - 1];
}

static void pred_motion(int n, int part_width, int list, int ref, int *mx, int *my)
{
	const int index8 = scan8[n];
	const int top_ref = sl.ref_cache[list][index8 - 8];
	const int left_ref = sl.ref_cache[list][index8 - 1];
	const int16_t *A = sl.mv_cache[list][index8 - 1];
	const int16_t *B = sl.mv_cache[list][index8 - 8];
	const int16_t *C;
	int diagonal_ref = fetch_diagonal_mv(&C, index8, list, part_width);
	int match_count = (diagonal_ref == ref) + (top_ref == ref) + (left_ref == ref);
	if (match_count > 1)
	{
		*mx = mid_pred(A[0], B[0], C[0]);
		*my = mid_pred(A[1], B[1], C[1]);
	}
	else if (match_count == 1)
	{
		if (left_ref == ref) { *mx = A[0]; *my = A[1]; }
		else if (top_ref == ref) { *mx = B[0]; *my = B[1]; }
		else { *mx = C[0]; *my = C[1]; }
	}
	else
	{
		if (top_ref == PART_NOT_AVAILABLE && diagonal_ref == PART_NOT_AVAILABLE && left_ref != PART_NOT_AVAILABLE)
		{
			*mx = A[0];
			*my = A[1];
		}
		else
		{
			*mx = mid_pred(A[0], B[0], C[0]);
			*my = mid_pred(A[1], B[1], C[1]);
		}
	}
}

static void pred_16x8_motion(int n, int list, int ref, int *mx, int *my)
{
	if (n == 0)
	{
		const int top_ref = sl.ref_cache[list][scan8[0] - 8];
		const int16_t *B = sl.mv_cache[list][scan8[0] - 8];
		if (top_ref == ref) { *mx = B[0]; *my = B[1]; return; }
	}
	else
	{
		const int left_ref = sl.ref_cache[list][scan8[8] - 1];
		const int16_t *A = sl.mv_cache[list][scan8[8] - 1];
		if (left_ref == ref) { *mx = A[0]; *my = A[1]; return; }
	}
	pred_motion(n, 4, list, ref, mx, my);
}

static void pred_8x16_motion(int n, int list, int ref, int *mx, int *my)
{
	if (n == 0)
	{
		const int left_ref = sl.ref_cache[list][scan8[0] - 1];
		const int16_t *A = sl.mv_cache[list][scan8[0] - 1];
		if (left_ref == ref) { *mx = A[0]; *my = A[1]; return; }
	}
	else
	{
		const int16_t *C;
		int diagonal_ref = fetch_diagonal_mv(&C, scan8[4], list, 2);
		if (diagonal_ref == ref) { *mx = C[0]; *my = C[1]; return; }
	}
	pred_motion(n, 2, list, ref, mx, my);
}

static void pred_pskip_motion(void)
{
	static const int16_t zeromv[2] = { 0, 0 };
	int8_t *ref = cur->ref_index[0];
	int16_t (*mv)[2] = cur->motion_val[0];
	int top_ref, left_ref, diagonal_ref, match_count, mx, my;
	const int16_t *A, *B, *C;
	fill_rectangle(&sl.ref_cache[0][scan8[0]], 4, 4, 8, 0, 1);
	if (USES_LIST(sl.left_type[0], 0))
	{
		left_ref = ref[4 * sl.left_mb_xy[0] + 1 + (sl.left_block[0] & ~1)];
		A = mv[mb2b(sl.left_mb_xy[0]) + 3 + b_stride * sl.left_block[0]];
		if (!(left_ref | rn32(A))) goto zeromv;
	}
	else if (sl.left_type[0])
	{
		left_ref = LIST_NOT_USED;
		A = zeromv;
	}
	else goto zeromv;
	if (USES_LIST(sl.top_type, 0))
	{
		top_ref = ref[4 * sl.top_mb_xy + 2];
		B = mv[mb2b(sl.top_mb_xy) + 3 * b_stride];
		if (!(top_ref | rn32(B))) goto zeromv;
	}
	else if (sl.top_type)
	{
		top_ref = LIST_NOT_USED;
		B = zeromv;
	}
	else goto zeromv;
	if (USES_LIST(sl.topright_type, 0))
	{
		diagonal_ref = ref[4 * sl.topright_mb_xy + 2];
		C = mv[mb2b(sl.topright_mb_xy) + 3 * b_stride];
	}
	else if (sl.topright_type)
	{
		diagonal_ref = LIST_NOT_USED;
		C = zeromv;
	}
	else
	{
		if (USES_LIST(sl.topleft_type, 0))
		{
			diagonal_ref = ref[4 * sl.topleft_mb_xy + 1 + (sl.topleft_partition & 2)];
			C = mv[mb2b(sl.topleft_mb_xy) + 3 + b_stride + (sl.topleft_partition & 2 * b_stride)];
		}
		else if (sl.topleft_type)
		{
			diagonal_ref = LIST_NOT_USED;
			C = zeromv;
		}
		else
		{
			diagonal_ref = PART_NOT_AVAILABLE;
			C = zeromv;
		}
	}
	match_count = !diagonal_ref + !top_ref + !left_ref;
	if (match_count > 1)
	{
		mx = mid_pred(A[0], B[0], C[0]);
		my = mid_pred(A[1], B[1], C[1]);
	}
	else if (match_count == 1)
	{
		if (!left_ref) { mx = A[0]; my = A[1]; }
		else if (!top_ref) { mx = B[0]; my = B[1]; }
		else { mx = C[0]; my = C[1]; }
	}
	else
	{
		mx = mid_pred(A[0], B[0], C[0]);
		my = mid_pred(A[1], B[1], C[1]);
	}
	fill_rectangle(sl.mv_cache[0][scan8[0]], 4, 4, 8, pack16to32(mx, my), 4);
	return;
zeromv:
	fill_rectangle(sl.mv_cache[0][scan8[0]], 4, 4, 8, 0, 4);
}

// ---------------------------------------------------------------- direct prediction (B)

#define MB_TYPE_16x16_OR_INTRA (MB_TYPE_16x16 | MB_TYPE_INTRA4x4 | MB_TYPE_INTRA16x16 | MB_TYPE_INTRA_PCM)

static Pic *col_pic(void)
{
	Pic *c = &pics[D.slots[1][0] & 255];
	if (!c->valid || c->mb_w != mb_w || c->mb_h != mb_h) { fprintf(stderr, "pmodel: co-located picture (slot %d) missing\n", D.slots[1][0]); exit(1); }
	return c;
}

static void pred_spatial_direct_motion(int *mb_type)
{
	int b4_stride = b_stride;
	int mb_xy = sl.mb_xy;
	int mb_type_col[2];
	const int16_t (*l1mv0)[2], (*l1mv1)[2];
	const int8_t *l1ref0, *l1ref1;
	const int is_b8x8 = IS_8X8(*mb_type);
	unsigned int sub_mb_type = MB_TYPE_L0L1;
	int i8, i4;
	int ref[2];
	uint32_t mv[2];
	Pic *col = col_pic();

	for (int list = 0; list < 2; list++)
	{
		int left_ref = sl.ref_cache[list][scan8[0] - 1];
		int top_ref = sl.ref_cache[list][scan8[0] - 8];
		int refc = sl.ref_cache[list][scan8[0] - 8 + 4];
		const int16_t *C = sl.mv_cache[list][scan8[0] - 8 + 4];
		if (refc == PART_NOT_AVAILABLE)
		{
			refc = sl.ref_cache[list][scan8[0] - 8 - 1];
			C = sl.mv_cache[list][scan8[0] - 8 - 1];
		}
		unsigned a = (unsigned)left_ref, b = (unsigned)top_ref, c = (unsigned)refc;
		unsigned m = a < b ? a : b;
		m = m < c ? m : c;
		ref[list] = (int)m;
		if (ref[list] >= 0)
		{
			const int16_t *A = sl.mv_cache[list][scan8[0] - 1];
			const int16_t *B = sl.mv_cache[list][scan8[0] - 8];
			int match_count = (left_ref == ref[list]) + (top_ref == ref[list]) + (refc == ref[list]);
			if (match_count > 1)
				mv[list] = pack16to32(mid_pred(A[0], B[0], C[0]), mid_pred(A[1], B[1], C[1]));
			else
			{
				if (left_ref == ref[list]) mv[list] = rn32(A);
				else if (top_ref == ref[list]) mv[list] = rn32(B);
				else mv[list] = rn32(C);
			}
		}
		else
		{
			int mask = ~(MB_TYPE_L0 << (2 * list));
			mv[list] = 0;
			ref[list] = -1;
			if (!is_b8x8) *mb_type &= mask;
			sub_mb_type &= mask;
		}
	}
	if (ref[0] < 0 && ref[1] < 0)
	{
		ref[0] = ref[1] = 0;
		if (!is_b8x8) *mb_type |= MB_TYPE_L0L1;
		sub_mb_type |= MB_TYPE_L0L1;
	}
	if (!(is_b8x8 | mv[0] | mv[1]))
	{
		fill_rectangle(&sl.ref_cache[0][scan8[0]], 4, 4, 8, (uint8_t)ref[0], 1);
		fill_rectangle(&sl.ref_cache[1][scan8[0]], 4, 4, 8, (uint8_t)ref[1], 1);
		fill_rectangle(&sl.mv_cache[0][scan8[0]], 4, 4, 8, 0, 4);
		fill_rectangle(&sl.mv_cache[1][scan8[0]], 4, 4, 8, 0, 4);
		*mb_type = (*mb_type & ~(MB_TYPE_8x8 | MB_TYPE_16x8 | MB_TYPE_8x16 | MB_TYPE_P1L0 | MB_TYPE_P1L1)) |
		           MB_TYPE_16x16 | MB_TYPE_DIRECT2;
		return;
	}
	mb_type_col[0] = mb_type_col[1] = col->mb_type[mb_xy];
	sub_mb_type |= MB_TYPE_16x16 | MB_TYPE_DIRECT2;
	if (!is_b8x8 && (mb_type_col[0] & MB_TYPE_16x16_OR_INTRA))
		*mb_type |= MB_TYPE_16x16 | MB_TYPE_DIRECT2;
	else if (!is_b8x8 && (mb_type_col[0] & (MB_TYPE_16x8 | MB_TYPE_8x16)))
		*mb_type |= MB_TYPE_DIRECT2 | (mb_type_col[0] & (MB_TYPE_16x8 | MB_TYPE_8x16));
	else
	{
		if (!D.direct_8x8_inference) sub_mb_type += (MB_TYPE_8x8 - MB_TYPE_16x16);
		*mb_type |= MB_TYPE_8x8;
	}
	l1mv0 = (const void *)&col->motion_val[0][mb2b(mb_xy)];
	l1mv1 = (const void *)&col->motion_val[1][mb2b(mb_xy)];
	l1ref0 = &col->ref_index[0][4 * mb_xy];
	l1ref1 = &col->ref_index[1][4 * mb_xy];
	if (IS_16X16(*mb_type))
	{
		uint32_t a, b;
		fill_rectangle(&sl.ref_cache[0][scan8[0]], 4, 4, 8, (uint8_t)ref[0], 1);
		fill_rectangle(&sl.ref_cache[1][scan8[0]], 4, 4, 8, (uint8_t)ref[1], 1);
		if (!IS_INTRA(mb_type_col[0]) && !D.l1_long &&
		    ((l1ref0[0] == 0 && iabs(l1mv0[0][0]) <= 1 && iabs(l1mv0[0][1]) <= 1) ||
		     (l1ref0[0] < 0 && !l1ref1[0] && iabs(l1mv1[0][0]) <= 1 && iabs(l1mv1[0][1]) <= 1 && D.x264_gt33)))
		{
			a = b = 0;
			if (ref[0] > 0) a = mv[0];
			if (ref[1] > 0) b = mv[1];
		}
		else
		{
			a = mv[0];
			b = mv[1];
		}
		fill_rectangle(&sl.mv_cache[0][scan8[0]], 4, 4, 8, a, 4);
		fill_rectangle(&sl.mv_cache[1][scan8[0]], 4, 4, 8, b, 4);
	}
	else
	{
		int n = 0;
		for (i8 = 0; i8 < 4; i8++)
		{
			const int x8 = i8 & 1, y8 = i8 >> 1;
			if (is_b8x8 && !IS_DIRECT(sl.sub_mb_type[i8])) continue;
			sl.sub_mb_type[i8] = sub_mb_type;
			fill_rectangle(&sl.mv_cache[0][scan8[i8 * 4]], 2, 2, 8, mv[0], 4);
			fill_rectangle(&sl.mv_cache[1][scan8[i8 * 4]], 2, 2, 8, mv[1], 4);
			fill_rectangle(&sl.ref_cache[0][scan8[i8 * 4]], 2, 2, 8, (uint8_t)ref[0], 1);
			fill_rectangle(&sl.ref_cache[1][scan8[i8 * 4]], 2, 2, 8, (uint8_t)ref[1], 1);
			if (!IS_INTRA(mb_type_col[0]) && !D.l1_long &&
			    (l1ref0[i8] == 0 || (l1ref0[i8] < 0 && l1ref1[i8] == 0 && D.x264_gt33)))
			{
				const int16_t (*l1mv)[2] = l1ref0[i8] == 0 ? l1mv0 : l1mv1;
				if (IS_SUB_8X8(sub_mb_type))
				{
					const int16_t *mv_col = l1mv[x8 * 3 + y8 * 3 * b4_stride];
					if (iabs(mv_col[0]) <= 1 && iabs(mv_col[1]) <= 1)
					{
						if (ref[0] == 0) fill_rectangle(&sl.mv_cache[0][scan8[i8 * 4]], 2, 2, 8, 0, 4);
						if (ref[1] == 0) fill_rectangle(&sl.mv_cache[1][scan8[i8 * 4]], 2, 2, 8, 0, 4);
						n += 4;
					}
				}
				else
				{
					int m = 0;
					for (i4 = 0; i4 < 4; i4++)
					{
						const int16_t *mv_col = l1mv[x8 * 2 + (i4 & 1) + (y8 * 2 + (i4 >> 1)) * b4_stride];
						if (iabs(mv_col[0]) <= 1 && iabs(mv_col[1]) <= 1)
						{
							if (ref[0] == 0) memset(sl.mv_cache[0][scan8[i8 * 4 + i4]], 0, 4);
							if (ref[1] == 0) memset(sl.mv_cache[1][scan8[i8 * 4 + i4]], 0, 4);
							m++;
						}
					}
					if (!(m & 3)) sl.sub_mb_type[i8] += MB_TYPE_16x16 - MB_TYPE_8x8;
					n += m;
				}
			}
		}
		if (!is_b8x8 && !(n & 15))
			*mb_type = (*mb_type & ~(MB_TYPE_8x8 | MB_TYPE_16x8 | MB_TYPE_8x16 | MB_TYPE_P1L0 | MB_TYPE_P1L1)) |
			           MB_TYPE_16x16 | MB_TYPE_DIRECT2;
	}
}

static void pred_temp_direct_motion(int *mb_type)
{
	int b4_stride = b_stride;
	int mb_xy = sl.mb_xy;
	int mb_type_col[2];
	const int16_t (*l1mv0)[2], (*l1mv1)[2];
	const int8_t *l1ref0, *l1ref1;
	const int is_b8x8 = IS_8X8(*mb_type);
	unsigned int sub_mb_type;
	int i8, i4;
	Pic *col = col_pic();

	mb_type_col[0] = mb_type_col[1] = col->mb_type[mb_xy];
	sub_mb_type = MB_TYPE_16x16 | MB_TYPE_P0L0 | MB_TYPE_P0L1 | MB_TYPE_DIRECT2;
	if (!is_b8x8 && (mb_type_col[0] & MB_TYPE_16x16_OR_INTRA))
		*mb_type |= MB_TYPE_16x16 | MB_TYPE_P0L0 | MB_TYPE_P0L1 | MB_TYPE_DIRECT2;
	else if (!is_b8x8 && (mb_type_col[0] & (MB_TYPE_16x8 | MB_TYPE_8x16)))
		*mb_type |= MB_TYPE_L0L1 | MB_TYPE_DIRECT2 | (mb_type_col[0] & (MB_TYPE_16x8 | MB_TYPE_8x16));
	else
	{
		if (!D.direct_8x8_inference)
			sub_mb_type = MB_TYPE_8x8 | MB_TYPE_P0L0 | MB_TYPE_P0L1 | MB_TYPE_DIRECT2;
		*mb_type |= MB_TYPE_8x8 | MB_TYPE_L0L1;
	}
	l1mv0 = (const void *)&col->motion_val[0][mb2b(mb_xy)];
	l1mv1 = (const void *)&col->motion_val[1][mb2b(mb_xy)];
	l1ref0 = &col->ref_index[0][4 * mb_xy];
	l1ref1 = &col->ref_index[1][4 * mb_xy];
	{
		const int *map_col_to_list0[2] = { D.map_col[0], D.map_col[1] };
		const int *dist_scale_factor = D.dist_scale;
		const int ref_offset = 0;
		if (IS_16X16(*mb_type))
		{
			int ref;
			uint32_t mv0, mv1;
			fill_rectangle(&sl.ref_cache[1][scan8[0]], 4, 4, 8, 0, 1);
			if (IS_INTRA(mb_type_col[0]))
				ref = mv0 = mv1 = 0;
			else
			{
				const int ref0 = l1ref0[0] >= 0 ? map_col_to_list0[0][l1ref0[0] + ref_offset]
				                                : map_col_to_list0[1][l1ref1[0] + ref_offset];
				const int scale = dist_scale_factor[ref0];
				const int16_t *mv_col = l1ref0[0] >= 0 ? l1mv0[0] : l1mv1[0];
				int mv_l0[2];
				mv_l0[0] = (scale * mv_col[0] + 128) >> 8;
				mv_l0[1] = (scale * mv_col[1] + 128) >> 8;
				ref = ref0;
				mv0 = pack16to32(mv_l0[0], mv_l0[1]);
				mv1 = pack16to32(mv_l0[0] - mv_col[0], mv_l0[1] - mv_col[1]);
			}
			fill_rectangle(&sl.ref_cache[0][scan8[0]], 4, 4, 8, ref, 1);
			fill_rectangle(&sl.mv_cache[0][scan8[0]], 4, 4, 8, mv0, 4);
			fill_rectangle(&sl.mv_cache[1][scan8[0]], 4, 4, 8, mv1, 4);
		}
		else
		{
			for (i8 = 0; i8 < 4; i8++)
			{
				const int x8 = i8 & 1, y8 = i8 >> 1;
				int ref0, scale;
				const int16_t (*l1mv)[2] = l1mv0;
				if (is_b8x8 && !IS_DIRECT(sl.sub_mb_type[i8])) continue;
				sl.sub_mb_type[i8] = sub_mb_type;
				fill_rectangle(&sl.ref_cache[1][scan8[i8 * 4]], 2, 2, 8, 0, 1);
				if (IS_INTRA(mb_type_col[0]))
				{
					fill_rectangle(&sl.ref_cache[0][scan8[i8 * 4]], 2, 2, 8, 0, 1);
					fill_rectangle(&sl.mv_cache[0][scan8[i8 * 4]], 2, 2, 8, 0, 4);
					fill_rectangle(&sl.mv_cache[1][scan8[i8 * 4]], 2, 2, 8, 0, 4);
					continue;
				}
				ref0 = l1ref0[i8];
				if (ref0 >= 0)
					ref0 = map_col_to_list0[0][ref0 + ref_offset];
				else
				{
					ref0 = map_col_to_list0[1][l1ref1[i8] + ref_offset];
					l1mv = l1mv1;
				}
				scale = dist_scale_factor[ref0];
				fill_rectangle(&sl.ref_cache[0][scan8[i8 * 4]], 2, 2, 8, ref0, 1);
				if (IS_SUB_8X8(sub_mb_type))
				{
					const int16_t *mv_col = l1mv[x8 * 3 + y8 * 3 * b4_stride];
					int mx = (scale * mv_col[0] + 128) >> 8;
					int my = (scale * mv_col[1] + 128) >> 8;
					fill_rectangle(&sl.mv_cache[0][scan8[i8 * 4]], 2, 2, 8, pack16to32(mx, my), 4);
					fill_rectangle(&sl.mv_cache[1][scan8[i8 * 4]], 2, 2, 8, pack16to32(mx - mv_col[0], my - mv_col[1]), 4);
				}
				else
				{
					for (i4 = 0; i4 < 4; i4++)
					{
						const int16_t *mv_col = l1mv[x8 * 2 + (i4 & 1) + (y8 * 2 + (i4 >> 1)) * b4_stride];
						int16_t *mv_l0 = sl.mv_cache[0][scan8[i8 * 4 + i4]];
						mv_l0[0] = (scale * mv_col[0] + 128) >> 8;
						mv_l0[1] = (scale * mv_col[1] + 128) >> 8;
						uint32_t v = pack16to32(mv_l0[0] - mv_col[0], mv_l0[1] - mv_col[1]);
						memcpy(sl.mv_cache[1][scan8[i8 * 4 + i4]], &v, 4);
					}
				}
			}
		}
	}
}

static void pred_direct_motion(int *mb_type)
{
	if (sl.direct_spatial_mv_pred) pred_spatial_direct_motion(mb_type);
	else pred_temp_direct_motion(mb_type);
}

// ---------------------------------------------------------------- skip

static void decode_mb_skip(void)
{
	const int mb_xy = sl.mb_xy;
	int mb_type = 0;
	memset(non_zero_count[mb_xy], 0, 48);
	if (sl.slice_type_nos == 1)
	{
		mb_type |= MB_TYPE_L0L1 | MB_TYPE_DIRECT2 | MB_TYPE_SKIP;
		if (sl.direct_spatial_mv_pred)
		{
			fill_decode_neighbors(mb_type);
			fill_decode_caches(mb_type);
		}
		pred_direct_motion(&mb_type);
		mb_type |= MB_TYPE_SKIP;
	}
	else
	{
		mb_type |= MB_TYPE_16x16 | MB_TYPE_P0L0 | MB_TYPE_P1L0 | MB_TYPE_SKIP;
		fill_decode_neighbors(mb_type);
		pred_pskip_motion();
	}
	write_back_motion(mb_type);
	cur->mb_type[mb_xy] = mb_type;
	cur->qscale_table[mb_xy] = sl.qscale;
	cur->slice_table[mb_xy] = sl.slice_num;
}

// ---------------------------------------------------------------- intra mode checks

static int check_intra4x4_pred_mode(int8_t *pred_mode_cache, int top_samples_available, int left_samples_available)
{
	static const int8_t top[12] = { -1, 0, LEFT_DC_PRED, -1, -1, -1, -1, -1, 0 };
	static const int8_t left[12] = { 0, -1, TOP_DC_PRED, 0, -1, -1, -1, 0, -1, DC_128_PRED };
	if (!(top_samples_available & 0x8000))
		for (int i = 0; i < 4; i++)
		{
			int status = top[pred_mode_cache[scan8[0] + i]];
			if (status < 0) return -1;
			else if (status) pred_mode_cache[scan8[0] + i] = status;
		}
	if ((left_samples_available & 0x8888) != 0x8888)
	{
		static const int mask[4] = { 0x8000, 0x2000, 0x80, 0x20 };
		for (int i = 0; i < 4; i++)
			if (!(left_samples_available & mask[i]))
			{
				int status = left[pred_mode_cache[scan8[0] + 8 * i]];
				if (status < 0) return -1;
				else if (status) pred_mode_cache[scan8[0] + 8 * i] = status;
			}
	}
	return 0;
}

static int check_intra_pred_mode(int top_samples_available, int left_samples_available, int mode, int is_chroma)
{
	static const int8_t top[4] = { LEFT_DC_PRED8x8, 1, -1, -1 };
	static const int8_t left[5] = { TOP_DC_PRED8x8, -1, 2, -1, DC_128_PRED8x8 };
	if (mode > 3) return -1;
	if (!(top_samples_available & 0x8000))
	{
		mode = top[mode];
		if (mode < 0) return -1;
	}
	if ((left_samples_available & 0x8080) != 0x8080)
	{
		mode = left[mode];
		if (mode < 0) return -1;
		if (is_chroma && (left_samples_available & 0x8080))
			mode = ALZHEIMER_DC_L0T_PRED8x8 + (!(left_samples_available & 0x8000)) + 2 * (mode == DC_128_PRED8x8);
	}
	return mode;
}

static int pred_intra_mode(int n)
{
	const int index8 = scan8[n];
	const int left = sl.intra4x4_pred_mode_cache[index8 - 1];
	const int top = sl.intra4x4_pred_mode_cache[index8 - 8];
	const int min = FFMIN(left, top);
	return min < 0 ? DC_PRED : min;
}

static int get_dct8x8_allowed(void)
{
	uint32_t m = MB_TYPE_16x8 | MB_TYPE_8x16 | MB_TYPE_8x8;
	if (!D.direct_8x8_inference) m |= MB_TYPE_DIRECT2;
	for (int i = 0; i < 4; i++) if (sl.sub_mb_type[i] & m) return 0;
	return 1;
}

// ---------------------------------------------------------------- syntax elements

static int decode_cabac_intra_mb_type(int ctx_base, int intra_slice)
{
	uint8_t *state = &sl.cabac_state[ctx_base];
	int mb_type;
	if (intra_slice)
	{
		int ctx = 0;
		if (sl.left_type[0] & (MB_TYPE_INTRA16x16 | MB_TYPE_INTRA_PCM)) ctx++;
		if (sl.top_type & (MB_TYPE_INTRA16x16 | MB_TYPE_INTRA_PCM)) ctx++;
		if (get_cabac(&state[ctx]) == 0) return 0;
		state += 2;
	}
	else if (get_cabac(state) == 0) return 0;
	if (get_cabac_terminate()) return 25;
	mb_type = 1;
	mb_type += 12 * get_cabac(&state[1]);
	if (get_cabac(&state[2])) mb_type += 4 + 4 * get_cabac(&state[2 + intra_slice]);
	mb_type += 2 * get_cabac(&state[3 + intra_slice]);
	mb_type += 1 * get_cabac(&state[3 + 2 * intra_slice]);
	return mb_type;
}

static int decode_cabac_mb_skip(void)
{
	int mba_xy = sl.mb_xy - 1, mbb_xy = sl.mb_xy - mb_stride, ctx = 0;
	if (cur->slice_table[mba_xy] == sl.slice_num && !IS_SKIP(cur->mb_type[mba_xy])) ctx++;
	if (cur->slice_table[mbb_xy] == sl.slice_num && !IS_SKIP(cur->mb_type[mbb_xy])) ctx++;
	if (sl.slice_type_nos == 1) ctx += 13;
	return get_cabac(&sl.cabac_state[11 + ctx]);
}

static int decode_cabac_mb_intra4x4_pred_mode(int pred_mode)
{
	int mode = 0;
	if (get_cabac(&sl.cabac_state[68])) return pred_mode;
	mode += 1 * get_cabac(&sl.cabac_state[69]);
	mode += 2 * get_cabac(&sl.cabac_state[69]);
	mode += 4 * get_cabac(&sl.cabac_state[69]);
	return mode + (mode >= pred_mode);
}

static int decode_cabac_mb_chroma_pre_mode(void)
{
	const int mba_xy = sl.left_mb_xy[0], mbb_xy = sl.top_mb_xy;
	int ctx = 0;
	if (sl.left_type[0] && chroma_pred_mode_table[mba_xy] != 0) ctx++;
	if (sl.top_type && chroma_pred_mode_table[mbb_xy] != 0) ctx++;
	if (get_cabac(&sl.cabac_state[64 + ctx]) == 0) return 0;
	if (get_cabac(&sl.cabac_state[64 + 3]) == 0) return 1;
	if (get_cabac(&sl.cabac_state[64 + 3]) == 0) return 2;
	return 3;
}

static int decode_cabac_mb_cbp_luma(void)
{
	int cbp_b, cbp_a, ctx, cbp = 0;
	cbp_a = sl.left_cbp;
	cbp_b = sl.top_cbp;
	ctx = !(cbp_a & 0x02) + 2 * !(cbp_b & 0x04);
	cbp += get_cabac(&sl.cabac_state[73 + ctx]);
	ctx = !(cbp & 0x01) + 2 * !(cbp_b & 0x08);
	cbp += get_cabac(&sl.cabac_state[73 + ctx]) << 1;
	ctx = !(cbp_a & 0x08) + 2 * !(cbp & 0x01);
	cbp += get_cabac(&sl.cabac_state[73 + ctx]) << 2;
	ctx = !(cbp & 0x04) + 2 * !(cbp & 0x02);
	cbp += get_cabac(&sl.cabac_state[73 + ctx]) << 3;
	return cbp;
}

static int decode_cabac_mb_cbp_chroma(void)
{
	int ctx, cbp_a = (sl.left_cbp >> 4) & 0x03, cbp_b = (sl.top_cbp >> 4) & 0x03;
	ctx = 0;
	if (cbp_a > 0) ctx++;
	if (cbp_b > 0) ctx += 2;
	if (get_cabac(&sl.cabac_state[77 + ctx]) == 0) return 0;
	ctx = 4;
	if (cbp_a == 2) ctx++;
	if (cbp_b == 2) ctx += 2;
	return 1 + get_cabac(&sl.cabac_state[77 + ctx]);
}

static int decode_cabac_p_mb_sub_type(void)
{
	if (get_cabac(&sl.cabac_state[21])) return 0;
	if (!get_cabac(&sl.cabac_state[22])) return 1;
	if (get_cabac(&sl.cabac_state[23])) return 2;
	return 3;
}

static int decode_cabac_b_mb_sub_type(void)
{
	int type;
	if (!get_cabac(&sl.cabac_state[36])) return 0;
	if (!get_cabac(&sl.cabac_state[37])) return 1 + get_cabac(&sl.cabac_state[39]);
	type = 3;
	if (get_cabac(&sl.cabac_state[38]))
	{
		if (get_cabac(&sl.cabac_state[39])) return 11 + get_cabac(&sl.cabac_state[39]);
		type += 4;
	}
	type += 2 * get_cabac(&sl.cabac_state[39]);
	type += get_cabac(&sl.cabac_state[39]);
	return type;
}

static int decode_cabac_mb_ref(int list, int n)
{
	int refa = sl.ref_cache[list][scan8[n] - 1];
	int refb = sl.ref_cache[list][scan8[n] - 8];
	int ref = 0, ctx = 0;
	if (sl.slice_type_nos == 1)
	{
		if (refa > 0 && !(sl.direct_cache[scan8[n] - 1] & (MB_TYPE_DIRECT2 >> 1))) ctx++;
		if (refb > 0 && !(sl.direct_cache[scan8[n] - 8] & (MB_TYPE_DIRECT2 >> 1))) ctx += 2;
	}
	else
	{
		if (refa > 0) ctx++;
		if (refb > 0) ctx += 2;
	}
	while (get_cabac(&sl.cabac_state[54 + ctx]))
	{
		ref++;
		ctx = (ctx >> 2) + 4;
		if (ref >= 32) return -1;
	}
	return ref;
}

static int decode_cabac_mb_mvd(int ctxbase, int amvd, int *mvda)
{
	int mvd;
	if (!get_cabac(&sl.cabac_state[ctxbase + (amvd > 2) + (amvd > 32)]))
	{
		*mvda = 0;
		return 0;
	}
	mvd = 1;
	ctxbase += 3;
	while (mvd < 9 && get_cabac(&sl.cabac_state[ctxbase]))
	{
		if (mvd < 4) ctxbase++;
		mvd++;
	}
	if (mvd >= 9)
	{
		int k = 3;
		while (get_cabac_bypass())
		{
			mvd += 1 << k;
			k++;
			if (k > 24) return -0x7FFFFFFF;
		}
		while (k--) mvd += get_cabac_bypass() << k;
		*mvda = mvd < 70 ? mvd : 70;
	}
	else
		*mvda = mvd;
	return get_cabac_bypass() ? -mvd : mvd;
}

#define DECODE_CABAC_MB_MVD(list, n) \
{ \
	int amvd0 = sl.mvd_cache[list][scan8[n] - 1][0] + sl.mvd_cache[list][scan8[n] - 8][0]; \
	int amvd1 = sl.mvd_cache[list][scan8[n] - 1][1] + sl.mvd_cache[list][scan8[n] - 8][1]; \
	int mxd = decode_cabac_mb_mvd(40, amvd0, &mpx); \
	int myd = decode_cabac_mb_mvd(47, amvd1, &mpy); \
	if (mxd == -0x7FFFFFFF || myd == -0x7FFFFFFF) return -1; \
	mx += mxd; \
	my += myd; \
}

// ---------------------------------------------------------------- residual

static int get_cabac_cbf_ctx(int cat, int idx, int is_dc)
{
	int nza, nzb, ctx = 0;
	static const uint16_t base_ctx[14] = { 85, 89, 93, 97, 101, 1012, 460, 464, 468, 1016, 472, 476, 480, 1020 };
	if (is_dc)
	{
		if (cat == 3)
		{
			idx -= CHROMA_DC_BLOCK_INDEX;
			nza = (sl.left_cbp >> (6 + idx)) & 0x01;
			nzb = (sl.top_cbp >> (6 + idx)) & 0x01;
		}
		else
		{
			idx -= LUMA_DC_BLOCK_INDEX;
			nza = sl.left_cbp & (0x100 << idx);
			nzb = sl.top_cbp & (0x100 << idx);
		}
	}
	else
	{
		nza = sl.non_zero_count_cache[scan8[idx] - 1];
		nzb = sl.non_zero_count_cache[scan8[idx] - 8];
	}
	if (nza > 0) ctx++;
	if (nzb > 0) ctx += 2;
	return base_ctx[cat] + ctx;
}

static void decode_cabac_residual_internal(int16_t *block, int cat, int n, const uint8_t *scantable,
                                           const uint32_t *qmul, int max_coeff, int is_dc)
{
	static const int significant_coeff_flag_offset[6] = { 105 + 0, 105 + 15, 105 + 29, 105 + 44, 105 + 47, 402 };
	static const int last_coeff_flag_offset[6] = { 166 + 0, 166 + 15, 166 + 29, 166 + 44, 166 + 47, 417 };
	static const int coeff_abs_level_m1_offset[6] = { 227 + 0, 227 + 10, 227 + 20, 227 + 30, 227 + 39, 426 };
	static const uint8_t coeff_abs_level1_ctx[8] = { 1, 2, 3, 4, 0, 0, 0, 0 };
	static const uint8_t coeff_abs_levelgt1_ctx[8] = { 5, 5, 5, 5, 6, 7, 8, 9 };
	static const uint8_t coeff_abs_level_transition[2][8] = {
		{ 1, 2, 3, 3, 4, 5, 6, 7 },
		{ 4, 4, 4, 4, 5, 6, 7, 7 }
	};
	int index[64];
	int last, coeff_count = 0, node_ctx = 0;
	uint8_t *sig = sl.cabac_state + significant_coeff_flag_offset[cat];
	uint8_t *lastc = sl.cabac_state + last_coeff_flag_offset[cat];
	uint8_t *absc = sl.cabac_state + coeff_abs_level_m1_offset[cat];

	if (!is_dc && max_coeff == 64)
	{
		for (last = 0; last < 63; last++)
			if (get_cabac(sig + cab_sig8[last]))
			{
				index[coeff_count++] = last;
				if (get_cabac(lastc + cab_last8[last])) { last = max_coeff; break; }
			}
	}
	else
	{
		for (last = 0; last < max_coeff - 1; last++)
			if (get_cabac(sig + last))
			{
				index[coeff_count++] = last;
				if (get_cabac(lastc + last)) { last = max_coeff; break; }
			}
	}
	if (last == max_coeff - 1) index[coeff_count++] = last;

	if (is_dc)
	{
		if (cat == 3) cbp_table[sl.mb_xy] |= 0x40 << (n - CHROMA_DC_BLOCK_INDEX);
		else cbp_table[sl.mb_xy] |= 0x100 << (n - LUMA_DC_BLOCK_INDEX);
		sl.non_zero_count_cache[scan8[n]] = coeff_count;
	}
	else
	{
		if (max_coeff == 64) fill_rectangle(&sl.non_zero_count_cache[scan8[n]], 2, 2, 8, coeff_count, 1);
		else sl.non_zero_count_cache[scan8[n]] = coeff_count;
	}

	do
	{
		uint8_t *ctx = coeff_abs_level1_ctx[node_ctx] + absc;
		int j = scantable[index[--coeff_count]];
		int s;
		if (get_cabac(ctx) == 0)
		{
			node_ctx = coeff_abs_level_transition[0][node_ctx];
			s = get_cabac_bypass() ? -1 : 1;
		}
		else
		{
			unsigned coeff_abs = 2;
			ctx = coeff_abs_levelgt1_ctx[node_ctx] + absc;
			node_ctx = coeff_abs_level_transition[1][node_ctx];
			while (coeff_abs < 15 && get_cabac(ctx)) coeff_abs++;
			if (coeff_abs >= 15)
			{
				int k = 0;
				while (get_cabac_bypass() && k < 16 + 7) k++;
				coeff_abs = 1;
				while (k--) coeff_abs += coeff_abs + get_cabac_bypass();
				coeff_abs += 14U;
			}
			s = get_cabac_bypass() ? -(int)coeff_abs : (int)coeff_abs;
		}
		if (is_dc) block[j] = s;
		else block[j] = (int)(s * qmul[j] + 32) >> 6;
	} while (coeff_count);
}

static void decode_cabac_residual_dc(int16_t *block, int cat, int n, const uint8_t *scantable, int max_coeff)
{
	if (get_cabac(&sl.cabac_state[get_cabac_cbf_ctx(cat, n, 1)]) == 0)
	{
		sl.non_zero_count_cache[scan8[n]] = 0;
		return;
	}
	decode_cabac_residual_internal(block, cat, n, scantable, NULL, max_coeff, 1);
}

static void decode_cabac_residual_nondc(int16_t *block, int cat, int n, const uint8_t *scantable, const uint32_t *qmul, int max_coeff)
{
	if (cat != 5 && get_cabac(&sl.cabac_state[get_cabac_cbf_ctx(cat, n, 0)]) == 0)
	{
		if (max_coeff == 64) fill_rectangle(&sl.non_zero_count_cache[scan8[n]], 2, 2, 8, 0, 1);
		else sl.non_zero_count_cache[scan8[n]] = 0;
		return;
	}
	decode_cabac_residual_internal(block, cat, n, scantable, qmul, max_coeff, 0);
}

static void decode_cabac_luma_residual(const uint8_t *scan, const uint8_t *scan8x8, int mb_type, int cbp)
{
	int qscale = sl.qscale;
	if (IS_INTRA16x16(mb_type))
	{
		memset(sl.mb_luma_dc, 0, sizeof(sl.mb_luma_dc));
		decode_cabac_residual_dc(sl.mb_luma_dc, 0, LUMA_DC_BLOCK_INDEX, scan, 16);
		if (cbp & 15)
		{
			const uint32_t *qmul = dequant4[0][qscale];
			for (int i4x4 = 0; i4x4 < 16; i4x4++)
				decode_cabac_residual_nondc(sl.mb + 16 * i4x4, 1, i4x4, scan + 1, qmul, 15);
		}
		else
			fill_rectangle(&sl.non_zero_count_cache[scan8[0]], 4, 4, 8, 0, 1);
	}
	else
	{
		int cqm = IS_INTRA(mb_type) ? 0 : 3;
		for (int i8x8 = 0; i8x8 < 4; i8x8++)
		{
			if (cbp & (1 << i8x8))
			{
				if (IS_8x8DCT(mb_type))
					decode_cabac_residual_nondc(sl.mb + 16 * 4 * i8x8, 5, 4 * i8x8, scan8x8, dequant8[cqm][qscale], 64);
				else
				{
					const uint32_t *qmul = dequant4[cqm][qscale];
					for (int i4x4 = 0; i4x4 < 4; i4x4++)
					{
						const int index = 4 * i8x8 + i4x4;
						decode_cabac_residual_nondc(sl.mb + 16 * index, 2, index, scan, qmul, 16);
					}
				}
			}
			else
				fill_rectangle(&sl.non_zero_count_cache[scan8[4 * i8x8]], 2, 2, 8, 0, 1);
		}
	}
}

// ---------------------------------------------------------------- macroblock

static int decode_mb_cabac(void)
{
	int mb_xy, mb_type, partition_count, cbp = 0;
	int dct8x8_allowed = D.t8x8;

	mb_xy = sl.mb_xy = sl.mb_x + sl.mb_y * mb_stride;
	if (sl.slice_type_nos != 2)
	{
		if (decode_cabac_mb_skip())
		{
			decode_mb_skip();
			cbp_table[mb_xy] = 0;
			chroma_pred_mode_table[mb_xy] = 0;
			sl.last_qscale_diff = 0;
			return 0;
		}
	}
	fill_decode_neighbors(0);

	if (sl.slice_type_nos == 1)
	{
		int ctx = 0;
		if (!IS_DIRECT(sl.left_type[0] - 1)) ctx++;
		if (!IS_DIRECT(sl.top_type - 1)) ctx++;
		if (!get_cabac(&sl.cabac_state[27 + ctx]))
			mb_type = 0;
		else if (!get_cabac(&sl.cabac_state[27 + 3]))
			mb_type = 1 + get_cabac(&sl.cabac_state[27 + 5]);
		else
		{
			int bits;
			bits = get_cabac(&sl.cabac_state[27 + 4]) << 3;
			bits += get_cabac(&sl.cabac_state[27 + 5]) << 2;
			bits += get_cabac(&sl.cabac_state[27 + 5]) << 1;
			bits += get_cabac(&sl.cabac_state[27 + 5]);
			if (bits < 8) mb_type = bits + 3;
			else if (bits == 13)
			{
				mb_type = decode_cabac_intra_mb_type(32, 0);
				goto decode_intra_mb;
			}
			else if (bits == 14) mb_type = 11;
			else if (bits == 15) mb_type = 22;
			else
			{
				bits = (bits << 1) + get_cabac(&sl.cabac_state[27 + 5]);
				mb_type = bits - 4;
			}
		}
		partition_count = b_mb_type_info[mb_type].partition_count;
		mb_type = b_mb_type_info[mb_type].type;
	}
	else if (sl.slice_type_nos == 0)
	{
		if (get_cabac(&sl.cabac_state[14]) == 0)
		{
			if (get_cabac(&sl.cabac_state[15]) == 0)
				mb_type = 3 * get_cabac(&sl.cabac_state[16]);
			else
				mb_type = 2 - get_cabac(&sl.cabac_state[17]);
			partition_count = p_mb_type_info[mb_type].partition_count;
			mb_type = p_mb_type_info[mb_type].type;
		}
		else
		{
			mb_type = decode_cabac_intra_mb_type(17, 0);
			goto decode_intra_mb;
		}
	}
	else
	{
		mb_type = decode_cabac_intra_mb_type(3, 1);
decode_intra_mb:
		partition_count = 0;
		cbp = i_mb_type_info[mb_type].cbp;
		sl.intra16x16_pred_mode = i_mb_type_info[mb_type].pred_mode;
		mb_type = i_mb_type_info[mb_type].type;
	}

	cur->slice_table[mb_xy] = sl.slice_num;

	if (IS_INTRA_PCM(mb_type))
	{
		// the samples start at the byte after the bits the engine has taken
		uint64_t at = (bitpos + 7) >> 3;
		if (at + 384 > D.nb) return -1;
		sl.intra_pcm_ptr = bs + at;
		bitpos = 8 * (at + 384);
		engine_init();
		cbp_table[mb_xy] = 0xf7ef;
		chroma_pred_mode_table[mb_xy] = 0;
		cur->qscale_table[mb_xy] = 0;
		memset(non_zero_count[mb_xy], 16, 48);
		cur->mb_type[mb_xy] = mb_type;
		sl.last_qscale_diff = 0;
		return 0;
	}

	fill_decode_caches(mb_type);

	if (IS_INTRA(mb_type))
	{
		int i, pred_mode;
		if (IS_INTRA4x4(mb_type))
		{
			if (dct8x8_allowed && get_cabac(&sl.cabac_state[399 + sl.neighbor_transform_size]))
			{
				mb_type |= MB_TYPE_8x8DCT;
				for (i = 0; i < 16; i += 4)
				{
					int pred = pred_intra_mode(i);
					int mode = decode_cabac_mb_intra4x4_pred_mode(pred);
					fill_rectangle(&sl.intra4x4_pred_mode_cache[scan8[i]], 2, 2, 8, mode, 1);
				}
			}
			else
				for (i = 0; i < 16; i++)
				{
					int pred = pred_intra_mode(i);
					sl.intra4x4_pred_mode_cache[scan8[i]] = decode_cabac_mb_intra4x4_pred_mode(pred);
				}
			write_back_intra_pred_mode();
			if (check_intra4x4_pred_mode(sl.intra4x4_pred_mode_cache, sl.top_samples_available, sl.left_samples_available) < 0)
				return -1;
		}
		else
		{
			sl.intra16x16_pred_mode = check_intra_pred_mode(sl.top_samples_available, sl.left_samples_available, sl.intra16x16_pred_mode, 0);
			if (sl.intra16x16_pred_mode < 0) return -1;
		}
		chroma_pred_mode_table[mb_xy] = pred_mode = decode_cabac_mb_chroma_pre_mode();
		pred_mode = check_intra_pred_mode(sl.top_samples_available, sl.left_samples_available, pred_mode, 1);
		if (pred_mode < 0) return -1;
		sl.chroma_pred_mode = pred_mode;
	}
	else if (partition_count == 4)
	{
		int i, j, sub_partition_count[4], list, ref[2][4];
		if (sl.slice_type_nos == 1)
		{
			for (i = 0; i < 4; i++)
			{
				int t = decode_cabac_b_mb_sub_type();
				sub_partition_count[i] = b_sub_mb_type_info[t].partition_count;
				sl.sub_mb_type[i] = b_sub_mb_type_info[t].type;
			}
			if (IS_DIRECT(sl.sub_mb_type[0] | sl.sub_mb_type[1] | sl.sub_mb_type[2] | sl.sub_mb_type[3]))
			{
				pred_direct_motion(&mb_type);
				sl.ref_cache[0][scan8[4]] = sl.ref_cache[1][scan8[4]] =
				sl.ref_cache[0][scan8[12]] = sl.ref_cache[1][scan8[12]] = PART_NOT_AVAILABLE;
				for (i = 0; i < 4; i++)
					fill_rectangle(&sl.direct_cache[scan8[4 * i]], 2, 2, 8, (sl.sub_mb_type[i] >> 1) & 0xFF, 1);
			}
		}
		else
			for (i = 0; i < 4; i++)
			{
				int t = decode_cabac_p_mb_sub_type();
				sub_partition_count[i] = p_sub_mb_type_info[t].partition_count;
				sl.sub_mb_type[i] = p_sub_mb_type_info[t].type;
			}

		for (list = 0; list < sl.list_count; list++)
			for (i = 0; i < 4; i++)
			{
				if (IS_DIRECT(sl.sub_mb_type[i])) continue;
				if (IS_DIR(sl.sub_mb_type[i], 0, list))
				{
					unsigned rc = sl.ref_count[list];
					if (rc > 1)
					{
						ref[list][i] = decode_cabac_mb_ref(list, 4 * i);
						if ((unsigned)ref[list][i] >= rc) return -1;
					}
					else
						ref[list][i] = 0;
				}
				else
					ref[list][i] = -1;
				sl.ref_cache[list][scan8[4 * i] + 1] = sl.ref_cache[list][scan8[4 * i] + 8] =
				sl.ref_cache[list][scan8[4 * i] + 9] = ref[list][i];
			}

		if (dct8x8_allowed) dct8x8_allowed = get_dct8x8_allowed();

		for (list = 0; list < sl.list_count; list++)
			for (i = 0; i < 4; i++)
			{
				sl.ref_cache[list][scan8[4 * i]] = sl.ref_cache[list][scan8[4 * i] + 1];
				if (IS_DIRECT(sl.sub_mb_type[i]))
				{
					fill_rectangle(sl.mvd_cache[list][scan8[4 * i]], 2, 2, 8, 0, 2);
					continue;
				}
				if (IS_DIR(sl.sub_mb_type[i], 0, list) && !IS_DIRECT(sl.sub_mb_type[i]))
				{
					const int sub_mb_type = sl.sub_mb_type[i];
					const int block_width = (sub_mb_type & (MB_TYPE_16x16 | MB_TYPE_16x8)) ? 2 : 1;
					for (j = 0; j < sub_partition_count[i]; j++)
					{
						int mpx, mpy, mx, my;
						const int index = 4 * i + block_width * j;
						int16_t (*mv_cache)[2] = &sl.mv_cache[list][scan8[index]];
						uint8_t (*mvd_cache)[2] = &sl.mvd_cache[list][scan8[index]];
						pred_motion(index, block_width, list, sl.ref_cache[list][scan8[index]], &mx, &my);
						DECODE_CABAC_MB_MVD(list, index)
						if (IS_SUB_8X8(sub_mb_type))
						{
							mv_cache[1][0] = mv_cache[8][0] = mv_cache[9][0] = mx;
							mv_cache[1][1] = mv_cache[8][1] = mv_cache[9][1] = my;
							mvd_cache[1][0] = mvd_cache[8][0] = mvd_cache[9][0] = mpx;
							mvd_cache[1][1] = mvd_cache[8][1] = mvd_cache[9][1] = mpy;
						}
						else if (IS_SUB_8X4(sub_mb_type))
						{
							mv_cache[1][0] = mx;
							mv_cache[1][1] = my;
							mvd_cache[1][0] = mpx;
							mvd_cache[1][1] = mpy;
						}
						else if (IS_SUB_4X8(sub_mb_type))
						{
							mv_cache[8][0] = mx;
							mv_cache[8][1] = my;
							mvd_cache[8][0] = mpx;
							mvd_cache[8][1] = mpy;
						}
						mv_cache[0][0] = mx;
						mv_cache[0][1] = my;
						mvd_cache[0][0] = mpx;
						mvd_cache[0][1] = mpy;
					}
				}
				else
				{
					fill_rectangle(sl.mv_cache[list][scan8[4 * i]], 2, 2, 8, 0, 4);
					fill_rectangle(sl.mvd_cache[list][scan8[4 * i]], 2, 2, 8, 0, 2);
				}
			}
	}
	else if (IS_DIRECT(mb_type))
	{
		pred_direct_motion(&mb_type);
		fill_rectangle(sl.mvd_cache[0][scan8[0]], 4, 4, 8, 0, 2);
		fill_rectangle(sl.mvd_cache[1][scan8[0]], 4, 4, 8, 0, 2);
		dct8x8_allowed &= D.direct_8x8_inference;
	}
	else
	{
		int list, i;
		if (IS_16X16(mb_type))
		{
			for (list = 0; list < sl.list_count; list++)
				if (IS_DIR(mb_type, 0, list))
				{
					int ref;
					unsigned rc = sl.ref_count[list];
					if (rc > 1)
					{
						ref = decode_cabac_mb_ref(list, 0);
						if ((unsigned)ref >= rc) return -1;
					}
					else
						ref = 0;
					fill_rectangle(&sl.ref_cache[list][scan8[0]], 4, 4, 8, ref, 1);
				}
			for (list = 0; list < sl.list_count; list++)
				if (IS_DIR(mb_type, 0, list))
				{
					int mx, my, mpx, mpy;
					pred_motion(0, 4, list, sl.ref_cache[list][scan8[0]], &mx, &my);
					DECODE_CABAC_MB_MVD(list, 0)
					fill_rectangle(sl.mvd_cache[list][scan8[0]], 4, 4, 8, pack8to16(mpx, mpy), 2);
					fill_rectangle(sl.mv_cache[list][scan8[0]], 4, 4, 8, pack16to32(mx, my), 4);
				}
		}
		else if (IS_16X8(mb_type))
		{
			for (list = 0; list < sl.list_count; list++)
				for (i = 0; i < 2; i++)
				{
					if (IS_DIR(mb_type, i, list))
					{
						int ref;
						unsigned rc = sl.ref_count[list];
						if (rc > 1)
						{
							ref = decode_cabac_mb_ref(list, 8 * i);
							if ((unsigned)ref >= rc) return -1;
						}
						else
							ref = 0;
						fill_rectangle(&sl.ref_cache[list][scan8[0] + 16 * i], 4, 2, 8, ref, 1);
					}
					else
						fill_rectangle(&sl.ref_cache[list][scan8[0] + 16 * i], 4, 2, 8, (LIST_NOT_USED & 0xFF), 1);
				}
			for (list = 0; list < sl.list_count; list++)
				for (i = 0; i < 2; i++)
				{
					if (IS_DIR(mb_type, i, list))
					{
						int mx, my, mpx, mpy;
						pred_16x8_motion(8 * i, list, sl.ref_cache[list][scan8[0] + 16 * i], &mx, &my);
						DECODE_CABAC_MB_MVD(list, 8 * i)
						fill_rectangle(sl.mvd_cache[list][scan8[0] + 16 * i], 4, 2, 8, pack8to16(mpx, mpy), 2);
						fill_rectangle(sl.mv_cache[list][scan8[0] + 16 * i], 4, 2, 8, pack16to32(mx, my), 4);
					}
					else
					{
						fill_rectangle(sl.mvd_cache[list][scan8[0] + 16 * i], 4, 2, 8, 0, 2);
						fill_rectangle(sl.mv_cache[list][scan8[0] + 16 * i], 4, 2, 8, 0, 4);
					}
				}
		}
		else
		{
			for (list = 0; list < sl.list_count; list++)
				for (i = 0; i < 2; i++)
				{
					if (IS_DIR(mb_type, i, list))
					{
						int ref;
						unsigned rc = sl.ref_count[list];
						if (rc > 1)
						{
							ref = decode_cabac_mb_ref(list, 4 * i);
							if ((unsigned)ref >= rc) return -1;
						}
						else
							ref = 0;
						fill_rectangle(&sl.ref_cache[list][scan8[0] + 2 * i], 2, 4, 8, ref, 1);
					}
					else
						fill_rectangle(&sl.ref_cache[list][scan8[0] + 2 * i], 2, 4, 8, (LIST_NOT_USED & 0xFF), 1);
				}
			for (list = 0; list < sl.list_count; list++)
				for (i = 0; i < 2; i++)
				{
					if (IS_DIR(mb_type, i, list))
					{
						int mx, my, mpx, mpy;
						pred_8x16_motion(i * 4, list, sl.ref_cache[list][scan8[0] + 2 * i], &mx, &my);
						DECODE_CABAC_MB_MVD(list, 4 * i)
						fill_rectangle(sl.mvd_cache[list][scan8[0] + 2 * i], 2, 4, 8, pack8to16(mpx, mpy), 2);
						fill_rectangle(sl.mv_cache[list][scan8[0] + 2 * i], 2, 4, 8, pack16to32(mx, my), 4);
					}
					else
					{
						fill_rectangle(sl.mvd_cache[list][scan8[0] + 2 * i], 2, 4, 8, 0, 2);
						fill_rectangle(sl.mv_cache[list][scan8[0] + 2 * i], 2, 4, 8, 0, 4);
					}
				}
		}
	}

	if (IS_INTER(mb_type))
	{
		chroma_pred_mode_table[mb_xy] = 0;
		write_back_motion(mb_type);
	}

	if (!IS_INTRA16x16(mb_type))
	{
		cbp = decode_cabac_mb_cbp_luma();
		cbp |= decode_cabac_mb_cbp_chroma() << 4;
	}

	cbp_table[mb_xy] = sl.cbp = cbp;

	if (dct8x8_allowed && (cbp & 15) && !IS_INTRA(mb_type))
		mb_type |= MB_TYPE_8x8DCT * get_cabac(&sl.cabac_state[399 + sl.neighbor_transform_size]);

	cur->mb_type[mb_xy] = mb_type;

	if (cbp || IS_INTRA16x16(mb_type))
	{
		if (get_cabac(&sl.cabac_state[60 + (sl.last_qscale_diff != 0)]))
		{
			int val = 1, ctx = 2;
			const int max_qp = 51;
			while (get_cabac(&sl.cabac_state[60 + ctx]))
			{
				ctx = 3;
				val++;
				if (val > 2 * max_qp) return -1;
			}
			if (val & 0x01) val = (val + 1) >> 1;
			else val = -((val + 1) >> 1);
			sl.last_qscale_diff = val;
			sl.qscale += val;
			if (((unsigned)sl.qscale) > max_qp)
			{
				if (sl.qscale < 0) sl.qscale += max_qp + 1;
				else sl.qscale -= max_qp + 1;
			}
			sl.chroma_qp[0] = get_chroma_qp(0, sl.qscale);
			sl.chroma_qp[1] = get_chroma_qp(1, sl.qscale);
		}
		else
			sl.last_qscale_diff = 0;

		decode_cabac_luma_residual(zz4, zz8, mb_type, cbp);
		if (cbp & 0x30)
			for (int c = 0; c < 2; c++)
				decode_cabac_residual_dc(sl.mb + 256 + 16 * 16 * c, 3, CHROMA_DC_BLOCK_INDEX + c, chroma_dc_scan, 4);
		if (cbp & 0x20)
		{
			for (int c = 0; c < 2; c++)
			{
				const uint32_t *qmul = dequant4[c + 1 + (IS_INTRA(mb_type) ? 0 : 3)][sl.chroma_qp[c]];
				for (int i = 0; i < 4; i++)
				{
					const int index = 16 + 16 * c + i;
					decode_cabac_residual_nondc(sl.mb + 16 * index, 4, index, zz4 + 1, qmul, 15);
				}
			}
		}
		else
		{
			fill_rectangle(&sl.non_zero_count_cache[scan8[16]], 4, 4, 8, 0, 1);
			fill_rectangle(&sl.non_zero_count_cache[scan8[32]], 4, 4, 8, 0, 1);
		}
	}
	else
	{
		fill_rectangle(&sl.non_zero_count_cache[scan8[0]], 4, 4, 8, 0, 1);
		fill_rectangle(&sl.non_zero_count_cache[scan8[16]], 4, 4, 8, 0, 1);
		fill_rectangle(&sl.non_zero_count_cache[scan8[32]], 4, 4, 8, 0, 1);
		sl.last_qscale_diff = 0;
	}

	cur->qscale_table[mb_xy] = sl.qscale;
	write_back_non_zero_count();
	return 0;
}
