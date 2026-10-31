// CABAC decoder for the 4.0 parser, step 1 (measure only): the arithmetic
// decoding engine of H.264 9.3.3.2, the 1024 contexts with their
// initialisation (9.3.1.1) and the residual block (7.3.5.3.3: significance
// map, coeff_abs_level_minus1, signs) for 4:2:0 frame macroblocks, ctxBlockCat
// 0-5. The other syntax elements come in as single bins (op DEC with the
// context, BYP, TERM): the macroblock layer is step 2.
//
// Bit stream: 32-bit words, first byte in bits 31:24, of the slice data from
// the byte after cabac_alignment_one_bit (the RBSP: emulation prevention
// bytes removed). The words of a slice are taken after its INIT command.
//
// Commands (cmd_op):
//   0 INIT  cmd_arg[1:0] slice type (0 P, 1 B, 2 I), [3:2] cabac_init_idc,
//           [9:4] SliceQPY: contexts from the tables, then the engine reads
//           9 bits (codIRange = 510)
//   1 DEC   cmd_arg[9:0] context: one decision bin
//   2 BYP   one bypass bin
//   3 TERM  one terminate bin
//   4 RES   cmd_arg[2:0] ctxBlockCat: a residual block whose
//           coded_block_flag was 1
//   5 PCM   after the I_PCM mb_type: skips to the byte boundary and the 384
//           sample bytes, then the engine reads 9 bits again
// Results (out_*):
//   out_kind 0 bin (out_val[0]); 1 coefficient (out_pos, out_val signed,
//   last coefficient first); 2 end of block (out_pos = count);
//   3 I_PCM start (out_val = byte offset in the slice data)
// Timing: a decision bin takes 2 clocks (context read, then the decision),
// a bypass bin 1, the INIT 1024 + a few.
module cab_dec
(
	input             clk,
	input             reset,

	input             cmd_valid,
	output            cmd_ready,
	input       [2:0] cmd_op,
	input      [11:0] cmd_arg,

	input             in_valid,
	output            in_ready,
	input      [31:0] in_data,

	output reg        out_valid,
	output reg  [1:0] out_kind,
	output reg  [6:0] out_pos,
	output reg [31:0] out_val
);

// ---------------------------------------------------------------- tables
reg [15:0] init_rom[0:4095];      // m << 8 | n, address set * 1024 + context
reg  [7:0] rlps_rom[0:255];       // rangeTabLPS, address pStateIdx * 4 + qCodIRangeIdx
reg  [5:0] tlps_rom[0:63];        // transIdxLPS
initial begin
	$readmemh("cab_init.hex", init_rom);
	$readmemh("cab_rlps.hex", rlps_rom);
	$readmemh("cab_tlps.hex", tlps_rom);
end

// significant_coeff_flag and last ctxIdxInc for 8x8 frame blocks (table 9-43)
function [3:0] sig8(input [5:0] i);
	case (i)
	6'd0: sig8 = 4'd0; 6'd1: sig8 = 4'd1; 6'd2: sig8 = 4'd2; 6'd3: sig8 = 4'd3; 6'd4: sig8 = 4'd4; 6'd5: sig8 = 4'd5; 6'd6: sig8 = 4'd5; 6'd7: sig8 = 4'd4;
	6'd8: sig8 = 4'd4; 6'd9: sig8 = 4'd3; 6'd10: sig8 = 4'd3; 6'd11: sig8 = 4'd4; 6'd12: sig8 = 4'd4; 6'd13: sig8 = 4'd4; 6'd14: sig8 = 4'd5; 6'd15: sig8 = 4'd5;
	6'd16: sig8 = 4'd4; 6'd17: sig8 = 4'd4; 6'd18: sig8 = 4'd4; 6'd19: sig8 = 4'd4; 6'd20: sig8 = 4'd3; 6'd21: sig8 = 4'd3; 6'd22: sig8 = 4'd6; 6'd23: sig8 = 4'd7;
	6'd24: sig8 = 4'd7; 6'd25: sig8 = 4'd7; 6'd26: sig8 = 4'd8; 6'd27: sig8 = 4'd9; 6'd28: sig8 = 4'd10; 6'd29: sig8 = 4'd9; 6'd30: sig8 = 4'd8; 6'd31: sig8 = 4'd7;
	6'd32: sig8 = 4'd7; 6'd33: sig8 = 4'd6; 6'd34: sig8 = 4'd11; 6'd35: sig8 = 4'd12; 6'd36: sig8 = 4'd13; 6'd37: sig8 = 4'd11; 6'd38: sig8 = 4'd6; 6'd39: sig8 = 4'd7;
	6'd40: sig8 = 4'd8; 6'd41: sig8 = 4'd9; 6'd42: sig8 = 4'd14; 6'd43: sig8 = 4'd10; 6'd44: sig8 = 4'd9; 6'd45: sig8 = 4'd8; 6'd46: sig8 = 4'd6; 6'd47: sig8 = 4'd11;
	6'd48: sig8 = 4'd12; 6'd49: sig8 = 4'd13; 6'd50: sig8 = 4'd11; 6'd51: sig8 = 4'd6; 6'd52: sig8 = 4'd9; 6'd53: sig8 = 4'd14; 6'd54: sig8 = 4'd10; 6'd55: sig8 = 4'd9;
	6'd56: sig8 = 4'd11; 6'd57: sig8 = 4'd12; 6'd58: sig8 = 4'd13; 6'd59: sig8 = 4'd11; 6'd60: sig8 = 4'd14; 6'd61: sig8 = 4'd10; 6'd62: sig8 = 4'd12; 6'd63: sig8 = 4'd0;
	default: sig8 = 4'd0;
	endcase
