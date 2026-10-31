// MiSTer MediaCenter "zero-copy": the core copies a decoded picture into the
// frame buffer instead of the ARM.
//
// The player decodes into memory that Linux keeps in place, and gives this
// block, through a small mailbox in DDR, the physical address of each 4 KB
// page of that memory (a page table). The block then builds the frame
// buffer page, line by line: it reads a line of Y and a line of U and V
// (planar 4:2:0, as the decoder writes them), narrows them by 3:2 (1920 to
// 1280: every 3 source pixels give 2, the nearer one weighing 2/3, the other
// 1/3, exactly as the player's pack_yuyv_32), or by 2:1 when asked (1920 to
// 960, for the 4:3 format: each pair of pixels averaged), and writes the YUYV
// line. The ARM only waits for it.
//
// Mailbox (MBOX, 32-bit words, written by the player through /dev/mem):
//   0 magic "MCZC"           1 sequence: a new value starts a picture
//   2 frame buffer page (physical byte address)
//   3 [15:0] frame buffer stride (bytes)   [31:16] lines to make
//   4 page table (physical byte address): one 32-bit page number per page
//   5 [15:0] pages in the table            [31:16] 64-bit words per output line
//   6 Y offset in the picture memory (bytes)
//   7 Y bytes between output lines (2 source lines when half the lines are made)
//   8 U offset    9 V offset
//  10 [15:0] U/V bytes between output lines  [16] U/V line = output line / 2
//     [17] narrow by 2:1 instead of 3:2 (at most 256 output words a line)
//     [18] the player's controls are blended over the lines from word 12 on
//     [19] resample (any width, see below) instead of 3:2 or 2:1
//  11 controls (physical byte address, outside Linux): for each blended
//     line, one 64-bit word per output word, bytes a0 g0 a1 g1 a2 g2 a3 g3
//     (coverage a and grey g = 16 + v * 219 / 255 of its 4 pixels)
//  12 [15:0] first line with the controls
//  13 resample: [15:0] source pixels a line  [31:16] source lines
// Resample, as the player's pack_yuyv_map and yuv_draw: output pixel x takes
// source pixel s = x * source pixels / output pixels (whole part) and each
// output pair the colour of s / 2 of its first pixel; output line r takes
// source line l = r * source lines / output lines (word 7 apart), and the
// colour line l / 2 (word 10 bit 16) or l (word 10 apart). Output pixels
// from source pixels up to twice as many (2048 at most), and the same for
// the lines. The lines' places are counted one by one (no division).
// Blending, as the player's yuv_band: Y = (Y * (255 - a) + g * a + 127) / 255
// for each pixel; U and V the same toward 128 with (a0 + a1 + 1) / 2 of
// their two pixels.
// Answer (written by the block, 64 bytes on):
//  16 clock cycles spent   17 status: 0 done, 1 refused (bad values)
//  18 the sequence done (written last, in a word of its own)
//
// The bus is shared with mc_out (the CRT output), which always goes first:
// this block only starts a transfer while mc_out has nothing to fetch, and
// gives the bus back between its bursts (at most 32 words).

// The 2/3 and 1/3 weights are done in logic, leaving the DSP blocks to the
// HDMI scaler (with them, its timing failed).
(* multstyle = "logic" *)
module mc_zc #(parameter [31:0] MBOX = 32'h23F00000)
(
	input             clk,
	input             enable,       // the bus is the frame buffer's (FB mode)

	output reg [28:0] ddr_addr,
	output reg  [7:0] ddr_burst,
	output reg        ddr_rd = 0,
	output reg        ddr_we = 0,
	output     [63:0] ddr_din,
	input             ddr_busy,
	input      [63:0] ddr_dout,
	input             ddr_dout_ready, // only for this block's reads

	input             grant,        // a new transfer may start
	output            want,         // has a transfer to start
	output            idle          // nothing under way
);

localparam BURST = 32;
localparam [31:0] MAGIC = 32'h435A434D; // "MCZC"

// ---------------------------------------------------------------- state
localparam S_WAIT   = 0,  // waiting to look at the mailbox
           S_HDR    = 1,  // reading the mailbox
           S_HDR2   = 2,  // reading it again (the values written before the sequence)
           S_CHECK  = 3,
           S_TABLE  = 4,  // reading the page table
           S_ROWS   = 5,  // making the lines
           S_ANSWER = 6,  // writing the answer
           S_DEAD   = 7,  // the memory never answered: stay out of the way
           S_MAP    = 8;  // resample: the source pixel of each output pixel

