create_clock -period 10 -name clk [get_ports clk]
set_input_delay 1 -clock clk [get_ports in]
set_output_delay 1 -clock clk [get_ports out]
set_false_path -through [get_nets p]
puts "slack baseline: [format %.2f [worst_slack -max]]"
# Reconnecting a load caches the drivers of top net p, found through the
# u_sub boundary: {u_sub/i1/Y}.
disconnect_pin p u_inv2/A
connect_pin p u_inv2/A
# Swap the driver inside the child. The exception re-derives its edges for p
# from the cached driver set; a stale set misses u_sub/i2/Y and the path to
# out is reported although it is a false path.
disconnect_pin u_sub/y u_sub/i1/Y
connect_pin u_sub/y u_sub/i2/Y
report_checks -to [get_ports out]
