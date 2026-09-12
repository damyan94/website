#pragma once

#include <filesystem>
#include <json/json.h>
#include <string>
#include <vector>

namespace Reservations
{
// Validates deployment settings and prepares/checks the existing schema and resources.
class Configuration
{
public:
	Configuration(Json::Value settings,
				  const std::filesystem::path& configDirectory,
				  const std::vector<std::string>& locales);
	void Prepare(const std::string& connection, bool migrate);

	const Json::Value& Settings() const
	{
		return m_Settings;
	}

private:
	Json::Value m_Settings;
	std::filesystem::path m_Schema;
};
} // namespace Reservations
