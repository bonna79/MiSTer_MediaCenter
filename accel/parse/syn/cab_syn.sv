// Wrapper for the synthesis-only project: registers on every port so that
// the timing seen is the block's own.
module cab_syn
(
	input             clk,
	input             cmd_valid,
	input       [2:0] cmd_op,
	input      [11:0] cmd_arg,
	output reg        cmd_ready,
	input             in_valid,
	input      [31:0] in_data,
	output reg        in_ready,
	output reg        out_valid,
	output reg  [1:0] out_kind,
	output reg  [6:0] out_pos,
	output reg [31:0] out_val
);

reg        reset = 1;
reg        r_cv, r_iv;
reg  [2:0] r_op;
reg [11:0] r_arg;
reg [31:0] r_in;
wire       c_ready, i_ready, o_valid;
wire [1:0] o_kind;
wire [6:0] o_pos;
wire [31:0] o_val;

always @(posedge clk) begin
	reset <= 0;
	r_cv <= cmd_valid;
	r_op <= cmd_op;
	r_arg <= cmd_arg;
	r_iv <= in_valid;
	r_in <= in_data;
	cmd_ready <= c_ready;
	in_ready <= i_ready;
	out_valid <= o_valid;
	out_kind <= o_kind;
	out_pos <= o_pos;
	out_val <= o_val;
end

cab_dec dec
(
	.clk(clk),
	.reset(reset),
	.cmd_valid(r_cv),
	.cmd_ready(c_ready),
	.cmd_op(r_op),
	.cmd_arg(r_arg),
	.in_valid(r_iv),
	.in_ready(i_ready),
	.in_data(r_in),
	.out_valid(o_valid),
	.out_kind(o_kind),
	.out_pos(o_pos),
	.out_val(o_val)
);

endmodule
