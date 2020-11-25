{
  sstore(0, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
  sstore(32, 0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff)
  sstore(0, difficulty())
}
// ----
// Trace:
// Memory dump:
//     20: 0000000000000000000000000000000000000000000000000000000009999999
// Storage dump:
//   0000000000000000000000000000000000000000000000000000000000000000: 0000000000000000000000000000000000000000000000000000000009999999
//   0000000000000000000000000000000000000000000000000000000000000020: ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
