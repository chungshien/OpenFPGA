//-----------------------------------------------------
// Design Name : General Purpose I/Os
// File Name   : gpio.v
// Coder       : Xifan TANG
//-----------------------------------------------------

//-----------------------------------------------------
// Function    : A minimum general purpose I/O
//-----------------------------------------------------
module GPIO (
  input A, // Data output
  output Y, // Data input
  inout PAD, // bi-directional pad
  input DIR // direction control
);
  //----- when direction enabled, the signal is propagated from PAD to data input
  assign Y = DIR ? PAD : 1'bz;
  //----- when direction is disabled, the signal is propagated from data out to pad
  assign PAD = DIR ? 1'bz : A;
endmodule

//-----------------------------------------------------
// Function    : A minimum input pad
//-----------------------------------------------------
module GPIN (
  inout A, // External PAD signal
  output Y // Data input
);
  assign Y = A;
endmodule

//-----------------------------------------------------
// Function    : A minimum output pad
//-----------------------------------------------------
module GPOUT (
  inout A, // External PAD signal
  input Y // Data output
);
  assign A = Y;
endmodule
