#pragma once
// Bounded frame filenames without a caller-controlled printf format.

#include <stdint.h>
#include <string>

// One integer placeholder (d/i/u/x/X/o), optional width and l/ll; %% is literal.
// Returns false for invalid syntax, more than one placeholder, or a long path.
bool FormatFramePath(const std::string& Pattern, uint64_t Frame, std::string& OutPath);
