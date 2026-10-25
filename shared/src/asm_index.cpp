#include <asm/index.h>

#include "sld_fields.h"

#include <algorithm>
#include <string.h>

namespace
{

bool IsWordChar(char C)
{
	return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_';
}

bool IsNameStart(char C)
{
	return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || C == '_';
}

// Word characters plus '.', so a dotted local label reads as one name.
bool IsNameChar(char C)
{
	return IsWordChar(C) || C == '.';
}

char Lower(char C)
{
	return (C >= 'A' && C <= 'Z') ? char(C - 'A' + 'a') : C;
}

std::string LowerOf(const std::string& S)
{
	std::string Out = S;
	for (char& C : Out)
	{
		C = Lower(C);
	}
	return Out;
}

std::string Trim(const std::string& S)
{
	size_t A = 0, B = S.size();
	while (A < B && (S[A] == ' ' || S[A] == '\t'))
	{
		++A;
	}
	while (B > A && (S[B - 1] == ' ' || S[B - 1] == '\t'))
	{
		--B;
	}
	return S.substr(A, B - A);
}

// Everything before the first ';', trimmed. Assembly comments never appear
// inside string literals in the sources this reads.
std::string StripComment(const std::string& S)
{
	const size_t At = S.find(';');
	return Trim(At == std::string::npos ? S : S.substr(0, At));
}

std::vector<std::string> SplitOn(const std::string& S, char Sep)
{
	std::vector<std::string> Out;
	size_t Start = 0;
	for (size_t IdxI = 0; IdxI <= S.size(); ++IdxI)
	{
		if (IdxI == S.size() || S[IdxI] == Sep)
		{
			Out.push_back(Trim(S.substr(Start, IdxI - Start)));
			Start = IdxI + 1;
		}
	}
	return Out;
}

bool IsAbsolute(const std::string& Path)
{
	return !Path.empty() && (Path[0] == '/' || Path[0] == '\\'
			   || (Path.size() > 1 && Path[1] == ':'));
}

std::string JoinPath(const std::string& Root, const std::string& File)
{
	if (Root.empty() || IsAbsolute(File))
	{
		return File;
	}
	return Root + "/" + File;
}

std::string DirectoryOf(const std::string& Path)
{
	const size_t At = Path.find_last_of("/\\");
	return At == std::string::npos ? std::string(".") : Path.substr(0, At);
}

// ---------------------------------------------------------- expressions
// + - * and parentheses over decimal, $hex and symbol values. Anything else
// is an error rather than a guess: these expressions come from a build
// configuration that decides whether a page is judged to fit.
struct Evaluator
{
	const AsmIndex& Index;
	const std::string& Text;
	size_t At = 0;
	bool bOk = true;
	std::string Error;

	void Skip()
	{
		while (At < Text.size() && (Text[At] == ' ' || Text[At] == '\t'))
		{
			++At;
		}
	}

	void Fail(const std::string& Why)
	{
		if (bOk)
		{
			Error = Why;
			bOk = false;
		}
	}

	long Factor()
	{
		Skip();
		if (At >= Text.size())
		{
			Fail("unexpected end of expression");
			return 0;
		}
		if (Text[At] == '(')
		{
			++At;
			const long V = Expression();
			Skip();
			if (At >= Text.size() || Text[At] != ')')
			{
				Fail("unbalanced parenthesis");
				return 0;
			}
			++At;
			return V;
		}
		if (Text[At] == '-')
		{
			++At;
			return -Factor();
		}
		if (Text[At] == '$' || (Text[At] >= '0' && Text[At] <= '9'))
		{
			const size_t Start = At;
			if (Text[At] == '$')
			{
				++At;
			}
			else if (Text[At] == '0' && At + 1 < Text.size() && Lower(Text[At + 1]) == 'x')
			{
				At += 2;
			}
			while (At < Text.size() && IsWordChar(Text[At]))
			{
				++At;
			}
			bool bValue = false;
			const long V = sld::ParseValue(Text.substr(Start, At - Start), bValue);
			if (!bValue)
			{
				Fail("not a number: " + Text.substr(Start, At - Start));
			}
			return V;
		}
		if (Text[At] == '@' || IsNameStart(Text[At]))
		{
			const size_t Start = At;
			if (Text[At] == '@')
			{
				++At;
			}
			const size_t NameStart = At;
			while (At < Text.size() && IsNameChar(Text[At]))
			{
				++At;
			}
			const std::string Name = Text.substr(NameStart, At - NameStart);
			const AsmIndex::Record* Sym = Index.Symbol(Name);
			if (Sym == nullptr)
			{
				Fail("unknown symbol: " + Text.substr(Start, At - Start));
				return 0;
			}
			return long(Sym->Address);
		}
		Fail(std::string("unexpected character: ") + Text[At]);
		return 0;
	}

