create_clock -period 10 -name clk [get_ports clk]
set_max_delay 5 -from [get_cells sub2]
