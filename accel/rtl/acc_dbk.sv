// H.264 deblocking filter of the FPGA accelerator (phase 4), between the
// rebuilt macroblock (acc_recon) and the picture in DDR: it takes the place
// of the old writer of acc_engine.
//
// The strengths (bS) and qp of each macroblock come from the ARM (command A5,
// accel/doc/comandi.md: FFmpeg's own loop filter, run in "capture" mode);
// a macroblock without A5 is not filtered. Intra prediction keeps using the
// unfiltered pixels (acc_engine's own line buffers); the pictures in DDR,
// used as references, are filtered.
//
// Filtering a macroblock changes up to 3 pixels on the other side of its left
// and top edges, so pixels are written to DDR only once nothing can change
// them any more: each step writes a region shifted 8 pixels left and 4 rows
// up (chroma: 8 left, 2 up) from the macroblock. Kept here:
//  - the working area, luma rows -4..15 x columns -8..15 of the macroblock
//    (L: -8..-1, the previous macroblock; C0 0..7; C1 8..15), chroma rows
//    -2..7 x columns -8..7, per plane;
//  - line buffers: the bottom 4 luma rows (2 chroma rows) of the macroblock
//    row above, as filtered so far.
// One step per macroblock:
//  ROT  the area turns row by row: each row leaving the top is written out
//       (DDR, or the line buffers for the bottom rows) and comes back at the
//       bottom with the next macroblock's pixels (recon / line buffers), its
//       columns 8..15 becoming the new L;
//  V    vertical edges (filtering along rows): the 16 rows turn through 4
//       edge stages (luma; chroma 2), each stage 2 clocks;
//  H    horizontal edges: the 16 columns turn through the 4 edge stages.
// At the end of a picture (fin) a last ROT writes the last macroblock and the
// line buffers (bottom rows of the picture) are written out.
module acc_dbk #(
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

	input             start,      // a macroblock is in recon buffer sbuf
	input             fin,        // end of picture
	input             sbuf,
	input       [7:0] mbx, mby, wmb, hmb, slot,
	input             prm_on,     // its command A5 (else not filtered)
	input     [159:0] prm,        // A5 words 0..4
	output            busy,

	output reg  [6:0] rc_addr,    // acc_recon read port (data next clock)
	input      [63:0] rc_word,

	output reg        wb_we,
	output reg [AW-1:0] wb_addr,
	output reg [63:0] wb_din,
	input             mem_busy,
	input             mc_rd       // the MC has a request: no new write
);

// ---------------------------------------------------------------- tables

function [7:0] alpha_t(input [5:0] i);
	case (i)
		16: alpha_t = 4;   17: alpha_t = 4;   18: alpha_t = 5;   19: alpha_t = 6;
		20: alpha_t = 7;   21: alpha_t = 8;   22: alpha_t = 9;   23: alpha_t = 10;
		24: alpha_t = 12;  25: alpha_t = 13;  26: alpha_t = 15;  27: alpha_t = 17;
		28: alpha_t = 20;  29: alpha_t = 22;  30: alpha_t = 25;  31: alpha_t = 28;
		32: alpha_t = 32;  33: alpha_t = 36;  34: alpha_t = 40;  35: alpha_t = 45;
		36: alpha_t = 50;  37: alpha_t = 56;  38: alpha_t = 63;  39: alpha_t = 71;
		40: alpha_t = 80;  41: alpha_t = 90;  42: alpha_t = 101; 43: alpha_t = 113;
		44: alpha_t = 127; 45: alpha_t = 144; 46: alpha_t = 162; 47: alpha_t = 182;
		48: alpha_t = 203; 49: alpha_t = 226; 50: alpha_t = 255; 51: alpha_t = 255;
		default: alpha_t = 0;
	endcase
endfunction

function [4:0] beta_t(input [5:0] i);
	case (i)
		16, 17, 18:         beta_t = 2;
		19, 20, 21, 22:     beta_t = 3;
		23, 24, 25:         beta_t = 4;
		26, 27:             beta_t = 6;
		28, 29:             beta_t = 7;
		30, 31:             beta_t = 8;
		32, 33:             beta_t = 9;
		34, 35:             beta_t = 10;
		36, 37:             beta_t = 11;
		38, 39:             beta_t = 12;
		40, 41:             beta_t = 13;
		42, 43:             beta_t = 14;
		44, 45:             beta_t = 15;
		46, 47:             beta_t = 16;
		48, 49:             beta_t = 17;
		50, 51:             beta_t = 18;
		default:            beta_t = 0;
	endcase
