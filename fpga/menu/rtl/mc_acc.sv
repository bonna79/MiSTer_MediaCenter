// MiSTer MediaCenter FPGA accelerator: the H.264 reconstruction engine
// (accel/rtl/acc_engine.sv) inside the menu core.
//
// The player reads the stream on the ARM and writes the engine's commands
// (accel/doc/comandi.md) into a ring buffer in memory; this block reads them
// and feeds the engine, which rebuilds each picture into its slot in DDR,
// outside Linux (SLOTS, 4 MB a slot, 64 slots: 0x30000000 to 0x3FFFFFFF). The zero-copy block (mc_zc)
// then copies a finished slot into the frame buffer.
//
// Mailbox (MBOX, 32-bit words, written by the player through /dev/mem):
//   0 magic "MCAC"
//   1 session: a new value resets the engine and starts the ring again
//   2 ring page table (physical byte address): one 32-bit page number per
//     4 KB page of the ring
//   3 pages in the ring (1 to 1024)
//   4 write pointer: bytes written into the ring since the session started
//     (a multiple of 8; the ring is used again from its start when full)
// Answer (written by this block, 64 bytes on):
//  16 "ACOK"                       17 session being run
//  18 bytes read from the ring     19 pictures finished
//  20 [0] unexpected command [1] memory never answered [15:8] last slot
//     [16] 1: mc_zc can also narrow by 2:1 (core 3.0 test builds on)
//     [17] 0 (core of phase 4: deblocking with the strengths from the ARM)
//     [18] 1: the engine filters, working out the strengths (command A5 of 3 words)
//  21 clocks between the last two finished pictures
//  22 of them, clocks the engine had no command to read
//  23 of them, clocks the engine waited for the memory
//  24 macroblocks done            25 clocks since the session started
// The player may write over ring bytes once word 18 says they were read.
//
// The bus is shared with mc_out (the CRT output, always first) and mc_zc
// through mc_bus: this block starts a transfer only while granted.
module mc_acc #(
	parameter [31:0] MBOX  = 32'h23E00000,
	parameter [31:0] SLOTS = 32'h30000000
)
(
	input             clk,
	input             enable,        // the bus is the frame buffer's (FB mode)

	output reg [28:0] ddr_addr,
	output reg  [7:0] ddr_burst,
	output reg        ddr_rd = 0,
	output reg        ddr_we = 0,
	output reg [63:0] ddr_din,
	input             ddr_busy,
	input      [63:0] ddr_dout,
	input             ddr_dout_ready, // only for this block's reads

	input             grant,         // a new transfer may start
	output            want,          // has a transfer to start
	output            idle           // nothing under way
);

localparam [31:0] MAGIC = 32'h4341434D; // "MCAC"
localparam [31:0] OKAY  = 32'h4B4F4341; // "ACOK"
localparam BURST = 32;
localparam CF_L2 = 9;                   // command FIFO: 512 words of 64 bits

// ================================================================ engine
reg         e_reset = 1;
wire        e_rd, e_we;
wire [24:0] e_addr;
wire  [7:0] e_burst, e_be;
wire [63:0] e_din;
wire        e_take;                     // the engine's request goes into the queue
wire        e_valid, e_ready;
wire [31:0] e_cmd;
wire        e_done, e_err;
wire  [7:0] e_slot;
wire [31:0] e_mbs;
reg  [63:0] e_dout;
reg         e_dout_ready = 0;

// 25-bit word addresses: 256 MB from SLOTS, slot numbers taken modulo 64
acc_engine #(.AW(25)) engine
(
	.clk(clk), .reset(e_reset),
	.cmd_valid(e_valid), .cmd_data(e_cmd), .cmd_ready(e_ready),
	.mem_rd(e_rd), .mem_we(e_we), .mem_addr(e_addr), .mem_burst(e_burst), .mem_din(e_din), .mem_be(e_be),
	.mem_busy(!e_take), .mem_dout(e_dout), .mem_dout_ready(e_dout_ready),
	.pic_done(e_done), .pic_slot(e_slot), .error(e_err), .mb_count(e_mbs)
);

