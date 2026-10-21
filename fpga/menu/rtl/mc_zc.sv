// MiSTer MediaCenter "zero-copy": the core copies a decoded picture into the
// frame buffer instead of the ARM.
//
// The player decodes into memory that Linux keeps in place, and gives this
// block, through a small mailbox in DDR, the physical address of each 4 KB
// page of that memory (a page table). The block then builds the frame
// buffer page, line by line: it reads a line of Y and a line of U and V
// (planar 4:2:0, as the decoder writes them), narrows them by 3:2 (1920 to
// 1280: every 3 source pixels give 2, the nearer one weighing 2/3, the other
// 1/3, exactly as the player's pack_yuyv_32) and writes the YUYV line. The
// ARM only waits for it.
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
// Answer (written by the block, 64 bytes on):
//  16 clock cycles spent   17 status: 0 done, 1 refused (bad values)
//  18 the sequence done (written last, in a word of its own)
//
// The bus is shared with mc_out (the CRT output), which always goes first:
// this block only starts a transfer while mc_out has nothing to fetch, and
// gives the bus back between its bursts (at most 32 words).

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
           S_DEAD   = 7;  // the memory never answered: stay out of the way

reg  [2:0] state = S_WAIT;
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

wire [31:0] y_step = hdr[7];
wire [15:0] c_step = hdr[10][15:0];
wire        c_half = hdr[10][16];

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

// output line
reg [63:0] obuf[512];
reg        owe = 0;
reg  [8:0] owa;
reg [63:0] owd;
always @(posedge clk) if(owe) obuf[owa] <= owd;

// ---------------------------------------------------------------- making a line
// Output word n takes Y bytes 6n..6n+5 and U, V bytes 3n..3n+2 of the line.
reg        mk_on = 0;
reg        mk_bank;
reg  [9:0] mk_n;          // word being asked for
reg [12:0] mk_y;          // 6n
reg [11:0] mk_c;          // 3n
reg [63:0] ye_q, yo_q, ue_q, uo_q, ve_q, vo_q;
reg        p1_v = 0;
reg  [8:0] p1_n;
reg        mk_go = 0;
reg  [2:0] p1_ys, p1_cs;   // byte of the first Y, U
reg        p1_yodd, p1_codd; // the first word is an odd one

always @(posedge clk) begin
	reg [7:0] ya, yb;
	reg [6:0] ca, cb;
	ya = mk_y[12:3];       // first Y word
	yb = ya + 1'd1;
	ca = mk_c[11:3];
	cb = ca + 1'd1;
	// even/odd words: the word a and a+1, one of them even
	ye_q <= ye[{mk_bank, ya[0] ? yb[7:1] : ya[7:1]}];
	yo_q <= yo[{mk_bank, ya[7:1]}];
	ue_q <= ue[{mk_bank, ca[0] ? cb[6:1] : ca[6:1]}];
	uo_q <= uo[{mk_bank, ca[6:1]}];
	ve_q <= ve[{mk_bank, ca[0] ? cb[6:1] : ca[6:1]}];
	vo_q <= vo[{mk_bank, ca[6:1]}];
	p1_v    <= mk_on;
	p1_n    <= mk_n[8:0];
	p1_ys   <= mk_y[2:0];
	p1_cs   <= mk_c[2:0];
	p1_yodd <= ya[0];
	p1_codd <= ca[0];
