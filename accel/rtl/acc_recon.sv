// Reconstructed macroblock of acc_engine, in block RAM: written 4 (or 2)
// pixels of a row at a time, read 8 pixels at a time (one memory word) for
// the write back. Two banks by x bit 2, words of 4 pixels; word address:
// luma {y, x[3]} 0..31, U 32 + y, V 40 + y. The write back word k (0..47)
// is address k in both banks. Read data comes on the next clock.
module acc_recon
(
	input             clk,

	input             w_en,
	input             w_buf,     // two buffers: one written, the other read
	input             r_buf,
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

// one RAM per bank and byte lane (block RAM without byte enables)
genvar gb, gl;
generate
	for (gb = 0; gb < 2; gb = gb + 1) begin : bank
		for (gl = 0; gl < 4; gl = gl + 1) begin : lane
			reg [7:0] mem[128];
			reg [7:0] q;
			wire we = w_en && w_x[2] == gb && wbe[gl];
			always @(posedge clk) begin
				if (we) mem[{w_buf, wa}] <= wd[gl * 8 +: 8];
				q <= mem[{r_buf, r_addr}];
			end
			assign r_word[gb * 32 + gl * 8 +: 8] = q;
		end
	end
endgenerate

endmodule
