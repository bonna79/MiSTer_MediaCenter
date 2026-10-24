// H.264 reconstruction engine for the MiSTer MediaCenter FPGA accelerator.
//
// The ARM reads the stream (CABAC/CAVLC, modes, vectors, coefficients) and
// sends the commands of accel/doc/comandi.md; this engine rebuilds the
// pixels: inverse transforms, intra prediction, inter prediction (acc_mc),
// and writes every macroblock into the picture slot in DDR. Progressive
// 8-bit 4:2:0 only, no deblocking (light mode, as the player today).
//
// Picture layout in memory (64-bit words): slot base = slot << SLOT_L2;
// Y rows of (1 << YSTRIDE_L2) words, U at UOFF and V at VOFF words from the
// slot base, rows of (1 << CSTRIDE_L2) words.
module acc_engine #(
	parameter AW         = 28,
	parameter SLOT_L2    = 19,
	parameter YSTRIDE_L2 = 8,
	parameter CSTRIDE_L2 = 7,
	parameter UOFF       = 278528,
	parameter VOFF       = 348160
)
(
	input             clk,
	input             reset,

	// commands
	input             cmd_valid,
	input      [31:0] cmd_data,
	output            cmd_ready,

	// memory (DDRAM style: 64-bit words, bursts, in-order read data)
	output            mem_rd,
	output            mem_we,
	output [AW-1:0]   mem_addr,
	output      [7:0] mem_burst,
	output     [63:0] mem_din,
	output      [7:0] mem_be,
	input             mem_busy,
	input      [63:0] mem_dout,
	input             mem_dout_ready,

	output reg        pic_done,     // pulse at the end of each picture
	output reg  [7:0] pic_slot,
	output reg        error,        // unexpected command
	output reg [31:0] mb_count
);

// ------------------------------------------------------------------ state

localparam S_CMD = 0, S_PIC = 1, S_REFS = 2, S_MB1 = 3, S_INFO = 4, S_NCOEF = 5, S_COEF = 6,
           S_IDCTW = 7, S_CTX = 8, S_I4 = 9, S_I8 = 10, S_I16 = 11, S_CHR = 12, S_MC = 13,
           S_WB = 14, S_LB = 15, S_PCM = 16, S_DONE = 17, S_HAND = 18, S_IDLE = 19;

// Two stages working on consecutive macroblocks:
//  A (sa): reads the commands, transforms the coefficients into the residual
//    buffer, then hands the macroblock to B;
//  B (sb): context, prediction + residual, write back.
// Picture and slice commands wait until B is idle.
reg  [4:0] sa, sb;
reg        abuf, bbuf;        // residual buffer of A and of B

// macroblock being read by A
reg  [7:0] a_mbx, a_mby;
reg  [2:0] a_mtype;
reg  [3:0] a_imode[16];
reg [15:0] a_tr, a_tl;
reg  [3:0] a_cmode, a_i16mode;
reg [13:0] a_qref[4];
reg  [8:0] a_iw[4];
reg [31:0] a_mv[16], a_mv1[16];
reg  [7:0] a_q_slot[4], a_q_slot1[4];
reg [50:0] a_q_wt[4], a_q_wt1[4];

reg  [7:0] wmb, hmb;          // picture size in macroblocks
reg  [7:0] cur_slot;
reg  [7:0] mbx, mby;
reg  [2:0] mtype;             // 0 inter, 1 I4x4, 2 I8x8, 3 I16x16, 4 PCM
wire       pcm = mtype == 4;  // done as 16x16 + chroma with prediction 0
reg [23:0] cmask;
reg        t8;
reg  [3:0] imode[16];
reg [15:0] tr_mask, tl_mask;
reg  [3:0] cmode, i16mode;
reg [13:0] qref[4];           // reference of each 8x8: list 0 bits 0-5, list 1 bits 7-12, bits 6 and 13 = list unused
reg  [8:0] iw[4];             // implicit weight of list 0 of each 8x8
reg [31:0] mv[16], mv1[16];

// reference lists
reg  [5:0] n0, n1;
reg        two_lists;
reg  [1:0] use_w;
reg        use_wc;
reg  [3:0] lden, cden;
reg  [7:0] rcnt;              // reference words left
reg  [1:0] rword;
reg  [5:0] ridx;
reg        rlist;

reg  [5:0] icnt;              // info words read
reg [15:0] ncoef;
reg  [6:0] pcnt;
reg  [3:0] ctxk;
reg        go_pending;

// ------------------------------------------------------------- MB storage

// The reconstructed macroblock and its residual are in block RAM (acc_recon,
// acc_resid); here only the pixels the intra prediction needs inside the
// macroblock: bottom row of each band of 4 luma rows (rows 3, 7, 11, 15)
// and right column of each band of 4 luma columns (columns 3, 7, 11, 15),
// pixel i at bits i*8; for chroma the bottom row and right column.
reg [127:0] brow[4], rcol[4];
reg  [63:0] cbot_u, cbot_v, crt_u, crt_v;

reg  [7:0] topy[24];          // row above (16 + 8 above right)
reg  [7:0] topu[8], topv[8];
reg  [7:0] lefty[16], leftu[8], leftv[8];
reg  [7:0] cornery, corneru, cornerv;

// line buffers: bottom row of the macroblock row above, 4 pixels a word
reg [31:0] lby[512];
reg [31:0] lbu[256], lbv[256];
reg  [8:0] lby_ra;
reg  [7:0] lbc_ra;
reg [31:0] lby_q, lbu_q, lbv_q;
always @(posedge clk) begin
	lby_q <= lby[lby_ra];
	lbu_q <= lbu[lbc_ra];
	lbv_q <= lbv[lbc_ra];
end

function [7:0] clip8(input signed [19:0] v);
	clip8 = v < 0 ? 8'd0 : (v > 255 ? 8'd255 : v[7:0]);
endfunction

// ------------------------------------------------------------------ IDCT

reg        idct_clr, idct_wr, idct_go;
reg  [5:0] idct_pos;
reg signed [15:0] idct_val;
reg  [4:0] idct_blk, coef_blk;
reg        idct_big, coef_have, coef_last;
wire       idct_busy, idct_rv, idct_in;
wire [2:0] idct_col;
wire signed [15:0] idct_res[8];

