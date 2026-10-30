`timescale 1ns/1ps
// Test bench of the accelerator inside the menu core: mc_acc (ring, mailbox,
// engine) on mc_bus with a stand-in for mc_out (the CRT output, first on the
// bus) and a DDR model (latency, busy now and then, reads in order).
// The test bench plays the player: it writes the commands of cmd.hex into a
// ring of RING_PAGES pages scattered in memory, only where the answer says
// the bytes were read, and writes every finished picture to out.yuv.
module tb_core;

`ifndef LAT
`define LAT 30
`endif
`ifndef RING_PAGES
`define RING_PAGES 6
`endif
`ifndef NSLOT
`define NSLOT 8
`endif
localparam [31:0] MBOX = 32'h23E00000;
localparam [31:0] SLOTS = 32'h30000000;

reg clk = 0;
always #4.63 clk = ~clk;   // 108 MHz

reg  [31:0] cmd[0:`CMDMAX];
integer ncmd;

// ------------------------------------------------------------ memory
reg [63:0] mbox[0:2047];                 // MBOX .. +16 KB (mailbox, page table)
reg [63:0] ring[0:`RING_PAGES * 512 - 1];
reg [63:0] slot[0:(`NSLOT << 19) - 1];
integer pfn[0:`RING_PAGES - 1];
integer bad_addr = 0;

function integer region(input [28:0] a);   // 0 mailbox, 1 ring, 2 slots, -1 other
	integer k;
	begin
		region = -1;
		if (a >= MBOX[31:3] && a < MBOX[31:3] + 2048) region = 0;
		else if (a >= SLOTS[31:3] && a < SLOTS[31:3] + (1 << 25)) region = 2;
		else for (k = 0; k < `RING_PAGES; k = k + 1) if (a[28:9] == pfn[k]) region = 1;
	end
endfunction
function integer ring_i(input [28:0] a);
	integer k;
	begin
		ring_i = 0;
		for (k = 0; k < `RING_PAGES; k = k + 1) if (a[28:9] == pfn[k]) ring_i = k * 512 + a[8:0];
	end
endfunction
function integer slot_i(input [28:0] a);
	slot_i = (((a - SLOTS[31:3]) >> 19) % `NSLOT) * (1 << 19) + ((a - SLOTS[31:3]) & ((1 << 19) - 1));
endfunction

task mem_write(input [28:0] a, input [63:0] d);
	case (region(a))
		0: mbox[a - MBOX[31:3]] = d;
		1: ring[ring_i(a)] = d;
		2: slot[slot_i(a)] = d;
		default: bad_addr = bad_addr + 1;
	endcase
endtask
function [63:0] mem_read(input [28:0] a);
	case (region(a))
		0: mem_read = mbox[a - MBOX[31:3]];
		1: mem_read = ring[ring_i(a)];
		2: mem_read = slot[slot_i(a)];
		default: mem_read = 64'hDEAD_BEEF_DEAD_BEEF;
	endcase
endfunction

// ------------------------------------------------------------ the bus
wire [28:0] a_addr, bus_addr;
wire  [7:0] a_burst, bus_burst;
wire [63:0] a_din, bus_din;
wire        a_rd, a_we, a_want, a_idle, a_grant, a_ready;
wire        m_grant, m_ready, bus_rd, bus_we, bus_idle;
reg         busy = 0;
reg  [63:0] dout;
reg         dout_ready = 0;

mc_acc dut
(
	.clk(clk), .enable(1'b1),
	.ddr_addr(a_addr), .ddr_burst(a_burst), .ddr_rd(a_rd), .ddr_we(a_we), .ddr_din(a_din),
	.ddr_busy(busy), .ddr_dout(dout), .ddr_dout_ready(a_ready),
	.grant(a_grant), .want(a_want), .idle(a_idle)
);

// mc_out's stand-in: a line of 180 words every 6912 clocks (13.5 MHz, 64 us),
// in bursts of 64, asked for at a random moment of the line
reg  [28:0] m_addr = 29'h4000000;
reg   [7:0] m_burst = 64;
reg         m_rd = 0;
reg         m_want = 0;
integer     m_left = 0, m_got = 0, m_t0 = 0, m_worst = 0, clocks = 0, m_next = 3000;
wire        m_idle = !m_want;

mc_bus bus
(
	.clk(clk), .own(1'b1),
	.m_addr(m_addr), .m_burst(m_burst), .m_rd(m_rd), .m_idle(m_idle), .m_grant(m_grant), .m_ready(m_ready),
	.z_addr(29'd0), .z_burst(8'd0), .z_rd(1'b0), .z_we(1'b0), .z_din(64'd0), .z_want(1'b0), .z_idle(1'b1),
	.z_grant(), .z_ready(),
	.a_addr(a_addr), .a_burst(a_burst), .a_rd(a_rd), .a_we(a_we), .a_din(a_din), .a_want(a_want), .a_idle(a_idle),
	.a_grant(a_grant), .a_ready(a_ready),
	.busy(busy), .ready(dout_ready),
	.addr(bus_addr), .burst(bus_burst), .rd(bus_rd), .we(bus_we), .din(bus_din), .idle(bus_idle)
);

always @(posedge clk) begin
	clocks <= clocks + 1;
	if (!m_want && clocks >= m_next) begin
		m_want <= 1;
		m_left <= 180;
		m_got  <= 0;
		m_t0   <= clocks;
		m_next <= m_next + 6912;
	end
	if (m_rd && !busy) m_rd <= 0;
	if (m_want && m_grant && !m_rd && m_left) begin
		m_rd    <= 1;
		m_burst <= m_left > 64 ? 64 : m_left;
		m_left  <= m_left - (m_left > 64 ? 64 : m_left);
	end
	if (m_ready) begin
		m_got <= m_got + 1;
		if (m_got + 1 == 180) begin
			m_want <= 0;
			if (clocks - m_t0 > m_worst) m_worst <= clocks - m_t0;
		end
	end
end

// DDR: reads in order LAT clocks after the request, a word a clock
integer qa[0:255], qb[0:255], qt[0:255];
integer qh = 0, qtl = 0, beat = 0, seed = 11;
integer wbeat = 0, wb_a = 0, wb_n = 0;
always @(posedge clk) begin
	dout_ready <= 0;
	busy <= ($random(seed) & 7) == 0;
	if (bus_rd && !busy) begin
		qa[qtl % 256] = bus_addr;
		qb[qtl % 256] = bus_burst;
		qt[qtl % 256] = clocks + `LAT;
		qtl = qtl + 1;
	end
	// a write burst gives all its words one after the other, same address
	if (wbeat != 0 && (!bus_we || bus_rd || bus_addr != wb_a || bus_burst != wb_n)) begin
		$display("write burst broken at clock %0d: beat %0d of %0d", clocks, wbeat, wb_n);
		$finish;
	end
	if (bus_we && !busy) begin
		mem_write(bus_addr + wbeat, bus_din);
		if (wbeat == 0) begin wb_a = bus_addr; wb_n = bus_burst; end
		wbeat = (wbeat + 1 == bus_burst) ? 0 : wbeat + 1;
	end
	if (qh != qtl && qt[qh % 256] <= clocks) begin
		dout <= mem_read(qa[qh % 256] + beat);
		dout_ready <= 1;
		beat = beat + 1;
		if (beat == qb[qh % 256]) begin beat = 0; qh = qh + 1; end
	end
end

// ------------------------------------------------------------ the player
integer wp = 0, total, i, k, seen_pics = 0, rd_back = 0;
reg [63:0] w;
initial begin
	$readmemh(`CMDFILE, cmd);
	ncmd = `NCMD;
	if (ncmd & 1) begin cmd[ncmd] = 32'hA0000000; ncmd = ncmd + 1; end
	total = ncmd * 4;
	for (k = 0; k < `RING_PAGES; k = k + 1) pfn[k] = 20'h1000 + k * 37 + (k & 1) * 5;
	for (i = 0; i < 2048; i = i + 1) mbox[i] = 0;
	// the page table at MBOX + 4096
	for (k = 0; k < `RING_PAGES; k = k + 1) begin
		w = mbox[512 + k / 2];
		if (k & 1) w[63:32] = pfn[k]; else w[31:0] = pfn[k];
		mbox[512 + k / 2] = w;
	end
	// the core first finds an empty mailbox, then the player starts a session
	repeat (3000) @(posedge clk);
	mbox[1] = {32'd`RING_PAGES, MBOX + 32'd4096};     // words 2, 3
	mbox[2] = 0;                                     // write pointer
	mbox[0] = {32'd1, 32'h4341434D};                 // session 1, "MCAC"
	// wait for the core's answer
	while (mbox[8][31:0] != 32'h4B4F4341 || mbox[8][63:32] != 1) @(posedge clk);
	$display("core answered at %0d clocks", clocks);
	while (wp < total) begin
		// room: bytes read by the core (answer word 18)
		rd_back = mbox[9][31:0];
		if (wp + 8 - rd_back <= `RING_PAGES * 4096) begin
			// a word into the ring
			k = (wp / 8) % (`RING_PAGES * 512);
			ring[k] = {cmd[wp / 4 + 1], cmd[wp / 4]};
			wp = wp + 8;
			if ((wp & 255) == 0 || wp == total) mbox[2][31:0] = wp;
		end
		else @(posedge clk);
	end
	$display("all commands written at %0d clocks", clocks);
end

// every command the engine takes must be the next one of the stream
integer ci = 0;
always @(posedge clk) if (dut.e_used) begin
	if (dut.e_cmd !== cmd[ci]) begin
		$display("command %0d: engine got %08x, expected %08x (clock %0d)", ci, dut.e_cmd, cmd[ci], clocks);
		$finish;
	end
	ci <= ci + 1;
end

// pictures as they finish
integer fo, pics = 0, x, y, p, wd, ht, last_clk = 0, last_mb = 0;
reg [63:0] pw;
reg [7:0] dump_slot;
reg dump_wait = 0;
initial fo = $fopen("out.yuv", "wb");
always @(posedge clk) begin
	if (dut.e_err) begin $display("engine error"); $finish; end
	if (dut.dead) begin $display("memory dead"); $finish; end
	// a picture is in memory once its last writes left the queue
	if (dut.engine.pic_done) begin dump_slot <= dut.engine.pic_slot; dump_wait <= 1; end
	if (dump_wait && dut.rq_n == 0 && !dut.on_bus && !dut.engine.pic_done) begin
		dump_wait <= 0;
		wd = dut.engine.wmb * 16;
		ht = dut.engine.hmb * 16;
		for (p = 0; p < 3; p = p + 1)
			for (y = 0; y < (p ? ht / 2 : ht); y = y + 1)
				for (x = 0; x < (p ? wd / 2 : wd); x = x + 1) begin
					pw = slot[(dump_slot % `NSLOT) * (1 << 19) + (p == 0 ? 0 : (p == 1 ? 278528 : 348160)) +
					          (y << (p ? 7 : 8)) + x / 8];
					$fwrite(fo, "%c", pw[(x % 8) * 8 +: 8]);
				end
		$display("picture %0d slot %0d: %0d macroblocks, %0d clocks, %0d per macroblock",
			pics, dump_slot, dut.engine.mb_count - last_mb, clocks - last_clk,
			(clocks - last_clk) / (dut.engine.mb_count - last_mb));
		last_clk = clocks;
		last_mb = dut.engine.mb_count;
		pics = pics + 1;
	end
	if (pics == `NPICS && mbox[9][63:32] == `NPICS) begin
		$display("answer: %0d pictures, %0d bytes read of %0d; last %0d clocks, waiting for commands %0d, for memory %0d; %0d macroblocks",
			mbox[9][63:32], mbox[9][31:0], total, mbox[10][63:32], mbox[11][31:0], mbox[11][63:32], mbox[12][31:0]);
		$display("mc_out stand-in: worst line %0d clocks (limit 6912); writes outside the memory map: %0d", m_worst, bad_addr);
		$fclose(fo);
		$finish;
	end
end

endmodule
