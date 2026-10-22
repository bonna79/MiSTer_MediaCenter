`timescale 1ns/1ps
// Engine test bench: feeds <name>.hex (commands, one word per line), keeps
// the frame memory, writes every finished picture to out.yuv (coded size)
// and prints the clocks per macroblock. Compare with check.py.
//   iverilog -DSLOT_L2=16 -DYS_L2=6 -DCS_L2=5 -DUOFF=16384 -DVOFF=20480 ...
module tb_engine;

`ifndef SLOT_L2
`define SLOT_L2 16
`define YS_L2 6
`define CS_L2 5
`define UOFF 16384
`define VOFF 20480
`endif
`ifndef NSLOT
`define NSLOT 8
`endif
`ifndef LAT
`define LAT 24
`endif

reg clk = 0;
always #5 clk = ~clk;
reg reset = 1;

reg  [31:0] cmd[0:`CMDMAX];
integer ncmd, cp = 0;
wire cmd_ready;
wire cmd_valid = cp < ncmd;

wire mem_rd, mem_we;
wire [27:0] mem_addr;
wire [7:0] mem_burst, mem_be;
wire [63:0] mem_din;
reg  mem_busy = 0;
reg  [63:0] mem_dout;
reg  mem_dout_ready = 0;
wire pic_done, error;
wire [7:0] pic_slot;
wire [31:0] mb_count;

acc_engine #(.SLOT_L2(`SLOT_L2), .YSTRIDE_L2(`YS_L2), .CSTRIDE_L2(`CS_L2), .UOFF(`UOFF), .VOFF(`VOFF)) dut
(
	.clk(clk), .reset(reset),
	.cmd_valid(cmd_valid), .cmd_data(cmd[cp]), .cmd_ready(cmd_ready),
	.mem_rd(mem_rd), .mem_we(mem_we), .mem_addr(mem_addr), .mem_burst(mem_burst), .mem_din(mem_din), .mem_be(mem_be),
	.mem_busy(mem_busy), .mem_dout(mem_dout), .mem_dout_ready(mem_dout_ready),
	.pic_done(pic_done), .pic_slot(pic_slot), .error(error), .mb_count(mb_count)
);

always @(posedge clk) if (cmd_valid && cmd_ready && !reset) cp <= cp + 1;

// memory: slot s kept at (s % NSLOT)
reg [63:0] mem[0:(`NSLOT << `SLOT_L2) - 1];
function integer maddr(input [27:0] a);
	maddr = ((a >> `SLOT_L2) % `NSLOT) * (1 << `SLOT_L2) + (a & ((1 << `SLOT_L2) - 1));
endfunction

// reads: in order, LAT clocks after the request, one word a clock
integer qa[0:255], qb[0:255], qt[0:255];
integer qh = 0, qtl = 0, beat = 0;
integer clocks = 0, seed = 7;
always @(posedge clk) begin
	clocks <= clocks + 1;
	mem_dout_ready <= 0;
	// busy now and then
	mem_busy <= ($random(seed) & 15) == 0;
	if (mem_rd && !mem_busy) begin
		qa[qtl % 256] = mem_addr;
		qb[qtl % 256] = mem_burst;
		qt[qtl % 256] = clocks + `LAT;
		qtl = qtl + 1;
	end
	if (mem_we && !mem_busy) mem[maddr(mem_addr)] <= mem_din;
	if (qh != qtl && qt[qh % 256] <= clocks) begin
		mem_dout <= mem[maddr(qa[qh % 256] + beat)];
		mem_dout_ready <= 1;
		beat = beat + 1;
		if (beat == qb[qh % 256]) begin beat = 0; qh = qh + 1; end
	end
end

integer fo, pics = 0, x, y, p, w, h, last_clk = 0, last_mb = 0;
reg [63:0] wd;
initial begin
	$readmemh(`CMDFILE, cmd);
	ncmd = `NCMD;
	fo = $fopen("out.yuv", "wb");
	repeat (4) @(posedge clk);
	reset <= 0;
end

always @(posedge clk) begin
	if (error) begin $display("engine error at command %0d", cp); $finish; end
	if (pic_done) begin
		w = dut.wmb * 16;
		h = dut.hmb * 16;
		for (p = 0; p < 3; p = p + 1)
			for (y = 0; y < (p ? h / 2 : h); y = y + 1)
				for (x = 0; x < (p ? w / 2 : w); x = x + 1) begin
					wd = mem[maddr((pic_slot << `SLOT_L2) + (p == 0 ? 0 : (p == 1 ? `UOFF : `VOFF)) +
					               (y << (p ? `CS_L2 : `YS_L2)) + x / 8)];
					$fwrite(fo, "%c", wd[(x % 8) * 8 +: 8]);
				end
		$display("picture %0d slot %0d: %0d macroblocks, %0d clocks, %0d clocks per macroblock",
			pics, pic_slot, mb_count - last_mb, clocks - last_clk, (clocks - last_clk) / (mb_count - last_mb));
		last_clk = clocks;
		last_mb = mb_count;
		pics = pics + 1;
		if (pics == `NPICS) begin $fclose(fo); $finish; end
	end
end

endmodule
