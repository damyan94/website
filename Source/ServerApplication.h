#pragma once

#include "Felis/Application/Application.h"
#include <memory>

namespace Json
{
class Value;
}

namespace PublicContent
{
class Snapshot;
}
#if WEBSITE_ENABLE_RESERVATIONS
namespace Reservations
{
class Module;
}
#endif
#if WEBSITE_ENABLE_CONTENT_EDITOR
namespace ContentEditor
{
class ServiceEditor;
}
#endif

#if WEBSITE_ENABLE_ACCOUNTS
namespace Accounts
{
class Module;
}
#endif

class ServerApplication final : public Felis::Application
{
public:
	ServerApplication(int argC, char** argV);
	~ServerApplication() override = default;

private:
	Felis::ApplicationError OnInit() override;
	Felis::ApplicationError OnRun() override;
	Felis::ApplicationError OnDeinit() override;

	Felis::ApplicationError LoadConfiguration(const std::filesystem::path& configPath);
	Felis::ApplicationError ValidateConfiguration(Json::Value& config, const std::filesystem::path& configPath) const;
	Felis::ApplicationError ConfigureModules(const Json::Value& config, const std::filesystem::path& configPath);
	void					ConfigureAccounts(const Json::Value& config, const std::filesystem::path& configPath);
	void					ConfigureContentEditor(const Json::Value&			config,
												   const std::filesystem::path& configPath,
												   const std::filesystem::path& contentPath);
	void					ConfigureReservations(const Json::Value& config, const std::filesystem::path& configPath);
	void					RegisterHandlers();
	void					PrintUsage() const;

private:
	bool									 m_ShowHelp = false;
	std::shared_ptr<PublicContent::Snapshot> m_PublicContent;
	std::string								 m_AccountsCommand;
	std::string								 m_AccountEmail;
	bool									 m_PasswordStdin		= false;
	bool									 m_AccountsEnabled		= false;
	bool									 m_ContentEditorEnabled = false;
	bool									 m_ReservationsEnabled	= false;
	bool									 m_ReservationsCommand	= false;
#if WEBSITE_ENABLE_RESERVATIONS
	std::shared_ptr<Reservations::Module> m_Reservations;
#endif
#if WEBSITE_ENABLE_CONTENT_EDITOR
	std::shared_ptr<ContentEditor::ServiceEditor> m_ContentEditor;
#endif
#if WEBSITE_ENABLE_ACCOUNTS
	std::shared_ptr<Accounts::Module> m_Accounts;
#endif
};
