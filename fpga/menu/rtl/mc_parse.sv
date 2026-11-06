// acc_parse (4.0: CABAC and macroblock reading, accel/parse/rtl) on its own
// clock (pclk), with the FIFOs that carry its words to and from mc_acc's
// clock (clk):
//   in   slice words from the ring (descriptor + slice data), clk -> pclk
//   out  the engine commands acc_parse writes, pclk -> clk; bit 32 marks the
//        end of a slice (a word of its own, after the slice's last command)
//   mreq motion memory requests {we, 32-bit word address, data}, pclk -> clk
//   mret motion memory read data, 64-bit words (two 32-bit words each, low
//        first), clk -> pclk; a record is 35 words, so the 36th is dropped
// While reset (clk side) is high, acc_parse is held in reset and every FIFO
// is emptied on its read side, so a new session starts clean.
module mc_parse
(
	input             clk,
	input             pclk,
	input             reset,          // clk side

	input             in_we,          // clk side
	input      [31:0] in_wd,
	output            in_room,

	input             out_rd,         // clk side
	output     [32:0] out_q,
	output            out_valid,

	input             mreq_rd,        // clk side
	output     [64:0] mreq_q,
	output            mreq_valid,

	input             mret_we,        // clk side
	input      [63:0] mret_wd,

	output            err_pulse       // clk side: a slice with an error
);

// reset into pclk's domain
reg prst_s1 = 1, prst = 1;
always @(posedge pclk) begin prst_s1 <= reset; prst <= prst_s1; end

// ------------------------------------------------ slice words in
wire [31:0] pin_q;
wire        pin_valid;
wire        ap_in_ready;
mc_afifo #(.W(32), .L2(5), .ROOM(3)) fin
(
	.wclk(clk), .we(in_we), .wd(in_wd), .room(in_room),
	.rclk(pclk), .rd(pin_valid && (prst || ap_in_ready)), .q(pin_q), .q_valid(pin_valid)
);

// ------------------------------------------------ acc_parse
wire        ap_out_valid, ap_out_ready;
wire [31:0] ap_out;
wire        ap_mem_req, ap_mem_we;
wire [31:0] ap_mem_addr, ap_mem_wdata;
wire        ap_mem_ack;
reg         ap_rvalid = 0;
reg  [31:0] ap_rdata;
wire        ap_done, ap_err, ap_busy;

acc_parse ap
(
	.clk(pclk), .reset(prst),
	.in_valid(pin_valid && !prst), .in_ready(ap_in_ready), .in_data(pin_q),
	.out_valid(ap_out_valid), .out_ready(ap_out_ready), .out_data(ap_out),
	.mem_req(ap_mem_req), .mem_we(ap_mem_we), .mem_addr(ap_mem_addr), .mem_wdata(ap_mem_wdata),
	.mem_ack(ap_mem_ack), .mem_rvalid(ap_rvalid), .mem_rdata(ap_rdata),
	.sl_done(ap_done), .sl_err(ap_err), .busy(ap_busy)
);

// ------------------------------------------------ commands out, then the end mark
// sl_done comes many clocks after the slice's last command word is written
// (the motion record of the last macroblock is written in between); the
// mark still waits for acc_parse's own output queue to be empty.
wire out_room;
reg  done_l = 0;
reg  [2:0] done_d = 0;
wire mark = done_l && done_d == 3'd7 && !ap_out_valid && out_room;
assign ap_out_ready = out_room && !prst;
always @(posedge pclk) begin
	if (prst) begin
		done_l <= 0;
		done_d <= 0;
	end
	else begin
		if (ap_done) begin done_l <= 1; done_d <= 0; end
		else if (done_l && done_d != 3'd7) done_d <= done_d + 1'd1;
		if (mark) done_l <= 0;
	end
end
mc_afifo #(.W(33), .L2(9), .ROOM(4)) fout
(
	.wclk(pclk), .we((ap_out_valid && ap_out_ready) || mark), .wd(mark ? {1'b1, 32'd0} : {1'b0, ap_out}), .room(out_room),
	.rclk(clk), .rd(out_valid && (reset || out_rd)), .q(out_q), .q_valid(out_valid)
);

// ------------------------------------------------ motion memory requests
wire mreq_room;
assign ap_mem_ack = ap_mem_req && mreq_room && !prst;
mc_afifo #(.W(65), .L2(5), .ROOM(4)) fmreq
(
	.wclk(pclk), .we(ap_mem_ack), .wd({ap_mem_we, ap_mem_addr, ap_mem_wdata}), .room(mreq_room),
	.rclk(clk), .rd(mreq_valid && (reset || mreq_rd)), .q(mreq_q), .q_valid(mreq_valid)
);

// ------------------------------------------------ motion memory read data
// a 64-bit word gives two 32-bit words; words 0..34 of each 36-word record
wire [63:0] mret_q;
wire        mret_valid;
reg         mr_hi = 0;                  // the high half is next
reg  [5:0]  mr_n = 0;                   // 32-bit words of this record given
wire        mr_pop = mret_valid && (prst || mr_hi);
mc_afifo #(.W(64), .L2(5), .ROOM(3)) fmret
(
	.wclk(clk), .we(mret_we), .wd(mret_wd), .room(),
	.rclk(pclk), .rd(mr_pop), .q(mret_q), .q_valid(mret_valid)
);
always @(posedge pclk) begin
	ap_rvalid <= 0;
	if (prst) begin
		mr_hi <= 0;
		mr_n  <= 0;
	end
	else if (mret_valid) begin
		ap_rdata  <= mr_hi ? mret_q[63:32] : mret_q[31:0];
		ap_rvalid <= mr_n != 6'd35;
		mr_hi     <= !mr_hi;
		mr_n      <= mr_n == 6'd35 ? 6'd0 : mr_n + 1'd1;
	end
end

// ------------------------------------------------ slice errors, to clk
reg perr_t = 0;
always @(posedge pclk) if (ap_err && !prst) perr_t <= !perr_t;
reg [2:0] err_s = 0;
always @(posedge clk) err_s <= {err_s[1:0], perr_t};
assign err_pulse = err_s[2] != err_s[1];

endmodule
