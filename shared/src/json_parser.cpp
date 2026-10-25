#include "json_parser.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

namespace
{

struct Parser
{
	const std::string& m_Src;
	size_t m_Pos = 0;
	std::string& m_Err;

	Parser(const std::string& S, std::string& OutError) : m_Src(S), m_Err(OutError)
	{
	}

	bool Eof() const
	{
		return m_Pos >= m_Src.size();
	}
	char Peek() const
	{
		return m_Pos < m_Src.size() ? m_Src[m_Pos] : '\0';
	}

	void SkipWs()
	{
		while (m_Pos < m_Src.size())
		{
			char C = m_Src[m_Pos];
			if (C == ' ' || C == '\t' || C == '\n' || C == '\r')
			{
				++m_Pos;
			}
			else
			{
				break;
			}
		}
	}

	bool Fail(const char* Msg)
	{
		char Buf[64];
		snprintf(Buf, sizeof(Buf), " (at offset %zu)", m_Pos);
		m_Err = std::string(Msg) + Buf;
		return false;
	}

	bool Match(const char* Lit)
	{
		const size_t N = std::char_traits<char>::length(Lit);
		if (m_Pos + N > m_Src.size())
		{
			return false;
		}
		if (m_Src.compare(m_Pos, N, Lit) != 0)
		{
			return false;
		}
		m_Pos += N;
		return true;
	}

	bool Expect(char C)
	{
		if (Peek() != C)
		{
			char Buf[64];
			snprintf(Buf, sizeof(Buf), "expected '%c'", C);
			return Fail(Buf);
		}
		++m_Pos;
		return true;
	}

	bool ParseHexUnit(uint32_t& OutCode)
	{
		if (m_Src.size() - m_Pos < 4)
		{
			return Fail("short Unicode escape");
		}
		OutCode = 0;
		for (int Idx = 0; Idx < 4; ++Idx)
		{
			const char C = m_Src[m_Pos++];
			unsigned Nibble;
			if (C >= '0' && C <= '9')
			{
				Nibble = unsigned(C - '0');
			}
			else if (C >= 'a' && C <= 'f')
			{
				Nibble = unsigned(C - 'a' + 10);
			}
			else if (C >= 'A' && C <= 'F')
			{
				Nibble = unsigned(C - 'A' + 10);
			}
			else
			{
				return Fail("bad hex in Unicode escape");
			}
			OutCode = (OutCode << 4) | Nibble;
		}
		return true;
	}

	bool ParseString(std::string& Out)
	{
		if (!Expect('"'))
		{
			return false;
		}
		Out.clear();
		while (m_Pos < m_Src.size())
		{
			char C = m_Src[m_Pos++];
			if (C == '"')
			{
				return true;
			}
			if (C == '\\')
			{
				if (m_Pos >= m_Src.size())
				{
					return Fail("unterminated escape");
				}
				char E = m_Src[m_Pos++];
				switch (E)
				{
				case '"':
					Out += '"';
					break;
				case '\\':
					Out += '\\';
					break;
				case '/':
					Out += '/';
					break;
				case 'b':
					Out += '\b';
					break;
				case 'f':
					Out += '\f';
					break;
				case 'n':
					Out += '\n';
					break;
				case 'r':
					Out += '\r';
					break;
				case 't':
					Out += '\t';
					break;
				case 'u':
				{
					uint32_t CP;
					if (!ParseHexUnit(CP))
					{
						return false;
					}
					if (CP >= 0xD800 && CP <= 0xDBFF)
					{
						if (!Match("\\u"))
						{
							return Fail("missing low surrogate");
						}
						uint32_t Low;
						if (!ParseHexUnit(Low))
						{
							return false;
						}
						if (Low < 0xDC00 || Low > 0xDFFF)
						{
							return Fail("invalid low surrogate");
						}
						CP = 0x10000 + ((CP - 0xD800) << 10) + Low - 0xDC00;
					}
					else if (CP >= 0xDC00 && CP <= 0xDFFF)
					{
						return Fail("unpaired low surrogate");
					}
					if (CP < 0x80)
					{
						Out += char(CP);
					}
					else if (CP < 0x800)
					{
						Out += char(0xC0 | (CP >> 6));
						Out += char(0x80 | (CP & 0x3F));
					}
					else if (CP < 0x10000)
					{
						Out += char(0xE0 | (CP >> 12));
						Out += char(0x80 | ((CP >> 6) & 0x3F));
						Out += char(0x80 | (CP & 0x3F));
					}
					else
					{
						Out += char(0xF0 | (CP >> 18));
						Out += char(0x80 | ((CP >> 12) & 0x3F));
						Out += char(0x80 | ((CP >> 6) & 0x3F));
						Out += char(0x80 | (CP & 0x3F));
					}
					break;
				}
				default:
					return Fail("bad escape character");
				}
			}
			else
			{
				const unsigned First = static_cast<unsigned char>(C);
				if (First < 0x20)
				{
					return Fail("unescaped control character");
				}
				Out += C;
				if (First >= 0x80)
				{
					const unsigned Count =
						First >= 0xC2 && First <= 0xDF
							? 1
							: (First >= 0xE0 && First <= 0xEF ? 2 : (First >= 0xF0 && First <= 0xF4 ? 3 : 0));
					if (!Count || m_Src.size() - m_Pos < Count)
					{
						return Fail("invalid UTF-8");
					}
					uint32_t Code = First & ((1u << (6 - Count)) - 1);
					for (unsigned Idx = 0; Idx < Count; ++Idx)
					{
						const unsigned Next = static_cast<unsigned char>(m_Src[m_Pos++]);
						if ((Next & 0xC0) != 0x80)
						{
							return Fail("invalid UTF-8 continuation");
						}
						Code = (Code << 6) | (Next & 0x3F);
						Out += char(Next);
					}
					const uint32_t Minimum[] = {0, 0x80, 0x800, 0x10000};
					if (Code < Minimum[Count] || Code > 0x10FFFF || (Code >= 0xD800 && Code <= 0xDFFF))
					{
						return Fail("invalid UTF-8 code point");
					}
				}
			}
		}
		return Fail("unterminated string");
	}

