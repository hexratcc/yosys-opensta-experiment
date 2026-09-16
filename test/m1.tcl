puts "insts: [sta::network_leaf_instance_count] pins: [sta::network_leaf_pin_count] nets: [sta::network_net_count]"
foreach c [get_cells *] { puts "cell: [get_full_name $c] ([get_property $c ref_name])" }
foreach p [get_pins u_dff0/*] { puts "pin: [get_full_name $p]" }
foreach n [get_nets *] { puts "net: [get_full_name $n]" }
foreach p [get_ports *] { puts "port: [get_full_name $p]" }
create_clock -period 10 -name clk [get_ports clk]
foreach c [all_clocks] { puts "clock: [get_name $c] period [get_property $c period]" }
set_input_delay 1 -clock clk [get_ports in]
set_output_delay 1 -clock clk [get_ports out]
puts "sdc done"
report_checks
