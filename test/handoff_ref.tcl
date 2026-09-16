read_liberty test/simple.lib
read_verilog test/handoff_out.v
link_design top
read_sdc test/handoff_out.sdc
puts "HANDOFF_BEGIN"
report_checks
puts "HANDOFF_END"
