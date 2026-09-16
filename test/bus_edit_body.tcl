create_clock -period 10 -name clk [get_ports clk]
set_input_delay 1 -clock clk [get_ports {in[*]}]
set_output_delay 1 -clock clk [get_ports {out[*]}]
# Bus-bit edits must touch only the named bit.
disconnect_pin m[2] u1/A[2]
connect_pin m[0] u1/A[2]
connect_pin m[1] u1/A[3]
foreach b {0 1 2 3} {
  set n [get_nets -of_objects [get_pins u1/A\[$b\]]]
  puts "u1/A\[$b\] net: [expr {[llength $n] ? [get_full_name $n] : "-"}]"
}
puts "slack: [format %.2f [worst_slack -max]]"
