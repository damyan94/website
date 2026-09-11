#pragma once

#include "PublicContent.h"
#include <json/json.h>
#include <mutex>
#include <vector>

namespace Accounts
{
class Module;
}

namespace ContentEditor
{
// Edits the reusable services/categories/locales content contract, not arbitrary
// JSON paths or server configuration. A site enables this module explicitly.
class ServiceEditor
{
public:
	ServiceEditor(std::filesystem::path					   source,
				  const std::filesystem::path&			   publicRoot,
				  std::filesystem::path					   backups,
				  std::vector<std::string>				   currencies,
				  std::shared_ptr<PublicContent::Snapshot> snapshot);
	~ServiceEditor();
	ServiceEditor(const ServiceEditor&)			   = delete;
	ServiceEditor& operator=(const ServiceEditor&) = delete;
	Json::Value	   Read();
	Json::Value	   Save(const std::string& actor, const Json::Value& request, bool create);

private:
	Json::Value				 ReadDocument() const;
	Json::Value				 View(const Json::Value& document) const;
	void					 ValidateDocument(const Json::Value& document) const;
	void					 ValidateService(const Json::Value& service, const Json::Value& document) const;
	std::filesystem::path	 m_Source;
	std::filesystem::path	 m_Backups;
	std::vector<std::string> m_Currencies;
	std::shared_ptr<PublicContent::Snapshot> m_Snapshot;
	std::mutex								 m_Mutex;
	int										 m_Lock = -1;
};

void RegisterHandlers(Accounts::Module& accounts, std::shared_ptr<ServiceEditor> editor);
} // namespace ContentEditor