endfunction

// tc0 for bS 1, 2, 3: {bS3, bS2, bS1}
function [14:0] tc0_t(input [5:0] i);
	case (i)
		17, 18, 19, 20: tc0_t = {5'd1, 5'd0, 5'd0};
		21, 22:         tc0_t = {5'd1, 5'd1, 5'd0};
		23, 24, 25, 26: tc0_t = {5'd1, 5'd1, 5'd1};
		27, 28, 29, 30: tc0_t = {5'd2, 5'd1, 5'd1};
		31, 32:         tc0_t = {5'd3, 5'd2, 5'd1};
		33:             tc0_t = {5'd3, 5'd2, 5'd2};
		34:             tc0_t = {5'd4, 5'd2, 5'd2};
		35, 36:         tc0_t = {5'd4, 5'd3, 5'd2};
		37:             tc0_t = {5'd5, 5'd3, 5'd3};
		38, 39:         tc0_t = {5'd6, 5'd4, 5'd3};
		40:             tc0_t = {5'd7, 5'd5, 5'd4};
		41:             tc0_t = {5'd8, 5'd5, 5'd4};
		42:             tc0_t = {5'd9, 5'd6, 5'd4};
		43:             tc0_t = {5'd10, 5'd7, 5'd5};
		44:             tc0_t = {5'd11, 5'd8, 5'd6};
		45:             tc0_t = {5'd13, 5'd8, 5'd6};
		46:             tc0_t = {5'd14, 5'd10, 5'd7};
		47:             tc0_t = {5'd16, 5'd11, 5'd8};
		48:             tc0_t = {5'd18, 5'd12, 5'd9};
		49:             tc0_t = {5'd20, 5'd13, 5'd10};
		50:             tc0_t = {5'd23, 5'd15, 5'd11};
		51:             tc0_t = {5'd25, 5'd17, 5'd13};
		default:        tc0_t = 0;
	endcase
endfunction

function [5:0] idx(input [5:0] qp, input [4:0] off);
	reg signed [7:0] t;
	begin
		t = $signed({2'b00, qp}) + $signed({{3{off[4]}}, off});
		idx = t < 0 ? 6'd0 : (t > 51 ? 6'd51 : t[5:0]);
	end
endfunction

// ---------------------------------------------------------------- filters
// luma line: v = p3 p2 p1 p0 q0 q1 q2 q3 (bytes 0..7), in two clocks: a
// (comparisons, sums) then b (clips, choice). Bundle of a:
//  0 filter, 1 strong (bS 4), 2 ap, 3 aq, 4 strong condition, 9:5 tc0,
//  14:10 tc, 24:15 delta, 34:25 p1 change, 44:35 q1 change,
//  52:45 p0s, 60:53 p1s, 68:61 p2s, 76:69 q0s, 84:77 q1s, 92:85 q2s,
//  100:93 p0 weak, 108:101 q0 weak
function [108:0] lfa(input [63:0] v, input [7:0] al, input [4:0] be, input [4:0] tc0, input [2:0] bs);
	reg [9:0] p3, p2, p1, p0, q0, q1, q2, q3;
	reg signed [10:0] d01, dp10, dq10, dp20, dq20;
	reg fl, ap, aq, sp;
	reg signed [12:0] dl;
	reg signed [11:0] dp, dq;
	reg [9:0] av;
	begin
		p3 = v[7:0];   p2 = v[15:8];  p1 = v[23:16]; p0 = v[31:24];
		q0 = v[39:32]; q1 = v[47:40]; q2 = v[55:48]; q3 = v[63:56];
		d01  = $signed({1'b0, p0}) - $signed({1'b0, q0});
		dp10 = $signed({1'b0, p1}) - $signed({1'b0, p0});
		dq10 = $signed({1'b0, q1}) - $signed({1'b0, q0});
		dp20 = $signed({1'b0, p2}) - $signed({1'b0, p0});
		dq20 = $signed({1'b0, q2}) - $signed({1'b0, q0});
		if (d01 < 0) d01 = -d01;
		if (dp10 < 0) dp10 = -dp10;
		if (dq10 < 0) dq10 = -dq10;
		if (dp20 < 0) dp20 = -dp20;
		if (dq20 < 0) dq20 = -dq20;
		fl = bs != 0 && d01 < al && dp10 < be && dq10 < be;
		ap = dp20 < be;
		aq = dq20 < be;
		sp = d01 < ((al >> 2) + 2);
		dl = ($signed({3'b000, q0}) - $signed({3'b000, p0})) * 4 + ($signed({3'b000, p1}) - $signed({3'b000, q1})) + 4;
		dl = dl >>> 3;
		av = (p0 + q0 + 10'd1) >> 1;
		dp = ($signed({2'b00, p2}) + $signed({2'b00, av}) - $signed({1'b0, p1, 1'b0})) >>> 1;
		dq = ($signed({2'b00, q2}) + $signed({2'b00, av}) - $signed({1'b0, q1, 1'b0})) >>> 1;
		lfa[0] = fl;
		lfa[1] = bs == 4;
		lfa[2] = ap;
		lfa[3] = aq;
		lfa[4] = sp;
		lfa[9:5] = tc0;
		lfa[14:10] = tc0 + ap + aq;
		lfa[24:15] = dl[9:0];
		lfa[34:25] = dp[9:0];
		lfa[44:35] = dq[9:0];
		lfa[52:45] = (p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3;
		lfa[60:53] = (p2 + p1 + p0 + q0 + 2) >> 2;
		lfa[68:61] = (2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3;
		lfa[76:69] = (p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3;
		lfa[84:77] = (p0 + q0 + q1 + q2 + 2) >> 2;
		lfa[92:85] = (2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3;
		lfa[100:93] = (2 * p1 + p0 + q1 + 2) >> 2;
		lfa[108:101] = (2 * q1 + q0 + p1 + 2) >> 2;
	end
endfunction

function signed [9:0] clipt(input signed [9:0] d, input [4:0] t);
	clipt = d > $signed({5'd0, t}) ? $signed({5'd0, t}) : (d < -$signed({5'd0, t}) ? -$signed({5'd0, t}) : d);
endfunction

function [7:0] clip8(input signed [10:0] v);
	clip8 = v < 0 ? 8'd0 : (v > 255 ? 8'd255 : v[7:0]);
endfunction

function [63:0] lfb(input [63:0] v, input [108:0] a);
	reg [7:0] p2, p1, p0, q0, q1, q2;
	reg signed [9:0] d;
	begin
		p2 = v[15:8];  p1 = v[23:16]; p0 = v[31:24];
		q0 = v[39:32]; q1 = v[47:40]; q2 = v[55:48];
		lfb = v;
		if (a[0]) begin
			if (a[1]) begin
				if (a[4]) begin
					if (a[2]) begin
						lfb[31:24] = a[52:45];
						lfb[23:16] = a[60:53];
						lfb[15:8]  = a[68:61];
					end
					else lfb[31:24] = a[100:93];
					if (a[3]) begin
						lfb[39:32] = a[76:69];
						lfb[47:40] = a[84:77];
						lfb[55:48] = a[92:85];
					end
					else lfb[39:32] = a[108:101];
				end
				else begin
					lfb[31:24] = a[100:93];
					lfb[39:32] = a[108:101];
				end
			end
			else begin
				d = clipt($signed(a[24:15]), a[14:10]);
				lfb[31:24] = clip8($signed({3'b000, p0}) + d);
				lfb[39:32] = clip8($signed({3'b000, q0}) - d);
				if (a[2]) lfb[23:16] = p1 + clipt($signed(a[34:25]), a[9:5]);
				if (a[3]) lfb[47:40] = q1 + clipt($signed(a[44:35]), a[9:5]);
			end
		end
	end
endfunction

// chroma line: v = p1 p0 q0 q1. Bundle: 0 filter, 1 strong, 6:2 tc,
// 16:7 delta, 24:17 p0 weak, 32:25 q0 weak
function [32:0] cfa(input [31:0] v, input [7:0] al, input [4:0] be, input [4:0] tc0, input [2:0] bs);
	reg [9:0] p1, p0, q0, q1;
	reg signed [10:0] d01, dp10, dq10;
	reg signed [12:0] dl;
	begin
		p1 = v[7:0]; p0 = v[15:8]; q0 = v[23:16]; q1 = v[31:24];
		d01  = $signed({1'b0, p0}) - $signed({1'b0, q0});
		dp10 = $signed({1'b0, p1}) - $signed({1'b0, p0});
		dq10 = $signed({1'b0, q1}) - $signed({1'b0, q0});
		if (d01 < 0) d01 = -d01;
		if (dp10 < 0) dp10 = -dp10;
		if (dq10 < 0) dq10 = -dq10;
		dl = ($signed({3'b000, q0}) - $signed({3'b000, p0})) * 4 + ($signed({3'b000, p1}) - $signed({3'b000, q1})) + 4;
		dl = dl >>> 3;
		cfa[0] = bs != 0 && d01 < al && dp10 < be && dq10 < be;
		cfa[1] = bs == 4;
		cfa[6:2] = tc0 + 1'd1;
		cfa[16:7] = dl[9:0];
		cfa[24:17] = (2 * p1 + p0 + q1 + 2) >> 2;
		cfa[32:25] = (2 * q1 + q0 + p1 + 2) >> 2;
	end
endfunction

function [31:0] cfb(input [31:0] v, input [32:0] a);
	reg signed [9:0] d;
	begin
		cfb = v;
		if (a[0]) begin
			if (a[1]) begin
				cfb[15:8] = a[24:17];
				cfb[23:16] = a[32:25];
			end
			else begin
				d = clipt($signed(a[16:7]), a[6:2]);
				cfb[15:8] = clip8($signed({3'b000, v[15:8]}) + d);
				cfb[23:16] = clip8($signed({3'b000, v[23:16]}) - d);
			end
		end
	end
endfunction

// -------------------------------------------------------- parameters
// of the macroblock being loaded / filtered (latched at start)

reg        p_on, p_sv, p_sh;
reg  [4:0] p_offa, p_offb;
reg  [5:0] p_qp[9];           // plane * 3 + class (0 left, 1 top, 2 inside)
reg [31:0] p_bsv, p_bsh;      // edge e segment s at (e * 4 + s) * 2
reg  [7:0] t_al[9];
reg  [4:0] t_be[9];
reg [14:0] t_tc[9];

integer k;
always @(posedge clk) begin
	for (k = 0; k < 9; k = k + 1) begin
		t_al[k] <= alpha_t(idx(p_qp[k], p_offa));
		t_be[k] <= beta_t(idx(p_qp[k], p_offb));
		t_tc[k] <= tc0_t(idx(p_qp[k], p_offa));
	end
end

// strength of a segment: dir 0 vertical, edge e (luma numbering), segment s
function [2:0] bsof(input dir, input [1:0] e, input [1:0] s);
	reg [1:0] b;
	begin
		b = dir ? p_bsh[{e, s, 1'b0} +: 2] : p_bsv[{e, s, 1'b0} +: 2];
		bsof = !p_on ? 3'd0 : (e == 0 && (dir ? p_sh : p_sv)) ? 3'd4 : {1'b0, b};
	end
endfunction

function [4:0] tcof(input [14:0] t, input [2:0] bs);
	tcof = bs == 1 ? t[4:0] : (bs == 2 ? t[9:5] : (bs == 3 ? t[14:10] : 5'd0));
endfunction

// ------------------------------------------------------------- the area

reg [191:0] Y[20];            // rows -4..15, pixel of column c (-8..15) at (c + 8) * 8
reg [127:0] CU[10], CV[10];   // rows -2..7, column c (-8..7) at (c + 8) * 8

// macroblock in the area (written out by the next ROT), and the next one
reg        cur_ok;
reg  [7:0] cur_x, cur_y, cur_slot, cur_w, cur_h;
reg        n_load, n_buf;
reg  [7:0] n_x, n_y, n_slot, n_w, n_h;
wire       cur_end = cur_x == cur_w - 1'd1;   // last of its row

// line buffers: luma {mb x, row 0..3, half}, chroma {mb x, row 0..1, plane}
reg        lb_we, clb_we;
reg  [9:0] lb_wa, lb_ra;
reg  [8:0] clb_wa, clb_ra;
reg [63:0] lb_wd, clb_wd;
wire [63:0] lb_q, clb_q;
acc_ram #(.W(64), .A(10)) lbuf (.clk(clk), .we(lb_we), .wa(lb_wa), .wd(lb_wd), .ra(lb_ra), .q(lb_q));
acc_ram #(.W(64), .A(9)) clbuf (.clk(clk), .we(clb_we), .wa(clb_wa), .wd(clb_wd), .ra(clb_ra), .q(clb_q));

wire [AW-1:0] slot_base = AW'(cur_slot) << SLOT_L2;
function [AW-1:0] yaddr(input [11:0] row, input [8:0] col);
	yaddr = slot_base + (AW'(row) << YSTRIDE_L2) + col;
endfunction
function [AW-1:0] caddr(input plane, input [10:0] row, input [7:0] col);
	caddr = slot_base + (plane ? AW'(VOFF) : AW'(UOFF)) + (AW'(row) << CSTRIDE_L2) + col;
endfunction

// ------------------------------------------------------------ sequencer

localparam Q_IDLE = 0, Q_LROT = 1, Q_CROT = 2, Q_FLY = 3, Q_FLC = 4, Q_WAIT = 5;
reg  [2:0] q;
reg  [4:0] ri;                // row of the turn (luma 0..19, chroma 0..9)
reg  [1:0] rj;                // read clock of the row: 0, 1 addresses; 2 second
                              // word arrives (used live); 3 both held
reg  [2:0] wi;                // write item of the row
reg  [2:0] li;                // line buffer write item of the row
reg [63:0] nw0, nw1;          // the next macroblock's words of the row
reg        fin_pend;

// filters: running and clock counters
reg        lv_on, lh_on, cv_on, ch_on;
reg  [4:0] lt, ct;

wire   filt_on = lv_on || lh_on || cv_on || ch_on;
assign busy = q != Q_IDLE || filt_on || start || fin || fin_pend;

// write items of the turning row (luma): 0 L, 1 C0, 2 C1 to DDR
wire        rows_out = ri < 16 && (ri >= 4 || cur_y != 0);
wire  [2:0] lwant = !cur_ok ? 3'd0 : {rows_out && cur_end, rows_out, rows_out && cur_x != 0};
// line buffer items (rows 16..19): 0 C0 -> x, 1 L -> x - 1, 2 C1 -> x (row end)
wire  [2:0] lbwant = !cur_ok || ri < 16 ? 3'd0 : {cur_end, cur_x != 0, 1'b1};
// chroma: 0 UL, 1 VL, 2 UC, 3 VC to DDR; line buffer rows 8, 9: same items
wire        crows_out = ri < 8 && (ri >= 2 || cur_y != 0);
wire  [3:0] cwant = !cur_ok || !crows_out ? 4'd0 : {cur_end, cur_end, cur_x != 0, cur_x != 0};
wire  [3:0] cbwant = !cur_ok || ri < 8 ? 4'd0 : {cur_end, cur_end, cur_x != 0, cur_x != 0};

// first item at or after i of a want mask (4 = none)
function [2:0] nexti(input [3:0] m, input [2:0] i);
	nexti = (i <= 0 && m[0]) ? 3'd0 : (i <= 1 && m[1]) ? 3'd1 : (i <= 2 && m[2]) ? 3'd2 : (i <= 3 && m[3]) ? 3'd3 : 3'd4;
endfunction

wire  [2:0] lw_i  = nexti({1'b0, lwant}, wi);
wire  [2:0] lb_i  = nexti({1'b0, lbwant}, li);
wire  [2:0] cw_i  = nexti(cwant, wi);
wire  [2:0] cb_i  = nexti(cbwant, li);

// the DDR port: a new word goes out when the last one is taken and the MC
// has nothing waiting
wire        port_free = (!wb_we || !mem_busy) && !mc_rd;

// flush counters
reg [10:0] fk;                // word of the row
reg  [1:0] fr;                // row
reg        fp;                // chroma plane
reg  [1:0] fs;                // 0 read, 1 data, 2 write

// read addresses (combinational: RAM data on the next clock)
always @* begin
	rc_addr = {n_buf, 6'd0};
	lb_ra = 0;
	clb_ra = 0;
	if (q == Q_LROT) begin
		rc_addr = {n_buf, 1'b0, ri[3:0] - 4'd4, rj[0]};
		lb_ra = {n_x[6:0], ri[1:0], rj[0]};
	end
	else if (q == Q_CROT) begin
		rc_addr = {n_buf, 2'b10, rj[0], ri[2:0] - 3'd2};
		clb_ra = {n_x[6:0], ri[0], rj[0]};
	end
	else if (q == Q_FLY) lb_ra = {fk[7:1], fr, fk[0]};
	else if (q == Q_FLC) clb_ra = {fk[6:0], fr[0], fp};
end

// the word read last clock: line buffer above the picture rows, or recon
wire [63:0] lrd = ri < 4 ? (n_y != 0 ? lb_q : 64'd0) : rc_word;
wire [63:0] crd = ri < 2 ? (n_y != 0 ? clb_q : 64'd0) : rc_word;

// ------------------------------------------------------- filter rings

// luma V: stages a/b of edges 0..3
reg [191:0] va_r[4], vb_r[4];
reg [108:0] va_b[4];
// luma H
reg [159:0] ha_c[4], hb_c[4];
reg [108:0] ha_b[4];
// chroma V and H, per plane (0 U, 1 V)
reg [127:0] cva_r[2][2], cvb_r[2][2];
reg  [32:0] cva_b[2][2];
reg  [79:0] cha_c[2][2], chb_c[2][2];
reg  [32:0] cha_b[2][2];

// columns entering the H rings
reg [159:0] ycol;
reg  [79:0] ucol, vcol;
integer r;
always @* begin
	for (r = 0; r < 20; r = r + 1) ycol[r * 8 +: 8] = Y[r][64 +: 8];
	for (r = 0; r < 10; r = r + 1) begin
		ucol[r * 8 +: 8] = CU[r][64 +: 8];
		vcol[r * 8 +: 8] = CV[r][64 +: 8];
	end
end

// parameters of a stage: line j = t - 2 e of the turn (valid 0..15)
function [2:0] lbs(input dir, input [1:0] e, input [4:0] t);
	reg signed [6:0] j;
	begin
		j = $signed({2'b00, t}) - $signed({4'b0000, e, 1'b0});
		lbs = (j < 0 || j > 15) ? 3'd0 : bsof(dir, e, j[3:2]);
	end
endfunction
function [2:0] cbs(input dir, input e, input [4:0] t);
	reg signed [6:0] j;
	begin
		j = $signed({2'b00, t}) - $signed({5'b00000, e, 1'b0});
		cbs = (j < 0 || j > 7) ? 3'd0 : bsof(dir, {e, 1'b0}, j[2:1]);
	end
endfunction

integer e, pl;
reg [2:0] sb;
reg [3:0] ci;
always @(posedge clk) begin
	lb_we <= 0;
	clb_we <= 0;

	// ---------------------------------------------------------- luma V
	if (lv_on) begin
		lt <= lt + 1'd1;
		for (e = 0; e < 4; e = e + 1) begin
			sb = lbs(0, e[1:0], lt);
			ci = e == 0 ? 4'd0 : 4'd2;
			va_r[e] <= e == 0 ? Y[4] : vb_r[e - 1];
			va_b[e] <= lfa((e == 0 ? Y[4] : vb_r[e - 1]) >> ((4 + 4 * e) * 8), t_al[ci], t_be[ci], tcof(t_tc[ci], sb), sb);
			vb_r[e] <= va_r[e];
			vb_r[e][(4 + 4 * e) * 8 +: 64] <= lfb(va_r[e][(4 + 4 * e) * 8 +: 64], va_b[e]);
		end
		for (r = 4; r < 19; r = r + 1) Y[r] <= Y[r + 1];
		Y[19] <= vb_r[3];
		if (lt == 23) begin
			// the last move: the ring is back in place
			lv_on <= 0;
			lh_on <= 1;
			lt <= 0;
		end
	end

	// ---------------------------------------------------------- luma H
	if (lh_on) begin
		lt <= lt + 1'd1;
		for (e = 0; e < 4; e = e + 1) begin
			sb = lbs(1, e[1:0], lt);
			ci = e == 0 ? 4'd1 : 4'd2;
			ha_c[e] <= e == 0 ? ycol : hb_c[e - 1];
			ha_b[e] <= lfa((e == 0 ? ycol : hb_c[e - 1]) >> (4 * e * 8), t_al[ci], t_be[ci], tcof(t_tc[ci], sb), sb);
			hb_c[e] <= ha_c[e];
			hb_c[e][4 * e * 8 +: 64] <= lfb(ha_c[e][4 * e * 8 +: 64], ha_b[e]);
		end
		for (r = 0; r < 20; r = r + 1) Y[r][191:64] <= {hb_c[3][r * 8 +: 8], Y[r][191:72]};
		if (lt == 23) begin
			lh_on <= 0;
			lt <= 0;
		end
	end

	// -------------------------------------------------------- chroma V
	if (cv_on) begin
		ct <= ct + 1'd1;
		for (pl = 0; pl < 2; pl = pl + 1)
			for (e = 0; e < 2; e = e + 1) begin
				sb = cbs(0, e[0], ct);
				ci = (pl + 1) * 3 + (e == 0 ? 0 : 2);
				cva_r[pl][e] <= e == 0 ? (pl ? CV[2] : CU[2]) : cvb_r[pl][e - 1];
				cva_b[pl][e] <= cfa((e == 0 ? (pl ? CV[2] : CU[2]) : cvb_r[pl][e - 1]) >> ((6 + 4 * e) * 8),
				                    t_al[ci], t_be[ci], tcof(t_tc[ci], sb), sb);
				cvb_r[pl][e] <= cva_r[pl][e];
				cvb_r[pl][e][(6 + 4 * e) * 8 +: 32] <= cfb(cva_r[pl][e][(6 + 4 * e) * 8 +: 32], cva_b[pl][e]);
			end
		for (r = 2; r < 9; r = r + 1) begin
			CU[r] <= CU[r + 1];
			CV[r] <= CV[r + 1];
		end
		CU[9] <= cvb_r[0][1];
		CV[9] <= cvb_r[1][1];
		if (ct == 11) begin
			cv_on <= 0;
			ch_on <= 1;
			ct <= 0;
		end
	end

	// -------------------------------------------------------- chroma H
	if (ch_on) begin
		ct <= ct + 1'd1;
		for (pl = 0; pl < 2; pl = pl + 1)
			for (e = 0; e < 2; e = e + 1) begin
				sb = cbs(1, e[0], ct);
				ci = (pl + 1) * 3 + (e == 0 ? 1 : 2);
				cha_c[pl][e] <= e == 0 ? (pl ? vcol : ucol) : chb_c[pl][e - 1];
				cha_b[pl][e] <= cfa((e == 0 ? (pl ? vcol : ucol) : chb_c[pl][e - 1]) >> (4 * e * 8),
				                    t_al[ci], t_be[ci], tcof(t_tc[ci], sb), sb);
				chb_c[pl][e] <= cha_c[pl][e];
				chb_c[pl][e][4 * e * 8 +: 32] <= cfb(cha_c[pl][e][4 * e * 8 +: 32], cha_b[pl][e]);
			end
		for (r = 0; r < 10; r = r + 1) begin
			CU[r][127:64] <= {chb_c[0][1][r * 8 +: 8], CU[r][127:72]};
			CV[r][127:64] <= {chb_c[1][1][r * 8 +: 8], CV[r][127:72]};
		end
		if (ct == 11) begin
			ch_on <= 0;
			ct <= 0;
		end
	end

	// ------------------------------------------------------ the port
	if (!wb_we || !mem_busy) wb_we <= 0;

	case (q)
	Q_IDLE: begin
		if (start) begin
			n_load <= 1;
			n_buf <= sbuf;
			n_x <= mbx;
			n_y <= mby;
			n_slot <= slot;
			n_w <= wmb;
			n_h <= hmb;
			p_on <= prm_on;
			p_sv <= prm[0];
			p_sh <= prm[1];
			p_offa <= prm[6:2];
			p_offb <= prm[11:7];
			for (k = 0; k < 5; k = k + 1) p_qp[k] <= prm[32 + 6 * k +: 6];
			for (k = 0; k < 4; k = k + 1) p_qp[5 + k] <= prm[64 + 6 * k +: 6];
			p_bsv <= prm[127:96];
			p_bsh <= prm[159:128];
			q <= Q_LROT;
			ri <= 0; rj <= 0; wi <= 0; li <= 0;
		end
		else if (fin_pend && !filt_on) begin
			fin_pend <= 0;
			n_load <= 0;
			q <= cur_ok ? Q_LROT : Q_WAIT;
			ri <= 0; rj <= 0; wi <= 0; li <= 0;
		end
	end

	// one row of the luma turn: reads (rj), DDR writes (wi), line buffer
	// writes (li), then the move
	Q_LROT: begin
		if (rj != 3) rj <= rj + 1'd1;
		if (rj == 1) nw0 <= lrd;
		if (rj == 2) nw1 <= lrd;
		if (lw_i != 4 && port_free) begin
			wb_we <= 1;
			wb_addr <= yaddr({cur_y, 4'd0} + ri - 12'd4, {cur_x, 1'b0} + lw_i[1:0] - 9'd1);
			wb_din <= Y[0][lw_i * 64 +: 64];
			wi <= lw_i + 1'd1;
		end
		if (lb_i != 4) begin
			lb_we <= 1;
			lb_wa <= {(lb_i == 1 ? cur_x - 1'd1 : cur_x), ri[1:0], lb_i != 0};
			lb_wd <= Y[0][(lb_i == 0 ? 1 : (lb_i == 1 ? 0 : 2)) * 64 +: 64];
			li <= lb_i + 1'd1;
		end
		// the move, once the row is read and written
		if (rj[1] && lw_i == 4 && lb_i == 4) begin
			for (r = 0; r < 19; r = r + 1) Y[r] <= Y[r + 1];
			Y[19] <= {rj[0] ? nw1 : lrd, nw0, Y[0][191:128]};
			rj <= 0; wi <= 0; li <= 0;
			ri <= ri + 1'd1;
			if (ri == 19) begin
				ri <= 0;
				q <= Q_CROT;
				if (n_load) begin lv_on <= 1; lt <= 0; end
			end
		end
	end

	Q_CROT: begin
		if (rj != 3) rj <= rj + 1'd1;
		if (rj == 1) nw0 <= crd;
		if (rj == 2) nw1 <= crd;
		if (cw_i != 4 && port_free) begin
			wb_we <= 1;
			wb_addr <= caddr(cw_i[0], {cur_y, 3'd0} + ri - 11'd2, cur_x + cw_i[1] - 8'd1);
			wb_din <= cw_i[0] ? CV[0][cw_i[1] * 64 +: 64] : CU[0][cw_i[1] * 64 +: 64];
			wi <= cw_i + 1'd1;
		end
		if (cb_i != 4) begin
			clb_we <= 1;
			clb_wa <= {(cb_i[1] ? cur_x : cur_x - 1'd1), ri[0], cb_i[0]};
			clb_wd <= cb_i[0] ? CV[0][cb_i[1] * 64 +: 64] : CU[0][cb_i[1] * 64 +: 64];
			li <= cb_i + 1'd1;
		end
		if (rj[1] && cw_i == 4 && cb_i == 4) begin
			for (r = 0; r < 9; r = r + 1) begin
				CU[r] <= CU[r + 1];
				CV[r] <= CV[r + 1];
			end
			CU[9] <= {nw0, CU[0][127:64]};
			CV[9] <= {rj[0] ? nw1 : crd, CV[0][127:64]};
			rj <= 0; wi <= 0; li <= 0;
			ri <= ri + 1'd1;
			if (ri == 9) begin
				ri <= 0;
				if (n_load) begin
					cv_on <= 1;
					ct <= 0;
					cur_ok <= 1;
					cur_x <= n_x;
					cur_y <= n_y;
					cur_slot <= n_slot;
					cur_w <= n_w;
					cur_h <= n_h;
					q <= Q_IDLE;
				end
				else begin
					// end of picture: the bottom rows from the line buffers
					q <= Q_FLY;
					fk <= 0; fr <= 0; fp <= 0; fs <= 0;
				end
			end
		end
	end

	// flush: luma rows h*16-4.. from the line buffer, then chroma
	Q_FLY: begin
		if (fs == 0) fs <= 1;
		else if (fs == 1) begin nw0 <= lb_q; fs <= 2; end
		else if (port_free) begin
			wb_we <= 1;
			wb_addr <= yaddr({cur_h, 4'd0} - 12'd4 + fr, fk[8:0]);
			wb_din <= nw0;
			fs <= 0;
			fk <= fk + 1'd1;
			if (fk == {cur_w, 1'b0} - 1'd1) begin
				fk <= 0;
				fr <= fr + 1'd1;
				if (fr == 3) begin fr <= 0; q <= Q_FLC; end
			end
		end
	end

	Q_FLC: begin
		if (fs == 0) fs <= 1;
		else if (fs == 1) begin nw0 <= clb_q; fs <= 2; end
		else if (port_free) begin
			wb_we <= 1;
			wb_addr <= caddr(fp, {cur_h, 3'd0} - 11'd2 + fr, fk[7:0]);
			wb_din <= nw0;
			fs <= 0;
			fp <= ~fp;
			if (fp) begin
				fk <= fk + 1'd1;
				if (fk == cur_w - 1'd1) begin
					fk <= 0;
					fr <= fr + 1'd1;
					if (fr == 1) begin q <= Q_WAIT; cur_ok <= 0; end
				end
			end
		end
	end

	// the last word is taken
	Q_WAIT: if (!wb_we || !mem_busy) q <= Q_IDLE;

	default: q <= Q_IDLE;
	endcase

	if (fin) fin_pend <= 1;

	if (reset) begin
		q <= Q_IDLE;
		cur_ok <= 0;
		wb_we <= 0;
		lv_on <= 0;
		lh_on <= 0;
		cv_on <= 0;
		ch_on <= 0;
		fin_pend <= 0;
	end
end

endmodule
