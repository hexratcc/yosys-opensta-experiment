foreach b {0 1 2 3} {
  puts "u1/A\[$b\] net: [get_full_name [get_nets -of_objects [get_pins u1/A\[$b\]]]]"
}
create_clock -period 10 -name clk [get_ports clk]
set_input_delay 1 -clock clk [get_ports {in[*]}]
set_output_delay 1 -clock clk [get_ports {out[*]}]
puts "slack: [format %.2f [worst_slack -max]]"
