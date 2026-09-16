module a(input clk, input in, output out);
  wire q0;
  DFF u_dff0 (.CLK(clk), .D(in), .Q(q0));
  INV u_inv1 (.A(q0), .Y(out));
endmodule
