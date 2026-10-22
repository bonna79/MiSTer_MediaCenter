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
// the first pixel, then the pixels outside the picture replaced by the edge
reg [255:0] rb;
reg [255:0] rsh;
reg [167:0] newrow;
reg  [7:0] lpix, rpix;
always @* begin
	rb = {mem_dout, rowbuf};
	rb = rb >> ((3 - rv_word) * 64);      // the row's words, first at bit 0
	rsh = klf[rk_i] != 0 ? rb << (klf[rk_i] * 8) : rb >> (ksh[rk_i] * 8);
	lpix = rb[7:0];
	rpix = rb[krc[rk_i] * 8 +: 8];
	for (i = 0; i < 21; i = i + 1)
		newrow[i * 8 +: 8] = i < klf[rk_i] ? lpix : ($signed(7'(i)) > krl[rk_i] ? rpix : rsh[i * 8 +: 8]);
end

// window RAMs: luma rows {buffer, row}, chroma rows {buffer, V, row}
reg [167:0] winrow[64];
reg  [71:0] cwin[64];
wire        rv_last = mem_dout_ready && rv_word == kK[rk_i] - 1;
always @(posedge clk) begin
	if (rv_last && rv_j == 0) winrow[{rv_b, rv_row}] <= newrow;
	if (rv_last && rv_j != 0) cwin[{rv_b, rv_j[1], rv_row[3:0]}] <= newrow[71:0];
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

function integer tap(input integer a, b, c, d, e, f);
	tap = a - 5 * b + 20 * c + 20 * d - 5 * e + f;
endfunction

integer p, gr;
integer b1v[0:5];   // b1 at (X, Y-2 .. Y+3)
integer bb0, bb1, hh0, hh1, jj, v;
reg  [7:0] lpx[4];
// window sample at (dx, dy) from pixel pp of the current group (direct
// array read, so that the simulator sees lw6[] in the sensitivity)
`define G(dx, dy, pp) lw6[2 + (dy)][(lg * 4 + (pp) + 2 + (dx)) * 8 +: 8]
always @* begin
	for (p = 0; p < 4; p = p + 1) begin
		for (gr = 0; gr < 6; gr = gr + 1)
			b1v[gr] = tap(`G(-2, gr - 2, p), `G(-1, gr - 2, p), `G(0, gr - 2, p),
			              `G(1, gr - 2, p), `G(2, gr - 2, p), `G(3, gr - 2, p));
		bb0 = clip8((b1v[2] + 16) >>> 5);
		bb1 = clip8((b1v[3] + 16) >>> 5);
		hh0 = clip8((tap(`G(0, -2, p), `G(0, -1, p), `G(0, 0, p), `G(0, 1, p), `G(0, 2, p), `G(0, 3, p)) + 16) >>> 5);
		hh1 = clip8((tap(`G(1, -2, p), `G(1, -1, p), `G(1, 0, p), `G(1, 1, p), `G(1, 2, p), `G(1, 3, p)) + 16) >>> 5);
		jj  = clip8((tap(b1v[0], b1v[1], b1v[2], b1v[3], b1v[4], b1v[5]) + 512) >>> 10);
		case ({xf, yf})
			4'b00_00: v = `G(0, 0, p);
			4'b00_01: v = (`G(0, 0, p) + hh0 + 1) >> 1;
			4'b00_10: v = hh0;
			4'b00_11: v = (`G(0, 1, p) + hh0 + 1) >> 1;
			4'b01_00: v = (`G(0, 0, p) + bb0 + 1) >> 1;
			4'b10_00: v = bb0;
			4'b11_00: v = (`G(1, 0, p) + bb0 + 1) >> 1;
			4'b10_10: v = jj;
			4'b01_01: v = (bb0 + hh0 + 1) >> 1;
			4'b11_01: v = (bb0 + hh1 + 1) >> 1;
			4'b01_11: v = (hh0 + bb1 + 1) >> 1;
			4'b11_11: v = (hh1 + bb1 + 1) >> 1;
			4'b10_01: v = (bb0 + jj + 1) >> 1;
			4'b10_11: v = (jj + bb1 + 1) >> 1;
			4'b01_10: v = (hh0 + jj + 1) >> 1;
			default:  v = (jj + hh1 + 1) >> 1;
		endcase
		lpx[p] = v;
	end
end

// chroma rows: output row cr from window rows cr, cr + 1 (crow0, crow1),
// read one row ahead like the luma
reg  [3:0] cr;                // chroma output row
reg  [1:0] cg;                // group of 4 pixels
reg  [1:0] ck;
wire       cg_last = S == 16 ? cg == 1 : 1'b1;
wire [2:0] cxf = mvx[2:0], cyf = mvy[2:0];
reg  [7:0] cpx[4];
reg [71:0] crow0, crow1, cwin_q;
reg  [3:0] cwin_addr;
always @* begin
	if (ms == M_CLOAD) cwin_addr = {2'b00, ck};
	else cwin_addr = cg_last ? cr + 4'd3 : cr + 4'd2;
end
always @(posedge clk) cwin_q <= cwin[{c_b, plane[1], cwin_addr}];

integer A, B, C, D;
always @* begin
	for (p = 0; p < 4; p = p + 1) begin
		A = crow0[(cg * 4 + p) * 8 +: 8];
		B = crow0[(cg * 4 + p + 1) * 8 +: 8];
		C = crow1[(cg * 4 + p) * 8 +: 8];
		D = crow1[(cg * 4 + p + 1) * 8 +: 8];
		cpx[p] = ((8 - cxf) * (8 - cyf) * A + cxf * (8 - cyf) * B + (8 - cxf) * cyf * C + cxf * cyf * D + 32) >> 6;
	end
end

// ------------------------------------------------------------- weighting

function [7:0] wt(input [7:0] pv, input signed [8:0] w, input signed [7:0] o, input [3:0] d);
	integer t, ip, iw, io, id;
	begin
		ip = pv;
		iw = w;
		io = o;
		id = d;
		if (id != 0) t = ((ip * iw + (1 << (id - 1))) >>> id) + io;
		else t = ip * iw + io;
		wt = clip8(t);
	end
endfunction

// ---------------------------------------------------------------- control

always @(posedge clk) begin
	done <= 0;
	px_valid <= 0;

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
				got[rv_b][rv_j] <= 1;
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
			px_valid <= 1;
			px_plane <= 0;
			px_x <= rx + {lg, 2'b00};
			px_y <= ry + ly;
			px_n <= 4;
			for (i = 0; i < 4; i = i + 1) px[i * 8 +: 8] <= use_w ? wt(lpx[i], lw, lo, lden) : lpx[i];
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
			px_valid <= 1;
			px_plane <= plane;
			px_x <= (rx >> 1) + {cg, 2'b00};
			px_y <= (ry >> 1) + cr;
			px_n <= S == 4 ? 3'd2 : 3'd4;
			for (i = 0; i < 4; i = i + 1)
				px[i * 8 +: 8] <= !use_wc ? cpx[i] :
					plane == 1 ? wt(cpx[i], cwu[8:0], cwu[16:9], cden) : wt(cpx[i], cwv[8:0], cwv[16:9], cden);
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
			if (all16) begin done <= 1; ms <= M_IDLE; end
			else if (quad_eq[c_rq] || c_rk == 3) begin
				c_rk <= 0;
				c_rq <= c_rq + 1'd1;
				if (c_rq == 3) begin done <= 1; ms <= M_IDLE; end
			end
			else c_rk <= c_rk + 1'd1;
		end

		default: ms <= M_IDLE;
	endcase
end

initial begin
	ms = M_IDLE;
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
