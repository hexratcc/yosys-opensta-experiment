create_clock -period 10 -name clk [get_ports clk]
set_input_delay 1 -clock clk [get_ports in]
set_output_delay 1 -clock clk [get_ports out]
puts "slack baseline: [format %.2f [worst_slack -max]]"
replace_cell u_inv1 INV_SLOW
puts "slack after replace_cell: [format %.2f [worst_slack -max]]"