endfunction

function [3:0] last8(input [5:0] i);
	begin
		last8 = i == 0 ? 4'd0 : i < 16 ? 4'd1 : i < 32 ? 4'd2 : i < 40 ? 4'd3 : i < 48 ? 4'd4 :
		        i < 52 ? 4'd5 : i < 56 ? 4'd6 : i < 60 ? 4'd7 : 4'd8;
	end
endfunction

localparam S_IDLE = 0, S_INIT = 1, S_INIT2 = 2, S_LOAD = 3, S_DREAD = 4, S_DEXE = 5, S_BYP = 6, S_TERM = 7,
           S_PCM = 8, S_RSIG = 9, S_RPOP = 12, S_SKIP = 13;
reg  [3:0] st;
reg  [9:0] dctx;                  // context of the decision being decoded

// ---------------------------------------------------------------- bit buffer
reg [63:0] bb;                    // next bits, first in bit 63
reg  [6:0] bcnt;                  // valid bits in bb
reg [31:0] bitpos;                // bits taken since the slice start
// the buffer is updated in the control block below, with the bits taken
assign in_ready = !reset && bcnt <= 7'd32 && st != S_INIT;

// ---------------------------------------------------------------- contexts
reg  [6:0] ctx_ram[0:1023];       // pStateIdx << 1 | valMPS
reg  [6:0] ctx_q;
reg        ctx_we;
reg  [9:0] ctx_wa;
reg  [6:0] ctx_wd;
// the write comes a clock after the decision, so a read of the same context
// in that clock takes the new state
reg  [6:0] ctx_rd;
always @(posedge clk) begin
	if (ctx_we) ctx_ram[ctx_wa] <= ctx_wd;
	ctx_rd <= ctx_ram[dctx];
end
reg        ctx_fw;
reg  [6:0] ctx_fwd;
always @(posedge clk) begin
	ctx_fw <= ctx_we && ctx_wa == dctx;
	ctx_fwd <= ctx_wd;
end
always @(*) ctx_q = ctx_fw ? ctx_fwd : ctx_rd;

// initialisation, 2 stages: ROM, then the state
reg [11:0] irom_a;
reg [15:0] irom_q;
always @(posedge clk) irom_q <= init_rom[irom_a];

