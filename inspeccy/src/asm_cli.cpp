#include "asm_cli.h"

#include <asm/index.h>
#include "diagnostic_json.h"
#include "json_parser.h"
#include "sha1.h"

#include <stdio.h>
#include <string.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

using DiagnosticJson::Quote;

struct Options
{
	std::string Command, Symbol, Config, SourceRoot, Image, ManifestOut;
	std::vector<std::string> Sld;
	long Limit = 60;
	bool bJson = false;
};

// One of the command flags, or empty. Only the first is honoured, so
// "--asm-layout --asm-banks" is a usage error rather than a silent choice.
std::string CommandOf(const std::string& A)
{
	if (A == "--asm-layout" || A == "--asm-banks" || A == "--asm-query" || A == "--write-manifest")
	{
		return A;
	}
	return std::string();
}

std::string Slurp(const std::string& Path, size_t Limit)
{
	std::ifstream F(Path, std::ios::binary);
	if (!F)
	{
		throw std::runtime_error("cannot read " + Path);
	}
	F.seekg(0, std::ios::end);
	if (F.tellg() < 0 || size_t(F.tellg()) > Limit)
	{
		throw std::runtime_error(Path + " is unreasonably large");
	}
	F.seekg(0);
	return std::string((std::istreambuf_iterator<char>(F)), std::istreambuf_iterator<char>());
}

JsonValue ReadConfig(const std::string& Path)
{
	JsonValue Root;
	std::string Error;
	if (!ParseJson(Slurp(Path, 4u << 20), Root, Error))
	{
		throw std::runtime_error(Path + ": " + Error);
	}
	const JsonValue* Version = Root.Find("version");
	uint64_t V = 0;
	if (Version == nullptr || !Version->TryAsUInt(V) || V != 1)
	{
		throw std::runtime_error("configuration version must be 1");
	}
	return Root;
}

void Write(const std::string& Path, const std::string& Text)
{
	std::ofstream F(Path, std::ios::binary);
	if (!F || !(F << Text))
	{
		throw std::runtime_error("cannot write " + Path);
	}
}

std::string Hex4(uint32_t V)
{
	char Buf[16];
	snprintf(Buf, sizeof(Buf), "%04X", V);
	return Buf;
}

// ------------------------------------------------------------------ layout
int DoLayout(const AsmIndex& Index, const Options& Opt)
{
	const JsonValue Config = ReadConfig(Opt.Config);
	std::vector<AsmLayoutRow> Rows;
	std::string Error;
	if (!AsmLayout(Index, Config, Rows, Error))
	{
		throw std::runtime_error(Error);
	}
	if (Opt.bJson)
	{
		std::string Out = "[\n";
		for (size_t IdxI = 0; IdxI < Rows.size(); ++IdxI)
		{
			const AsmLayoutRow& R = Rows[IdxI];
			Out += "  {\"name\": " + Quote(R.Name) + ", \"page\": " + std::to_string(R.Page)
				+ ", \"start\": " + std::to_string(R.Start) + ", \"end\": " + std::to_string(R.End)
				+ ", \"limit\": " + std::to_string(R.Limit) + ", \"used\": " + std::to_string(R.Used)
				+ ", \"free\": " + std::to_string(R.Free) + "}";
			Out += (IdxI + 1 < Rows.size()) ? ",\n" : "\n";
		}
		fputs((Out + "]\n").c_str(), stdout);
		return 0;
	}
	printf("%-24s %4s %6s %6s %6s %6s\n", "REGION", "PAGE", "START", "END", "USED", "FREE");
	for (const AsmLayoutRow& R : Rows)
	{
		printf("%-24s %4ld %06lX %06lX %6ld %6ld\n", R.Name.c_str(), R.Page, R.Start, R.End, R.Used,
			R.Free);
	}
	return 0;
}

// ------------------------------------------------------------------- banks
std::string NotesJson(const std::vector<AsmBankNote>& Notes, const char* Indent)
{
	if (Notes.empty())
	{
		return "[]";
	}
	std::string Out = "[\n";
	for (size_t IdxI = 0; IdxI < Notes.size(); ++IdxI)
	{
		const AsmBankNote& N = Notes[IdxI];
		Out += std::string(Indent) + "  {\"routine\": " + Quote(N.Routine)
			+ ", \"address\": " + std::to_string(N.Address) + ", \"file\": " + Quote(N.File)
			+ ", \"line\": " + std::to_string(N.Line) + ", \"message\": " + Quote(N.Message) + "}";
		Out += (IdxI + 1 < Notes.size()) ? ",\n" : "\n";
	}
	return Out + Indent + "]";
}

