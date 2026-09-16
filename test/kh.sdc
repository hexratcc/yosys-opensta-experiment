create_clock -period 10 -name clk [get_ports clk]
set_input_delay 1 -clock clk [get_ports in1]
set_false_path -through [get_pins sub1/and_gate/A1]
