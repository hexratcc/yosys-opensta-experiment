module sub(input a, output y);
  INV i1 (.A(a), .Y(y));
  INV i2 (.A(a), .Y());
endmodule
module top(input clk, input in, output out);
  wire d0, q0, p;
  INV u_inv0 (.A(in), .Y(d0));
  DFF u_dff0 (.CLK(clk), .D(d0), .Q(q0));
  sub u_sub (.a(q0), .y(p));
  INV u_inv2 (.A(p), .Y(out));
endmodule
