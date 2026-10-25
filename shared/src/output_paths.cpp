#include "output_paths.h"
#include "frame_path.h"

#include <charconv>
#include <filesystem>

namespace
{
std::string NormalizePath(const std::string& Path)
{
	std::error_code Error;
	auto Full = std::filesystem::weakly_canonical(std::filesystem::absolute(Path, Error), Error);
	if (Error)
	{
		Error.clear();
		Full = std::filesystem::absolute(Path, Error).lexically_normal();
	}
	std::string Key = Full.generic_string();
#ifdef _WIN32
	for (char& Ch : Key)
	{
		if (Ch >= 'A' && Ch <= 'Z')
		{
			Ch += 'a' - 'A';
		}
	}
#endif
	return Key;
}

bool SameFile(const std::string& Left, const std::string& Right)
{
	if (NormalizePath(Left) == NormalizePath(Right))
	{
		return true;
	}
	std::error_code Error;
	return std::filesystem::equivalent(Left, Right, Error) && !Error;
}

bool MatchesPattern(const OutputPaths::Pattern& Pattern, const std::string& Path)
{
	const std::string Key = NormalizePath(Path);
	const size_t Begin = Pattern.m_Prefix.size();
	const size_t Tail = Pattern.m_Suffix.size();
	if (Key.size() <= Begin + Tail || Key.compare(0, Begin, Pattern.m_Prefix) != 0 ||
		Key.compare(Key.size() - Tail, Tail, Pattern.m_Suffix) != 0)
	{
		return false;
	}
	std::string Digits = Key.substr(Begin, Key.size() - Begin - Tail);
	const size_t First = Digits.find_first_not_of(' ');
	if (First == std::string::npos)
	{
		return false;
	}
	uint64_t Frame = 0;
	const auto Parsed = std::from_chars(Digits.data() + First, Digits.data() + Digits.size(), Frame, Pattern.m_Base);
	if (Parsed.ec != std::errc() || Parsed.ptr != Digits.data() + Digits.size())
	{
		return false;
	}
	std::string Expected;
	return FormatFramePath(Pattern.m_Format, Frame, Expected) && SameFile(Expected, Path);
}

bool IsNullDevice(const std::string& Path)
{
#ifdef _WIN32
	return Path == "NUL" || Path == "nul" || Path == "NUL:" || Path == "nul:";
#else
	return Path == "/dev/null";
#endif
}
} // namespace

void OutputPaths::AddInput(const std::string& Path)
{
	if (!Path.empty() && !IsNullDevice(Path))
	{
		m_Files.push_back({Path, false});
	}
}

void OutputPaths::AddOutput(const std::string& Path)
{
	if (!Path.empty() && !IsNullDevice(Path))
	{
		m_Files.push_back({Path, true});
	}
}

bool OutputPaths::AddPattern(const std::string& Format)
{
	if (Format.empty())
	{
		return true;
	}
	std::string Example;
	if (!FormatFramePath(Format, 0, Example))
	{
		return false;
	}
	std::string Marker = "inspeccy_frame_placeholder";
	while (Format.find(Marker) != std::string::npos)
	{
		Marker += 'x';
	}
	std::string Literal;
	int Base = 10;
	for (size_t Idx = 0; Idx < Format.size(); ++Idx)
	{
		if (Format[Idx] != '%')
		{
			Literal += Format[Idx];
		}
		else if (Format[++Idx] == '%')
		{
			Literal += '%';
		}
		else
		{
			while ((Format[Idx] >= '0' && Format[Idx] <= '9') || Format[Idx] == 'l')
			{
				++Idx;
			}
			Base = Format[Idx] == 'o' ? 8 : (Format[Idx] == 'x' || Format[Idx] == 'X') ? 16 : 10;
			Literal += Marker;
		}
	}
	const std::string Key = NormalizePath(Literal);
	const size_t Split = Key.find(Marker);
	if (Split == std::string::npos)
	{
		return false;
	}
	m_Patterns.push_back({Format, Key.substr(0, Split), Key.substr(Split + Marker.size()), Base});
	return true;
}

bool OutputPaths::Validate(std::string& OutError) const
{
	for (size_t Idx = 0; Idx < m_Files.size(); ++Idx)
	{
		const File& Current = m_Files[Idx];
		if (Current.m_Path.find('\0') != std::string::npos)
		{
			OutError = "file path contains a null byte";
			return false;
		}
		for (size_t Other = 0; Other < Idx; ++Other)
		{
			if ((Current.m_bOutput || m_Files[Other].m_bOutput) && SameFile(Current.m_Path, m_Files[Other].m_Path))
			{
				OutError = "output path aliases another input or output: " + Current.m_Path;
				return false;
			}
		}
		for (const Pattern& Candidate : m_Patterns)
		{
			if (MatchesPattern(Candidate, Current.m_Path))
			{
				OutError = "frame output pattern includes an input or another output: " + Current.m_Path;
				return false;
			}
		}
	}
	for (size_t Idx = 0; Idx < m_Patterns.size(); ++Idx)
	{
		std::string First;
		FormatFramePath(m_Patterns[Idx].m_Format, 0, First);
		for (size_t Other = 0; Other < Idx; ++Other)
		{
			if (MatchesPattern(m_Patterns[Other], First))
			{
				OutError = "frame output patterns overlap: " + First;
				return false;
			}
		}
	}
	return true;
}

bool OutputPaths::CheckFrame(const std::string& Path, const std::string& OwnPattern, std::string& OutError) const
{
	for (const File& Existing : m_Files)
	{
		if (SameFile(Path, Existing.m_Path))
		{
			OutError = "frame output aliases an input or another output: " + Path;
			return false;
		}
	}
	for (const Pattern& Other : m_Patterns)
	{
		if (Other.m_Format != OwnPattern && MatchesPattern(Other, Path))
		{
			OutError = "frame output patterns overlap: " + Path;
			return false;
		}
	}
	return true;
}
