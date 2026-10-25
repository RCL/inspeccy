#include <asm/index.h>

#include "json_parser.h"

#include <stdlib.h>

#include <array>
#include <set>

// A conservative MMU lint. It walks the instructions of each explicitly
// bounded routine carrying an eight-slot symbolic mapping, and reports where
// that mapping cannot be shown to be what an access needs.
//
// The important property is what it does when it does not know: a slot whose
// page is unknown, a call with no declared contract, a port write, an
// indirect access - all of these are reported as UNVERIFIED and the state is
// widened, never quietly assumed safe. An external contract is an assumption
// the author has written down, not a proof.
//
// The state is eight pages, with -1 meaning unknown. Physical pages are never
// negative, so -1 cannot collide with a real one.

namespace
{

using State = std::array<int, 8>;
constexpr int kUnknown = -1;
constexpr size_t kStateLimit = 4096;
constexpr size_t kUnverifiedLimit = 200;

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

bool IsWordChar(char C)
{
	return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_';
}

// The head word of an instruction, lowercased: "ld", "ret", "outi", "nextreg".
std::string Mnemonic(const std::string& Text)
{
	size_t At = 0;
	while (At < Text.size() && (Text[At] == ' ' || Text[At] == '\t'))
	{
		++At;
	}
	const size_t Start = At;
	while (At < Text.size() && IsWordChar(Text[At]))
	{
		++At;
	}
	return LowerOf(Text.substr(Start, At - Start));
}

// Every non-nested "(...)" group, which is how the Z80 writes a memory
// access. Nested parentheses are not produced by these sources.
std::vector<std::string> ParenGroups(const std::string& Text)
{
	std::vector<std::string> Out;
	size_t At = 0;
	while (At < Text.size())
	{
		const size_t Open = Text.find('(', At);
		if (Open == std::string::npos)
		{
			break;
		}
		const size_t Close = Text.find(')', Open + 1);
		if (Close == std::string::npos)
		{
			break;
		}
		const std::string Inner = Text.substr(Open + 1, Close - Open - 1);
		if (!Inner.empty() && Inner.find('(') == std::string::npos)
		{
			Out.push_back(Inner);
		}
		At = Close + 1;
	}
	return Out;
}

// The leading symbol-ish token of a memory operand: "Table", ".Local",
// "@Thing", "Table-$2000" -> "Table".
std::string LeadingToken(const std::string& Expression)
{
	size_t At = 0;
	while (At < Expression.size() && (Expression[At] == ' ' || Expression[At] == '\t'))
	{
		++At;
	}
	const size_t Start = At;
	while (At < Expression.size()
		&& (IsWordChar(Expression[At]) || Expression[At] == '.' || Expression[At] == '@'))
	{
		++At;
	}
	return Expression.substr(Start, At - Start);
}

// "hl", "de", "bc" alone, or "ix"/"iy" followed by anything non-word
// ("ix+3"): a pointer the lint cannot follow.
bool IsIndirectRegister(const std::string& Expression)
{
	const std::string Low = LowerOf(Trim(Expression));
	if (Low == "hl" || Low == "de" || Low == "bc")
	{
		return true;
	}
	if (Low.compare(0, 2, "ix") == 0 || Low.compare(0, 2, "iy") == 0)
	{
		return Low.size() == 2 || !IsWordChar(Low[2]);
	}
	return false;
}

// "ret", "reti", "retn" as the mnemonic - not "retry".
bool IsReturn(const std::string& Mnem)
{
	return Mnem == "ret" || Mnem == "reti" || Mnem == "retn";
}

bool IsPortOut(const std::string& Mnem)
{
	return Mnem == "out" || Mnem == "outi" || Mnem == "outd" || Mnem == "outir"
		|| Mnem == "outdr" || Mnem == "otir" || Mnem == "otdr" || Mnem == "outinb";
}

bool IsBranch(const std::string& Mnem)
{
	return Mnem == "call" || Mnem == "jp" || Mnem == "jr" || Mnem == "djnz";
}

bool IsFlag(const std::string& Token)
{
	static const char* kFlags[] = {"nz", "z", "nc", "c", "po", "pe", "p", "m"};
	for (const char* Flag : kFlags)
	{
		if (Token == Flag)
		{
			return true;
		}
	}
	return false;
}

const JsonValue* FindObject(const JsonValue& Parent, const char* Key)
{
	const JsonValue* V = Parent.Find(Key);
	return (V != nullptr && V->IsObject()) ? V : nullptr;
}

} // namespace

