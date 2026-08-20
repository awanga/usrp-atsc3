// axi4s_skid_buffer_bind.sv — attaches the white-box checker to every
// axi4s_skid_buffer instance. Verification-only; see axi4s_skid_buffer.sby.
bind axi4s_skid_buffer axi4s_skid_buffer_checks u_checks ();
