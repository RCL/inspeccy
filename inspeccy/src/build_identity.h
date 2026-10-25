#pragma once
#include <string>
#include <vector>

// Validate a build producer's manifest and populate SLD paths. An override
// records deliberate use of mismatched/legacy state, never invents its identity.
bool ValidateBuildManifest(const std::string& Path, const std::string& LoadedSha1, std::vector<std::string>& SldPaths,
	const std::string& Override, std::string& Error);
