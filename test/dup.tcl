create_clock -period 10 -name clk [get_ports clk]
set_input_delay 1 -clock clk [get_ports in]
set_output_delay 1 -clock clk [get_ports out]
puts "slack baseline: [format %.2f [worst_slack -max]]"
# Duplicate u_dff0 and move u_inv1's input onto the copy.
make_instance u_dff0_dup DFF
connect_pin clk u_dff0_dup/CLK
connect_pin d0 u_dff0_dup/D
make_net q1
connect_pin q1 u_dff0_dup/Q
disconnect_pin q0 u_inv1/A
connect_pin q1 u_inv1/A
puts "slack after dup: [format %.2f [worst_slack -max]]"
