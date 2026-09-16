create_clock -period 10 -name clk [get_ports clk]
set_input_delay 1 -clock clk [get_ports in]
set_output_delay 1 -clock clk [get_ports out]
replace_cell u_inv1 INV_SLOW
write_sdc -no_timestamp test/handoff_out.sdc
puts "HANDOFF_BEGIN"
report_checks
puts "HANDOFF_END"
