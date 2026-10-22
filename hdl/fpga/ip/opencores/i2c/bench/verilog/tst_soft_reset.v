// bladeRF: soft reset (offset 7 write) and diagnostic readback (offset 7
// read). A controller frozen mid-byte by a held-low SCL keeps TIP set; the
// soft reset must return it to idle and the next transfer must complete.
// Run: host/misc/run_i2c_soft_reset_sim_test.sh (needs iverilog)
`include "timescale.v"

module tst_soft_reset();

	reg clk = 0;
	reg rstn = 0;
	reg hold_scl = 0;
	reg hold_sda = 0;
	integer errors = 0;

	wire [31:0] adr;
	wire [7:0] dat_i, dat_o;
	wire we, stb, cyc, ack, inta;
	wire scl, scl_o, scl_oen, sda, sda_o, sda_oen;
	reg  [7:0] q;

	parameter PRER_LO = 3'b000;
	parameter PRER_HI = 3'b001;
	parameter CTR     = 3'b010;
	parameter TXR     = 3'b011;
	parameter CR      = 3'b100;
	parameter SR      = 3'b100;
	parameter CR_R    = 3'b110;
	parameter DIAG    = 3'b111;
	parameter SADR    = 7'b0010_000;

	always #5 clk = ~clk;

	wb_master_model #(8, 32) u0 (
		.clk(clk), .rst(rstn), .adr(adr), .din(dat_i), .dout(dat_o),
		.cyc(cyc), .stb(stb), .we(we), .sel(), .ack(ack),
		.err(1'b0), .rty(1'b0));

	i2c_master_top #(.ARST_LVL(1'b0)) dut (
		.wb_clk_i(clk), .wb_rst_i(1'b0), .arst_i(rstn),
		.wb_adr_i(adr[2:0]), .wb_dat_i(dat_o), .wb_dat_o(dat_i),
		.wb_we_i(we), .wb_stb_i(stb), .wb_cyc_i(cyc), .wb_ack_o(ack),
		.wb_inta_o(inta),
		.scl_pad_i(scl), .scl_pad_o(scl_o), .scl_padoen_o(scl_oen),
		.sda_pad_i(sda), .sda_pad_o(sda_o), .sda_padoen_o(sda_oen));

	i2c_slave_model #(SADR) slave (.scl(scl), .sda(sda));

	assign scl = scl_oen ? 1'bz : scl_o;
	assign sda = sda_oen ? 1'bz : sda_o;
	assign scl = hold_scl ? 1'b0 : 1'bz;
	assign sda = hold_sda ? 1'b0 : 1'bz;
	pullup p1(scl);
	pullup p2(sda);

	task check(input [7:0] got, input [7:0] mask, input [7:0] want,
	           input [8*24-1:0] what);
	  if ((got & mask) !== want) begin
	    $display("FAIL %0s: got %02h mask %02h want %02h", what, got, mask, want);
	    errors = errors + 1;
	  end
	endtask

	task wait_tip_clear(output timed_out);
	  integer n;
	  begin
	    timed_out = 1;
	    // TIP is registered from CR; let it assert first, as the Nios
	    // completion poll does.
	    u0.wb_read(1, SR, q);
	    for (n = 0; n < 20000; n = n + 1) begin
	      u0.wb_read(0, SR, q);
	      if (!q[1]) begin
	        timed_out = 0;
	        n = 20000;
	      end
	    end
	  end
	endtask

	task write_byte_to_slave(output ok);
	  reg t;
	  begin
	    u0.wb_write(1, TXR, {SADR, 1'b0});
	    u0.wb_write(0, CR, 8'h90);              // STA | WR
	    wait_tip_clear(t);
	    ok = !t && !q[7];
	    u0.wb_write(1, TXR, 8'h01);
	    u0.wb_write(0, CR, 8'h50);              // STO | WR
	    wait_tip_clear(t);
	    ok = ok && !t && !q[7];
	  end
	endtask

	reg ok, timed_out;
	integer stop_wait;
	integer scl_rises = 0;
	reg count_scl = 0;
	always @(posedge scl) if (count_scl) scl_rises = scl_rises + 1;

	initial begin
	  #2 rstn = 0;
	  #100 rstn = 1;
	  #20;
	  u0.wb_write(1, PRER_LO, 8'hc8);
	  u0.wb_write(1, PRER_HI, 8'h00);
	  u0.wb_write(1, CTR, 8'h80);

	  u0.wb_read(1, DIAG, q);
	  check(q, 8'hff, 8'hc0, "idle diag");

	  write_byte_to_slave(ok);
	  if (!ok) begin $display("FAIL baseline transfer"); errors = errors + 1; end

	  // The Nios completion poll discards one status read after a command
	  // write: the second back-to-back read must show the new command
	  // (TIP set, IF cleared by IACK), not the previous one's IF.
	  u0.wb_read(1, SR, q);
	  check(q, 8'h03, 8'h01, "IF set by last command");
	  u0.wb_write(1, TXR, {SADR, 1'b0});
	  u0.wb_write(0, CR, 8'h91);               // STA | WR | IACK
	  u0.wb_read(0, SR, q);
	  u0.wb_read(0, SR, q);
	  check(q, 8'h03, 8'h02, "second read sees TIP, IF clear");
	  wait_tip_clear(timed_out);
	  u0.wb_read(1, SR, q);
	  check(q, 8'h03, 8'h01, "IF marks completion");
	  u0.wb_write(0, CR, 8'h41);               // STO | IACK
	  u0.wb_read(0, SR, q);
	  u0.wb_read(0, SR, q);
	  check(q, 8'h03, 8'h00, "STOP-only: no TIP, IF cleared");
	  for (stop_wait = 0; stop_wait < 20000 && !q[0]; stop_wait = stop_wait + 1)
	    u0.wb_read(0, SR, q);
	  check(q, 8'h03, 8'h01, "STOP-only completes: IF");
	  for (stop_wait = 0; stop_wait < 20000 && q[6]; stop_wait = stop_wait + 1)
	    u0.wb_read(0, SR, q);
	  check(q, 8'h40, 8'h00, "bus free after STOP");

	  // Freeze the controller mid-byte: SCL held low while it drives a byte.
	  u0.wb_write(1, TXR, {SADR, 1'b0});
	  u0.wb_write(0, CR, 8'h90);
	  #20000 hold_scl = 1;
	  wait_tip_clear(timed_out);
	  if (!timed_out) begin $display("FAIL freeze not reproduced"); errors = errors + 1; end
	  u0.wb_read(1, DIAG, q);
	  check(q, 8'h40, 8'h00, "frozen scl low");
	  check(q, 8'h20, 8'h20, "frozen slave_wait");
	  if ((q & 8'h1f) == 0) begin
	    $display("FAIL frozen byte state reads idle: %02h", q);
	    errors = errors + 1;
	  end

	  // A wrong key is ignored.
	  u0.wb_write(1, DIAG, 8'h5a);
	  u0.wb_read(1, SR, q);
	  check(q, 8'h02, 8'h02, "wrong key keeps TIP");

	  u0.wb_write(1, DIAG, 8'ha5);
	  u0.wb_read(1, SR, q);
	  check(q, 8'h22, 8'h00, "soft reset clears TIP/AL");
	  u0.wb_read(1, CR_R, q);
	  check(q, 8'hf0, 8'h00, "soft reset clears CR");
	  u0.wb_read(1, DIAG, q);
	  check(q, 8'h3f, 8'h00, "soft reset idles FSM");
	  u0.wb_read(1, CTR, q);
	  check(q, 8'hff, 8'h80, "CTR kept");

	  hold_scl = 0;
	  #40000;
	  write_byte_to_slave(ok);
	  if (!ok) begin $display("FAIL transfer after soft reset"); errors = errors + 1; end

	  // Bus recovery: RD|NACK|STO without a START clocks nine SCL pulses
	  // (eight data, one ACK) and ends with a STOP, whose SCL rise is the
	  // tenth.
	  scl_rises = 0;
	  count_scl = 1;
	  u0.wb_write(0, CR, 8'h69);               // STO | RD | NACK | IACK
	  wait_tip_clear(timed_out);
	  for (stop_wait = 0; stop_wait < 20000 && q[6]; stop_wait = stop_wait + 1)
	    u0.wb_read(0, SR, q);
	  count_scl = 0;
	  if (timed_out || scl_rises != 10) begin
	    $display("FAIL recovery clocks: %0d rises, timed_out %0d",
	             scl_rises, timed_out);
	    errors = errors + 1;
	  end
	  check(q, 8'h60, 8'h00, "recovery: no AL, bus free");

	  // SDA held low reads back as such.
	  hold_sda = 1;
	  #40000;
	  u0.wb_read(1, DIAG, q);
	  check(q, 8'h80, 8'h00, "sda low visible");

	  // The soft reset presets the filtered lines high. The Nios sequence
	  // (key, CTRL 0, PRER, CTRL EN, I2C_LINE_SETTLE_READS reads) must see
	  // the real SDA again before it decides on bus recovery.
	  u0.wb_write(0, DIAG, 8'ha5);
	  u0.wb_write(0, CTR, 8'h00);
	  u0.wb_write(0, PRER_LO, 8'hc8);
	  u0.wb_write(0, PRER_HI, 8'h00);
	  u0.wb_write(0, CTR, 8'h80);
	  for (stop_wait = 0; stop_wait < 32; stop_wait = stop_wait + 1)
	    u0.wb_read(0, DIAG, q);
	  check(q, 8'h80, 8'h00, "sda low after soft reset settle");
	  hold_sda = 0;

	  if (errors == 0) $display("PASS tst_soft_reset");
	  else $display("FAIL tst_soft_reset: %0d errors", errors);
	  $finish;
	end

	initial begin
	  #50000000;
	  $display("FAIL tst_soft_reset: watchdog");
	  $finish;
	end

endmodule