std::string NamesJson(const std::vector<std::string>& Names)
{
	std::string Out = "[";
	for (size_t IdxI = 0; IdxI < Names.size(); ++IdxI)
	{
		Out += (IdxI != 0 ? ", " : "") + Quote(Names[IdxI]);
	}
	return Out + "]";
}

int DoBanks(const AsmIndex& Index, const Options& Opt)
{
	const JsonValue Config = ReadConfig(Opt.Config);
	AsmBankReport Report;
	std::string Error;
	if (!AsmCheckBanks(Index, Config, Report, Error))
	{
		throw std::runtime_error(Error);
	}
	if (Opt.bJson)
	{
		std::string Out = "{\n";
		Out += "  \"checked\": " + NamesJson(Report.Checked) + ",\n";
		Out += "  \"external_assumptions\": " + NamesJson(Report.ExternalAssumptions) + ",\n";
		Out += "  \"errors\": " + NotesJson(Report.Errors, "  ") + ",\n";
		Out += "  \"unverified\": " + NotesJson(Report.Unverified, "  ") + ",\n";
		Out += "  \"omitted_unverified\": " + std::to_string(Report.OmittedUnverified) + ",\n";
		Out += "  \"inventory\": {\"direct_sites\": " + std::to_string(Report.DirectSites)
			+ ", \"cross_page_sites\": " + std::to_string(Report.CrossPageSites)
			+ ", \"annotated_routines\": " + std::to_string(Report.AnnotatedRoutines) + "},\n";
		Out += "  \"coverage\": " + Quote("Only declared routine ranges are checked; external "
										  "contracts are assumptions. Unlisted code is unverified.")
			+ "\n}\n";
		fputs(Out.c_str(), stdout);
	}
	else
	{
		printf("%zu declared routines checked, %zu external contracts assumed\n",
			Report.Checked.size(), Report.ExternalAssumptions.size());
		for (const AsmBankNote& N : Report.Errors)
		{
			printf("ERROR      %s $%s  %s:%d  %s\n", N.Routine.c_str(), Hex4(N.Address).c_str(),
				N.File.c_str(), N.Line, N.Message.c_str());
		}
		for (const AsmBankNote& N : Report.Unverified)
		{
			printf("UNVERIFIED %s $%s  %s:%d  %s\n", N.Routine.c_str(), Hex4(N.Address).c_str(),
				N.File.c_str(), N.Line, N.Message.c_str());
		}
		if (Report.OmittedUnverified != 0)
		{
			printf("... and %zu more unverified paths\n", Report.OmittedUnverified);
		}
		printf("%zu errors, %zu unverified paths; %zu direct sites, %zu cross-page, %zu annotated\n",
			Report.Errors.size(), Report.Unverified.size(), Report.DirectSites,
			Report.CrossPageSites, Report.AnnotatedRoutines);
		puts("Only declared routine ranges are checked; external contracts are assumptions. "
			 "Unlisted code is unverified.");
	}
	return Report.Errors.empty() ? 0 : 1;
}

