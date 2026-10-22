// Simple dual-port block RAM (one write port, one read port, read data on
// the next clock), written so that Quartus maps it to M10K.
module acc_ram #(
	parameter W = 8,
	parameter A = 7
)
(
	input              clk,
	input              we,
	input      [A-1:0] wa,
	input      [W-1:0] wd,
	input      [A-1:0] ra,
	output reg [W-1:0] q
);

(* ramstyle = "M10K, no_rw_check" *) reg [W-1:0] mem[0:(1 << A) - 1];
always @(posedge clk) begin
	if (we) mem[wa] <= wd;
	q <= mem[ra];
end

endmodule