// ================================================================ request queue
// Every transfer (the engine's, the ring, page table and mailbox reads, the
// answer) goes through this queue, in order. Reads also leave a tag saying
// whose data come back and how many words.
localparam K_ENG = 0, K_MBOX = 1, K_TAB = 2, K_RING = 3;
localparam RQ = 8;
reg  [2:0] rq_wp = 0, rq_rp = 0;
reg  [3:0] rq_n = 0;
reg        rq_room = 1;                 // room for a request (counted a clock late)
reg        rq_we_[RQ];
reg [28:0] rq_addr[RQ];
reg  [7:0] rq_burst[RQ];
reg [63:0] rq_din[RQ];

localparam TQ = 32;
reg  [4:0] tq_wp = 0, tq_rp = 0;
reg  [5:0] tq_n = 0;
reg        tq_room = 1;
reg  [1:0] tq_kind[TQ];
reg  [7:0] tq_len[TQ];
reg  [7:0] rx_n = 0;                    // words of the head tag arrived
reg  [5:0] out_n = 0;                   // reads on the memory, not all arrived

// this block's own requests
reg        f_req = 0, f_we = 0;
reg  [1:0] f_kind = K_MBOX;
reg [28:0] f_addr;
reg  [7:0] f_burst;
reg [63:0] f_din;
wire       f_take = f_req && rq_room && (f_we || tq_room);

assign e_take = (e_rd || e_we) && !e_reset && !f_req && rq_room && (e_we || tq_room);

wire        push    = f_take || e_take;
wire        p_we    = f_take ? f_we : e_we;
wire [28:0] p_addr  = f_take ? f_addr : {SLOTS[31:28], e_addr};
wire  [7:0] p_burst = f_take ? f_burst : e_burst;
wire [63:0] p_din   = f_take ? f_din : e_din;
wire  [1:0] p_kind  = f_take ? f_kind : 2'd0;
wire        tpush   = push && !p_we;

// the head of the queue goes on the bus
wire on_bus = ddr_rd || ddr_we;
wire bus_ok = on_bus && !ddr_busy;
wire pop    = rq_n != 0 && (!on_bus || bus_ok) && grant;

always @(posedge clk) begin
	reg [3:0] n;
	if (push) begin
		rq_we_[rq_wp]   <= p_we;
		rq_addr[rq_wp]  <= p_addr;
		rq_burst[rq_wp] <= p_burst;
		rq_din[rq_wp]   <= p_din;
		rq_wp <= rq_wp + 1'd1;
	end
	if (pop) rq_rp <= rq_rp + 1'd1;
	n = rq_n + push - pop;
	rq_n    <= n;
	rq_room <= n <= RQ - 3;

	if (bus_ok) begin
		ddr_rd <= 0;
		ddr_we <= 0;
	end
	if (pop) begin
		ddr_rd    <= !rq_we_[rq_rp];
		ddr_we    <= rq_we_[rq_rp];
		ddr_addr  <= rq_addr[rq_rp];
		ddr_burst <= rq_burst[rq_rp];
		ddr_din   <= rq_din[rq_rp];
	end
end

assign want = rq_n != 0 && !on_bus;
assign idle = !on_bus && !out_n;

// read data: to the engine or to this block, by the head tag
wire [1:0] rx_kind = tq_kind[tq_rp];
wire       rx_last = ddr_dout_ready && rx_n + 1'd1 == tq_len[tq_rp];
reg        f_rx = 0;                    // a word for this block
reg  [1:0] f_rx_kind;
reg [63:0] f_dout;

always @(posedge clk) begin
	reg [5:0] n;
	if (tpush) begin
		tq_kind[tq_wp] <= p_kind;
		tq_len[tq_wp]  <= p_burst;
		tq_wp <= tq_wp + 1'd1;
	end
	if (rx_last) tq_rp <= tq_rp + 1'd1;
	rx_n <= rx_last ? 8'd0 : rx_n + ddr_dout_ready;
	n = tq_n + tpush - rx_last;
	tq_n    <= n;
	tq_room <= n <= TQ - 3;
	out_n   <= out_n + (bus_ok && ddr_rd) - rx_last;

	e_dout_ready <= ddr_dout_ready && rx_kind == K_ENG;
	f_rx         <= ddr_dout_ready && rx_kind != K_ENG;
	f_rx_kind    <= rx_kind;
	e_dout       <= ddr_dout;
	f_dout       <= ddr_dout;
