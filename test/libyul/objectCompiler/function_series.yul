object "Contract" {
  code {
    function f() {}
    function g() {}
    sstore(0, 1)
  }
}

// ----
// Assembly:
//     /* "source":83:84   */
//   0x01
//     /* "source":80:81   */
//   0x00
//     /* "source":73:85   */
//   sstore
// Bytecode: 6001600055
// Opcodes: PUSH1 0x1 PUSH1 0x0 SSTORE
// SourceMappings: 83:1:0:-:0;80;73:12
