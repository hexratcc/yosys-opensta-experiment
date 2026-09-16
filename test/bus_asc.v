module top(input clk, input [3:0] in, output [3:0] out);
  wire [3:0] m;
  BUF4A u0 (.A(in), .Y(m));
  BUF4A u1 (.A(m), .Y(out));
endmodule
