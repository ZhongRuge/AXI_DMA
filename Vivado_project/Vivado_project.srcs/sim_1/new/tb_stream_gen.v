`timescale 1ns / 1ps

module tb_stream_gen;

localparam [4:0] REG_CTRL               = 5'h00;
localparam [4:0] REG_STATUS             = 5'h04;
localparam [4:0] REG_PACKET_LEN         = 5'h08;
localparam [4:0] REG_RATE_DIV           = 5'h0c;
localparam [4:0] REG_WORD_COUNT         = 5'h10;
localparam [4:0] REG_PACKET_COUNT       = 5'h14;
localparam [4:0] REG_BACKPRESSURE_COUNT = 5'h18;
localparam [4:0] REG_VERSION            = 5'h1c;

reg         s_axi_aclk;
reg         s_axi_aresetn;
reg  [4:0]  s_axi_awaddr;
reg         s_axi_awvalid;
wire        s_axi_awready;
reg  [31:0] s_axi_wdata;
reg  [3:0]  s_axi_wstrb;
reg         s_axi_wvalid;
wire        s_axi_wready;
wire [1:0]  s_axi_bresp;
wire        s_axi_bvalid;
reg         s_axi_bready;
reg  [4:0]  s_axi_araddr;
reg         s_axi_arvalid;
wire        s_axi_arready;
wire [31:0] s_axi_rdata;
wire [1:0]  s_axi_rresp;
wire        s_axi_rvalid;
reg         s_axi_rready;
wire [31:0] m_axis_tdata;
wire        m_axis_tvalid;
reg         m_axis_tready;
wire        m_axis_tlast;
wire [3:0]  m_axis_tkeep;

integer fail_count;
integer current_test;
integer test_failed [1:5];
integer stream_seen;
integer i;
reg [31:0] read_data;
reg [31:0] hold_data;
reg        hold_last;
reg [31:0] bp_before;
reg [31:0] bp_after;

stream_gen dut (
    .s_axi_aclk       (s_axi_aclk),
    .s_axi_aresetn    (s_axi_aresetn),
    .s_axi_awaddr     (s_axi_awaddr),
    .s_axi_awvalid    (s_axi_awvalid),
    .s_axi_awready    (s_axi_awready),
    .s_axi_wdata      (s_axi_wdata),
    .s_axi_wstrb      (s_axi_wstrb),
    .s_axi_wvalid     (s_axi_wvalid),
    .s_axi_wready     (s_axi_wready),
    .s_axi_bresp      (s_axi_bresp),
    .s_axi_bvalid     (s_axi_bvalid),
    .s_axi_bready     (s_axi_bready),
    .s_axi_araddr     (s_axi_araddr),
    .s_axi_arvalid    (s_axi_arvalid),
    .s_axi_arready    (s_axi_arready),
    .s_axi_rdata      (s_axi_rdata),
    .s_axi_rresp      (s_axi_rresp),
    .s_axi_rvalid     (s_axi_rvalid),
    .s_axi_rready     (s_axi_rready),
    .m_axis_tdata     (m_axis_tdata),
    .m_axis_tvalid    (m_axis_tvalid),
    .m_axis_tready    (m_axis_tready),
    .m_axis_tlast     (m_axis_tlast),
    .m_axis_tkeep     (m_axis_tkeep)
);

always #5 s_axi_aclk = ~s_axi_aclk;

task check32;
    input [127:0] label;
    input [31:0]  actual;
    input [31:0]  expected;
    begin
        if (actual !== expected) begin
            $display("FAIL T%0d %0s actual=%h expected=%h", current_test,
                     label, actual, expected);
            test_failed[current_test] = 1;
            fail_count = fail_count + 1;
        end
    end
endtask

task check1;
    input [127:0] label;
    input         actual;
    input         expected;
    begin
        if (actual !== expected) begin
            $display("FAIL T%0d %0s actual=%b expected=%b", current_test,
                     label, actual, expected);
            test_failed[current_test] = 1;
            fail_count = fail_count + 1;
        end
    end
endtask

task axi_write;
    input [4:0]  address;
    input [31:0] data;
    integer      guard;
    reg          aw_done;
    reg          w_done;
    begin
        @(negedge s_axi_aclk);
        s_axi_awaddr  = address;
        s_axi_awvalid = 1'b1;
        s_axi_wdata   = data;
        s_axi_wstrb   = 4'b1111;
        s_axi_wvalid  = 1'b1;
        s_axi_bready  = 1'b0;

        aw_done = 1'b0;
        w_done  = 1'b0;
        guard   = 0;
        while (!(aw_done && w_done)) begin
            @(posedge s_axi_aclk);
            if (s_axi_awready) begin
                aw_done = 1'b1;
            end
            if (s_axi_wready) begin
                w_done = 1'b1;
            end
            guard = guard + 1;
            if (guard > 1000) begin
                $display("FAIL T%0d AXI-Lite 写地址或写数据握手超时", current_test);
                test_failed[current_test] = 1;
                fail_count = fail_count + 1;
                disable axi_write;
            end
        end

        @(negedge s_axi_aclk);
        s_axi_awvalid = 1'b0;
        s_axi_wvalid  = 1'b0;

        guard = 0;
        while (!s_axi_bvalid) begin
            @(posedge s_axi_aclk);
            guard = guard + 1;
            if (guard > 1000) begin
                $display("FAIL T%0d AXI-Lite 写响应等待超时", current_test);
                test_failed[current_test] = 1;
                fail_count = fail_count + 1;
                disable axi_write;
            end
        end

        check32("BRESP", {30'd0, s_axi_bresp}, 32'h0000_0000);
        @(negedge s_axi_aclk);
        s_axi_bready = 1'b1;
        @(posedge s_axi_aclk);
        @(negedge s_axi_aclk);
        s_axi_bready = 1'b0;
    end
endtask

task axi_read;
    input  [4:0]  address;
    output [31:0] data;
    integer       guard;
    begin
        @(negedge s_axi_aclk);
        s_axi_araddr  = address;
        s_axi_arvalid = 1'b1;
        s_axi_rready  = 1'b0;

        guard = 0;
        while (!s_axi_arready) begin
            @(posedge s_axi_aclk);
            guard = guard + 1;
            if (guard > 1000) begin
                $display("FAIL T%0d AXI-Lite 读地址握手超时", current_test);
                test_failed[current_test] = 1;
                fail_count = fail_count + 1;
                disable axi_read;
            end
        end

        @(posedge s_axi_aclk);
        @(negedge s_axi_aclk);
        s_axi_arvalid = 1'b0;

        guard = 0;
        while (!s_axi_rvalid) begin
            @(posedge s_axi_aclk);
            guard = guard + 1;
            if (guard > 1000) begin
                $display("FAIL T%0d AXI-Lite 读响应等待超时", current_test);
                test_failed[current_test] = 1;
                fail_count = fail_count + 1;
                disable axi_read;
            end
        end

        data = s_axi_rdata;
        check32("RRESP", {30'd0, s_axi_rresp}, 32'h0000_0000);
        @(negedge s_axi_aclk);
        s_axi_rready = 1'b1;
        @(posedge s_axi_aclk);
        @(negedge s_axi_aclk);
        s_axi_rready = 1'b0;
    end
endtask

initial begin
    s_axi_aclk    = 1'b0;
    s_axi_aresetn = 1'b0;
    s_axi_awaddr  = 5'd0;
    s_axi_awvalid = 1'b0;
    s_axi_wdata   = 32'd0;
    s_axi_wstrb   = 4'd0;
    s_axi_wvalid  = 1'b0;
    s_axi_bready  = 1'b0;
    s_axi_araddr  = 5'd0;
    s_axi_arvalid = 1'b0;
    s_axi_rready  = 1'b0;
    m_axis_tready = 1'b0;
    fail_count    = 0;

    for (i = 1; i <= 5; i = i + 1) begin
        test_failed[i] = 0;
    end

    repeat (4) @(posedge s_axi_aclk);
    @(negedge s_axi_aclk);
    s_axi_aresetn = 1'b1;
    repeat (2) @(posedge s_axi_aclk);

    // 测试 1：复位默认值
    current_test = 1;
    axi_read(REG_CTRL, read_data);
    check32("CTRL 复位值", read_data, 32'd0);
    axi_read(REG_STATUS, read_data);
    check32("STATUS 复位值", read_data, 32'd0);
    axi_read(REG_PACKET_LEN, read_data);
    check32("PACKET_LEN 复位值", read_data, 32'd16);
    axi_read(REG_RATE_DIV, read_data);
    check32("RATE_DIV 复位值", read_data, 32'd0);
    axi_read(REG_WORD_COUNT, read_data);
    check32("WORD_COUNT 复位值", read_data, 32'd0);
    axi_read(REG_PACKET_COUNT, read_data);
    check32("PACKET_COUNT 复位值", read_data, 32'd0);
    axi_read(REG_BACKPRESSURE_COUNT, read_data);
    check32("BACKPRESSURE_COUNT 复位值", read_data, 32'd0);
    axi_read(REG_VERSION, read_data);
    check32("VERSION 固定值", read_data, 32'h0001_0000);
    check1("TVALID 复位值", m_axis_tvalid, 1'b0);
    check32("TKEEP 固定值", {28'd0, m_axis_tkeep}, 32'h0000_000f);

    // 测试 2：AXI-Lite 基本读写
    current_test = 2;
    axi_write(REG_PACKET_LEN, 32'd4);
    axi_read(REG_PACKET_LEN, read_data);
    check32("PACKET_LEN 写入 4", read_data, 32'd4);
    axi_write(REG_RATE_DIV, 32'd0);
    axi_read(REG_RATE_DIV, read_data);
    check32("RATE_DIV 写入 0", read_data, 32'd0);
    axi_write(REG_CTRL, 32'd1);
    axi_read(REG_CTRL, read_data);
    check32("CTRL ENABLE", read_data, 32'd1);
    repeat (2) @(posedge s_axi_aclk);
    axi_write(REG_PACKET_LEN, 32'd0);
    axi_read(REG_PACKET_LEN, read_data);
    check32("PACKET_LEN 零值强制为 1", read_data, 32'd1);

    // 为连续输出测试重新配置并清除运行状态
    axi_write(REG_CTRL, 32'd0);
    axi_write(REG_PACKET_LEN, 32'd4);
    axi_write(REG_RATE_DIV, 32'd0);
    axi_write(REG_CTRL, 32'd2);
    repeat (2) @(posedge s_axi_aclk);

    // 测试 3：AXI-Stream 连续输出和 TLAST
    current_test = 3;
    m_axis_tready = 1'b0;
    axi_write(REG_CTRL, 32'd1);
    m_axis_tready = 1'b1;
    stream_seen = 0;
    while (stream_seen < 8) begin
        @(posedge s_axi_aclk);
        if (m_axis_tvalid && m_axis_tready) begin
            check32("连续 counter", m_axis_tdata, stream_seen);
            if (stream_seen == 3 || stream_seen == 7) begin
                check1("包尾 TLAST", m_axis_tlast, 1'b1);
            end
            else begin
                check1("非包尾 TLAST", m_axis_tlast, 1'b0);
            end
            stream_seen = stream_seen + 1;
        end
    end
    @(negedge s_axi_aclk);
    m_axis_tready = 1'b0;
    axi_read(REG_WORD_COUNT, read_data);
    check32("WORD_COUNT 八个 beat", read_data, 32'd8);
    axi_read(REG_PACKET_COUNT, read_data);
    check32("PACKET_COUNT 两个 packet", read_data, 32'd2);

    // 测试 4：背压期间保持输出且只累计三个周期
    current_test = 4;
    @(negedge s_axi_aclk);
    m_axis_tready = 1'b1;
    @(posedge s_axi_aclk);
    axi_read(REG_BACKPRESSURE_COUNT, bp_before);
    @(negedge s_axi_aclk);
    hold_data = m_axis_tdata;
    hold_last = m_axis_tlast;
    m_axis_tready = 1'b0;
    for (i = 0; i < 3; i = i + 1) begin
        @(posedge s_axi_aclk);
        check1("背压期间 TVALID", m_axis_tvalid, 1'b1);
        check32("背压期间 TDATA", m_axis_tdata, hold_data);
        check1("背压期间 TLAST", m_axis_tlast, hold_last);
    end
    @(negedge s_axi_aclk);
    m_axis_tready = 1'b1;
    @(posedge s_axi_aclk);
    check32("解除背压后的数据", m_axis_tdata, hold_data);
    axi_read(REG_BACKPRESSURE_COUNT, bp_after);
    check32("背压累计增加三周期", bp_after, bp_before + 32'd3);

    // 测试 5：软件复位和重新使能
    current_test = 5;
    repeat (3) @(posedge s_axi_aclk);
    axi_write(REG_CTRL, 32'd2);
    repeat (2) @(posedge s_axi_aclk);
    axi_read(REG_CTRL, read_data);
    check32("软件复位 CTRL", read_data, 32'd0);
    axi_read(REG_PACKET_LEN, read_data);
    check32("软件复位保留 PACKET_LEN", read_data, 32'd4);
    axi_read(REG_RATE_DIV, read_data);
    check32("软件复位保留 RATE_DIV", read_data, 32'd0);
    axi_read(REG_WORD_COUNT, read_data);
    check32("软件复位 WORD_COUNT", read_data, 32'd0);
    axi_read(REG_PACKET_COUNT, read_data);
    check32("软件复位 PACKET_COUNT", read_data, 32'd0);
    axi_read(REG_BACKPRESSURE_COUNT, read_data);
    check32("软件复位 BACKPRESSURE_COUNT", read_data, 32'd0);
    check1("软件复位 TVALID", m_axis_tvalid, 1'b0);

    m_axis_tready = 1'b0;
    axi_write(REG_CTRL, 32'd1);
    m_axis_tready = 1'b1;
    stream_seen = 0;
    while (stream_seen == 0) begin
        @(posedge s_axi_aclk);
        if (m_axis_tvalid && m_axis_tready) begin
            check32("软件复位后 counter", m_axis_tdata, 32'd0);
            stream_seen = 1;
        end
    end

    for (i = 1; i <= 5; i = i + 1) begin
        if (test_failed[i] == 0) begin
            $display("TEST %0d: PASS", i);
        end
        else begin
            $display("TEST %0d: FAIL", i);
        end
    end

    if (fail_count == 0) begin
        $display("TEST PASS");
    end
    else begin
        $display("TEST FAIL (%0d failures)", fail_count);
    end
    $finish;
end

initial begin
    #200000;
    $display("TEST FAIL (仿真超时)");
    $finish;
end

endmodule