reg  [3:0] state = S_WAIT;
reg [11:0] poll = 0;
reg [31:0] hdr[16];
reg [31:0] last_seq = 0;
reg        primed = 0;     // last_seq holds the mailbox's value at start
reg [31:0] cycles = 0;
reg        refused = 0;

// read engine: a list of reads whose words arrive in order
reg        rd_go = 0;      // start the reads set below
reg        rd_on = 0;
reg  [1:0] rd_kind;        // 0 mailbox, 1 table, 2 line
reg  [1:0] rd_seg;         // line: 0 Y, 1 U, 2 V
reg [31:0] rd_off;         // line: offset in the picture memory; else physical
reg [10:0] rd_left;        // words still to ask for in this segment
reg [11:0] pend = 0;       // words asked for, not arrived
reg [11:0] rx = 0;         // words arrived for the current list
reg        rd_bank;        // line buffer being filled
reg  [1:0] lk = 0;         // page table look-up steps

// write engine
reg        wr_on = 0;
reg [31:0] wr_addr;
reg [10:0] wr_left;        // words still to write
reg  [7:0] wr_beats = 0;   // words left in the current burst
reg  [9:0] wr_idx;         // next word of the line buffer
reg        wr_ans = 0;     // writing the answer, not a line

// lines
reg [15:0] rows, row_r, row_c, row_w;  // lines: in all, read, made, written
reg [31:0] y_off, u_off, v_off;        // where the next line to read starts
reg [31:0] rd_u, rd_v;                 // U and V of the line being read
reg [31:0] row_a;                      // frame buffer address of the next line to write                      // U/V line step: every line or every other one
reg  [9:0] ow;                         // output words a line
reg [10:0] ny, nc;                     // source words a line: Y, U (V)
reg [31:0] ov_a;                       // controls of the next blended line to read
reg        rd_band;                    // the line being read has the controls
reg [31:0] rd_o;                       // where its controls are

wire [31:0] y_step = hdr[7];
wire [15:0] c_step = hdr[10][15:0];
wire        c_half = hdr[10][16];
wire        n21    = hdr[10][17];   // 2:1 (the mailbox is not read again while lines are made)
wire        ov_on  = hdr[10][18];
wire [15:0] ov_y0  = hdr[12][15:0];
wire        rs     = hdr[10][19];   // resample
wire [11:0] rs_sw  = hdr[13][11:0]; // source pixels a line (checked: 2048 at most)
wire [15:0] rs_sh  = hdr[13][31:16];// source lines

