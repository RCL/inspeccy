#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>

// SHA-1 (RFC 3174). Returns 40-char lowercase hex digest. Self-contained
// implementation; no third-party deps. Used only for external-file identity
// in dumps - not for cryptographic security.
std::string Sha1Hex(const uint8_t* Data, size_t Length);

// Convenience: hash a file's full content. Returns "" on open/read failure.
std::string Sha1FileHex(const char* Path);
