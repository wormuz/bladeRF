`timescale 1ns/1ps

module tb_up_axi_timeout;
  reg clk = 0;
  reg rstn = 0;

  reg awvalid = 0;
  reg [31:0] awaddr = 0;
  wire awready;
  reg wvalid = 0;
  reg [31:0] wdata = 0;
  reg [3:0] wstrb = 4'hf;
  wire wready;
  wire bvalid;
  wire [1:0] bresp;
  reg bready = 0;
  reg arvalid = 0;
  reg [31:0] araddr = 0;
  wire arready;
  wire rvalid;
  wire [1:0] rresp;
  wire [31:0] rdata;
  reg rready = 0;
  reg timeout_clear_toggle = 0;
  wire timeout;
  wire wreq;
  wire [13:0] waddr;
  wire [31:0] wdata_core;
  reg wack = 0;
  wire rreq;
  wire [13:0] raddr;
  reg [31:0] rdata_core = 32'h12345678;
  reg rack = 0;

  always #5 clk = !clk;

  up_axi dut (
    .up_rstn(rstn), .up_clk(clk),
    .up_axi_awvalid(awvalid), .up_axi_awaddr(awaddr),
    .up_axi_awready(awready), .up_axi_wvalid(wvalid),
    .up_axi_wdata(wdata), .up_axi_wstrb(wstrb),
    .up_axi_wready(wready), .up_axi_bvalid(bvalid),
    .up_axi_bresp(bresp), .up_axi_bready(bready),
    .up_axi_arvalid(arvalid), .up_axi_araddr(araddr),
    .up_axi_arready(arready), .up_axi_rvalid(rvalid),
    .up_axi_rresp(rresp), .up_axi_rdata(rdata),
    .up_axi_rready(rready),
    .up_axi_timeout_clear_toggle(timeout_clear_toggle),
    .up_axi_timeout(timeout),
    .up_wreq(wreq), .up_waddr(waddr), .up_wdata(wdata_core),
    .up_wack(wack), .up_rreq(rreq), .up_raddr(raddr),
    .up_rdata(rdata_core), .up_rack(rack)
  );

  task write_and_check;
    input integer ack_write;
    input [1:0] expected_resp;
    integer cycles;
    begin
      @(negedge clk);
      awvalid = 1;
      wvalid = 1;
      awaddr = 32'h00000100;
      wdata = 32'hcafef00d;
      cycles = 0;
      if (ack_write) begin
        wait (wreq);
        @(posedge clk);
        @(negedge clk);
        wack = 1;
        @(negedge clk);
        wack = 0;
      end
      while (!(awready && wready) && cycles < 80) begin
        @(negedge clk);
        cycles = cycles + 1;
      end
      if (!(awready && wready)) $fatal(1, "write response timeout");
      awvalid = 0;
      wvalid = 0;
      wack = 0;
      while (!bvalid && cycles < 90) begin
        @(negedge clk);
        cycles = cycles + 1;
      end
      if (!bvalid) $fatal(1, "missing BVALID");
      if (bresp !== expected_resp)
        $fatal(1, "BRESP=%b expected=%b", bresp, expected_resp);
      if (timeout !== (expected_resp == 2'b10))
        $fatal(1, "timeout sticky=%b expected for BRESP %b", timeout,
               expected_resp);
      bready = 1;
      @(negedge clk);
      bready = 0;
      repeat (2) @(negedge clk);
    end
  endtask

  task read_and_check;
    input integer ack_read;
    input [1:0] expected_resp;
    input [31:0] expected_data;
    integer cycles;
    begin
      @(negedge clk);
      arvalid = 1;
      araddr = 32'h00000104;
      cycles = 0;
      if (ack_read) begin
        wait (rreq);
        @(posedge clk);
        @(negedge clk);
        rack = 1;
        @(negedge clk);
        rack = 0;
      end
      while (!arready && cycles < 80) begin
        @(negedge clk);
        cycles = cycles + 1;
      end
      if (!arready) $fatal(1, "read address timeout");
      arvalid = 0;
      rack = 0;
      while (!rvalid && cycles < 90) begin
        @(negedge clk);
        cycles = cycles + 1;
      end
      if (!rvalid) $fatal(1, "missing RVALID");
      if (rresp !== expected_resp)
        $fatal(1, "RRESP=%b expected=%b", rresp, expected_resp);
      if (rdata !== expected_data)
        $fatal(1, "RDATA=%h expected=%h", rdata, expected_data);
      if (timeout !== (expected_resp == 2'b10))
        $fatal(1, "timeout sticky=%b expected for RRESP %b", timeout,
               expected_resp);
      rready = 1;
      @(negedge clk);
      rready = 0;
      repeat (2) @(negedge clk);
    end
  endtask

  initial begin
    repeat (4) @(negedge clk);
    rstn = 1;
    repeat (2) @(negedge clk);

    write_and_check(1, 2'b00);
    write_and_check(0, 2'b10);

    timeout_clear_toggle = !timeout_clear_toggle;
    repeat (2) @(negedge clk);
    if (timeout !== 1'b0) $fatal(1, "clear toggle did not clear status");
    read_and_check(0, 2'b10, 32'hdead_dead);

    timeout_clear_toggle = !timeout_clear_toggle;
    repeat (2) @(negedge clk);
    if (timeout !== 1'b0) $fatal(1, "second clear toggle did not clear status");
    read_and_check(1, 2'b00, 32'h1234_5678);

    $display("up_axi timeout sideband: PASS");
    $finish;
  end
endmodule
