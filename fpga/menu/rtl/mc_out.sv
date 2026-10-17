// Linux frame buffer on the menu core's own (15 kHz) video output.
//
// A frame buffer that fits the 640 pixels wide output is shown 1:1 and
// centered, like the stock console output. A larger one (MiSTer MediaCenter
// playing a DVD or a video file) is scaled to fill the output: the source
// pixel and line of every output pixel are picked by nearest neighbour, so an
// interlaced output (480i/576i) gets the frame buffer's own lines in each
// field. With "wide" a 16:9 picture is letterboxed on the 4:3 screen.
//
// CRT Adjust (after rmonic79's MiSTer CRT Adjust): a scaled picture can be
// made narrower or wider, shorter or taller and moved on the screen to fit a
// CRT's geometry. Only the picture is resampled; the sync timing never
// changes, so the CRT cannot lose lock, and the colours are the source's.
//
// Formats: 32 bits per pixel, or YUV 4:2:2 (bytes Y0 U Y1 V, frame buffer
// format bit 5 with 16 bits per pixel) that the player writes for decoded
// video and that is converted to RGB here.

module mc_out
(
	input             clk_sys,
	input             ddr_grant,
	output reg [28:0] ddr_addr,
	output reg  [7:0] ddr_burst,
	output reg        ddr_rd = 0,
	input             ddr_busy,
	input      [63:0] ddr_dout,
	input             ddr_dout_ready,
	output            ddr_idle,

	input       [5:0] fb_fmt,
	input      [31:0] fb_base,
	input      [11:0] fb_width,
	input      [11:0] fb_height,
	input      [13:0] fb_stride,

	input             clk_vid,
	input             ce_pix,
	input             show,
	input             scaled,   // fill the output instead of 1:1
	input             wide,     // 16:9 picture: letterbox
	input             ilace,    // interlaced output: two fields per frame
	input             fields,   // the picture is interlaced: keep its fields apart
	input             field,    // field being shown (1: odd lines)
	input       [9:0] hc,
	input       [9:0] vc,
	input       [9:0] height,   // active lines of one field
	input signed [4:0] adj_hsize, // width: 8 pixels per step
	input signed [5:0] adj_hpos,  // horizontal position: 2 pixels per step
	input signed [4:0] adj_vsize, // height: 4 lines of the picture per step
	input signed [4:0] adj_vpos,  // vertical position: 1 line of the field per step
	output reg [23:0] rgb,
	output reg        active = 0
);

localparam WIDTH = 640;
localparam BURST = 32;

reg  [5:0] fmt;
reg [11:0] w, h;
reg  [9:0] out_h, cols, lines, left, top;
reg  [1:0] show_s = 0;
reg        scale, ilc, wd, fld;
wire       yuv = fmt[5] && fmt[2:0] == 3'b100;

// lines of the whole output picture (both fields when interlaced)
wire [10:0] pic_h  = ilc ? {out_h, 1'b0} : {1'b0, out_h};
// lines used by the picture: 3/4 of them for a letterboxed 16:9 picture
wire [10:0] act_h  = wd ? (pic_h - (pic_h >> 2)) : pic_h;
wire [10:0] act_y0 = wd ? (pic_h >> 3) : 11'd0;

