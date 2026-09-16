create_clock -period 10 -name clk [get_ports clk]
set_input_delay 1 -clock clk [get_ports in]
set_output_delay 1 -clock clk [get_ports {out out2}]
puts "insts: [sta::network_leaf_instance_count]"
puts "slack: [format %.4f [worst_slack -max]]"
