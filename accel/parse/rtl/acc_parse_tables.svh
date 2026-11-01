// Tables of acc_parse (generated from FFmpeg's, see pmodel.c): scan8, transposed
// zigzag scans, macroblock type tables (type | partition count << 16; intra:
// type | pred mode << 3 | cbp << 7), chroma QP.

function automatic [6:0] f_scan8(input [5:0] i);
	case (i)
	6'd0: f_scan8 = 7'd12;
	6'd1: f_scan8 = 7'd13;
	6'd2: f_scan8 = 7'd20;
	6'd3: f_scan8 = 7'd21;
	6'd4: f_scan8 = 7'd14;
	6'd5: f_scan8 = 7'd15;
	6'd6: f_scan8 = 7'd22;
	6'd7: f_scan8 = 7'd23;
	6'd8: f_scan8 = 7'd28;
	6'd9: f_scan8 = 7'd29;
	6'd10: f_scan8 = 7'd36;
	6'd11: f_scan8 = 7'd37;
	6'd12: f_scan8 = 7'd30;
	6'd13: f_scan8 = 7'd31;
	6'd14: f_scan8 = 7'd38;
	6'd15: f_scan8 = 7'd39;
	6'd16: f_scan8 = 7'd52;
	6'd17: f_scan8 = 7'd53;
	6'd18: f_scan8 = 7'd60;
	6'd19: f_scan8 = 7'd61;
	6'd20: f_scan8 = 7'd54;
	6'd21: f_scan8 = 7'd55;
	6'd22: f_scan8 = 7'd62;
	6'd23: f_scan8 = 7'd63;
	6'd24: f_scan8 = 7'd68;
	6'd25: f_scan8 = 7'd69;
	6'd26: f_scan8 = 7'd76;
	6'd27: f_scan8 = 7'd77;
	6'd28: f_scan8 = 7'd70;
	6'd29: f_scan8 = 7'd71;
	6'd30: f_scan8 = 7'd78;
	6'd31: f_scan8 = 7'd79;
	6'd32: f_scan8 = 7'd92;
	6'd33: f_scan8 = 7'd93;
	6'd34: f_scan8 = 7'd100;
	6'd35: f_scan8 = 7'd101;
	6'd36: f_scan8 = 7'd94;
	6'd37: f_scan8 = 7'd95;
	6'd38: f_scan8 = 7'd102;
	6'd39: f_scan8 = 7'd103;
	6'd40: f_scan8 = 7'd108;
	6'd41: f_scan8 = 7'd109;
	6'd42: f_scan8 = 7'd116;
	6'd43: f_scan8 = 7'd117;
	6'd44: f_scan8 = 7'd110;
	6'd45: f_scan8 = 7'd111;
	6'd46: f_scan8 = 7'd118;
	6'd47: f_scan8 = 7'd119;
	6'd48: f_scan8 = 7'd0;
	6'd49: f_scan8 = 7'd40;
	6'd50: f_scan8 = 7'd80;
	default: f_scan8 = 7'd0;
	endcase
endfunction

function automatic [3:0] f_zz4(input [3:0] i);
	case (i)
	4'd0: f_zz4 = 4'd0;
	4'd1: f_zz4 = 4'd4;
	4'd2: f_zz4 = 4'd1;
	4'd3: f_zz4 = 4'd2;
	4'd4: f_zz4 = 4'd5;
	4'd5: f_zz4 = 4'd8;
	4'd6: f_zz4 = 4'd12;
	4'd7: f_zz4 = 4'd9;
	4'd8: f_zz4 = 4'd6;
	4'd9: f_zz4 = 4'd3;
	4'd10: f_zz4 = 4'd7;
	4'd11: f_zz4 = 4'd10;
	4'd12: f_zz4 = 4'd13;
	4'd13: f_zz4 = 4'd14;
	4'd14: f_zz4 = 4'd11;
	4'd15: f_zz4 = 4'd15;
	default: f_zz4 = 4'd0;
	endcase
endfunction

