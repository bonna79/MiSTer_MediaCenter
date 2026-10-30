// Wrapper for the synthesis-only project: registers on every port so that
// the timing seen is the engine's own.
module acc_syn
(
	input             clk,
	input             cmd_valid,
	input      [31:0] cmd_data,
	output reg        cmd_ready,
	output reg        mem_rd,
	output reg        mem_we,
	output reg [27:0] mem_addr,
	output reg  [7:0] mem_burst,
	output reg [63:0] mem_din,
	output reg [63:0] mem_din2,
	input             mem_busy,
	input      [63:0] mem_dout,
	input             mem_dout_ready,
	output reg [31:0] st_count
);

reg        r_valid, r_busy, r_ready, reset = 1;
reg [31:0] r_data;
reg [63:0] r_dout;
wire       e_ready, e_rd, e_we, e_done;
wire [27:0] e_addr;
wire [7:0] e_burst, e_be, e_slot;
wire [63:0] e_din, e_din2;
wire [31:0] e_mbs;
wire       e_err;

always @(posedge clk) begin
	reset <= 0;
	r_valid <= cmd_valid;
	r_data <= cmd_data;
	r_busy <= mem_busy;
	r_dout <= mem_dout;
	r_ready <= mem_dout_ready;
	cmd_ready <= e_ready;
	mem_rd <= e_rd;
	mem_we <= e_we;
	mem_addr <= e_addr;
	mem_burst <= e_burst;
	mem_din <= e_din;
	mem_din2 <= e_din2;
	st_count <= e_mbs ^ {e_slot, 23'd0, e_err};
end

acc_engine engine
(
	.clk(clk), .reset(reset),
	.cmd_valid(r_valid), .cmd_data(r_data), .cmd_ready(e_ready),
	.mem_rd(e_rd), .mem_we(e_we), .mem_addr(e_addr), .mem_burst(e_burst), .mem_din(e_din), .mem_din2(e_din2), .mem_be(e_be),
	.mem_busy(r_busy), .mem_dout(r_dout), .mem_dout_ready(r_ready),
	.pic_done(e_done), .pic_slot(e_slot), .error(e_err), .mb_count(e_mbs)
);

endmodule
