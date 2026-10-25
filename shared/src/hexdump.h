#pragma once
#include <stdint.h>
#include <string>

// Encode `Length` bytes as a hexdump-style multi-line string suitable for
// inclusion in a JSON value. Format per line:
//   "<BankIdx>:<offset_hex>  XX XX XX XX XX XX XX XX   XX XX...   ...   XX"
// 32 bytes/line, 4 groups of 8 with triple-space separators. Lines joined
// with single '\n'. No trailing newline.
//
// If `bAllowZeroElision` is true and every byte is 0, returns the short
// sentinel "<BankIdx>:zeros" (the loader treats this as fill-with-zeros).
std::string EncodeBankAsHexdump(const uint8_t* Data, size_t Length, int BankIdx, bool bAllowZeroElision = true);

// Decode a hexdump string produced by EncodeBankAsHexdump back into Length bytes
// at Out. Handles the "<N>:zeros" sentinel by zeroing Out. Tolerant of
// whitespace variations within data lines. Returns false on parse failure.
bool DecodeBankFromHexdump(const std::string& Encoded, uint8_t* Out, size_t Length, std::string& OutError);
