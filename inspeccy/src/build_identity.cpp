#include "build_identity.h"
#include "json_parser.h"
#include "sha1.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <set>

bool ValidateBuildManifest(const std::string& Path, const std::string& LoadedSha1, std::vector<std::string>& SldPaths,
	const std::string& Override, std::string& Error)
{
	try
	{
		std::ifstream F(Path, std::ios::binary);
		if (!F)
		{
			throw std::runtime_error("cannot read build manifest: " + Path);
		}
		F.seekg(0, std::ios::end);
		if (F.tellg() > 1024 * 1024)
		{
			throw std::runtime_error("manifest exceeds 1 MiB");
		}
		F.seekg(0);
		std::string Text((std::istreambuf_iterator<char>(F)), {}), ParseError;
		JsonValue Root;
		if (!ParseJson(Text, Root, ParseError))
		{
			throw std::runtime_error(ParseError);
		}
		const auto* Version = Root.Find("version");
		uint64_t V = 0;
		if (!Version || !Version->TryAsUInt(V) || V != 1)
		{
			throw std::runtime_error("manifest version must be 1");
		}
		auto String = [](const JsonValue& O, const char* K) -> std::string
		{
			const auto* S = O.Find(K);
			if (!S || !S->IsString() || S->m_S.empty())
			{
				throw std::runtime_error(std::string("missing manifest field: ") + K);
			}
			return S->m_S;
		};
		const auto* Image = Root.Find("image");
		const auto* Symbols = Root.Find("symbols");
		if (!Image || !Image->IsObject() || !Symbols || !Symbols->IsArray() || Symbols->m_A.empty())
		{
			throw std::runtime_error("manifest needs image and symbols");
		}
		if (String(*Image, "sha1") != LoadedSha1 && Override.empty())
		{
			throw std::runtime_error("build manifest does not match loaded program (or legacy dump has no identity)");
		}
		const auto Base = std::filesystem::absolute(Path).parent_path();
		std::vector<std::string> Declared;
		std::set<std::string> Hashes;
		for (const auto& S : Symbols->m_A)
		{
			const auto File = (Base / String(S, "path")).lexically_normal().string();
			const auto Hash = String(S, "sha1");
			if (Sha1FileHex(File.c_str()) != Hash)
			{
				throw std::runtime_error("symbol file differs from manifest: " + File);
			}
			Declared.push_back(File);
			Hashes.insert(Hash);
		}
		for (const auto& File : SldPaths)
		{
			if (!Hashes.count(Sha1FileHex(File.c_str())))
			{
				throw std::runtime_error("--sld file is not part of this build: " + File);
			}
		}
		SldPaths = Declared;
		return true;
	}
	catch (const std::exception& E)
	{
		Error = E.what();
		return false;
	}
}
