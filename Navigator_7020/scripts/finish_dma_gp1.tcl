# Run in the open Navigator_7020 system.bd. No synthesis or IP regeneration.
if {[current_bd_design -quiet] ne "system"} {
    error "Open system.bd in Navigator_7020 before sourcing this script."
}
current_bd_instance /

# GP1 exposes 0x80000000..0xBFFFFFFF, unlike GP0.
set_property CONFIG.PCW_USE_M_AXI_GP1 {1} [get_bd_cells processing_system7_0]
if {[llength [get_bd_cells -quiet dma_ctrl_intercon]] == 0} {
    create_bd_cell -type ip -vlnv xilinx.com:ip:axi_interconnect:2.1 dma_ctrl_intercon
}
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {2}] \
    [get_bd_cells dma_ctrl_intercon]

# Keep connections already made in the GUI; add missing control connections.
foreach {source destination} {
    processing_system7_0/M_AXI_GP1 dma_ctrl_intercon/S00_AXI
    dma_ctrl_intercon/M00_AXI axi_dma_0/S_AXI_LITE
    dma_ctrl_intercon/M01_AXI stream_gen_0/s_axi
} {
    set source_pin [get_bd_intf_pins $source]
    set destination_pin [get_bd_intf_pins $destination]
    set source_net [get_bd_intf_nets -quiet -of_objects $source_pin]
    set destination_net [get_bd_intf_nets -quiet -of_objects $destination_pin]
    if {[llength $destination_net] == 0} {
        connect_bd_intf_net $source_pin $destination_pin
    } elseif {$source_net ne $destination_net} {
        error "Unexpected existing connection at $destination; left unchanged."
    }
}

foreach pin {
    processing_system7_0/M_AXI_GP1_ACLK
    dma_ctrl_intercon/ACLK
    dma_ctrl_intercon/S00_ACLK
    dma_ctrl_intercon/M00_ACLK
    dma_ctrl_intercon/M01_ACLK
    axi_dma_0/s_axi_lite_aclk
    axi_dma_0/m_axi_s2mm_aclk
    stream_gen_0/s_axi_aclk
    lcd_out/dma_aclk
} {
    if {[llength [get_bd_nets -quiet -of_objects [get_bd_pins $pin]]] == 0} {
        connect_bd_net [get_bd_pins processing_system7_0/FCLK_CLK0] [get_bd_pins $pin]
    }
}

if {[llength [get_bd_nets -quiet -of_objects [get_bd_pins dma_ctrl_intercon/ARESETN]]] == 0} {
    connect_bd_net [get_bd_pins rst_ps7_0_100M/interconnect_aresetn] \
        [get_bd_pins dma_ctrl_intercon/ARESETN]
}
foreach pin {
    dma_ctrl_intercon/S00_ARESETN
    dma_ctrl_intercon/M00_ARESETN
    dma_ctrl_intercon/M01_ARESETN
    axi_dma_0/axi_resetn
    stream_gen_0/s_axi_aresetn
    lcd_out/dma_aresetn
} {
    if {[llength [get_bd_nets -quiet -of_objects [get_bd_pins $pin]]] == 0} {
        connect_bd_net [get_bd_pins rst_ps7_0_100M/peripheral_aresetn] [get_bd_pins $pin]
    }
}

set_property CONFIG.NUM_PORTS {11} [get_bd_cells xlconcat_0]
if {[llength [get_bd_nets -quiet -of_objects [get_bd_pins xlconcat_0/In10]]] == 0} {
    connect_bd_net [get_bd_pins axi_dma_0/s2mm_introut] [get_bd_pins xlconcat_0/In10]
}

assign_bd_address -offset 0x80400000 -range 64K \
    -target_address_space [get_bd_addr_spaces processing_system7_0/Data] \
    [get_bd_addr_segs axi_dma_0/S_AXI_LITE/Reg]

assign_bd_address -offset 0x83CB0000 -range 64K \
    -target_address_space [get_bd_addr_spaces processing_system7_0/Data] \
    [get_bd_addr_segs stream_gen_0/s_axi/reg0]

assign_bd_address -offset 0x00000000 -range 1G \
    -target_address_space [get_bd_addr_spaces axi_dma_0/Data_S2MM] \
    [get_bd_addr_segs processing_system7_0/S_AXI_HP0/HP0_DDR_LOWOCM]

save_bd_design
puts "DMA GP1 connections saved: DMA=0x80400000, stream_gen=0x83CB0000."
