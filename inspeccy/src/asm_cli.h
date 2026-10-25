#pragma once

// The static-analysis commands: they read an sjasmplus build's .sld and its
// sources, and never construct a machine. Recognised before argument parsing
// proper, because none of them take a program or a run length.
//
//   --asm-layout     --sld F --asm-config F [--source-root D] [--json]
//   --asm-banks      --sld F --asm-config F [--source-root D] [--json]
//   --asm-query SYM  --sld F [--source-root D] [--limit N] [--json]
//   --write-manifest F --image F --sld F [--sld F ...]
//
// Returns false when Argv names none of them. Otherwise OutExitCode is the
// process exit code: 0 success, 1 the bank lint found errors, 2 bad usage or
// unreadable input.
bool RunAsmCommand(int Argc, char** Argv, int& OutExitCode);
