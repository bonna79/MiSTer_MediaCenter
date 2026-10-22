// Inter prediction of one P macroblock for acc_engine: quarter-sample luma
// (6-tap filter), eighth-sample chroma (bilinear), explicit weighting, with
// the reference picture read from DDR and its edges repeated (clamping).
//
// The macroblock is split into regions with one vector each: 16x16 when all
// 16 vectors and references are equal, else 8x8 quadrants, else 4x4 blocks.
// For each region the reference window is read once (rows in flight back to
// back), then the prediction comes out 4 pixels a clock on px_*.
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

// vector of 4x4 block b (a macro, not a function, so that simulators see
// mvs in the sensitivity)
`define MVOF(b) mvs[(b) * 32 +: 32]

// ----------------------------------------------------------- region plan

localparam M_IDLE = 0, M_REGION = 1, M_FETCH = 2, M_LLOAD = 3, M_LCALC = 4, M_CLOAD = 5, M_CCALC = 6, M_NEXT = 7;
reg  [2:0] ms;

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
reg  [1:0] plane;             // 0 luma, 1 U, 2 V

integer i;

// ----------------------------------------------------------- window fetch

wire signed [15:0] W  = {4'd0, wmb, 4'd0};
wire signed [15:0] H  = {4'd0, hmb, 4'd0};
wire signed [15:0] Wc = {5'd0, wmb, 3'd0};
wire signed [15:0] Hc = {5'd0, hmb, 3'd0};
wire signed [15:0] X  = {4'd0, mbx, 4'd0};
wire signed [15:0] Y  = {4'd0, mby, 4'd0};

reg signed [15:0] wx, wy;     // window top left (may be outside the picture)
reg  [4:0] nrows, ncols;      // window size
reg  [4:0] req_row, rcv_row;
reg  [1:0] rcv_word;
reg  [2:0] K;                 // words per row
reg [15:0] w0;                // first word of each row
reg [191:0] rowbuf;

function signed [15:0] clampv(input signed [15:0] v, input signed [15:0] lim);
	clampv = v < 0 ? 16'sd0 : (v > lim - 1 ? lim - 16'sd1 : v);
endfunction

wire signed [15:0] pw = plane == 0 ? W : Wc;
wire signed [15:0] ph = plane == 0 ? H : Hc;
wire signed [15:0] c0 = clampv(wx, pw);
wire signed [15:0] c1 = clampv(wx + $signed({11'd0, ncols}) - 16'sd1, pw);

wire [AW-1:0] slot_base = AW'(rslot) << SLOT_L2;
wire signed [15:0] row_y = clampv(wy + $signed({11'd0, req_row}), ph);
wire [AW-1:0] row_addr = plane == 0 ? slot_base + (AW'(row_y) << YSTRIDE_L2) + w0 :
                         slot_base + (plane == 1 ? AW'(UOFF) : AW'(VOFF)) + (AW'(row_y) << CSTRIDE_L2) + w0;

// window rows (21 pixels for luma, 9 for chroma)
reg [167:0] winrow[21];
reg [255:0] rb;
reg [167:0] newrow;
integer col;
always @* begin
	rb = {mem_dout, rowbuf};
	rb = rb >> ((3 - rcv_word) * 64);      // the row's words, first at bit 0
	newrow = 0;
	for (i = 0; i < 21; i = i + 1) begin
		col = clampv(wx + i, pw) - {w0, 3'b000};
		if (i < ncols) newrow[i * 8 +: 8] = rb[col * 8 +: 8];
	end
end

// ------------------------------------------------------- luma 4x4 blocks

reg  [3:0] lb;                // 4x4 block inside the region
reg  [3:0] lk;
reg  [1:0] lr;                // output row
reg  [7:0] sw[81];            // 9 x 9 window of the block
reg  [4:0] win_ra;
reg [167:0] win_q;
always @(posedge clk) win_q <= winrow[win_ra];

wire [3:0] lb_last = S == 16 ? 4'd15 : (S == 8 ? 4'd3 : 4'd0);
wire [3:0] sx = S == 16 ? {lb[1:0], 2'b00} : {1'b0, lb[0], 2'b00};
wire [3:0] sy = S == 16 ? {lb[3:2], 2'b00} : {1'b0, lb[1], 2'b00};

wire [1:0] xf = mvx[1:0], yf = mvy[1:0];

function integer tap(input integer a, b, c, d, e, f);
	tap = a - 5 * b + 20 * c + 20 * d - 5 * e + f;
endfunction

integer p, gr, gc;
integer b1v[0:5];   // b1 at (X, Y-2 .. Y+3)
integer bb0, bb1, hh0, hh1, jj, v;
reg  [7:0] lpx[4];
// window sample at (dx, dy) from pixel p of output row lr (direct array
// read, so that the simulator sees sw[] in the sensitivity)
`define G(dx, dy, pp, rr) sw[((rr) + 2 + (dy)) * 9 + (pp) + 2 + (dx)]
always @* begin
	for (p = 0; p < 4; p = p + 1) begin
		for (gr = 0; gr < 6; gr = gr + 1)
			b1v[gr] = tap(`G(-2, gr - 2, p, lr), `G(-1, gr - 2, p, lr), `G(0, gr - 2, p, lr),
			              `G(1, gr - 2, p, lr), `G(2, gr - 2, p, lr), `G(3, gr - 2, p, lr));
		bb0 = clip8((b1v[2] + 16) >>> 5);
		bb1 = clip8((b1v[3] + 16) >>> 5);
		hh0 = clip8((tap(`G(0, -2, p, lr), `G(0, -1, p, lr), `G(0, 0, p, lr), `G(0, 1, p, lr), `G(0, 2, p, lr), `G(0, 3, p, lr)) + 16) >>> 5);
		hh1 = clip8((tap(`G(1, -2, p, lr), `G(1, -1, p, lr), `G(1, 0, p, lr), `G(1, 1, p, lr), `G(1, 2, p, lr), `G(1, 3, p, lr)) + 16) >>> 5);
		jj  = clip8((tap(b1v[0], b1v[1], b1v[2], b1v[3], b1v[4], b1v[5]) + 512) >>> 10);
		case ({xf, yf})
			4'b00_00: v = `G(0, 0, p, lr);
			4'b00_01: v = (`G(0, 0, p, lr) + hh0 + 1) >> 1;
			4'b00_10: v = hh0;
			4'b00_11: v = (`G(0, 1, p, lr) + hh0 + 1) >> 1;
			4'b01_00: v = (`G(0, 0, p, lr) + bb0 + 1) >> 1;
			4'b10_00: v = bb0;
			4'b11_00: v = (`G(1, 0, p, lr) + bb0 + 1) >> 1;
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
integer A, B, C, D;
always @* begin
	for (p = 0; p < 4; p = p + 1) begin
		A = winrow[cr][(cg * 4 + p) * 8 +: 8];
		B = winrow[cr][(cg * 4 + p + 1) * 8 +: 8];
		C = winrow[cr + 1][(cg * 4 + p) * 8 +: 8];
		D = winrow[cr + 1][(cg * 4 + p + 1) * 8 +: 8];
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

