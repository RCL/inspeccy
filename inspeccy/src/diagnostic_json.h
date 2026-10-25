#pragma once
#include "json_parser.h"
#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>

// Strict, bounded JSON helpers shared by inspection and IRQ configuration.
namespace DiagnosticJson
{
inline std::string Quote(const std::string& S)
{
	std::string R = "\"";
	const char* H = "0123456789abcdef";
	for (unsigned char C : S)
	{
		if (C == '"' || C == '\\')
		{
			R += '\\';
			R += char(C);
		}
		else if (C < 32)
		{
			R += "\\u00";
			R += H[C >> 4];
			R += H[C & 15];
		}
		else
		{
			R += char(C);
		}
	}
	return R + '"';
}
inline void Fields(const JsonValue& V, std::initializer_list<const char*> Allowed)
{
	if (!V.IsObject())
	{
		throw std::runtime_error("expected an object");
	}
	std::set<std::string> Seen;
	for (const auto& K : V.m_O)
	{
		if (!Seen.insert(K.first).second)
		{
			throw std::runtime_error("duplicate field: " + K.first);
		}
		if (std::none_of(Allowed.begin(), Allowed.end(),
				[&](const char* A)
				{
					return K.first == A;
				}))
		{
			throw std::runtime_error("unknown field: " + K.first);
		}
	}
}
inline const JsonValue& Required(const JsonValue& V, const char* Key)
{
	const auto* P = V.Find(Key);
	if (!P)
	{
		throw std::runtime_error(std::string("missing field: ") + Key);
	}
	return *P;
}
inline std::string String(const JsonValue& V, const char* Key)
{
	const auto& P = Required(V, Key);
	if (!P.IsString())
	{
		throw std::runtime_error(std::string("expected string: ") + Key);
	}
	return P.m_S;
}
inline uint64_t Number(const JsonValue& V, uint64_t Max)
{
	uint64_t N = 0;
	uint32_t Hex = 0;
	if (V.IsString() && ParseHexQuoted(V.m_S, Hex))
	{
		N = Hex;
	}
	else if (!V.TryAsUInt(N))
	{
		throw std::runtime_error("expected unsigned integer or $hex string");
	}
	if (N > Max)
	{
		throw std::runtime_error("number out of range");
	}
	return N;
}
inline uint32_t Num(const JsonValue& V, const char* Key, uint32_t Max)
{
	return uint32_t(Number(Required(V, Key), Max));
}
inline std::string Hex(const uint8_t* B, size_t N)
{
	const char* H = "0123456789ABCDEF";
	std::string R = "\"";
	for (size_t I = 0; I < N; ++I)
	{
		R += H[B[I] >> 4];
		R += H[B[I] & 15];
	}
	return R + '"';
}
inline JsonValue Read(const std::string& Path, size_t Max)
{
	std::ifstream F(Path, std::ios::binary);
	if (!F)
	{
		throw std::runtime_error("cannot open: " + Path);
	}
	F.seekg(0, std::ios::end);
	const auto Size = F.tellg();
	if (Size < 0 || uint64_t(Size) > Max)
	{
		throw std::runtime_error("JSON file exceeds size limit: " + Path);
	}
	F.seekg(0);
	std::string Text((std::istreambuf_iterator<char>(F)), {}), Error;
	JsonValue Root;
	if (!ParseJson(Text, Root, Error))
	{
		throw std::runtime_error(Error);
	}
	return Root;
}
inline bool Write(const std::string& Path, const std::string& Json)
{
	std::ofstream F(Path, std::ios::binary);
	F << Json << '\n';
	F.close();
	return bool(F);
}
} // namespace DiagnosticJson