// ------------------------------------------------------------------- query
int DoQuery(const AsmIndex& Index, const Options& Opt)
{
	if (Opt.Limit < 1 || Opt.Limit > 500)
	{
		throw std::runtime_error("limit must be 1..500");
	}
	const size_t Limit = size_t(Opt.Limit);
	const AsmIndex::Record* Sym = Index.Symbol(Opt.Symbol);
	if (Sym == nullptr)
	{
		throw std::runtime_error("unknown symbol: " + Opt.Symbol);
	}
	const std::vector<const AsmIndex::Record*> Body = Index.Body(Opt.Symbol);
	const uint32_t End = Body.empty() ? Sym->Address : Body.back()->Address + 1;

	std::vector<const AsmIndex::Edge*> Callers, Callees;
	for (const AsmIndex::Edge& E : Index.Edges())
	{
		if (E.Target == Opt.Symbol)
		{
			Callers.push_back(&E);
		}
		if (E.Page == Sym->Page && E.Address >= Sym->Address && E.Address < End)
		{
			Callees.push_back(&E);
		}
	}
	auto Clipped = [&](size_t N)
	{
		return N > Limit ? N - Limit : size_t(0);
	};

	if (Opt.bJson)
	{
		auto EdgesJson = [&](const std::vector<const AsmIndex::Edge*>& Edges)
		{
			std::string Out = "[";
			for (size_t IdxI = 0; IdxI < Edges.size() && IdxI < Limit; ++IdxI)
			{
				const AsmIndex::Edge& E = *Edges[IdxI];
				Out += (IdxI != 0 ? ",\n    " : "\n    ");
				Out += "{\"owner\": " + Quote(E.Owner) + ", \"operation\": " + Quote(E.Operation)
					+ ", \"operand\": " + Quote(E.Operand) + ", \"target\": "
					+ (E.Target.empty() ? std::string("null") : Quote(E.Target))
					+ ", \"target_page\": " + std::to_string(E.TargetPage)
					+ ", \"file\": " + Quote(E.File) + ", \"line\": " + std::to_string(E.Line)
					+ "}";
			}
			return Out + (Edges.empty() ? "]" : "\n  ]");
		};
		std::string Out = "{\n  \"name\": " + Quote(Opt.Symbol)
			+ ",\n  \"page\": " + std::to_string(Sym->Page)
			+ ",\n  \"address\": " + std::to_string(Sym->Address) + ",\n  \"slot\": "
			+ (Sym->Page >= 0 ? std::to_string(Sym->Address / 8192) : std::string("null"))
			+ ",\n  \"file\": " + Quote(Sym->File) + ",\n  \"line\": " + std::to_string(Sym->Line)
			+ ",\n  \"body\": [";
		for (size_t IdxI = 0; IdxI < Body.size() && IdxI < Limit; ++IdxI)
		{
			Out += (IdxI != 0 ? ",\n    " : "\n    ");
			Out += "{\"address\": " + std::to_string(Body[IdxI]->Address)
				+ ", \"text\": " + Quote(Body[IdxI]->Text) + "}";
		}
		Out += (Body.empty() ? "]" : "\n  ]");
		Out += ",\n  \"callers\": " + EdgesJson(Callers);
		Out += ",\n  \"callees\": " + EdgesJson(Callees);
		Out += ",\n  \"truncated\": {\"body\": " + std::to_string(Clipped(Body.size()))
			+ ", \"callers\": " + std::to_string(Clipped(Callers.size()))
			+ ", \"callees\": " + std::to_string(Clipped(Callees.size())) + "}\n}\n";
		fputs(Out.c_str(), stdout);
		return 0;
	}

	printf("%s: page %d, address $%s, slot %s\n", Opt.Symbol.c_str(), Sym->Page,
		Hex4(Sym->Address).c_str(),
		Sym->Page >= 0 ? std::to_string(Sym->Address / 8192).c_str() : "none");
	printf("Source: %s:%d\n", Sym->File.c_str(), Sym->Line);
	for (size_t IdxI = 0; IdxI < Body.size() && IdxI < Limit; ++IdxI)
	{
		printf("  %s  %s\n", Hex4(Body[IdxI]->Address).c_str(), Body[IdxI]->Text.c_str());
	}
	for (int Which = 0; Which < 2; ++Which)
	{
		const std::vector<const AsmIndex::Edge*>& Edges = Which == 0 ? Callers : Callees;
		puts(Which == 0 ? "callers:" : "callees:");
		for (size_t IdxI = 0; IdxI < Edges.size() && IdxI < Limit; ++IdxI)
		{
			const AsmIndex::Edge& E = *Edges[IdxI];
			const std::string To = E.Target.empty() ? "UNRESOLVED " + E.Operand : E.Target;
			printf("  %s -> %s (%s:%d)\n", E.Owner.c_str(), To.c_str(), E.File.c_str(), E.Line);
		}
	}
	if (Clipped(Body.size()) + Clipped(Callers.size()) + Clipped(Callees.size()) != 0)
	{
		printf("Omitted: body %zu, callers %zu, callees %zu\n", Clipped(Body.size()),
			Clipped(Callers.size()), Clipped(Callees.size()));
	}
	return 0;
}