function [6:0] pre_state(input [15:0] mn, input [5:0] qp);
	reg signed [7:0] m, n;
	reg signed [15:0] t;
	reg signed [15:0] p;
	begin
		m = mn[15:8];
		n = mn[7:0];
		t = m * $signed({2'b00, qp});
		p = (t >>> 4) + n;
		if (p < 1) p = 1;
		if (p > 126) p = 126;
		pre_state = p <= 63 ? {6'(63 - p), 1'b0} : {6'(p - 64), 1'b1};
	end
endfunction

// ---------------------------------------------------------------- engine
reg  [8:0] range, offset;

// renormalisation shift: leading zeros of a 9-bit range (it is >= 2)
function [2:0] nshift(input [8:0] r);
	nshift = r[8] ? 3'd0 : r[7] ? 3'd1 : r[6] ? 3'd2 : r[5] ? 3'd3 : r[4] ? 3'd4 : r[3] ? 3'd5 : r[2] ? 3'd6 : 3'd7;
endfunction

// ---------------------------------------------------------------- control
reg  [3:0] ret;                   // what the decision bin belongs to
localparam R_CMD = 0, R_SIG = 1, R_LAST = 2, R_ABS0 = 3, R_ABSN = 4, R_EGP = 5, R_EGS = 6, R_SIGN = 7;

reg  [1:0] s_type, s_idc;
reg  [5:0] s_qp;
reg [10:0] icnt;
reg  [2:0] cat;
reg  [6:0] ri;                    // coefficient index in the map
reg  [6:0] rmax;                  // maxNumCoeff
reg  [5:0] lifo[0:63];            // significant positions
reg  [6:0] rn;                    // their count
reg  [6:0] rk;                    // levels left
reg  [5:0] rpos;
reg  [2:0] eq1, gt1;
reg  [3:0] pre;
reg  [4:0] k0;
reg [19:0] suf, absv;
reg [31:0] skip;                  // bits to skip (I_PCM)

assign cmd_ready = st == S_IDLE && !reset;

wire [9:0] sigbase = cat == 3'd5 ? 10'd402 : 10'd105 + (cat == 3'd1 ? 10'd15 : cat == 3'd2 ? 10'd29 : cat == 3'd3 ? 10'd44 : cat == 3'd4 ? 10'd47 : 10'd0);
wire [9:0] lastbase = cat == 3'd5 ? 10'd417 : 10'd166 + (cat == 3'd1 ? 10'd15 : cat == 3'd2 ? 10'd29 : cat == 3'd3 ? 10'd44 : cat == 3'd4 ? 10'd47 : 10'd0);
wire [9:0] absbase = cat == 3'd5 ? 10'd426 : 10'd227 + (cat == 3'd1 ? 10'd10 : cat == 3'd2 ? 10'd20 : cat == 3'd3 ? 10'd30 : cat == 3'd4 ? 10'd39 : 10'd0);
wire [3:0] sinc = cat == 3'd5 ? sig8(ri[5:0]) : cat == 3'd3 ? (ri > 7'd2 ? 4'd2 : ri[3:0]) : ri[3:0];
wire [3:0] linc = cat == 3'd5 ? last8(ri[5:0]) : cat == 3'd3 ? (ri > 7'd2 ? 4'd2 : ri[3:0]) : ri[3:0];
wire [2:0] inc1 = gt1 != 3'd0 ? 3'd0 : (eq1 >= 3'd3 ? 3'd4 : eq1 + 3'd1);
wire [2:0] gmax = cat == 3'd3 ? 3'd3 : 3'd4;
wire [3:0] inc2 = 4'd5 + {1'b0, gt1 > gmax ? gmax : gt1};

