create_clock -period 10 -name clk [get_ports clk]
set_input_delay 1 -clock clk [get_ports in]
set_output_delay 1 -clock clk [get_ports out]
puts "slack fresh: [format %.2f [worst_slack -max]]"
puts "dff count: [llength [get_cells -filter {ref_name == DFF} *]]"