	bool ParseNumber(JsonValue& Out)
	{
		const size_t Start = m_Pos;
		const bool bNegative = Peek() == '-';
		if (bNegative)
		{
			++m_Pos;
		}
		if (Peek() == '0')
		{
			++m_Pos;
		}
		else if (Peek() >= '1' && Peek() <= '9')
		{
			do
			{
				++m_Pos;
			} while (Peek() >= '0' && Peek() <= '9');
		}
		else
		{
			return Fail("expected integer digits");
		}
		bool bFloat = false;
		if (Peek() == '.')
		{
			bFloat = true;
			++m_Pos;
			const size_t Fraction = m_Pos;
			while (Peek() >= '0' && Peek() <= '9')
			{
				++m_Pos;
			}
			if (m_Pos == Fraction)
			{
				return Fail("expected fractional digits");
			}
		}
		if (Peek() == 'e' || Peek() == 'E')
		{
			bFloat = true;
			++m_Pos;
			if (Peek() == '+' || Peek() == '-')
			{
				++m_Pos;
			}
			const size_t Exponent = m_Pos;
			while (Peek() >= '0' && Peek() <= '9')
			{
				++m_Pos;
			}
			if (m_Pos == Exponent)
			{
				return Fail("expected exponent digits");
			}
		}
		const std::string Token = m_Src.substr(Start, m_Pos - Start);
		char* End;
		errno = 0;
		if (bFloat)
		{
			Out.m_K = JsonValue::Kind::kKindDouble;
			Out.m_D = strtod(Token.c_str(), &End);
			if (!isfinite(Out.m_D))
			{
				return Fail("number out of range");
			}
		}
		else if (bNegative)
		{
			Out.m_K = JsonValue::Kind::kKindInt;
			Out.m_I = strtoll(Token.c_str(), &End, 10);
		}
		else
		{
			Out.m_U = strtoull(Token.c_str(), &End, 10);
			if (Out.m_U <= INT64_MAX)
			{
				Out.m_K = JsonValue::Kind::kKindInt;
				Out.m_I = int64_t(Out.m_U);
			}
			else
			{
				Out.m_K = JsonValue::Kind::kKindUInt;
			}
		}
		if (errno == ERANGE || *End != '\0')
		{
			return Fail("number out of range");
		}
		return true;
	}

	bool ParseValue(JsonValue& Out, unsigned Depth = 0)
	{
		SkipWs();
		const char C = Peek();
		if (C == '"')
		{
			Out.m_K = JsonValue::Kind::kKindString;
			return ParseString(Out.m_S);
		}
		if ((C == '{' || C == '[') && Depth >= 64)
		{
			return Fail("JSON nesting exceeds 64 containers");
		}
		if (C == '{')
		{
			return ParseObject(Out, Depth + 1);
		}
		if (C == '[')
		{
			return ParseArray(Out, Depth + 1);
		}
		if (C == 't' || C == 'f')
		{
			if (Match("true"))
			{
				Out.m_K = JsonValue::Kind::kKindBool;
				Out.m_bB = true;
				return true;
			}
			if (Match("false"))
			{
				Out.m_K = JsonValue::Kind::kKindBool;
				Out.m_bB = false;
				return true;
			}
			return Fail("expected true/false");
		}
		if (C == 'n')
		{
			if (Match("null"))
			{
				Out.m_K = JsonValue::Kind::kKindNull;
				return true;
			}
			return Fail("expected null");
		}
		if (C == '-' || (C >= '0' && C <= '9'))
		{
			return ParseNumber(Out);
		}
		return Fail("unexpected character");
	}

