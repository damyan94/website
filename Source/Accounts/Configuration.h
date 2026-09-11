#pragma once

#include "Types.h"
#include <filesystem>

namespace Accounts
{
struct Configuration
{
	Configuration(const Json::Value& settings,
				  const Json::Value& listeners,
				  const std::filesystem::path& configDirectory,
				  const std::filesystem::path& publicRoot);

	std::string connection;
	std::string origin;
	std::string authority;
	std::string cookieName;
	std::filesystem::path uiRoot;
	std::filesystem::path schemaFile;
	std::filesystem::path customerSchemaFile;
	std::filesystem::path activitySchemaFile;
	std::filesystem::path deliverySchemaFile;
	bool secure = true;
	StoreSettings store;

private:
	void		   ConfigureOrigin(const Json::Value& settings, const Json::Value& listeners);
	void		   ConfigureResources(const Json::Value& settings, const std::filesystem::path& configDirectory);
	void		   ConfigureUi(const Json::Value& settings);
	void		   ConfigureEmail(const Json::Value&		   settings,
								  const std::filesystem::path& configDirectory,
								  const std::filesystem::path& publicRoot);
	void		   ConfigureSessions(const Json::Value& settings);
};
} // namespace Accounts
