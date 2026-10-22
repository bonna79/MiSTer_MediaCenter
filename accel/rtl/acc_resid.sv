// Residual store of one macroblock for acc_engine, in block RAM.
//
// Written by the inverse transform one column at a time (4 or 8 values of
// the same x), read by the reconstruction one row group at a time (4 values
// of the same y). To do both in one clock the values sit in 8 banks chosen
// by (x + y) mod 8: a column of 8 and a row of 4 always fall in different
// banks. Values are kept on 10 bits (saturated): since prediction + residual
// is clipped to 0..255, any residual beyond -256..255 gives the same pixel.
//
// Blocks never written (no coefficients) read as zero: a flag per 4x4 block,
// cleared with clr at the start of each macroblock.
//
// Two buffers: the transform fills one macroblock (w_buf) while the
// reconstruction reads the previous one (r_buf). An I_PCM macroblock is
// written here too, a row of 4 pixels at a time (p_*), as residual of a
// zero prediction.
module acc_resid
(
	input             clk,
	input             clr,       // clears the flags of buffer w_buf
	input             w_buf,
	input             r_buf,

	// one column out of the transform: block (0-15 luma 4x4 or 8x8 * 4,
	// 16-19 U, 20-23 V), 8x8 or not, column, values of rows 0..7
	input             w_en,
	input       [4:0] w_blk,
	input             w_big,
	input       [2:0] w_col,
	input     [127:0] w_vals,

	// I_PCM: 4 pixels of a row
	input             p_en,
	input       [1:0] p_plane,
	input       [3:0] p_x,
	input       [3:0] p_y,
	input      [31:0] p_px,

	// row group: plane (0 Y, 1 U, 2 V), first x, y; values of x .. x+3 come
	// out on the next clock
	input       [1:0] r_plane,
	input       [3:0] r_x,
	input       [3:0] r_y,
	output reg [39:0] r_vals
);

integer k, j;

// ------------------------------------------------------------------ write

reg  [3:0] wx, wy0;         // x and first row of the column
reg  [1:0] wpl;
reg  [3:0] wn;
always @* begin
	if (w_blk < 16 && w_big) begin
		wx  = {w_blk[2], w_col};
		wy0 = {w_blk[3], 3'd0};
		wpl = 0;
		wn  = 8;
	end
	else if (w_blk < 16) begin
		wx  = {w_blk[2], w_blk[0], w_col[1:0]};
		wy0 = {w_blk[3], w_blk[1], 2'd0};
		wpl = 0;
		wn  = 4;
	end
	else begin
		wx  = {1'b0, w_blk[0], w_col[1:0]};
		wy0 = {1'b0, w_blk[1], 2'd0};
		wpl = w_blk < 20 ? 2'd1 : 2'd2;
		wn  = 4;
	end
end

// address inside a bank: luma {y, x[3]} 0..31, U 32 + y, V 40 + y
function [5:0] baddr(input [1:0] pl, input [3:0] x, input [3:0] y);
	baddr = pl == 0 ? {1'b0, y, x[3]} : (pl == 1 ? {3'b100, y[2:0]} : {3'b101, y[2:0]});
endfunction

function [9:0] sat10(input signed [15:0] v);
	sat10 = v < -512 ? 10'h200 : (v > 511 ? 10'h1FF : v[9:0]);
endfunction

reg        bwe[8];
reg  [6:0] bwa[8];
reg  [9:0] bwd[8];
reg  [3:0] wi;
reg  [3:0] wy;
always @* begin
	for (k = 0; k < 8; k = k + 1) begin
		if (p_en) begin
			wi = 4'(3'(k - p_x - p_y));       // pixel of the row landing in bank k
			bwe[k] = wi < 4;
			bwa[k] = {w_buf, baddr(p_plane, p_x + wi, p_y)};
			bwd[k] = {2'b00, p_px[wi[1:0] * 8 +: 8]};
		end
		else begin
			wi = 4'(3'(k - wx - wy0));        // row of the column landing in bank k
			wy = wy0 + wi;
			bwe[k] = w_en && wi < wn;
			bwa[k] = {w_buf, baddr(wpl, wx, wy)};
			bwd[k] = sat10(w_vals[wi[2:0] * 16 +: 16]);
		end
	end
end

// ------------------------------------------------------------------- read

reg  [6:0] bra[8];
reg  [3:0] rj;
always @* begin
	for (k = 0; k < 8; k = k + 1) begin
		rj = 4'(3'(k - r_x - r_y));          // pixel of the group in bank k
		bra[k] = {r_buf, baddr(r_plane, r_x + rj, r_y)};
	end
end

wire [79:0] bq;
genvar gb;
generate
	for (gb = 0; gb < 8; gb = gb + 1) begin : bank
		acc_ram #(.W(10), .A(7)) ram
		(
			.clk(clk), .we(bwe[gb]), .wa(bwa[gb]), .wd(bwd[gb]), .ra(bra[gb]), .q(bq[gb * 10 +: 10])
		);
	end
endgenerate

// flags of the 4x4 blocks written, per buffer
reg [23:0] coded[2];
always @(posedge clk) begin
	if (clr) coded[w_buf] <= 0;
	else if (p_en) coded[w_buf] <= 24'hFFFFFF;
	else if (w_en) begin
		if (w_blk < 16 && w_big) coded[w_buf][{w_blk[3:2], 2'b00} +: 4] <= 4'hF;
		else coded[w_buf][w_blk] <= 1;
	end
end

// output: the group's x and y of the read, then pick the banks
reg  [1:0] q_pl;
reg  [3:0] q_x, q_y;
reg [23:0] q_coded;
always @(posedge clk) begin
	q_coded <= coded[r_buf];
	q_pl <= r_plane;
	q_x <= r_x;
	q_y <= r_y;
end

reg  [4:0] qb;
reg  [3:0] qx;
always @* begin
	for (j = 0; j < 4; j = j + 1) begin
		qx = q_x + 4'(j);
		qb = q_pl == 0 ? {1'b0, q_y[3], qx[3], q_y[2], qx[2]} : (q_pl == 1 ? 5'd16 : 5'd20) + {q_y[2], qx[2]};
		r_vals[j * 10 +: 10] = q_coded[qb] ? bq[3'(qx + q_y) * 10 +: 10] : 10'd0;
	end
end

endmodule
