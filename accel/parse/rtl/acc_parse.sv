// acc_parse, step 2 of 4.0 (measure only: not in the core yet). From what the
// ARM gives it for each CABAC slice (descriptor + slice data, below) it reads
// every macroblock: CABAC (cab_dec), macroblock syntax, motion vector and
// direct prediction, intra modes, dequantisation, DC transforms. It writes the
// engine commands of each macroblock (A5 + A3 + modes/vectors + coefficients,
// accel/doc/comandi.md), the same words the ARM writes today (A1, A2 and A4
// stay with the ARM).
//
// Progressive frames, 4:2:0, 8 bit, CABAC only. The logic follows the C model
// accel/parse/sim/pmodel.c, a port of FFmpeg's H.264 decoder (LGPL 2.1 or
// later), keeping its caches (scan8 layout) so that every word matches.
//
// Slice descriptor (32-bit words):
//   D0  [31:24] 0x5C, [23:16] slot of the picture, [15:8] slot of the
//       co-located picture (list 1 [0]), [7:0] picture tag
//   D1  [7:0] mb_w, [15:8] mb_h, [23:16] first mb_x, [31:24] first mb_y
//   D2  [15:0] slice_num, [21:16] QP, [23:22] type (0 P, 1 B, 2 I),
//       [25:24] cabac_init_idc, [27:26] list_count, [29:28] deblocking_filter,
//       [31:30] use_weight
//   D3  [5:0] ref_count0, [11:6] ref_count1, [12] direct_spatial,
//       [13] transform_8x8_mode, [14] constrained_intra_pred,
//       [15] direct_8x8_inference, [16] x264_build > 33, [17] list 1 [0] is a
//       long-term reference, [22:18] chroma_qp_index_offset 0, [27:23] offset 1
//   D4  [7:0] slice_alpha_c0_offset, [15:8] slice_beta_offset (signed)
//   B slices with temporal direct: 32 words dist_scale_factor (signed, 15:0),
//       64 words map_col_to_list0 [list 0 then 1][32] (7:0)
//   use_weight 2: ref_count0 x ref_count1 words implicit_weight (8:0)
//   24 words scaling_matrix4 [6][16] bytes (byte i in bits 8 * (i % 4))
//   transform_8x8_mode: 32 words scaling_matrix8 [0] and [3], 64 bytes each
//   1 word: bytes of slice data, then the data words (first byte in 31:24),
//   from the byte after cabac_alignment_one_bit (the RBSP)
//
// Motion memory (32-bit words, for the co-located picture of B slices): each
// macroblock writes 35 words at slot * SLOT_WORDS + mb * 36: packed mb_type,
// ref_index list 0 (4 bytes), list 1, then 16 + 16 motion vectors (x | y << 16).
// mem_req holds until mem_ack; reads return in order on mem_rvalid.
`default_nettype none
module acc_parse #(
	parameter integer MAXW = 128,
	parameter [31:0]  SLOT_WORDS = 32'd294912
) (
	input  wire        clk,
	input  wire        reset,

	input  wire        in_valid,
	output wire        in_ready,
	input  wire [31:0] in_data,

	output wire        out_valid,
	input  wire        out_ready,
	output wire [31:0] out_data,

	output reg         mem_req,
	output reg         mem_we,
	output reg  [31:0] mem_addr,
	output reg  [31:0] mem_wdata,
	input  wire        mem_ack,
	input  wire        mem_rvalid,
	input  wire [31:0] mem_rdata,

	output reg         sl_done,
	output reg         sl_err,
	output wire        busy
);

`include "acc_parse_tables.svh"

// ---------------------------------------------------------------- mb_type (FFmpeg's bits)
localparam [31:0] MT_I4 = 32'h1, MT_I16 = 32'h2, MT_PCM = 32'h4, MT_16 = 32'h8, MT_168 = 32'h10,
                  MT_816 = 32'h20, MT_88 = 32'h40, MT_DIR = 32'h100, MT_SKIP = 32'h800,
                  MT_P0L0 = 32'h1000, MT_P1L0 = 32'h2000, MT_P0L1 = 32'h4000, MT_P1L1 = 32'h8000,
                  MT_L0 = 32'h3000, MT_L1 = 32'hC000, MT_L0L1 = 32'hF000, MT_DCT = 32'h01000000,
                  MT_16I = 32'h0F;    // 16x16 or intra

function automatic logic uses(input [31:0] t, input logic l);
	uses = l ? (t[15:14] != 2'd0) : (t[13:12] != 2'd0);
endfunction
function automatic logic isdir(input [31:0] t, input logic p, input logic l);
	isdir = t[12 + {l, p}];
endfunction
function automatic logic intra(input [31:0] t); intra = t[2:0] != 3'd0; endfunction
function automatic logic inter(input [31:0] t); inter = t[6:3] != 4'd0; endfunction
function automatic [13:0] tpack(input [31:0] t); tpack = {t[24], t[15:11], t[8], t[6:0]}; endfunction
function automatic [31:0] tunpack(input [13:0] p);
	tunpack = {7'd0, p[13], 8'd0, p[12:8], 2'd0, p[7], 1'b0, p[6:0]};
endfunction

function automatic [3:0] t4(input [3:0] x); t4 = {x[1:0], x[3:2]}; endfunction
function automatic [5:0] t8(input [5:0] x); t8 = {x[2:0], x[5:3]}; endfunction

function automatic signed [15:0] mid_pred(input signed [15:0] a, input signed [15:0] b, input signed [15:0] c);
	reg signed [15:0] r;
	begin
		r = b;
		if (a > b) begin if (c > b) r = c > a ? a : c; end
		else begin if (b > c) r = c > a ? c : a; end
		mid_pred = r;
	end
endfunction

function automatic [4:0] d4init(input [2:0] m, input [1:0] c);
	case ({m, c})
	5'd0: d4init = 10; 5'd1: d4init = 13; 5'd2: d4init = 16;
	5'd4: d4init = 11; 5'd5: d4init = 14; 5'd6: d4init = 18;
	5'd8: d4init = 13; 5'd9: d4init = 16; 5'd10: d4init = 20;
	5'd12: d4init = 14; 5'd13: d4init = 18; 5'd14: d4init = 23;
	5'd16: d4init = 16; 5'd17: d4init = 20; 5'd18: d4init = 25;
	5'd20: d4init = 18; 5'd21: d4init = 23; 5'd22: d4init = 29;
	default: d4init = 0;
	endcase
endfunction
function automatic [5:0] d8init(input [2:0] m, input [2:0] c);
	reg [35:0] r;
	begin
		case (m)
		3'd0: r = {6'd24, 6'd25, 6'd19, 6'd32, 6'd18, 6'd20};
		3'd1: r = {6'd26, 6'd28, 6'd21, 6'd35, 6'd19, 6'd22};
		3'd2: r = {6'd31, 6'd33, 6'd24, 6'd42, 6'd23, 6'd26};
		3'd3: r = {6'd33, 6'd35, 6'd26, 6'd45, 6'd25, 6'd28};
		3'd4: r = {6'd38, 6'd40, 6'd30, 6'd51, 6'd28, 6'd32};
		default: r = {6'd43, 6'd46, 6'd34, 6'd58, 6'd32, 6'd36};
		endcase
		d8init = r[6 * c +: 6];
	end
endfunction
function automatic [2:0] d8scan(input [3:0] i);
	case (i)
	4'd0: d8scan = 0; 4'd1: d8scan = 3; 4'd2: d8scan = 4; 4'd3: d8scan = 3;
	4'd4: d8scan = 3; 4'd5: d8scan = 1; 4'd6: d8scan = 5; 4'd7: d8scan = 1;
	4'd8: d8scan = 4; 4'd9: d8scan = 5; 4'd10: d8scan = 2; 4'd11: d8scan = 5;
	4'd12: d8scan = 3; 4'd13: d8scan = 1; 4'd14: d8scan = 5; default: d8scan = 1;
	endcase
endfunction
// QP / 6 and QP % 6, QP 0..51
function automatic [6:0] qpdm(input [5:0] q);
	reg [3:0] d;
	begin
		d = q >= 48 ? 4'd8 : q >= 42 ? 4'd7 : q >= 36 ? 4'd6 : q >= 30 ? 4'd5 : q >= 24 ? 4'd4 :
		    q >= 18 ? 4'd3 : q >= 12 ? 4'd2 : q >= 6 ? 4'd1 : 4'd0;
		qpdm = {d, 3'(q - 6'(d * 6))};
	end
endfunction
function automatic [3:0] qdiv(input [5:0] q); reg [6:0] r; begin r = qpdm(q); qdiv = r[6:3]; end endfunction
function automatic [2:0] qmod(input [5:0] q); reg [6:0] r; begin r = qpdm(q); qmod = r[2:0]; end endfunction

// ---------------------------------------------------------------- neighbour record
// what a macroblock leaves for the one below (row buffer, bottom row) or on its
// right (left record, right column)
typedef struct packed {
	logic [15:0]  sn;
	logic [7:0]   tag;
	logic [13:0]  ty;
	logic [5:0]   qp;
	logic [8:0]   cbp;
	logic         cpm;
	logic [15:0]  i4;     // 4 intra 4x4 modes, raw
	logic [7:0]   nz;     // luma 4, Cb 2, Cr 2
	logic [1:0]   dt;     // direct flags of two 8x8 blocks
	logic [23:0]  rf;     // [l * 12 + k * 6], k: 2 ref_index
	logic [255:0] mv;     // [l * 128 + k * 32], k: 4 vectors
	logic [111:0] mvd;    // [l * 56 + k * 14], k: 4 |mvd| x | y << 7
} nrec_t;

// ---------------------------------------------------------------- CABAC
reg         cab_v;
reg   [2:0] cab_op;
reg  [11:0] cab_arg;
wire        cab_ready;
wire        cab_in_valid, cab_in_ready;
wire        cab_ov;
wire  [1:0] cab_kind;
wire  [6:0] cab_pos;
wire [31:0] cab_val;
wire        cab_busy;

cab_dec cab (
	.clk(clk), .reset(reset),
	.cmd_valid(cab_v), .cmd_ready(cab_ready), .cmd_op(cab_op), .cmd_arg(cab_arg),
	.in_valid(cab_in_valid), .in_ready(cab_in_ready), .in_data(in_data),
	.out_valid(cab_ov), .out_kind(cab_kind), .out_pos(cab_pos), .out_val(cab_val), .busy(cab_busy)
);

// ---------------------------------------------------------------- output FIFO
reg  [31:0] of_mem[0:63];
reg   [5:0] of_wp, of_rp;
reg   [6:0] of_cnt;
reg         of_we;
reg  [31:0] of_wd;
assign out_valid = of_cnt != 7'd0;
assign out_data = of_mem[of_rp];
wire of_room = of_cnt < 7'd56;
always @(posedge clk) begin
	if (reset) begin
		of_wp <= 6'd0;
		of_rp <= 6'd0;
		of_cnt <= 7'd0;
	end
	else begin
		if (of_we) begin
			of_mem[of_wp] <= of_wd;
			of_wp <= of_wp + 6'd1;
		end
		if (out_valid && out_ready) of_rp <= of_rp + 6'd1;
		of_cnt <= of_cnt + {6'd0, of_we} - {6'd0, out_valid && out_ready};
	end
end

// ---------------------------------------------------------------- slice
reg   [7:0] st, rs0, rs1, rsd, rsf, pm_ret;
localparam S_DESC = 8'd0;
reg   [3:0] dpart;
reg   [6:0] dcnt;
reg   [4:0] iw0, iw1;
reg         data_on;
reg  [29:0] dleft;

reg   [7:0] s_slot, s_col, s_tag, s_mbw, s_mbh, s_x0, s_y0;
reg  [15:0] s_sn;
reg   [5:0] s_qp0, s_rc0, s_rc1;
reg   [1:0] s_type, s_idc, s_lc, s_dbk, s_uw;
reg         s_sp, s_t8, s_cip, s_d8, s_x264, s_l1l;
reg signed [4:0] s_cqo0, s_cqo1;
reg signed [7:0] s_alpha, s_beta;
wire        s_B = s_type == 2'd1, s_I = s_type == 2'd2;

