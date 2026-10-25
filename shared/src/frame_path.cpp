#include "frame_path.h"

#include <stdio.h>
#include <utility>

bool FormatFramePath(const std::string& Pattern, uint64_t Frame, std::string& OutPath)
{
	constexpr size_t kMaxPath = 1279;
	constexpr size_t kMaxWidth = 1024;
	OutPath.clear();
	if (Pattern.empty() || Pattern.size() > kMaxPath)
	{
		return false;
	}
	std::string Result;
	bool bFormatted = false;
	for (size_t Idx = 0; Idx < Pattern.size(); ++Idx)
	{
		if (Pattern[Idx] != '%')
		{
			if (Pattern[Idx] == '\0')
			{
				return false;
			}
			Result += Pattern[Idx];
			continue;
		}
		if (++Idx == Pattern.size())
		{
			return false;
		}
		if (Pattern[Idx] == '%')
		{
			Result += '%';
			continue;
		}
		if (bFormatted)
		{
			return false;
		}
		bFormatted = true;
		const bool bZeroPad = Pattern[Idx] == '0';
		size_t Width = 0;
		while (Idx < Pattern.size() && Pattern[Idx] >= '0' && Pattern[Idx] <= '9')
		{
			Width = Width * 10 + size_t(Pattern[Idx++] - '0');
			if (Width > kMaxWidth)
			{
				return false;
			}
		}
		if (Idx < Pattern.size() && Pattern[Idx] == 'l')
		{
			++Idx;
			if (Idx < Pattern.size() && Pattern[Idx] == 'l')
			{
				++Idx;
			}
		}
		if (Idx == Pattern.size())
		{
			return false;
		}
		const char* Format;
		switch (Pattern[Idx])
		{
		case 'd':
		case 'i':
		case 'u':
			Format = "%llu";
			break;
		case 'x':
			Format = "%llx";
			break;
		case 'X':
			Format = "%llX";
			break;
		case 'o':
			Format = "%llo";
			break;
		default:
			return false;
		}
		char Digits[32];
		const int Count = snprintf(Digits, sizeof(Digits), Format, static_cast<unsigned long long>(Frame));
		if (Count < 0 || size_t(Count) >= sizeof(Digits))
		{
			return false;
		}
		if (Width > size_t(Count))
		{
			Result.append(Width - size_t(Count), bZeroPad ? '0' : ' ');
		}
		Result.append(Digits, size_t(Count));
	}
	if (!bFormatted || Result.size() > kMaxPath)
	{
		return false;
	}
	OutPath = std::move(Result);
	return true;
}