bool AsmEvalValue(const AsmIndex& Index, const JsonValue& V, long& Out, std::string& OutError)
{
	if (V.IsInt())
	{
		int64_t I = 0;
		if (!V.TryAsInt(I))
		{
			OutError = "value out of range";
			return false;
		}
		Out = long(I);
		return true;
	}
	if (V.IsString())
	{
		return Index.Evaluate(V.m_S, Out, OutError);
	}
	OutError = "value must be a number or an expression";
	return false;
}

bool AsmCheckBanks(const AsmIndex& Index, const JsonValue& Config, AsmBankReport& Out,
	std::string& OutError)
{
	const JsonValue* Contracts = FindObject(Config, "routines");
	const JsonValue* FixedCfg = FindObject(Config, "fixed");

	State Fixed;
	Fixed.fill(kUnknown);
	if (FixedCfg != nullptr)
	{
		for (const auto& kSlot : FixedCfg->m_O)
		{
			const int Slot = atoi(kSlot.first.c_str());
			long Page = 0;
			if (Slot < 0 || Slot > 7)
			{
				OutError = "fixed: slot " + kSlot.first + " is not 0..7";
				return false;
			}
			if (!AsmEvalValue(Index, kSlot.second, Page, OutError))
			{
				OutError = "fixed slot " + kSlot.first + ": " + OutError;
				return false;
			}
			Fixed[size_t(Slot)] = int(Page);
		}
	}

	Out.AnnotatedRoutines = Contracts != nullptr ? Contracts->m_O.size() : 0;

	auto Note = [&](std::vector<AsmBankNote>& Bucket, const std::string& Routine,
					const AsmIndex::Record& Rec, const std::string& Message)
	{
		AsmBankNote Item;
		Item.Routine = Routine;
		Item.Address = Rec.Address;
		Item.File = Rec.File;
		Item.Line = Rec.Line;
		Item.Message = Message;
		for (const AsmBankNote& Have : Bucket)
		{
			if (Have.Routine == Item.Routine && Have.Address == Item.Address
				&& Have.File == Item.File && Have.Line == Item.Line
				&& Have.Message == Item.Message)
			{
				return;
			}
		}
		Bucket.push_back(Item);
	};

	if (Contracts != nullptr)
	{
		for (const auto& kRoutine : Contracts->m_O)
		{
			const std::string& Name = kRoutine.first;
			const JsonValue& Contract = kRoutine.second;

			const JsonValue* External = Contract.Find("external");
			if (External != nullptr && External->m_K == JsonValue::kKindBool && External->m_bB)
			{
				Out.ExternalAssumptions.push_back(Name);
				continue;
			}
			const AsmIndex::Record* Start = Index.Symbol(Name);
			if (Start == nullptr)
			{
				OutError = "unknown symbol: " + Name;
				return false;
			}
			const JsonValue* EndV = Contract.Find("end");
			if (EndV == nullptr)
			{
				OutError = Name + ": checked routines require an exclusive end label/address";
				return false;
			}
			long End = 0;
			if (!AsmEvalValue(Index, *EndV, End, OutError))
			{
				OutError = Name + ": " + OutError;
				return false;
			}
			const std::vector<const AsmIndex::Record*> Body = Index.Body(Name, End);
			if (Body.empty() || Body[0]->Address != Start->Address)
			{
				OutError = Name + ": no instruction at entry (check SLD/source pairing)";
				return false;
			}
			std::map<uint32_t, size_t> Positions;
			for (size_t IdxI = 0; IdxI < Body.size(); ++IdxI)
			{
				Positions[Body[IdxI]->Address] = IdxI;
			}

			State Entry = Fixed;
			const JsonValue* Requires = FindObject(Contract, "requires");
			if (Requires != nullptr)
			{
				for (const auto& kSlot : Requires->m_O)
				{
					const int Slot = atoi(kSlot.first.c_str());
					long Page = 0;
					if (Slot < 0 || Slot > 7
						|| !AsmEvalValue(Index, kSlot.second, Page, OutError))
					{
						OutError = Name + " requires slot " + kSlot.first + ": " + OutError;
						return false;
					}
					Entry[size_t(Slot)] = int(Page);
				}
			}
			if (Start->Page >= 0)
			{
				const size_t Slot = Start->Address / 8192;
				if (Entry[Slot] != kUnknown && Entry[Slot] != Start->Page)
				{
					OutError = Name + ": entry mapping contradicts its physical page";
					return false;
				}
				Entry[Slot] = Start->Page;
			}

			// Reported per routine, so the messages name the right one.
			auto Require = [&](State& Now, size_t Slot, int Page, const AsmIndex::Record& Rec,
							   const std::string& What)
			{
				if (Slot > 7)
				{
					return;
				}
				if (Now[Slot] == kUnknown)
				{
					Note(Out.Unverified, Name, Rec,
						What + ": slot " + std::to_string(Slot) + " mapping unknown; needs page "
							+ std::to_string(Page));
				}
				else if (Now[Slot] != Page)
				{
					Note(Out.Errors, Name, Rec,
						What + ": slot " + std::to_string(Slot) + " maps "
							+ std::to_string(Now[Slot]) + ", needs " + std::to_string(Page));
				}
			};
			const JsonValue* Returns = FindObject(Contract, "returns");
			auto Finish = [&](State& Now, const AsmIndex::Record& Rec)
			{
				if (Returns == nullptr)
				{
					return;
				}
				for (const auto& kSlot : Returns->m_O)
				{
					const int Slot = atoi(kSlot.first.c_str());
					if (Slot < 0 || Slot > 7)
					{
						continue;
					}
					int Page = kUnknown;
					if (kSlot.second.IsString() && kSlot.second.m_S == "entry")
					{
						Page = Entry[size_t(Slot)];
					}
					else
					{
						long V = 0;
						std::string Ignored;
						Page = AsmEvalValue(Index, kSlot.second, V, Ignored) ? int(V) : kUnknown;
					}
					if (Page == kUnknown)
					{
						Note(Out.Unverified, Name, Rec,
							"cannot prove restoration of dynamic slot " + std::to_string(Slot));
					}
					else
					{
						Require(Now, size_t(Slot), Page, Rec, "return");
					}
				}
			};

			std::vector<std::pair<size_t, State>> Queue{{0, Entry}};
			std::set<std::pair<size_t, State>> Seen;
			while (!Queue.empty())
			{
				const std::pair<size_t, State> Item = Queue.back();
				Queue.pop_back();
				if (Seen.count(Item) != 0)
				{
					continue;
				}
				Seen.insert(Item);
				if (Seen.size() > kStateLimit)
				{
					Note(Out.Unverified, Name, *Start, "analysis stopped at 4096 distinct states");
					break;
				}
				const size_t Pos = Item.first;
				State Now = Item.second;
				const AsmIndex::Record& Rec = *Body[Pos];
				Require(Now, Rec.Address / 8192, Rec.Page, Rec, "instruction fetch");

				const std::string& Text = Rec.Text;
				if (Text.empty())
				{
					Note(Out.Unverified, Name, Rec, "source instruction unavailable");
				}
				const std::string Mnem = Mnemonic(Text);

				// Direct reads and writes, including a symbol deliberately
				// reached through another CPU window (for example Table-$2000).
				for (const std::string& Expression : ParenGroups(Text))
				{
					const std::string Token = LeadingToken(Expression);
					const std::string Target
						= Token.empty() ? std::string() : Index.ResolveRef(Token, Rec.Owner);
					const AsmIndex::Record* Sym
						= Target.empty() ? nullptr : Index.Symbol(Target);
					if (Sym != nullptr && Sym->Page >= 0)
					{
						std::string Rewritten = Expression;
						const size_t At = Rewritten.find(Token);
						if (At != std::string::npos)
						{
							Rewritten.replace(At, Token.size(), Target);
						}
						long Address = 0;
						std::string Why;
						if (!Index.Evaluate(Rewritten, Address, Why) || Address < 0
							|| Address > 65535)
						{
							Note(Out.Unverified, Name, Rec,
								"unresolved data address: " + Expression);
						}
						else
						{
							Require(Now, size_t(Address) / 8192, Sym->Page, Rec,
								"data " + Target);
						}
					}
					else if (IsIndirectRegister(Expression))
					{
						Note(Out.Unverified, Name, Rec,
							"indirect access: (" + Expression
								+ "); declare required data mappings");
					}
				}

				if (Mnem == "nextreg")
				{
					const std::string Rest = Trim(Text.substr(Mnem.size()));
					const size_t Comma = Rest.find(',');
					long Register = 0;
					std::string Why;
					if (Comma == std::string::npos
						|| !Index.Evaluate(Trim(Rest.substr(0, Comma)), Register, Why))
					{
						Note(Out.Unverified, Name, Rec, "unknown Next register destination");
						Now = Fixed;
					}
					else if (Register >= 0x50 && Register <= 0x57)
					{
						long Page = 0;
						Now[size_t(Register - 0x50)]
							= Index.Evaluate(Trim(Rest.substr(Comma + 1)), Page, Why) ? int(Page)
																					  : kUnknown;
					}
				}
				else if (IsPortOut(Mnem))
				{
					Note(Out.Unverified, Name, Rec, "port output may change mappings");
					Now = Fixed;
				}

				bool bTerminal = false;
				if (IsReturn(Mnem))
				{
					Finish(Now, Rec);
					bTerminal = Trim(Text) == Mnem; // an unconditional "ret" ends the path
				}
				else if (IsBranch(Mnem))
				{
					std::string Rest = Trim(Text.substr(Mnem.size()));
					bool bConditional = (Mnem == "djnz");
					const size_t Comma = Rest.find(',');
					if (Comma != std::string::npos
						&& IsFlag(LowerOf(Trim(Rest.substr(0, Comma)))))
					{
						bConditional = true;
						Rest = Trim(Rest.substr(Comma + 1));
					}
					const std::string Operand = Rest;
					const std::string Target = Index.ResolveRef(Operand, Rec.Owner);
					const AsmIndex::Record* Dest
						= Target.empty() ? nullptr : Index.Symbol(Target);
					auto Inside = Dest != nullptr && Dest->Page == Start->Page
						? Positions.find(Dest->Address)
						: Positions.end();
					if (Mnem != "call" && Inside != Positions.end())
					{
						Queue.push_back({Inside->second, Now});
					}
					else
					{
						const State Before = Now;
						const JsonValue* Callee
							= (Contracts != nullptr && !Target.empty()) ? Contracts->Find(Target.c_str())
																	   : nullptr;
						if (Dest != nullptr && Dest->Page >= 0)
						{
							Require(Now, Dest->Address / 8192, Dest->Page, Rec,
								Mnem + " " + Target);
						}
						if (Callee != nullptr)
						{
							const JsonValue* Needs = FindObject(*Callee, "requires");
							if (Needs != nullptr)
							{
								for (const auto& kSlot : Needs->m_O)
								{
									const int Slot = atoi(kSlot.first.c_str());
									long Page = 0;
									std::string Why;
									if (Slot >= 0 && Slot <= 7
										&& AsmEvalValue(Index, kSlot.second, Page, Why))
									{
										Require(Now, size_t(Slot), int(Page), Rec,
											Target + " precondition");
									}
								}
							}
							const JsonValue* MineCtx = Contract.Find("context");
							const JsonValue* TheirCtx = Callee->Find("context");
							if (MineCtx != nullptr && TheirCtx != nullptr && MineCtx->IsString()
								&& TheirCtx->IsString() && MineCtx->m_S != TheirCtx->m_S)
							{
								Note(Out.Errors, Name, Rec,
									Target + ": incompatible execution context");
							}
							const JsonValue* Clobbers = Callee->Find("clobbers");
							if (Clobbers != nullptr && Clobbers->IsArray())
							{
								for (const JsonValue& Slot : Clobbers->m_A)
								{
									int64_t I = 0;
									if (Slot.TryAsInt(I) && I >= 0 && I <= 7)
									{
										Now[size_t(I)] = kUnknown;
									}
								}
							}
							const JsonValue* Gives = FindObject(*Callee, "returns");
							if (Gives != nullptr)
							{
								for (const auto& kSlot : Gives->m_O)
								{
									const int Slot = atoi(kSlot.first.c_str());
									if (Slot < 0 || Slot > 7)
									{
										continue;
									}
									if (kSlot.second.IsString() && kSlot.second.m_S == "entry")
									{
										Now[size_t(Slot)] = Before[size_t(Slot)];
									}
									else
									{
										long Page = 0;
										std::string Why;
										Now[size_t(Slot)]
											= AsmEvalValue(Index, kSlot.second, Page, Why)
											? int(Page)
											: kUnknown;
									}
								}
							}
						}
						else
						{
							Note(Out.Unverified, Name, Rec,
								"no mapping contract for " + Operand);
							Now = Fixed;
						}
						if (Mnem != "call")
						{
							Finish(Now, Rec);
							Now = Before; // the taken tail call never falls through
						}
						if (bConditional)
						{
							if (Pos + 1 < Body.size())
							{
								Queue.push_back({Pos + 1, Before});
							}
							else
							{
								State Tail = Before;
								Finish(Tail, Rec);
							}
						}
					}
					bTerminal = (Mnem != "call") && !bConditional;
				}

				if (!bTerminal)
				{
					if (Pos + 1 < Body.size())
					{
						Queue.push_back({Pos + 1, Now});
					}
					else
					{
						Note(Out.Unverified, Name, Rec,
							"falls outside declared routine boundary");
					}
				}
			}
			Out.Checked.push_back(Name);
		}
	}

	for (const AsmIndex::Edge& E : Index.Edges())
	{
		if (E.bResolved)
		{
			++Out.DirectSites;
			// A resolved but unbanked target (page -1) counts as a crossing:
			// the inventory is about sites worth looking at, not proofs.
			if (E.TargetPage != E.Page)
			{
				++Out.CrossPageSites;
			}
		}
	}
	if (Out.Unverified.size() > kUnverifiedLimit)
	{
		Out.OmittedUnverified = Out.Unverified.size() - kUnverifiedLimit;
		Out.Unverified.resize(kUnverifiedLimit);
	}
	return true;
}
