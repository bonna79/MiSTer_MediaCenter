// Reconstructed macroblock of acc_engine, in block RAM: written 4 (or 2)
// pixels of a row at a time, read 8 pixels at a time (one memory word) for
// the write back. Two banks by x bit 2, words of 4 pixels; word address:
// luma {y, x[3]} 0..31, U 32 + y, V 40 + y. The write back word k (0..47)
// is address k in both banks. Read data comes on the next clock.
module acc_recon
(
	input             clk,

	input             w_en,
	input       [1:0] w_plane,
	input       [3:0] w_x,       // multiple of 4, or of 2 when w_n = 2 (chroma)
	input       [3:0] w_y,
	input       [2:0] w_n,       // 4 or 2 pixels
	input      [31:0] w_px,

	input       [5:0] r_addr,
	output     [63:0] r_word
);

wire [5:0] wa = w_plane == 0 ? {1'b0, w_y, w_x[3]} : (w_plane == 1 ? {3'b100, w_y[2:0]} : {3'b101, w_y[2:0]});
wire       two_hi = w_n == 2 && w_x[1];
wire [31:0] wd = two_hi ? {w_px[15:0], 16'd0} : w_px;
wire [3:0] wbe = w_n == 2 ? (w_x[1] ? 4'b1100 : 4'b0011) : 4'b1111;

genvar gb;
generate
	for (gb = 0; gb < 2; gb = gb + 1) begin : bank
		reg [31:0] mem[64];
		reg [31:0] q;
		wire we = w_en && w_x[2] == gb;
		always @(posedge clk) begin
			if (we && wbe[0]) mem[wa][7:0]   <= wd[7:0];
			if (we && wbe[1]) mem[wa][15:8]  <= wd[15:8];
			if (we && wbe[2]) mem[wa][23:16] <= wd[23:16];
			if (we && wbe[3]) mem[wa][31:24] <= wd[31:24];
			q <= mem[r_addr];
		end
		assign r_word[gb * 32 +: 32] = q;
	end
endgenerate

endmodule
