#pragma once
// Reject output destinations that alias inputs or another output stream.

#include <string>
#include <vector>

struct OutputPaths
{
	struct File
	{
		std::string m_Path;
		bool m_bOutput;
	};
	struct Pattern
	{
		std::string m_Format;
		std::string m_Prefix;
		std::string m_Suffix;
		int m_Base;
	};
	std::vector<File> m_Files;
	std::vector<Pattern> m_Patterns;

	void AddInput(const std::string& Path);
	void AddOutput(const std::string& Path);
	bool AddPattern(const std::string& Format);
	bool Validate(std::string& OutError) const;
	bool CheckFrame(const std::string& Path, const std::string& OwnPattern, std::string& OutError) const;
};
