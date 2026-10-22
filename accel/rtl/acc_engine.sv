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
           S_WB = 14, S_LB = 15, S_PCM = 16, S_DONE = 17;

reg  [4:0] st;

reg  [7:0] wmb, hmb;          // picture size in macroblocks
reg  [7:0] cur_slot;
reg  [7:0] mbx, mby;
reg  [2:0] mtype;             // 0 inter, 1 I4x4, 2 I8x8, 3 I16x16, 4 PCM
reg [23:0] cmask;
reg        t8;
reg  [3:0] imode[16];
reg [15:0] tr_mask, tl_mask;
reg  [3:0] cmode, i16mode;
reg  [6:0] qref[4];           // reference of each 8x8 (list 0), bit 6 = unused
reg [31:0] mv[16];

// reference list 0 (list 1 kept for B pictures, phase 2)
reg  [7:0] ref_slot[32];
reg  [8:0] ref_lw[32];
reg  [7:0] ref_lo[32];
reg [16:0] ref_cb[32], ref_cr[32];
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

reg  [7:0] cy[256];           // reconstructed macroblock
reg  [7:0] cu[64], cv[64];
reg signed [15:0] ry[256];    // residual
reg signed [15:0] ru[64], rv[64];

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
wire       idct_busy, idct_rv;
wire [2:0] idct_col;
wire signed [15:0] idct_res[8];

acc_idct idct
(
	.clk(clk), .clr(idct_clr), .t8(idct_big), .wr(idct_wr), .wr_pos(idct_pos), .wr_val(idct_val),
	.go(idct_go), .busy(idct_busy), .res_valid(idct_rv), .res_col(idct_col), .res(idct_res)
);

