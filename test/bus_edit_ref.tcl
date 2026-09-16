read_liberty test/simple.lib
read_verilog test/bus.v
link_design top
source test/bus_edit_body.tcl
