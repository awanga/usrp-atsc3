// axi4s_interface.vh — AXI4-Stream Interface Definitions for gr-atsc3
//
// Common macros and parameters for AXI4-Stream interfaces.
// Include this file in all RTL stubs.
//
// Usage:
//   `include "axi4s_interface.vh"

`ifndef AXI4S_INTERFACE_VH
`define AXI4S_INTERFACE_VH

//------------------------------------------------------------------------------
// Data width parameters
//------------------------------------------------------------------------------

// Complex sample widths
parameter ATSC3_SAMPLE_WIDTH_FLOAT = 64;    // {re[31:0], im[31:0]} IEEE 754
parameter ATSC3_SAMPLE_WIDTH_FIXED = 32;    // {re[15:0], im[15:0]} Q1.15

// Select based on fixed-point mode
`ifdef ATSC3_FIXED_POINT
    parameter ATSC3_SAMPLE_WIDTH = ATSC3_SAMPLE_WIDTH_FIXED;
    parameter ATSC3_REAL_WIDTH = 16;
`else
    parameter ATSC3_SAMPLE_WIDTH = ATSC3_SAMPLE_WIDTH_FLOAT;
    parameter ATSC3_REAL_WIDTH = 32;
`endif

// LLR width (soft bits from demapper)
parameter ATSC3_LLR_WIDTH = 8;

// Decoded bit width
parameter ATSC3_BIT_WIDTH = 1;

//------------------------------------------------------------------------------
// AXI4-Stream interface macro
//------------------------------------------------------------------------------

// Macro to declare AXI4-Stream master interface signals
// Usage: `AXI4S_MASTER(prefix, data_width)
`define AXI4S_MASTER(prefix, data_width) \
    output reg [(data_width)-1:0] prefix``_tdata, \
    output reg                    prefix``_tvalid, \
    input  wire                   prefix``_tready, \
    output reg                    prefix``_tlast

// Macro to declare AXI4-Stream slave interface signals
// Usage: `AXI4S_SLAVE(prefix, data_width)
`define AXI4S_SLAVE(prefix, data_width) \
    input  wire [(data_width)-1:0] prefix``_tdata, \
    input  wire                    prefix``_tvalid, \
    output reg                     prefix``_tready, \
    input  wire                    prefix``_tlast

//------------------------------------------------------------------------------
// Handshake macros
//------------------------------------------------------------------------------

// Check if transfer is valid (data accepted)
`define AXI4S_TRANSFER(valid, ready) ((valid) && (ready))

// Fire signal for single-cycle pulse on transfer
`define AXI4S_FIRE(prefix) ((prefix``_tvalid) && (prefix``_tready))

//------------------------------------------------------------------------------
// Common FFT sizes
//------------------------------------------------------------------------------

parameter ATSC3_FFT_4K  = 4096;     // Bootstrap FFT
parameter ATSC3_FFT_8K  = 8192;
parameter ATSC3_FFT_16K = 16384;
parameter ATSC3_FFT_32K = 32768;

// Log2 of FFT sizes for address width calculation
parameter ATSC3_FFT_4K_LOG2  = 12;
parameter ATSC3_FFT_8K_LOG2  = 13;
parameter ATSC3_FFT_16K_LOG2 = 14;
parameter ATSC3_FFT_32K_LOG2 = 15;

//------------------------------------------------------------------------------
// LDPC parameters
//------------------------------------------------------------------------------

parameter ATSC3_LDPC_SHORT = 16200;
parameter ATSC3_LDPC_LONG  = 64800;

//------------------------------------------------------------------------------
// Timing parameters
//------------------------------------------------------------------------------

// Default clock period (ns) — placeholder for synthesis constraints
parameter ATSC3_CLK_PERIOD_NS = 10;  // 100 MHz default

`endif // AXI4S_INTERFACE_VH
