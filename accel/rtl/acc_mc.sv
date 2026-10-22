// Inter prediction of one P macroblock for acc_engine: quarter-sample luma
// (6-tap filter), eighth-sample chroma (bilinear), explicit weighting, with
// the reference picture read from DDR and its edges repeated (clamping).
//
// The macroblock is split into regions with one vector each: 16x16 when all
// 16 vectors and references are equal, else 8x8 quadrants, else 4x4 blocks.
// For each region the three reference windows (Y, U, V) are requested back
// to back; the luma prediction starts as soon as its window is in, while
// the chroma windows are still arriving. Prediction comes out 4 pixels a
// clock on px_*.
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

localparam M_IDLE = 0, M_REGION = 1, M_SETUP = 2, M_WAITY = 3, M_LLOAD = 4, M_LCALC = 5, M_WAITC = 6,
           M_CCALC = 7, M_NEXT = 8;
reg  [3:0] ms;

reg        all16;
reg  [3:0] quad_eq;
reg  [1:0] rq;                // quadrant
reg  [1:0] rk;                // 4x4 block inside the quadrant
reg  [4:0] S;                 // region size 16, 8, 4
reg  [3:0] rx, ry;            // region position in the macroblock
reg signed [15:0] mvx, mvy;
reg  [7:0] rslot;
reg  [8:0] lw;
reg  [7:0] lo;
reg [16:0] cwu, cwv;
reg  [1:0] plane;             // chroma plane being predicted: 1 U, 2 V
reg signed [15:0] jlim;

wire [3:0] rblk = {rq, rk};   // luma4x4BlkIdx of a 4x4 region
wire [31:0] rmv = all16 || quad_eq[rq] ? `MVOF({rq, 2'b00}) : `MVOF(rblk);

// ----------------------------------------------------------- window fetch
// Three jobs per region: 0 luma window (S+5 square), 1 U and 2 V windows
// (S/2+1 square). Rows are requested back to back over all three jobs and
// come back in order.

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
	irx = rx;
	iry = ry;
	imx = mvx;
	imy = mvy;
	iS = S;
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

reg signed [15:0] jwx[3], jwy[3];
reg  [4:0] jn[3];
reg [15:0] jw0[3];            // first word of each row
reg  [2:0] jK[3];             // words per row
reg [AW-1:0] jbase[3];
reg  [2:0] jsh[3];            // first pixel's byte in the first word
reg  [4:0] jlf[3];            // pixels repeated from the left edge (window starting left of x 0)
reg signed [6:0] jrl[3];      // last pixel inside the picture (later ones repeat it)
reg  [4:0] jrc[3];            // byte of that pixel among the row's words

wire [AW-1:0] slot_base = AW'(rslot) << SLOT_L2;

reg        rq_on;
reg  [1:0] rq_j;
reg  [4:0] rq_row;
reg  [1:0] rv_j;
reg  [4:0] rv_row;
reg  [1:0] rv_word;
reg  [2:0] got;               // window of each job complete
reg [191:0] rowbuf;

