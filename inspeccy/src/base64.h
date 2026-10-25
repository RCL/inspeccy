#pragma once
#include <stdint.h>
#include <string>

// Standard base64 (RFC 4648) encoder. Uses '+' and '/' (not URL-safe variant).
// Always pads to a multiple of 4 with '='. No line wrapping.
std::string EncodeBase64(const uint8_t* Data, size_t Length);

// Inverse of EncodeBase64. Tolerant of internal whitespace. Returns false on
// invalid character or wrong padding.
#include <vector>
bool DecodeBase64(const std::string& In, std::vector<uint8_t>& Out, std::string& OutError);
