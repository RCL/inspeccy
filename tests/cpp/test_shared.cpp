#include "frame_path.h"
#include "output_paths.h"
#include "json_parser.h"

#include <stdio.h>
#include <string>

namespace
{
int GFailures = 0;

void Check(bool bSuccess, const char* Description)
{
	if (!bSuccess)
	{
		fprintf(stderr, "FAIL: %s\n", Description);
		++GFailures;
	}
}
} // namespace

int main()
{
	std::string Path;
	Check(FormatFramePath("frames/%06d.bmp", 42, Path) && Path == "frames/000042.bmp", "decimal frame padding");
	Check(FormatFramePath("%%_%4X.bmp", 255, Path) && Path == "%_  FF.bmp", "hex padding and escaped percent");
	Check(FormatFramePath("%llu.bmp", UINT64_MAX, Path) && Path == "18446744073709551615.bmp", "64-bit frame");
	Check(FormatFramePath("%o.bmp", 64, Path) && Path == "100.bmp", "octal frame");
	for (const char* Pattern : {"", "frame.bmp", "%", "%s", "%n", "%*d", "%1$d", "%d_%u", "%1025d", "%999999999999999d",
			 "%ld%", "%f", "%lld%d"})
	{
		Check(!FormatFramePath(Pattern, 0, Path) && Path.empty(), Pattern);
	}
	Check(!FormatFramePath(std::string("%d\0.bmp", 7), 0, Path), "embedded null filename");
	Check(!FormatFramePath(std::string(1278, 'a') + "%d", 0, Path), "long filename");

	for (const char* Pattern : {"frames/%06d.bmp", "frames/%6u.bmp", "frames/%%_%llX.bmp", "frames/%o.bmp"})
	{
		OutputPaths Files;
		std::string Error;
		Check(Files.AddPattern(Pattern), "accept output pattern");
		FormatFramePath(Pattern, 255, Path);
		Files.AddInput(Path);
		Check(!Files.Validate(Error), "reject input inside frame sequence");
	}
	{
		OutputPaths Files;
		std::string Error;
		Files.AddInput("distinct/input.bin");
		Files.AddOutput("distinct/output.bin");
		Files.AddPattern("distinct/frames/%06d.bmp");
		Check(Files.Validate(Error), "distinct output destinations");
		Check(
			Files.CheckFrame("distinct/frames/000001.bmp", "distinct/frames/%06d.bmp", Error), "own frame destination");
		Check(
			!Files.CheckFrame("distinct/../distinct/input.bin", "distinct/frames/%06d.bmp", Error), "normalized alias");
	}

	JsonValue Value;
	std::string Error;
	for (const char* Input : {"null", "true", "false", "0", "-0", "-12", "1.25e+3", "[]", "{}", "{\"a\":[1,2,null]}",
			 "\"\\b\\f\\n\\r\\t\\/\\\\\\\"\""})
	{
		Check(ParseJson(Input, Value, Error), Input);
	}
	for (const char* Input : {"", "-", "01", "-01", "1.", "1e", "1e+", "+1", "NaN", "Infinity", "1e9999", "1e-9999",
			 "18446744073709551616", "-9223372036854775809", "[1,]", "{\"a\":1,}", "true false", "\"a\nb\"",
			 "\"\\uD800\"", "\"\\uDC00\"", "\"\\uD800\\u0041\"", "\"\\uFFFFF", "\"\\x00\""})
	{
		Check(!ParseJson(Input, Value, Error) && !Error.empty(), Input);
	}
	Check(ParseJson("\"\\uD83D\\uDE00\"", Value, Error) && Value.m_S == "\xF0\x9F\x98\x80", "surrogate pair");
	Check(ParseJson("\"\xE2\x82\xAC\"", Value, Error) && Value.m_S == "\xE2\x82\xAC", "UTF-8 string");
	for (const char* Input : {"\"\xC0\x80\"", "\"\xED\xA0\x80\"", "\"\xF4\x90\x80\x80\"", "\"\xE2\x82\"", "\"\x80\""})
	{
		Check(!ParseJson(Input, Value, Error), "malformed UTF-8");
	}
	Check(ParseJson(std::string(64, '[') + "0" + std::string(64, ']'), Value, Error), "nesting limit accepted");
	Check(!ParseJson(std::string(10000, '[') + "0" + std::string(10000, ']'), Value, Error), "deep nesting rejected");
	int64_t Signed;
	uint64_t Unsigned;
	Check(ParseJson("-9223372036854775808", Value, Error) && Value.TryAsInt(Signed) && Signed == INT64_MIN &&
			  !Value.TryAsUInt(Unsigned),
		"signed minimum");
	Check(ParseJson("18446744073709551615", Value, Error) && Value.TryAsUInt(Unsigned) && Unsigned == UINT64_MAX &&
			  !Value.TryAsInt(Signed),
		"unsigned maximum");
	Check(ParseJson("1.5", Value, Error) && !Value.TryAsInt(Signed) && !Value.TryAsUInt(Unsigned),
		"fractional conversion rejected");
	Check(ParseJson("9223372036854775808.0", Value, Error) && !Value.TryAsInt(Signed) && Value.TryAsUInt(Unsigned),
		"double signed boundary");
	Check(ParseJson("18446744073709551616.0", Value, Error) && !Value.TryAsUInt(Unsigned), "double unsigned boundary");
	uint32_t Hex;
	Check(ParseHexQuoted("$FFFFFFFF", Hex) && Hex == UINT32_MAX, "hex maximum");
	Check(!ParseHexQuoted("$100000000", Hex), "hex overflow");
	Check(!ParseHexQuoted("$", Hex), "empty hex");
	Check(ParseJson("[]", Value, Error) && Error.empty() && Value.m_O.empty(), "reuse parser output");
	return GFailures ? 1 : 0;
}
