set project_dir [file normalize [file join [file dirname [info script]] ..]]
set project_file [file join $project_dir zynq_stream_dma.xpr]

open_project $project_file
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1

set run [get_runs impl_1]
set status [get_property STATUS $run]
puts "impl_1 status: $status"
if {![string match "*Complete*" $status]} {
    close_project
    exit 1
}

set bit_file [file join [get_property DIRECTORY $run] system_wrapper.bit]
if {![file exists $bit_file]} {
    puts "ERROR: bitstream not found: $bit_file"
    close_project
    exit 1
}

puts "OUTPUT: $bit_file"
close_project