	long Term()
	{
		long V = Factor();
		for (;;)
		{
			Skip();
			if (At < Text.size() && Text[At] == '*')
			{
				++At;
				V *= Factor();
				continue;
			}
			return V;
		}
	}

	long Expression()
	{
		long V = Term();
		for (;;)
		{
			Skip();
			if (At < Text.size() && (Text[At] == '+' || Text[At] == '-'))
			{
				const char Op = Text[At++];
				const long R = Term();
				V = (Op == '+') ? V + R : V - R;
				continue;
			}
			return V;
		}
	}
};

// "call", "jp", "jr" or "djnz" at the head of an instruction, with its
// optional condition removed. Returns false when the text is something else.
bool SplitBranch(const std::string& Text, std::string& OutOp, std::string& OutCondition,
	std::string& OutOperand)
{
	static const char* kOps[] = {"call", "djnz", "jp", "jr"};
	const std::string Low = LowerOf(Text);
	for (const char* Op : kOps)
	{
		const size_t Len = strlen(Op);
		if (Low.compare(0, Len, Op) != 0)
		{
			continue;
		}
		if (Low.size() <= Len || (Low[Len] != ' ' && Low[Len] != '\t'))
		{
			continue;
		}
		OutOp = Op;
		std::string Rest = Trim(Text.substr(Len));
		OutCondition.clear();
		// A condition is the first comma-separated token, when it is one of
		// the Z80's flags. "jp nz, Label" and "jp NzThing" must not be
		// confused, so the token has to match exactly.
		const size_t Comma = Rest.find(',');
		if (Comma != std::string::npos)
		{
			static const char* kFlags[] = {"nz", "z", "nc", "c", "po", "pe", "p", "m"};
			const std::string First = LowerOf(Trim(Rest.substr(0, Comma)));
			for (const char* Flag : kFlags)
			{
				if (First == Flag)
				{
					OutCondition = First;
					Rest = Trim(Rest.substr(Comma + 1));
					break;
				}
			}
		}
		OutOperand = Rest;
		return true;
	}
	return false;
}

} // namespace

bool AsmIndex::Load(const std::string& SldPath, const std::string& SourceRoot, std::string& OutError)
{
	std::vector<std::string> Lines;
	if (!sld::ReadLines(SldPath, Lines))
	{
		OutError = "asm: cannot read " + SldPath;
		return false;
	}
	m_Root = SourceRoot.empty() ? DirectoryOf(SldPath) : SourceRoot;

	const long PageSize = sld::DetectPageSize(Lines);
	if (PageSize != 8192 && PageSize != 16384)
	{
		OutError = "asm: unsupported SLD page size";
		return false;
	}

	for (const std::string& Line : Lines)
	{
		if (Line.empty() || Line[0] == '|' || Line[0] == ';')
		{
			continue;
		}
		std::string Type;
		if (!sld::SplitField(Line, 6, Type))
		{
			continue;
		}
		if (Type != "F" && Type != "D" && Type != "T")
		{
			continue;
		}
		std::string FileS, LineS, DefFileS, DefLineS, PageS, ValueS, Name;
		if (!sld::SplitField(Line, 0, FileS) || !sld::SplitField(Line, 1, LineS)
			|| !sld::SplitField(Line, 2, DefFileS) || !sld::SplitField(Line, 3, DefLineS)
			|| !sld::SplitField(Line, 4, PageS) || !sld::SplitField(Line, 5, ValueS)
			|| !sld::SplitField(Line, 7, Name))
		{
			continue;
		}
		bool bPageOk = false, bValueOk = false;
		long Page = sld::ParseValue(PageS, bPageOk);
		const long Value = sld::ParseValue(ValueS, bValueOk);
		if (!bPageOk || !bValueOk)
		{
			continue;
		}
		// A 16 KB-bank device states bank numbers; bit 13 of the address says
		// which physical 8 KB half the label really sits in.
		if (PageSize == 16384 && Page >= 0)
		{
			Page = Page * 2 + ((Value >> 13) & 1);
		}

		Record Rec;
		Rec.Page = int(Page);
		Rec.Address = uint32_t(Value);
		Rec.File = FileS;
		Rec.Line = atoi(LineS.c_str());
		Rec.DefFile = DefFileS.empty() ? FileS : DefFileS;
		Rec.DefLine = atoi(DefLineS.c_str());

		if (Type == "T")
		{
			m_Instructions.push_back(Rec);
			continue;
		}
		auto Existing = m_Symbols.find(Name);
		if (Existing != m_Symbols.end()
			&& (Existing->second.Page != Rec.Page || Existing->second.Address != Rec.Address))
		{
			OutError = "asm: ambiguous symbol: " + Name;
			return false;
		}
		m_Symbols[Name] = Rec;
	}

	std::sort(m_Instructions.begin(), m_Instructions.end(),
		[](const Record& A, const Record& B)
		{ return A.Page != B.Page ? A.Page < B.Page : A.Address < B.Address; });

	for (const auto& kEntry : m_Symbols)
	{
		if (kEntry.second.Page >= 0)
		{
			m_Labels[kEntry.second.Page].push_back({kEntry.second.Address, kEntry.first});
		}
	}
	for (auto& kEntry : m_Labels)
	{
		std::sort(kEntry.second.begin(), kEntry.second.end());
	}

	for (Record& Rec : m_Instructions)
	{
		Rec.Text = SourceText(Rec);
		Rec.Owner = Owner(Rec.Page, Rec.Address);
	}
	return true;
}

