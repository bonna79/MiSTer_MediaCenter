`timescale 1ns/1ps
module tb_idct;
reg clk = 0;
always #5 clk = ~clk;
reg clr = 0, t8 = 0, wr = 0, go = 0;
reg [5:0] pos;
reg signed [15:0] val;
wire busy, rv;
wire [2:0] rc;
wire signed [15:0] res[8];
acc_idct dut(.clk(clk), .clr(clr), .t8(t8), .wr(wr), .wr_pos(pos), .wr_val(val), .go(go), .busy(busy),
	.res_valid(rv), .res_col(rc), .res(res));
integer f, n, i, p, v, errs = 0, blocks = 0, e;
reg [15:0] exp[64];
reg [15:0] got[64];
initial begin
	f = $fopen("idct_vec.txt", "r");
	while ($fscanf(f, "%d %d", t8, n) == 2) begin
		@(posedge clk) clr <= 1;
		@(posedge clk) clr <= 0;
		for (i = 0; i < n; i++) begin
			e = $fscanf(f, "%d %d", p, v);
			wr <= 1; pos <= p; val <= v;
			@(posedge clk);
		end
		wr <= 0;
		for (i = 0; i < (t8 ? 64 : 16); i++) e = $fscanf(f, "%d", exp[i]);
		go <= 1;
		@(posedge clk) go <= 0;
		@(posedge clk);
		while (busy || rv) begin
			if (rv) for (i = 0; i < (t8 ? 8 : 4); i++) got[i * (t8 ? 8 : 4) + rc] = res[i];
			@(posedge clk);
		end
		for (i = 0; i < (t8 ? 64 : 16); i++) if (got[i] !== exp[i]) begin
			if (errs < 10) $display("block %0d t8 %0d pos %0d: got %0d expected %0d", blocks, t8, i, $signed(got[i]), $signed(exp[i]));
			errs++;
		end
		blocks++;
	end
	$display("idct: %0d blocks, %0d errors", blocks, errs);
	$finish;
end
endmodule