// resample: for output word n, its first source pixel (y0) and how far the
// other three are from it (d1, d2, d3, at most 7)
reg  [19:0] xm[512];
reg  [19:0] xq;
reg  [11:0] mx_k;           // output pixel
reg  [10:0] mx_s;           // its source pixel
reg  [12:0] mx_f;           // x * source pixels - mx_s * output pixels
reg  [10:0] mx_y0;
reg   [5:0] mx_d;           // d1, d2 of the word being made
reg  [12:0] mx_w;           // output pixels a line
reg         xm_we = 0;
reg   [8:0] xm_wa;
reg  [19:0] xm_wd;
always @(posedge clk) if(xm_we) xm[xm_wa] <= xm_wd;
// resample: source line of the next line to read, and its fraction
reg  [15:0] v_l;
reg  [16:0] v_f;
// worked out a clock ahead (the lines are read many clocks apart): the
// step to the line after (0, 1 or 2 source lines, 0 to 2 colour lines)
// and the fraction there; and for the map, the source pixels less the
// output pixels once and twice
reg   [1:0] v_n, v_cn;
reg  [16:0] v_fn;
reg  [17:0] kv1, kv2;
reg  [13:0] kx1, kx2;
always @(posedge clk) begin
	reg [17:0] a, b, c;
	kv1 <= {2'b0, rs_sh} - {2'b0, rows};
	kv2 <= {2'b0, rs_sh} - {1'b0, rows, 1'b0};
	// from the mailbox, steady from S_CHECK on (mx_w is set there)
	kx1 <= {2'b0, rs_sw} - {2'b0, hdr[5][25:16], 2'b00};
	kx2 <= {2'b0, rs_sw} - {1'b0, hdr[5][25:16], 3'b000};
	a = {1'b0, v_f} + rs_sh;
	b = {1'b0, v_f} + kv1;
	c = {1'b0, v_f} + kv2;
	v_n  <= !c[17] ? 2'd2 : !b[17] ? 2'd1 : 2'd0;
	v_fn <= !c[17] ? c[16:0] : !b[17] ? b[16:0] : a[16:0];
	// colour line l / 2: one more when l odd and 1 more line, or 2 more
	v_cn <= !c_half ? (!c[17] ? 2'd2 : !b[17] ? 2'd1 : 2'd0) : (!c[17] || (!b[17] && v_l[0])) ? 2'd1 : 2'd0;
end

// ---------------------------------------------------------------- page table
reg  [31:0] tab_lo[1024], tab_hi[1024];
reg  [10:0] tab_n;
reg  [10:0] tab_ra;
reg  [31:0] tab_lq, tab_hq;
reg         tab_sel;
// read straight into a register each, so both are block RAM
always @(posedge clk) tab_lq <= tab_lo[tab_ra[10:1]];
always @(posedge clk) tab_hq <= tab_hi[tab_ra[10:1]];
always @(posedge clk) tab_sel <= tab_ra[0];
wire [31:0] tab_q = tab_sel ? tab_hq : tab_lq;

// written a clock after the word arrives
reg         twe = 0;
reg   [9:0] twa;
reg  [63:0] twd;
always @(posedge clk) if(twe) tab_lo[twa] <= twd[31:0];
always @(posedge clk) if(twe) tab_hi[twa] <= twd[63:32];

// ---------------------------------------------------------------- line buffers
// Two banks (one filled while the other is used). Y, U and V each split into
// their even and odd words, so a word and the next one are read together.
reg [63:0] ye[256], yo[256], ue[128], uo[128], ve[128], vo[128];

reg        bwe = 0;
reg  [1:0] bseg;
reg [10:0] bidx;
reg        bbank;
reg [63:0] bdata;
always @(posedge clk) if(bwe && bseg == 0 && !bidx[0]) ye[{bbank, bidx[7:1]}] <= bdata;
always @(posedge clk) if(bwe && bseg == 0 &&  bidx[0]) yo[{bbank, bidx[7:1]}] <= bdata;
always @(posedge clk) if(bwe && bseg == 1 && !bidx[0]) ue[{bbank, bidx[6:1]}] <= bdata;
always @(posedge clk) if(bwe && bseg == 1 &&  bidx[0]) uo[{bbank, bidx[6:1]}] <= bdata;
always @(posedge clk) if(bwe && bseg == 2 && !bidx[0]) ve[{bbank, bidx[6:1]}] <= bdata;
always @(posedge clk) if(bwe && bseg == 2 &&  bidx[0]) vo[{bbank, bidx[6:1]}] <= bdata;

// controls of the line, two banks like the others
reg [63:0] abuf[1024];
always @(posedge clk) if(bwe && bseg == 3) abuf[{bbank, bidx[8:0]}] <= bdata;

// output line
reg [63:0] obuf[512];
reg        owe = 0;
reg  [8:0] owa;
reg [63:0] owd;
always @(posedge clk) if(owe) obuf[owa] <= owd;

// ---------------------------------------------------------------- making a line
// Output word n takes Y bytes 6n..6n+5 and U, V bytes 3n..3n+2 of the line
// (2:1: Y bytes 8n..8n+7, U and V bytes 4n..4n+3).
reg        mk_on = 0;
reg        mk_bank;
reg  [9:0] mk_n;          // word being asked for
reg [12:0] mk_y;          // 6n
reg [11:0] mk_c;          // 3n
reg [63:0] ye_q, yo_q, ue_q, uo_q, ve_q, vo_q, ov_q;
reg        mk_blend;      // the line being made has the controls
reg        s2_v = 0, sa_v = 0, s3_v = 0, s4_v = 0;   // the steps after p1 (see "the controls")
reg  [8:0] s2_n, sa_n, s3_n, s4_n;
reg [63:0] s2_d, s2_ov, sa_d, sa_a, s3_d;
reg        sa_b, s3_b, s4_b;
reg [71:0] sa_g;          // 8 x 9 bits: g - p, signed
reg [127:0] sa_k, s3_k;   // 8 x 16 bits: 255 * p + 127
reg [103:0] s3_l, s3_h;   // 8 x 13 bits: (g - p) * low and high 4 bits of a, signed
reg [127:0] s4_s;         // 8 x 16 bits: p * (255 - a) + g * a + 127
reg        p1_v = 0;
reg  [8:0] p1_n;
reg        mk_go = 0;
reg  [2:0] p1_ys, p1_cs;   // byte of the first Y, U
reg        p1_yodd, p1_codd; // the first word is an odd one

// p0: the resample word of the map (a step for every mode)
reg        p0_v = 0;
reg  [9:0] p0_n;
reg [12:0] p0_y;
reg [11:0] p0_c;
always @(posedge clk) begin
	xq   <= xm[mk_n[8:0]];
	p0_v <= mk_on;
	p0_n <= mk_n;
	p0_y <= mk_y;
	p0_c <= mk_c;
end

reg  [2:0] p1_d1, p1_d2, p1_d3;  // resample: Y bytes 1, 2, 3 from the first
reg  [1:0] p1_dc;                // and the second U, V byte from the first
always @(posedge clk) begin
	reg [7:0] ya, yb;
	reg [6:0] ca, cb;
	reg [12:0] yy;
	reg [11:0] cc, c2;
	yy = rs ? {2'b0, xq[10:0]} : p0_y;
	cc = rs ? {2'b0, xq[10:1]} : p0_c;
	c2 = rs ? ({1'b0, xq[10:0]} + xq[16:14]) >> 1 : 12'd0;
	ya = yy[12:3];         // first Y word
	yb = ya + 1'd1;
	ca = cc[11:3];
	cb = ca + 1'd1;
	// even/odd words: the word a and a+1, one of them even
	ye_q <= ye[{mk_bank, ya[0] ? yb[7:1] : ya[7:1]}];
	yo_q <= yo[{mk_bank, ya[7:1]}];
	ue_q <= ue[{mk_bank, ca[0] ? cb[6:1] : ca[6:1]}];
	uo_q <= uo[{mk_bank, ca[6:1]}];
	ve_q <= ve[{mk_bank, ca[0] ? cb[6:1] : ca[6:1]}];
	vo_q <= vo[{mk_bank, ca[6:1]}];
	ov_q <= abuf[{mk_bank, p0_n[8:0]}];
	p1_v    <= p0_v;
	p1_n    <= p0_n[8:0];
	p1_ys   <= yy[2:0];
	p1_cs   <= cc[2:0];
	p1_yodd <= ya[0];
	p1_codd <= ca[0];
	p1_d1   <= xq[13:11];
	p1_d2   <= xq[16:14];
	p1_d3   <= xq[19:17];
	p1_dc   <= c2[1:0] - cc[1:0];
end

// (a + b + 1) / 2
function [7:0] avg2(input [7:0] a, input [7:0] b);
	reg [8:0] t;
	begin
		t = a + b + 1'd1;
		avg2 = t[8:1];
	end
endfunction

// a * 171/256 + b * 85/256 (2/3 and 1/3), as the player's third2
function [7:0] third2(input [7:0] a, input [7:0] b);
	reg [15:0] t;
	begin
		t = a * 8'd171 + b * 8'd85;
		third2 = t[15:8];
	end
endfunction

always @(posedge clk) begin
	reg [127:0] y16, u16, v16;
	reg  [63:0] y8;
	reg  [31:0] u4, v4;

	y16 = p1_yodd ? {ye_q, yo_q} : {yo_q, ye_q};
	u16 = p1_codd ? {ue_q, uo_q} : {uo_q, ue_q};
	v16 = p1_codd ? {ve_q, vo_q} : {vo_q, ve_q};
	y8  = y16 >> {p1_ys, 3'b000};
	u4  = u16 >> {p1_cs, 3'b000};
	v4  = v16 >> {p1_cs, 3'b000};

	s2_v  <= p1_v;
	s2_n  <= p1_n;
	s2_ov <= ov_q;
	// bytes in memory order: Y0 U Y1 V Y2 U Y3 V
	if(rs)
		s2_d <= {v4[{p1_dc, 3'b000} +: 8], y8[{p1_d3, 3'b000} +: 8],
		        u4[{p1_dc, 3'b000} +: 8], y8[{p1_d2, 3'b000} +: 8],
		        v4[7:0],                  y8[{p1_d1, 3'b000} +: 8],
		        u4[7:0],                  y8[7:0]};
	else if(n21)
		s2_d <= {avg2(v4[23:16], v4[31:24]),   avg2(y8[55:48], y8[63:56]),
		        avg2(u4[23:16], u4[31:24]),   avg2(y8[39:32], y8[47:40]),
		        avg2(v4[7:0],   v4[15:8]),    avg2(y8[23:16], y8[31:24]),
		        avg2(u4[7:0],   u4[15:8]),    avg2(y8[7:0],   y8[15:8])};
	else
		s2_d <= {third2(v4[23:16], v4[15:8]),  third2(y8[47:40], y8[39:32]),
		        third2(u4[23:16], u4[15:8]),  third2(y8[31:24], y8[39:32]),
		        third2(v4[7:0],   v4[15:8]),  third2(y8[23:16], y8[15:8]),
		        third2(u4[7:0],   u4[15:8]),  third2(y8[7:0],   y8[15:8])};
end

always @(posedge clk) begin
	if(mk_on) begin
		mk_n <= mk_n + 1'd1;
		mk_y <= mk_y + (n21 ? 13'd8 : 13'd6);
		mk_c <= mk_c + (n21 ? 12'd4 : 12'd3);
		if(mk_n + 1'd1 == ow) mk_on <= 0;
	end
	if(mk_go) begin
		mk_on   <= 1;
		mk_blend <= ov_on && row_c >= ov_y0;
		mk_bank <= row_c[0];
		mk_n    <= 0;
		mk_y    <= 0;
		mk_c    <= 0;
	end
end

// ---------------------------------------------------------------- the controls
// Four more steps for every word (also when no line has the controls):
// sa coverage, grey - picture and 255 * picture, s3 the products (in two
// halves of a), s4 their sums, then the division by 255 into owd.
always @(posedge clk) begin
	reg  [8:0] ac0, ac1;
	reg [63:0] A, G;
	reg  [7:0] pb;
	reg [15:0] t;
	// coverage and grey for each byte of the output word Y0 U Y1 V Y2 U Y3 V
	ac0 = s2_ov[7:0] + s2_ov[23:16] + 1'd1;
	ac1 = s2_ov[39:32] + s2_ov[55:48] + 1'd1;
	A = {ac1[8:1], s2_ov[55:48], ac1[8:1], s2_ov[39:32], ac0[8:1], s2_ov[23:16], ac0[8:1], s2_ov[7:0]};
	G = {8'd128,   s2_ov[63:56], 8'd128,   s2_ov[47:40], 8'd128,   s2_ov[31:24], 8'd128,   s2_ov[15:8]};
	for(int i = 0; i < 8; i++) begin
		pb = s2_d[i*8 +: 8];
		sa_g[i*9 +: 9]   <= {1'b0, G[i*8 +: 8]} - {1'b0, pb};
		sa_k[i*16 +: 16] <= {pb, 8'd0} - pb + 16'd127;
	end
	sa_a <= A;
	sa_v <= s2_v;
	sa_n <= s2_n;
	sa_d <= s2_d;
	sa_b <= mk_blend;

	for(int i = 0; i < 8; i++) begin
		s3_l[i*13 +: 13] <= $signed(sa_g[i*9 +: 9]) * $signed({1'b0, sa_a[i*8 +: 4]});
		s3_h[i*13 +: 13] <= $signed(sa_g[i*9 +: 9]) * $signed({1'b0, sa_a[i*8 + 4 +: 4]});
	end
	s3_k <= sa_k;
	s3_v <= sa_v;
	s3_n <= sa_n;
	s3_d <= sa_d;
	s3_b <= sa_b;

	// the sum is never negative and never above 65535: 16 bits are enough
	for(int i = 0; i < 8; i++)
		s4_s[i*16 +: 16] <= s3_b ? s3_k[i*16 +: 16] + {s3_h[i*13 +: 12], 4'd0} + {{3{s3_l[i*13 + 12]}}, s3_l[i*13 +: 13]}
		                         : {s3_d[i*8 +: 8], 8'd0};
	s4_v <= s3_v;
	s4_n <= s3_n;
	s4_b <= s3_b;

	// x / 255 = (x + 1 + x / 256) / 256 for every x used here (up to 65152)
	owe <= s4_v;
	owa <= s4_n;
	for(int i = 0; i < 8; i++) begin
		t = s4_b ? s4_s[i*16 +: 16] + 1'd1 + s4_s[i*16 + 8 +: 8] : s4_s[i*16 +: 16];
		owd[i*8 +: 8] <= t[15:8];
	end
end

wire mk_busy = mk_on | p0_v | p1_v | s2_v | sa_v | s3_v | s4_v | owe;

// ---------------------------------------------------------------- the bus
// output line word for the write: always the word at wr_idx
wire       w_acc = ddr_we && !ddr_busy;
wire [8:0] o_ra = w_acc ? wr_idx[8:0] + 1'd1 : wr_idx[8:0];
reg [63:0] o_q;
always @(posedge clk) o_q <= obuf[o_ra];

reg [63:0] ans;
assign ddr_din = wr_ans ? ans : o_q;

// a transfer can start: a read to ask for, or a write burst to begin
wire rd_ready = rd_on && rd_left && (rd_kind != 2 || lk == 2) && pend < 12'd256;
wire wr_ready = wr_on && !wr_beats && wr_left;
assign want = (rd_ready || wr_ready) && !ddr_rd && !ddr_we;
assign idle = !ddr_rd && !ddr_we && !pend && !wr_beats;

reg [19:0] wd = 0;  // watchdog: words that never arrive
// words to the end of rd_off's page, a clock late: a page read waits for the
// table look-up (lk 0, 1, 3, 2) after every change of rd_off, so it is current
reg [11:0] pg_end;

always @(posedge clk) begin
	reg [31:0] pa;
	reg [11:0] to_end;
	reg [10:0] n;
	reg [13:0] f, f1, f2;
	reg [10:0] dd;

	bwe   <= 0;
	twe   <= 0;
	xm_we <= 0;
	mk_go <= 0;
	if(state != S_WAIT && state != S_DEAD) cycles <= cycles + 1'd1;

	// ------------------------------------------------ transfers under way
	pg_end <= 12'd512 - rd_off[11:3];
	if(ddr_rd && !ddr_busy) ddr_rd <= 0;

	if(w_acc) begin
		wr_idx   <= wr_idx + 1'd1;
		wr_beats <= wr_beats - 1'd1;
		if(wr_beats == 1) ddr_we <= 0;
	end

	if(ddr_dout_ready && pend) begin
		pend <= pend - 1'd1 + ((ddr_rd && !ddr_busy) ? ddr_burst : 8'd0);
		rx   <= rx + 1'd1;
		case(rd_kind)
			0: if(rx < 16) begin hdr[{rx[2:0], 1'b0}] <= ddr_dout[31:0]; hdr[{rx[2:0], 1'b1}] <= ddr_dout[63:32]; end
			1: begin twe <= 1; twa <= rx[9:0]; twd <= ddr_dout; end
			2: begin
					bwe   <= 1;
					bbank <= rd_bank;
					bdata <= ddr_dout;
					if(rx < ny) begin bseg <= 0; bidx <= rx[10:0]; end
					else if(rx < ny + nc) begin bseg <= 1; bidx <= rx[10:0] - ny; end
					else if(rx < ny + nc + nc) begin bseg <= 2; bidx <= rx[10:0] - ny - nc; end
					else begin bseg <= 3; bidx <= rx[10:0] - ny - nc - nc; end
				end
		endcase
	end
	else if(ddr_rd && !ddr_busy) pend <= pend + ddr_burst;

	// a burst asked for that never arrives (the memory refused): give up
	if(pend && !ddr_dout_ready) begin
		wd <= wd + 1'd1;
		if(&wd) begin
			state  <= S_DEAD;
			pend   <= 0;
			rd_on  <= 0;
			wr_on  <= 0;
		end
	end
	else wd <= 0;

	// ------------------------------------------------ read engine
	if(rd_go) begin
		rd_on <= 1;
		rx    <= 0;
		lk    <= 0;
	end
	else if(rd_on) begin
		if(rd_kind == 2 && rd_left && lk == 0) begin
			// the page of rd_off
			tab_ra <= rd_off[22:12];
			lk     <= 1;
		end
		else if(lk == 1) lk <= 3;       // table word read
		else if(lk == 3) lk <= 2;       // tab_q ready
		if(grant && rd_ready && !wr_ready && !ddr_rd && !ddr_we) begin
			if(rd_kind == 2 && rd_seg != 3) begin
				pa     = {tab_q[19:0], rd_off[11:0]};
				to_end = pg_end;
			end
			else begin
				pa     = rd_off;
				to_end = 12'd512;
			end
			n = rd_left;
			if(n > BURST) n = BURST;
			if(n > to_end) n = to_end[10:0];
			ddr_addr  <= pa[31:3];
			ddr_burst <= n[7:0];
			ddr_rd    <= 1;
			rd_off    <= rd_off + {n, 3'b000};
			rd_left   <= rd_left - n;
			lk        <= 0;
			if(rd_left == n && rd_kind == 2 && rd_seg < 2) begin
				// next segment of the line: U, then V
				rd_seg  <= rd_seg + 1'd1;
				rd_off  <= (rd_seg == 0) ? rd_u : rd_v;
				rd_left <= nc;
			end
			if(rd_left == n && rd_kind == 2 && rd_seg == 2 && rd_band) begin
				// then the controls, at a physical address
				rd_seg  <= 3;
				rd_off  <= rd_o;
				rd_left <= {1'b0, ow};
			end
		end
	end

	// ------------------------------------------------ write engine
	if(wr_on && grant && wr_ready && !ddr_rd && !ddr_we) begin
		n = wr_left;
		if(n > BURST) n = BURST;
		ddr_addr  <= wr_addr[31:3];
		ddr_burst <= n[7:0];
		ddr_we    <= 1;
		wr_beats  <= n[7:0];
		wr_addr   <= wr_addr + {n, 3'b000};
		wr_left   <= wr_left - n;
	end

	// ------------------------------------------------ steps
	case(state)
		S_WAIT: begin
				poll <= poll + 1'd1;
				if(enable && &poll) begin
					rd_kind <= 0;
					rd_off  <= MBOX;
					rd_left <= 8;
					rd_go   <= 1;
					state   <= S_HDR;
				end
			end

		S_HDR, S_HDR2: begin
				rd_go <= 0;
				if(!rd_go && rd_on && !rd_left && !pend && rx == 8) begin
					rd_on <= 0;
					if(state == S_HDR && primed && hdr[0] == MAGIC && hdr[1] != last_seq) begin
						// read it again: the values come before the sequence
						rd_off  <= MBOX;
						rd_left <= 8;
						rd_go   <= 1;
						state   <= S_HDR2;
					end
					else if(state == S_HDR2) state <= S_CHECK;
					else begin
						last_seq <= hdr[1];
						primed   <= 1;
						state    <= S_WAIT;
					end
				end
			end

		S_CHECK: begin
				last_seq <= hdr[1];
				cycles   <= 0;
				ow       <= hdr[5][25:16];
				// source words: 3/4 and 3/8 of the output words (2:1: as many, half)
				ny       <= rs ? (rs_sw + 4'd7) >> 3 : n21 ? {1'b0, hdr[5][25:16]} : {1'b0, hdr[5][25:16]} - hdr[5][25:18];
				nc       <= rs ? (rs_sw + 4'd15) >> 4 : n21 ? {2'b0, hdr[5][25:17]} : hdr[5][25:17] - hdr[5][25:19];
				mx_w     <= {hdr[5][25:16], 2'b00};
				rows     <= hdr[3][31:16];
				tab_n    <= hdr[5][10:0];
				// output words: a multiple of 8, at most 340 (Y fits 256 words);
				// 2048 pages at most; addresses on 8 bytes
				// resample: output pixels at most the source's, at least half
				// (more than half); lines the same; 2048 source pixels at most
				refused  <= !hdr[5][25:16] || hdr[5][18:16] || hdr[5][25:16] > (rs ? 10'd512 : n21 ? 10'd256 : 10'd340) ||
				            (rs && (hdr[13][15:0] > 16'd2048 || {hdr[5][25:16], 2'b00} > hdr[13][15:0] ||
				                    {hdr[5][25:16], 3'b000} <= hdr[13][15:0] || hdr[3][31:16] > rs_sh ||
				                    {hdr[3][31:16], 1'b0} <= {1'b0, rs_sh})) ||
				            !hdr[5][15:0] || hdr[5][15:0] > 16'd2048 || !hdr[3][31:16] ||
				            |{hdr[2][2:0], hdr[3][2:0], hdr[4][2:0], hdr[6][2:0], hdr[7][2:0], hdr[8][2:0], hdr[9][2:0], hdr[10][2:0]} ||
				            (hdr[10][18] && hdr[11][2:0]);
				mx_k     <= 0;
				mx_s     <= 0;
				mx_f     <= 0;
				v_l      <= 0;
				v_f      <= 0;
				state    <= rs ? S_MAP : S_TABLE;
			end

		S_MAP: begin
				// one output pixel a clock: s and f for x + 1 from those of x
				f  = {1'b0, mx_f} + rs_sw;
				f1 = {1'b0, mx_f} + kx1;
				f2 = {1'b0, mx_f} + kx2;
				dd = mx_s - mx_y0;
				if(refused) state <= S_TABLE;
				else begin
					case(mx_k[1:0])
						0: mx_y0 <= mx_s;
						1: mx_d[2:0] <= dd[2:0];
						2: mx_d[5:3] <= dd[2:0];
						3: begin
								xm_we <= 1;
								xm_wa <= mx_k[10:2];
								xm_wd <= {dd[2:0], mx_d, mx_y0};
							end
					endcase
					if(!f2[13]) begin mx_s <= mx_s + 2'd2; mx_f <= f2[12:0]; end
					else if(!f1[13]) begin mx_s <= mx_s + 1'd1; mx_f <= f1[12:0]; end
					else mx_f <= f[12:0];
					mx_k <= mx_k + 1'd1;
					if(mx_k + 1'd1 == mx_w) state <= S_TABLE;
				end
			end

		S_TABLE: begin
				if(refused) begin
					ans    <= {32'd1, cycles};
					state  <= S_ANSWER;
					wr_ans <= 1;
					wr_on  <= 1;
					wr_addr <= MBOX + 32'd64;
					wr_left <= 2;
					wr_idx  <= 0;
				end
				else if(!rd_on && !rd_go) begin
					rd_kind <= 1;
					rd_off  <= hdr[4];
					rd_left <= (tab_n + 1'd1) >> 1;
					rd_go   <= 1;
				end
				else begin
					rd_go <= 0;
					if(!rd_go && !rd_left && !pend && !ddr_rd && rx == (tab_n + 1'd1) >> 1) begin
						rd_on   <= 0;
						// first line
						row_r   <= 0;
						row_c   <= 0;
						row_w   <= 0;
						y_off   <= hdr[6];
						u_off   <= hdr[8];
						v_off   <= hdr[9];
						row_a   <= hdr[2];
						ov_a    <= hdr[11];
						state   <= S_ROWS;
					end
				end
			end

		S_ROWS: begin
				rd_go <= 0;
				// read the next line into its bank, once that bank is free:
				// the line two before it is made
				if(!rd_on && !rd_go && !mk_go && row_r < rows && row_r <= row_c + 1'd1 && !(row_r == row_c + 1'd1 && mk_busy)) begin
					rd_kind <= 2;
					rd_seg  <= 0;
					rd_bank <= row_r[0];
					rd_off  <= y_off;
					rd_u    <= u_off;
					rd_v    <= v_off;
					rd_left <= ny;
					rd_go   <= 1;
					row_r   <= row_r + 1'd1;
					rd_band <= ov_on && row_r >= ov_y0;
					rd_o    <= ov_a;
					if(ov_on && row_r >= ov_y0) ov_a <= ov_a + {ow, 3'b000};
					// the line after
					if(rs) begin
						// source line l + n, n = 0, 1 or 2 (whole part of
						// (r + 1) * source lines / lines); its colour line
						v_f   <= v_fn;
						v_l   <= v_l + v_n;
						y_off <= y_off + (v_n == 2 ? {y_step[30:0], 1'b0} : v_n == 1 ? y_step : 32'd0);
						u_off <= u_off + (v_cn == 2 ? {15'd0, c_step, 1'b0} : v_cn == 1 ? {16'd0, c_step} : 32'd0);
						v_off <= v_off + (v_cn == 2 ? {15'd0, c_step, 1'b0} : v_cn == 1 ? {16'd0, c_step} : 32'd0);
					end
					else begin
						y_off   <= y_off + y_step;
						if(!c_half || row_r[0]) begin
							u_off <= u_off + c_step;
							v_off <= v_off + c_step;
						end
					end
				end
				if(rd_on && !rd_go && !rd_left && !pend && rx == ny + nc + nc + (rd_band ? ow : 10'd0)) rd_on <= 0;

				// make a line once it is read, the output buffer free
				if(!mk_busy && !mk_go && row_c < row_r && !(row_c + 1'd1 == row_r && (rd_on || rd_go)) && !wr_on && row_c == row_w)
					mk_go <= 1;
				if(mk_go) row_c <= row_c + 1'd1;

				// write it once made
				if(!wr_on && !mk_busy && !mk_go && row_w < row_c) begin
					wr_on   <= 1;
					wr_ans  <= 0;
					wr_addr <= row_a;
					row_a   <= row_a + hdr[3][15:0];
					wr_left <= ow;
					wr_idx  <= 0;
				end
				if(wr_on && !wr_left && !wr_beats && !ddr_we) begin
					wr_on <= 0;
					row_w <= row_w + 1'd1;
					if(row_w + 1'd1 == rows) begin
						ans     <= {32'd0, cycles};
						state   <= S_ANSWER;
						wr_ans  <= 1;
						wr_on   <= 1;
						wr_addr <= MBOX + 32'd64;
						wr_left <= 2;
						wr_idx  <= 0;
					end
				end
			end

		S_ANSWER: begin
				// two words: cycles and status, then the sequence
				if(w_acc) ans <= {32'd0, last_seq};
				if(wr_on && !wr_left && !wr_beats && !ddr_we) begin
					wr_on  <= 0;
					wr_ans <= 0;
					state  <= S_WAIT;
				end
			end

		S_DEAD: ;
	endcase
end

endmodule
