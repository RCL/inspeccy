#pragma once
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

// JSON dump values; object key order and 64-bit integers are preserved.
struct JsonValue
{
	enum Kind
	{
		kKindNull,
		kKindBool,
		kKindInt,
		kKindUInt,
		kKindDouble,
		kKindString,
		kKindArray,
		kKindObject
	};

	Kind m_K = Kind::kKindNull;
	bool m_bB = false;
	int64_t m_I = 0;
	uint64_t m_U = 0;
	double m_D = 0.0;
	std::string m_S;
	std::vector<JsonValue> m_A;
	std::vector<std::pair<std::string, JsonValue>> m_O;

	bool IsNull() const
	{
		return m_K == Kind::kKindNull;
	}
	bool IsBool() const
	{
		return m_K == Kind::kKindBool;
	}
	bool IsInt() const
	{
		return m_K == Kind::kKindInt || m_K == Kind::kKindUInt;
	}
	bool IsDouble() const
	{
		return m_K == Kind::kKindDouble;
	}
	bool IsNumber() const
	{
		return IsInt() || IsDouble();
	}
	bool IsString() const
	{
		return m_K == Kind::kKindString;
	}
	bool IsArray() const
	{
		return m_K == Kind::kKindArray;
	}
	bool IsObject() const
	{
		return m_K == Kind::kKindObject;
	}

	// Object helpers. Find returns nullptr if Key is absent.
	const JsonValue* Find(const char* Key) const
	{
		for (const auto& kKV : m_O)
		{
			if (kKV.first == Key)
			{
				return &kKV.second;
			}
		}
		return nullptr;
	}

	// Checked conversions reject fractional, negative (for unsigned), or out-of-range values.
	bool TryAsInt(int64_t& OutValue) const;
	bool TryAsUInt(uint64_t& OutValue) const;
	double AsDouble() const
	{
		return m_K == Kind::kKindInt ? double(m_I) : (m_K == Kind::kKindUInt ? double(m_U) : m_D);
	}
};

// Parse the entire input as a single JSON value. Returns false and sets OutError
// with a diagnostic on failure. Input must be UTF-8, with at most 64 nested containers.
bool ParseJson(const std::string& Input, JsonValue& Out, std::string& OutError);

// Convenience: parse a "$XXXX" or "$XX" hex string into a uint32_t.
// Returns false if the string is malformed.
bool ParseHexQuoted(const std::string& S, uint32_t& Out);
