// Inter prediction of one P macroblock for acc_engine: quarter-sample luma
// (6-tap filter), eighth-sample chroma (bilinear), explicit weighting, with
// the reference picture read from DDR and its edges repeated (clamping).
//
// The macroblock is split into regions with one vector each: 16x16 when all
// 16 vectors and references are equal, else 8x8 quadrants, else 4x4 blocks.
// Fetch and calculation run apart on two window buffers: while the
// prediction of one region is calculated, the reference windows (Y, U, V)
// of the next region are already being read. The luma prediction of a
// region starts as soon as its luma window is in. Prediction comes out 4
// pixels a clock on px_*.
module acc_mc #(
	parameter AW         = 28,
	parameter SLOT_L2    = 19,
	parameter YSTRIDE_L2 = 8,
	parameter CSTRIDE_L2 = 7,
	parameter UOFF       = 278528,
	parameter VOFF       = 348160
)
(
	input              clk,
	input              start,
	input        [7:0] mbx, mby, wmb, hmb,
	input      [511:0] mvs,      // 16 x {y, x}, quarter samples, luma4x4BlkIdx order
	input       [23:0] qref,     // reference index of each 8x8
	input       [31:0] qslot,    // picture slot of each 8x8
	input      [203:0] qwt,      // weights of each 8x8: lw 9, lo 8, cb {o 8, w 9}, cr {o 8, w 9}
	input              use_w,
	input              use_wc,
	input        [3:0] lden, cden,

	output reg         mem_rd,
	output reg [AW-1:0] mem_addr,
	output reg   [7:0] mem_burst,
	input              mem_busy,
	input       [63:0] mem_dout,
	input              mem_dout_ready,

	output reg         px_valid,
	output reg   [1:0] px_plane,
	output reg   [3:0] px_x, px_y,
	output reg   [2:0] px_n,
	output reg  [31:0] px,
	output reg         done
);

function [7:0] clip8(input integer v);
	clip8 = v < 0 ? 8'd0 : (v > 255 ? 8'd255 : v[7:0]);
endfunction

function signed [15:0] clampv(input signed [15:0] v, input signed [15:0] lim);
	clampv = v < 0 ? 16'sd0 : (v > lim - 1 ? lim - 16'sd1 : v);
endfunction

