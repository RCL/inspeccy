#include <asm/index.h>

#include "json_parser.h"

namespace
{

// Every field of a region may be written as a number or as an expression
// over the build's own symbols, so that a limit can say STACK_TOP-256 rather
// than repeating an address the source already states.
bool Field(const AsmIndex& Index, const JsonValue& Region, const char* Key, long& Out,
	std::string& OutError)
{
	const JsonValue* V = Region.Find(Key);
	if (V == nullptr)
	{
		OutError = std::string("region is missing \"") + Key + '"';
		return false;
	}
	if (!AsmEvalValue(Index, *V, Out, OutError))
	{
		OutError = std::string("region \"") + Key + "\": " + OutError;
		return false;
	}
	return true;
}

} // namespace

bool AsmLayout(const AsmIndex& Index, const JsonValue& Config, std::vector<AsmLayoutRow>& Out,
	std::string& OutError)
{
	const JsonValue* Regions = Config.Find("regions");
	if (Regions == nullptr || !Regions->IsArray())
	{
		OutError = "configuration has no \"regions\" array";
		return false;
	}
	for (const JsonValue& Region : Regions->m_A)
	{
		const JsonValue* NameV = Region.Find("name");
		AsmLayoutRow Row;
		Row.Name = (NameV != nullptr && NameV->IsString()) ? NameV->m_S : std::string("(unnamed)");
		if (!Field(Index, Region, "page", Row.Page, OutError)
			|| !Field(Index, Region, "start", Row.Start, OutError)
			|| !Field(Index, Region, "end", Row.End, OutError)
			|| !Field(Index, Region, "limit", Row.Limit, OutError))
		{
			OutError = Row.Name + ": " + OutError;
			return false;
		}
		if (!(Row.Start <= Row.End && Row.End <= Row.Limit))
		{
			OutError = "region " + Row.Name + " exceeds its declared limit or is inverted";
			return false;
		}
		Row.Used = Row.End - Row.Start;
		Row.Free = Row.Limit - Row.End;
		Out.push_back(Row);
	}
	return true;
}