// the decision: combinational on the context read (S_DEXE)
wire [5:0] d_p   = ctx_q[6:1];
wire       d_mps = ctx_q[0];
wire [7:0] d_rlps = rlps_rom[{d_p, range[7:6]}];
wire [8:0] d_rmps = range - {1'b0, d_rlps};
wire       d_lps = offset >= d_rmps;
wire       d_bin = d_lps ? !d_mps : d_mps;
wire [8:0] d_r   = d_lps ? {1'b0, d_rlps} : d_rmps;
wire [8:0] d_o   = d_lps ? offset - d_rmps : offset;
wire [2:0] d_sh  = nshift(d_r);
wire [6:0] d_ns  = d_lps ? {tlps_rom[d_p], (d_p == 6'd0) ? !d_mps : d_mps} : {(d_p == 6'd62 || d_p == 6'd63) ? d_p : d_p + 6'd1, d_mps};
wire       d_ok  = bcnt >= {4'd0, d_sh};

// bypass
wire [9:0] b_o   = {offset, bb[63]};
wire       b_bin = b_o >= {1'b0, range};
wire       b_ok  = bcnt != 7'd0;

// terminate
wire [8:0] t_r   = range - 9'd2;
wire       t_bin = offset >= t_r;
wire [2:0] t_sh  = t_bin ? 3'd0 : nshift(t_r);
wire       t_ok  = bcnt >= {4'd0, t_sh};

// next step after a bin of kind ret with value b: sets st, dctx and the residual state
task automatic after_bin(input b);
	begin
		case (ret)
		R_CMD: st <= S_IDLE;
		R_SIG: begin
			if (b) begin
				lifo[rn[5:0]] <= ri[5:0];
				rn <= rn + 7'd1;
				st <= S_DREAD;
				ret <= R_LAST;
				dctx <= lastbase + {6'd0, linc};
			end
			else begin
				ri <= ri + 7'd1;
				st <= S_RSIG;
			end
		end
		R_LAST: begin
			if (b) begin_levels(rn);
			else begin
				ri <= ri + 7'd1;
				st <= S_RSIG;
			end
		end
		R_ABS0: begin
			if (!b) begin
				absv <= 20'd1;
				st <= S_BYP;
				ret <= R_SIGN;
			end
			else begin
				pre <= 4'd1;
				st <= S_DREAD;
				ret <= R_ABSN;
				dctx <= absbase + {6'd0, inc2};
			end
		end
		R_ABSN: begin
			if (b && pre != 4'd13) begin
				pre <= pre + 4'd1;
				st <= S_DREAD;
			end
			else if (b) begin
				// prefix 14: Exp-Golomb suffix in bypass bins
				k0 <= 5'd0;
				suf <= 20'd0;
				st <= S_BYP;
				ret <= R_EGP;
			end
			else begin
				absv <= {16'd0, pre} + 20'd1;
				st <= S_BYP;
				ret <= R_SIGN;
			end
		end
		default: st <= S_IDLE;
		endcase
	end
endtask


task automatic begin_levels(input [6:0] n);
	begin
		rk <= n;
		eq1 <= 3'd0;
		gt1 <= 3'd0;
		st <= S_RPOP;
	end
endtask

reg [6:0] ocnt;
`ifdef CABDBG
reg dbg_on = 1;
`endif

always @(posedge clk) begin
	reg  [6:0] take;              // bits taken this clock
	reg        flush;
	take = 7'd0;
	flush = 1'b0;
	out_valid <= 1'b0;
	ctx_we <= 1'b0;
	if (reset) begin
		st <= S_IDLE;
		range <= 9'd510;
		offset <= 9'd0;
		bitpos <= 32'd0;
		bb <= 64'd0;
		bcnt <= 7'd0;
	end
	else begin
		case (st)
		S_IDLE: if (cmd_valid) begin
			case (cmd_op)
			3'd0: begin
				s_type <= cmd_arg[1:0];
				s_idc <= cmd_arg[3:2];
				s_qp <= cmd_arg[9:4] > 6'd51 ? 6'd51 : cmd_arg[9:4];
				icnt <= 11'd0;
				flush = 1'b1;
				st <= S_INIT;
			end
			3'd1: begin
				dctx <= cmd_arg[9:0];
				ret <= R_CMD;
				st <= S_DREAD;
			end
			3'd2: begin
				ret <= R_CMD;
				st <= S_BYP;
			end
			3'd3: st <= S_TERM;
			3'd4: begin
				cat <= cmd_arg[2:0];
				rmax <= cmd_arg[2:0] == 3'd5 ? 7'd64 : cmd_arg[2:0] == 3'd3 ? 7'd4 : (cmd_arg[2:0] == 3'd1 || cmd_arg[2:0] == 3'd4) ? 7'd15 : 7'd16;
				ri <= 7'd0;
				rn <= 7'd0;
				ocnt <= 7'd0;
				st <= S_RSIG;
			end
			3'd5: st <= S_PCM;
			default: ;
			endcase
		end

		// context initialisation: ROM address, ROM data, write
		S_INIT: begin
			irom_a <= {s_type == 2'd2 ? 2'd0 : s_idc + 2'd1, 10'd0};
			st <= S_INIT2;
		end
		S_INIT2: begin
			icnt <= icnt + 11'd1;
			irom_a[9:0] <= irom_a[9:0] + 10'd1;
			if (icnt != 11'd0) begin
				ctx_we <= 1'b1;
				ctx_wa <= icnt[9:0] - 10'd1;
				ctx_wd <= pre_state(irom_q, s_qp);
			end
			if (icnt == 11'd1024) st <= S_LOAD;
		end
		// engine start: 9 bits
		S_LOAD: if (bcnt >= 7'd9) begin
			range <= 9'd510;
			offset <= bb[63:55];
			take = 7'd9;
			st <= S_IDLE;
		end

		S_DREAD: st <= S_DEXE;       // the context RAM reads dctx
		S_DEXE: if (d_ok) begin
`ifdef CABDBG
			if (dbg_on) $display("D ctx %0d st %0d bin %0d range %0d off %0d ret %0d", dctx, ctx_q, d_bin, range, offset, ret);
`endif
			range <= d_r << d_sh;
			offset <= (d_o << d_sh) | ({2'd0, bb[63:57]} >> (3'd7 - d_sh));
			take = {4'd0, d_sh};
			ctx_we <= 1'b1;
			ctx_wa <= dctx;
			ctx_wd <= d_ns;
			if (ret == R_CMD) begin
				out_valid <= 1'b1;
				out_kind <= 2'd0;
				out_val <= {31'd0, d_bin};
			end
			after_bin(d_bin);
		end

		S_BYP: if (b_ok) begin
			offset <= b_bin ? 9'(b_o - {1'b0, range}) : b_o[8:0];
			take = 7'd1;
			case (ret)
			R_CMD: begin
				out_valid <= 1'b1;
				out_kind <= 2'd0;
				out_val <= {31'd0, b_bin};
				st <= S_IDLE;
			end
			R_EGP: begin
				if (b_bin) begin
					suf <= suf + (20'd1 << k0);
					k0 <= k0 + 5'd1;
				end
				else begin
					ret <= k0 == 5'd0 ? R_SIGN : R_EGS;
					absv <= 20'd15 + suf;
				end
			end
			R_EGS: begin
				absv <= absv + ({19'd0, b_bin} << (k0 - 5'd1));
				k0 <= k0 - 5'd1;
				if (k0 == 5'd1) ret <= R_SIGN;
			end
			R_SIGN: begin
				out_valid <= 1'b1;
				out_kind <= 2'd1;
				out_pos <= {1'b0, rpos};
				out_val <= b_bin ? -{12'd0, absv} : {12'd0, absv};
				ocnt <= ocnt + 7'd1;
				if (absv == 20'd1) eq1 <= eq1 == 3'd7 ? eq1 : eq1 + 3'd1;
				else gt1 <= gt1 == 3'd7 ? gt1 : gt1 + 3'd1;
				st <= S_RPOP;
			end
			default: st <= S_IDLE;
			endcase
		end

		S_TERM: if (t_ok) begin
			range <= t_bin ? t_r : t_r << t_sh;
			offset <= t_bin ? offset : (offset << t_sh) | ({2'd0, bb[63:57]} >> (3'd7 - t_sh));
			take = {4'd0, t_sh};
			out_valid <= 1'b1;
			out_kind <= 2'd0;
			out_val <= {31'd0, t_bin};
			st <= S_IDLE;
		end

		// I_PCM: the samples start at the byte boundary after the bits read
		S_PCM: begin
			out_valid <= 1'b1;
			out_kind <= 2'd3;
			out_val <= (bitpos + 32'd7) >> 3;
			skip <= {((bitpos + 32'd7) >> 3) + 32'd384, 3'd0} - bitpos;
			st <= S_SKIP;
		end
		S_SKIP: begin
			if (skip == 32'd0) begin
				ret <= R_CMD;
				st <= S_LOAD;
			end
			else if (bcnt != 7'd0) begin
				reg [6:0] n;
				n = bcnt > 7'd32 ? 7'd32 : bcnt;
				if ({25'd0, n} > skip) n = skip[6:0];
				take = n;
				skip <= skip - {25'd0, n};
			end
		end

		// residual: next significance bin (index ri)
		S_RSIG: begin
			if (ri == rmax - 7'd1) begin
				lifo[rn[5:0]] <= ri[5:0];
				rn <= rn + 7'd1;
				begin_levels(rn + 7'd1);
			end
			else begin
				dctx <= sigbase + {6'd0, sinc};
				ret <= R_SIG;
				st <= S_DREAD;
			end
		end
		// residual: next level, from the last significant coefficient
		S_RPOP: begin
			if (rk == 7'd0) begin
				out_valid <= 1'b1;
				out_kind <= 2'd2;
				out_pos <= ocnt;
				st <= S_IDLE;
			end
			else begin
				rpos <= lifo[rk[5:0] - 6'd1];
				rk <= rk - 7'd1;
				dctx <= absbase + {7'd0, inc1};
				ret <= R_ABS0;
				st <= S_DREAD;
			end
		end
		default: st <= S_IDLE;
		endcase

		// bit buffer: bits taken, then a new word
		if (flush) begin
			bb <= 64'd0;
			bcnt <= 7'd0;
			bitpos <= 32'd0;
		end
		else begin
			reg [63:0] b;
			reg  [6:0] c;
			b = bb << take;
			c = bcnt - take;
			if (in_valid && in_ready) begin
				b = b | ({in_data, 32'd0} >> c);
				c = c + 7'd32;
			end
			bb <= b;
			bcnt <= c;
			bitpos <= bitpos + {25'd0, take};
		end
	end
end

endmodule