end

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
	reg  [47:0] y6;
	reg  [23:0] u3, v3;

	y16 = p1_yodd ? {ye_q, yo_q} : {yo_q, ye_q};
	u16 = p1_codd ? {ue_q, uo_q} : {uo_q, ue_q};
	v16 = p1_codd ? {ve_q, vo_q} : {vo_q, ve_q};
	y6  = y16 >> {p1_ys, 3'b000};
	u3  = u16 >> {p1_cs, 3'b000};
	v3  = v16 >> {p1_cs, 3'b000};

	owe <= p1_v;
	owa <= p1_n;
	// bytes in memory order: Y0 U Y1 V Y2 U Y3 V
	owd <= {third2(v3[23:16], v3[15:8]),  third2(y6[47:40], y6[39:32]),
	        third2(u3[23:16], u3[15:8]),  third2(y6[31:24], y6[39:32]),
	        third2(v3[7:0],   v3[15:8]),  third2(y6[23:16], y6[15:8]),
	        third2(u3[7:0],   u3[15:8]),  third2(y6[7:0],   y6[15:8])};
end

always @(posedge clk) begin
	if(mk_on) begin
		mk_n <= mk_n + 1'd1;
		mk_y <= mk_y + 13'd6;
		mk_c <= mk_c + 12'd3;
		if(mk_n + 1'd1 == ow) mk_on <= 0;
	end
	if(mk_go) begin
		mk_on   <= 1;
		mk_bank <= row_c[0];
		mk_n    <= 0;
		mk_y    <= 0;
		mk_c    <= 0;
	end
end

wire mk_busy = mk_on | p1_v | owe;

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

always @(posedge clk) begin
	reg [31:0] pa;
	reg [11:0] to_end;
	reg [10:0] n;

	bwe   <= 0;
	twe   <= 0;
	mk_go <= 0;
	if(state != S_WAIT && state != S_DEAD) cycles <= cycles + 1'd1;

	// ------------------------------------------------ transfers under way
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
					else begin bseg <= 2; bidx <= rx[10:0] - ny - nc; end
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
			if(rd_kind == 2) begin
				pa     = {tab_q[19:0], rd_off[11:0]};
				to_end = 12'd512 - rd_off[11:3];
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
			if(rd_left == n && rd_kind == 2 && rd_seg != 2) begin
				// next segment of the line: U, then V
				rd_seg  <= rd_seg + 1'd1;
				rd_off  <= (rd_seg == 0) ? rd_u : rd_v;
				rd_left <= nc;
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
				ny       <= {1'b0, hdr[5][25:16]} - hdr[5][25:18];  // 3/4 of the output words
				nc       <= hdr[5][25:17] - hdr[5][25:19];          // 3/8
				rows     <= hdr[3][31:16];
				tab_n    <= hdr[5][10:0];
				// output words: a multiple of 8, at most 340 (Y fits 256 words);
				// 2048 pages at most; addresses on 8 bytes
				refused  <= !hdr[5][25:16] || hdr[5][18:16] || hdr[5][25:16] > 10'd340 ||
				            !hdr[5][15:0] || hdr[5][15:0] > 16'd2048 || !hdr[3][31:16] ||
				            |{hdr[2][2:0], hdr[3][2:0], hdr[4][2:0], hdr[6][2:0], hdr[7][2:0], hdr[8][2:0], hdr[9][2:0], hdr[10][2:0]};
				state    <= S_TABLE;
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
					// the line after
					y_off   <= y_off + y_step;
					if(!c_half || row_r[0]) begin
						u_off <= u_off + c_step;
						v_off <= v_off + c_step;
					end
				end
				if(rd_on && !rd_go && !rd_left && !pend && rx == ny + nc + nc) rd_on <= 0;

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

// The DDR port shared by mc_out (first) and mc_zc. The bus goes to mc_zc only
// while mc_out has nothing to fetch, and back as soon as mc_out asks for a
// line and mc_zc's transfer under way (one burst at most) is over. Read data
// go to the one that asked.
module mc_bus
(
	input         clk,
	input         own,       // the bus is the frame buffer's

	input  [28:0] m_addr,
	input   [7:0] m_burst,
	input         m_rd,
	input         m_idle,
	output        m_grant,
	output        m_ready,

	input  [28:0] z_addr,
	input   [7:0] z_burst,
	input         z_rd,
	input         z_we,
	input         z_want,
	input         z_idle,
	output        z_grant,
	output        z_ready,

	input         ready,
	output [28:0] addr,
	output  [7:0] burst,
	output        rd,
	output        we,
	output        zsel_o,
	output        idle
);

reg zsel = 0;
always @(posedge clk) begin
	if(!zsel) begin
		if(own && m_idle && z_want) zsel <= 1;
	end
	else if(z_idle && (!m_idle || !z_want || !own)) zsel <= 0;
end

assign m_grant = own & ~zsel;
assign z_grant = own & zsel & m_idle;
assign m_ready = ready & ~zsel;
assign z_ready = ready & zsel;
assign addr    = zsel ? z_addr  : m_addr;
assign burst   = zsel ? z_burst : m_burst;
assign rd      = zsel ? z_rd    : m_rd;
assign we      = zsel & z_we;
assign zsel_o  = zsel;
assign idle    = m_idle & ~zsel;

endmodule