// vector of 4x4 block b (a macro, not a function, so that simulators see
// mvs in the sensitivity)
`define MVOF(b) mvs[(b) * 32 +: 32]

integer i;

// ----------------------------------------------------------- region plan
// Regions in order: quadrant rq, 4x4 block rk inside it. The fetch side (f_)
// and the calculation side (c_) each walk the same list.

reg        all16;
reg  [3:0] quad_eq;

function [4:0] reg_size(input a16, input [3:0] qe, input [1:0] q);
	reg_size = a16 ? 5'd16 : (qe[q] ? 5'd8 : 5'd4);
endfunction

// ------------------------------------------------------------ fetch side

localparam F_IDLE = 0, F_REGION = 1, F_SETUP = 2, F_REQ = 3, F_NEXT = 4;
reg  [2:0] fs;
reg  [1:0] f_rq, f_rk;
reg        f_b;               // window buffer being filled
reg  [4:0] f_S;
reg  [3:0] f_rx, f_ry;
reg signed [15:0] f_mvx, f_mvy;
reg  [7:0] f_slot;
wire [31:0] f_mv = all16 || quad_eq[f_rq] ? `MVOF({f_rq, 2'b00}) : `MVOF({f_rq, f_rk});

reg        bused[2];          // buffer holds a region not yet calculated

wire signed [15:0] W  = {4'd0, wmb, 4'd0};
wire signed [15:0] H  = {4'd0, hmb, 4'd0};
wire signed [15:0] Wc = {5'd0, wmb, 3'd0};
wire signed [15:0] Hc = {5'd0, hmb, 3'd0};

// window of each plane, from the region (all signed integers)
integer iX, iY, irx, iry, imx, imy, iS;
reg signed [15:0] swx[3], swy[3];
reg  [4:0] sn[3];
always @* begin
	iX = mbx * 16;
	iY = mby * 16;
	irx = f_rx;
	iry = f_ry;
	imx = f_mvx;
	imy = f_mvy;
	iS = f_S;
	swx[0] = iX + irx + (imx >>> 2) - 2;
	swy[0] = iY + iry + (imy >>> 2) - 2;
	sn[0] = iS + 5;
	swx[1] = ((iX + irx) >>> 1) + (imx >>> 3);
	swy[1] = ((iY + iry) >>> 1) + (imy >>> 3);
	sn[1] = (iS >> 1) + 1;
	swx[2] = swx[1];
	swy[2] = swy[1];
	sn[2] = sn[1];
end

// request parameters of the region being requested
reg signed [15:0] jwy[3];
reg  [4:0] jn[3];
reg [15:0] jw0[3];            // first word of each row
reg  [2:0] jK[3];             // words per row
reg [AW-1:0] jbase[3];
// receive parameters, per buffer (index buffer * 3 + job)
reg  [4:0] kn[6];
reg  [2:0] kK[6];
reg  [2:0] ksh[6];            // first pixel's byte in the first word
reg  [4:0] klf[6];            // pixels repeated from the left edge (window starting left of x 0)
reg signed [6:0] krl[6];      // last pixel inside the picture (later ones repeat it)
reg  [4:0] krc[6];            // byte of that pixel among the row's words
reg signed [15:0] jlim;

wire [AW-1:0] slot_base = AW'(f_slot) << SLOT_L2;

reg  [1:0] rq_j;
reg  [4:0] rq_row;
wire signed [15:0] rq_y = clampv(jwy[rq_j] + $signed({11'd0, rq_row}), rq_j == 0 ? H : Hc);
wire [AW-1:0] rq_addr = jbase[rq_j] + (AW'(rq_y) << (rq_j == 0 ? YSTRIDE_L2 : CSTRIDE_L2)) + jw0[rq_j];

// ---------------------------------------------------------- receive side

reg        rv_b;
reg  [1:0] rv_j;
reg  [4:0] rv_row;
reg  [1:0] rv_word;
reg  [2:0] got[2];            // window of each job complete, per buffer
reg [191:0] rowbuf;
wire [2:0] rk_i = rv_b ? 3'd3 + rv_j : {1'b0, rv_j};

// row being received, with the picture edges repeated: the words shifted to
// the first pixel, then the pixels outside the picture replaced by the edge.
// Two register stages: r1 (the row's words, first at bit 0, and its
// parameters), r2 (the finished row, written to the window RAM).
reg [255:0] rb;
always @* begin
	rb = {mem_dout, rowbuf};
	rb = rb >> ((3 - rv_word) * 64);
end
wire        rv_last = mem_dout_ready && rv_word == kK[rk_i] - 1;

reg         r1_v, r1_done, r1_b;
reg   [1:0] r1_j;
reg   [4:0] r1_row;
reg [255:0] r1_rb;
reg   [4:0] r1_lf, r1_rc;
reg   [2:0] r1_sh;
reg signed [6:0] r1_rl;
always @(posedge clk) begin
	r1_v <= rv_last;
	r1_done <= rv_row + 1'd1 == kn[rk_i];
	r1_b <= rv_b;
	r1_j <= rv_j;
	r1_row <= rv_row;
	r1_rb <= rb;
	r1_lf <= klf[rk_i];
	r1_sh <= ksh[rk_i];
	r1_rl <= krl[rk_i];
	r1_rc <= krc[rk_i];
end

reg [255:0] rsh;
reg [167:0] newrow;
reg  [7:0] lpix, rpix;
always @* begin
	rsh = r1_lf != 0 ? r1_rb << (r1_lf * 8) : r1_rb >> (r1_sh * 8);
	lpix = r1_rb[7:0];
	rpix = r1_rb[r1_rc * 8 +: 8];
	for (i = 0; i < 21; i = i + 1)
		newrow[i * 8 +: 8] = i < r1_lf ? lpix : ($signed(7'(i)) > r1_rl ? rpix : rsh[i * 8 +: 8]);
end

reg         r2_v, r2_done, r2_b;
reg   [1:0] r2_j;
reg   [4:0] r2_row;
reg [167:0] r2_d;
always @(posedge clk) begin
	r2_v <= r1_v;
	r2_done <= r1_done;
	r2_b <= r1_b;
	r2_j <= r1_j;
	r2_row <= r1_row;
	r2_d <= newrow;
end

// window RAMs: luma rows {buffer, row}, chroma rows {buffer, V, row}
reg [167:0] winrow[64];
reg  [71:0] cwin[64];
always @(posedge clk) begin
	if (r2_v && r2_j == 0) winrow[{r2_b, r2_row}] <= r2_d;
	if (r2_v && r2_j != 0) cwin[{r2_b, r2_j[1], r2_row[3:0]}] <= r2_d[71:0];
end

// ------------------------------------------------------ calculation side

localparam M_IDLE = 0, M_REGION = 1, M_WAITY = 3, M_LLOAD = 4, M_LCALC = 5, M_WAITC = 6,
           M_CCALC = 7, M_NEXT = 8, M_CLOAD = 9;
reg  [3:0] ms;
reg  [1:0] c_rq, c_rk;
reg        c_b;
reg  [4:0] S;                 // region size 16, 8, 4
reg  [3:0] rx, ry;            // region position in the macroblock
reg signed [15:0] mvx, mvy;
reg  [8:0] lw;
reg  [7:0] lo;
reg [16:0] cwu, cwv;
reg  [1:0] plane;             // chroma plane being predicted: 1 U, 2 V
wire [31:0] c_mv = all16 || quad_eq[c_rq] ? `MVOF({c_rq, 2'b00}) : `MVOF({c_rq, c_rk});

// luma rows: output row ly comes from window rows ly .. ly + 5, kept in
// lw6; groups of 4 pixels (lg) are done one a clock, then the rows move up
// by one. The window RAM is read one row ahead.
reg  [3:0] lk;
reg  [3:0] ly;                // output row in the region
reg  [1:0] lg;                // group of 4 pixels in the row
reg [167:0] lw6[6];
reg [167:0] win_q;
wire [1:0] lg_last = S == 16 ? 2'd3 : (S == 8 ? 2'd1 : 2'd0);
reg  [4:0] win_addr;
always @* begin
	if (ms == M_LLOAD) win_addr = lk[2:0];
	else win_addr = lg == lg_last ? ly + 5'd7 : ly + 5'd6;
end
always @(posedge clk) win_q <= winrow[{c_b, win_addr}];

wire [1:0] xf = mvx[1:0], yf = mvy[1:0];

// chroma rows: output row cr from window rows cr, cr + 1 (crow0, crow1),
// read one row ahead like the luma
reg  [3:0] cr;                // chroma output row
reg  [1:0] cg;                // group of 4 pixels
reg  [1:0] ck;
wire       cg_last = S == 16 ? cg == 1 : 1'b1;
wire [2:0] cxf = mvx[2:0], cyf = mvy[2:0];
reg [71:0] crow0, crow1, cwin_q;
reg  [3:0] cwin_addr;
always @* begin
	if (ms == M_CLOAD) cwin_addr = {2'b00, ck};
	else cwin_addr = cg_last ? cr + 4'd3 : cr + 4'd2;
end
always @(posedge clk) cwin_q <= cwin[{c_b, plane[1], cwin_addr}];

// ------------------------------------------------------- output pipeline
// The calculation side hands one group of 4 pixels a clock to stage 0:
// the window samples it needs (luma: 6 rows x 9, chroma: 2 rows x 5), the
// fraction of the vector and the weights. Stages 1..6 make the pixels:
//   1 six-tap sums (horizontal for 6 rows, vertical for 2 columns),
//     chroma: two pairs of products
//   2 half samples b, h clipped, raw centre sum j; chroma: sum, round
//   3 j clipped
//   4 quarter sample (select, average)
//   5 weight multiply
//   6 weight shift, offset, clip -> px

function signed [14:0] tap(input [7:0] a, b, c, d, e, f);
	tap = $signed({7'd0, a}) + $signed({7'd0, f}) - 15'sd5 * ($signed({7'd0, b}) + $signed({7'd0, e}))
	      + 15'sd20 * ($signed({7'd0, c}) + $signed({7'd0, d}));
endfunction

function signed [20:0] tapw(input signed [14:0] a, b, c, d, e, f);
	tapw = 21'(a) + 21'(f) - 21'sd5 * (21'(b) + 21'(e)) + 21'sd20 * (21'(c) + 21'(d));
endfunction

function [7:0] clip8s(input signed [21:0] v);
	clip8s = v < 0 ? 8'd0 : (v > 255 ? 8'd255 : v[7:0]);
endfunction

// chroma bilinear weights of the current region
reg  [6:0] ka, kb, kc, kd;
always @(posedge clk) begin
	ka <= (7'd8 - cxf) * (7'd8 - cyf);
	kb <= cxf * (7'd8 - cyf);
	kc <= (7'd8 - cxf) * cyf;
	kd <= cxf * cyf;
end

// stage tags: valid, chroma, plane, x, y, n, fraction, weights (stage 0
// is written by the calculation side, stages 1..6 here)
reg        sv0, sc0, suw0;
reg  [1:0] spl0;
reg  [3:0] sx0, sy0, sf0, sd0;
reg  [2:0] sn_0;
reg signed [8:0] sw0;
reg signed [7:0] so0;
reg  [6:0] ska0, skb0, skc0, skd0;
reg        sv[1:6], sc[1:6], suw[1:6];
reg  [1:0] spl[1:6];
reg  [3:0] sx[1:6], sy[1:6], sf[1:6], sd[1:6];
reg  [2:0] sn_[1:6];
reg signed [8:0] sw[1:6];
reg signed [7:0] so[1:6];

reg  [7:0] nb[6][9];          // stage 0 samples

reg signed [14:0] b1[4][6];   // stage 1
reg signed [14:0] hv0[4], hv1[4];
reg  [7:0] s00[2:4][4], s10[2:4][4], s01[2:4][4];   // G(0,0), G(1,0), G(0,1), stages 1..3
reg [15:0] ct0[4], ct1[4];

reg  [7:0] bb0[2:3][4], bb1[2:3][4], hh0[2:3][4], hh1[2:3][4];   // stage 2, 3
reg signed [20:0] jr[4];
reg  [7:0] cv[2:3][4];

reg  [7:0] jj[4];             // stage 3
reg  [7:0] vq[4];             // stage 4
reg signed [17:0] mq[4];      // stage 5

wire pipe_busy = sv0 || sv[1] || sv[2] || sv[3] || sv[4] || sv[5] || px_valid;

integer p, r, k;
always @(posedge clk) begin
	sv[1] <= sv0; sc[1] <= sc0; spl[1] <= spl0; sx[1] <= sx0; sy[1] <= sy0; sn_[1] <= sn_0;
	sf[1] <= sf0; suw[1] <= suw0; sw[1] <= sw0; so[1] <= so0; sd[1] <= sd0;
	for (k = 2; k < 7; k = k + 1) begin
		sv[k] <= sv[k - 1]; sc[k] <= sc[k - 1]; spl[k] <= spl[k - 1];
		sx[k] <= sx[k - 1]; sy[k] <= sy[k - 1]; sn_[k] <= sn_[k - 1]; sf[k] <= sf[k - 1];
		suw[k] <= suw[k - 1]; sw[k] <= sw[k - 1]; so[k] <= so[k - 1]; sd[k] <= sd[k - 1];
	end

	for (p = 0; p < 4; p = p + 1) begin
		// 1
		for (r = 0; r < 6; r = r + 1)
			b1[p][r] <= tap(nb[r][p], nb[r][p + 1], nb[r][p + 2], nb[r][p + 3], nb[r][p + 4], nb[r][p + 5]);
		hv0[p] <= tap(nb[0][p + 2], nb[1][p + 2], nb[2][p + 2], nb[3][p + 2], nb[4][p + 2], nb[5][p + 2]);
		hv1[p] <= tap(nb[0][p + 3], nb[1][p + 3], nb[2][p + 3], nb[3][p + 3], nb[4][p + 3], nb[5][p + 3]);
		s00[2][p] <= nb[2][p + 2];
		s10[2][p] <= nb[2][p + 3];
		s01[2][p] <= nb[3][p + 2];
		ct0[p] <= ska0 * nb[0][p] + skb0 * nb[0][p + 1];
		ct1[p] <= skc0 * nb[1][p] + skd0 * nb[1][p + 1];
		// 2
		bb0[2][p] <= clip8s((22'(b1[p][2]) + 22'sd16) >>> 5);
		bb1[2][p] <= clip8s((22'(b1[p][3]) + 22'sd16) >>> 5);
		hh0[2][p] <= clip8s((22'(hv0[p]) + 22'sd16) >>> 5);
		hh1[2][p] <= clip8s((22'(hv1[p]) + 22'sd16) >>> 5);
		jr[p] <= tapw(b1[p][0], b1[p][1], b1[p][2], b1[p][3], b1[p][4], b1[p][5]);
		cv[2][p] <= 8'((17'(ct0[p]) + 17'(ct1[p]) + 17'd32) >> 6);
		s00[3][p] <= s00[2][p]; s10[3][p] <= s10[2][p]; s01[3][p] <= s01[2][p];
		// 3
		jj[p] <= clip8s((22'(jr[p]) + 22'sd512) >>> 10);
		bb0[3][p] <= bb0[2][p]; bb1[3][p] <= bb1[2][p]; hh0[3][p] <= hh0[2][p]; hh1[3][p] <= hh1[2][p];
		cv[3][p] <= cv[2][p];
		s00[4][p] <= s00[3][p]; s10[4][p] <= s10[3][p]; s01[4][p] <= s01[3][p];
		// 4
		if (sc[3]) vq[p] <= cv[3][p];
		else case (sf[3])
			4'b00_00: vq[p] <= s00[4][p];
			4'b00_01: vq[p] <= 8'((9'(s00[4][p]) + hh0[3][p] + 1) >> 1);
			4'b00_10: vq[p] <= hh0[3][p];
			4'b00_11: vq[p] <= 8'((9'(s01[4][p]) + hh0[3][p] + 1) >> 1);
			4'b01_00: vq[p] <= 8'((9'(s00[4][p]) + bb0[3][p] + 1) >> 1);
			4'b10_00: vq[p] <= bb0[3][p];
			4'b11_00: vq[p] <= 8'((9'(s10[4][p]) + bb0[3][p] + 1) >> 1);
			4'b10_10: vq[p] <= jj[p];
			4'b01_01: vq[p] <= 8'((9'(bb0[3][p]) + hh0[3][p] + 1) >> 1);
			4'b11_01: vq[p] <= 8'((9'(bb0[3][p]) + hh1[3][p] + 1) >> 1);
			4'b01_11: vq[p] <= 8'((9'(hh0[3][p]) + bb1[3][p] + 1) >> 1);
			4'b11_11: vq[p] <= 8'((9'(hh1[3][p]) + bb1[3][p] + 1) >> 1);
			4'b10_01: vq[p] <= 8'((9'(bb0[3][p]) + jj[p] + 1) >> 1);
			4'b10_11: vq[p] <= 8'((9'(jj[p]) + bb1[3][p] + 1) >> 1);
			4'b01_10: vq[p] <= 8'((9'(hh0[3][p]) + jj[p] + 1) >> 1);
			default:  vq[p] <= 8'((9'(jj[p]) + hh1[3][p] + 1) >> 1);
		endcase
		// 5
		mq[p] <= !suw[4] ? 18'(vq[p]) :
		         $signed({10'd0, vq[p]}) * 18'(sw[4]) + (sd[4] != 0 ? 18'sd1 <<< (sd[4] - 1'd1) : 18'sd0);
		// 6
		px[p * 8 +: 8] <= !suw[5] ? mq[p][7:0] : clip8s(22'(mq[p] >>> sd[5]) + 22'(so[5]));
	end

	px_valid <= sv[5];
	px_plane <= spl[5];
	px_x <= sx[5];
	px_y <= sy[5];
	px_n <= sn_[5];
end

// ---------------------------------------------------------------- control

reg        fin;
always @(posedge clk) begin
	done <= 0;
	sv0 <= 0;
	if (fin && !pipe_busy) begin fin <= 0; done <= 1; end

	// ---- fetch: regions in order, each on a free buffer
	if (mem_rd && !mem_busy) mem_rd <= 0;
	case (fs)
		F_IDLE: ;

		F_REGION: if (!bused[f_b]) begin
			f_S <= reg_size(all16, quad_eq, f_rq);
			if (all16) begin f_rx <= 0; f_ry <= 0; end
			else if (quad_eq[f_rq]) begin f_rx <= {f_rq[0], 3'd0}; f_ry <= {f_rq[1], 3'd0}; end
			else begin f_rx <= {f_rq[0], f_rk[0], 2'd0}; f_ry <= {f_rq[1], f_rk[1], 2'd0}; end
			f_mvx <= f_mv[15:0];
			f_mvy <= f_mv[31:16];
			f_slot <= qslot[f_rq * 8 +: 8];
			fs <= F_SETUP;
		end

		// the three windows of the region; its rows are requested next
		F_SETUP: begin
			for (i = 0; i < 3; i = i + 1) begin
				jwy[i] <= swy[i];
				jn[i] <= sn[i];
				jw0[i] <= clampv(swx[i], i == 0 ? W : Wc) >>> 3;
				jK[i] <= 3'((clampv(swx[i] + $signed({11'd0, sn[i]}) - 16'sd1, i == 0 ? W : Wc) >>> 3) -
				            (clampv(swx[i], i == 0 ? W : Wc) >>> 3) + 1);
				jbase[i] <= slot_base + (i == 0 ? AW'(0) : (i == 1 ? AW'(UOFF) : AW'(VOFF)));
				kn[f_b * 3 + i] <= sn[i];
				kK[f_b * 3 + i] <= 3'((clampv(swx[i] + $signed({11'd0, sn[i]}) - 16'sd1, i == 0 ? W : Wc) >>> 3) -
				                      (clampv(swx[i], i == 0 ? W : Wc) >>> 3) + 1);
				ksh[f_b * 3 + i] <= 3'(swx[i]);
				klf[f_b * 3 + i] <= swx[i] >= 0 ? 5'd0 : (swx[i] < -21 ? 5'd21 : 5'(-swx[i]));
				jlim = (i == 0 ? W : Wc) - 16'sd1;
				krl[f_b * 3 + i] <= jlim - swx[i] < -1 ? -7'sd1 : (jlim - swx[i] > 31 ? 7'sd31 : 7'(jlim - swx[i]));
				krc[f_b * 3 + i] <= 5'(jlim - (clampv(swx[i], i == 0 ? W : Wc) & ~16'sd7));
			end
			bused[f_b] <= 1;
			rq_j <= 0;
			rq_row <= 0;
			fs <= F_REQ;
		end

		// rows of the three windows, back to back
		F_REQ: if (!mem_rd || !mem_busy) begin
			mem_rd <= 1;
			mem_addr <= rq_addr;
			mem_burst <= jK[rq_j];
			if (rq_row + 1'd1 == jn[rq_j]) begin
				rq_row <= 0;
				rq_j <= rq_j + 1'd1;
				if (rq_j == 2) fs <= F_NEXT;
			end
			else rq_row <= rq_row + 1'd1;
		end

		F_NEXT: begin
			f_b <= ~f_b;
			fs <= F_REGION;
			if (all16) fs <= F_IDLE;
			else if (quad_eq[f_rq] || f_rk == 3) begin
				f_rk <= 0;
				f_rq <= f_rq + 1'd1;
				if (f_rq == 3) fs <= F_IDLE;
			end
			else f_rk <= f_rk + 1'd1;
		end

		default: fs <= F_IDLE;
	endcase

	// ---- receive: rows come back in request order
	if (mem_dout_ready) begin
		rowbuf <= {mem_dout, rowbuf[191:64]};
		if (rv_word == kK[rk_i] - 1) begin
			rv_word <= 0;
			if (rv_row + 1'd1 == kn[rk_i]) begin
				rv_row <= 0;
				if (rv_j == 2) begin
					rv_j <= 0;
					rv_b <= ~rv_b;
				end
				else rv_j <= rv_j + 1'd1;
			end
			else rv_row <= rv_row + 1'd1;
		end
		else rv_word <= rv_word + 1'd1;
	end

	// a window is complete once its last row is in the RAM
	if (r2_v && r2_done) got[r2_b][r2_j] <= 1;

	// ---- calculation
	case (ms)
		M_IDLE: if (start) begin
			all16 <= 1;
			for (i = 1; i < 16; i = i + 1) if (`MVOF(i) != `MVOF(0)) all16 <= 0;
			if (qref[5:0] != qref[11:6] || qref[5:0] != qref[17:12] || qref[5:0] != qref[23:18]) all16 <= 0;
			for (i = 0; i < 4; i = i + 1)
				quad_eq[i] <= `MVOF(i * 4) == `MVOF(i * 4 + 1) && `MVOF(i * 4) == `MVOF(i * 4 + 2) && `MVOF(i * 4) == `MVOF(i * 4 + 3);
			c_rq <= 0;
			c_rk <= 0;
			f_rq <= 0;
			f_rk <= 0;
			fs <= F_REGION;
			ms <= M_REGION;
		end

		// next region: vector, weights
		M_REGION: begin
			S <= reg_size(all16, quad_eq, c_rq);
			if (all16) begin rx <= 0; ry <= 0; end
			else if (quad_eq[c_rq]) begin rx <= {c_rq[0], 3'd0}; ry <= {c_rq[1], 3'd0}; end
			else begin rx <= {c_rq[0], c_rk[0], 2'd0}; ry <= {c_rq[1], c_rk[1], 2'd0}; end
			mvx <= c_mv[15:0];
			mvy <= c_mv[31:16];
			{lw, lo, cwu, cwv} <= qwt[c_rq * 51 +: 51];
			ms <= M_WAITY;
		end

		M_WAITY: if (got[c_b][0]) begin
			ms <= M_LLOAD;
			lk <= 0;
		end

		// first 6 window rows
		M_LLOAD: begin
			lk <= lk + 1'd1;
			if (lk >= 1) lw6[lk - 1] <= win_q;
			if (lk == 6) begin
				ms <= M_LCALC;
				ly <= 0;
				lg <= 0;
			end
		end

		M_LCALC: begin
			sv0 <= 1;
			sc0 <= 0;
			spl0 <= 0;
			sx0 <= rx + {lg, 2'b00};
			sy0 <= ry + ly;
			sn_0 <= 4;
			sf0 <= {xf, yf};
			suw0 <= use_w;
			sw0 <= lw;
			so0 <= lo;
			sd0 <= lden;
			for (r = 0; r < 6; r = r + 1)
				for (k = 0; k < 9; k = k + 1) nb[r][k] <= lw6[r][(lg * 4 + k) * 8 +: 8];
			if (lg == lg_last) begin
				lg <= 0;
				ly <= ly + 1'd1;
				for (i = 0; i < 5; i = i + 1) lw6[i] <= lw6[i + 1];
				lw6[5] <= win_q;
				if (ly == S - 1) begin
					plane <= 1;
					ms <= M_WAITC;
				end
			end
			else lg <= lg + 1'd1;
		end

		M_WAITC: if (got[c_b][plane]) begin
			ms <= M_CLOAD;
			ck <= 0;
		end

		// first 2 chroma window rows
		M_CLOAD: begin
			ck <= ck + 1'd1;
			if (ck == 1) crow0 <= cwin_q;
			if (ck == 2) begin
				crow1 <= cwin_q;
				ms <= M_CCALC;
				cr <= 0;
				cg <= 0;
			end
		end

		M_CCALC: begin
			sv0 <= 1;
			sc0 <= 1;
			spl0 <= plane;
			sx0 <= (rx >> 1) + {cg, 2'b00};
			sy0 <= (ry >> 1) + cr;
			sn_0 <= S == 4 ? 3'd2 : 3'd4;
			sf0 <= 0;
			suw0 <= use_wc;
			sw0 <= plane == 1 ? cwu[8:0] : cwv[8:0];
			so0 <= plane == 1 ? cwu[16:9] : cwv[16:9];
			sd0 <= cden;
			ska0 <= ka; skb0 <= kb; skc0 <= kc; skd0 <= kd;
			for (k = 0; k < 5; k = k + 1) begin
				nb[0][k] <= crow0[(cg * 4 + k) * 8 +: 8];
				nb[1][k] <= crow1[(cg * 4 + k) * 8 +: 8];
			end
			if (!cg_last) cg <= cg + 1'd1;
			else begin
				cg <= 0;
				cr <= cr + 1'd1;
				crow0 <= crow1;
				crow1 <= cwin_q;
				if (cr == (S >> 1) - 1) begin
					if (plane == 1) begin plane <= 2; ms <= M_WAITC; end
					else ms <= M_NEXT;
				end
			end
		end

		// region done: its buffer is free for the fetch
		M_NEXT: begin
			bused[c_b] <= 0;
			got[c_b] <= 0;
			c_b <= ~c_b;
			ms <= M_REGION;
			if (all16) begin fin <= 1; ms <= M_IDLE; end
			else if (quad_eq[c_rq] || c_rk == 3) begin
				c_rk <= 0;
				c_rq <= c_rq + 1'd1;
				if (c_rq == 3) begin fin <= 1; ms <= M_IDLE; end
			end
			else c_rk <= c_rk + 1'd1;
		end

		default: ms <= M_IDLE;
	endcase
end

initial begin
	ms = M_IDLE;
	r1_v = 0;
	r2_v = 0;
	fin = 0;
	sv0 = 0;
	for (i = 1; i < 7; i = i + 1) sv[i] = 0;
	px_valid = 0;
	fs = F_IDLE;
	mem_rd = 0;
	f_b = 0;
	c_b = 0;
	rv_b = 0;
	rv_j = 0;
	rv_row = 0;
	rv_word = 0;
	bused[0] = 0;
	bused[1] = 0;
	got[0] = 0;
	got[1] = 0;
end

endmodule
