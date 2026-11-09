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
//     [19] 1: mc_zc blends the player's controls (mailbox word 10 bit 18)
//     [20] 1: mc_zc resamples any width and height (mailbox word 10 bit 19)
//     [21] 1: the clocks of words 21 to 25 are this block's own, 100 MHz
//          (0: the core clock, 108 MHz)
//     [22] 1: acc_parse reads CABAC slices (command A6, 4.0)
//     [2]  acc_parse found an error in a slice since the session started
//  21 clocks between the last two finished pictures
//  22 of them, clocks the engine had no command to read
//  23 of them, clocks the engine waited for the memory
//  24 macroblocks done            25 clocks since the session started
// The player may write over ring bytes once word 18 says they were read.
//
// Command A6 (4.0): 0xA6 << 24 | n, then n words for acc_parse (a CABAC
// slice: descriptor + slice data, see accel/parse/rtl/acc_parse.sv). As the
// engine takes A6, the next n ring words go to acc_parse (mc_parse, on its
// own clock clk_par) and the engine's commands come from acc_parse until it
// marks the end of the slice; then the ring again. acc_parse keeps the
// motion record of every macroblock (for later B pictures) at MOTION.
//
// The bus is shared with mc_out (the CRT output, always first) and mc_zc
// through mc_bus: this block starts a transfer only while granted.
// This block runs on its own clock (clk, 100 MHz from pll_acc); only the
// bus side of its queue runs on the bus clock (clk_bus, 108 MHz).
module mc_acc #(
	parameter [31:0] MBOX  = 32'h23E00000,
	parameter [31:0] SLOTS = 32'h30000000,
	parameter [31:0] MOTION = 32'h24000000   // acc_parse's motion records, 64 x 1.18 MB
)
(
	input             clk,           // this block's own clock
	input             clk_par,       // acc_parse's clock
	input             clk_bus,       // the bus clock: ddr_*, grant, want, idle
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
wire [63:0] e_din, e_din2;
wire        e_take;                     // the engine's request goes into the queue
wire        e_valid, e_ready;
wire [31:0] e_cmd;
wire        e_done, e_err;
wire  [7:0] e_slot;
wire [31:0] e_mbs;
wire        ps_go;                      // the engine took an A6
wire [23:0] ps_len;
reg  [63:0] e_dout;
reg         e_dout_ready = 0;

// 25-bit word addresses: 256 MB from SLOTS, slot numbers taken modulo 64
acc_engine #(.AW(25)) engine
(
	.clk(clk), .reset(e_reset),
	.cmd_valid(e_valid), .cmd_data(e_cmd), .cmd_ready(e_ready),
	.mem_rd(e_rd), .mem_we(e_we), .mem_addr(e_addr), .mem_burst(e_burst), .mem_din(e_din), .mem_din2(e_din2), .mem_be(e_be),
	.mem_busy(!e_take), .mem_dout(e_dout), .mem_dout_ready(e_dout_ready),
	.pic_done(e_done), .pic_slot(e_slot), .error(e_err), .mb_count(e_mbs),
	.ps_go(ps_go), .ps_len(ps_len)
);

// ================================================================ request queue
// Every transfer (the engine's, the ring, page table and mailbox reads, the
// answer) goes through this queue, in order. Reads also leave a tag saying
// whose data come back and how many words.
// The queue crosses from this block's clock to the bus clock (clk_bus) in a
// FIFO (mc_afifo), and the read data come back in another one. cr counts the
// words asked for and not yet arrived: they always have room in the second.
localparam K_ENG = 0, K_MBOX = 1, K_TAB = 2, K_RING = 3, K_PAR = 4;
localparam RQ_L2 = 4;                   // 16 requests
localparam RX_L2 = 9;                   // 512 words on their way back
wire       rq_room;                     // room for a request (counted a clock late)

localparam TQ = 32;
reg  [4:0] tq_wp = 0, tq_rp = 0;
reg  [5:0] tq_n = 0;
reg        tq_room = 1;
reg  [2:0] tq_kind[TQ];
reg  [7:0] tq_len[TQ];
reg  [7:0] rx_n = 0;                    // words of the head tag arrived
reg  [9:0] cr = 0;                      // read words asked for, not arrived
reg        cr_ok = 1;                   // room for 32 more, counted a clock late

// this block's own requests
reg        f_req = 0, f_we = 0;
reg  [2:0] f_kind = K_MBOX;
reg [28:0] f_addr;
reg  [7:0] f_burst;
reg [63:0] f_din;
wire       f_take = f_req && rq_room && (f_we || (tq_room && cr_ok));

// acc_parse's motion memory transfers (m_*, below): after this block's own,
// before the engine's
reg        m_req = 0, m_we = 0;
reg [28:0] m_addr;
reg  [7:0] m_burst;
reg [63:0] m_din, m_din2;
wire       m_take = m_req && !f_req && rq_room && (m_we || (tq_room && cr_ok));

assign e_take = (e_rd || e_we) && !e_reset && !f_req && !m_req && rq_room && (e_we || (tq_room && cr_ok));

wire        push    = f_take || m_take || e_take;
wire        p_we    = f_take ? f_we : m_take ? m_we : e_we;
wire [28:0] p_addr  = f_take ? f_addr : m_take ? m_addr : {SLOTS[31:28], e_addr};
wire  [7:0] p_burst = f_take ? f_burst : m_take ? m_burst : e_burst;
wire [63:0] p_din   = f_take ? f_din : m_take ? m_din : e_din;
wire [63:0] p_din2  = m_take ? m_din2 : e_din2;
wire  [2:0] p_kind  = f_take ? f_kind : m_take ? K_PAR[2:0] : 3'd0;
wire        tpush   = push && !p_we;

// requests handed to the bus and finished there (Gray code, from clk_bus)
reg  [4:0] a_push = 0;
reg  [4:0] b_done_g = 0, b_done_s1 = 0, b_done_s2 = 0;
reg        b_out = 0, b_out_s1 = 0, b_out_s2 = 0;   // reads on the memory, not all arrived
always @(posedge clk) begin
	b_done_s1 <= b_done_g;
	b_done_s2 <= b_done_s1;
	b_out_s1  <= b_out;
	b_out_s2  <= b_out_s1;
	if (push) a_push <= a_push + 1'd1;
end
wire q_done = b_done_s2 == (a_push ^ (a_push >> 1));

// ------------------------------------------------ on the bus clock
wire         h_valid;
wire [165:0] h_q;
wire         h_we    = h_q[165];
wire  [28:0] h_addr  = h_q[164:136];
wire   [7:0] h_burst = h_q[135:128];
wire  [63:0] h_din   = h_q[127:64];
wire  [63:0] h_din2  = h_q[63:0];

// the head of the queue goes on the bus; a write burst of two keeps the
// bus for its second word (w2: the first word is out)
reg        w2 = 0;
reg [63:0] din2;
reg  [9:0] out_w = 0;                   // read words on the memory, not arrived
reg  [4:0] b_done = 0;
wire on_bus = ddr_rd || ddr_we;
wire bus_ok = on_bus && !ddr_busy;
wire pop    = h_valid && (!on_bus || (bus_ok && !w2)) && grant;

mc_afifo #(.W(166), .L2(RQ_L2), .ROOM(3)) rq
(
	.wclk(clk), .we(push), .wd({p_we, p_addr, p_burst, p_din, p_din2}), .room(rq_room),
	.rclk(clk_bus), .rd(pop), .q(h_q), .q_valid(h_valid)
);

always @(posedge clk_bus) begin
	reg [4:0] d;
	d = b_done;
	if (bus_ok) begin
		if (w2) begin
			ddr_din <= din2;
			w2 <= 0;
		end
		else begin
			ddr_rd <= 0;
			ddr_we <= 0;
			d = b_done + 1'd1;
		end
	end
	if (pop) begin
		ddr_rd    <= !h_we;
		ddr_we    <= h_we;
		ddr_addr  <= h_addr;
		ddr_burst <= h_burst;
		ddr_din   <= h_din;
		din2      <= h_din2;
		w2        <= h_we && h_burst == 8'd2;
	end
	b_done   <= d;
	b_done_g <= d ^ (d >> 1);
	out_w    <= out_w + ((bus_ok && ddr_rd) ? ddr_burst : 8'd0) - ddr_dout_ready;
	b_out    <= out_w != 0;
end

assign want = h_valid && !on_bus;
assign idle = !on_bus && !out_w;

// read data back to this block's clock
wire        rx;
wire [63:0] rx_d;
mc_afifo #(.W(64), .L2(RX_L2), .ROOM(3)) rxq
(
	.wclk(clk_bus), .we(ddr_dout_ready), .wd(ddr_dout), .room(),
	.rclk(clk), .rd(rx), .q(rx_d), .q_valid(rx)
);

// read data: to the engine or to this block, by the head tag
wire [2:0] rx_kind = tq_kind[tq_rp];
wire       rx_last = rx && rx_n + 1'd1 == tq_len[tq_rp];
reg        f_rx = 0;                    // a word for this block
reg  [2:0] f_rx_kind;
reg [63:0] f_dout;
reg [63:0] rx_d_q;
reg        mret_we = 0;

always @(posedge clk) begin
	reg [5:0] n;
	reg [9:0] c;
	if (tpush) begin
		tq_kind[tq_wp] <= p_kind;
		tq_len[tq_wp]  <= p_burst;
		tq_wp <= tq_wp + 1'd1;
	end
	if (rx_last) tq_rp <= tq_rp + 1'd1;
	rx_n <= rx_last ? 8'd0 : rx_n + rx;
	n = tq_n + tpush - rx_last;
	tq_n    <= n;
	tq_room <= n <= TQ - 3;
	c = cr + (tpush ? p_burst : 8'd0) - rx;
	cr      <= c;
	cr_ok   <= c <= (1 << RX_L2) - 64;

	e_dout_ready <= rx && rx_kind == K_ENG;
	f_rx         <= rx && rx_kind != K_ENG && rx_kind != K_PAR;
	mret_we      <= rx && rx_kind == K_PAR;
	f_rx_kind    <= rx_kind;
	e_dout       <= rx_d;
	f_dout       <= rx_d;
	rx_d_q       <= rx_d;
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

wire [31:0] r_word = half ? cf_q[63:32] : cf_q[31:0];

// ------------------------------------------------ acc_parse and the switch
// p_left: ring words still for acc_parse (after an A6); p_src: the engine
// reads acc_parse's commands; p_disc: acc_parse ended the slice early, the
// rest of its words are dropped.
reg  [23:0] p_left = 0;
reg         p_src = 0, p_disc = 0;
wire        pin_room;
wire [32:0] po_q;
wire        po_valid;
wire [64:0] mq;
wire        mq_valid;
wire        mq_rd;
wire        p_errp;
wire        po_rd, p_in;

mc_parse par
(
	.clk(clk), .pclk(clk_par), .reset(e_reset),
	.in_we(p_in), .in_wd(r_word), .in_room(pin_room),
	.out_rd(po_rd), .out_q(po_q), .out_valid(po_valid),
	.mreq_rd(mq_rd), .mreq_q(mq), .mreq_valid(mq_valid),
	.mret_we(mret_we), .mret_wd(rx_d_q),
	.err_pulse(p_errp)
);

wire po_last = po_q[32];
assign p_in  = cf_qv && p_left != 0 && !p_disc && pin_room && !e_reset;
wire p_drop  = cf_qv && p_left != 0 && p_disc;

assign e_valid = !e_reset && (p_src ? (po_valid && !po_last) : (cf_qv && p_left == 0));
assign e_cmd   = p_src ? po_q[31:0] : r_word;
wire e_used  = e_valid && e_ready;
assign po_rd = p_src && po_valid && (po_last || e_ready);
wire r_used  = p_in || p_drop || (e_used && !p_src);
wire cf_next = cf_wp != cf_rp && (!cf_qv || (r_used && half));

always @(posedge clk) begin
	if (e_reset) begin
		p_left <= 0;
		p_src  <= 0;
		p_disc <= 0;
	end
	else begin
		if (ps_go) begin
			p_left <= ps_len;
			p_src  <= 1;
			p_disc <= 0;
		end
		else if (p_in || p_drop) p_left <= p_left - 1'd1;
		if (po_rd && po_last) begin
			p_src <= 0;
			if (p_left > {23'd0, p_in || p_drop}) p_disc <= 1;
		end
	end
end

// ------------------------------------------------ acc_parse's motion memory
// Requests come one 32-bit word at a time, a record of 35 words at
// word * 36 (18 64-bit words, the last half unused). Writes are paired into
// 64-bit words and sent two at a time (a write burst of two); the first read
// of a record reads its 18 words in one burst, the next 34 just pass.
reg  [5:0]  mw_k = 0, mr_k = 0;         // words of the record written, read
reg  [31:0] mw_lo;
reg  [63:0] mw_p0;
reg         mw_have = 0;                // mw_p0 holds the burst's first word
wire        mq_we   = mq[64];
wire [31:0] mq_addr = mq[63:32];
wire [31:0] mq_data = mq[31:0];
assign mq_rd = mq_valid && !m_req && !e_reset;
always @(posedge clk) begin
	if (m_take) m_req <= 0;
	if (e_reset) begin
		m_req   <= 0;
		mw_k    <= 0;
		mr_k    <= 0;
		mw_have <= 0;
	end
	else if (mq_rd) begin
		if (mq_we) begin
			mw_k <= mw_k == 6'd34 ? 6'd0 : mw_k + 1'd1;
			if (!mq_addr[0] && mw_k != 6'd34) mw_lo <= mq_data;
			else begin
				// a 64-bit word is complete (the record's last one alone)
				if (!mw_have) begin
					mw_p0   <= mq_addr[0] ? {mq_data, mw_lo} : {32'd0, mq_data};
					mw_have <= 1;
				end
				else begin
					m_req   <= 1;
					m_we    <= 1;
					m_addr  <= MOTION[31:3] + {mq_addr[31:1] - 31'd1};
					m_burst <= 8'd2;
					m_din   <= mw_p0;
					m_din2  <= mq_addr[0] ? {mq_data, mw_lo} : {32'd0, mq_data};
					mw_have <= 0;
				end
			end
		end
		else begin
			mr_k <= mr_k == 6'd34 ? 6'd0 : mr_k + 1'd1;
			if (mr_k == 6'd0) begin
				m_req   <= 1;
				m_we    <= 0;
				m_addr  <= MOTION[31:3] + mq_addr[31:1];
				m_burst <= 8'd18;
			end
		end
	end
end

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
reg        err = 0, dead = 0, p_err = 0;
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
		i == 2 ? {l_since, 9'd0, 7'b1111101, l_slot, 5'd0, p_err, dead, err} :
		i == 3 ? {l_mwait, l_starve} :
		         {clocks, e_mbs};
endfunction

reg         en_s1 = 0, en_s2 = 0;
always @(posedge clk) begin en_s1 <= enable; en_s2 <= en_s1; end
wire        poll_now = en_s2 && &poll && !f_req;
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
	if (r_used) half <= !half;
	if (cf_next) begin
		cf_rp <= cf_rp + 1'd1;
		cf_qv <= 1;
	end
	else if (r_used && half) cf_qv <= 0;
	cf_resv <= cf_resv - (r_used && half) + ((f_take && f_kind == K_RING) ? f_burst : 8'd0);
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
	if (p_errp && !p_err) begin p_err <= 1; need_ans <= 1; end

	// ------------------------------------------------ the memory never answers
	if (b_out_s2 && !rx) begin
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
				if (!f_req && !tq_n && q_done) begin
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
					p_err   <= 0;
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
