// Deblocking strengths of the FPGA accelerator (phase 4b): the 32 edge
// strengths (bS) of a macroblock, worked out from its macroblock command
// (A3) and those of its left and top neighbours, as FFmpeg's loop filter
// (ff_h264_filter_mb, progressive frame) does. The command A5 only says
// whether the left and top neighbours are filtered against (avail).
//
// For each 4x4 block the strength needs: intra, coefficients (nz), the
// reference of its 8x8 in each list (a slot, or "list not used") and its
// vectors. The neighbours are kept here: the right column of the previous
// macroblock (left) and the bottom row of each macroblock of the row above
// (line buffer, one entry per macroblock column).
//
// Started with each macroblock (the engine's stage B holds its data until
// the next start); 32 segments one a clock through 3 stages, then the
// macroblock's own edges are stored for the next ones. About 38 clocks.
//
// Strengths out as in the command A5 of before: bsv / bsh 2 bits per segment
// at bit (edge * 4 + segment) * 2 (4 written as 3); bs4[d]: edge 0 of
// direction d has strength 4.
module acc_bs
(
	input             clk,
	input             reset,
	input             start,
	input       [7:0] mbx,
	input       [2:0] mtype,          // 0 inter, 1 I4x4, 2 I8x8, 3 I16x16, 4 PCM
	input             t8,             // inter: 8x8 transform
	input      [15:0] nzm,            // inter: luma blocks with coefficients (8x8: bits 0, 4, 8, 12)
	input      [55:0] qref,           // per 8x8: 14 bits, bit 6 list 0 unused, bit 13 list 1 unused
	input      [31:0] slot0, slot1,   // per 8x8: slot of its reference in list 0, list 1
	input     [511:0] mv0, mv1,       // per 4x4 block: {y, x} 16 bits each
	input             two,            // the slice has two lists
	input       [1:0] avail,          // left, top filtered against
	output reg [31:0] bsv, bsh,
	output reg  [1:0] bs4,
	output            busy
);

function [3:0] bidx(input [1:0] x, input [1:0] y);
	bidx = {y[1], x[1], y[0], x[0]};
endfunction

// far: two vectors 4 quarter pels or more apart (x or y)
function far(input [31:0] a, input [31:0] b);
	reg signed [16:0] dx, dy;
	begin
		dx = $signed({a[15], a[15:0]}) - $signed({b[15], b[15:0]});
		dy = $signed({a[31], a[31:16]}) - $signed({b[31], b[31:16]});
		far = dx >= 4 || dx <= -4 || dy >= 4 || dy <= -4;
	end
endfunction

// ---------------------------------------------------------------- current
wire       intra = mtype != 0;
wire       t8e = mtype == 0 ? t8 : mtype == 2;
reg [15:0] nz;
reg  [8:0] r0[4], r1[4];          // reference ids: {list not used, slot}
always @* begin : cur
	integer k;
	for (k = 0; k < 16; k = k + 1)
		nz[k] = mtype == 0 && (t8 ? nzm[k & 12] : nzm[k]);
	for (k = 0; k < 4; k = k + 1) begin
		r0[k] = qref[k * 14 + 6] ? 9'h100 : {1'b0, slot0[k * 8 +: 8]};
		r1[k] = qref[k * 14 + 13] ? 9'h100 : {1'b0, slot1[k * 8 +: 8]};
	end
end

// -------------------------------------------------------------- neighbours
// one edge: intra, nz of the 4 blocks, references of the 2 8x8, 4 vectors
// per list: {mv1[4], mv0[4], r1[2], r0[2], nz[4], intra} = 297 bits
localparam EW = 297;
reg [EW-1:0] left_e;
reg [EW-1:0] top_lb[128];
reg [EW-1:0] top_e;
reg  [6:0] lb_a;
always @(posedge clk) top_e <= top_lb[lb_a];

// the edge of the current macroblock for the next ones: right column (x 3,
// 8x8 1 and 3), bottom row (y 3, 8x8 2 and 3)
function [EW-1:0] edge_of(input dir);
	integer j;
	begin
		edge_of[0] = intra;
		for (j = 0; j < 4; j = j + 1) begin
			edge_of[1 + j] = nz[dir ? bidx(j, 3) : bidx(3, j)];
			edge_of[41 + j * 32 +: 32] = mv0[(dir ? bidx(j, 3) : bidx(3, j)) * 32 +: 32];
			edge_of[169 + j * 32 +: 32] = mv1[(dir ? bidx(j, 3) : bidx(3, j)) * 32 +: 32];
		end
		for (j = 0; j < 2; j = j + 1) begin
			edge_of[5 + j * 9 +: 9] = r0[dir ? 2 + j : 1 + 2 * j];
			edge_of[23 + j * 9 +: 9] = r1[dir ? 2 + j : 1 + 2 * j];
		end
	end
endfunction

// -------------------------------------------------------------- sequence
reg  [1:0] st;                    // 0 idle, 1 top read, 2 segments, 3 store
reg  [5:0] cnt;                   // segment issued (stage 1)
reg        two_q;
reg  [1:0] av_q;
reg  [7:0] mbx_q;
assign busy = st != 0 || start;

