read_liberty deps/OpenSTA/test/nangate45/Nangate45_typ.lib
read_verilog test/stress.v
link_design stress
source test/stress_body.tcl
