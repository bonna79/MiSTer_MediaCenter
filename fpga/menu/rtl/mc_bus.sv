// The DDR port shared by mc_out (the CRT output, always first), mc_zc (the
// zero-copy) and mc_acc (the FPGA accelerator).
//
// The bus goes to mc_zc or mc_acc only while mc_out has nothing to fetch,
// and back to mc_out as soon as it asks for a line and the transfers under
// way are over. mc_zc and mc_acc take turns when both have work: a turn ends
// after ZT (mc_zc) or AT (mc_acc) transfers. Read data go to the one that
// asked (the bus changes hands only when no read is under way).
module mc_bus #(parameter ZT = 16, parameter AT = 256)
(
	input         clk,
	input         own,       // the bus is the frame buffer's

	input  [28:0] m_addr,
	input   [7:0] m_burst,
	input         m_rd,
	input         m_idle,
	output        m_grant,
	output        m_ready,

	input  [28:0] z_addr,
	input   [7:0] z_burst,
	input         z_rd,
	input         z_we,
	input  [63:0] z_din,
	input         z_want,
	input         z_idle,
	output        z_grant,
	output        z_ready,

	input  [28:0] a_addr,
	input   [7:0] a_burst,
	input         a_rd,
	input         a_we,
	input  [63:0] a_din,
	input         a_want,
	input         a_idle,
	output        a_grant,
	output        a_ready,

	input         busy,
	input         ready,
	output [28:0] addr,
	output  [7:0] burst,
	output        rd,
	output        we,
	output [63:0] din,
	output        idle
);

reg       zsel = 0, asel = 0;
reg       last_a = 0;          // the last turn was mc_acc's
reg [8:0] turns = 0;           // transfers in this turn

wire z_full = turns >= ZT && a_want;
wire a_full = turns >= AT && z_want;

always @(posedge clk) begin
	if (!zsel && !asel) begin
		turns <= 0;
		if (own && m_idle) begin
			if (z_want && (!a_want || last_a)) zsel <= 1;
			else if (a_want) asel <= 1;
		end
	end
	else if (zsel) begin
		if ((z_rd || z_we) && !busy) turns <= turns + 1'd1;
		if (z_idle && (!m_idle || !z_want || !own || z_full)) begin
			zsel   <= 0;
			last_a <= 0;
		end
	end
	else begin
		if ((a_rd || a_we) && !busy) turns <= turns + 1'd1;
		if (a_idle && (!m_idle || !a_want || !own || a_full)) begin
			asel   <= 0;
			last_a <= 1;
		end
	end
end

assign m_grant = own & ~zsel & ~asel;
assign z_grant = own & zsel & m_idle & ~z_full;
assign a_grant = own & asel & m_idle & ~a_full;
assign m_ready = ready & ~zsel & ~asel;
assign z_ready = ready & zsel;
assign a_ready = ready & asel;
assign addr    = zsel ? z_addr  : asel ? a_addr  : m_addr;
assign burst   = zsel ? z_burst : asel ? a_burst : m_burst;
assign rd      = zsel ? z_rd    : asel ? a_rd    : m_rd;
assign we      = (zsel & z_we) | (asel & a_we);
assign din     = asel ? a_din : z_din;
assign idle    = m_idle & ~zsel & ~asel;

endmodule