// residual position of IDCT output column idct_col, row i
function [7:0] res_index(input [4:0] b, input big, input [2:0] col, input [2:0] row);
	reg [3:0] x, y;
	begin
		if (b < 16 && big) begin
			x = {b[2], col};
			y = {b[3], row};
		end
		else if (b < 16) begin
			x = {b[2], b[0], col[1:0]};
			y = {b[3], b[1], row[1:0]};
		end
		else begin
			x = {1'b0, b[0], col[1:0]};
			y = {1'b0, b[1], row[1:0]};
		end
		res_index = b < 16 ? {y, x} : {2'b00, y[2:0], x[2:0]};
	end
endfunction

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
		eraw[9 + ii] = byq == 0 ? topy[bx + ii] : (bx + ii < 16 ? cy[{by - 4'd1, 4'(bx + ii)}] : 8'd0);
	for (ii = 0; ii < 8; ii = ii + 1)
		eraw[7 - ii] = bxq == 0 ? lefty[by + ii] : cy[{4'(by + ii), bx - 4'd1}];
	eraw[8] = byq == 0 ? (bxq == 0 ? cornery : topy[bx - 1]) : (bxq == 0 ? lefty[by - 1] : cy[{by - 4'd1, bx - 4'd1}]);
end

// 8x8 reference filtering (FFmpeg PREDICT_8x8_LOAD_*)
wire has_tl8 = tl_mask[15 - blk];
wire has_tr8 = tr_mask[14 - blk];
reg  [7:0] efil[25];
always @* begin
	efil[9] = ((has_tl8 ? eraw[8] : eraw[9]) + 2 * eraw[9] + eraw[10] + 2) >> 2;
	for (ii = 1; ii < 7; ii = ii + 1)
		efil[9 + ii] = (eraw[8 + ii] + 2 * eraw[9 + ii] + eraw[10 + ii] + 2) >> 2;
	efil[16] = ((has_tr8 ? eraw[17] : eraw[16]) + 2 * eraw[16] + eraw[15] + 2) >> 2;
	for (ii = 8; ii < 15; ii = ii + 1)
		efil[9 + ii] = has_tr8 ? 8'((eraw[8 + ii] + 2 * eraw[9 + ii] + eraw[10 + ii] + 2) >> 2) : eraw[16];
	efil[24] = has_tr8 ? 8'((eraw[23] + 3 * eraw[24] + 2) >> 2) : eraw[16];
	// left: e[7] = l0 ... e[0] = l7
	efil[7] = ((has_tl8 ? eraw[8] : eraw[7]) + 2 * eraw[7] + eraw[6] + 2) >> 2;
	for (ii = 1; ii < 7; ii = ii + 1)
		efil[7 - ii] = (eraw[8 - ii] + 2 * eraw[7 - ii] + eraw[6 - ii] + 2) >> 2;
	efil[0] = (eraw[1] + 3 * eraw[0] + 2) >> 2;
	efil[8] = (eraw[7] + 2 * eraw[8] + eraw[9] + 2) >> 2;
end

// DC of the block from the (filtered) edge
reg  [7:0] dcv;
reg [11:0] st4, sl4, st8, sl8;
always @* begin
	st4 = e[9] + e[10] + e[11] + e[12];
	sl4 = e[7] + e[6] + e[5] + e[4];
	st8 = st4 + e[13] + e[14] + e[15] + e[16];
	sl8 = sl4 + e[3] + e[2] + e[1] + e[0];
	if (mtype == 1)
		case (imode[blk])
			9:       dcv = (sl4 + 2) >> 2;
			10:      dcv = (st4 + 2) >> 2;
			11:      dcv = 128;
			default: dcv = (st4 + sl4 + 4) >> 3;
		endcase
	else
		case (imode[blk])
			9:       dcv = (sl8 + 4) >> 3;
			10:      dcv = (st8 + 4) >> 3;
			11:      dcv = 128;
			default: dcv = (st8 + sl8 + 8) >> 4;
		endcase
end

// four predicted pixels of row prow, columns pcol..pcol+3 of the block
reg  [2:0] prow, pcol;
reg  [7:0] dpred[4];
integer pp, xx, yy, z, idx, nn, op, kk;
// edge sample i (-n .. 2n, clamped), read directly: no function here, so
// that the simulator sees e[] in the sensitivity of the block
`define ECL(i) e[8 + ((i) < -nn ? -nn : ((i) > 2 * nn ? 2 * nn : (i)))]
always @* begin
	nn = mtype == 1 ? 4 : 8;
	for (pp = 0; pp < 4; pp = pp + 1) begin
		xx = pcol + pp;
		yy = prow;
		op = 0;
		idx = 0;
		case (imode[blk])
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
		case (op)
			0: dpred[pp] = `ECL(idx);
			1: dpred[pp] = (`ECL(idx) + `ECL(idx + 1) + 1) >> 1;
			2: dpred[pp] = (`ECL(idx - 1) + 2 * `ECL(idx) + `ECL(idx + 1) + 2) >> 2;
			default: dpred[pp] = dcv;
		endcase
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
reg signed [19:0] hsum, vsum;
reg [12:0] stop, sleft;
reg [10:0] sq_t0, sq_t1, sq_l0, sq_l1;
integer q;
always @* begin
	hsum = 0;
	vsum = 0;
	if (st == S_I16) begin
		for (q = 0; q < 8; q = q + 1) begin
			hsum = hsum + (q + 1) * ($signed({1'b0, topy[8 + q]}) - $signed({1'b0, (q == 7 ? cornery : topy[6 - q])}));
			vsum = vsum + (q + 1) * ($signed({1'b0, lefty[8 + q]}) - $signed({1'b0, (q == 7 ? cornery : lefty[6 - q])}));
		end
	end
	else begin
		for (q = 0; q < 4; q = q + 1) begin
			hsum = hsum + (q + 1) * ($signed({1'b0, (cpl ? topv[4 + q] : topu[4 + q])}) -
			                         $signed({1'b0, (q == 3 ? (cpl ? cornerv : corneru) : (cpl ? topv[2 - q] : topu[2 - q]))}));
			vsum = vsum + (q + 1) * ($signed({1'b0, (cpl ? leftv[4 + q] : leftu[4 + q])}) -
			                         $signed({1'b0, (q == 3 ? (cpl ? cornerv : corneru) : (cpl ? leftv[2 - q] : leftu[2 - q]))}));
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

// four predicted pixels of the 16x16 / chroma block, row prow16, group pgrp
reg  [7:0] bpred[4];
reg signed [19:0] pv;
integer bxx, byy;
always @* begin
	byy = prow16;
	for (pp = 0; pp < 4; pp = pp + 1) begin
		bxx = pgrp * 4 + pp;
		if (st == S_I16) begin
			pv = (pa + pb * (bxx - 7) + pc * (byy - 7) + 16) >>> 5;
			case (i16mode)
				1: bpred[pp] = lefty[prow16];
				2: bpred[pp] = topy[bxx];
				3: bpred[pp] = clip8(pv);
				default: bpred[pp] = dc16;
			endcase
		end
		else begin
			pv = (pa + pb * (bxx - 3) + pc * (byy - 3) + 16) >>> 5;
			case (cmode)
				1: bpred[pp] = cpl ? leftv[prow16] : leftu[prow16];
				2: bpred[pp] = cpl ? topv[bxx] : topu[bxx];
				3: bpred[pp] = clip8(pv);
				default: bpred[pp] = dcq[{prow16[2], pgrp[0]}];
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

// per 8x8 quadrant: slot and weights of its reference
wire [7:0]  q_slot[4];
wire [50:0] q_wt[4];
genvar gq;
generate
	for (gq = 0; gq < 4; gq = gq + 1) begin : qw
		assign q_slot[gq] = ref_slot[qref[gq][4:0]];
		assign q_wt[gq] = {ref_lw[qref[gq][4:0]], ref_lo[qref[gq][4:0]], ref_cb[qref[gq][4:0]], ref_cr[qref[gq][4:0]]};
	end
endgenerate

wire [511:0] mv_flat;
generate
	for (gq = 0; gq < 16; gq = gq + 1) begin : mvf
		assign mv_flat[gq * 32 +: 32] = mv[gq];
	end
endgenerate

acc_mc #(.AW(AW), .SLOT_L2(SLOT_L2), .YSTRIDE_L2(YSTRIDE_L2), .CSTRIDE_L2(CSTRIDE_L2), .UOFF(UOFF), .VOFF(VOFF)) mc
(
	.clk(clk),
	.start(mc_start),
	.mbx(mbx), .mby(mby), .wmb(wmb), .hmb(hmb),
	.mvs(mv_flat),
	.qref({qref[3][5:0], qref[2][5:0], qref[1][5:0], qref[0][5:0]}),
	.qslot({q_slot[3], q_slot[2], q_slot[1], q_slot[0]}),
	.qwt({q_wt[3], q_wt[2], q_wt[1], q_wt[0]}),
	.use_w(use_w[0]), .use_wc(use_wc), .lden(lden), .cden(cden),
	.mem_rd(mc_rd), .mem_addr(mc_addr), .mem_burst(mc_burst), .mem_busy(mem_busy),
	.mem_dout(mem_dout), .mem_dout_ready(mem_dout_ready),
	.px_valid(mc_px_valid), .px_plane(mc_px_plane), .px_x(mc_px_x), .px_y(mc_px_y), .px_n(mc_px_n), .px(mc_px),
	.done(mc_done)
);

// --------------------------------------------------------- write back

reg  [5:0] wbk;               // 0..31 luma words, 32..39 U, 40..47 V
reg        wb_we;
reg [AW-1:0] wb_addr;
reg [63:0] wb_din;

wire [AW-1:0] slot_base = AW'(cur_slot) << SLOT_L2;
integer bb;
reg [63:0] wb_word;
always @* begin
	wb_word = 0;
	for (bb = 0; bb < 8; bb = bb + 1)
		if (wbk < 32) wb_word[bb * 8 +: 8] = cy[{wbk[4:1], wbk[0], 3'(bb)}];
		else if (wbk < 40) wb_word[bb * 8 +: 8] = cu[{wbk[2:0], 3'(bb)}];
		else wb_word[bb * 8 +: 8] = cv[{wbk[2:0], 3'(bb)}];
end
wire [AW-1:0] wb_word_addr =
	wbk < 32 ? slot_base + ((AW'({mby, wbk[4:1]})) << YSTRIDE_L2) + {mbx, wbk[0]} :
	slot_base + (wbk < 40 ? AW'(UOFF) : AW'(VOFF)) + ((AW'({mby, wbk[2:0]})) << CSTRIDE_L2) + mbx;

assign mem_rd    = st == S_MC ? mc_rd : 1'b0;
assign mem_we    = st == S_MC ? 1'b0 : wb_we;
assign mem_addr  = st == S_MC ? mc_addr : wb_addr;
assign mem_burst = st == S_MC ? mc_burst : 8'd1;
assign mem_din   = wb_din;
assign mem_be    = 8'hFF;

// ------------------------------------------------------------- commands

wire [7:0] tag = cmd_data[31:24];
wire coef_blk_change = coef_have && cmd_data[26:22] != coef_blk;
assign cmd_ready = !reset && (st == S_CMD || st == S_PIC || st == S_REFS || st == S_MB1 || st == S_INFO ||
                   st == S_NCOEF || st == S_PCM || (st == S_COEF && !coef_blk_change));
wire take = cmd_valid && cmd_ready;

wire [15:0] X16 = {mbx, 4'd0};

integer i;
always @(posedge clk) begin
	idct_clr <= 0;
	idct_wr <= 0;
	idct_go <= 0;
	mc_start <= 0;
	pic_done <= 0;

	// IDCT results into the residual
	if (idct_rv)
		for (i = 0; i < 8; i = i + 1)
			if (i < (idct_big ? 8 : 4)) begin
				if (idct_blk < 16) ry[res_index(idct_blk, idct_big, idct_col, 3'(i))] <= idct_res[i];
				else if (idct_blk < 20) ru[res_index(idct_blk, idct_big, idct_col, 3'(i))] <= idct_res[i];
				else rv[res_index(idct_blk, idct_big, idct_col, 3'(i))] <= idct_res[i];
			end

	// inter prediction + residual
	if (mc_px_valid)
		for (i = 0; i < 4; i = i + 1)
			if (i < mc_px_n) begin
				if (mc_px_plane == 0)
					cy[{mc_px_y, 4'(mc_px_x + i)}] <= clip8($signed({12'd0, mc_px[i * 8 +: 8]}) + ry[{mc_px_y, 4'(mc_px_x + i)}]);
				else if (mc_px_plane == 1)
					cu[{mc_px_y[2:0], 3'(mc_px_x + i)}] <= clip8($signed({12'd0, mc_px[i * 8 +: 8]}) + ru[{mc_px_y[2:0], 3'(mc_px_x + i)}]);
				else
					cv[{mc_px_y[2:0], 3'(mc_px_x + i)}] <= clip8($signed({12'd0, mc_px[i * 8 +: 8]}) + rv[{mc_px_y[2:0], 3'(mc_px_x + i)}]);
			end

	if (reset) begin
		st <= S_CMD;
		error <= 0;
		mb_count <= 0;
		wb_we <= 0;
	end
	else case (st)
		S_CMD: if (take) begin
			case (tag)
				8'hA1: begin cur_slot <= cmd_data[7:0]; st <= S_PIC; end
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
					if (cmd_data[5:0] != 0 || (cmd_data[12] && cmd_data[11:6] != 0)) st <= S_REFS;
				end
				8'hA3: begin
					mbx <= cmd_data[7:0];
					mby <= cmd_data[15:8];
					mtype <= cmd_data[18:16];
					for (i = 0; i < 256; i = i + 1) ry[i] <= 0;
					for (i = 0; i < 64; i = i + 1) begin ru[i] <= 0; rv[i] <= 0; end
					icnt <= 0;
					pcnt <= 0;
					st <= cmd_data[18:16] == 4 ? S_PCM : S_MB1;
				end
				8'hA4: begin
					pic_done <= 1;
					pic_slot <= cur_slot;
				end
				default: error <= 1;
			endcase
		end

		S_PIC: if (take) begin
			wmb <= cmd_data[7:0];
			hmb <= cmd_data[15:8];
			st <= S_CMD;
		end

		S_REFS: if (take) begin
			if (!rlist) begin
				case (rword)
					0: begin ref_slot[ridx[4:0]] <= cmd_data[7:0]; ref_lw[ridx[4:0]] <= cmd_data[16:8]; ref_lo[ridx[4:0]] <= cmd_data[24:17]; end
					1: ref_cb[ridx[4:0]] <= cmd_data[16:0];
					default: ref_cr[ridx[4:0]] <= cmd_data[16:0];
				endcase
			end
			if (rword == 2) begin
				rword <= 0;
				if (!rlist && ridx + 1'd1 == n0) begin rlist <= 1; ridx <= 0; end
				else ridx <= ridx + 1'd1;
			end
			else rword <= rword + 1'd1;
			rcnt <= rcnt - 1'd1;
			if (rcnt == 1) st <= S_CMD;
		end

		S_MB1: if (take) begin
			cmask <= cmd_data[23:0];
			t8 <= cmd_data[24];
			st <= S_INFO;
		end

		S_INFO: if (take) begin
			icnt <= icnt + 1'd1;
			if (mtype != 0) begin
				case (icnt)
					0: for (i = 0; i < 8; i = i + 1) imode[i] <= cmd_data[i * 4 +: 4];
					1: for (i = 0; i < 8; i = i + 1) imode[8 + i] <= cmd_data[i * 4 +: 4];
					2: begin tr_mask <= cmd_data[15:0]; tl_mask <= cmd_data[31:16]; end
					default: begin cmode <= cmd_data[3:0]; i16mode <= cmd_data[7:4]; end
				endcase
				if (icnt == 3) st <= S_NCOEF;
			end
			else begin
				if (icnt < 4) qref[icnt[1:0]] <= cmd_data[6:0];
				else if (icnt < 20) mv[icnt - 4] <= cmd_data;
				if (icnt == 35) st <= S_NCOEF;
			end
		end

		S_NCOEF: if (take) begin
			ncoef <= cmd_data[15:0];
			coef_have <= 0;
			st <= cmd_data[15:0] == 0 ? S_CTX : S_COEF;
			ctxk <= 0;
		end

		S_COEF: begin
			if (coef_blk_change && cmd_valid) begin
				// the previous block is complete: transform it
				idct_go <= 1;
				idct_blk <= coef_blk;
				idct_big <= coef_blk < 16 && t8;
				coef_last <= 0;
				st <= S_IDCTW;
			end
			else if (take) begin
				idct_clr <= !coef_have;
				idct_wr <= 1;
				idct_pos <= cmd_data[21:16];
				idct_val <= cmd_data[15:0];
				coef_have <= 1;
				coef_blk <= cmd_data[26:22];
				ncoef <= ncoef - 1'd1;
				if (ncoef == 1) begin
					st <= S_IDCTW;
					coef_last <= 1;
					go_pending <= 1;
				end
			end
		end

		S_IDCTW: begin
			if (go_pending) begin
				// last block: its last coefficient is written this clock
				go_pending <= 0;
				idct_go <= 1;
				idct_blk <= coef_blk;
				idct_big <= coef_blk < 16 && t8;
			end
			else if (!idct_go && !idct_busy && !idct_rv) begin
				coef_have <= 0;
				st <= coef_last ? S_CTX : S_COEF;
				ctxk <= 0;
			end
		end

		// row above and corner from the line buffers
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
					0: begin st <= S_MC; mc_start <= 1; end
					1: st <= S_I4;
					2: st <= S_I8;
					3: begin st <= S_I16; ph <= 0; end
					default: begin st <= S_WB; wbk <= 0; end
				endcase
			end
		end

		// intra 4x4: edge, then 4 rows
		S_I4: begin
			if (ph == 0) begin
				for (i = 0; i < 25; i = i + 1) e[i] <= eraw[i];
				if (!tr_mask[15 - blk]) for (i = 0; i < 4; i = i + 1) e[13 + i] <= eraw[12];
				ph <= 1;
				prow <= 0;
				pcol <= 0;
			end
			else begin
				for (i = 0; i < 4; i = i + 1)
					cy[{by + 4'(prow), bx + 4'(i)}] <= clip8($signed({12'd0, dpred[i]}) + ry[{by + 4'(prow), bx + 4'(i)}]);
				prow <= prow + 1'd1;
				if (prow == 3) begin
					ph <= 0;
					blk <= blk + 1'd1;
					if (blk == 15) begin st <= S_CHR; cpl <= 0; ph <= 0; end
				end
			end
		end

		// intra 8x8: edge, filter, then 8 rows of 2 groups
		S_I8: begin
			if (ph == 0) begin
				for (i = 0; i < 25; i = i + 1) e[i] <= efil[i];
				ph <= 1;
				prow <= 0;
				pcol <= 0;
			end
			else begin
				for (i = 0; i < 4; i = i + 1)
					cy[{by + 4'(prow), bx + 4'(pcol) + 4'(i)}] <= clip8($signed({12'd0, dpred[i]}) + ry[{by + 4'(prow), bx + 4'(pcol) + 4'(i)}]);
				pcol <= pcol ^ 3'd4;
				if (pcol == 4) begin
					prow <= prow + 1'd1;
					if (prow == 7) begin
						ph <= 0;
						blk <= blk + 4'd4;
						if (blk == 12) begin st <= S_CHR; cpl <= 0; ph <= 0; end
					end
				end
			end
		end

		// intra 16x16: setup, then 16 rows of 4 groups
		S_I16: begin
			if (ph == 0) begin
				pa <= 16 * (lefty[15] + topy[15]);
				pb <= (5 * hsum + 32) >>> 6;
				pc <= (5 * vsum + 32) >>> 6;
				case (i16mode)
					4: dc16 <= (sleft + 8) >> 4;
					5: dc16 <= (stop + 8) >> 4;
					6: dc16 <= 128;
					default: dc16 <= (stop + sleft + 16) >> 5;
				endcase
				ph <= 1;
				prow16 <= 0;
				pgrp <= 0;
			end
			else begin
				for (i = 0; i < 4; i = i + 1)
					cy[{prow16, 4'({pgrp, 2'b00}) + 4'(i)}] <= clip8($signed({12'd0, bpred[i]}) + ry[{prow16, 4'({pgrp, 2'b00}) + 4'(i)}]);
				pgrp <= pgrp + 1'd1;
				if (pgrp == 3) begin
					prow16 <= prow16 + 1'd1;
					if (prow16 == 15) begin st <= S_CHR; cpl <= 0; ph <= 0; end
				end
			end
		end

		// chroma intra: per plane setup, then 8 rows of 2 groups
		S_CHR: begin
			if (ph == 0) begin
				pa <= 16 * ((cpl ? leftv[7] : leftu[7]) + (cpl ? topv[7] : topu[7]));
				pb <= (34 * hsum + 32) >>> 6;
				pc <= (34 * vsum + 32) >>> 6;
				case (cmode)
					4: begin dcq[0] <= (sq_l0 + 2) >> 2; dcq[1] <= (sq_l0 + 2) >> 2; dcq[2] <= (sq_l1 + 2) >> 2; dcq[3] <= (sq_l1 + 2) >> 2; end
					5: begin dcq[0] <= (sq_t0 + 2) >> 2; dcq[1] <= (sq_t1 + 2) >> 2; dcq[2] <= (sq_t0 + 2) >> 2; dcq[3] <= (sq_t1 + 2) >> 2; end
					6: begin dcq[0] <= 128; dcq[1] <= 128; dcq[2] <= 128; dcq[3] <= 128; end
					default: begin
						dcq[0] <= (sq_t0 + sq_l0 + 4) >> 3;
						dcq[1] <= (sq_t1 + 2) >> 2;
						dcq[2] <= (sq_l1 + 2) >> 2;
						dcq[3] <= (sq_t1 + sq_l1 + 4) >> 3;
					end
				endcase
				ph <= 1;
				prow16 <= 0;
				pgrp <= 0;
			end
			else begin
				for (i = 0; i < 4; i = i + 1)
					if (!cpl) cu[{prow16[2:0], pgrp[0], 2'(i)}] <= clip8($signed({12'd0, bpred[i]}) + ru[{prow16[2:0], pgrp[0], 2'(i)}]);
					else      cv[{prow16[2:0], pgrp[0], 2'(i)}] <= clip8($signed({12'd0, bpred[i]}) + rv[{prow16[2:0], pgrp[0], 2'(i)}]);
				pgrp[0] <= ~pgrp[0];
				if (pgrp[0]) begin
					prow16 <= prow16 + 1'd1;
					if (prow16 == 7) begin
						ph <= 0;
						cpl <= 1;
						if (cpl) begin st <= S_WB; wbk <= 0; end
					end
				end
			end
		end

		S_MC: if (mc_done) begin st <= S_WB; wbk <= 0; end

		S_PCM: if (take) begin
			for (i = 0; i < 4; i = i + 1) begin
				if (pcnt < 64) cy[{pcnt[5:0], 2'(i)}] <= cmd_data[i * 8 +: 8];
				else if (pcnt < 80) cu[{pcnt[3:0], 2'(i)}] <= cmd_data[i * 8 +: 8];
				else cv[{pcnt[3:0], 2'(i)}] <= cmd_data[i * 8 +: 8];
			end
			pcnt <= pcnt + 1'd1;
			if (pcnt == 95) begin st <= S_CTX; ctxk <= 0; end
		end

		// macroblock to memory, one word per write
		S_WB: begin
			if (!wb_we || !mem_busy) begin
				if (wbk == 48 && (!wb_we || !mem_busy)) begin
					wb_we <= 0;
					st <= S_LB;
					ctxk <= 0;
				end
				else begin
					wb_we <= 1;
					wb_addr <= wb_word_addr;
					wb_din <= wb_word;
					wbk <= wbk + 1'd1;
				end
			end
		end

		// line buffers, left column and corner for the next macroblock
		S_LB: begin
			for (i = 0; i < 4; i = i + 1)
				lby[{mbx, 2'(i)}] <= {cy[240 + i * 4 + 3], cy[240 + i * 4 + 2], cy[240 + i * 4 + 1], cy[240 + i * 4]};
			for (i = 0; i < 2; i = i + 1) begin
				lbu[{mbx, 1'(i)}] <= {cu[56 + i * 4 + 3], cu[56 + i * 4 + 2], cu[56 + i * 4 + 1], cu[56 + i * 4]};
				lbv[{mbx, 1'(i)}] <= {cv[56 + i * 4 + 3], cv[56 + i * 4 + 2], cv[56 + i * 4 + 1], cv[56 + i * 4]};
			end
			for (i = 0; i < 16; i = i + 1) lefty[i] <= cy[i * 16 + 15];
			for (i = 0; i < 8; i = i + 1) begin leftu[i] <= cu[i * 8 + 7]; leftv[i] <= cv[i * 8 + 7]; end
			cornery <= topy[15];
			corneru <= topu[7];
			cornerv <= topv[7];
			mb_count <= mb_count + 1'd1;
			st <= S_CMD;
		end

		default: st <= S_CMD;
	endcase
end

endmodule