acc_idct idct
(
	.clk(clk), .clr(idct_clr), .t8(idct_big), .wr(idct_wr), .wr_pos(idct_pos), .wr_val(idct_val),
	.go(idct_go), .busy(idct_busy), .in_ready(idct_in), .res_valid(idct_rv), .res_col(idct_col), .res(idct_res)
);


// -------------------------------------------------- intra 4x4 / 8x8 edges

reg  [3:0] blk;               // 4x4 block (luma4x4BlkIdx) or 8x8 block * 4
reg  [4:0] ph;                // phase inside the block
reg  [7:0] e[25];             // edge: e[8] top left, e[9..] top, e[7..0] left
reg  [7:0] eraw[25];
wire [1:0] bxq = {blk[2], blk[0]};
wire [1:0] byq = {blk[3], blk[1]};
wire [3:0] bx = {bxq, 2'b00};
wire [3:0] by = {byq, 2'b00};

// raw neighbours of the block at (bx, by)
integer ii;
always @* begin
	for (ii = 0; ii < 16; ii = ii + 1)
		eraw[9 + ii] = byq == 0 ? topy[bx + ii] : (bx + ii < 16 ? brow[byq - 2'd1][(bx + ii) * 8 +: 8] : 8'd0);
	for (ii = 0; ii < 8; ii = ii + 1)
		eraw[7 - ii] = by + ii >= 16 ? 8'd0 : (bxq == 0 ? lefty[by + ii] : rcol[bxq - 2'd1][(by + ii) * 8 +: 8]);
	eraw[8] = byq == 0 ? (bxq == 0 ? cornery : topy[bx - 1]) : (bxq == 0 ? lefty[by - 1] : brow[byq - 2'd1][(bx - 1) * 8 +: 8]);
end

// 8x8 reference filtering (FFmpeg PREDICT_8x8_LOAD_*)
wire has_tl8 = tl_mask[15 - blk];
wire has_tr8 = tr_mask[14 - blk];
reg  [7:0] er[25];            // raw edge, registered (phase 0)
reg  [7:0] efil[25];
always @* begin
	efil[9] = ((has_tl8 ? er[8] : er[9]) + 2 * er[9] + er[10] + 2) >> 2;
	for (ii = 1; ii < 7; ii = ii + 1)
		efil[9 + ii] = (er[8 + ii] + 2 * er[9 + ii] + er[10 + ii] + 2) >> 2;
	efil[16] = ((has_tr8 ? er[17] : er[16]) + 2 * er[16] + er[15] + 2) >> 2;
	for (ii = 8; ii < 15; ii = ii + 1)
		efil[9 + ii] = has_tr8 ? 8'((er[8 + ii] + 2 * er[9 + ii] + er[10 + ii] + 2) >> 2) : er[16];
	efil[24] = has_tr8 ? 8'((er[23] + 3 * er[24] + 2) >> 2) : er[16];
	// left: e[7] = l0 ... e[0] = l7
	efil[7] = ((has_tl8 ? er[8] : er[7]) + 2 * er[7] + er[6] + 2) >> 2;
	for (ii = 1; ii < 7; ii = ii + 1)
		efil[7 - ii] = (er[8 - ii] + 2 * er[7 - ii] + er[6 - ii] + 2) >> 2;
	efil[0] = (er[1] + 3 * er[0] + 2) >> 2;
	efil[8] = (er[7] + 2 * er[8] + er[9] + 2) >> 2;
end

// DC of the block from the (filtered) edge: sums (phase 2), then value
// (phase 6), only for the DC modes
reg  [3:0] mode_r;            // prediction mode of the block (phase 0)
reg  [7:0] dcv;
wire       blk_dc = mode_r == 2 || mode_r > 8;
reg [11:0] dst, dsl;
always @(posedge clk) begin
	if (ph == 2) begin
		if (mtype == 1) begin
			dst <= e[9] + e[10] + e[11] + e[12];
			dsl <= e[7] + e[6] + e[5] + e[4];
		end
		else begin
			dst <= e[9] + e[10] + e[11] + e[12] + e[13] + e[14] + e[15] + e[16];
			dsl <= e[7] + e[6] + e[5] + e[4] + e[3] + e[2] + e[1] + e[0];
		end
	end
	if (mtype == 1)
		case (mode_r)
			9:       dcv <= (dsl + 2) >> 2;
			10:      dcv <= (dst + 2) >> 2;
			11:      dcv <= 128;
			default: dcv <= (dst + dsl + 4) >> 3;
		endcase
	else
		case (mode_r)
			9:       dcv <= (dsl + 4) >> 3;
			10:      dcv <= (dst + 4) >> 3;
			11:      dcv <= 128;
			default: dcv <= (dst + dsl + 8) >> 4;
		endcase
end

// four predicted pixels of row prow, columns pcol..pcol+3 of the block. The
// operation and the edge samples of each pixel are worked out one clock
// ahead (for the next group) and kept in registers.
reg  [2:0] prow, pcol;
reg  [1:0] opr[4];
reg  [4:0] ia[4], ib[4], ic[4];
integer pp, pq, pd, xx, yy, z, idx, nn, op, kk, nx, ny;
function [4:0] ecl(input integer i, input integer n);
	ecl = 5'(8 + (i < -n ? -n : (i > 2 * n ? 2 * n : i)));
endfunction
always @(posedge clk) begin
	nn = mtype == 1 ? 4 : 8;
	if (ph != 1) begin nx = 0; ny = 0; end
	else if (mtype != 1 && pcol == 0) begin nx = 4; ny = prow; end
	else begin nx = 0; ny = prow + 1; end
	for (pq = 0; pq < 4; pq = pq + 1) begin
		xx = nx + pq;
		yy = ny;
		op = 0;
		idx = 0;
		case (mode_r)
			0: begin op = 0; idx = xx + 1; end
			1: begin op = 0; idx = -1 - yy; end
			3: begin op = 2; idx = xx + yy + 2; end
			4: begin op = 2; idx = xx - yy; end
			5: begin
				z = 2 * xx - yy;
				if (z >= 0) begin op = z[0] ? 2 : 1; idx = xx - (yy >> 1); end
				else begin op = 2; idx = z + 1; end
			end
			6: begin
				z = 2 * yy - xx;
				if (z >= 0 && !z[0]) begin op = 1; idx = (xx >> 1) - yy - 1; end
				else if (z >= 0) begin op = 2; idx = (xx >> 1) - yy; end
				else begin op = 2; idx = xx - 2 * yy - 1; end
			end
			7: begin
				if (!yy[0]) begin op = 1; idx = xx + (yy >> 1) + 1; end
				else begin op = 2; idx = xx + (yy >> 1) + 2; end
			end
			8: begin
				z = xx + 2 * yy;
				kk = yy + (xx >> 1);
				if (z < 2 * nn - 3) begin op = z[0] ? 2 : 1; idx = -2 - kk; end
				else if (z == 2 * nn - 3) begin op = 2; idx = -nn; end
				else begin op = 0; idx = -nn; end
			end
			default: op = 3;
		endcase
		opr[pq] <= 2'(op);
		ia[pq] <= ecl(idx - 1, nn);
		ib[pq] <= ecl(idx, nn);
		ic[pq] <= ecl(idx + 1, nn);
	end
end
// ------------------------------------------------- 16x16 and chroma intra

reg signed [19:0] pa, pb, pc;        // plane parameters
reg  [7:0] dc16;
reg  [7:0] dcq[4];                    // chroma DC of each 4x4 quadrant
reg  [3:0] prow16;
reg  [1:0] pgrp;
reg        cpl;                       // chroma plane: 0 U, 1 V

// plane parameters and DCs, computed in a setup clock
reg signed [19:0] hsum, vsum, hsum2, vsum2;   // first and second half
reg [12:0] stop, sleft;
reg [10:0] sq_t0, sq_t1, sq_l0, sq_l1;
integer q;
always @* begin
	hsum = 0;
	vsum = 0;
	hsum2 = 0;
	vsum2 = 0;
	if (sb == S_I16) begin
		for (q = 0; q < 4; q = q + 1) begin
			hsum = hsum + (q + 1) * ($signed({1'b0, topy[8 + q]}) - $signed({1'b0, topy[6 - q]}));
			vsum = vsum + (q + 1) * ($signed({1'b0, lefty[8 + q]}) - $signed({1'b0, lefty[6 - q]}));
		end
		for (q = 4; q < 8; q = q + 1) begin
			hsum2 = hsum2 + (q + 1) * ($signed({1'b0, topy[8 + q]}) - $signed({1'b0, (q == 7 ? cornery : topy[6 - q])}));
			vsum2 = vsum2 + (q + 1) * ($signed({1'b0, lefty[8 + q]}) - $signed({1'b0, (q == 7 ? cornery : lefty[6 - q])}));
		end
	end
	else begin
		for (q = 0; q < 4; q = q + 1) begin
			if (q < 2) begin
				hsum = hsum + (q + 1) * ($signed({1'b0, (cpl ? topv[4 + q] : topu[4 + q])}) -
				                         $signed({1'b0, (cpl ? topv[2 - q] : topu[2 - q])}));
				vsum = vsum + (q + 1) * ($signed({1'b0, (cpl ? leftv[4 + q] : leftu[4 + q])}) -
				                         $signed({1'b0, (cpl ? leftv[2 - q] : leftu[2 - q])}));
			end
			else begin
				hsum2 = hsum2 + (q + 1) * ($signed({1'b0, (cpl ? topv[4 + q] : topu[4 + q])}) -
				                           $signed({1'b0, (q == 3 ? (cpl ? cornerv : corneru) : (cpl ? topv[2 - q] : topu[2 - q]))}));
				vsum2 = vsum2 + (q + 1) * ($signed({1'b0, (cpl ? leftv[4 + q] : leftu[4 + q])}) -
				                           $signed({1'b0, (q == 3 ? (cpl ? cornerv : corneru) : (cpl ? leftv[2 - q] : leftu[2 - q]))}));
			end
		end
	end
	stop = 0;
	sleft = 0;
	for (q = 0; q < 16; q = q + 1) begin
		stop = stop + topy[q];
		sleft = sleft + lefty[q];
	end
	sq_t0 = 0; sq_t1 = 0; sq_l0 = 0; sq_l1 = 0;
	for (q = 0; q < 4; q = q + 1) begin
		sq_t0 = sq_t0 + (cpl ? topv[q] : topu[q]);
		sq_t1 = sq_t1 + (cpl ? topv[4 + q] : topu[4 + q]);
		sq_l0 = sq_l0 + (cpl ? leftv[q] : leftu[q]);
		sq_l1 = sq_l1 + (cpl ? leftv[4 + q] : leftu[4 + q]);
	end
end

// four predicted pixels of the 16x16 / chroma block, row prow16, group
// pgrp: worked out one clock ahead (for the next group): the plane terms
// (bp_y + bp_x, finished at stage 0 of the write) or the value (bp_v)
reg  [7:0] bp_v[4];
reg signed [19:0] bp_y, bp_x[4];
reg        bp_pl;
reg signed [19:0] hs_r, vs_r;
reg signed [19:0] hA, hB, vA, vB;
reg [12:0] stop_r, sleft_r;
reg [10:0] sqt0_r, sqt1_r, sql0_r, sql1_r;
reg  [3:0] nrow;
reg  [1:0] ngrp;
integer bxx, byy;
wire signed [5:0] prow_o = $signed({2'b00, prow16}) - (sb == S_I16 ? 6'sd7 : 6'sd3);
always @(posedge clk) begin
	if (ph != 1) begin nrow = 0; ngrp = 0; end
	else if (sb == S_I16 ? pgrp == 3 : pgrp[0]) begin nrow = prow16 + 1'd1; ngrp = 0; end
	else begin nrow = prow16; ngrp = pgrp + 1'd1; end
	byy = nrow;
	bp_pl <= !pcm && (sb == S_I16 ? i16mode == 3 : cmode == 3);
	// plane row term pa + pc * (nrow - 7 or 3) + 16, with nrow = 0, prow16 or
	// prow16 + 1: the three values are made apart and nrow only picks one
	// (keeps nrow's logic out of the multiplier)
	if (ph != 1) bp_y <= pa + 20'sd16 - (sb == S_I16 ? (pc <<< 3) - pc : (pc <<< 2) - pc);
	else if (sb == S_I16 ? pgrp == 3 : pgrp[0]) bp_y <= pa + pc * (prow_o + 1) + 20'sd16;
	else bp_y <= pa + pc * prow_o + 20'sd16;
	for (pp = 0; pp < 4; pp = pp + 1) begin
		bxx = ngrp * 4 + pp;
		bp_x[pp] <= sb == S_I16 ? pb * (bxx - 7) : pb * (bxx - 3);
		if (sb == S_I16) begin
			if (pcm) bp_v[pp] <= 0;
			else case (i16mode)
				1: bp_v[pp] <= lefty[nrow];
				2: bp_v[pp] <= topy[bxx];
				default: bp_v[pp] <= dc16;
			endcase
		end
		else begin
			if (pcm) bp_v[pp] <= 0;
			else case (cmode)
				1: bp_v[pp] <= cpl ? leftv[nrow] : leftu[nrow];
				2: bp_v[pp] <= cpl ? topv[bxx] : topu[bxx];
				default: bp_v[pp] <= dcq[{nrow[2], ngrp[0]}];
			endcase
		end
	end
end

// ----------------------------------------------------------- inter (MC)

reg        mc_start;
wire       mc_done;
wire       mc_px_valid;
wire [1:0] mc_px_plane;
wire [3:0] mc_px_x, mc_px_y;
wire [2:0] mc_px_n;
wire [31:0] mc_px;
wire       mc_rd;
wire [AW-1:0] mc_addr;
wire [7:0] mc_burst;

// per 8x8 quadrant: slot and weights of its reference (read from the
// reference table as the reference indexes arrive)
reg  [7:0] q_slot[4], q_slot1[4];
reg [50:0] q_wt[4], q_wt1[4];
genvar gq;

wire [511:0] mv_flat, mv1_flat;
generate
	for (gq = 0; gq < 16; gq = gq + 1) begin : mvf
		assign mv_flat[gq * 32 +: 32] = mv[gq];
		assign mv1_flat[gq * 32 +: 32] = mv1[gq];
	end
endgenerate

acc_mc #(.AW(AW), .SLOT_L2(SLOT_L2), .YSTRIDE_L2(YSTRIDE_L2), .CSTRIDE_L2(CSTRIDE_L2), .UOFF(UOFF), .VOFF(VOFF)) mc
(
	.clk(clk), .reset(reset),
	.start(mc_start),
	.mbx(mbx), .mby(mby), .wmb(wmb), .hmb(hmb),
	.mvs(mv_flat), .mvs1(mv1_flat),
	.qref({qref[3], qref[2], qref[1], qref[0]}),
	.iw({iw[3], iw[2], iw[1], iw[0]}),
	.qslot({q_slot[3], q_slot[2], q_slot[1], q_slot[0]}),
	.qslot1({q_slot1[3], q_slot1[2], q_slot1[1], q_slot1[0]}),
	.qwt({q_wt[3], q_wt[2], q_wt[1], q_wt[0]}),
	.qwt1({q_wt1[3], q_wt1[2], q_wt1[1], q_wt1[0]}),
	.use_w(use_w), .use_wc(use_wc), .lden(lden), .cden(cden),
	.mem_rd(mc_rd), .mem_addr(mc_addr), .mem_burst(mc_burst), .mem_busy(mem_busy || wb_we),
	.mem_dout(mem_dout), .mem_dout_ready(mem_dout_ready),
	.px_valid(mc_px_valid), .px_plane(mc_px_plane), .px_x(mc_px_x), .px_y(mc_px_y), .px_n(mc_px_n), .px(mc_px),
	.done(mc_done)
);

// --------------------------------------------------------- write back
// The writer sends a finished macroblock to memory (48 words) while B
// already predicts the next one into the other half of acc_recon. It shares
// the memory port with the MC fetch: it starts a word only when the MC has
// no request waiting, and the MC sees the port busy while a word is out.

reg        pbuf;              // recon buffer B predicts into
reg        w_active, w_buf;
reg  [7:0] w_mbx, w_mby;
reg  [5:0] wbk;               // 0..31 luma words, 32..39 U, 40..47 V
reg        wb_we;
reg [AW-1:0] wb_addr;
reg [63:0] wb_din;
reg  [1:0] lbk;

wire [AW-1:0] slot_base = AW'(cur_slot) << SLOT_L2;
// the RAM is read one word ahead: address of the word to send next clock
wire        wb_adv = w_active && (!wb_we || !mem_busy) && wbk != 48 && !mc_rd;
wire  [6:0] rc_addr = w_active ? {w_buf, wb_adv ? wbk + 1'd1 : wbk} : {pbuf, 6'd0};
wire [63:0] wb_word;
wire [AW-1:0] wb_word_addr =
	wbk < 32 ? slot_base + ((AW'({w_mby, wbk[4:1]})) << YSTRIDE_L2) + {w_mbx, wbk[0]} :
	slot_base + (wbk < 40 ? AW'(UOFF) : AW'(VOFF)) + ((AW'({w_mby, wbk[2:0]})) << CSTRIDE_L2) + w_mbx;

assign mem_rd    = wb_we ? 1'b0 : mc_rd;
assign mem_we    = wb_we;
assign mem_addr  = wb_we ? wb_addr : mc_addr;
assign mem_burst = wb_we ? 8'd1 : mc_burst;
assign mem_din   = wb_din;
assign mem_be    = 8'hFF;

// ------------------------------------------------------------- commands

wire [7:0] tag = cmd_data[31:24];
wire coef_blk_change = coef_have && cmd_data[26:22] != coef_blk;
wire b_free = sb == S_IDLE;                // B can take the next macroblock
wire b_idle = b_free && !w_active;         // and all written
assign cmd_ready = !reset && ((sa == S_CMD && (tag == 8'hA3 || tag == 8'hA0 || b_idle)) || sa == S_PIC || sa == S_REFS || sa == S_MB1 ||
                   sa == S_INFO || sa == S_NCOEF || sa == S_PCM ||
                   (sa == S_COEF && !coef_blk_change && idct_in && !idct_go));
wire take = cmd_valid && cmd_ready;

wire [15:0] X16 = {mbx, 4'd0};

// ------------------------------------------------------ pixel write port
// Every reconstructed pixel goes through here: prediction + residual,
// clipped, into acc_recon; the residual of the group was read the clock
// before (intra: the next group is known in advance; inter: the MC output
// is delayed one clock).

reg        resid_clr;

// Pixel groups, in three stages:
//  0 (q_*)  made this clock: position, and for intra 4x4/8x8 the edge
//           samples of each pixel (or the predicted value, op 0); the
//           residual is read
//  1 (p1_*) the intra 4x4/8x8 prediction is finished (filter of 3 samples)
//  2 (px_*) prediction + residual, clipped, into acc_recon and the edges
reg        q_we;
reg  [1:0] q_pl;
reg  [3:0] q_x, q_y;
reg  [2:0] q_n;
reg  [7:0] q_ea[4], q_eb[4], q_ec[4];
reg  [1:0] q_op[4];
integer qk;
always @* begin
	q_we = 0;
	q_pl = 0;
	q_x = 0;
	q_y = 0;
	q_n = 4;
	for (qk = 0; qk < 4; qk = qk + 1) begin
		q_ea[qk] = e[ia[qk]];
		q_eb[qk] = opr[qk] == 3 ? dcv : e[ib[qk]];
		q_ec[qk] = e[ic[qk]];
		q_op[qk] = opr[qk] == 3 ? 2'd0 : opr[qk];
	end
	case (sb)
		S_I4:  if (ph == 1) begin q_we = 1; q_x = bx; q_y = by + 4'(prow); end
		S_I8:  if (ph == 1) begin q_we = 1; q_x = bx + 4'(pcol); q_y = by + 4'(prow); end
		S_I16, S_CHR: if (ph == 1) begin
			q_we = 1;
			q_pl = sb == S_I16 ? 2'd0 : (cpl ? 2'd2 : 2'd1);
			q_x = sb == S_I16 ? {pgrp, 2'b00} : {1'b0, pgrp[0], 2'b00};
			q_y = prow16;
		end
		default: ;
	endcase
	if (sb == S_I16 || sb == S_CHR)
		for (qk = 0; qk < 4; qk = qk + 1) begin
			q_eb[qk] = bp_pl ? clip8((bp_y + bp_x[qk]) >>> 5) : bp_v[qk];
			q_op[qk] = 0;
		end
	if (mc_px_valid) begin
		q_we = 1;
		q_pl = mc_px_plane;
		q_x = mc_px_x;
		q_y = mc_px_y;
		q_n = mc_px_n;
		for (qk = 0; qk < 4; qk = qk + 1) begin
			q_eb[qk] = mc_px[qk * 8 +: 8];
			q_op[qk] = 0;
		end
	end
end

reg        p1_we;
reg  [1:0] p1_pl;
reg  [3:0] p1_x, p1_y;
reg  [2:0] p1_n;
reg  [7:0] p1_ea[4], p1_eb[4], p1_ec[4];
reg  [1:0] p1_op[4];
reg [31:0] p1_pr;
always @* begin
	for (qk = 0; qk < 4; qk = qk + 1)
		case (p1_op[qk])
			1: p1_pr[qk * 8 +: 8] = (9'(p1_eb[qk]) + p1_ec[qk] + 1) >> 1;
			2: p1_pr[qk * 8 +: 8] = (10'(p1_ea[qk]) + 2 * p1_eb[qk] + p1_ec[qk] + 2) >> 2;
			default: p1_pr[qk * 8 +: 8] = p1_eb[qk];
		endcase
end

reg        px_we;
reg  [1:0] px_pl;
reg  [3:0] px_x, px_y;
reg  [2:0] px_n;
reg [31:0] px_pr, px_d;
wire [39:0] res_q;
integer pk;
always @* begin
	for (pk = 0; pk < 4; pk = pk + 1)
		px_d[pk * 8 +: 8] = clip8($signed({12'd0, px_pr[pk * 8 +: 8]}) + $signed({{10{res_q[pk * 10 + 9]}}, res_q[pk * 10 +: 10]}));
end

// reference tables (list 0, list 1) in block RAM: {slot, lw, lo}, cb, cr
wire       rt_we = sa == S_REFS && cmd_valid;
reg [24:0] rt_a[32], rt1_a[32];
reg [16:0] rt_b[32], rt_c[32], rt1_b[32], rt1_c[32];
reg [24:0] rt_aq, rt1_aq;
reg [16:0] rt_bq, rt_cq, rt1_bq, rt1_cq;
always @(posedge clk) begin
	if (rt_we && !rlist && rword == 0) rt_a[ridx[4:0]] <= cmd_data[24:0];
	if (rt_we && !rlist && rword == 1) rt_b[ridx[4:0]] <= cmd_data[16:0];
	if (rt_we && !rlist && rword == 2) rt_c[ridx[4:0]] <= cmd_data[16:0];
	rt_aq <= rt_a[cmd_data[4:0]];
	rt_bq <= rt_b[cmd_data[4:0]];
	rt_cq <= rt_c[cmd_data[4:0]];
end
always @(posedge clk) begin
	if (rt_we && rlist && rword == 0) rt1_a[ridx[4:0]] <= cmd_data[24:0];
	if (rt_we && rlist && rword == 1) rt1_b[ridx[4:0]] <= cmd_data[16:0];
	if (rt_we && rlist && rword == 2) rt1_c[ridx[4:0]] <= cmd_data[16:0];
	rt1_aq <= rt1_a[cmd_data[11:7]];
	rt1_bq <= rt1_b[cmd_data[11:7]];
	rt1_cq <= rt1_c[cmd_data[11:7]];
end
reg        rt_cap;
reg  [1:0] rt_k;
always @(posedge clk) begin
	rt_cap <= sa == S_INFO && cmd_valid && a_mtype == 0 && icnt < 4;
	rt_k <= icnt[1:0];
	if (rt_cap) begin
		a_q_slot[rt_k] <= rt_aq[7:0];
		a_q_wt[rt_k] <= {rt_aq[16:8], rt_aq[24:17], rt_bq, rt_cq};
		a_q_slot1[rt_k] <= rt1_aq[7:0];
		a_q_wt1[rt_k] <= {rt1_aq[16:8], rt1_aq[24:17], rt1_bq, rt1_cq};
	end
end

acc_resid resid
(
	.clk(clk), .clr(resid_clr), .w_buf(abuf), .r_buf(bbuf),
	.p_en(sa == S_PCM && cmd_valid), .p_plane(pcnt < 64 ? 2'd0 : (pcnt < 80 ? 2'd1 : 2'd2)),
	.p_x(pcnt < 64 ? {pcnt[1:0], 2'b00} : {1'b0, pcnt[0], 2'b00}),
	.p_y(pcnt < 64 ? pcnt[5:2] : {1'b0, pcnt[3:1]}), .p_px(cmd_data),
	.w_en(idct_rv), .w_blk(idct_blk), .w_big(idct_big), .w_col(idct_col),
	.w_vals({idct_res[7], idct_res[6], idct_res[5], idct_res[4], idct_res[3], idct_res[2], idct_res[1], idct_res[0]}),
	.r_plane(q_pl), .r_x(q_x), .r_y(q_y), .r_vals(res_q)
);

acc_recon recon
(
	.clk(clk),
	.w_en(px_we), .w_buf(pbuf), .r_buf(rc_addr[6]), .w_plane(px_pl), .w_x(px_x), .w_y(px_y), .w_n(px_n), .w_px(px_d),
	.r_addr(rc_addr[5:0]), .r_word(wb_word)
);

integer i;
always @(posedge clk) begin
	idct_clr <= reset;
	idct_wr <= 0;
	idct_go <= 0;
	mc_start <= 0;
	pic_done <= 0;

	resid_clr <= 0;

	// pixel groups: residual read this clock, written next clock
	p1_we <= q_we;
	p1_pl <= q_pl;
	p1_x <= q_x;
	p1_y <= q_y;
	p1_n <= q_n;
	for (i = 0; i < 4; i = i + 1) begin
		p1_ea[i] <= q_ea[i];
		p1_eb[i] <= q_eb[i];
		p1_ec[i] <= q_ec[i];
		p1_op[i] <= q_op[i];
	end
	px_we <= p1_we;
	px_pl <= p1_pl;
	px_x <= p1_x;
	px_y <= p1_y;
	px_n <= p1_n;
	px_pr <= p1_pr;

	// intra edges inside the macroblock, from every pixel write
	if (px_we) begin
		if (px_pl == 0) begin
			if (px_y[1:0] == 3) brow[px_y[3:2]][px_x * 8 +: 32] <= px_d;
			rcol[px_x[3:2]][px_y * 8 +: 8] <= px_d[31:24];
		end
		else begin
			if (px_y[2:0] == 7) begin
				if (px_n == 2) begin
					if (px_pl == 1) cbot_u[px_x[2:0] * 8 +: 16] <= px_d[15:0];
					else cbot_v[px_x[2:0] * 8 +: 16] <= px_d[15:0];
				end
				else begin
					if (px_pl == 1) cbot_u[px_x[2:0] * 8 +: 32] <= px_d;
					else cbot_v[px_x[2:0] * 8 +: 32] <= px_d;
				end
			end
			if (px_x[2:0] + px_n == 8) begin
				if (px_pl == 1) crt_u[px_y[2:0] * 8 +: 8] <= px_n == 2 ? px_d[15:8] : px_d[31:24];
				else crt_v[px_y[2:0] * 8 +: 8] <= px_n == 2 ? px_d[15:8] : px_d[31:24];
			end
		end
	end

	// writer
	if (w_active && (!wb_we || !mem_busy)) begin
		if (wbk == 48) begin
			wb_we <= 0;
			w_active <= 0;
		end
		else if (!mc_rd) begin
			wb_we <= 1;
			wb_addr <= wb_word_addr;
			wb_din <= wb_word;
			wbk <= wbk + 1'd1;
		end
		else wb_we <= 0;
	end

	if (reset) begin
		w_active <= 0;
		pbuf <= 0;
		sa <= S_CMD;
		sb <= S_IDLE;
		abuf <= 0;
		error <= 0;
		mb_count <= 0;
		wb_we <= 0;
	end
	else begin
		case (sa)
		S_CMD: if (take) begin
			case (tag)
				8'hA1: begin cur_slot <= cmd_data[7:0]; sa <= S_PIC; end
				8'hA2: begin
					n0 <= cmd_data[5:0];
					n1 <= cmd_data[11:6];
					two_lists <= cmd_data[12];
					use_w <= cmd_data[14:13];
					use_wc <= cmd_data[15];
					lden <= cmd_data[19:16];
					cden <= cmd_data[23:20];
					rcnt <= 8'd3 * (cmd_data[5:0] + (cmd_data[12] ? cmd_data[11:6] : 6'd0));
					rword <= 0;
					ridx <= 0;
					rlist <= 0;
					if (cmd_data[5:0] != 0 || (cmd_data[12] && cmd_data[11:6] != 0)) sa <= S_REFS;
				end
				8'hA3: begin
					a_mbx <= cmd_data[7:0];
					a_mby <= cmd_data[15:8];
					a_mtype <= cmd_data[18:16];
					resid_clr <= 1;
					icnt <= 0;
					pcnt <= 0;
					sa <= cmd_data[18:16] == 4 ? S_PCM : S_MB1;
				end
				8'hA4: begin
					pic_done <= 1;
					pic_slot <= cur_slot;
				end
				8'hA0: ;                     // padding to a 64-bit word: nothing
				default: error <= 1;
			endcase
		end

		S_PIC: if (cmd_valid) begin             // always ready here
			wmb <= cmd_data[7:0];
			hmb <= cmd_data[15:8];
			sa <= S_CMD;
		end

		S_REFS: if (cmd_valid) begin
			if (!rlist) begin
			end
			if (rword == 2) begin
				rword <= 0;
				if (!rlist && ridx + 1'd1 == n0) begin rlist <= 1; ridx <= 0; end
				else ridx <= ridx + 1'd1;
			end
			else rword <= rword + 1'd1;
			rcnt <= rcnt - 1'd1;
			if (rcnt == 1) sa <= S_CMD;
		end

		S_MB1: if (cmd_valid) begin
			cmask <= cmd_data[23:0];
			t8 <= cmd_data[24];
			sa <= S_INFO;
		end

		S_INFO: if (cmd_valid) begin
			icnt <= icnt + 1'd1;
			if (a_mtype != 0) begin
				case (icnt)
					0: for (i = 0; i < 8; i = i + 1) a_imode[i] <= cmd_data[i * 4 +: 4];
					1: for (i = 0; i < 8; i = i + 1) a_imode[8 + i] <= cmd_data[i * 4 +: 4];
					2: begin a_tr <= cmd_data[15:0]; a_tl <= cmd_data[31:16]; end
					default: begin a_cmode <= cmd_data[3:0]; a_i16mode <= cmd_data[7:4]; end
				endcase
				if (icnt == 3) sa <= S_NCOEF;
			end
			else begin
				if (icnt < 4) begin
					a_qref[icnt[1:0]] <= cmd_data[13:0];
					a_iw[icnt[1:0]] <= cmd_data[24:16];
				end
				else if (icnt < 20) a_mv[icnt - 4] <= cmd_data;
				else a_mv1[icnt - 20] <= cmd_data;
				if (icnt == 35) sa <= S_NCOEF;
			end
		end

		S_NCOEF: if (cmd_valid) begin
			ncoef <= cmd_data[15:0];
			coef_have <= 0;
			sa <= cmd_data[15:0] == 0 ? S_HAND : S_COEF;
			ctxk <= 0;
		end

		// coefficients of the next block go in as soon as the transform of the
		// previous one has read them (its row pass)
		S_COEF: begin
			if (coef_blk_change && cmd_valid) begin
				// the previous block is complete: transform it
				if (!idct_busy && !idct_go) begin
					idct_go <= 1;
					idct_blk <= coef_blk;
					idct_big <= coef_blk < 16 && t8;
					coef_have <= 0;
				end
			end
			else if (take) begin
				idct_wr <= 1;
				idct_pos <= cmd_data[21:16];
				idct_val <= cmd_data[15:0];
				coef_have <= 1;
				coef_blk <= cmd_data[26:22];
				ncoef <= ncoef - 1'd1;
				if (ncoef == 1) begin
					sa <= S_IDCTW;
					coef_last <= 1;
					go_pending <= 1;
				end
			end
		end

		S_IDCTW: begin
			if (go_pending) begin
				// last block: its last coefficient is written this clock
				if (!idct_busy && !idct_go) begin
					go_pending <= 0;
					idct_go <= 1;
					idct_blk <= coef_blk;
					idct_big <= coef_blk < 16 && t8;
				end
			end
			else if (!idct_go && !idct_busy && !idct_rv) begin
				coef_have <= 0;
				sa <= coef_last ? S_HAND : S_COEF;
				ctxk <= 0;
			end
		end

		// row above and corner from the line buffers
		S_PCM: if (cmd_valid) begin
			pcnt <= pcnt + 1'd1;
			if (pcnt == 95) begin sa <= S_HAND; ctxk <= 0; end
		end

		// macroblock to memory, one word per write
		// hand the macroblock to B once B is idle and the transform is done
		S_HAND: if (b_free && !idct_busy && !idct_go && !idct_rv) begin
			mbx <= a_mbx;
			mby <= a_mby;
			mtype <= a_mtype;
			for (i = 0; i < 16; i = i + 1) imode[i] <= a_imode[i];
			tr_mask <= a_tr;
			tl_mask <= a_tl;
			cmode <= a_cmode;
			i16mode <= a_i16mode;
			for (i = 0; i < 4; i = i + 1) begin
				qref[i] <= a_qref[i];
				iw[i] <= a_iw[i];
				q_slot[i] <= a_q_slot[i];
				q_wt[i] <= a_q_wt[i];
				q_slot1[i] <= a_q_slot1[i];
				q_wt1[i] <= a_q_wt1[i];
			end
			for (i = 0; i < 16; i = i + 1) begin
				mv[i] <= a_mv[i];
				mv1[i] <= a_mv1[i];
			end
			bbuf <= abuf;
			abuf <= ~abuf;
			sb <= S_CTX;
			ctxk <= 0;
			sa <= S_CMD;
		end

		default: sa <= S_CMD;
		endcase

		case (sb)
		S_CTX: begin
			ctxk <= ctxk + 1'd1;
			lby_ra <= {mbx, 2'b00} + ctxk;
			lbc_ra <= {mbx, 1'b0} + ctxk[0];
			// read data comes two clocks after the address
			if (ctxk >= 2 && ctxk <= 7)
				for (i = 0; i < 4; i = i + 1) topy[(ctxk - 2) * 4 + i] <= lby_q[i * 8 +: 8];
			if (ctxk >= 2 && ctxk <= 3)
				for (i = 0; i < 4; i = i + 1) begin
					topu[(ctxk - 2) * 4 + i] <= lbu_q[i * 8 +: 8];
					topv[(ctxk - 2) * 4 + i] <= lbv_q[i * 8 +: 8];
				end
			if (ctxk == 7) begin
				blk <= 0;
				ph <= 0;
				case (mtype)
					0: begin sb <= S_MC; mc_start <= 1; end
					1: sb <= S_I4;
					2: sb <= S_I8;
					3: begin sb <= S_I16; ph <= 0; end
					default: begin sb <= S_I16; ph <= 0; end
				endcase
			end
		end

		// intra 4x4: edge, then 4 rows
		S_I4: begin
			if (ph == 0) begin
				// the previous block is written: take the raw edge
				if (!p1_we && !px_we) begin
					for (i = 0; i < 25; i = i + 1) er[i] <= eraw[i];
					mode_r <= imode[blk];
					ph <= 5;
				end
			end
			else if (ph == 5) begin
				for (i = 0; i < 25; i = i + 1) e[i] <= er[i];
				if (!tr_mask[15 - blk]) for (i = 0; i < 4; i = i + 1) e[13 + i] <= er[12];
				ph <= blk_dc ? 5'd2 : 5'd1;
				prow <= 0;
				pcol <= 0;
			end
			else if (ph == 2) ph <= 6;       // DC: sums, then value
			else if (ph == 6) ph <= 1;
			else begin
				prow <= prow + 1'd1;
				if (prow == 3) begin
					ph <= 0;
					blk <= blk + 1'd1;
					if (blk == 15) begin sb <= S_CHR; cpl <= 0; ph <= 0; end
				end
			end
		end

		// intra 8x8: edge, filter, then 8 rows of 2 groups
		S_I8: begin
			if (ph == 0) begin
				if (!p1_we && !px_we) begin
					for (i = 0; i < 25; i = i + 1) er[i] <= eraw[i];
					mode_r <= imode[blk];
					ph <= 5;
				end
			end
			else if (ph == 5) begin
				for (i = 0; i < 25; i = i + 1) e[i] <= efil[i];
				ph <= blk_dc ? 5'd2 : 5'd1;
				prow <= 0;
				pcol <= 0;
			end
			else if (ph == 2) ph <= 6;
			else if (ph == 6) ph <= 1;
			else begin
				pcol <= pcol ^ 3'd4;
				if (pcol == 4) begin
					prow <= prow + 1'd1;
					if (prow == 7) begin
						ph <= 0;
						blk <= blk + 4'd4;
						if (blk == 12) begin sb <= S_CHR; cpl <= 0; ph <= 0; end
					end
				end
			end
		end

		// intra 16x16: sums, plane parameters, first group, then 16 rows of 4 groups
		S_I16: begin
			if (ph == 0) begin
				hA <= hsum;
				hB <= hsum2;
				vA <= vsum;
				vB <= vsum2;
				stop_r <= stop;
				sleft_r <= sleft;
				ph <= 4;
			end
			else if (ph == 4) begin
				hs_r <= hA + hB;
				vs_r <= vA + vB;
				ph <= 2;
			end
			else if (ph == 2) begin
				pa <= 16 * (lefty[15] + topy[15]);
				pb <= (5 * hs_r + 32) >>> 6;
				pc <= (5 * vs_r + 32) >>> 6;
				case (i16mode)
					4: dc16 <= (sleft_r + 8) >> 4;
					5: dc16 <= (stop_r + 8) >> 4;
					6: dc16 <= 128;
					default: dc16 <= (stop_r + sleft_r + 16) >> 5;
				endcase
				ph <= 3;
			end
			else if (ph == 3) begin
				ph <= 1;
				prow16 <= 0;
				pgrp <= 0;
			end
			else begin
				pgrp <= pgrp + 1'd1;
				if (pgrp == 3) begin
					prow16 <= prow16 + 1'd1;
					if (prow16 == 15) begin sb <= S_CHR; cpl <= 0; ph <= 0; end
				end
			end
		end

		// chroma intra: per plane sums, parameters, first group, then 8 rows of 2 groups
		S_CHR: begin
			if (ph == 0) begin
				hA <= hsum;
				hB <= hsum2;
				vA <= vsum;
				vB <= vsum2;
				sqt0_r <= sq_t0;
				sqt1_r <= sq_t1;
				sql0_r <= sq_l0;
				sql1_r <= sq_l1;
				ph <= 4;
			end
			else if (ph == 4) begin
				hs_r <= hA + hB;
				vs_r <= vA + vB;
				ph <= 2;
			end
			else if (ph == 2) begin
				pa <= 16 * ((cpl ? leftv[7] : leftu[7]) + (cpl ? topv[7] : topu[7]));
				pb <= (34 * hs_r + 32) >>> 6;
				pc <= (34 * vs_r + 32) >>> 6;
				case (cmode)
					4: begin dcq[0] <= (sql0_r + 2) >> 2; dcq[1] <= (sql0_r + 2) >> 2; dcq[2] <= (sql1_r + 2) >> 2; dcq[3] <= (sql1_r + 2) >> 2; end
					5: begin dcq[0] <= (sqt0_r + 2) >> 2; dcq[1] <= (sqt1_r + 2) >> 2; dcq[2] <= (sqt0_r + 2) >> 2; dcq[3] <= (sqt1_r + 2) >> 2; end
					6: begin dcq[0] <= 128; dcq[1] <= 128; dcq[2] <= 128; dcq[3] <= 128; end
					default: begin
						dcq[0] <= (sqt0_r + sql0_r + 4) >> 3;
						dcq[1] <= (sqt1_r + 2) >> 2;
						dcq[2] <= (sql1_r + 2) >> 2;
						dcq[3] <= (sqt1_r + sql1_r + 4) >> 3;
					end
				endcase
				ph <= 3;
			end
			else if (ph == 3) begin
				ph <= 1;
				prow16 <= 0;
				pgrp <= 0;
			end
			else begin
				pgrp[0] <= ~pgrp[0];
				if (pgrp[0]) begin
					prow16 <= prow16 + 1'd1;
					if (prow16 == 7) begin
						ph <= 0;
						cpl <= 1;
						if (cpl) begin sb <= S_LB; lbk <= 0; end
					end
				end
			end
		end

		S_MC: if (mc_done) begin sb <= S_LB; lbk <= 0; end

		// line buffers, left column and corner for the next macroblock, then
		// the macroblock goes to the writer
		S_LB: if (!p1_we && !px_we) begin
			lby[{mbx, lbk}] <= brow[3][lbk * 32 +: 32];
			if (lbk < 2) begin
				lbu[{mbx, lbk[0]}] <= cbot_u[lbk[0] * 32 +: 32];
				lbv[{mbx, lbk[0]}] <= cbot_v[lbk[0] * 32 +: 32];
			end
			if (lbk != 3) lbk <= lbk + 1'd1;
			else if (!w_active) begin
				for (i = 0; i < 16; i = i + 1) lefty[i] <= rcol[3][i * 8 +: 8];
				for (i = 0; i < 8; i = i + 1) begin leftu[i] <= crt_u[i * 8 +: 8]; leftv[i] <= crt_v[i * 8 +: 8]; end
				cornery <= topy[15];
				corneru <= topu[7];
				cornerv <= topv[7];
				mb_count <= mb_count + 1'd1;
				w_active <= 1;
				w_buf <= pbuf;
				w_mbx <= mbx;
				w_mby <= mby;
				wbk <= 0;
				pbuf <= ~pbuf;
				sb <= S_IDLE;
			end
		end

		S_IDLE: ;                     // started by A (S_HAND)
		default: sb <= S_IDLE;
		endcase
	end
end

endmodule