	bool ParseArray(JsonValue& Out, unsigned Depth)
	{
		if (!Expect('['))
		{
			return false;
		}
		Out.m_K = JsonValue::Kind::kKindArray;
		Out.m_A.clear();
		SkipWs();
		if (Peek() == ']')
		{
			++m_Pos;
			return true;
		}
		while (true)
		{
			JsonValue V;
			if (!ParseValue(V, Depth))
			{
				return false;
			}
			Out.m_A.push_back(std::move(V));
			SkipWs();
			if (Peek() == ',')
			{
				++m_Pos;
				SkipWs();
				continue;
			}
			if (Peek() == ']')
			{
				++m_Pos;
				return true;
			}
			return Fail("expected ',' or ']'");
		}
	}

	bool ParseObject(JsonValue& Out, unsigned Depth)
	{
		if (!Expect('{'))
		{
			return false;
		}
		Out.m_K = JsonValue::Kind::kKindObject;
		Out.m_O.clear();
		SkipWs();
		if (Peek() == '}')
		{
			++m_Pos;
			return true;
		}
		while (true)
		{
			SkipWs();
			std::string Key;
			if (!ParseString(Key))
			{
				return false;
			}
			SkipWs();
			if (!Expect(':'))
			{
				return false;
			}
			SkipWs();
			JsonValue V;
			if (!ParseValue(V, Depth))
			{
				return false;
			}
			Out.m_O.push_back({std::move(Key), std::move(V)});
			SkipWs();
			if (Peek() == ',')
			{
				++m_Pos;
				continue;
			}
			if (Peek() == '}')
			{
				++m_Pos;
				return true;
			}
			return Fail("expected ',' or '}'");
		}
	}
};

} // namespace

bool ParseJson(const std::string& Input, JsonValue& Out, std::string& OutError)
{
	Out = JsonValue{};
	OutError.clear();
	Parser P(Input, OutError);
	if (!P.ParseValue(Out))
	{
		return false;
	}
	P.SkipWs();
	if (!P.Eof())
	{
		OutError = "trailing content after JSON value";
		return false;
	}
	return true;
}

bool ParseHexQuoted(const std::string& S, uint32_t& Out)
{
	if (S.size() < 2 || S.size() > 9 || S[0] != '$')
	{
		return false;
	}
	Out = 0;
	for (size_t IdxI = 1; IdxI < S.size(); ++IdxI)
	{
		char C = S[IdxI];
		Out <<= 4;
		if (C >= '0' && C <= '9')
		{
			Out |= uint32_t(C - '0');
		}
		else if (C >= 'a' && C <= 'f')
		{
			Out |= uint32_t(C - 'a' + 10);
		}
		else if (C >= 'A' && C <= 'F')
		{
			Out |= uint32_t(C - 'A' + 10);
		}
		else
		{
			return false;
		}
	}
	return true;
}

bool JsonValue::TryAsInt(int64_t& OutValue) const
{
	if (m_K == Kind::kKindInt)
	{
		OutValue = m_I;
		return true;
	}
	if (m_K == Kind::kKindUInt && m_U <= INT64_MAX)
	{
		OutValue = int64_t(m_U);
		return true;
	}
	if (IsDouble() && isfinite(m_D) && m_D >= -9223372036854775808.0 && m_D < 9223372036854775808.0 &&
		trunc(m_D) == m_D)
	{
		OutValue = int64_t(m_D);
		return true;
	}
	return false;
}

bool JsonValue::TryAsUInt(uint64_t& OutValue) const
{
	if (m_K == Kind::kKindUInt)
	{
		OutValue = m_U;
		return true;
	}
	if (m_K == Kind::kKindInt && m_I >= 0)
	{
		OutValue = uint64_t(m_I);
		return true;
	}
	if (IsDouble() && isfinite(m_D) && m_D >= 0 && m_D < 18446744073709551616.0 && trunc(m_D) == m_D)
	{
		OutValue = uint64_t(m_D);
		return true;
	}
	return false;
}
