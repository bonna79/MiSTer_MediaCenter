// A FIFO between two clocks (mc_acc's own clock and the bus clock): the
// words are kept in a block RAM, the two pointers cross in Gray code.
// The read side shows its first word (q, q_valid) and takes the next one
// when rd is high.
// room: at least ROOM free places, counted a clock late, so the writer may
// write on the clock it sees room and the next one.
module mc_afifo #(parameter W = 64, parameter L2 = 4, parameter ROOM = 3)
(
	input              wclk,
	input              we,
	input      [W-1:0] wd,
	output reg         room = 1,

	input              rclk,
	input              rd,
	output reg [W-1:0] q,
	output reg         q_valid = 0
);

(* ramstyle = "M10K, no_rw_check" *) reg [W-1:0] mem[1 << L2];

function [L2:0] gray(input [L2:0] b);
	gray = b ^ (b >> 1);
endfunction

function [L2:0] bin(input [L2:0] g);
	integer i;
	bin[L2] = g[L2];
	for (i = L2 - 1; i >= 0; i = i - 1) bin[i] = bin[i + 1] ^ g[i];
endfunction

// ------------------------------------------------ write side
reg [L2:0] wb = 0, wg = 0;
reg [L2:0] rg_s1 = 0, rg_s2 = 0;
always @(posedge wclk) if (we) mem[wb[L2-1:0]] <= wd;
always @(posedge wclk) begin
	reg [L2:0] n;
	rg_s1 <= rg;
	rg_s2 <= rg_s1;
	n = wb + we;
	if (we) begin
		wb <= n;
		wg <= gray(n);
	end
	room <= (n - bin(rg_s2)) <= (1 << L2) - ROOM;
end

// ------------------------------------------------ read side
reg [L2:0] rb = 0, rg = 0;
reg [L2:0] wg_s1 = 0, wg_s2 = 0;
wire       take = (!q_valid || rd) && gray(rb) != wg_s2;
always @(posedge rclk) if (take) q <= mem[rb[L2-1:0]];
always @(posedge rclk) begin
	reg [L2:0] n;
	wg_s1 <= wg;
	wg_s2 <= wg_s1;
	if (rd) q_valid <= 0;
	if (take) begin
		n = rb + 1'd1;
		rb <= n;
		rg <= gray(n);
		q_valid <= 1;
	end
end

endmodule
