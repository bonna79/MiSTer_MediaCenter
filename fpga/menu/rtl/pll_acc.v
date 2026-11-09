// The accelerator's own clocks, from the 50 MHz input: 100 MHz for mc_acc
// (the engine), 85.7 MHz (600 / 7) for acc_parse (mc_parse).
// It is not tied to the core clock (108 MHz) and the video clock (27 MHz):
// mc_acc crosses to the bus clock in its FIFOs (mc_afifo).
`timescale 1ns/10ps
module pll_acc
(
	input  wire refclk,
	input  wire rst,
	output wire outclk_0,
	output wire outclk_1,
	output wire locked
);

altera_pll #(
	.fractional_vco_multiplier("false"),
	.reference_clock_frequency("50.0 MHz"),
	.operation_mode("direct"),
	.number_of_clocks(2),
	.output_clock_frequency0("100.000000 MHz"),
	.phase_shift0("0 ps"),
	.duty_cycle0(50),
	.output_clock_frequency1("85.714285 MHz"),
	.phase_shift1("0 ps"),
	.duty_cycle1(50),
	.pll_type("General"),
	.pll_subtype("General")
) altera_pll_i (
	.rst    (rst),
	.outclk ({outclk_1, outclk_0}),
	.locked (locked),
	.fboutclk ( ),
	.fbclk  (1'b0),
	.refclk (refclk)
);

endmodule
