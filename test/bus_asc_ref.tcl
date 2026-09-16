read_liberty test/simple.lib
read_verilog test/bus_asc.v
link_design top
source test/bus_body.tcl