wire [3:0] rblk = {rq, rk};   // luma4x4BlkIdx of a 4x4 region
wire [31:0] rmv = all16 || quad_eq[rq] ? `MVOF({rq, 2'b00}) : `MVOF(rblk);

// window position of the plane, in integers (all signed)
integer iX, iY, irx, iry, imx, imy, iS;
always @* begin
	iX = mbx * 16;
	iY = mby * 16;
	irx = rx;
	iry = ry;
	imx = mvx;
	imy = mvy;
	iS = S;
end

always @(posedge clk) begin
	done <= 0;
	px_valid <= 0;

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

		// next region: vector, reference, window of the luma
		M_REGION: begin
			if (all16) begin S <= 16; rx <= 0; ry <= 0; end
			else if (quad_eq[rq]) begin S <= 8; rx <= {rq[0], 3'd0}; ry <= {rq[1], 3'd0}; end
			else begin S <= 4; rx <= {rq[0], rk[0], 2'd0}; ry <= {rq[1], rk[1], 2'd0}; end
			mvx <= rmv[15:0];
			mvy <= rmv[31:16];
			rslot <= qslot[rq * 8 +: 8];
			{lw, lo, cwu, cwv} <= qwt[rq * 51 +: 51];
			plane <= 0;
			ms <= M_FETCH;
			req_row <= 31;    // window set up in the next clock
		end

		M_FETCH: begin
			if (req_row == 31) begin
				// window of this plane
				if (plane == 0) begin
					wx <= iX + irx + (imx >>> 2) - 2;
					wy <= iY + iry + (imy >>> 2) - 2;
					nrows <= S + 5'd5;
					ncols <= S + 5'd5;
				end
				else begin
					wx <= ((iX + irx) >>> 1) + (imx >>> 3);
					wy <= ((iY + iry) >>> 1) + (imy >>> 3);
					nrows <= (S >> 1) + 5'd1;
					ncols <= (S >> 1) + 5'd1;
				end
				req_row <= 30;
			end
			else if (req_row == 30) begin
				w0 <= c0[15:3];
				K <= c1[15:3] - c0[15:3] + 1;
				req_row <= 0;
				rcv_row <= 0;
				rcv_word <= 0;
				mem_rd <= 0;
			end
			else begin
				// rows requested back to back
				if (mem_rd && !mem_busy) mem_rd <= 0;
				if ((!mem_rd || !mem_busy) && req_row < nrows) begin
					mem_rd <= 1;
					mem_addr <= row_addr;
					mem_burst <= K;
					req_row <= req_row + 1'd1;
				end
				if (mem_dout_ready) begin
					rowbuf <= {mem_dout, rowbuf[191:64]};
					if (rcv_word == K - 1) begin
						winrow[rcv_row] <= newrow;
						rcv_word <= 0;
						rcv_row <= rcv_row + 1'd1;
						if (rcv_row + 1 == nrows) begin
							ms <= plane == 0 ? M_LLOAD : M_CCALC;
							lb <= 0;
							lk <= 0;
							cr <= 0;
							cg <= 0;
						end
					end
					else rcv_word <= rcv_word + 1'd1;
				end
			end
		end

		// 9 rows of the 4x4 block window
		M_LLOAD: begin
			win_ra <= sy + lk;
			lk <= lk + 1'd1;
			if (lk >= 2)
				for (i = 0; i < 9; i = i + 1) sw[(lk - 2) * 9 + i] <= win_q[(sx + i) * 8 +: 8];
			if (lk == 10) begin
				ms <= M_LCALC;
				lr <= 0;
			end
		end

		M_LCALC: begin
			px_valid <= 1;
			px_plane <= 0;
			px_x <= rx + sx;
			px_y <= ry + sy + lr;
			px_n <= 4;
			for (i = 0; i < 4; i = i + 1) px[i * 8 +: 8] <= use_w ? wt(lpx[i], lw, lo, lden) : lpx[i];
			lr <= lr + 1'd1;
			if (lr == 3) begin
				lk <= 0;
				lb <= lb + 1'd1;
				if (lb == lb_last) begin
					// luma done: chroma U
					plane <= 1;
					ms <= M_FETCH;
					req_row <= 31;
				end
				else ms <= M_LLOAD;
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
			if (S == 16 && cg == 0) cg <= 1;
			else begin
				cg <= 0;
				cr <= cr + 1'd1;
				if (cr == (S >> 1) - 1) begin
					if (plane == 1) begin plane <= 2; ms <= M_FETCH; req_row <= 31; end
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

initial ms = M_IDLE;

endmodule