function automatic [5:0] f_zz8(input [5:0] i);
	case (i)
	6'd0: f_zz8 = 6'd0;
	6'd1: f_zz8 = 6'd8;
	6'd2: f_zz8 = 6'd1;
	6'd3: f_zz8 = 6'd2;
	6'd4: f_zz8 = 6'd9;
	6'd5: f_zz8 = 6'd16;
	6'd6: f_zz8 = 6'd24;
	6'd7: f_zz8 = 6'd17;
	6'd8: f_zz8 = 6'd10;
	6'd9: f_zz8 = 6'd3;
	6'd10: f_zz8 = 6'd4;
	6'd11: f_zz8 = 6'd11;
	6'd12: f_zz8 = 6'd18;
	6'd13: f_zz8 = 6'd25;
	6'd14: f_zz8 = 6'd32;
	6'd15: f_zz8 = 6'd40;
	6'd16: f_zz8 = 6'd33;
	6'd17: f_zz8 = 6'd26;
	6'd18: f_zz8 = 6'd19;
	6'd19: f_zz8 = 6'd12;
	6'd20: f_zz8 = 6'd5;
	6'd21: f_zz8 = 6'd6;
	6'd22: f_zz8 = 6'd13;
	6'd23: f_zz8 = 6'd20;
	6'd24: f_zz8 = 6'd27;
	6'd25: f_zz8 = 6'd34;
	6'd26: f_zz8 = 6'd41;
	6'd27: f_zz8 = 6'd48;
	6'd28: f_zz8 = 6'd56;
	6'd29: f_zz8 = 6'd49;
	6'd30: f_zz8 = 6'd42;
	6'd31: f_zz8 = 6'd35;
	6'd32: f_zz8 = 6'd28;
	6'd33: f_zz8 = 6'd21;
	6'd34: f_zz8 = 6'd14;
	6'd35: f_zz8 = 6'd7;
	6'd36: f_zz8 = 6'd15;
	6'd37: f_zz8 = 6'd22;
	6'd38: f_zz8 = 6'd29;
	6'd39: f_zz8 = 6'd36;
	6'd40: f_zz8 = 6'd43;
	6'd41: f_zz8 = 6'd50;
	6'd42: f_zz8 = 6'd57;
	6'd43: f_zz8 = 6'd58;
	6'd44: f_zz8 = 6'd51;
	6'd45: f_zz8 = 6'd44;
	6'd46: f_zz8 = 6'd37;
	6'd47: f_zz8 = 6'd30;
	6'd48: f_zz8 = 6'd23;
	6'd49: f_zz8 = 6'd31;
	6'd50: f_zz8 = 6'd38;
	6'd51: f_zz8 = 6'd45;
	6'd52: f_zz8 = 6'd52;
	6'd53: f_zz8 = 6'd59;
	6'd54: f_zz8 = 6'd60;
	6'd55: f_zz8 = 6'd53;
	6'd56: f_zz8 = 6'd46;
	6'd57: f_zz8 = 6'd39;
	6'd58: f_zz8 = 6'd47;
	6'd59: f_zz8 = 6'd54;
	6'd60: f_zz8 = 6'd61;
	6'd61: f_zz8 = 6'd62;
	6'd62: f_zz8 = 6'd55;
	6'd63: f_zz8 = 6'd63;
	default: f_zz8 = 6'd0;
	endcase
endfunction

function automatic [21:0] f_imb(input [4:0] i);
	case (i)
	5'd0: f_imb = 22'd121;
	5'd1: f_imb = 22'd18;
	5'd2: f_imb = 22'd10;
	5'd3: f_imb = 22'd2;
	5'd4: f_imb = 22'd26;
	5'd5: f_imb = 22'd2066;
	5'd6: f_imb = 22'd2058;
	5'd7: f_imb = 22'd2050;
	5'd8: f_imb = 22'd2074;
	5'd9: f_imb = 22'd4114;
	5'd10: f_imb = 22'd4106;
	5'd11: f_imb = 22'd4098;
	5'd12: f_imb = 22'd4122;
	5'd13: f_imb = 22'd1938;
	5'd14: f_imb = 22'd1930;
	5'd15: f_imb = 22'd1922;
	5'd16: f_imb = 22'd1946;
	5'd17: f_imb = 22'd3986;
	5'd18: f_imb = 22'd3978;
	5'd19: f_imb = 22'd3970;
	5'd20: f_imb = 22'd3994;
	5'd21: f_imb = 22'd6034;
	5'd22: f_imb = 22'd6026;
	5'd23: f_imb = 22'd6018;
	5'd24: f_imb = 22'd6042;
	5'd25: f_imb = 22'd124;
	default: f_imb = 22'd0;
	endcase
endfunction

function automatic [18:0] f_pmb(input [1:0] i);
	case (i)
	2'd0: f_pmb = 19'd69640;
	2'd1: f_pmb = 19'd143376;
	2'd2: f_pmb = 19'd143392;
	2'd3: f_pmb = 19'd274496;
	default: f_pmb = 19'd0;
	endcase
endfunction

function automatic [18:0] f_psub(input [1:0] i);
	case (i)
	2'd0: f_psub = 19'd69640;
	2'd1: f_psub = 19'd135184;
	2'd2: f_psub = 19'd135200;
	2'd3: f_psub = 19'd266304;
	default: f_psub = 19'd0;
	endcase
endfunction

