// TODO: Implement interrupt stubs etc. in this file.
//       RISC-V Interrupt handling is slightly weird; in that there are two ways to handle interrupts:
//       - Using a single exception vector
//       - Using a table of jump instructions (similar to ARM)
//       An OS needs to support both, as the spec makes it possible for a CPU to only support one of these modes.
//       Technically, you can use the table of jump instructions to _slightly_ speed up interrupt handling (you instantly know
//       which exception/interrupt was fired), however in practice there is little difference.