const AsmIndex::Record* AsmIndex::Symbol(const std::string& Name) const
{
	auto At = m_Symbols.find(Name);
	return At == m_Symbols.end() ? nullptr : &At->second;
}

bool AsmIndex::Evaluate(const std::string& Expression, long& OutValue, std::string& OutError) const
{
	// A bare symbol name is the common case and may contain characters the
	// tokeniser would otherwise split on.
	const Record* Direct = Symbol(Trim(Expression));
	if (Direct != nullptr)
	{
		OutValue = long(Direct->Address);
		return true;
	}
	Evaluator Eval{*this, Expression, 0, true, std::string()};
	const long V = Eval.Expression();
	Eval.Skip();
	if (Eval.bOk && Eval.At != Expression.size())
	{
		Eval.Fail("trailing text in expression");
	}
	if (!Eval.bOk)
	{
		OutError = Eval.Error + " (in \"" + Expression + "\")";
		return false;
	}
	OutValue = V;
	return true;
}

std::string AsmIndex::Owner(int Page, uint32_t Address) const
{
	auto At = m_Labels.find(Page);
	if (At == m_Labels.end())
	{
		return std::string();
	}
	const auto& Entries = At->second;
	// The last label at or before Address.
	size_t Lo = 0, Hi = Entries.size();
	while (Lo < Hi)
	{
		const size_t Mid = (Lo + Hi) / 2;
		if (Entries[Mid].first <= Address)
		{
			Lo = Mid + 1;
		}
		else
		{
			Hi = Mid;
		}
	}
	return Lo == 0 ? std::string() : Entries[Lo - 1].second;
}

std::string AsmIndex::ResolveRef(const std::string& Text, const std::string& OwnerName) const
{
	std::string Name = Trim(Text);
	if (!Name.empty() && Name[0] == '@')
	{
		Name.erase(0, 1);
	}
	if (!Name.empty() && Name[0] == '.' && !OwnerName.empty())
	{
		// ".Next" inside "Thing.Inner" may mean "Thing.Inner.Next" or
		// "Thing.Next"; try the longest scope first.
		std::vector<std::string> Parts = SplitOn(OwnerName, '.');
		for (size_t N = Parts.size(); N > 0; --N)
		{
			std::string Candidate;
			for (size_t IdxI = 0; IdxI < N; ++IdxI)
			{
				if (IdxI != 0)
				{
					Candidate += '.';
				}
				Candidate += Parts[IdxI];
			}
			Candidate += Name;
			if (Symbol(Candidate) != nullptr)
			{
				return Candidate;
			}
		}
	}
	return Symbol(Name) != nullptr ? Name : std::string();
}

const std::vector<AsmIndex::Edge>& AsmIndex::Edges() const
{
	if (m_bEdgesBuilt)
	{
		return m_Edges;
	}
	m_bEdgesBuilt = true;
	for (const Record& Rec : m_Instructions)
	{
		std::string Op, Condition, Operand;
		if (Rec.Text.empty() || !SplitBranch(Rec.Text, Op, Condition, Operand))
		{
			continue;
		}
		Edge E;
		E.Page = Rec.Page;
		E.Address = Rec.Address;
		E.File = Rec.File;
		E.Line = Rec.Line;
		E.Owner = Rec.Owner;
		E.Operation = Op;
		E.Operand = Operand;
		E.Target = ResolveRef(Operand, Rec.Owner);
		E.bResolved = !E.Target.empty();
		if (E.bResolved)
		{
			E.TargetPage = Symbol(E.Target)->Page;
		}
		m_Edges.push_back(E);
	}
	return m_Edges;
}

