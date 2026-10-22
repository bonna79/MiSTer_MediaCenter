// H.264 inverse transform, 4x4 and 8x8 (8.5.12 / 8.5.13), for the FPGA
// reconstruction engine. Coefficients are already dequantised by the ARM.
//
// Use: pulse clr, write the non-zero coefficients (pos = row * n + col),
// pulse go. Rows are transformed first (one per clock), then columns: each
// column comes out on res (rows 0..n-1, (x + 32) >> 6 done) with res_valid
// and res_col. busy is high from go until the last column.
module acc_idct
(
	input             clk,
	input             clr,
	input             t8,      // 1: 8x8 block, 0: 4x4
	input             wr,
	input       [5:0] wr_pos,
	input signed [15:0] wr_val,
	input             go,
	output reg        busy,
	output reg        res_valid,
	output reg  [2:0] res_col,
	output reg signed [15:0] res[8]
);

// widths: coefficients 16 bits, after both passes at most 22 bits
reg signed [15:0] c[64];
reg signed [21:0] t[64];  // after the row pass, t[row * 8 + col]
reg        big;
reg        cols;
reg  [2:0] k;

// one 1D transform per clock: row k of c, or column k of t
reg signed [21:0] d0, d1, d2, d3, d4, d5, d6, d7;
reg signed [21:0] a0, a1, a2, a3, a4, a5, a6, a7, b0, b1, b2, b3, b4, b5, b6, b7;
reg signed [21:0] o0, o1, o2, o3, o4, o5, o6, o7;
always @* begin
	if (cols) begin
		d0 = t[{3'd0, k}]; d1 = t[{3'd1, k}]; d2 = t[{3'd2, k}]; d3 = t[{3'd3, k}];
		d4 = t[{3'd4, k}]; d5 = t[{3'd5, k}]; d6 = t[{3'd6, k}]; d7 = t[{3'd7, k}];
	end
	else if (big) begin
		d0 = c[{k, 3'd0}]; d1 = c[{k, 3'd1}]; d2 = c[{k, 3'd2}]; d3 = c[{k, 3'd3}];
		d4 = c[{k, 3'd4}]; d5 = c[{k, 3'd5}]; d6 = c[{k, 3'd6}]; d7 = c[{k, 3'd7}];
	end
	else begin
		d0 = c[{k[1:0], 2'd0}]; d1 = c[{k[1:0], 2'd1}]; d2 = c[{k[1:0], 2'd2}]; d3 = c[{k[1:0], 2'd3}];
		d4 = 0; d5 = 0; d6 = 0; d7 = 0;
	end
	if (big) begin
		a0 = d0 + d4;
		a4 = d0 - d4;
		a2 = (d2 >>> 1) - d6;
		a6 = d2 + (d6 >>> 1);
		b0 = a0 + a6;
		b2 = a4 + a2;
		b4 = a4 - a2;
		b6 = a0 - a6;
		a1 = -d3 + d5 - d7 - (d7 >>> 1);
		a3 = d1 + d7 - d3 - (d3 >>> 1);
		a5 = -d1 + d7 + d5 + (d5 >>> 1);
		a7 = d3 + d5 + d1 + (d1 >>> 1);
		b1 = a1 + (a7 >>> 2);
		b7 = a7 - (a1 >>> 2);
		b3 = a3 + (a5 >>> 2);
		b5 = (a3 >>> 2) - a5;
		o0 = b0 + b7; o1 = b2 + b5; o2 = b4 + b3; o3 = b6 + b1;
		o4 = b6 - b1; o5 = b4 - b3; o6 = b2 - b5; o7 = b0 - b7;
	end
	else begin
		a0 = d0 + d2;
		a1 = d0 - d2;
		a2 = (d1 >>> 1) - d3;
		a3 = d1 + (d3 >>> 1);
		o0 = a0 + a3; o1 = a1 + a2; o2 = a1 - a2; o3 = a0 - a3;
		o4 = 0; o5 = 0; o6 = 0; o7 = 0;
		{a4, a5, a6, a7, b0, b1, b2, b3, b4, b5, b6, b7} = 0;
	end
end

wire [2:0] last = big ? 3'd7 : 3'd3;

always @(posedge clk) begin
	res_valid <= 0;
	if (clr) for (int i = 0; i < 64; i++) c[i] <= 0;
	if (wr) c[wr_pos] <= wr_val; // clr and wr together: clear, then write

	if (go) begin
		busy <= 1;
		big <= t8;
		cols <= 0;
		k <= 0;
	end
	else if (busy) begin
		if (!cols) begin
			t[{k, 3'd0}] <= o0; t[{k, 3'd1}] <= o1; t[{k, 3'd2}] <= o2; t[{k, 3'd3}] <= o3;
			t[{k, 3'd4}] <= o4; t[{k, 3'd5}] <= o5; t[{k, 3'd6}] <= o6; t[{k, 3'd7}] <= o7;
			if (k == last) begin
				cols <= 1;
				k <= 0;
			end
			else k <= k + 1'd1;
		end
		else begin
			res_valid <= 1;
			res_col <= k;
			res[0] <= 16'((o0 + 22'sd32) >>> 6); res[1] <= 16'((o1 + 22'sd32) >>> 6);
			res[2] <= 16'((o2 + 22'sd32) >>> 6); res[3] <= 16'((o3 + 22'sd32) >>> 6);
			res[4] <= 16'((o4 + 22'sd32) >>> 6); res[5] <= 16'((o5 + 22'sd32) >>> 6);
			res[6] <= 16'((o6 + 22'sd32) >>> 6); res[7] <= 16'((o7 + 22'sd32) >>> 6);
			if (k == last) busy <= 0;
			else k <= k + 1'd1;
		end
	end
end

endmodule
