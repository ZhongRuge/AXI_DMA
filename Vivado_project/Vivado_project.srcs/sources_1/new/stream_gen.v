`timescale 1ns / 1ps

module stream_gen (
    input              s_axi_aclk,
    input              s_axi_aresetn,

    // AXI-Lite write address
    input  [4:0]       s_axi_awaddr,
    input              s_axi_awvalid,
    output             s_axi_awready,

    // AXI-Lite write data
    input  [31:0]      s_axi_wdata,
    input  [3:0]       s_axi_wstrb,
    input              s_axi_wvalid,
    output             s_axi_wready,

    // AXI-Lite write response
    output [1:0]       s_axi_bresp,
    output             s_axi_bvalid,
    input              s_axi_bready,

    // AXI-Lite read address
    input  [4:0]       s_axi_araddr,
    input              s_axi_arvalid,
    output             s_axi_arready,

    // AXI-Lite read data
    output [31:0]      s_axi_rdata,
    output [1:0]       s_axi_rresp,
    output             s_axi_rvalid,
    input              s_axi_rready,

    // AXI-Stream master
    output [31:0]      m_axis_tdata,
    output             m_axis_tvalid,
    input              m_axis_tready,
    output             m_axis_tlast,
    output [3:0]       m_axis_tkeep
);

// Register offsets
localparam [4:0] REG_CTRL               = 5'h00;
localparam [4:0] REG_STATUS             = 5'h04;
localparam [4:0] REG_PACKET_LEN         = 5'h08;
localparam [4:0] REG_RATE_DIV           = 5'h0c;
localparam [4:0] REG_WORD_COUNT         = 5'h10;
localparam [4:0] REG_PACKET_COUNT       = 5'h14;
localparam [4:0] REG_BACKPRESSURE_COUNT = 5'h18;
localparam [4:0] REG_VERSION            = 5'h1c;

// Fixed register values
localparam [31:0] VERSION_VALUE = 32'h0001_0000;

// Internal registers
reg [31:0] ctrl_reg;               // bit0: enable, bit1: reset
reg [31:0] status_reg;             // bit0: running, bit1: error, bit2: backpressure
reg [31:0] packet_len_reg;         // packet length in 32-bit words
reg [31:0] rate_div_reg;           // stream output rate divider
reg [31:0] word_count_reg;         // total output word counter
reg [31:0] packet_count_reg;       // total output packet counter
reg [31:0] backpressure_count_reg; // cycles blocked by tready deassertion

endmodule