end

// ================================================================ command FIFO
// 64-bit words from the ring, given to the engine 32 bits at a time, the
// low half first. cf_resv counts the words asked for and not yet used by
// the engine, so the words on their way always have room.
reg  [63:0] cf[1 << CF_L2];
reg [CF_L2:0] cf_wp = 0, cf_rp = 0;
reg [CF_L2:0] cf_resv = 0;
reg  [63:0] cf_q;
reg         cf_qv = 0;                  // cf_q holds a word for the engine
reg         half = 0;
reg         cf_we = 0;
reg  [63:0] cf_wd;

always @(posedge clk) if (cf_we) cf[cf_wp[CF_L2-1:0]] <= cf_wd;
always @(posedge clk) if (cf_next) cf_q <= cf[cf_rp[CF_L2-1:0]];

wire e_used  = e_valid && e_ready;
wire cf_next = cf_wp != cf_rp && (!cf_qv || (e_used && half));

assign e_valid = cf_qv && !e_reset;
assign e_cmd   = half ? cf_q[63:32] : cf_q[31:0];

// ================================================================ the block
localparam S_IDLE = 0, S_HDR = 1, S_HDR2 = 2, S_DRAIN = 3, S_TABLE = 4, S_RUN = 5, S_ANS = 6, S_DEAD = 7;
reg  [2:0] state = S_IDLE;
reg  [9:0] poll = 0;
reg [31:0] hdr[16];
reg  [3:0] hdr_got = 0;
reg        primed = 0;                  // session holds the mailbox's value at start
reg        live = 0;                    // a session is running
reg        stop = 0;                    // the player left: reset, no new session
reg [31:0] session = 0;
reg [31:0] wp = 0;                      // the player's write pointer
reg [31:0] rf = 0;                      // ring bytes asked for
reg [31:0] r_avail = 0;                 // wp - rf, a clock late
reg [31:0] rd_done = 0;                 // ring bytes arrived
reg [31:0] ans_rd = 0;                  // rd_done in the last answer
reg [22:0] ro = 0;                      // offset of rf in the ring
reg [22:0] rsize = 0;                   // bytes in the ring
// worked out a clock late from ro (timing): a ring read waits for the table
// look-up of ro (lk 0, 1, 2, 3) after every change of ro and rsize
reg  [9:0] r_pg_end;                    // words to the end of ro's page
reg        r_last;                      // ro is in the ring's last page
reg [10:0] pages = 0;
reg [10:0] t_left = 0, t_got = 0;       // page table words to ask for, arrived
reg        need_ans = 0;
reg  [2:0] ans_i = 0;
reg [31:0] pics = 0;
reg [31:0] since = 0, starve = 0, mwait = 0, clocks = 0;
reg [31:0] l_since = 0, l_starve = 0, l_mwait = 0;
reg  [7:0] l_slot = 0;
reg        err = 0, dead = 0;
reg [19:0] wd = 0;
reg  [5:0] rst_n = 0;

// page table: the page number of each 4 KB page of the ring
reg  [19:0] tab_lo[512], tab_hi[512];
reg  [19:0] tab_lq, tab_hq;
reg   [9:0] tab_ra = 0;
reg         tab_sel;
always @(posedge clk) tab_lq <= tab_lo[tab_ra[9:1]];
always @(posedge clk) tab_hq <= tab_hi[tab_ra[9:1]];
always @(posedge clk) tab_sel <= tab_ra[0];
wire [19:0] tab_q = tab_sel ? tab_hq : tab_lq;
reg         twe = 0;
reg   [8:0] twa;
reg  [63:0] twd;
always @(posedge clk) if (twe) tab_lo[twa] <= twd[19:0];
always @(posedge clk) if (twe) tab_hi[twa] <= twd[51:32];
reg   [1:0] lk = 0;                     // table look-up of ro: 3 = tab_q ready

