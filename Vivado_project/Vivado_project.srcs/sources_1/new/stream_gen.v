`timescale 1ns / 1ps

module stream_gen (
    input         s_axi_aclk,
    input         s_axi_aresetn,

    // AXI-Lite 写地址通道
    input  [4:0]  s_axi_awaddr,
    input         s_axi_awvalid,
    output        s_axi_awready,

    // AXI-Lite 写数据通道
    input  [31:0] s_axi_wdata,
    input  [3:0]  s_axi_wstrb,
    input         s_axi_wvalid,
    output        s_axi_wready,

    // AXI-Lite 写响应通道
    output [1:0]  s_axi_bresp,
    output        s_axi_bvalid,
    input         s_axi_bready,

    // AXI-Lite 读地址通道
    input  [4:0]  s_axi_araddr,
    input         s_axi_arvalid,
    output        s_axi_arready,

    // AXI-Lite 读数据通道
    output [31:0] s_axi_rdata,
    output [1:0]  s_axi_rresp,
    output        s_axi_rvalid,
    input         s_axi_rready,

    // AXI-Stream 主机输出接口
    output [31:0] m_axis_tdata,
    output        m_axis_tvalid,
    input         m_axis_tready,
    output        m_axis_tlast,
    output [3:0]  m_axis_tkeep
);

// 寄存器地址偏移
localparam [4:0] REG_CTRL               = 5'h00;
localparam [4:0] REG_STATUS             = 5'h04;
localparam [4:0] REG_PACKET_LEN         = 5'h08;
localparam [4:0] REG_RATE_DIV           = 5'h0c;
localparam [4:0] REG_WORD_COUNT         = 5'h10;
localparam [4:0] REG_PACKET_COUNT       = 5'h14;
localparam [4:0] REG_BACKPRESSURE_COUNT = 5'h18;
localparam [4:0] REG_VERSION            = 5'h1c;

// 固定寄存器值
localparam [31:0] VERSION_VALUE = 32'h0001_0000;

// 内部寄存器
reg [31:0] ctrl_reg;               // bit0：使能，bit1：软件复位命令，不保存
reg [31:0] status_reg;             // bit0：运行中，bit1：错误，bit2：背压
reg [31:0] packet_len_reg;         // 每个数据包包含的 32 位数据数量
reg [31:0] rate_div_reg;           // 数据流输出速率分频值
reg [31:0] word_count_reg;         // 已成功输出的数据总数
reg [31:0] packet_count_reg;       // 已成功输出的数据包总数
reg [31:0] backpressure_count_reg; // 因 tready 拉低而阻塞的时钟周期数
reg        stream_reset_reg;       // 通知 AXI-Stream 逻辑执行一次软件复位

// AXI-Lite 写通道
reg       s_axi_awready_reg;
reg       s_axi_wready_reg;
reg [1:0] s_axi_bresp_reg;
reg       s_axi_bvalid_reg;

// AXI-Lite 读通道
reg        s_axi_arready_reg;
reg [31:0] s_axi_rdata_reg;
reg [1:0]  s_axi_rresp_reg;
reg        s_axi_rvalid_reg;

// AXI-Stream 输出
reg [31:0] m_axis_tdata_reg;
reg        m_axis_tvalid_reg;
reg        m_axis_tlast_reg;
reg [31:0] seq_counter_reg;       // AXI-Stream 递增数据计数器
reg [31:0] packet_word_index_reg; // 当前数据在包内的下标
reg [31:0] rate_count_reg;        // 两次有效传输之间的等待周期计数器

// 内部寄存器连接到模块输出端口
assign s_axi_awready = s_axi_awready_reg;
assign s_axi_wready  = s_axi_wready_reg;
assign s_axi_bresp   = s_axi_bresp_reg;
assign s_axi_bvalid  = s_axi_bvalid_reg;

assign s_axi_arready = s_axi_arready_reg;
assign s_axi_rdata   = s_axi_rdata_reg;
assign s_axi_rresp   = s_axi_rresp_reg;
assign s_axi_rvalid  = s_axi_rvalid_reg;

assign m_axis_tdata  = m_axis_tdata_reg;
assign m_axis_tvalid = m_axis_tvalid_reg;
assign m_axis_tlast  = m_axis_tlast_reg;

assign m_axis_tkeep  = 4'b1111;

// 写通道暂存寄存器
reg [4:0]  s_axi_awaddr_reg;  // 暂存已接收的写地址
reg        s_axi_awvalid_reg; // 标记内部已经保存写地址
reg [31:0] s_axi_wdata_reg;   // 暂存已接收的写数据
reg [3:0]  s_axi_wstrb_reg;   // 暂存写数据对应的字节使能
reg        s_axi_wvalid_reg;  // 标记内部已经保存写数据

// AXI-Lite 写通道
always @(posedge s_axi_aclk) begin
    if (!s_axi_aresetn) begin
        ctrl_reg         <= 32'd0;
        packet_len_reg   <= 32'd16;
        rate_div_reg     <= 32'd0;
        stream_reset_reg <= 1'b0;

        s_axi_awready_reg <= 1'b0;
        s_axi_wready_reg  <= 1'b0;
        s_axi_bresp_reg   <= 2'b00;
        s_axi_bvalid_reg  <= 1'b0;

        s_axi_awaddr_reg  <= 5'd0;
        s_axi_awvalid_reg <= 1'b0;
        s_axi_wdata_reg   <= 32'd0;
        s_axi_wstrb_reg   <= 4'b0000;
        s_axi_wvalid_reg  <= 1'b0;
    end
    else begin
        // 软件复位请求默认只保持一个时钟周期
        stream_reset_reg <= 1'b0;

        // AW 写地址接收就绪控制
        if (!s_axi_awvalid_reg && !s_axi_bvalid_reg) begin
            s_axi_awready_reg <= 1'b1;
        end
        else begin
            s_axi_awready_reg <= 1'b0;
        end

        // AW 写地址握手成功后保存地址
        if (s_axi_awvalid && s_axi_awready_reg) begin
            s_axi_awaddr_reg  <= s_axi_awaddr;
            s_axi_awvalid_reg <= 1'b1;
            s_axi_awready_reg <= 1'b0;
        end

        // W 写数据接收就绪控制
        if (!s_axi_wvalid_reg && !s_axi_bvalid_reg) begin
            s_axi_wready_reg <= 1'b1;
        end
        else begin
            s_axi_wready_reg <= 1'b0;
        end

        // W 写数据握手成功后保存数据
        if (s_axi_wvalid && s_axi_wready_reg) begin
            s_axi_wdata_reg  <= s_axi_wdata;
            s_axi_wstrb_reg  <= s_axi_wstrb;
            s_axi_wvalid_reg <= 1'b1;
            s_axi_wready_reg <= 1'b0;
        end

        // B 通道握手成功，PS 已经接收写响应
        if (s_axi_bvalid_reg && s_axi_bready) begin
            s_axi_bvalid_reg <= 1'b0;
        end

        // 写地址和写数据已经收齐，开始执行写操作并产生写响应
        if (s_axi_awvalid_reg && s_axi_wvalid_reg && !s_axi_bvalid_reg) begin
            if (s_axi_wstrb_reg == 4'b1111) begin
                case (s_axi_awaddr_reg)
                    REG_PACKET_LEN: begin
                        if (s_axi_wdata_reg == 32'd0) begin
                            packet_len_reg <= 32'd1;
                        end
                        else begin
                            packet_len_reg <= s_axi_wdata_reg;
                        end
                    end

                    REG_RATE_DIV: begin
                        rate_div_reg <= s_axi_wdata_reg;
                    end

                    REG_CTRL: begin
                        // 软件复位优先于数据流使能
                        if (s_axi_wdata_reg[1]) begin
                            ctrl_reg         <= 32'd0;
                            stream_reset_reg <= 1'b1;
                        end
                        else begin
                            ctrl_reg <= {31'd0, s_axi_wdata_reg[0]};
                        end
                    end

                    default: begin
                    end
                endcase
            end

            // 产生写响应
            s_axi_bvalid_reg  <= 1'b1;
            s_axi_bresp_reg   <= 2'b00;
            s_axi_awvalid_reg <= 1'b0;
            s_axi_wvalid_reg  <= 1'b0;
        end
    end
end

// AXI-Lite 读通道
always @(posedge s_axi_aclk) begin
    if (!s_axi_aresetn) begin
        s_axi_arready_reg <= 1'b0;
        s_axi_rdata_reg   <= 32'd0;
        s_axi_rresp_reg   <= 2'b00;
        s_axi_rvalid_reg  <= 1'b0;
    end
    else begin
        // 没有未完成的读响应时，可以接收新的读地址
        if (!s_axi_rvalid_reg) begin
            s_axi_arready_reg <= 1'b1;
        end
        else begin
            s_axi_arready_reg <= 1'b0;
        end

        // AR 通道握手成功，生成对应的读数据
        if (s_axi_arvalid && s_axi_arready_reg) begin
            s_axi_arready_reg <= 1'b0;
            s_axi_rresp_reg   <= 2'b00;
            s_axi_rvalid_reg  <= 1'b1;

            case (s_axi_araddr)
                REG_CTRL: begin
                    s_axi_rdata_reg <= ctrl_reg;
                end

                REG_STATUS: begin
                    s_axi_rdata_reg <= status_reg;
                end

                REG_PACKET_LEN: begin
                    s_axi_rdata_reg <= packet_len_reg;
                end

                REG_RATE_DIV: begin
                    s_axi_rdata_reg <= rate_div_reg;
                end

                REG_WORD_COUNT: begin
                    s_axi_rdata_reg <= word_count_reg;
                end

                REG_PACKET_COUNT: begin
                    s_axi_rdata_reg <= packet_count_reg;
                end

                REG_BACKPRESSURE_COUNT: begin
                    s_axi_rdata_reg <= backpressure_count_reg;
                end

                REG_VERSION: begin
                    s_axi_rdata_reg <= VERSION_VALUE;
                end

                default: begin
                    s_axi_rdata_reg <= 32'd0;
                end
            endcase
        end

        // R 通道握手成功，PS 已经接收读数据
        if (s_axi_rvalid_reg && s_axi_rready) begin
            s_axi_rvalid_reg <= 1'b0;
        end
    end
end

// AXI-Stream 递增数据输出
always @(posedge s_axi_aclk) begin
    if (!s_axi_aresetn || stream_reset_reg) begin
        seq_counter_reg        <= 32'd0;
        packet_word_index_reg  <= 32'd0;
        rate_count_reg         <= 32'd0;
        word_count_reg         <= 32'd0;
        packet_count_reg       <= 32'd0;
        backpressure_count_reg <= 32'd0;
        status_reg             <= 32'd0;
        m_axis_tdata_reg       <= 32'd0;
        m_axis_tvalid_reg      <= 1'b0;
        m_axis_tlast_reg       <= 1'b0;
    end
    else begin
        // 状态寄存器：运行、错误、当前背压状态
        status_reg <= {
            29'd0,
            m_axis_tvalid_reg && !m_axis_tready,
            1'b0,
            ctrl_reg[0]
        };

        // 每持续一个背压周期，累计计数增加一次
        if (m_axis_tvalid_reg && !m_axis_tready) begin
            backpressure_count_reg <= backpressure_count_reg + 1'b1;
        end

        // 握手成功后推进计数器，并准备下一个数据
        if (m_axis_tvalid_reg && m_axis_tready) begin
            seq_counter_reg   <= seq_counter_reg + 1'b1;
            word_count_reg    <= word_count_reg + 1'b1;
            m_axis_tdata_reg  <= seq_counter_reg + 1'b1;

            // 禁用时完成当前握手后停止，不再提出下一份有效数据
            if (!ctrl_reg[0]) begin
                rate_count_reg    <= 32'd0;
                m_axis_tvalid_reg <= 1'b0;
            end
            // RATE_DIV 为 N 时，在本次传输后等待 N 个时钟周期
            else if (rate_div_reg == 32'd0) begin
                m_axis_tvalid_reg <= 1'b1;
            end
            else begin
                rate_count_reg    <= rate_div_reg;
                m_axis_tvalid_reg <= 1'b0;
            end

            // 当前包结束后，下一份数据从新包下标 0 开始
            if (m_axis_tlast_reg) begin
                packet_word_index_reg <= 32'd0;
                packet_count_reg      <= packet_count_reg + 1'b1;

                if (packet_len_reg == 32'd1) begin
                    m_axis_tlast_reg <= 1'b1;
                end
                else begin
                    m_axis_tlast_reg <= 1'b0;
                end
            end
            else begin
                packet_word_index_reg <= packet_word_index_reg + 1'b1;

                if (packet_word_index_reg + 1'b1 == packet_len_reg - 1'b1) begin
                    m_axis_tlast_reg <= 1'b1;
                end
                else begin
                    m_axis_tlast_reg <= 1'b0;
                end
            end
        end
        // 禁用时不再产生新数据；已提出的数据保持到握手完成
        else if (!ctrl_reg[0]) begin
            rate_count_reg <= 32'd0;
        end
        // 节流等待期间不输出有效数据
        else if (rate_count_reg != 32'd0) begin
            m_axis_tvalid_reg <= 1'b0;

            // 倒计时结束后，恢复已经准备好的下一份数据
            if (rate_count_reg == 32'd1) begin
                rate_count_reg    <= 32'd0;
                m_axis_tvalid_reg <= 1'b1;
            end
            else begin
                rate_count_reg <= rate_count_reg - 1'b1;
            end
        end
        // 当前没有有效数据时，准备第一个数据
        else if (!m_axis_tvalid_reg) begin
            m_axis_tdata_reg  <= seq_counter_reg;
            m_axis_tvalid_reg <= 1'b1;

            if (packet_word_index_reg == packet_len_reg - 1'b1) begin
                m_axis_tlast_reg <= 1'b1;
            end
            else begin
                m_axis_tlast_reg <= 1'b0;
            end
        end
    end
end

endmodule
