module b(input clk, input in, output out);
  wire q0, n1;
  DFF u_dff0 (.CLK(clk), .D(in), .Q(q0));
  INV u_inv1 (.A(q0), .Y(n1));
  INV u_inv2 (.A(n1), .Y(out));
endmodule