function [63:0] ans_word(input [2:0] i);
	ans_word =
		i == 0 ? {session, OKAY} :
		i == 1 ? {pics, rd_done} :
		i == 2 ? {l_since, 13'd0, 3'b101, l_slot, 6'd0, dead, err} :
		i == 3 ? {l_mwait, l_starve} :
		         {clocks, e_mbs};
endfunction

wire        poll_now = enable && &poll && !f_req;
wire [10:0] t_all = (pages + 1'd1) >> 1;
wire  [9:0] cf_room = (1 << CF_L2) - cf_resv;

always @(posedge clk) begin
	reg  [8:0] n;
	reg [12:0] to_end;

	cf_we <= 0;
	twe   <= 0;
	poll  <= poll + 1'd1;
	r_avail <= wp - rf;
	if (live) clocks <= clocks + 1'd1;

	// ------------------------------------------------ command FIFO
	if (e_used) half <= !half;
	if (cf_next) begin
		cf_rp <= cf_rp + 1'd1;
		cf_qv <= 1;
	end
	else if (e_used && half) cf_qv <= 0;
	cf_resv <= cf_resv - (e_used && half) + ((f_take && f_kind == K_RING) ? f_burst : 8'd0);
	if (cf_we) cf_wp <= cf_wp + 1'd1;

	// ------------------------------------------------ this block's read data
	if (f_rx) case (f_rx_kind)
		K_MBOX: begin
				hdr[{hdr_got[2:0], 1'b0}] <= f_dout[31:0];
				hdr[{hdr_got[2:0], 1'b1}] <= f_dout[63:32];
				hdr_got <= hdr_got + 1'd1;
			end
		K_TAB: begin
				twe   <= 1;
				twa   <= t_got[8:0];
				twd   <= f_dout;
				t_got <= t_got + 1'd1;
			end
		default: begin
				cf_we   <= 1;
				cf_wd   <= f_dout;
				rd_done <= rd_done + 4'd8;
			end
	endcase
	if (f_take) f_req <= 0;

	// ------------------------------------------------ the engine
	if (e_done) begin
		pics     <= pics + 1'd1;
		l_slot   <= e_slot;
		l_since  <= since;
		l_starve <= starve;
		l_mwait  <= mwait;
		since    <= 0;
		starve   <= 0;
		mwait    <= 0;
		need_ans <= 1;
	end
	else if (live) begin
		since  <= since + 1'd1;
		starve <= starve + !e_valid;
		mwait  <= mwait + ((e_rd || e_we) && !e_take);
	end
	if (e_err && !err) begin err <= 1; need_ans <= 1; end

	// ------------------------------------------------ the memory never answers
	if (out_n && !ddr_dout_ready) begin
		wd <= wd + 1'd1;
		if (&wd) begin dead <= 1; live <= 0; state <= S_DEAD; end
	end
	else wd <= 0;

	if (rst_n) begin
		rst_n <= rst_n - 1'd1;
		if (rst_n == 1 && !stop) e_reset <= 0;
	end

	r_pg_end <= 10'd512 - ro[11:3];
	r_last   <= ro[22:12] == rsize[22:12] - 1'd1;

	// ------------------------------------------------ steps
	case (state)
		S_IDLE, S_RUN: begin
				if (state == S_RUN) begin
					if (lk == 0) begin tab_ra <= ro[21:12]; lk <= 1; end
					else if (lk != 3) lk <= lk + 1'd1;
				end
				if (poll_now) begin
					// look at the mailbox every 1024 clocks (9.5 us)
					f_req   <= 1;
					f_we    <= 0;
					f_kind  <= K_MBOX;
					f_addr  <= MBOX[31:3];
					f_burst <= 8;
					hdr_got <= 0;
					state   <= S_HDR;
				end
				else if (state == S_RUN && lk == 3 && !f_req && r_avail >= 8 && cf_room >= BURST) begin
					// read the ring: bytes written, not past the page
					to_end = {3'd0, r_pg_end};
					n = r_avail[31:3] > BURST ? 9'd32 : {1'b0, r_avail[10:3]};
					if (n > to_end) n = to_end[8:0];
					f_req   <= 1;
					f_we    <= 0;
					f_kind  <= K_RING;
					f_addr  <= {tab_q, ro[11:3]};
					f_burst <= n[7:0];
					rf      <= rf + {n, 3'b000};
					ro      <= (r_last && {4'd0, n} == to_end) ? 23'd0 : ro + {n, 3'b000}; // a read stops at the page end
					lk      <= 0;
				end
			end

		S_HDR, S_HDR2: if (!f_req && hdr_got == 8) begin
				if (!primed) begin
					// the mailbox as found: a session only once the player writes one
					primed  <= 1;
					session <= hdr[1];
					state   <= S_IDLE;
				end
				else if (hdr[0] == MAGIC && hdr[1] != session) begin
					if (state == S_HDR) begin
						// a new session: read the mailbox again, its values are
						// written before the session number
						f_req   <= 1;
						hdr_got <= 0;
						state   <= S_HDR2;
					end
					else begin
						session <= hdr[1];
						pages   <= (hdr[3] == 0 || hdr[3] > 1024) ? 11'd1024 : hdr[3][10:0];
						live    <= 0;
						stop    <= 0;
						e_reset <= 1;
						state   <= S_DRAIN;
					end
				end
				else if (live && hdr[0] != MAGIC) begin
					// the player left
					live    <= 0;
					stop    <= 1;
					e_reset <= 1;
					state   <= S_DRAIN;
				end
				else begin
					if (live) wp <= hdr[4];
					if (need_ans || rd_done != ans_rd) begin
						need_ans <= 0;
						ans_rd   <= rd_done;
						ans_i    <= 0;
						f_req    <= 1;
						f_we     <= 1;
						f_addr   <= MBOX[31:3] + 29'd8;
						f_burst  <= 1;
						f_din    <= ans_word(0);
						state    <= S_ANS;
					end
					else state <= live ? S_RUN : S_IDLE;
				end
			end

		S_DRAIN: begin
				// wait for the transfers under way, then reset the engine and
				// read the page table
				e_reset <= 1;
				if (!f_req && !out_n && !tq_n && !rq_n && !on_bus) begin
					rst_n   <= 32;
					cf_wp   <= 0;
					cf_rp   <= 0;
					cf_qv   <= 0;
					cf_resv <= 0;
					half    <= 0;
					wp      <= 0;
					rf      <= 0;
					rd_done <= 0;
					ans_rd  <= 0;
					ro      <= 0;
					rsize   <= {pages, 12'd0};
					pics    <= 0;
					clocks  <= 0;
					since   <= 0;
					starve  <= 0;
					mwait   <= 0;
					err     <= 0;
					t_left  <= t_all;
					t_got   <= 0;
					lk      <= 0;
					state   <= stop ? S_IDLE : S_TABLE;
				end
			end

		S_TABLE: begin
				if (!f_req && t_left) begin
					n = t_left > BURST ? 9'd32 : t_left[8:0];
					f_req   <= 1;
					f_we    <= 0;
					f_kind  <= K_TAB;
					f_addr  <= hdr[2][31:3] + (t_all - t_left);
					f_burst <= n[7:0];
					t_left  <= t_left - n;
				end
				if (!f_req && !t_left && t_got == t_all) begin
					live     <= 1;
					need_ans <= 1;
					wp       <= hdr[4];
					state    <= S_RUN;
				end
			end

		S_ANS: if (!f_req) begin
				// the answer, a word at a time
				if (ans_i == 4) begin
					f_we  <= 0;
					state <= live ? S_RUN : S_IDLE;
				end
				else begin
					ans_i  <= ans_i + 1'd1;
					f_req  <= 1;
					f_addr <= MBOX[31:3] + 29'd9 + ans_i;
					f_din  <= ans_word(ans_i + 1'd1);
				end
			end

		S_DEAD: begin
				e_reset <= 1;
				f_req   <= 0;
			end
	endcase
end

endmodule
