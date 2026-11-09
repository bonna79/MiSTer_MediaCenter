// Wrapper for the synthesis-only project of acc_parse (step 2 of 4.0):
// registers on every port so that the timing seen is the block's own.
module apar_syn
(
	input             clk,
	input             in_valid,
	input      [31:0] in_data,
	output reg        in_ready,
	output reg        out_valid,
	input             out_ready,
	output reg [31:0] out_data,
	output reg        mem_req,
	output reg        mem_we,
	output reg [31:0] mem_addr,
	output reg [31:0] mem_wdata,
	input             mem_ack,
	input             mem_rvalid,
	input      [31:0] mem_rdata,
	output reg        sl_done,
	output reg        sl_err,
	output reg        busy
);

reg        reset = 1;
reg        r_iv, r_or, r_ack, r_rv;
reg [31:0] r_in, r_rd;
wire       i_ready, o_valid, m_req, m_we, s_done, s_err, b;
wire [31:0] o_data, m_addr, m_wdata;

always @(posedge clk) begin
	reset <= 0;
	r_iv <= in_valid;
	r_in <= in_data;
	r_or <= out_ready;
	r_ack <= mem_ack;
	r_rv <= mem_rvalid;
	r_rd <= mem_rdata;
	in_ready <= i_ready;
	out_valid <= o_valid;
	out_data <= o_data;
	mem_req <= m_req;
	mem_we <= m_we;
	mem_addr <= m_addr;
	mem_wdata <= m_wdata;
	sl_done <= s_done;
	sl_err <= s_err;
	busy <= b;
end

acc_parse ap
(
	.clk(clk),
	.reset(reset),
	.in_valid(r_iv),
	.in_ready(i_ready),
	.in_data(r_in),
	.out_valid(o_valid),
	.out_ready(r_or),
	.out_data(o_data),
	.mem_req(m_req),
	.mem_we(m_we),
	.mem_addr(m_addr),
	.mem_wdata(m_wdata),
	.mem_ack(r_ack),
	.mem_rvalid(r_rv),
	.mem_rdata(r_rd),
	.sl_done(s_done),
	.sl_err(s_err),
	.busy(b)
);

endmodule