wire signed [15:0] rq_y = clampv(jwy[rq_j] + $signed({11'd0, rq_row}), rq_j == 0 ? H : Hc);
wire [AW-1:0] rq_addr = jbase[rq_j] + (AW'(rq_y) << (rq_j == 0 ? YSTRIDE_L2 : CSTRIDE_L2)) + jw0[rq_j];

// row being received, with the picture edges repeated: the words shifted to
// the first pixel, then the pixels outside the picture replaced by the edge
reg [167:0] winrow[21];       // luma window rows
reg  [71:0] cwin_u[9], cwin_v[9];
reg [255:0] rb;
reg [255:0] rsh;
reg [167:0] newrow;
reg  [7:0] lpix, rpix;
always @* begin
	rb = {mem_dout, rowbuf};
	rb = rb >> ((3 - rv_word) * 64);      // the row's words, first at bit 0
	rsh = jlf[rv_j] != 0 ? rb << (jlf[rv_j] * 8) : rb >> (jsh[rv_j] * 8);
	lpix = rb[7:0];
	rpix = rb[jrc[rv_j] * 8 +: 8];
	for (i = 0; i < 21; i = i + 1)
		newrow[i * 8 +: 8] = i < jlf[rv_j] ? lpix : ($signed(7'(i)) > jrl[rv_j] ? rpix : rsh[i * 8 +: 8]);
end

// ------------------------------------------------------------ luma rows
// Output row ly comes from window rows ly .. ly + 5, kept in lw6; groups of
// 4 pixels (lg) are done one a clock, then the rows move up by one. The
// window RAM is read one row ahead (address set in the same clock).

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
always @(posedge clk) win_q <= winrow[win_addr];

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

// ---------------------------------------------------------------- chroma

reg  [3:0] cr;                // chroma output row
reg  [1:0] cg;                // group of 4 pixels
wire [2:0] cxf = mvx[2:0], cyf = mvy[2:0];
reg  [7:0] cpx[4];
reg [71:0] crow0, crow1;
integer A, B, C, D;
always @* begin
	crow0 = plane == 1 ? cwin_u[cr] : cwin_v[cr];
	crow1 = plane == 1 ? cwin_u[cr + 1] : cwin_v[cr + 1];
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

	// requests, back to back
	if (mem_rd && !mem_busy) mem_rd <= 0;
	if (rq_on && (!mem_rd || !mem_busy)) begin
		mem_rd <= 1;
		mem_addr <= rq_addr;
		mem_burst <= jK[rq_j];
		if (rq_row + 1'd1 == jn[rq_j]) begin
			rq_row <= 0;
			rq_j <= rq_j + 1'd1;
			if (rq_j == 2) rq_on <= 0;
		end
		else rq_row <= rq_row + 1'd1;
	end

	// data
	if (mem_dout_ready) begin
		rowbuf <= {mem_dout, rowbuf[191:64]};
		if (rv_word == jK[rv_j] - 1) begin
			rv_word <= 0;
			if (rv_j == 0) winrow[rv_row] <= newrow;
			else if (rv_j == 1) cwin_u[rv_row] <= newrow[71:0];
			else cwin_v[rv_row] <= newrow[71:0];
			if (rv_row + 1'd1 == jn[rv_j]) begin
				rv_row <= 0;
				rv_j <= rv_j + 1'd1;
				got[rv_j] <= 1;
			end
			else rv_row <= rv_row + 1'd1;
		end
		else rv_word <= rv_word + 1'd1;
	end

	case (ms)
		M_IDLE: if (start) begin
			all16 <= 1;
			for (i = 1; i < 16; i = i + 1) if (`MVOF(i) != `MVOF(0)) all16 <= 0;
			if (qref[5:0] != qref[11:6] || qref[5:0] != qref[17:12] || qref[5:0] != qref[23:18]) all16 <= 0;
			for (i = 0; i < 4; i = i + 1)
				quad_eq[i] <= `MVOF(i * 4) == `MVOF(i * 4 + 1) && `MVOF(i * 4) == `MVOF(i * 4 + 2) && `MVOF(i * 4) == `MVOF(i * 4 + 3);
			rq <= 0;
			rk <= 0;
			ms <= M_REGION;
		end

		// next region: vector, reference, weights
		M_REGION: begin
			if (all16) begin S <= 16; rx <= 0; ry <= 0; end
			else if (quad_eq[rq]) begin S <= 8; rx <= {rq[0], 3'd0}; ry <= {rq[1], 3'd0}; end
			else begin S <= 4; rx <= {rq[0], rk[0], 2'd0}; ry <= {rq[1], rk[1], 2'd0}; end
			mvx <= rmv[15:0];
			mvy <= rmv[31:16];
			rslot <= qslot[rq * 8 +: 8];
			{lw, lo, cwu, cwv} <= qwt[rq * 51 +: 51];
			ms <= M_SETUP;
		end

		// the three windows, then their rows are requested
		M_SETUP: begin
			for (i = 0; i < 3; i = i + 1) begin
				jwx[i] <= swx[i];
				jwy[i] <= swy[i];
				jn[i] <= sn[i];
				jw0[i] <= clampv(swx[i], i == 0 ? W : Wc) >>> 3;
				jK[i] <= 3'((clampv(swx[i] + $signed({11'd0, sn[i]}) - 16'sd1, i == 0 ? W : Wc) >>> 3) -
				            (clampv(swx[i], i == 0 ? W : Wc) >>> 3) + 1);
				jbase[i] <= slot_base + (i == 0 ? AW'(0) : (i == 1 ? AW'(UOFF) : AW'(VOFF)));
				jsh[i] <= 3'(swx[i]);
				jlf[i] <= swx[i] >= 0 ? 5'd0 : (swx[i] < -21 ? 5'd21 : 5'(-swx[i]));
				jlim = (i == 0 ? W : Wc) - 16'sd1;
				jrl[i] <= jlim - swx[i] < -1 ? -7'sd1 : (jlim - swx[i] > 31 ? 7'sd31 : 7'(jlim - swx[i]));
				jrc[i] <= 5'(jlim - (clampv(swx[i], i == 0 ? W : Wc) & ~16'sd7));
			end
			rq_on <= 1;
			rq_j <= 0;
			rq_row <= 0;
			rv_j <= 0;
			rv_row <= 0;
			rv_word <= 0;
			got <= 0;
			ms <= M_WAITY;
		end

		M_WAITY: if (got[0]) begin
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

		M_WAITC: if (got[plane]) begin
			ms <= M_CCALC;
			cr <= 0;
			cg <= 0;
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
			if (S == 16 && cg == 0) cg <= 1;
			else begin
				cg <= 0;
				cr <= cr + 1'd1;
				if (cr == (S >> 1) - 1) begin
					if (plane == 1) begin plane <= 2; ms <= M_WAITC; end
					else ms <= M_NEXT;
				end
			end
		end

		M_NEXT: begin
			ms <= M_REGION;
			if (all16) begin done <= 1; ms <= M_IDLE; end
			else if (quad_eq[rq] || rk == 3) begin
				rk <= 0;
				rq <= rq + 1'd1;
				if (rq == 3) begin done <= 1; ms <= M_IDLE; end
			end
			else rk <= rk + 1'd1;
		end

		default: ms <= M_IDLE;
	endcase
end

initial begin
	ms = M_IDLE;
	rq_on = 0;
	mem_rd = 0;
end

endmodule