// stage 1: operands of segment cnt (d = cnt[4], e = cnt[3:2], i = cnt[1:0])
reg        s1_v, s1_d, s1_e0, s1_skip, s1_intra, s1_av;
reg  [3:0] s1_pos;                // e * 4 + i
reg        s1_nz;
reg  [8:0] s1_r0a, s1_r1a, s1_r0b, s1_r1b;
reg [31:0] s1_m0a, s1_m1a, s1_m0b, s1_m1b;
// stage 2: comparisons
reg        s2_v, s2_d, s2_e0, s2_skip, s2_intra, s2_av, s2_nz;
reg  [3:0] s2_pos;
reg        s2_ne00, s2_ne11, s2_ne01, s2_ne10, s2_r0none;
reg        s2_f00, s2_f11, s2_f01, s2_f10;

wire [1:0] ci = cnt[1:0], ce = cnt[3:2];
wire       cd = cnt[4];
wire [1:0] cx = cd ? ci : ce, cy = cd ? ce : ci;
wire [3:0] cb = bidx(cx, cy);
wire [3:0] cpb = cd ? bidx(cx, cy - 1'd1) : bidx(cx - 1'd1, cy);
wire [EW-1:0] nb = cd ? top_e : left_e;

always @(posedge clk) begin
	s1_v <= 0;
	s2_v <= 0;
	if (reset) st <= 0;
	else case (st)
	0: if (start) begin
		lb_a <= mbx[6:0];
		mbx_q <= mbx;
		two_q <= two;
		av_q <= avail;
		cnt <= 0;
		st <= 1;
	end
	1: st <= 2;                    // top_e valid from the next clock
	2: begin
		s1_v <= 1;
		s1_d <= cd;
		s1_e0 <= ce == 0;
		s1_pos <= {ce, ci};
		s1_av <= av_q[cd];
		s1_skip <= ce != 0 && t8e && ce[0];
		s1_r0a <= r0[{cb[3], cb[2]}];
		s1_r1a <= r1[{cb[3], cb[2]}];
		s1_m0a <= mv0[cb * 32 +: 32];
		s1_m1a <= mv1[cb * 32 +: 32];
		if (ce == 0) begin
			s1_intra <= intra || nb[0];
			s1_nz <= nz[cb] || nb[1 + ci];
			s1_r0b <= nb[5 + ci[1] * 9 +: 9];
			s1_r1b <= nb[23 + ci[1] * 9 +: 9];
			s1_m0b <= nb[41 + ci * 32 +: 32];
			s1_m1b <= nb[169 + ci * 32 +: 32];
		end
		else begin
			s1_intra <= intra;
			s1_nz <= nz[cb] || nz[cpb];
			s1_r0b <= r0[{cpb[3], cpb[2]}];
			s1_r1b <= r1[{cpb[3], cpb[2]}];
			s1_m0b <= mv0[cpb * 32 +: 32];
			s1_m1b <= mv1[cpb * 32 +: 32];
		end
		cnt <= cnt + 1'd1;
		if (cnt == 31) st <= 3;
	end
	3: if (!s1_v && !s2_v) begin    // the last segment is written: store the edges
		left_e <= edge_of(0);
		top_lb[mbx_q[6:0]] <= edge_of(1);
		st <= 0;
	end
	endcase

	// stage 2
	if (s1_v) begin
		s2_v <= 1;
		s2_d <= s1_d;
		s2_e0 <= s1_e0;
		s2_pos <= s1_pos;
		s2_av <= s1_av;
		s2_skip <= s1_skip;
		s2_intra <= s1_intra;
		s2_nz <= s1_nz;
		s2_ne00 <= s1_r0a != s1_r0b;
		s2_ne11 <= s1_r1a != s1_r1b;
		s2_ne01 <= s1_r0a != s1_r1b;
		s2_ne10 <= s1_r1a != s1_r0b;
		s2_r0none <= s1_r0a[8];
		s2_f00 <= far(s1_m0a, s1_m0b);
		s2_f11 <= far(s1_m1a, s1_m1b);
		s2_f01 <= far(s1_m0a, s1_m1b);
		s2_f10 <= far(s1_m1a, s1_m0b);
	end

	// stage 3: the strength
	if (s2_v) begin : st3
		reg [2:0] v;
		reg       m0, m1;
		m0 = s2_ne00 || (!s2_r0none && s2_f00);
		m1 = m0 || s2_ne11 || s2_f11;
		if (s2_e0 && !s2_av) v = 0;
		else if (s2_skip) v = 0;
		else if (s2_intra) v = s2_e0 ? 3'd4 : 3'd3;
		else if (s2_nz) v = 2;
		else if (!two_q) v = m0;
		else if (!m1) v = 0;
		else if (s2_ne01 || s2_ne10) v = 1;
		else v = s2_f01 || s2_f10;
		if (s2_d) bsh[s2_pos * 2 +: 2] <= v == 4 ? 2'd3 : v[1:0];
		else bsv[s2_pos * 2 +: 2] <= v == 4 ? 2'd3 : v[1:0];
		if (s2_e0 && s2_pos[1:0] == 0) bs4[s2_d] <= v == 4;
	end
end

endmodule
