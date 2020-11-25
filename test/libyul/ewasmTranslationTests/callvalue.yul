{
  sstore(0, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
  sstore(32, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
  sstore(0, callvalue())
}
// ----
// Trace:
// Memory dump:
//     20: 0000000000000000000000000000000000000000000000000000000055555555
// Storage dump:
//   0000000000000000000000000000000000000000000000000000000000000000: 0000000000000000000000000000000000000000000000000000000055555555
//   0000000000000000000000000000000000000000000000000000000000000020: ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