std::vector<const AsmIndex::Record*> AsmIndex::Body(const std::string& Name, long End) const
{
	std::vector<const Record*> Out;
	const Record* Start = Symbol(Name);
	if (Start == nullptr)
	{
		return Out;
	}
	if (End < 0)
	{
		// The next top-level label bounds the body; a local label of this
		// routine's own scope is part of it.
		End = 65536;
		auto At = m_Labels.find(Start->Page);
		if (At != m_Labels.end())
		{
			const std::string Prefix = Name + ".";
			for (const auto& kEntry : At->second)
			{
				if (kEntry.first > Start->Address && kEntry.second.compare(0, Prefix.size(), Prefix) != 0)
				{
					End = long(kEntry.first);
					break;
				}
			}
		}
	}
	for (const Record& Rec : m_Instructions)
	{
		if (Rec.Page == Start->Page && Rec.Address >= Start->Address && long(Rec.Address) < End)
		{
			Out.push_back(&Rec);
		}
	}
	return Out;
}

const std::vector<std::string>* AsmIndex::Lines(const std::string& File) const
{
	auto At = m_Files.find(File);
	if (At == m_Files.end())
	{
		std::vector<std::string> Loaded;
		sld::ReadLines(JoinPath(m_Root, File), Loaded); // a missing file caches as empty
		At = m_Files.emplace(File, std::move(Loaded)).first;
	}
	return &At->second;
}

std::string AsmIndex::SourceText(const Record& Rec) const
{
	const std::vector<std::string>* Body = Lines(Rec.DefFile);
	const int N = Rec.DefLine != 0 ? Rec.DefLine : Rec.Line;
	if (Body == nullptr || N <= 0 || size_t(N) > Body->size())
	{
		return std::string();
	}
	std::string Text = StripComment((*Body)[size_t(N) - 1]);

	// Written inside a macro body: substitute the arguments the invocation
	// supplied, so the text reads as the instruction that was assembled.
	if (Rec.DefLine != 0)
	{
		const std::vector<std::string>* Site = Lines(Rec.File);
		std::string Invocation;
		if (Site != nullptr && Rec.Line > 0 && size_t(Rec.Line) <= Site->size())
		{
			Invocation = StripComment((*Site)[size_t(Rec.Line) - 1]);
		}
		for (int IdxI = N - 1; IdxI >= 1; --IdxI)
		{
			const std::string Candidate = Trim((*Body)[size_t(IdxI) - 1]);
			if (LowerOf(Candidate).compare(0, 6, "macro ") != 0)
			{
				continue;
			}
			std::string Rest = Trim(Candidate.substr(6));
			size_t NameEnd = 0;
			while (NameEnd < Rest.size() && IsWordChar(Rest[NameEnd]))
			{
				++NameEnd;
			}
			const std::string MacroName = Rest.substr(0, NameEnd);
			const std::vector<std::string> Params = SplitOn(Trim(Rest.substr(NameEnd)), ',');
			// The invocation must name this macro before its arguments.
			const std::string LowInvocation = LowerOf(Invocation);
			const std::string LowName = LowerOf(MacroName);
			if (!MacroName.empty() && LowInvocation.compare(0, LowName.size(), LowName) == 0
				&& LowInvocation.size() > LowName.size()
				&& (LowInvocation[LowName.size()] == ' ' || LowInvocation[LowName.size()] == '\t'))
			{
				const std::vector<std::string> Args
					= SplitOn(Trim(Invocation.substr(MacroName.size())), ',');
				std::map<std::string, std::string> Bound;
				for (size_t IdxJ = 0; IdxJ < Params.size() && IdxJ < Args.size(); ++IdxJ)
				{
					if (!Params[IdxJ].empty())
					{
						Bound[Params[IdxJ]] = Args[IdxJ];
					}
				}
				std::string Substituted;
				size_t At = 0;
				while (At < Text.size())
				{
					if (!IsWordChar(Text[At]))
					{
						Substituted += Text[At++];
						continue;
					}
					const size_t Start = At;
					while (At < Text.size() && IsWordChar(Text[At]))
					{
						++At;
					}
					const std::string Word = Text.substr(Start, At - Start);
					auto Found = Bound.find(Word);
					Substituted += (Found == Bound.end()) ? Word : Found->second;
				}
				Text = Substituted;
			}
			break;
		}
	}

	// Drop a leading "Label:" so the text is the instruction alone.
	size_t At = 0;
	while (At < Text.size() && (Text[At] == ' ' || Text[At] == '\t'))
	{
		++At;
	}
	const size_t NameStart = At;
	while (At < Text.size() && (IsNameChar(Text[At]) || Text[At] == '@'))
	{
		++At;
	}
	if (At > NameStart && At < Text.size() && Text[At] == ':')
	{
		Text = Trim(Text.substr(At + 1));
	}
	return Text;
}
