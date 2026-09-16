module stress (clk, \weird/name , \a[3] , rev_in, bus_out, out);
  input clk;
  input \weird/name ;
  input \a[3] ;
  input [0:3] rev_in;
  output [3:0] bus_out;
  output out;

  wire alias1, alias2;
  wire \x/y ;
  wire [3:0] n1;

  assign alias1 = \weird/name ;
  assign alias2 = alias1;

  BUF_X1 buf0 (.A(alias2), .Z(\x/y ));
  AND2_X1 and0 (.A1(\x/y ), .A2(\a[3] ), .ZN(n1[0]));
  BUF_X1 buf1 (.A(rev_in[0]), .Z(n1[1]));
  BUF_X1 buf2 (.A(rev_in[3]), .Z(n1[2]));
  BUF_X1 \esc/inst (.A(rev_in[1]), .Z(n1[3]));
  DFF_X1 reg0 (.D(n1[0]), .CK(clk), .Q(bus_out[0]));
  DFF_X1 reg1 (.D(n1[1]), .CK(clk), .Q(bus_out[1]));
  DFF_X1 reg2 (.D(n1[2]), .CK(clk), .Q(bus_out[2]));
  DFF_X1 reg3 (.D(n1[3]), .CK(clk), .Q(bus_out[3]));
  BUF_X1 bufo (.A(n1[0]), .Z(out));
endmodule