// ---------------------------------------------------------------- manifest
// Written after a successful build and before any input changes; the paths
// are stored relative to the manifest so the tree can move.
int DoManifest(const Options& Opt)
{
	if (Opt.Image.empty())
	{
		throw std::runtime_error("--write-manifest needs --image");
	}
	namespace fs = std::filesystem;
	const fs::path Out = fs::absolute(Opt.ManifestOut);
	auto Entry = [&](const std::string& Path)
	{
		const std::string Hash = Sha1FileHex(fs::absolute(Path).string().c_str());
		if (Hash.empty())
		{
			throw std::runtime_error("cannot read " + Path);
		}
		const std::string Relative
			= fs::absolute(Path).lexically_relative(Out.parent_path()).generic_string();
		return "{\"path\": " + Quote(Relative.empty() ? Path : Relative) + ", \"sha1\": "
			+ Quote(Hash) + "}";
	};
	std::string Text = "{\n  \"version\": 1,\n  \"image\": " + Entry(Opt.Image) + ",\n  \"symbols\": [";
	for (size_t IdxI = 0; IdxI < Opt.Sld.size(); ++IdxI)
	{
		Text += (IdxI != 0 ? ",\n    " : "\n    ") + Entry(Opt.Sld[IdxI]);
	}
	Text += "\n  ]\n}\n";
	Write(Opt.ManifestOut, Text);
	puts(Opt.ManifestOut.c_str());
	return 0;
}

} // namespace

bool RunAsmCommand(int Argc, char** Argv, int& OutExitCode)
{
	Options Opt;
	for (int IdxI = 1; IdxI < Argc && Opt.Command.empty(); ++IdxI)
	{
		Opt.Command = CommandOf(Argv[IdxI]);
	}
	if (Opt.Command.empty())
	{
		return false;
	}

	try
	{
		for (int IdxI = 1; IdxI < Argc; ++IdxI)
		{
			const std::string A = Argv[IdxI];
			auto Next = [&]() -> std::string
			{
				if (IdxI + 1 >= Argc)
				{
					throw std::runtime_error(A + " needs a value");
				}
				return Argv[++IdxI];
			};
			if (A == "--json")
			{
				Opt.bJson = true;
			}
			else if (A == "--sld")
			{
				Opt.Sld.push_back(Next());
			}
			else if (A == "--asm-config")
			{
				Opt.Config = Next();
			}
			else if (A == "--source-root")
			{
				Opt.SourceRoot = Next();
			}
			else if (A == "--image")
			{
				Opt.Image = Next();
			}
			else if (A == "--limit")
			{
				Opt.Limit = strtol(Next().c_str(), nullptr, 10);
			}
			else if (A == "--asm-query")
			{
				Opt.Symbol = Next();
			}
			else if (A == "--write-manifest")
			{
				Opt.ManifestOut = Next();
			}
			else if (A == "--asm-layout" || A == "--asm-banks")
			{
				// The command itself, already recognised.
			}
			else
			{
				throw std::runtime_error(Opt.Command + " does not take " + A);
			}
		}
		if (Opt.Sld.empty())
		{
			throw std::runtime_error(Opt.Command + " needs --sld");
		}
		if (Opt.Command == "--write-manifest")
		{
			OutExitCode = DoManifest(Opt);
			return true;
		}
		if (Opt.Sld.size() != 1)
		{
			throw std::runtime_error(Opt.Command + " takes exactly one --sld");
		}
		if (Opt.Command != "--asm-query" && Opt.Config.empty())
		{
			throw std::runtime_error(Opt.Command + " needs --asm-config");
		}

		AsmIndex Index;
		std::string Error;
		if (!Index.Load(Opt.Sld[0], Opt.SourceRoot, Error))
		{
			throw std::runtime_error(Error);
		}
		OutExitCode = Opt.Command == "--asm-layout" ? DoLayout(Index, Opt)
			: Opt.Command == "--asm-banks"			? DoBanks(Index, Opt)
													: DoQuery(Index, Opt);
	}
	catch (const std::exception& E)
	{
		fprintf(stderr, "%s\n", E.what());
		OutExitCode = 2;
	}
	return true;
}
