puts "--- ports ---"
foreach p [get_ports *] { puts "port: [get_full_name $p] dir [get_property $p direction]" }
puts "escaped port: [llength [get_ports {weird/name}]]"
puts "bracket port: [llength [get_ports {a[3]}]]"
puts "rev port bits: [llength [get_ports {rev_in[*]}]]"
puts "--- cells ---"
foreach c [get_cells *] { puts "cell: [get_full_name $c]" }
puts "esc inst: [llength [get_cells {esc/inst}]]"
puts "esc inst pins: [llength [get_pins {esc/inst/*}]]"
puts "--- nets ---"
foreach n [get_nets *] { puts "net: [get_full_name $n]" }
puts "alias1: [llength [get_nets alias1]]"
puts "alias2: [llength [get_nets alias2]]"
puts "escnet: [llength [get_nets {x/y}]]"
create_clock -period 10 -name clk [get_ports clk]
report_checks -endpoint_path_count 1
