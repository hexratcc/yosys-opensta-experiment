create_clock -period 10 -name clk [get_ports clk]
set_input_delay 1 -clock clk [all_inputs -no_clocks]
set_output_delay 1 -clock clk [all_outputs]
puts "insts: [sta::network_leaf_instance_count] pins: [sta::network_leaf_pin_count] nets: [sta::network_net_count]"
puts "query t0 [clock milliseconds]"
puts "cells *: [llength [get_cells *]]"
puts "pins *: [llength [get_pins -hierarchical *]]"
puts "query t1 [clock milliseconds]"
report_checks -path_delay max -endpoint_path_count 1
puts "sta t2 [clock milliseconds]"
report_tns
report_wns
puts "sta t3 [clock milliseconds]"
