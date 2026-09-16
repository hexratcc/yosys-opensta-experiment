create_clock -period 10 -name clk [get_ports clk]
puts "linked before: [sta::network_is_linked]"
# A cell rename is Monitor-silent (a delete would still notify unsetPort), so
# only the `yosys` command trace can drop the memoized staleness check; the
# next OpenSTA command must then refuse to run.
yosys cd top
yosys rename u_inv1 u_x
yosys cd
puts "linked after: [sta::network_is_linked]"
if {[catch {report_checks} msg]} { puts "refused: $msg" } else { puts "refused: NO" }