function automatic [18:0] f_bmb(input [4:0] i);
	case (i)
	5'd0: f_bmb = 19'd127232;
	5'd1: f_bmb = 19'd69640;
	5'd2: f_bmb = 19'd81928;
	5'd3: f_bmb = 19'd86024;
	5'd4: f_bmb = 19'd143376;
	5'd5: f_bmb = 19'd143392;
	5'd6: f_bmb = 19'd180240;
	5'd7: f_bmb = 19'd180256;
	5'd8: f_bmb = 19'd167952;
	5'd9: f_bmb = 19'd167968;
	5'd10: f_bmb = 19'd155664;
	5'd11: f_bmb = 19'd155680;
	5'd12: f_bmb = 19'd176144;
	5'd13: f_bmb = 19'd176160;
	5'd14: f_bmb = 19'd188432;
	5'd15: f_bmb = 19'd188448;
	5'd16: f_bmb = 19'd159760;
	5'd17: f_bmb = 19'd159776;
	5'd18: f_bmb = 19'd184336;
	5'd19: f_bmb = 19'd184352;
	5'd20: f_bmb = 19'd192528;
	5'd21: f_bmb = 19'd192544;
	5'd22: f_bmb = 19'd323648;
	default: f_bmb = 19'd0;
	endcase
endfunction

function automatic [18:0] f_bsub(input [3:0] i);
	case (i)
	4'd0: f_bsub = 19'd65792;
	4'd1: f_bsub = 19'd69640;
	4'd2: f_bsub = 19'd81928;
	4'd3: f_bsub = 19'd86024;
	4'd4: f_bsub = 19'd143376;
	4'd5: f_bsub = 19'd143392;
	4'd6: f_bsub = 19'd180240;
	4'd7: f_bsub = 19'd180256;
	4'd8: f_bsub = 19'd192528;
	4'd9: f_bsub = 19'd192544;
	4'd10: f_bsub = 19'd274496;
	4'd11: f_bsub = 19'd311360;
	4'd12: f_bsub = 19'd323648;
	default: f_bsub = 19'd0;
	endcase
endfunction

function automatic [5:0] f_cqtab(input [5:0] i);
	case (i)
	6'd0: f_cqtab = 6'd0;
	6'd1: f_cqtab = 6'd1;
	6'd2: f_cqtab = 6'd2;
	6'd3: f_cqtab = 6'd3;
	6'd4: f_cqtab = 6'd4;
	6'd5: f_cqtab = 6'd5;
	6'd6: f_cqtab = 6'd6;
	6'd7: f_cqtab = 6'd7;
	6'd8: f_cqtab = 6'd8;
	6'd9: f_cqtab = 6'd9;
	6'd10: f_cqtab = 6'd10;
	6'd11: f_cqtab = 6'd11;
	6'd12: f_cqtab = 6'd12;
	6'd13: f_cqtab = 6'd13;
	6'd14: f_cqtab = 6'd14;
	6'd15: f_cqtab = 6'd15;
	6'd16: f_cqtab = 6'd16;
	6'd17: f_cqtab = 6'd17;
	6'd18: f_cqtab = 6'd18;
	6'd19: f_cqtab = 6'd19;
	6'd20: f_cqtab = 6'd20;
	6'd21: f_cqtab = 6'd21;
	6'd22: f_cqtab = 6'd22;
	6'd23: f_cqtab = 6'd23;
	6'd24: f_cqtab = 6'd24;
	6'd25: f_cqtab = 6'd25;
	6'd26: f_cqtab = 6'd26;
	6'd27: f_cqtab = 6'd27;
	6'd28: f_cqtab = 6'd28;
	6'd29: f_cqtab = 6'd29;
	6'd30: f_cqtab = 6'd29;
	6'd31: f_cqtab = 6'd30;
	6'd32: f_cqtab = 6'd31;
	6'd33: f_cqtab = 6'd32;
	6'd34: f_cqtab = 6'd32;
	6'd35: f_cqtab = 6'd33;
	6'd36: f_cqtab = 6'd34;
	6'd37: f_cqtab = 6'd34;
	6'd38: f_cqtab = 6'd35;
	6'd39: f_cqtab = 6'd35;
	6'd40: f_cqtab = 6'd36;
	6'd41: f_cqtab = 6'd36;
	6'd42: f_cqtab = 6'd37;
	6'd43: f_cqtab = 6'd37;
	6'd44: f_cqtab = 6'd37;
	6'd45: f_cqtab = 6'd38;
	6'd46: f_cqtab = 6'd38;
	6'd47: f_cqtab = 6'd38;
	6'd48: f_cqtab = 6'd39;
	6'd49: f_cqtab = 6'd39;
	6'd50: f_cqtab = 6'd39;
	6'd51: f_cqtab = 6'd39;
	default: f_cqtab = 6'd0;
	endcase
endfunction