// descriptor tables, in block RAM (one write port fed by the descriptor, one read port)
reg  [15:0] dsf[0:31];
reg   [7:0] mapc[0:63];
reg   [8:0] iw_ram[0:1023];
reg  [31:0] smw[0:63];
reg         t_we;                   // table write: t_sel 0 dsf, 1 mapc, 2 iw, 3 smw
reg   [1:0] t_sel;
reg   [9:0] t_wa;
reg  [31:0] t_wd;
reg   [4:0] ds_ra;
reg   [5:0] mp_ra;
wire  [9:0] iw_ra;
wire  [5:0] sm_rw;
reg  [15:0] ds_q;
reg   [7:0] mp_q;
reg   [8:0] iw_q;
reg  [31:0] sm_q;
always @(posedge clk) begin
	if (t_we && t_sel == 2'd0) dsf[t_wa[4:0]] <= t_wd[15:0];
	ds_q <= dsf[ds_ra];
end
always @(posedge clk) begin
	if (t_we && t_sel == 2'd1) mapc[t_wa[5:0]] <= t_wd[7:0];
	mp_q <= mapc[mp_ra];
end
always @(posedge clk) begin
	if (t_we && t_sel == 2'd2) iw_ram[t_wa] <= t_wd[8:0];
	iw_q <= iw_ram[iw_ra];
end
always @(posedge clk) begin
	if (t_we && t_sel == 2'd3) smw[t_wa[5:0]] <= t_wd;
	sm_q <= smw[sm_rw];
end
reg   [7:0] sm0[0:5];             // scaling_matrix4 [l][0]

assign cab_in_valid = data_on && in_valid && dleft != 30'd0 && st != S_DRAIN;
assign in_ready = st == S_DESC ? 1'b1 : data_on ? ((st == S_DRAIN || cab_in_ready) && dleft != 30'd0) : 1'b0;
assign busy = st != S_DESC || data_on;

function automatic [5:0] cqp(input logic t, input [5:0] q);
	reg signed [7:0] j;
	begin
		j = $signed({2'b00, q}) + (t ? s_cqo1 : s_cqo0);
		cqp = f_cqtab(j < 0 ? 6'd0 : j > 51 ? 6'd51 : j[5:0]);
	end
endfunction

// ---------------------------------------------------------------- macroblock state
reg   [7:0] mbx, mby;
reg   [5:0] qs, cqp0, cqp1;
reg         lqd_nz;
reg  [31:0] mbt;
reg   [2:0] pc;
reg   [5:0] cbp;
reg   [8:0] cbprec;
reg   [3:0] i16m, cpmc;
reg   [1:0] cpm;
reg  [15:0] sa_t, sa_l, sa_tl, sa_tr;
reg   [8:0] tcbp, lcbp;
reg   [1:0] nts;
reg         d8ok;
reg  [31:0] sub[0:3];
reg   [2:0] spc[0:3];
reg         bin;

reg  [31:0] t_ty, l_ty, tl_ty, tr_ty;
reg         t_sm, l_sm, dt_av, dl_av;
nrec_t      trec, lrec, tlr, trr;
reg  [15:0] i4snap_b, i4snap_r;   // modes before the check (written back)

// caches, FFmpeg's scan8 layout (8 per row) stored densely: rows 0..4,
// columns 3..8 (column 8 of row y is scan8 position 8 * (y + 1))
function automatic [4:0] sx(input [6:0] p);
	reg [2:0] x, y;
	x = p[2:0]; y = p[5:3];
	if (x == 3'd0) sx = 5'(6 * (y - 3'd1) + 5);
	else sx = 5'(6 * y + x - 3);
endfunction
function automatic [3:0] ix(input [6:0] p);
	ix = {2'(p[5:3] - 3'd1), p[1:0]};
endfunction
// interior (rows 1..4, columns 4..7): 16 places per list, index (row - 1) * 4 + column - 4
reg  signed [5:0] refc[0:1][0:15];
reg  [31:0] mvc[0:1][0:15];
reg  [13:0] mvdc[0:1][0:15];
// neighbours of the macroblock (row 0, column 3; column 8 of rows 1..4 is
// never available): what fill_decode_caches would copy, read from the records
function automatic signed [5:0] rfr(input logic l, input [6:0] p);
	reg signed [5:0] r;
	reg [1:0] k;
	reg [23:0] tf, lf;
	tf = trec.rf; lf = lrec.rf;
	k = p[5:3] >= 3'd3 ? 2'd1 : 2'd0;               // left: 8x8 row of the place
	if (p[2:0] == 3'd0) begin
		if (p == 7'd8) r = uses(tr_ty, l) ? $signed(l ? trr.rf[17:12] : trr.rf[5:0]) : (tr_ty != 0 ? -6'sd1 : -6'sd2);
		else r = -6'sd2;
	end
	else if (p == 7'd3) r = uses(tl_ty, l) ? $signed(l ? tlr.rf[23:18] : tlr.rf[11:6]) : (tl_ty != 0 ? -6'sd1 : -6'sd2);
	else if (p[5:3] == 3'd0) r = uses(t_ty, l) ? $signed(6'(tf >> {l, p[1]} * 6)) : (t_ty != 0 ? -6'sd1 : -6'sd2);
	else if (p[2:0] == 3'd3) r = uses(l_ty, l) ? $signed(6'(lf >> {l, k[0]} * 6)) : (l_ty != 0 ? -6'sd1 : -6'sd2);
	else r = refc[l][ix(p)];
	rfr = r;
endfunction
function automatic [31:0] mvr(input logic l, input [6:0] p);
	reg [31:0] v;
	reg [255:0] tm, lm;
	reg [2:0] y;
	tm = trec.mv; lm = lrec.mv;
	y = p[5:3] - 3'd1;
	if (p[2:0] == 3'd0) v = p == 7'd8 && uses(tr_ty, l) ? (l ? trr.mv[159:128] : trr.mv[31:0]) : 32'd0;
	else if (p == 7'd3) v = uses(tl_ty, l) ? (l ? tlr.mv[255:224] : tlr.mv[127:96]) : 32'd0;
	else if (p[5:3] == 3'd0) v = uses(t_ty, l) ? 32'(tm >> {l, p[1:0], 5'd0}) : 32'd0;
	else if (p[2:0] == 3'd3) v = uses(l_ty, l) ? 32'(lm >> {l, y[1:0], 5'd0}) : 32'd0;
	else v = mvc[l][ix(p)];
	mvr = v;
endfunction
function automatic [13:0] mdr(input logic l, input [6:0] p);
	reg [13:0] v;
	reg [111:0] tm, lm;
	reg [2:0] y;
	tm = trec.mvd; lm = lrec.mvd;
	y = p[5:3] - 3'd1;
	if (p[2:0] == 3'd0 || p == 7'd3) v = 14'd0;
	else if (p[5:3] == 3'd0) v = uses(t_ty, l) ? 14'(tm >> ({l, p[1:0]} * 14)) : 14'd0;
	else if (p[2:0] == 3'd3) v = uses(l_ty, l) ? 14'(lm >> ({l, y[1:0]} * 14)) : 14'd0;
	else v = mvdc[l][ix(p)];
	mdr = v;
endfunction
reg [119:0] nzc;
reg   [3:0] i4c[0:39];              // 15 = -1
reg  [39:0] dirc;
// cache write buses: every write of a clock to one list's cache carries the
// same value (or 0 / -2), so each cache entry only needs an enable
reg  [1:0][39:0] mvw_m, mvw_z, mdw_m, mdw_z, rfw_m, rfw_n;
// one shared read port of the vector, mvd and ref caches
reg   [6:0] cr_a;                   // {list, scan8 index}
wire [31:0] cv_q = mvr(cr_a[6], 7'(cr_a[5:0]));
wire [13:0] cd_q = mdr(cr_a[6], 7'(cr_a[5:0]));
wire signed [5:0] cf_q = rfr(cr_a[6], 7'(cr_a[5:0]));
// neighbours of the block being predicted, read one a clock (S_PMA..S_PMD)
reg  [31:0] pv_a, pv_b, pv_c, pv_d;
reg  signed [5:0] pr_a, pr_b, pr_c, pr_d;
reg  [13:0] pd_a, pd_b;
reg  [1:0][31:0] mvw_v;
reg  [1:0][13:0] mdw_v;
reg  [1:0][5:0]  rfw_v;

// row buffer
nrec_t      rb[0:MAXW-1];
nrec_t      rb_q;
reg   [7:0] rb_ra, rb_wa;
reg         rb_we;
nrec_t      rb_wd;
always @(posedge clk) begin
	if (rb_we) rb[rb_wa] <= rb_wd;
	rb_q <= rb[rb_ra];
end

// ---------------------------------------------------------------- coefficients
reg  [15:0] cram[0:383];
reg   [8:0] cr_wa;
reg  [15:0] cr_wd, cr_q;
reg         cr_we;
wire  [8:0] cr_ra;
wire        cr_re;
always @(posedge clk) begin
	if (cr_we) cram[cr_wa] <= cr_wd;
	if (cr_re) cr_q <= cram[cr_ra];
end
// non-zero coefficients, bit block * 16 + position: 16 bits a block in a small
// RAM (valid when rv), DC bits (position 0 of I16 luma and chroma DC) in dcm
(* ramstyle = "MLAB, no_rw_check" *) reg [15:0] bmr[0:23];
reg  [23:0] rv, dcm;
reg         bw_we;
reg   [4:0] bw_a;
reg  [15:0] bw_d;
wire  [4:0] br_a;
wire [15:0] br_q;
wire [15:0] br_m;
always @(posedge clk) if (bw_we) bmr[bw_a] <= bw_d;
assign br_q = bw_we && bw_a == br_a ? bw_d : bmr[br_a];
assign br_m = rv[br_a] ? br_q : 16'd0;
reg   [8:0] ncoef;
reg  [31:0] pcm[0:95];
reg  signed [15:0] ldc[0:15];
reg  signed [15:0] cdc[0:7];

// residual block being read
reg   [2:0] r_cat;
reg   [5:0] r_blk;                // FFmpeg's block index (scan8)
reg   [8:0] r_base;               // coefficient base (bit and RAM)
reg   [2:0] r_list;               // dequant list (4x4: 0..5, 8x8: 0/1)
reg   [5:0] r_qp;
reg         r_on;
reg   [6:0] r_cnt;

// dequantisation pipeline
reg         c0_v, c1_v, c2_v, c3_v;
reg  [21:0] c0_s, c1_s, c2_s;
reg   [5:0] c0_p, c1_p, c2_p;
reg   [5:0] c0_j, c1_j, c2_j;
reg   [8:0] c0_b, c1_b, c2_b;
reg         c0_8;
reg   [2:0] c0_m;
reg   [3:0] c0_d;
reg   [1:0] c0_by;
reg  [21:0] c1_q;
reg  [21:0] c2_pr;
reg   [4:0] sc_blk_r;
reg   [5:0] sc_p_r;
wire  [8:0] bm_ix = c2_b + {3'd0, c2_p};

// col picture (direct)
reg  [31:0] colty;
reg   [7:0] rrk[0:3];
reg  [15:0] sck[0:3];
reg   [1:0] tk;
reg  signed [7:0] colr0[0:3], colr1[0:3];
reg  [31:0] colm00, colm10;          // vectors 0 of the co-located lists
// co-located vectors {list, block}: 4 copies, one per read of a clock (S_SD2 reads 4)
(* ramstyle = "MLAB, no_rw_check" *) reg [31:0] colmA[0:31];
(* ramstyle = "MLAB, no_rw_check" *) reg [31:0] colmB[0:31];
(* ramstyle = "MLAB, no_rw_check" *) reg [31:0] colmC[0:31];
(* ramstyle = "MLAB, no_rw_check" *) reg [31:0] colmD[0:31];
reg         colm_we;
reg   [4:0] colm_wa;
reg  [31:0] colm_wd;
reg   [4:0] col_a[0:3];
wire [31:0] col_q[0:3];
always @(posedge clk) if (colm_we) begin
	colmA[colm_wa] <= colm_wd; colmB[colm_wa] <= colm_wd;
	colmC[colm_wa] <= colm_wd; colmD[colm_wa] <= colm_wd;
end
assign col_q[0] = colmA[col_a[0]];
assign col_q[1] = colmB[col_a[1]];
assign col_q[2] = colmC[col_a[2]];
assign col_q[3] = colmD[col_a[3]];
reg   [5:0] mc_i, mc_r;

// ---------------------------------------------------------------- combinational helpers

// pred_motion / 16x8 / 8x16 (FFmpeg h264_mvpred.h), for pm_n, pm_pw, pm_l, pm_ref, pm_mode
reg   [3:0] pm_n;
reg   [2:0] pm_pw;
reg         pm_l;
reg  signed [5:0] pm_ref;
reg   [1:0] pm_mode;                // 0 pred_motion, 1 16x8, 2 8x16
reg  signed [15:0] pm_mx, pm_my;
always @(*) begin
	reg signed [5:0] tr, lr, dr, trr_;
	reg [31:0] A, B, C, D;
	reg [1:0] mc;
	reg done;
	reg signed [5:0] r2;
	reg [31:0] C2;
	// A left (i8 - 1), B top (i8 - 8), C top right (i8 - 8 + width) or top left (i8 - 9)
	tr = pr_b;
	lr = pr_a;
	A = pv_a;
	B = pv_b;
	trr_ = pr_c;
	if (trr_ != -6'sd2) begin dr = trr_; C = pv_c; end
	else begin dr = pr_d; C = pv_d; end
	done = 1'b0;
	r2 = 6'sd0;
	C2 = 32'd0;
	D = 32'd0;
	mc = 2'd0;
	pm_mx = 16'sd0;
	pm_my = 16'sd0;
	// 16x8 and 8x16 shortcuts
	if (pm_mode == 2'd1) begin
		if (pm_n == 4'd0) begin
			if (tr == pm_ref) begin D = B; pm_mx = D[15:0]; pm_my = D[31:16]; done = 1'b1; end
		end
		else if (lr == pm_ref) begin D = A; pm_mx = D[15:0]; pm_my = D[31:16]; done = 1'b1; end
	end
	else if (pm_mode == 2'd2) begin
		if (pm_n == 4'd0) begin
			if (lr == pm_ref) begin D = A; pm_mx = D[15:0]; pm_my = D[31:16]; done = 1'b1; end
		end
		else begin
			// fetch_diagonal_mv for scan8[4], width 2
			r2 = dr; C2 = C;
			if (r2 == pm_ref) begin pm_mx = C2[15:0]; pm_my = C2[31:16]; done = 1'b1; end
		end
	end
	if (!done) begin
		mc = {1'b0, dr == pm_ref} + {1'b0, tr == pm_ref} + {1'b0, lr == pm_ref};
		if (mc > 2'd1) begin
			pm_mx = mid_pred(A[15:0], B[15:0], C[15:0]);
			pm_my = mid_pred(A[31:16], B[31:16], C[31:16]);
		end
		else if (mc == 2'd1) begin
			if (lr == pm_ref) begin pm_mx = A[15:0]; pm_my = A[31:16]; end
			else if (tr == pm_ref) begin pm_mx = B[15:0]; pm_my = B[31:16]; end
			else begin pm_mx = C[15:0]; pm_my = C[31:16]; end
		end
		else if (tr == -6'sd2 && dr == -6'sd2 && lr != -6'sd2) begin
			pm_mx = A[15:0]; pm_my = A[31:16];
		end
		else begin
			pm_mx = mid_pred(A[15:0], B[15:0], C[15:0]);
			pm_my = mid_pred(A[31:16], B[31:16], C[31:16]);
		end
	end
end

// pred_intra_mode for block im_k
reg   [3:0] im_k;
wire  [6:0] im_i8 = f_scan8({2'd0, im_k});
wire  [3:0] im_l = i4c[im_i8 - 7'd1], im_t = i4c[im_i8 - 7'd8];
wire  [3:0] im_pred = (im_l == 4'd15 || im_t == 4'd15) ? 4'd2 : (im_l < im_t ? im_l : im_t);

// check_intra_pred_mode: -1 (5'h1F) when not allowed
function automatic [4:0] chk_mode(input [3:0] mode, input logic chroma, input [15:0] st_, input [15:0] sl_);
	reg [4:0] m;
	begin
		m = {1'b0, mode};
		if (mode > 4'd3) m = 5'h1F;
		else begin
			if (!st_[15]) m = mode == 4'd0 ? 5'd4 : mode == 4'd1 ? 5'd1 : 5'h1F;
			if (m != 5'h1F && (sl_ & 16'h8080) != 16'h8080) begin
				case (m)
				5'd0: m = 5'd5; 5'd2: m = 5'd2; 5'd4: m = 5'd6; default: m = 5'h1F;
				endcase
				if (m != 5'h1F && chroma && (sl_ & 16'h8080) != 16'd0)
					m = 5'd7 + {4'd0, !sl_[15]} + (m == 5'd6 ? 5'd2 : 5'd0);
			end
		end
		chk_mode = m;
	end
endfunction

// mvd context: |mvd| of the left and top blocks of block mv_n, list mv_l
reg         mv_l;
reg   [3:0] mv_n;
wire  [6:0] mv_i8 = f_scan8({2'd0, mv_n});
wire [13:0] mv_a = pd_a, mv_b = pd_b;          // fetched with the vector neighbours (mv_n = pm_n)
wire  [7:0] amvd0 = {1'b0, mv_a[6:0]} + {1'b0, mv_b[6:0]};
wire  [7:0] amvd1 = {1'b0, mv_a[13:7]} + {1'b0, mv_b[13:7]};

// ref context
reg         rf_l;
reg   [3:0] rf_n;
wire  [6:0] rf_i8 = f_scan8({2'd0, rf_n});
wire signed [5:0] rf_a = rfr(rf_l, 7'(rf_i8 - 7'd1)), rf_b = rfr(rf_l, 7'(rf_i8 - 7'd8));
wire  [1:0] rf_ctx0 = s_B ? {rf_b > 0 && !dirc[rf_i8 - 7'd8], rf_a > 0 && !dirc[rf_i8 - 7'd1]}
                          : {rf_b > 0, rf_a > 0};

// coded_block_flag context of the block being read
wire  [6:0] r_i8 = f_scan8(r_blk);
wire        r_nza = r_cat == 3'd0 ? lcbp[8] : r_cat == 3'd3 ? lcbp[6 + r_blk - 6'd49] : nzc[r_i8 - 7'd1];
wire        r_nzb = r_cat == 3'd0 ? tcbp[8] : r_cat == 3'd3 ? tcbp[6 + r_blk - 6'd49] : nzc[r_i8 - 7'd8];
wire  [9:0] r_cbfbase = r_cat == 3'd0 ? 10'd85 : r_cat == 3'd1 ? 10'd89 : r_cat == 3'd2 ? 10'd93 :
                        r_cat == 3'd3 ? 10'd97 : 10'd101;

// first non-zero coefficient (output scan)
wire [23:0] grp = rv | dcm;
reg   [4:0] sc_g;
reg   [3:0] sc_b;
always @(*) begin
	sc_g = 5'd0;
	for (int g = 23; g >= 0; g--) if (grp[g]) sc_g = 5'(g);
end
always @(*) begin
	reg [15:0] w;
	w = br_m | {15'd0, dcm[sc_g]};
	sc_b = 4'd0;
	for (int b = 15; b >= 0; b--) if (w[b]) sc_b = 4'(b);
end

// ---------------------------------------------------------------- states
localparam
	S_INIT = 1, S_INITW = 2, S_SL0 = 3, S_MB0 = 4, S_MB1 = 5, S_MB2 = 6, S_MB3 = 7, S_MB4 = 8,
	S_SKR = 9, S_BIN = 10, S_SK0 = 11, S_SKE = 12, S_PSK = 13,
	S_BT0 = 14, S_BT1 = 15, S_BT2 = 16, S_BT3 = 17, S_BT4 = 18, S_BT5 = 19, S_BT6 = 20, S_BT7 = 21, S_BT8 = 22,
	S_PT1 = 23, S_PT2 = 24, S_PT3 = 25, S_PT4 = 26,
	S_IT0 = 27, S_IT1 = 28, S_IT2 = 29, S_IT3 = 30, S_IT4 = 31, S_IT5 = 32, S_IT6 = 33, S_IT7 = 34, S_ITE = 35,
	S_MTD = 36, S_FC0 = 37, S_FCL = 38, S_DISP = 39,
	S_IN0 = 40, S_IN1 = 41, S_IM0 = 42, S_IM1 = 43, S_IM2 = 44, S_IM3 = 45, S_IWB = 46, S_ICHK = 47,
	S_CP0 = 48, S_CP1 = 49, S_CP2 = 50, S_CP3 = 51,
	S_PCM0 = 52, S_PCM1 = 53,
	S_SB0 = 54, S_SB1 = 55, S_SB2 = 56, S_SB3 = 57, S_SB4 = 58, S_SB5 = 59, S_SB6 = 60, S_SB7 = 61, S_SBE = 62,
	S_P8D = 63, S_RF0 = 64, S_RF1 = 65, S_MV0 = 66, S_MV1 = 67, S_MV2 = 68, S_MV3 = 69,
	S_RD0 = 70, S_RD1 = 71, S_RD2 = 72,
	S_MD0 = 73, S_MD1 = 74, S_MD2 = 75, S_MD3 = 76, S_MD4 = 77, S_MD5 = 78, S_MD6 = 79,
	S_DIRE = 80, S_PR0 = 81, S_PR1 = 82, S_PV0 = 83, S_PV1 = 84, S_PV2 = 85,
	S_CB0 = 86, S_CB1 = 87, S_CB2 = 88, S_CB3 = 89, S_CB4 = 90, S_CB5 = 91, S_CB6 = 92,
	S_T8 = 93, S_QD0 = 94, S_QD1 = 95, S_QD2 = 96, S_RS = 97, S_RC0 = 98, S_RC1 = 99, S_RW = 100,
	S_DC0 = 101, S_DC1 = 102, S_DC2 = 103, S_DC3 = 104,
	S_O0 = 105, S_O1 = 106, S_O2 = 107, S_O3 = 108, S_O4 = 109, S_O5 = 110, S_O6 = 111,
	S_E0 = 112, S_E1 = 113, S_E2 = 114, S_E3 = 115, S_DRAIN = 116, S_ERR = 117,
	S_SD0 = 118, S_SD1 = 119, S_SD2 = 120, S_TD1 = 121, S_TD2 = 122, S_TD3 = 123,
	S_CF0 = 124, S_CF1 = 125, S_DR0 = 126, S_MV4 = 127, S_RF2 = 128, S_PSK1 = 129,
	S_PTE = 130, S_IMA = 131, S_SB8 = 132, S_SB9 = 133, S_SP1 = 134, S_SP2 = 135, S_SP3 = 136,
	S_P8E = 137, S_RF3 = 138, S_PV3 = 139, S_T8B = 140, S_ERW = 141, S_PRN = 142,
	S_TP0 = 143, S_TP1 = 144, S_TP2 = 145, S_TP3 = 146, S_TP4 = 147,
	S_PMA = 148, S_PMB = 149, S_PMC = 150, S_PMD = 151;

// ---------------------------------------------------------------- tasks (cache writes)
task automatic dec(input [9:0] c, input [7:0] nx);
	cab_v <= 1'b1; cab_op <= 3'd1; cab_arg <= {2'd0, c}; rs0 <= nx; st <= S_BIN;
endtask
task automatic byp(input [7:0] nx);
	cab_v <= 1'b1; cab_op <= 3'd2; rs0 <= nx; st <= S_BIN;
endtask
task automatic trm(input [7:0] nx);
	cab_v <= 1'b1; cab_op <= 3'd3; rs0 <= nx; st <= S_BIN;
endtask
function automatic [39:0] rmask(input [6:0] s, input [2:0] w, input [2:0] h);
	reg [39:0] m;
	m = 40'd0;
	for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
		if (x < w && y < h) m[6'(s + 7'(x + 8 * y))] = 1'b1;
	rmask = m;
endfunction
task automatic fref(input logic l, input [6:0] s, input [2:0] w, input [2:0] h, input [5:0] v);
	rfw_v[l] = v; rfw_m[l] = rfw_m[l] | rmask(s, w, h);
endtask
task automatic fmv(input logic l, input [6:0] s, input [2:0] w, input [2:0] h, input [31:0] v);
	mvw_v[l] = v; mvw_m[l] = mvw_m[l] | rmask(s, w, h);
endtask
task automatic fmvz(input logic l, input [6:0] s, input [2:0] w, input [2:0] h);
	mvw_z[l] = mvw_z[l] | rmask(s, w, h);
endtask
task automatic fmvd(input logic l, input [6:0] s, input [2:0] w, input [2:0] h, input [13:0] v);
	mdw_v[l] = v; mdw_m[l] = mdw_m[l] | rmask(s, w, h);
endtask
task automatic fmvdz(input logic l, input [6:0] s, input [2:0] w, input [2:0] h);
	mdw_z[l] = mdw_z[l] | rmask(s, w, h);
endtask

// fill_decode_caches, motion part, one list
task automatic fill_list(input logic l);
	// the neighbours themselves are read from the records (nb_*)
	if ((mbt & (MT_SKIP | MT_DIR)) == 0) begin
		rfw_n[l][14] = 1'b1; rfw_n[l][30] = 1'b1;
		mvw_z[l][14] = 1'b1; mvw_z[l][30] = 1'b1;
		mdw_z[l][14] = 1'b1; mdw_z[l][30] = 1'b1;
		if (s_B) begin
			for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) dirc[12 + x + 8 * y] <= 1'b0;
			if ((t_ty & MT_DIR) != 0) dirc[7:4] <= 4'hF;
			else if ((t_ty & MT_88) != 0) begin dirc[4] <= trec.dt[0]; dirc[6] <= trec.dt[1]; end
			else dirc[7:4] <= 4'h0;
			dirc[11] <= (l_ty & MT_DIR) != 0 ? 1'b1 : (l_ty & MT_88) != 0 ? lrec.dt[0] : 1'b0;
			dirc[27] <= (l_ty & MT_DIR) != 0 ? 1'b1 : (l_ty & MT_88) != 0 ? lrec.dt[1] : 1'b0;
		end
	end
endtask

// ---------------------------------------------------------------- main FSM
reg   [4:0] mtidx;
reg   [3:0] bits_;
reg   [9:0] it_base;
reg         it_is;
reg   [3:0] imk, imstep, imm, impred;
reg   [1:0] cnt2;
reg   [1:0] sbk;
reg   [4:0] sbt;
reg         ll;                     // list loop
reg   [1:0] ii;                     // partition loop
reg   [1:0] jj;
reg  [31:0] refv;
reg   [5:0] rfv;
reg   [2:0] rfc;
reg  [31:0] mdv;                    // |mvd|
reg   [9:0] mdc;
reg   [4:0] mdk;
reg         mdy;                    // decoding y
reg  signed [31:0] mdx_r, mdy_r;    // decoded mvd x, y
reg   [6:0] mdax;                   // clipped |mvd| x
reg  signed [31:0] mx_acc, my_acc;
reg   [6:0] qdv;
reg   [1:0] qdc;
reg   [4:0] rq;
reg   [4:0] dck;
reg  signed [17:0] tmp[0:15];
reg  [23:0] qmul_dc;
reg   [5:0] ok;                     // output counter
reg         sc_pend;
reg   [3:0] sc_blk;
reg   [5:0] sc_p;
reg   [6:0] pcm_i;
reg   [8:0] pcm_b;
reg  [31:0] pcm_w;
reg   [5:0] mr_k;
reg   [1:0] mr_sub;
reg  [31:0] mr_base;
reg   [2:0] dr_i8;
reg   [1:0] dr_i4;
reg   [4:0] dr_n;
reg   [2:0] dr_m;
reg  [31:0] dr_sub;
reg  signed [5:0] dr_ref[0:1];
reg  [31:0] dr_mv[0:1];
reg         dr_b8;
reg  signed [15:0] dr_scale;
reg  signed [7:0] dr_r0;
reg         eos;

wire [31:0] mb_lin = {16'd0, mby} * {24'd0, s_mbw} + {24'd0, mbx};
wire        cur_pcm = (mbt & MT_PCM) != 0;
wire  [5:0] qrec = cur_pcm ? 6'd0 : qs;

always @(posedge clk) begin
	mvw_m = '0; mvw_z = '0; mvw_v = '0;
	mdw_m = '0; mdw_z = '0; mdw_v = '0;
	rfw_m = '0; rfw_n = '0; rfw_v = '0;
	of_we <= 1'b0;
	bw_we <= 1'b0;
	t_we <= 1'b0;
	cr_we <= 1'b0;
	colm_we <= 1'b0;
	rb_we <= 1'b0;
	sl_done <= 1'b0;
	sl_err <= 1'b0;
	if (data_on && in_valid && in_ready) dleft <= dleft - 30'd1;
	if (reset) begin
		st <= S_DESC;
		dpart <= 4'd0;
		data_on <= 1'b0;
		cab_v <= 1'b0;
		mem_req <= 1'b0;
		rv <= 24'd0;
		dcm <= 24'd0;
		ncoef <= 9'd0;
		r_on <= 1'b0;
		mdy <= 1'b0;
		c0_v <= 1'b0; c1_v <= 1'b0; c2_v <= 1'b0; c3_v <= 1'b0;
		for (int k = 0; k < 16; k++) ldc[k] <= 16'sd0;
		for (int k = 0; k < 8; k++) cdc[k] <= 16'sd0;
	end
	else begin
		case (st)
		// ------------------------------------------------ descriptor
		S_DESC: if (in_valid) begin
			case (dpart)
			4'd0: begin
				s_slot <= in_data[23:16];
				s_col <= in_data[15:8];
				s_tag <= in_data[7:0];
				dpart <= in_data[31:24] == 8'h5C ? 4'd1 : 4'd0;
			end
			4'd1: begin
				s_mbw <= in_data[7:0];
				s_mbh <= in_data[15:8];
				s_x0 <= in_data[23:16];
				s_y0 <= in_data[31:24];
				dpart <= 4'd2;
			end
			4'd2: begin
				s_sn <= in_data[15:0];
				s_qp0 <= in_data[21:16];
				s_type <= in_data[23:22];
				s_idc <= in_data[25:24];
				s_lc <= in_data[27:26];
				s_dbk <= in_data[29:28];
				s_uw <= in_data[31:30];
				dpart <= 4'd3;
			end
			4'd3: begin
				s_rc0 <= in_data[5:0];
				s_rc1 <= in_data[11:6];
				s_sp <= in_data[12];
				s_t8 <= in_data[13];
				s_cip <= in_data[14];
				s_d8 <= in_data[15];
				s_x264 <= in_data[16];
				s_l1l <= in_data[17];
				s_cqo0 <= in_data[22:18];
				s_cqo1 <= in_data[27:23];
				dpart <= 4'd4;
			end
			4'd4: begin
				s_alpha <= in_data[7:0];
				s_beta <= in_data[15:8];
				dcnt <= 7'd0;
				iw0 <= 5'd0;
				iw1 <= 5'd0;
				dpart <= (s_B && !s_sp) ? 4'd5 : s_uw == 2'd2 ? 4'd7 : 4'd8;
			end
			4'd5: begin
				t_we <= 1'b1; t_sel <= 2'd0; t_wa <= {5'd0, dcnt[4:0]}; t_wd <= in_data;
				dcnt <= dcnt[4:0] == 5'd31 ? 7'd0 : dcnt + 7'd1;
				if (dcnt[4:0] == 5'd31) dpart <= 4'd6;
			end
			4'd6: begin
				t_we <= 1'b1; t_sel <= 2'd1; t_wa <= {4'd0, dcnt[5:0]}; t_wd <= in_data;
				dcnt <= dcnt[5:0] == 6'd63 ? 7'd0 : dcnt + 7'd1;
				if (dcnt[5:0] == 6'd63) dpart <= s_uw == 2'd2 ? 4'd7 : 4'd8;
			end
			4'd7: begin
				t_we <= 1'b1; t_sel <= 2'd2; t_wa <= {iw0, iw1}; t_wd <= in_data;
				if ({1'b0, iw1} == s_rc1 - 6'd1) begin
					iw1 <= 5'd0;
					iw0 <= iw0 + 5'd1;
					if ({1'b0, iw0} == s_rc0 - 6'd1) dpart <= 4'd8;
				end
				else iw1 <= iw1 + 5'd1;
			end
			4'd8: begin
				t_we <= 1'b1; t_sel <= 2'd3; t_wa <= {4'd0, dcnt[5:0]}; t_wd <= in_data;
				if (dcnt[1:0] == 2'd0) sm0[dcnt[4:2]] <= in_data[7:0];
				dcnt <= dcnt == 7'd23 ? 7'd0 : dcnt + 7'd1;
				if (dcnt == 7'd23) dpart <= s_t8 ? 4'd9 : 4'd10;
			end
			4'd9: begin
				t_we <= 1'b1; t_sel <= 2'd3; t_wa <= {4'd0, 6'd24 + dcnt[5:0]}; t_wd <= in_data;
				dcnt <= dcnt + 7'd1;
				if (dcnt == 7'd31) dpart <= 4'd10;
			end
			default: begin
				dleft <= 30'((in_data + 32'd3) >> 2);
				dpart <= 4'd0;
				st <= S_INIT;
			end
			endcase
		end

		S_INIT: begin
			cab_v <= 1'b1;
			cab_op <= 3'd0;
			cab_arg <= {2'd0, s_qp0, s_idc, s_type};
			st <= S_INITW;
		end
		S_INITW: if (cab_ready) begin
			cab_v <= 1'b0;
			data_on <= 1'b1;
			st <= S_SL0;
		end
		S_SL0: begin
			mbx <= s_x0;
			mby <= s_y0;
			qs <= s_qp0;
			cqp0 <= cqp(1'b0, s_qp0);
			cqp1 <= cqp(1'b1, s_qp0);
			lqd_nz <= 1'b0;
			for (int l = 0; l < 2; l++) begin
				rfw_n[l][16] = 1'b1; rfw_n[l][24] = 1'b1; rfw_n[l][32] = 1'b1;
			end
			st <= S_MB0;
		end

		// ------------------------------------------------ macroblock start: neighbours
		S_MB0: begin rb_ra <= mbx; st <= S_MB1; end
		S_MB1: begin rb_ra <= mbx + 8'd1; st <= S_MB2; end
		S_MB2: begin tlr <= trec; trec <= rb_q; st <= S_MB3; end
		S_MB3: begin trr <= rb_q; st <= S_MB4; end
		S_MB4: begin
			reg ti, li, trv, tli, tsm, lsm, trsm, tlsm;
			ti = mby != 8'd0;
			li = mbx != 8'd0;
			trv = ti && mbx + 8'd1 < s_mbw;
			tli = ti && li;
			tsm = ti && trec.sn == s_sn && trec.tag == s_tag;
			lsm = li && lrec.sn == s_sn && lrec.tag == s_tag;
			trsm = trv && trr.sn == s_sn && trr.tag == s_tag;
			tlsm = tli && tlr.sn == s_sn && tlr.tag == s_tag;
			t_sm <= tsm;
			l_sm <= lsm;
			t_ty <= tsm ? tunpack(trec.ty) : 32'd0;
			l_ty <= lsm ? tunpack(lrec.ty) : 32'd0;
			tr_ty <= trsm ? tunpack(trr.ty) : 32'd0;
			tl_ty <= tlsm ? tunpack(tlr.ty) : 32'd0;
			dt_av <= s_dbk == 2'd2 ? tsm : (ti && trec.tag == s_tag);
			dl_av <= s_dbk == 2'd2 ? lsm : (li && lrec.tag == s_tag);
			if (!s_I)
				dec(10'd11 + {9'd0, lsm && !lrec.ty[8]} + {9'd0, tsm && !trec.ty[8]} + (s_B ? 10'd13 : 10'd0), S_SKR);
			else begin
				it_base <= 10'd3;
				it_is <= 1'b1;
				st <= S_IT0;
			end
		end

		S_BIN: begin
			if (cab_ready) cab_v <= 1'b0;
			if (cab_ov) begin
				bin <= cab_val[0];
				st <= rs0;
			end
		end

		S_SKR: if (bin) st <= S_SK0;
		else if (s_B) begin
			reg [31:0] lm, tm;
			lm = l_ty - 32'd1;
			tm = t_ty - 32'd1;
			dec(10'd27 + {9'd0, !lm[8]} + {9'd0, !tm[8]}, S_BT1);
		end
		else dec(10'd14, S_PT1);

		// ------------------------------------------------ skip
		S_SK0: begin
			if (s_B) begin
				mbt <= MT_L0L1 | MT_DIR | MT_SKIP;
				rsd <= S_SKE;
				if (s_sp) begin
					ll <= 1'b0;
					rsf <= S_DR0;
					st <= S_FCL;
				end
				else st <= S_DR0;
			end
			else begin
				mbt <= MT_16 | MT_P0L0 | MT_P1L0 | MT_SKIP;
				st <= S_PSK;
			end
		end
		S_PSK: begin
			// pred_pskip_motion (h264_mvpred.h)
			reg signed [5:0] lr, tr, dr;
			reg [31:0] A, B, C;
			reg z;
			reg [1:0] mc;
			reg signed [15:0] mx, my;
			z = 1'b0;
			lr = 6'sd0; tr = 6'sd0; dr = 6'sd0;
			A = 32'd0; B = 32'd0; C = 32'd0;
			fref(1'b0, 7'd12, 3'd4, 3'd4, 6'd0);
			if (uses(l_ty, 1'b0)) begin
				lr = lrec.rf[5:0];
				A = lrec.mv[31:0];
				if (lr == 6'sd0 && A == 32'd0) z = 1'b1;
			end
			else if (l_ty != 0) begin lr = -6'sd1; A = 32'd0; end
			else z = 1'b1;
			if (uses(t_ty, 1'b0)) begin
				tr = trec.rf[5:0];
				B = trec.mv[31:0];
				if (tr == 6'sd0 && B == 32'd0) z = 1'b1;
			end
			else if (t_ty != 0) begin tr = -6'sd1; B = 32'd0; end
			else z = 1'b1;
			if (uses(tr_ty, 1'b0)) begin dr = trr.rf[5:0]; C = trr.mv[31:0]; end
			else if (tr_ty != 0) begin dr = -6'sd1; C = 32'd0; end
			else if (uses(tl_ty, 1'b0)) begin dr = tlr.rf[11:6]; C = tlr.mv[127:96]; end
			else if (tl_ty != 0) begin dr = -6'sd1; C = 32'd0; end
			else begin dr = -6'sd2; C = 32'd0; end
			mc = {1'b0, dr == 6'sd0} + {1'b0, tr == 6'sd0} + {1'b0, lr == 6'sd0};
			if (mc == 2'd1) begin
				if (lr == 6'sd0) begin mx = A[15:0]; my = A[31:16]; end
				else if (tr == 6'sd0) begin mx = B[15:0]; my = B[31:16]; end
				else begin mx = C[15:0]; my = C[31:16]; end
			end
			else begin
				mx = mid_pred(A[15:0], B[15:0], C[15:0]);
				my = mid_pred(A[31:16], B[31:16], C[31:16]);
			end
			fmv(1'b0, 7'd12, 3'd4, 3'd4, z ? 32'd0 : {my, mx});
			st <= S_SKE;
		end
		S_SKE: begin
			mbt <= mbt | MT_SKIP;
			cbprec <= 9'd0;
			cpm <= 2'd0;
			lqd_nz <= 1'b0;
			st <= S_O0;
		end

		// ------------------------------------------------ mb_type, B
		S_BT1: if (!bin) begin mtidx <= 5'd0; st <= S_BT8; end
		else dec(10'd30, S_BT2);
		S_BT2: if (!bin) dec(10'd32, S_BT3); else dec(10'd31, S_BT4);
		S_BT3: begin mtidx <= 5'd1 + {4'd0, bin}; st <= S_BT8; end
		S_BT4: begin bits_ <= {bin, 3'd0}; dec(10'd32, S_BT5); end
		S_BT5: begin bits_[2] <= bin; dec(10'd32, S_BT6); end
		S_BT6: begin bits_[1] <= bin; dec(10'd32, S_BT7); end
		S_BT7: begin
			reg [3:0] b;
			b = {bits_[3:1], bin};
			if (b < 4'd8) begin mtidx <= {1'b0, b} + 5'd3; st <= S_BT8; end
			else if (b == 4'd13) begin it_base <= 10'd32; it_is <= 1'b0; st <= S_IT0; end
			else if (b == 4'd14) begin mtidx <= 5'd11; st <= S_BT8; end
			else if (b == 4'd15) begin mtidx <= 5'd22; st <= S_BT8; end
			else begin bits_ <= b; dec(10'd32, S_BT0); end
		end
		S_BT0: begin mtidx <= {bits_, bin} - 5'd4; st <= S_BT8; end
		S_BT8: begin
			reg [18:0] v;
			v = f_bmb(mtidx);
			mbt <= {16'd0, v[15:0]};
			pc <= v[18:16];
			st <= S_MTD;
		end

		// ------------------------------------------------ mb_type, P
		S_PT1: if (bin) begin it_base <= 10'd17; it_is <= 1'b0; st <= S_IT0; end
		else dec(10'd15, S_PT2);
		S_PT2: if (bin) dec(10'd17, S_PT4); else dec(10'd16, S_PT3);
		S_PT3: begin mtidx <= bin ? 5'd3 : 5'd0; st <= S_PTE; end
		S_PT4: begin mtidx <= bin ? 5'd1 : 5'd2; st <= S_PTE; end
		S_PTE: begin
			reg [18:0] v;
			v = f_pmb(mtidx[1:0]);
			mbt <= {16'd0, v[15:0]};
			pc <= v[18:16];
			st <= S_MTD;
		end

		// ------------------------------------------------ mb_type, intra (decode_cabac_intra_mb_type)
		S_IT0: if (it_is) dec(it_base + {9'd0, ((l_ty & (MT_I16 | MT_PCM)) != 0)} + {9'd0, ((t_ty & (MT_I16 | MT_PCM)) != 0)}, S_IT1);
		else dec(it_base, S_IT1);
		S_IT1: if (!bin) begin mtidx <= 5'd0; st <= S_ITE; end
		else begin
			if (it_is) it_base <= it_base + 10'd2;
			trm(S_IT2);
		end
		S_IT2: if (bin) begin mtidx <= 5'd25; st <= S_ITE; end
		else dec(it_base + 10'd1, S_IT3);
		S_IT3: begin mtidx <= bin ? 5'd13 : 5'd1; dec(it_base + 10'd2, S_IT4); end
		S_IT4: if (bin) dec(it_base + 10'd2 + {9'd0, it_is}, S_IT5);
		else dec(it_base + 10'd3 + {9'd0, it_is}, S_IT6);
		S_IT5: begin mtidx <= mtidx + 5'd4 + (bin ? 5'd4 : 5'd0); dec(it_base + 10'd3 + {9'd0, it_is}, S_IT6); end
		S_IT6: begin mtidx <= mtidx + (bin ? 5'd2 : 5'd0); dec(it_base + 10'd3 + {8'd0, it_is, 1'b0}, S_IT7); end
		S_IT7: begin mtidx <= mtidx + {4'd0, bin}; st <= S_ITE; end
		S_ITE: begin
			reg [21:0] v;
			v = f_imb(mtidx);
			mbt <= {29'd0, v[2:0]};
			i16m <= v[6:3];
			cbp <= v[12:7];
			pc <= 3'd0;
			st <= S_MTD;
		end

		S_MTD: if ((mbt & MT_PCM) != 0) begin
			cab_v <= 1'b1;
			cab_op <= 3'd5;
			pcm_b <= 9'd0;
			st <= S_PCM0;
		end
		else st <= S_FC0;

		// ------------------------------------------------ I_PCM
		S_PCM0: begin
			if (cab_ready) cab_v <= 1'b0;
			if (cab_ov && cab_pos[0]) begin
				pcm_w <= {cab_val[7:0], pcm_w[31:8]};
				pcm_b <= pcm_b + 9'd1;
				if (pcm_b[1:0] == 2'd3) pcm[pcm_b[8:2]] <= {cab_val[7:0], pcm_w[31:8]};
				if (pcm_b == 9'd383) st <= S_PCM1;
			end
		end
		S_PCM1: begin
			cbprec <= 9'h1EF;
			cpm <= 2'd0;
			lqd_nz <= 1'b0;
			st <= S_O0;
		end

		// ------------------------------------------------ fill_decode_caches (not skip)
		S_FC0: begin
			reg [31:0] tm;
			reg [15:0] a_t, a_l, a_tl, a_tr;
			reg in_;
			tm = s_cip ? 32'd7 : 32'hFFFFFFFF;
			in_ = intra(mbt);
			if (in_) begin
				a_tl = 16'hFFFF; a_t = 16'hFFFF; a_l = 16'hFFFF; a_tr = 16'hEEEA;
				if ((t_ty & tm) == 0) begin a_tl = 16'hB3FF; a_t = 16'h33FF; a_tr = 16'h26EA; end
				if ((l_ty & tm) == 0) begin a_tl = a_tl & 16'hDF5F; a_l = a_l & 16'h5F5F; end
				if ((tl_ty & tm) == 0) a_tl = a_tl & 16'h7FFF;
				if ((tr_ty & tm) == 0) a_tr = a_tr & 16'hFBFF;
				sa_t <= a_t; sa_l <= a_l; sa_tl <= a_tl; sa_tr <= a_tr;
				if ((mbt & MT_I4) != 0) begin
					for (int k = 0; k < 4; k++)
						i4c[4 + k] <= (t_ty & MT_I4) != 0 ? trec.i4[4 * k +: 4] : (t_ty & tm) != 0 ? 4'd2 : 4'd15;
					for (int k = 0; k < 4; k++)
						i4c[11 + 8 * k] <= (l_ty & MT_I4) != 0 ? lrec.i4[4 * k +: 4] : (l_ty & tm) != 0 ? 4'd2 : 4'd15;
				end
			end
			if (t_ty != 0) begin
				for (int k = 0; k < 4; k++) nzc[4 + k] <= trec.nz[k];
				nzc[44] <= trec.nz[4]; nzc[45] <= trec.nz[5];
				nzc[84] <= trec.nz[6]; nzc[85] <= trec.nz[7];
			end
			else begin
				for (int k = 0; k < 4; k++) nzc[4 + k] <= in_;
				nzc[44] <= in_; nzc[45] <= in_; nzc[84] <= in_; nzc[85] <= in_;
			end
			if (l_ty != 0) begin
				for (int k = 0; k < 4; k++) nzc[11 + 8 * k] <= lrec.nz[k];
				nzc[51] <= lrec.nz[4]; nzc[59] <= lrec.nz[5];
				nzc[91] <= lrec.nz[6]; nzc[99] <= lrec.nz[7];
			end
			else begin
				for (int k = 0; k < 4; k++) nzc[11 + 8 * k] <= in_;
				nzc[51] <= in_; nzc[59] <= in_; nzc[91] <= in_; nzc[99] <= in_;
			end
			tcbp <= t_ty != 0 ? trec.cbp : in_ ? 9'h1CF : 9'h00F;
			lcbp <= l_ty != 0 ? ((lrec.cbp & 9'h1F0) | (lrec.cbp & 9'h00A)) : in_ ? 9'h1CF : 9'h00F;
			nts <= {1'b0, t_ty[24]} + {1'b0, l_ty[24]};
			d8ok <= s_t8;
			ll <= 1'b0;
			rsf <= S_DISP;
			st <= S_FCL;
		end
		S_FCL: begin
			if ({1'b0, ll} < s_lc && (inter(mbt) || ((mbt & MT_DIR) != 0 && s_sp)) && uses(mbt, ll))
				fill_list(ll);
			if (ll) st <= rsf;
			ll <= 1'b1;
		end
		S_DISP: begin
			if (intra(mbt)) st <= S_IN0;
			else if (pc == 3'd4) begin
				sbk <= 2'd0;
				st <= s_B ? S_SB0 : S_SB4;
			end
			else if ((mbt & MT_DIR) != 0) begin
				rsd <= S_DIRE;
				st <= S_DR0;
			end
			else begin
				ll <= 1'b0;
				ii <= 2'd0;
				st <= S_PR0;
			end
		end

		// ------------------------------------------------ intra modes
		S_IN0: if ((mbt & MT_I4) != 0) begin
			if (s_t8) dec(10'd399 + {8'd0, nts}, S_IN1);
			else begin imk <= 4'd0; imstep <= 4'd1; st <= S_IM0; end
		end
		else begin
			reg [4:0] m;
			m = chk_mode(i16m, 1'b0, sa_t, sa_l);
			if (m == 5'h1F) st <= S_ERR;
			else begin i16m <= m[3:0]; st <= S_CP0; end
		end
		S_IN1: begin
			if (bin) begin mbt <= mbt | MT_DCT; imstep <= 4'd4; end
			else imstep <= 4'd1;
			imk <= 4'd0;
			st <= S_IM0;
		end
		S_IM0: begin
			im_k <= imk;
			st <= S_IMA;
		end
		S_IMA: begin
			impred <= im_pred;
			dec(10'd68, S_IM1);
		end
		S_IM1: if (bin) begin imm <= impred; st <= S_IM3; end
		else begin imm <= 4'd0; cnt2 <= 2'd0; dec(10'd69, S_IM2); end
		S_IM2: begin
			reg [3:0] m;
			m = imm | ({3'd0, bin} << cnt2);
			if (cnt2 == 2'd2) begin
				imm <= m + (m >= impred ? 4'd1 : 4'd0);
				st <= S_IM3;
			end
			else begin
				imm <= m;
				cnt2 <= cnt2 + 2'd1;
				dec(10'd69, S_IM2);
			end
		end
		S_IM3: begin
			reg [6:0] s8;
			s8 = f_scan8({2'd0, imk});
			i4c[s8] <= imm;
			if (imstep == 4'd4) begin i4c[s8 + 7'd1] <= imm; i4c[s8 + 7'd8] <= imm; i4c[s8 + 7'd9] <= imm; end
			if (imk + imstep == 5'd16 || imk + imstep < imk) st <= S_IWB;
			else begin imk <= imk + imstep; st <= S_IM0; end
		end
		S_IWB: begin
			i4snap_b <= {i4c[39], i4c[38], i4c[37], i4c[36]};
			i4snap_r <= {i4c[39], i4c[31], i4c[23], i4c[15]};
			st <= S_ICHK;
		end
		S_ICHK: begin
			// check_intra4x4_pred_mode
			reg [3:0] m[0:7];
			reg bad;
			reg [3:0] s;
			bad = 1'b0;
			for (int i = 0; i < 4; i++) m[i] = i4c[12 + i];
			for (int i = 0; i < 4; i++) m[4 + i] = i4c[12 + 8 * i];
			if (!sa_t[15])
				for (int i = 0; i < 4; i++) begin
					case (m[i])
					4'd0, 4'd3, 4'd4, 4'd5, 4'd6, 4'd7: bad = 1'b1;
					4'd2: m[i] = 4'd9;
					default: ;
					endcase
				end
			m[4] = m[0];                  // the same cell (scan8[0])
			if ((sa_l & 16'h8888) != 16'h8888)
				for (int i = 0; i < 4; i++) begin
					reg [15:0] mk;
					mk = i == 0 ? 16'h8000 : i == 1 ? 16'h2000 : i == 2 ? 16'h0080 : 16'h0020;
					if ((sa_l & mk) == 0)
						case (m[4 + i])
						4'd1, 4'd4, 4'd5, 4'd6, 4'd8: bad = 1'b1;
						4'd2: m[4 + i] = 4'd10;
						4'd9: m[4 + i] = 4'd11;
						default: ;
						endcase
				end
			for (int i = 1; i < 4; i++) i4c[12 + i] <= m[i];
			i4c[12] <= m[4];
			for (int i = 1; i < 4; i++) i4c[12 + 8 * i] <= m[4 + i];
			st <= bad ? S_ERR : S_CP0;
		end
		// chroma prediction mode
		S_CP0: dec(10'd64 + {9'd0, l_ty != 0 && lrec.cpm} + {9'd0, t_ty != 0 && trec.cpm}, S_CP1);
		S_CP1: if (!bin) begin cpm <= 2'd0; st <= S_CP3; end
		else begin cpm <= 2'd1; dec(10'd67, S_CP2); end
		S_CP2: if (!bin) st <= S_CP3;
		else if (cpm == 2'd1) begin cpm <= 2'd2; dec(10'd67, S_CP2); end
		else begin cpm <= 2'd3; st <= S_CP3; end
		S_CP3: begin
			reg [4:0] m;
			m = chk_mode({2'd0, cpm}, 1'b1, sa_t, sa_l);
			if (m == 5'h1F) st <= S_ERR;
			else begin cpmc <= m[3:0]; st <= (mbt & MT_I16) != 0 ? S_T8 : S_CB0; end
		end

		// ------------------------------------------------ 8x8 partitions
		// sub_mb_type, B
		S_SB0: dec(10'd36, S_SB1);
		S_SB1: if (!bin) begin sbt <= 5'd0; st <= S_SBE; end
		else dec(10'd37, S_SB2);
		S_SB2: if (!bin) dec(10'd39, S_SB3);
		else begin sbt <= 5'd3; dec(10'd38, S_SB5); end
		S_SB3: begin sbt <= 5'd1 + {4'd0, bin}; st <= S_SBE; end
		S_SB5: if (bin) dec(10'd39, S_SB6);
		else dec(10'd39, S_SB7);
		S_SB6: if (bin) dec(10'd39, S_SB8);
		else begin sbt <= 5'd7; dec(10'd39, S_SB7); end
		S_SB8: begin sbt <= 5'd11 + {4'd0, bin}; st <= S_SBE; end
		S_SB7: begin sbt <= sbt + (bin ? 5'd2 : 5'd0); dec(10'd39, S_SB9); end
		S_SB9: begin sbt <= sbt + {4'd0, bin}; st <= S_SBE; end
		// sub_mb_type, P
		S_SB4: dec(10'd21, S_SP1);
		S_SP1: if (bin) begin sbt <= 5'd0; st <= S_SBE; end
		else dec(10'd22, S_SP2);
		S_SP2: if (!bin) begin sbt <= 5'd1; st <= S_SBE; end
		else dec(10'd23, S_SP3);
		S_SP3: begin sbt <= bin ? 5'd2 : 5'd3; st <= S_SBE; end
		S_SBE: begin
			reg [18:0] v;
			v = s_B ? f_bsub(sbt[3:0]) : f_psub(sbt[1:0]);
			sub[sbk] <= {16'd0, v[15:0]};
			spc[sbk] <= v[18:16];
			if (sbk == 2'd3) st <= S_P8D;
			else begin
				sbk <= sbk + 2'd1;
				st <= s_B ? S_SB0 : S_SB4;
			end
		end
		S_P8D: begin
			ll <= 1'b0;
			ii <= 2'd0;
			if (s_B && ((sub[0] | sub[1] | sub[2] | sub[3]) & MT_DIR) != 0) begin
				rsd <= S_P8E;
				st <= S_DR0;
			end
			else st <= S_RF0;
		end
		S_P8E: begin
			for (int l = 0; l < 2; l++) begin
				rfw_n[l][14] = 1'b1; rfw_n[l][30] = 1'b1;
			end
			for (int i = 0; i < 4; i++) begin
				reg [6:0] s8;
				s8 = f_scan8(6'(4 * i));
				dirc[s8] <= sub[i][8]; dirc[s8 + 1] <= sub[i][8];
				dirc[s8 + 8] <= sub[i][8]; dirc[s8 + 9] <= sub[i][8];
			end
			st <= S_RF0;
		end
		// references of the 8x8 partitions
		S_RF0: begin
			if ((sub[ii] & MT_DIR) != 0) st <= S_RF2;
			else if (isdir(sub[ii], 1'b0, ll)) begin
				if ((ll ? s_rc1 : s_rc0) > 6'd1) begin
					rf_l <= ll;
					rf_n <= {ii, 2'b00};
					rs1 <= S_RF1;
					st <= S_RD0;
				end
				else begin rfv <= 6'd0; st <= S_RF1; end
			end
			else begin rfv <= 6'h3F; st <= S_RF1; end
		end
		S_RF1: begin
			reg [6:0] s8;
			s8 = f_scan8({2'd0, ii, 2'b00});
			fref(ll, s8 + 7'd1, 3'd1, 3'd1, rfv);
			fref(ll, s8 + 7'd8, 3'd2, 3'd1, rfv);
			st <= S_RF2;
		end
		S_RF2: begin
			ii <= ii + 2'd1;
			if (ii != 2'd3) st <= S_RF0;
			else if (ll || s_lc == 2'd1) st <= S_RF3;
			else begin ll <= 1'b1; st <= S_RF0; end
		end
		S_RF3: begin
			// get_dct8x8_allowed
			reg [31:0] m;
			m = MT_168 | MT_816 | MT_88 | (s_d8 ? 32'd0 : MT_DIR);
			if (((sub[0] | sub[1] | sub[2] | sub[3]) & m) != 0) d8ok <= 1'b0;
			ll <= 1'b0;
			ii <= 2'd0;
			st <= S_MV0;
		end
		// vectors of the 8x8 partitions
		S_MV0: begin
			reg [6:0] s8;
			s8 = f_scan8({2'd0, ii, 2'b00});
			fref(ll, s8, 3'd1, 3'd1, rfr(ll, 7'(s8 + 1)));
			if ((sub[ii] & MT_DIR) != 0) begin
				fmvdz(ll, s8, 3'd2, 3'd2);
				st <= S_MV4;
			end
			else if (isdir(sub[ii], 1'b0, ll)) begin
				jj <= 2'd0;
				st <= S_MV1;
			end
			else begin
				fmvz(ll, s8, 3'd2, 3'd2);
				fmvdz(ll, s8, 3'd2, 3'd2);
				st <= S_MV4;
			end
		end
		S_MV1: begin
			// index = 4 i + block_width j
			reg [3:0] idx;
			reg [2:0] bw;
			bw = (sub[ii] & (MT_16 | MT_168)) != 0 ? 3'd2 : 3'd1;
			idx = {ii, 2'b00} + (bw == 3'd2 ? {1'b0, jj, 1'b0} : {2'b00, jj});
			pm_n <= idx;
			pm_pw <= bw;
			pm_l <= ll;
			pm_mode <= 2'd0;
			pm_ref <= rfr(ll, 7'(f_scan8({2'd0, idx})));
			mv_l <= ll;
			mv_n <= idx;
			pm_ret <= S_MV2;
			st <= S_PMA;
		end
		// neighbours of block pm_n, list pm_l: left, top, top right, top left
		S_PMA: begin pv_a <= cv_q; pr_a <= cf_q; pd_a <= cd_q; st <= S_PMB; end
		S_PMB: begin pv_b <= cv_q; pr_b <= cf_q; pd_b <= cd_q; st <= S_PMC; end
		S_PMC: begin pv_c <= cv_q; pr_c <= cf_q; st <= S_PMD; end
		S_PMD: begin pv_d <= cv_q; pr_d <= cf_q; st <= pm_ret; end
		S_MV2: begin
			mx_acc <= pm_mx;
			my_acc <= pm_my;
			rs1 <= S_MV3;
			st <= S_MD0;
		end
		S_MV3: begin
			reg [6:0] s8;
			reg [31:0] v;
			reg [13:0] d;
			s8 = f_scan8({2'd0, mv_n});
			v = {16'(my_acc), 16'(mx_acc)};
			d = {mdy_r[6:0], mdax};
			if ((sub[ii] & MT_16) != 0) begin
				fmv(ll, s8, 3'd2, 3'd2, v);
				fmvd(ll, s8, 3'd2, 3'd2, d);
			end
			else if ((sub[ii] & MT_168) != 0) begin
				fmv(ll, s8, 3'd2, 3'd1, v);
				fmvd(ll, s8, 3'd2, 3'd1, d);
			end
			else if ((sub[ii] & MT_816) != 0) begin
				fmv(ll, s8, 3'd1, 3'd2, v);
				fmvd(ll, s8, 3'd1, 3'd2, d);
			end
			else begin
				fmv(ll, s8, 3'd1, 3'd1, v);
				fmvd(ll, s8, 3'd1, 3'd1, d);
			end
			if ({1'b0, jj} + 3'd1 >= spc[ii]) st <= S_MV4;
			else begin jj <= jj + 2'd1; st <= S_MV1; end
		end
		S_MV4: begin
			if (ii == 2'd3) begin
				ii <= 2'd0;
				if (ll || s_lc == 2'd1) st <= S_CB0;
				else begin ll <= 1'b1; st <= S_MV0; end
			end
			else begin ii <= ii + 2'd1; st <= S_MV0; end
		end

		// ------------------------------------------------ B_Direct_16x16
		S_DIRE: begin
			fmvdz(1'b0, 7'd12, 3'd4, 3'd4);
			fmvdz(1'b1, 7'd12, 3'd4, 3'd4);
			d8ok <= d8ok & s_d8;
			st <= S_CB0;
		end

		// ------------------------------------------------ 16x16, 16x8, 8x16: references
		S_PR0: begin
			if (isdir(mbt, ii[0], ll)) begin
				if ((ll ? s_rc1 : s_rc0) > 6'd1) begin
					rf_l <= ll;
					rf_n <= (mbt & MT_168) != 0 ? {ii[0], 3'b000} : {1'b0, ii[0], 2'b00};
					rs1 <= S_PR1;
					st <= S_RD0;
				end
				else begin rfv <= 6'd0; st <= S_PR1; end
			end
			else if ((mbt & MT_16) != 0) st <= S_PRN;
			else begin rfv <= 6'h3F; st <= S_PR1; end
		end
		S_PR1: begin
			if ((mbt & MT_16) != 0) fref(ll, 7'd12, 3'd4, 3'd4, rfv);
			else if ((mbt & MT_168) != 0) fref(ll, 7'd12 + (ii[0] ? 7'd16 : 7'd0), 3'd4, 3'd2, rfv);
			else fref(ll, 7'd12 + (ii[0] ? 7'd2 : 7'd0), 3'd2, 3'd4, rfv);
			st <= S_PRN;
		end
		S_PRN: begin
			if ((mbt & MT_16) != 0 || ii[0]) begin
				ii <= 2'd0;
				if (ll || s_lc == 2'd1) begin ll <= 1'b0; st <= S_PV0; end
				else begin ll <= 1'b1; st <= S_PR0; end
			end
			else begin ii <= 2'd1; st <= S_PR0; end
		end
		// vectors
		S_PV0: begin
			if (isdir(mbt, ii[0], ll)) begin
				reg [3:0] n;
				n = (mbt & MT_16) != 0 ? 4'd0 : (mbt & MT_168) != 0 ? {ii[0], 3'b000} : {1'b0, ii[0], 2'b00};
				pm_n <= n;
				pm_pw <= (mbt & MT_816) != 0 ? 3'd2 : 3'd4;
				pm_l <= ll;
				pm_mode <= (mbt & MT_16) != 0 ? 2'd0 : (mbt & MT_168) != 0 ? 2'd1 : 2'd2;
				pm_ref <= (mbt & MT_816) != 0 ? rfr(ll, 7'(12 + (ii[0] ? 2 : 0))) : rfr(ll, 7'(12 + (ii[0] ? 16 : 0)));
				mv_l <= ll;
				mv_n <= n;
				pm_ret <= S_PV1;
				st <= S_PMA;
			end
			else begin
				if ((mbt & MT_168) != 0) begin
					fmvdz(ll, 7'd12 + (ii[0] ? 7'd16 : 7'd0), 3'd4, 3'd2);
					fmvz(ll, 7'd12 + (ii[0] ? 7'd16 : 7'd0), 3'd4, 3'd2);
				end
				else if ((mbt & MT_816) != 0) begin
					fmvdz(ll, 7'd12 + (ii[0] ? 7'd2 : 7'd0), 3'd2, 3'd4);
					fmvz(ll, 7'd12 + (ii[0] ? 7'd2 : 7'd0), 3'd2, 3'd4);
				end
				st <= S_PV2;
			end
		end
		S_PV1: begin
			mx_acc <= pm_mx;
			my_acc <= pm_my;
			rs1 <= S_PV3;
			st <= S_MD0;
		end
		S_PV3: begin
			reg [31:0] v;
			reg [13:0] d;
			v = {16'(my_acc), 16'(mx_acc)};
			d = {mdy_r[6:0], mdax};
			if ((mbt & MT_16) != 0) begin
				fmvd(ll, 7'd12, 3'd4, 3'd4, d);
				fmv(ll, 7'd12, 3'd4, 3'd4, v);
			end
			else if ((mbt & MT_168) != 0) begin
				fmvd(ll, 7'd12 + (ii[0] ? 7'd16 : 7'd0), 3'd4, 3'd2, d);
				fmv(ll, 7'd12 + (ii[0] ? 7'd16 : 7'd0), 3'd4, 3'd2, v);
			end
			else begin
				fmvd(ll, 7'd12 + (ii[0] ? 7'd2 : 7'd0), 3'd2, 3'd4, d);
				fmv(ll, 7'd12 + (ii[0] ? 7'd2 : 7'd0), 3'd2, 3'd4, v);
			end
			st <= S_PV2;
		end
		S_PV2: begin
			if ((mbt & MT_16) != 0 || ii[0]) begin
				ii <= 2'd0;
				if (ll || s_lc == 2'd1) st <= S_CB0;
				else begin ll <= 1'b1; st <= S_PV0; end
			end
			else begin ii <= 2'd1; st <= S_PV0; end
		end

		// ------------------------------------------------ decode_cabac_mb_ref (rf_l, rf_n) -> rfv
		S_RD0: begin
			rfv <= 6'd0;
			rfc <= {1'b0, rf_ctx0};
			dec(10'd54 + {8'd0, rf_ctx0}, S_RD1);
		end
		S_RD1: if (bin) begin
			if (rfv == 6'd31) st <= S_ERR;
			else begin
				rfv <= rfv + 6'd1;
				rfc <= (rfc >> 2) + 3'd4;
				dec(10'd54 + {7'd0, (rfc >> 2) + 3'd4}, S_RD1);
			end
		end
		else if (rfv >= (rf_l ? s_rc1 : s_rc0)) st <= S_ERR;
		else st <= rs1;

		// ------------------------------------------------ mvd (mv_l, mv_n): mx_acc, my_acc += mvd
		S_MD0: begin
			reg [7:0] a;
			a = mdy ? amvd1 : amvd0;
			mdc <= mdy ? 10'd47 : 10'd40;
			dec((mdy ? 10'd47 : 10'd40) + {9'd0, a > 8'd2} + {9'd0, a > 8'd32}, S_MD1);
		end
		S_MD1: if (!bin) begin
			if (!mdy) begin mdx_r <= 32'sd0; mdax <= 7'd0; mdy <= 1'b1; st <= S_MD0; end
			else begin mdy_r <= 32'sd0; mdy <= 1'b0; st <= S_MD6; end
		end
		else begin
			mdv <= 32'd1;
			mdc <= mdc + 10'd3;
			dec(mdc + 10'd3, S_MD2);
		end
		S_MD2: if (bin && mdv < 32'd8) begin
			// still unary (mvd < 9 after this one)
			mdv <= mdv + 32'd1;
			if (mdv < 32'd4) begin mdc <= mdc + 10'd1; dec(mdc + 10'd1, S_MD2); end
			else dec(mdc, S_MD2);
		end
		else if (bin) begin
			mdv <= 32'd9;
			mdk <= 5'd3;
			byp(S_MD3);
		end
		else byp(S_MD5);
		S_MD3: if (bin) begin
			mdv <= mdv + (32'd1 << mdk);
			if (mdk == 5'd24) st <= S_ERR;
			else begin mdk <= mdk + 5'd1; byp(S_MD3); end
		end
		else begin
			mdk <= mdk - 5'd1;
			byp(S_MD4);
		end
		S_MD4: begin
			reg [31:0] v;
			v = mdv + ({31'd0, bin} << mdk);
			mdv <= v;
			if (mdk == 5'd0) byp(S_MD5);
			else begin mdk <= mdk - 5'd1; byp(S_MD4); end
		end
		S_MD5: begin
			// sign; |mvd| for the context, clipped at 70
			reg signed [31:0] s;
			reg [6:0] a;
			s = bin ? -$signed(mdv) : $signed(mdv);
			a = mdv < 32'd70 ? mdv[6:0] : 7'd70;
			if (!mdy) begin mdx_r <= s; mdax <= a; mdy <= 1'b1; st <= S_MD0; end
			else begin mdy_r <= {25'd0, a}; my_acc <= my_acc + s; mdy <= 1'b0; st <= S_MD6; end
		end
		S_MD6: begin
			mx_acc <= mx_acc + mdx_r;
			st <= rs1;
		end

		// ------------------------------------------------ cbp
		S_CB0: begin
			if (inter(mbt)) cpm <= 2'd0;
			if ((mbt & MT_I16) != 0) st <= S_T8;
			else dec(10'd73 + {9'd0, !lcbp[1]} + {8'd0, !tcbp[2], 1'b0}, S_CB1);
		end
		S_CB1: begin cbp <= {5'd0, bin}; dec(10'd73 + {9'd0, !bin} + {8'd0, !tcbp[3], 1'b0}, S_CB2); end
		S_CB2: begin cbp[1] <= bin; dec(10'd73 + {9'd0, !lcbp[3]} + {8'd0, !cbp[0], 1'b0}, S_CB3); end
		S_CB3: begin cbp[2] <= bin; dec(10'd73 + {9'd0, !bin} + {8'd0, !cbp[1], 1'b0}, S_CB4); end
		S_CB4: begin
			cbp[3] <= bin;
			dec(10'd77 + {9'd0, lcbp[5:4] != 2'd0} + {8'd0, tcbp[5:4] != 2'd0, 1'b0}, S_CB5);
		end
		S_CB5: if (!bin) begin cbp[5:4] <= 2'd0; st <= S_T8; end
		else dec(10'd81 + {9'd0, lcbp[5:4] == 2'd2} + {8'd0, tcbp[5:4] == 2'd2, 1'b0}, S_CB6);
		S_CB6: begin cbp[5:4] <= bin ? 2'd2 : 2'd1; st <= S_T8; end

		S_T8: begin
			cbprec <= {3'd0, cbp};
			if (d8ok && cbp[3:0] != 4'd0 && !intra(mbt)) dec(10'd399 + {8'd0, nts}, S_T8B);
			else st <= S_QD0;
		end
		S_T8B: begin
			if (bin) mbt <= mbt | MT_DCT;
			st <= S_QD0;
		end
		S_QD0: if (cbp != 6'd0 || (mbt & MT_I16) != 0) dec(10'd60 + {9'd0, lqd_nz}, S_QD1);
		else begin
			for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) begin
				nzc[12 + x + 8 * y] <= 1'b0;
				nzc[52 + x + 8 * y] <= 1'b0;
				nzc[92 + x + 8 * y] <= 1'b0;
			end
			lqd_nz <= 1'b0;
			st <= S_O0;
		end
		S_QD1: if (!bin) begin lqd_nz <= 1'b0; rq <= 5'd0; st <= S_RS; end
		else begin qdv <= 7'd1; dec(10'd62, S_QD2); end
		S_QD2: if (bin) begin
			if (qdv == 7'd102) st <= S_ERR;
			else begin qdv <= qdv + 7'd1; dec(10'd63, S_QD2); end
		end
		else begin
			reg signed [7:0] v, q;
			v = qdv[0] ? $signed({1'b0, qdv} + 8'd1) >>> 1 : -($signed({1'b0, qdv} + 8'd1) >>> 1);
			q = $signed({2'b00, qs}) + v;
			if (q < 0) q = q + 8'sd52;
			else if (q > 51) q = q - 8'sd52;
			qs <= q[5:0];
			cqp0 <= cqp(1'b0, q[5:0]);
			cqp1 <= cqp(1'b1, q[5:0]);
			lqd_nz <= 1'b1;
			rq <= 5'd0;
			st <= S_RS;
		end

		// ------------------------------------------------ residual sequence
		S_RS: begin
			reg go;
			go = 1'b0;
			if (rq == 5'd0) begin
				if ((mbt & MT_I16) != 0) begin
					for (int k = 0; k < 16; k++) ldc[k] <= 16'sd0;
					r_cat <= 3'd0; r_blk <= 6'd48; go = 1'b1;
				end
			end
			else if (rq <= 5'd16) begin
				reg [3:0] b;
				b = 4'(rq - 5'd1);
				if ((mbt & MT_I16) != 0) begin
					if (cbp[3:0] != 4'd0) begin
						r_cat <= 3'd1; r_blk <= {2'd0, b}; r_base <= {1'b0, b, 4'd0};
						r_list <= 3'd0; r_qp <= qs; go = 1'b1;
					end
					else if (b == 4'd0)
						for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) nzc[12 + x + 8 * y] <= 1'b0;
				end
				else if (cbp[b[3:2]]) begin
					if ((mbt & MT_DCT) != 0) begin
						if (b[1:0] == 2'd0) begin
							r_cat <= 3'd5; r_blk <= {2'd0, b}; r_base <= {1'b0, b[3:2], 6'd0};
							r_list <= intra(mbt) ? 3'd0 : 3'd1; r_qp <= qs; go = 1'b1;
						end
					end
					else begin
						r_cat <= 3'd2; r_blk <= {2'd0, b}; r_base <= {1'b0, b, 4'd0};
						r_list <= intra(mbt) ? 3'd0 : 3'd3; r_qp <= qs; go = 1'b1;
					end
				end
				else if (b[1:0] == 2'd0) begin
					reg [6:0] s8;
					s8 = f_scan8({2'd0, b});
					nzc[s8] <= 1'b0; nzc[s8 + 1] <= 1'b0; nzc[s8 + 8] <= 1'b0; nzc[s8 + 9] <= 1'b0;
				end
			end
			else if (rq <= 5'd18) begin
				if (cbp[5:4] != 2'd0) begin
					r_cat <= 3'd3; r_blk <= rq == 5'd17 ? 6'd49 : 6'd50; go = 1'b1;
				end
			end
			else if (rq <= 5'd26) begin
				reg [2:0] k;
				k = 3'(rq - 5'd19);
				if (cbp[5]) begin
					r_cat <= 3'd4;
					r_blk <= 6'd16 + {1'b0, k[2], 4'd0} + {4'd0, k[1:0]};
					r_base <= 9'd256 + {4'd0, k, 4'd0};
					r_list <= 3'd1 + {2'd0, k[2]} + (intra(mbt) ? 3'd0 : 3'd3);
					r_qp <= k[2] ? cqp1 : cqp0;
					go = 1'b1;
				end
				else if (rq == 5'd19)
					for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) begin
						nzc[52 + x + 8 * y] <= 1'b0;
						nzc[92 + x + 8 * y] <= 1'b0;
					end
			end
			else st <= S_O0;
			if (rq <= 5'd26) begin
				rq <= rq + 5'd1;
				if (go) st <= S_RC0;
			end
		end
		// one residual block: coded_block_flag, then the block in cab_dec
		S_RC0: if (r_cat == 3'd5) begin
			cab_v <= 1'b1; cab_op <= 3'd4; cab_arg <= {9'd0, r_cat};
			r_on <= 1'b1;
			st <= S_RW;
		end
		else dec(r_cbfbase + {9'd0, r_nza} + {8'd0, r_nzb, 1'b0}, S_RC1);
		S_RC1: if (!bin) begin
			nzc[r_i8] <= 1'b0;
			st <= S_RS;
		end
		else begin
			cab_v <= 1'b1; cab_op <= 3'd4; cab_arg <= {9'd0, r_cat};
			r_on <= 1'b1;
			st <= S_RW;
		end
		S_RW: begin
			if (cab_ready) cab_v <= 1'b0;
			if (cab_ov && cab_kind == 2'd2) begin
				reg nz;
				nz = cab_pos != 7'd0;
				r_on <= 1'b0;
				if (r_cat == 3'd5) begin
					nzc[r_i8] <= nz; nzc[r_i8 + 1] <= nz; nzc[r_i8 + 8] <= nz; nzc[r_i8 + 9] <= nz;
				end
				else nzc[r_i8] <= nz;
				if (r_cat == 3'd0) cbprec[8] <= 1'b1;
				if (r_cat == 3'd3) cbprec[6 + r_blk - 6'd49] <= 1'b1;
				st <= S_RS;
			end
		end

		// ------------------------------------------------ mcacc: DC transforms
		S_O0: if (!(c0_v || c1_v || c2_v || c3_v)) begin
			dck <= 5'd0;
			ok <= 6'd0;
			if ((mbt & MT_I16) != 0 && nzc[0]) begin
				qmul_dc <= 24'({19'd0, d4init(qmod(qs), 2'd0)} * {16'd0, sm0[0]} << (qdiv(qs) + 4'd2));
				st <= S_DC1;
			end
			else st <= S_DC2;
		end
		S_DC1: begin
			// luma DC, first pass: row dck a clock
			reg signed [17:0] z0, z1, z2, z3;
			reg [1:0] i;
			i = dck[1:0];
			z0 = 18'(ldc[{i, 2'd0}]) + 18'(ldc[{i, 2'd1}]);
			z1 = 18'(ldc[{i, 2'd0}]) - 18'(ldc[{i, 2'd1}]);
			z2 = 18'(ldc[{i, 2'd2}]) - 18'(ldc[{i, 2'd3}]);
			z3 = 18'(ldc[{i, 2'd2}]) + 18'(ldc[{i, 2'd3}]);
			tmp[{i, 2'd0}] <= z0 + z3;
			tmp[{i, 2'd1}] <= z0 - z3;
			tmp[{i, 2'd2}] <= z1 - z2;
			tmp[{i, 2'd3}] <= z1 + z2;
			dck <= i == 2'd3 ? 5'd0 : dck + 5'd1;
			if (i == 2'd3) st <= S_DC0;
		end
		S_DC0: begin
			// output dck: column i = dck >> 2, row r = dck & 3 of {0, 1, 4, 5}
			reg [1:0] i, r;
			reg [31:0] z0, z1, z2, z3, z, pr;
			reg [3:0] blk;
			reg [15:0] v;
			i = dck[3:2];
			r = dck[1:0];
			z0 = tmp[i] + tmp[8 + i];
			z1 = tmp[i] - tmp[8 + i];
			z2 = tmp[4 + i] - tmp[12 + i];
			z3 = tmp[4 + i] + tmp[12 + i];
			z = r == 2'd0 ? z0 + z3 : r == 2'd1 ? z1 + z2 : r == 2'd2 ? z1 - z2 : z0 - z3;
			pr = z * {8'd0, qmul_dc} + 32'd128;
			v = pr[23:8];
			blk = (i == 2'd0 ? 4'd0 : i == 2'd1 ? 4'd2 : i == 2'd2 ? 4'd8 : 4'd10) +
			      (r == 2'd0 ? 4'd0 : r == 2'd1 ? 4'd1 : r == 2'd2 ? 4'd4 : 4'd5);
			cr_we <= 1'b1;
			cr_wa <= {1'b0, blk, 4'd0};
			cr_wd <= v;
			if (v != 16'd0) begin
				dcm[{1'b0, blk}] <= 1'b1;
				ncoef <= ncoef + 9'd1;
			end
			dck <= dck == 5'd15 ? 5'd0 : dck + 5'd1;
			if (dck == 5'd15) st <= S_DC2;
		end
		// chroma DC, dck >> 2 = component
		S_DC2: begin
			if (cbp[5:4] != 2'd0 && (mbt & (MT_SKIP | MT_PCM)) == 0 && nzc[dck[2] ? 80 : 40]) begin
				reg [5:0] q;
				reg [2:0] l;
				q = dck[2] ? cqp1 : cqp0;
				l = (intra(mbt) ? 3'd1 : 3'd4) + {2'd0, dck[2]};
				qmul_dc <= 24'({19'd0, d4init(qmod(q), 2'd0)} * {16'd0, sm0[l]} << (qdiv(q) + 4'd2));
				st <= S_DC3;
			end
			else if (dck[2]) st <= S_O1;
			else dck <= 5'd4;
		end
		S_DC3: begin
			reg [31:0] a, b, c, d, e, z, pr;
			reg [15:0] v;
			reg [2:0] cb;
			a = 32'(cdc[{dck[2], 2'd0}]);
			b = 32'(cdc[{dck[2], 2'd1}]);
			c = 32'(cdc[{dck[2], 2'd2}]);
			d = 32'(cdc[{dck[2], 2'd3}]);
			e = a - b; a = a + b; b = c - d; c = c + d;
			z = dck[1:0] == 2'd0 ? a + c : dck[1:0] == 2'd1 ? e + b : dck[1:0] == 2'd2 ? a - c : e - b;
			pr = z * {8'd0, qmul_dc};
			v = pr[22:7];
			cb = {dck[2], dck[1:0]};
			cr_we <= 1'b1;
			cr_wa <= 9'd256 + {2'd0, cb, 4'd0};
			cr_wd <= v;
			if (v != 16'd0) begin
				dcm[5'd16 + {2'd0, cb}] <= 1'b1;
				ncoef <= ncoef + 9'd1;
			end
			if (dck[1:0] == 2'd3) begin
				if (dck[2]) st <= S_O1;
				else begin dck <= 5'd4; st <= S_DC2; end
			end
			else dck <= dck + 5'd1;
		end

		// ------------------------------------------------ mcacc: output
		S_O1: if (of_room) begin
			// A5 (or A0 A0 A0), word ok
			if (s_dbk == 2'd0) begin ok <= 6'd0; st <= S_O2; end
			else begin
				reg [5:0] q, nl, nt;
				reg [6:0] qy0, qy1, qb0, qb1, qr0, qr1;
				reg bad;
				bad = s_alpha < -8'sd16 || s_alpha > 8'sd15 || s_beta < -8'sd16 || s_beta > 8'sd15;
				q = qrec;
				nl = dl_av ? lrec.qp : q;
				nt = dt_av ? trec.qp : q;
				qy0 = ({1'b0, q} + {1'b0, nl} + 7'd1) >> 1;
				qy1 = ({1'b0, q} + {1'b0, nt} + 7'd1) >> 1;
				qb0 = ({1'b0, cqp(1'b0, q)} + {1'b0, cqp(1'b0, nl)} + 7'd1) >> 1;
				qb1 = ({1'b0, cqp(1'b0, q)} + {1'b0, cqp(1'b0, nt)} + 7'd1) >> 1;
				qr0 = ({1'b0, cqp(1'b1, q)} + {1'b0, cqp(1'b1, nl)} + 7'd1) >> 1;
				qr1 = ({1'b0, cqp(1'b1, q)} + {1'b0, cqp(1'b1, nt)} + 7'd1) >> 1;
				of_we <= 1'b1;
				if (bad) of_wd <= 32'hA0000000;
				else case (ok)
				6'd0: of_wd <= {8'hA5, 12'd0, s_beta[4:0], s_alpha[4:0], dt_av, dl_av};
				6'd1: of_wd <= {2'd0, qb1[5:0], qb0[5:0], q, qy1[5:0], qy0[5:0]};
				default: of_wd <= {8'd0, cqp(1'b1, q), qr1[5:0], qr0[5:0], cqp(1'b0, q)};
				endcase
				ok <= ok == 6'd2 ? 6'd0 : ok + 6'd1;
				if (ok == 6'd2) st <= S_O2;
			end
		end
		S_O2: if (of_room) begin
			reg [2:0] ty;
			ty = cur_pcm ? 3'd4 : (mbt & MT_I16) != 0 ? 3'd3 : (mbt & MT_I4) != 0 ? ((mbt & MT_DCT) != 0 ? 3'd2 : 3'd1) : 3'd0;
			of_we <= 1'b1;
			of_wd <= {8'hA3, 5'd0, ty, mby, mbx};
			ok <= 6'd0;
			pcm_i <= 7'd0;
			st <= cur_pcm ? S_O6 : S_O3;
		end
		S_O3: if (of_room) begin
			// head: mask, then 4 intra words or 36 inter words
			reg [23:0] mask;
			reg [31:0] w;
			mask = {grp[23:16], (mbt & MT_DCT) != 0 ? {3'd0, |grp[15:12], 3'd0, |grp[11:8], 3'd0, |grp[7:4], 3'd0, |grp[3:0]} : grp[15:0]};
			if (ok == 6'd0) w = {7'd0, (mbt & MT_DCT) != 0, mask};
			else if (intra(mbt)) begin
				case (ok)
				6'd1: w = (mbt & MT_I16) != 0 ? 32'd0 : {i4c[23], i4c[22], i4c[15], i4c[14], i4c[21], i4c[20], i4c[13], i4c[12]};
				6'd2: w = (mbt & MT_I16) != 0 ? 32'd0 : {i4c[39], i4c[38], i4c[31], i4c[30], i4c[37], i4c[36], i4c[29], i4c[28]};
				6'd3: w = {sa_tl, sa_tr};
				default: w = {24'd0, (mbt & MT_I16) != 0 ? i16m : 4'd0, cpmc};
				endcase
			end
			else if (ok <= 6'd4) begin
				reg [6:0] s8;
				reg signed [5:0] r0, r1;
				reg [8:0] wt;
				s8 = f_scan8({2'd0, ok[1:0] - 2'd1, 2'b00});
				r0 = uses(mbt, 1'b0) ? rfr(0, 7'(s8)) : -6'sd1;
				r1 = s_lc > 2'd1 && uses(mbt, 1'b1) ? rfr(1, 7'(s8)) : -6'sd1;
				wt = iw_q;
				w = {7'd0, (r0 >= 0 && r1 >= 0 && s_uw == 2'd2) ? wt : 9'd32, 2'd0, r1 < 0, r1[5:0], r0 < 0, r0[5:0]};
			end
			else begin
				reg [4:0] k;
				reg l;
				k = 5'(ok - 6'd5);
				l = k[4];
				w = ({1'b0, l} < s_lc && uses(mbt, l)) ? cv_q : 32'd0;
			end
			of_we <= 1'b1;
			of_wd <= w;
			if (ok == (intra(mbt) ? 6'd4 : 6'd36)) st <= S_O4;
			ok <= ok + 6'd1;
		end
		S_O4: if (of_room) begin
			of_we <= 1'b1;
			of_wd <= {23'd0, ncoef};
			sc_pend <= 1'b0;
			st <= S_O5;
		end
		// coefficients: the first set bit, its RAM word a clock later
		S_O5: if (of_room) begin
			if (sc_pend) begin
				of_we <= 1'b1;
				of_wd <= {5'd0, sc_blk_r, sc_p_r, cr_q};
			end
			if (grp != 24'd0) begin
				reg [4:0] blk;
				reg [5:0] p;
				reg [15:0] nw;
				nw = br_m & ~(16'd1 << sc_b);
				bw_we <= 1'b1;
				bw_a <= sc_g;
				bw_d <= nw;
				rv[sc_g] <= nw != 16'd0;
				if (sc_b == 4'd0) dcm[sc_g] <= 1'b0;
				if (sc_g < 5'd16 && (mbt & MT_DCT) != 0) begin
					blk = {1'b0, sc_g[3:2], 2'b00};
					p = {sc_g[1:0], sc_b};
				end
				else begin
					blk = sc_g;
					p = {2'd0, sc_b};
				end
				sc_blk_r <= blk;
				sc_p_r <= p;
				sc_pend <= 1'b1;
			end
			else begin
				sc_pend <= 1'b0;
				ncoef <= 9'd0;
				st <= S_E0;
			end
		end
		S_O6: if (of_room) begin
			// I_PCM samples
			of_we <= 1'b1;
			of_wd <= pcm[pcm_i];
			pcm_i <= pcm_i + 7'd1;
			if (pcm_i == 7'd95) st <= S_E0;
		end

		// ------------------------------------------------ end of the macroblock
		S_E0: begin
			nrec_t t, l;
			reg sk;
			sk = (mbt & MT_SKIP) != 0;
			t.sn = s_sn; t.tag = s_tag; t.ty = tpack(mbt); t.qp = qrec; t.cbp = cbprec;
			t.cpm = cpm != 2'd0 && intra(mbt) && !cur_pcm;
			t.i4 = i4snap_b;
			t.nz = sk ? 8'd0 : cur_pcm ? 8'hFF : {nzc[101], nzc[100], nzc[61], nzc[60], nzc[39], nzc[38], nzc[37], nzc[36]};
			t.dt = {sub[3][8], sub[2][8]};
			for (int ls = 0; ls < 2; ls++) begin
				t.rf[ls * 12 +: 6] = rfr(ls, 7'(28));
				t.rf[ls * 12 + 6 +: 6] = rfr(ls, 7'(30));
				for (int k = 0; k < 4; k++) begin
					t.mv[ls * 128 + k * 32 +: 32] = mvr(ls, 7'(36 + k));
					t.mvd[ls * 56 + k * 14 +: 14] = sk ? 14'd0 : mdr(ls, 7'(36 + k));
				end
			end
			l = t;
			l.i4 = i4snap_r;
			l.nz = sk ? 8'd0 : cur_pcm ? 8'hFF : {nzc[101], nzc[93], nzc[61], nzc[53], nzc[39], nzc[31], nzc[23], nzc[15]};
			l.dt = {sub[3][8], sub[1][8]};
			for (int ls = 0; ls < 2; ls++) begin
				l.rf[ls * 12 +: 6] = rfr(ls, 7'(14));
				l.rf[ls * 12 + 6 +: 6] = rfr(ls, 7'(30));
				for (int k = 0; k < 4; k++) begin
					l.mv[ls * 128 + k * 32 +: 32] = mvr(ls, 7'(15 + 8 * k));
					l.mvd[ls * 56 + k * 14 +: 14] = sk ? 14'd0 : mdr(ls, 7'(15 + 8 * k));
				end
			end
			rb_we <= 1'b1;
			rb_wa <= mbx;
			rb_wd <= t;
			lrec <= l;
			mr_k <= 6'd0;
			mr_base <= {24'd0, s_slot} * SLOT_WORDS + mb_lin * 32'd36;
			st <= S_E1;
		end
		// motion record of the macroblock (co-located data for later B pictures)
		S_E1: begin
			if (mem_req && mem_ack) mem_req <= 1'b0;
			if (!mem_req || mem_ack) begin
				if (mr_k == 6'd35) begin
					mem_req <= 1'b0;
					st <= S_E2;
				end
				else begin
					reg [31:0] w;
					reg l;
					if (mr_k == 6'd0) w = {18'd0, tpack(mbt)};
					else if (mr_k <= 6'd2) begin
						l = mr_k == 6'd2;
						if (uses(mbt, l))
							w = {8'(rfr(l, 7'(30))), 8'(rfr(l, 7'(28))), 8'(rfr(l, 7'(14))), 8'(rfr(l, 7'(12)))};
						else w = 32'hFFFFFFFF;
					end
					else begin
						reg [4:0] k;
						k = 5'(mr_k - 6'd3);
						w = cv_q;
					end
					mem_req <= 1'b1;
					mem_we <= 1'b1;
					mem_addr <= mr_base + {26'd0, mr_k};
					mem_wdata <= w;
					mr_k <= mr_k + 6'd1;
				end
			end
		end
		S_E2: begin
			for (int k = 0; k < 8; k++) cdc[k] <= 16'sd0;
			trm(S_E3);
		end
		S_E3: begin
			if (mbx + 8'd1 >= s_mbw) begin
				mbx <= 8'd0;
				mby <= mby + 8'd1;
			end
			else mbx <= mbx + 8'd1;
			if (bin || (mbx + 8'd1 >= s_mbw && mby + 8'd1 >= s_mbh)) st <= S_DRAIN;
			else st <= S_MB0;
		end
		S_DRAIN: begin
			// the rest of the slice data is not needed
			if (dleft == 30'd0 || (dleft == 30'd1 && in_valid)) begin
				data_on <= 1'b0;
				sl_done <= 1'b1;
				st <= S_DESC;
			end
		end
		S_ERR: begin
			sl_err <= 1'b1;
			cab_v <= 1'b0;
			st <= S_ERW;
		end
		S_ERW: if (!cab_busy) st <= S_DRAIN;

		// ------------------------------------------------ direct prediction (rsd returns)
		S_DR0: begin
			dr_b8 <= (mbt & MT_88) != 0;
			if (s_sp) st <= S_SD0;
			else begin rs1 <= S_TP0; st <= S_CF0; end
		end
		// spatial: references and vectors from the neighbours
		S_SD0: begin
			reg signed [5:0] rr[0:1];
			reg [31:0] mm[0:1];
			reg [31:0] t, sb;
			t = mbt;
			sb = MT_L0L1;
			for (int l = 0; l < 2; l++) begin
				reg signed [5:0] lr, tr, rc;
				reg [31:0] A, B, C;
				reg [5:0] m;
				lr = rfr(l, 7'(11)); tr = rfr(l, 7'(4)); rc = rfr(l, 7'(8)); C = mvr(l, 7'(8));
				if (rc == -6'sd2) begin rc = rfr(l, 7'(3)); C = mvr(l, 7'(3)); end
				A = mvr(l, 7'(11)); B = mvr(l, 7'(4));
				m = $unsigned(lr) < $unsigned(tr) ? lr : tr;
				m = $unsigned(m) < $unsigned(rc) ? m : rc;
				rr[l] = m;
				if (rr[l] >= 0) begin
					reg [1:0] mc;
					mc = {1'b0, lr == rr[l]} + {1'b0, tr == rr[l]} + {1'b0, rc == rr[l]};
					if (mc > 2'd1) mm[l] = {mid_pred(A[31:16], B[31:16], C[31:16]), mid_pred(A[15:0], B[15:0], C[15:0])};
					else if (lr == rr[l]) mm[l] = A;
					else if (tr == rr[l]) mm[l] = B;
					else mm[l] = C;
				end
				else begin
					mm[l] = 32'd0;
					rr[l] = -6'sd1;
					if (!dr_b8) t = t & ~(l ? MT_L1 : MT_L0);
					sb = sb & ~(l ? MT_L1 : MT_L0);
				end
			end
			if (rr[0] < 0 && rr[1] < 0) begin
				rr[0] = 6'sd0; rr[1] = 6'sd0;
				if (!dr_b8) t = t | MT_L0L1;
				sb = sb | MT_L0L1;
			end
			dr_ref[0] <= rr[0]; dr_ref[1] <= rr[1];
			dr_mv[0] <= mm[0]; dr_mv[1] <= mm[1];
			if (!(dr_b8 || mm[0] != 32'd0 || mm[1] != 32'd0)) begin
				fref(1'b0, 7'd12, 3'd4, 3'd4, rr[0]);
				fref(1'b1, 7'd12, 3'd4, 3'd4, rr[1]);
				fmvz(1'b0, 7'd12, 3'd4, 3'd4);
				fmvz(1'b1, 7'd12, 3'd4, 3'd4);
				mbt <= (t & ~(MT_88 | MT_168 | MT_816 | MT_P1L0 | MT_P1L1)) | MT_16 | MT_DIR;
				st <= rsd;
			end
			else begin
				mbt <= t;
				dr_sub <= sb;
				rs1 <= S_SD1;
				st <= S_CF0;
			end
		end
		S_SD1: begin
			reg [31:0] t, sb;
			t = mbt;
			sb = dr_sub | MT_16 | MT_DIR;
			if (!dr_b8 && (colty & MT_16I) != 0) t = t | MT_16 | MT_DIR;
			else if (!dr_b8 && (colty & (MT_168 | MT_816)) != 0) t = t | MT_DIR | (colty & (MT_168 | MT_816));
			else begin
				if (!s_d8) sb = sb + (MT_88 - MT_16);
				t = t | MT_88;
			end
			dr_sub <= sb;
			if ((t & MT_16) != 0) begin
				reg [31:0] a, b;
				reg signed [15:0] x0, y0, x1, y1;
				x0 = colm00[15:0]; y0 = colm00[31:16];
				x1 = colm10[15:0]; y1 = colm10[31:16];
				fref(1'b0, 7'd12, 3'd4, 3'd4, dr_ref[0]);
				fref(1'b1, 7'd12, 3'd4, 3'd4, dr_ref[1]);
				if (!intra(colty) && !s_l1l &&
				    ((colr0[0] == 8'sd0 && x0 >= -1 && x0 <= 1 && y0 >= -1 && y0 <= 1) ||
				     (colr0[0] < 0 && colr1[0] == 8'sd0 && x1 >= -1 && x1 <= 1 && y1 >= -1 && y1 <= 1 && s_x264))) begin
					a = dr_ref[0] > 0 ? dr_mv[0] : 32'd0;
					b = dr_ref[1] > 0 ? dr_mv[1] : 32'd0;
				end
				else begin
					a = dr_mv[0];
					b = dr_mv[1];
				end
				fmv(1'b0, 7'd12, 3'd4, 3'd4, a);
				fmv(1'b1, 7'd12, 3'd4, 3'd4, b);
				mbt <= t;
				st <= rsd;
			end
			else begin
				mbt <= t;
				dr_i8 <= 3'd0;
				dr_n <= 5'd0;
				st <= S_SD2;
			end
		end
		S_SD2: begin
			if (dr_i8 == 3'd4) begin
				if (!dr_b8 && dr_n[3:0] == 4'd0)
					mbt <= (mbt & ~(MT_88 | MT_168 | MT_816 | MT_P1L0 | MT_P1L1)) | MT_16 | MT_DIR;
				st <= rsd;
			end
			else begin
				reg [1:0] i8;
				reg [6:0] s8;
				reg [31:0] v0, v1;
				reg [4:0] n;
				i8 = dr_i8[1:0];
				s8 = f_scan8({2'd0, i8, 2'b00});
				n = dr_n;
				if (!(dr_b8 && (sub[i8] & MT_DIR) == 0)) begin
					reg [31:0] sb;
					sb = dr_sub;
					fref(1'b0, s8, 3'd2, 3'd2, dr_ref[0]);
					fref(1'b1, s8, 3'd2, 3'd2, dr_ref[1]);
					for (int k = 0; k < 4; k++) begin
						reg [6:0] e;
						reg ze;
						reg signed [15:0] cx, cy;
						reg [31:0] cm;
						e = s8 + (k[0] ? 7'd1 : 7'd0) + (k[1] ? 7'd8 : 7'd0);
						ze = 1'b0;
						if (!intra(colty) && !s_l1l && (colr0[i8] == 8'sd0 || (colr0[i8] < 0 && colr1[i8] == 8'sd0 && s_x264))) begin
							reg [3:0] ci;
							if ((sb & MT_16) != 0) ci = {i8[1], i8[1], i8[0], i8[0]};          // x8 * 3 + y8 * 12
							else ci = {i8[1], k[1], i8[0], k[0]};
							cm = col_q[k];         // col_a[k]: {colr0 != 0, ci}
							cx = cm[15:0]; cy = cm[31:16];
							ze = cx >= -1 && cx <= 1 && cy >= -1 && cy <= 1;
						end
						for (int l = 0; l < 2; l++)
							if (ze && dr_ref[l] == 6'sd0) mvw_z[l][6'(e)] = 1'b1;
							else begin mvw_v[l] = dr_mv[l]; mvw_m[l][6'(e)] = 1'b1; end
						if (ze) n = n + 5'd1;
						if (k == 3 && (sb & MT_16) == 0) begin
							// !(m & 3): sub-block count of this 8x8
							reg [2:0] m;
							m = 3'(n - dr_n);
							if (m[1:0] == 2'd0) sb = sb + (MT_16 - MT_88);
						end
					end
					sub[i8] <= sb;
				end
				dr_n <= n;
				dr_i8 <= dr_i8 + 3'd1;
			end
		end
		// temporal: map_col_to_list0 and dist_scale_factor of the 4 co-located 8x8 blocks
		S_TP0: begin
			mp_ra <= colr0[tk] >= 0 ? {1'b0, colr0[tk][4:0]} : {1'b1, colr1[tk][4:0]};
			st <= S_TP1;
		end
		S_TP1: st <= S_TP2;
		S_TP2: begin
			rrk[tk] <= mp_q;
			ds_ra <= mp_q[4:0];
			st <= S_TP3;
		end
		S_TP3: st <= S_TP4;
		S_TP4: begin
			sck[tk] <= ds_q;
			tk <= tk + 2'd1;
			st <= tk == 2'd3 ? S_TD1 : S_TP0;
		end
		// temporal
		S_TD1: begin
			reg [31:0] t, sb;
			t = mbt;
			sb = MT_16 | MT_P0L0 | MT_P0L1 | MT_DIR;
			if (!dr_b8 && (colty & MT_16I) != 0) t = t | MT_16 | MT_P0L0 | MT_P0L1 | MT_DIR;
			else if (!dr_b8 && (colty & (MT_168 | MT_816)) != 0) t = t | MT_L0L1 | MT_DIR | (colty & (MT_168 | MT_816));
			else begin
				if (!s_d8) sb = MT_88 | MT_P0L0 | MT_P0L1 | MT_DIR;
				t = t | MT_88 | MT_L0L1;
			end
			mbt <= t;
			dr_sub <= sb;
			dr_i8 <= (t & MT_16) != 0 ? 3'd7 : 3'd0;
			dr_i4 <= 2'd0;
			st <= S_TD2;
		end
		S_TD2: begin
			// one 4x4 block a clock: 16x16 (dr_i8 7) or 8x8 dr_i8, block dr_i4
			if (dr_i8 == 3'd4) st <= rsd;
			else begin
				reg [1:0] i8;
				reg is16, sub8;
				reg signed [7:0] r0, rr;
				reg [31:0] cm;
				reg [3:0] ci;
				reg [6:0] s8;
				i8 = dr_i8[1:0];
				is16 = dr_i8 == 3'd7;
				sub8 = is16 || (dr_sub & MT_16) != 0;
				if (is16) i8 = 2'd0;
				s8 = is16 ? 7'd12 : f_scan8({2'd0, i8, 2'b00});
				if (!is16 && dr_b8 && (sub[i8] & MT_DIR) == 0) begin
					dr_i8 <= dr_i8 + 3'd1;
				end
				else begin
					if (dr_i4 == 2'd0) begin
						if (!is16) sub[i8] <= dr_sub;
						if (is16) fref(1'b1, 7'd12, 3'd4, 3'd4, 6'd0);
						else fref(1'b1, s8, 3'd2, 3'd2, 6'd0);
					end
					if (intra(colty)) begin
						if (is16) begin
							fref(1'b0, 7'd12, 3'd4, 3'd4, 6'd0);
							fmvz(1'b0, 7'd12, 3'd4, 3'd4);
							fmvz(1'b1, 7'd12, 3'd4, 3'd4);
							st <= rsd;
						end
						else begin
							fref(1'b0, s8, 3'd2, 3'd2, 6'd0);
							fmvz(1'b0, s8, 3'd2, 3'd2);
							fmvz(1'b1, s8, 3'd2, 3'd2);
							dr_i8 <= dr_i8 + 3'd1;
						end
					end
					else begin
						reg signed [31:0] px, py, mx, my;
						reg signed [15:0] cx, cy;
						r0 = colr0[i8];
						rr = $signed(rrk[i8]);
						if (sub8) ci = {i8[1], i8[1], i8[0], i8[0]};
						else ci = {i8[1], dr_i4[1], i8[0], dr_i4[0]};
						cm = col_q[0];           // col_a[0]: {r0 < 0, ci}
						cx = cm[15:0]; cy = cm[31:16];
						px = $signed(sck[i8]) * cx + 32'sd128;
						py = $signed(sck[i8]) * cy + 32'sd128;
						mx = px >>> 8;
						my = py >>> 8;
						if (sub8) begin
							if (is16) begin
								fref(1'b0, 7'd12, 3'd4, 3'd4, rr[5:0]);
								fmv(1'b0, 7'd12, 3'd4, 3'd4, {my[15:0], mx[15:0]});
								fmv(1'b1, 7'd12, 3'd4, 3'd4, {16'(my - cy), 16'(mx - cx)});
								st <= rsd;
							end
							else begin
								fref(1'b0, s8, 3'd2, 3'd2, rr[5:0]);
								fmv(1'b0, s8, 3'd2, 3'd2, {my[15:0], mx[15:0]});
								fmv(1'b1, s8, 3'd2, 3'd2, {16'(my - cy), 16'(mx - cx)});
								dr_i8 <= dr_i8 + 3'd1;
							end
						end
						else begin
							reg [6:0] e;
							reg signed [15:0] lx, ly;
							e = s8 + (dr_i4[0] ? 7'd1 : 7'd0) + (dr_i4[1] ? 7'd8 : 7'd0);
							if (dr_i4 == 2'd0) fref(1'b0, s8, 3'd2, 3'd2, rr[5:0]);
							lx = mx[15:0];
							ly = my[15:0];
							fmv(1'b0, e, 3'd1, 3'd1, {ly, lx});
							fmv(1'b1, e, 3'd1, 3'd1, {16'(ly - cy), 16'(lx - cx)});
							dr_i4 <= dr_i4 + 2'd1;
							if (dr_i4 == 2'd3) dr_i8 <= dr_i8 + 3'd1;
						end
					end
				end
			end
		end
		// co-located macroblock from the motion memory (35 words)
		S_CF0: begin
			tk <= 2'd0;
			mc_i <= 6'd0;
			mc_r <= 6'd0;
			mr_base <= {24'd0, s_col} * SLOT_WORDS + mb_lin * 32'd36;
			mem_we <= 1'b0;
			st <= S_CF1;
		end
		S_CF1: begin
			if (mem_req && mem_ack) mem_req <= 1'b0;
			if ((!mem_req || mem_ack) && mc_i != 6'd35) begin
				mem_req <= 1'b1;
				mem_addr <= mr_base + {26'd0, mc_i};
				mc_i <= mc_i + 6'd1;
			end
			if (mem_rvalid) begin
				if (mc_r == 6'd0) colty <= tunpack(mem_rdata[13:0]);
				else if (mc_r == 6'd1) for (int k = 0; k < 4; k++) colr0[k] <= mem_rdata[8 * k +: 8];
				else if (mc_r == 6'd2) for (int k = 0; k < 4; k++) colr1[k] <= mem_rdata[8 * k +: 8];
				else begin
					colm_we <= 1'b1;
					colm_wa <= 5'(mc_r - 6'd3);
					colm_wd <= mem_rdata;
					if (mc_r == 6'd3) colm00 <= mem_rdata;
					if (mc_r == 6'd19) colm10 <= mem_rdata;
				end
				mc_r <= mc_r + 6'd1;
				if (mc_r == 6'd34) st <= rs1;
			end
		end

		default: st <= S_ERR;
		endcase

		// ------------------------------------------------ dequantisation pipeline (coefficients from cab_dec)
		c0_v <= 1'b0;
		c1_v <= c0_v;
		c2_v <= c1_v;
		c3_v <= c2_v;
		if (r_on && cab_ov && cab_kind == 2'd1) begin
			reg [5:0] j, p;
			reg [6:0] pos;
			reg [7:0] smi;
			pos = cab_pos;
			if (r_cat == 3'd0) ldc[f_zz4(pos[3:0])] <= cab_val[15:0];
			else if (r_cat == 3'd3) cdc[{r_blk == 6'd50, pos[1:0]}] <= cab_val[15:0];
			else begin
				if (r_cat == 3'd5) begin j = f_zz8(pos[5:0]); p = t8(j); end
				else begin
					j = {2'd0, f_zz4(r_cat == 3'd2 ? pos[3:0] : pos[3:0] + 4'd1)};
					p = {2'd0, t4(j[3:0])};
				end
				if (r_cat == 3'd5) smi = 8'd96 + {1'b0, r_list[0], p};
				else smi = {1'b0, r_list, 4'd0} + {2'd0, p};
				c0_v <= 1'b1;
				c0_s <= cab_val[21:0];
				c0_j <= j;
				c0_p <= p;
				c0_b <= r_base;
				c0_8 <= r_cat == 3'd5;
				c0_m <= qmod(r_qp);
				c0_d <= qdiv(r_qp);
				c0_by <= smi[1:0];
			end
		end
		// cache write buses
		// (interior places only; the -2 of column 8 is constant in rfr)
		for (int l = 0; l < 2; l++) for (int i = 12; i < 40; i++) if (i % 8 >= 4) begin
			if (mvw_m[l][i] || mvw_z[l][i]) mvc[l][ix(7'(i))] <= mvw_z[l][i] ? 32'd0 : mvw_v[l];
			if (mdw_m[l][i] || mdw_z[l][i]) mvdc[l][ix(7'(i))] <= mdw_z[l][i] ? 14'd0 : mdw_v[l];
			if (rfw_m[l][i] || rfw_n[l][i]) refc[l][ix(7'(i))] <= rfw_n[l][i] ? -6'sd2 : rfw_v[l];
		end
		// stage 1: dequant factor (class, scaling matrix byte, QP / 6)
		if (c0_v) begin
			reg [10:0] f;
			reg [7:0] sm;
			sm = 8'(sm_q >> {c0_by, 3'd0});
			f = c0_8 ? 11'(d8init(c0_m, d8scan({c0_p[4:3], c0_p[1:0]}))) : 11'(d4init(c0_m, {1'b0, c0_p[0]} + {1'b0, c0_p[2]}));
			c1_s <= c0_s; c1_j <= c0_j; c1_p <= c0_p; c1_b <= c0_b;
			c1_q <= 22'({11'd0, f} * {14'd0, sm} << (c0_8 ? {1'b0, c0_d} : {1'b0, c0_d} + 5'd2));
		end
		// stage 2: product (the low 22 bits are enough for (product + 32) >> 6 in 16 bits)
		if (c1_v) begin
			c2_s <= c1_s; c2_j <= c1_j; c2_p <= c1_p; c2_b <= c1_b;
			c2_pr <= 22'(c1_s * c1_q);
		end
		// stage 3: (product + 32) >> 6 into the coefficient RAM
		if (c2_v) begin
			reg [21:0] r;
			r = c2_pr + 22'd32;
			cr_we <= 1'b1;
			cr_wa <= c2_b + {3'd0, c2_j};
			cr_wd <= r[21:6];
			if (r[21:6] != 16'd0) begin
				bw_we <= 1'b1;
				bw_a <= br_a;
				bw_d <= br_m | (16'd1 << bm_ix[3:0]);
				rv[br_a] <= 1'b1;
				ncoef <= ncoef + 9'd1;
			end
		end
	end
end


// ---------------------------------------------------------------- RAM read addresses
// scaling matrix word of the coefficient cab_dec gives now (stage 0)
reg  [7:0] smi_c;
always @(*) begin
	reg [5:0] j, p;
	if (r_cat == 3'd5) begin j = f_zz8(cab_pos[5:0]); p = t8(j); end
	else begin
		j = {2'd0, f_zz4(r_cat == 3'd2 ? cab_pos[3:0] : cab_pos[3:0] + 4'd1)};
		p = {2'd0, t4(j[3:0])};
	end
	smi_c = r_cat == 3'd5 ? 8'd96 + {1'b0, r_list[0], p} : {1'b0, r_list, 4'd0} + {2'd0, p};
end
assign sm_rw = smi_c[7:2];
// implicit weight of the partition whose refs word goes out next (S_O3)
reg  [9:0] iw_c;
always @(*) begin
	reg [1:0] pt;
	reg [6:0] s8;
	pt = of_room ? ok[1:0] : ok[1:0] - 2'd1;
	s8 = f_scan8({2'd0, pt, 2'b00});
	iw_c = {5'(rfr(0, 7'(s8))), 5'(rfr(1, 7'(s8)))};
end
assign iw_ra = iw_c;
// shared cache read port
always @(*) begin
	reg [6:0] i8;
	reg [4:0] k;
	i8 = f_scan8({2'd0, pm_n});
	cr_a = {pm_l, 6'(i8 - 7'd1)};
	case (st)
	S_PMB: cr_a = {pm_l, 6'(i8 - 7'd8)};
	S_PMC: cr_a = {pm_l, 6'(i8 - 7'd8 + {4'd0, pm_pw})};
	S_PMD: cr_a = {pm_l, 6'(i8 - 7'd9)};
	S_O3: begin k = 5'(ok - 6'd5); cr_a = {k[4], 6'(f_scan8({2'd0, k[3:0]}))}; end
	S_E1: begin k = 5'(mr_k - 6'd3); cr_a = {k[4], 6'(7'd12 + {5'd0, k[1:0]} + {2'd0, k[3:2], 3'd0})}; end
	default: ;
	endcase
end
// co-located vector reads: S_TD2 one block (copy 0), S_SD2 the 4 blocks of 8x8 dr_i8
always @(*) begin
	reg [1:0] i8;
	reg sub8;
	if (st == S_TD2) begin
		i8 = dr_i8 == 3'd7 ? 2'd0 : dr_i8[1:0];
		sub8 = dr_i8 == 3'd7 || (dr_sub & MT_16) != 0;
		col_a[0] = {colr0[i8] < 0, sub8 ? {i8[1], i8[1], i8[0], i8[0]} : {i8[1], dr_i4[1], i8[0], dr_i4[0]}};
		for (int k = 1; k < 4; k++) col_a[k] = 5'd0;
	end
	else begin
		i8 = dr_i8[1:0];
		for (int k = 0; k < 4; k++)
			col_a[k] = {colr0[i8] != 8'sd0, (dr_sub & MT_16) != 0 ? {i8[1], i8[1], i8[0], i8[0]} : {i8[1], k[1], i8[0], k[0]}};
	end
end
// coefficient word of the output scan
// bitmap RAM read: the output scan in S_O5, else the coefficient of stage 3
assign br_a = st == S_O5 ? sc_g : bm_ix[8:4];
assign cr_re = st == S_O5 && of_room && grp != 24'd0;
assign cr_ra = (sc_g < 5'd16 && (mbt & MT_DCT) != 0) ? {1'b0, sc_g[3:2], t8({sc_g[1:0], sc_b})} : {sc_g, t4(sc_b)};

endmodule
`default_nettype wire
