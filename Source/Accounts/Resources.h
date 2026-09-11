#pragma once

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace Accounts
{
inline std::string ReadResource(const std::filesystem::path& path, std::size_t maximum)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("Cannot read accounts resource: " + path.string());
	std::string data(maximum + 1, '\0');
	input.read(data.data(), data.size());
	if (input.bad())
		throw std::runtime_error("Cannot read accounts resource");
	data.resize(input.gcount());
	if (data.size() > maximum)
		throw std::runtime_error("Accounts resource too large");
	return data;
}

} // namespace Accounts
