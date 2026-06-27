`timescale 1ns / 1ps

module stream_gen (
    input              s_axi_aclk,
    input              s_axi_aresetn,

    // AXI-Lite 写地址通道
    input  [4:0]       s_axi_awaddr,
    input              s_axi_awvalid,
    output             s_axi_awready,

    // AXI-Lite 写数据通道
    input  [31:0]      s_axi_wdata,
    input  [3:0]       s_axi_wstrb,
    input              s_axi_wvalid,
    output             s_axi_wready,

    // AXI-Lite 写响应通道
    output [1:0]       s_axi_bresp,
    output             s_axi_bvalid,
    input              s_axi_bready,

    // AXI-Lite 读地址通道
    input  [4:0]       s_axi_araddr,
    input              s_axi_arvalid,
    output             s_axi_arready,

    // AXI-Lite 读数据通道
    output [31:0]      s_axi_rdata,
    output [1:0]       s_axi_rresp,
    output             s_axi_rvalid,
    input              s_axi_rready,

    // AXI-Stream 主机输出接口
    output [31:0]      m_axis_tdata,
    output             m_axis_tvalid,
    input              m_axis_tready,
    output             m_axis_tlast,
    output [3:0]       m_axis_tkeep
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
reg [31:0] ctrl_reg;               // bit0: 使能，bit1: 复位
reg [31:0] status_reg;             // bit0: 运行中，bit1: 错误，bit2: 背压
reg [31:0] packet_len_reg;         // 每包包含的 32-bit word 数量
reg [31:0] rate_div_reg;           // 数据流输出速率分频配置
reg [31:0] word_count_reg;         // 已输出 word 总数
reg [31:0] packet_count_reg;       // 已输出 packet 总数
reg [31:0] backpressure_count_reg; // 因 tready 拉低导致阻塞的周期数

// AXI-Lite 输出寄存器
reg        s_axi_awready_reg;
reg        s_axi_wready_reg;
reg [1:0]  s_axi_bresp_reg;
reg        s_axi_bvalid_reg;
reg        s_axi_arready_reg;
reg [31:0] s_axi_rdata_reg;
reg [1:0]  s_axi_rresp_reg;
reg        s_axi_rvalid_reg;

// AXI-Stream 输出寄存器
reg [31:0] m_axis_tdata_reg;
reg        m_axis_tvalid_reg;
reg        m_axis_tlast_reg;
reg [3:0]  m_axis_tkeep_reg;

// 输出端口连接
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
assign m_axis_tkeep  = m_axis_tkeep_reg;

always @(posedge s_axi_aclk) begin
    // 初始化
    if (!s_axi_aresetn) begin
        ctrl_reg                 <= 0;
        status_reg               <= 0;
        packet_len_reg           <= 32'd16;
        rate_div_reg             <= 0;
        word_count_reg           <= 0;
        packet_count_reg         <= 0;
        backpressure_count_reg   <= 0;

        s_axi_awready_reg        <= 0;
        s_axi_wready_reg         <= 0;
        s_axi_bresp_reg          <= 2'b00;
        s_axi_bvalid_reg         <= 0;
        s_axi_arready_reg        <= 0;
        s_axi_rdata_reg          <= 0;
        s_axi_rresp_reg          <= 2'b00;
        s_axi_rvalid_reg         <= 0;

        m_axis_tdata_reg         <= 0;
        m_axis_tvalid_reg        <= 0;
        m_axis_tlast_reg         <= 0;
        m_axis_tkeep_reg         <= 4'b1111;
    end

    else begin
        // 非复位时的 AXI-Lite write 逻辑
        s_axi_awready_reg <= 0;
        s_axi_wready_reg  <= 0;

        if (s_axi_bvalid_reg && s_axi_bready) begin
            s_axi_bvalid_reg <= 0;
        end

        if (!s_axi_bvalid_reg && s_axi_wvalid && s_axi_awvalid) begin
            s_axi_wready_reg  <= 1;
            s_axi_awready_reg <= 1;
            s_axi_bvalid_reg  <= 1;
            s_axi_bresp_reg   <= 2'b00;

            if (s_axi_wstrb == 4'b1111) begin
                case (s_axi_awaddr)
                    REG_CTRL: begin
                        if (s_axi_wdata[1]) begin
                            ctrl_reg               <= 32'd0;
                            status_reg             <= 32'd0;
                            packet_len_reg         <= 32'd16;
                            rate_div_reg           <= 32'd0;
                            word_count_reg         <= 32'd0;
                            packet_count_reg       <= 32'd0;
                            backpressure_count_reg <= 32'd0;

                            m_axis_tdata_reg       <= 32'd0;
                            m_axis_tvalid_reg      <= 1'b0;
                            m_axis_tlast_reg       <= 1'b0;
                            m_axis_tkeep_reg       <= 4'b1111;
                        end else begin
                            ctrl_reg <= {30'd0, s_axi_wdata[1:0]};
                        end
                    end

                    REG_PACKET_LEN: begin
                        if (s_axi_wdata == 32'd0) begin
                            packet_len_reg <= 32'd1;
                        end else begin
                            packet_len_reg <= s_axi_wdata;
                        end
                    end

                    REG_RATE_DIV: begin
                        rate_div_reg <= s_axi_wdata;
                    end

                    default: begin
                    end
                endcase
            end
        end

        // 非复位时的 AXI-Lite read 逻辑
        s_axi_arready_reg <= 0;
        if (s_axi_rvalid_reg && s_axi_rready) begin
            s_axi_rvalid_reg <= 0;
        end

        if (!s_axi_rvalid_reg && s_axi_arvalid) begin
            s_axi_arready_reg <= 1;
            s_axi_rvalid_reg  <= 1;
            s_axi_rresp_reg   <= 2'b00;

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
    end

end

endmodule