// The picture's rectangle after CRT Adjust: width and left edge in output
// pixels, height and top in lines of the whole picture (both fields). The top
// stays even on an interlaced output, so the fields keep their order.
reg signed [11:0] pic_w = 640, pic_x0 = 0, pic_hh = 0, pic_y0 = 0;
always @(posedge clk_vid) begin
	pic_w  <= 12'sd640 + $signed({{4{adj_hsize[4]}}, adj_hsize, 3'b000});
	pic_x0 <= ((12'sd640 - pic_w) >>> 1) + $signed({{5{adj_hpos[5]}}, adj_hpos, 1'b0});
	pic_hh <= $signed({1'b0, act_h}) + (ilc ? $signed({{4{adj_vsize[4]}}, adj_vsize, 3'b000})
	                                        : $signed({{5{adj_vsize[4]}}, adj_vsize, 2'b00}));
	pic_y0 <= $signed({1'b0, act_y0}) + ((($signed({1'b0, act_h}) - pic_hh) >>> 2) <<< 1) +
	          (ilc ? $signed({{6{adj_vpos[4]}}, adj_vpos, 1'b0}) : $signed({{7{adj_vpos[4]}}, adj_vpos}));
end

always @(posedge clk_vid) begin
	fmt    <= fb_fmt;
	w      <= fb_width;
	h      <= fb_height;
	out_h  <= height;
	scale  <= scaled;
	ilc    <= ilace;
	wd     <= wide;
	fld    <= fields;
	cols   <= w[9:0];
	lines  <= h[9:0];
	left   <= (WIDTH[9:0] - cols) >> 1;
	top    <= (out_h - lines) >> 1;
	show_s <= {show_s[0], show};
	active <= show_s[1] && (fmt[2:0] == 3'b110 || yuv) && w && h && (scale ? w <= 12'd2048 : (w <= WIDTH && h <= out_h));
end

// Scale factors, 16.16 fixed point: source pixels per output pixel and source
// lines per output line. Divided again at every frame start (slowly, bit by
// bit) so they follow a frame buffer change.
reg [27:0] hstep = 0, vstep = 0;
wire       div_go = ce_pix && hc == 0 && vc == out_h;
wire [27:0] hq, vq;
wire        hdone, vdone;

mc_div hdiv(.clk(clk_vid), .start(div_go), .num({w, 16'd0}), .den(pic_w[10:0]), .q(hq), .done(hdone));
mc_div vdiv(.clk(clk_vid), .start(div_go), .num({h, 16'd0}), .den(pic_hh[10:0]), .q(vq), .done(vdone));

// The frame buffer's address (the page the player shows) is taken once per
// field, or on an interlaced output once per frame, before its first field:
// each picture is then shown as a whole frame, its first field first. Taken
// at any field, a picture that came on the second one would be shown with its
// fields out of order: the picture jumps up and down by a line.
//
// The player switches pages at its own pace (25 pictures a second for a PAL
// DVD, timed by the sound and the HDMI refresh), which drifts slowly against
// the CRT's 25 frames. When the switches come close to the moment a frame
// takes its page, their small irregularities make some pictures last two
// frames and the next one none: the motion stutters. So the new page waits
// half a frame before it can be taken whenever the switches keep arriving
// near that moment: they then land in the middle of the frame, away from it.
//
// The Main writes the address in two halves: a page counts only once the
// address has stayed the same for a while, or the moment between the two
// writes would count as a page of its own, pointing to the wrong memory.
reg [31:0] base;
reg [31:0] fb_s1, fb_s2, seen, pend, ready;
reg [11:0] stable = 0;
reg        pend_v = 0, dly = 0;
reg  [1:0] danger = 0;
reg  [9:0] fpos = 0, flen = 0, pcnt = 0;

wire       frame_go = div_go && ilc && field;  // the page is taken here
wire       line_go  = ce_pix && hc == 0;
wire [9:0] half     = flen >> 1;

always @(posedge clk_vid) begin
	reg  [9:0] q;
	reg        near, d;

	if(hdone) hstep <= hq;
	if(vdone) vstep <= vq;

	fb_s1 <= fb_base;
	fb_s2 <= fb_s1;
	if(fb_s1 != fb_s2) stable <= 0;
	else if(~&stable) stable <= stable + 1'd1;

	if(frame_go) begin
		flen <= fpos;
		fpos <= 0;
	end
	else if(line_go && fpos != 10'h3FF) fpos <= fpos + 1'd1;

	if(&stable && fb_s2 != seen) begin
		// a new page: where it came in the frame, after the wait
		seen <= fb_s2;
		q = fpos + (dly ? half : 10'd0);
		if(q >= flen) q = q - flen;
		near = flen && (q < 10'd96 || q + 10'd96 > flen);
		d = dly;
		if(near && danger == 2'd2) begin
			d = ~dly;
			danger <= 0;
		end
		else if(near) danger <= danger + 1'd1;
		else if(danger) danger <= danger - 1'd1;
		dly <= d;
		if(pend_v) ready <= pend;
		pend   <= fb_s2;
		pend_v <= 1;
		pcnt   <= d ? half : 10'd0;
	end
	else if(pend_v && line_go) begin
		if(!pcnt) begin
			ready  <= pend;
			pend_v <= 0;
		end
		else pcnt <= pcnt - 1'd1;
	end

	if(!active) begin
		base   <= fb_base;
		ready  <= fb_base;
		pend_v <= 0;
	end
	else if(div_go && !ilc) base <= seen;
	else if(frame_go) base <= (pend_v && !pcnt) ? pend : ready;
end

// The line about to be shown, as a line of the whole picture.
function [10:0] pic_line(input [9:0] y, input f);
	pic_line = ilc ? {y, f} : {1'b0, y};
endfunction

reg        req_t = 0;
reg [11:0] req_line;
reg        req_bank;

always @(posedge clk_vid) begin
	reg  [9:0] ny;
	reg        nf;
	reg [10:0] py, ay;
	reg signed [11:0] ays;
	reg [38:0] sy;

	if(ce_pix && !hc && active && (vc + 1'd1 < out_h || vc == out_h)) begin
		// fetch the next line; at the end of the field, the first line of
		// the next one
		ny = (vc == out_h) ? 10'd0 : vc + 1'd1;
		nf = (vc == out_h) ? ~field : field;
		if(scale) begin
			py  = pic_line(ny, ilc & nf);
			ays = $signed({1'b0, py}) - pic_y0;
			ay  = ays[10:0];
			// an interlaced picture is scaled field by field, so each
			// output field only shows lines of the same field
			sy = (ilc & fld) ? (ay >> 1) * vstep : ay * vstep;
			if(ilc & fld) sy[27:16] = {sy[26:16], ay[0]};
			if(ays >= 0 && ays < pic_hh && sy[27:16] < h) begin
				req_line <= sy[27:16];
				req_bank <= ny[0];
				req_t    <= ~req_t;
			end
		end
		else if(ny >= top && ny - top < lines) begin
			req_line <= ny - top;
			req_bank <= ny[0];
			req_t    <= ~req_t;
		end
	end
end

reg  [2:0] req_s = 0;
reg  [1:0] start = 0;
reg        fetch = 0;
reg [11:0] line;
reg        bank, wbank;
reg [25:0] offset;
reg [10:0] words;
reg  [9:0] waddr;
reg  [7:0] beats = 0;
reg        bwe = 0;
reg [10:0] baddr;
reg [63:0] bdata;

assign ddr_idle = ~fetch & ~|start & ~ddr_rd;

wire [12:0] colsp1 = w + 1'd1;
wire [12:0] colsp3 = w + 2'd3;

always @(posedge clk_sys) begin
	reg [31:0] laddr;

	req_s <= {req_s[1:0], req_t};
	bwe   <= 0;

	if(ddr_rd && !ddr_busy) begin
		ddr_rd   <= 0;
		ddr_addr <= ddr_addr + ddr_burst;
	end

	if(ddr_dout_ready && fetch && beats) begin
		bwe   <= 1;
		baddr <= {wbank, waddr};
		bdata <= ddr_dout;
		waddr <= waddr + 1'd1;
		beats <= beats - 1'd1;
	end

	case(start)
		0: if(!fetch && req_s[2] != req_s[1] && ddr_grant) begin
				line  <= req_line;
				bank  <= req_bank;
				start <= 1;
			end
		1: begin
				offset <= line * fb_stride;
				start  <= 2;
			end
		2: begin
				laddr    = base + offset;
				ddr_addr <= laddr[31:3];
				wbank    <= bank;
				waddr    <= 0;
				words    <= yuv ? {1'b0, colsp3[11:2]} : colsp1[11:1];
				fetch    <= 1;
				start    <= 0;
			end
	endcase

	if(fetch && !start && !ddr_rd && !beats && ddr_grant) begin
		if(!words) fetch <= 0;
		else begin
			ddr_burst <= (words > BURST) ? BURST[7:0] : words[7:0];
			beats     <= (words > BURST) ? BURST[7:0] : words[7:0];
			words     <= (words > BURST) ? words - BURST[10:0] : 11'd0;
			ddr_rd    <= 1;
		end
	end
end

reg [63:0] buffer[2048];
always @(posedge clk_sys) if(bwe) buffer[baddr] <= bdata;

reg [63:0] q;
reg  [1:0] hi;
reg        shown, shown2;

// the current line's source line is inside the picture (letterbox bars)
reg        line_on;
always @(posedge clk_vid) begin
	reg [10:0] py, ay;
	reg signed [11:0] ays;
	reg [38:0] sy;

	py  = pic_line(vc, ilc & field);
	ays = $signed({1'b0, py}) - pic_y0;
	ay  = ays[10:0];
	sy  = (ilc & fld) ? (ay >> 1) * vstep : ay * vstep;
	if(ilc & fld) sy[27:16] = {sy[26:16], ay[0]};
	line_on <= ays >= 0 && ays < pic_hh && sy[27:16] < h;
end

always @(posedge clk_vid) begin
	reg [10:0] sx;
	reg [37:0] px;
	reg signed [11:0] hx;

	hx = $signed({2'b00, hc}) - pic_x0;
	if(scale) begin
		px = hx[10:0] * hstep;
		sx = px[26:16];
	end
	else begin
		sx = hc - left;
	end
	if(yuv) q <= buffer[{vc[0], 1'b0, sx[10:2]}];
	else    q <= buffer[{vc[0], sx[10:1]}];
	hi    <= sx[1:0];
	shown <= scale ? (active && hc < WIDTH && hx >= 0 && hx < pic_w && px[27:16] < w && vc < out_h && line_on)
	               : (active && hc >= left && sx < cols && vc >= top && vc - top < lines);
end

// RGB, or the products of the YUV to RGB conversion (BT.601 limited range,
// 8.8 fixed point), then their sums bounded to 0..255.
reg signed [21:0] cy, cr, cgu, cgv, cb;
reg        [23:0] rgb2;
reg               yuv2;

function [7:0] clip(input signed [21:0] v);
	reg signed [21:0] t;
	begin
		t = v + 22'sd128;
		clip = t < 0 ? 8'd0 : t >= 22'sd65536 ? 8'd255 : t[15:8];
	end
endfunction

always @(posedge clk_vid) begin
	reg [31:0] p;
	reg  [7:0] y, u, v;

	if(yuv) begin
		p = hi[1] ? q[63:32] : q[31:0];
		y = hi[0] ? p[23:16] : p[7:0];
		u = p[15:8];
		v = p[31:24];
	end
	else begin
		p = hi[0] ? q[63:32] : q[31:0];
		y = 0; u = 0; v = 0;
	end
	cy  <= ($signed({3'b000, y}) - 11'sd16)  * 11'sd298;
	cr  <= ($signed({3'b000, v}) - 11'sd128) * 11'sd409;
	cgu <= ($signed({3'b000, u}) - 11'sd128) * 11'sd100;
	cgv <= ($signed({3'b000, v}) - 11'sd128) * 11'sd208;
	cb  <= ($signed({3'b000, u}) - 11'sd128) * 11'sd516;
	rgb2   <= fmt[4] ? p[23:0] : {p[7:0], p[15:8], p[23:16]};
	yuv2   <= yuv;
	shown2 <= shown;

	if(!shown2)    rgb <= 0;
	else if(yuv2)  rgb <= {clip(cy + cr), clip(cy - cgu - cgv), clip(cy + cb)};
	else           rgb <= rgb2;
end

endmodule

// Unsigned divider, one quotient bit per clock: q = num / den.
module mc_div
(
	input             clk,
	input             start,
	input      [27:0] num,
	input      [10:0] den,
	output reg [27:0] q,
	output reg        done = 0
);

reg [27:0] n;
reg [11:0] r;
reg  [4:0] cnt = 0;

always @(posedge clk) begin
	reg [11:0] t;

	done <= 0;
	if(start) begin
		n   <= num;
		r   <= 0;
		q   <= 0;
		cnt <= 5'd28;
	end
	else if(cnt) begin
		t = {r[10:0], n[27]};
		n <= {n[26:0], 1'b0};
		if(den && t >= den) begin
			r <= t - den;
			q <= {q[26:0], 1'b1};
		end
		else begin
			r <= t;
			q <= {q[26:0], 1'b0};
		end
		cnt <= cnt - 1'd1;
		if(cnt == 1) done <= 1;
	end
end

endmodule
