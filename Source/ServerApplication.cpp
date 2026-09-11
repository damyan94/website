#include "stdafx.h"

#include "ServerApplication.h"

#include "PublicContent.h"
#if WEBSITE_ENABLE_RESERVATIONS
#include "Reservations/Module.h"
#endif
#if WEBSITE_ENABLE_ACCOUNTS
#include "Accounts/Module.h"
#endif
#if WEBSITE_ENABLE_CONTENT_EDITOR
#include "ContentEditor/ServiceEditor.h"
#endif

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>

ServerApplication::ServerApplication(int argC, char** argV)
	: Felis::Application(argC, argV)
{
}

Felis::ApplicationError ServerApplication::OnInit()
{
	auto& logger = Felis::Logger::GetGlobalLogger();
	logger.SetLogPrefix("WebSiteBackend");
	logger.SetLogLevel(Felis::ELogLevel::Info);

	auto& args = GetCommandLineArguments();
	args.EnableArgumentValidation(true);
	args.AddAllowedArgument("help");
	args.AddAllowedArgument("config");
	args.AddAllowedArgument("migrate-accounts");
	args.AddAllowedArgument("migrate-reservations");
	args.AddAllowedArgument("bootstrap-admin");
	args.AddAllowedArgument("reset-account-password");
	args.AddAllowedArgument("password-stdin");

	Felis::NonAllowedArgumentsContainer unexpectedArguments;
	const auto							validationError = args.ValidateNamedArguments(unexpectedArguments);
	if (validationError || !args.GetPositionalArguments().empty())
	{
		for (const auto& argument : unexpectedArguments)
		{
			LogError("Unexpected argument: --", argument);
		}

		if (!args.GetPositionalArguments().empty())
		{
			LogError("Positional arguments are not supported.");
		}

		PrintUsage();
		return Felis::EApplicationErrorCode::InvalidCommandLineArguments;
	}

	m_ShowHelp = args.HasArgument("help");
	ReturnIf(m_ShowHelp, Felis::EApplicationErrorCode::Success);
	for (const char* command : {"migrate-accounts", "bootstrap-admin", "reset-account-password"})
	{
		if (!args.HasArgument(command))
			continue;
		if (!m_AccountsCommand.empty())
		{
			LogError("Only one accounts command may be supplied.");
			return Felis::EApplicationErrorCode::InvalidCommandLineArguments;
		}
		m_AccountsCommand = command;
		if (m_AccountsCommand != "migrate-accounts")
		{
			if (args.GetOrDefault(command, m_AccountEmail, std::string()) || m_AccountEmail.empty())
			{
				LogError("The accounts command requires an email address.");
				return Felis::EApplicationErrorCode::InvalidCommandLineArguments;
			}
		}
	}
	m_PasswordStdin		  = args.HasArgument("password-stdin");
	m_ReservationsCommand = args.HasArgument("migrate-reservations");
	if (m_ReservationsCommand && !m_AccountsCommand.empty())
	{
		LogError("Run each migration command separately.");
		return Felis::EApplicationErrorCode::InvalidCommandLineArguments;
	}
	if (m_PasswordStdin && (m_AccountsCommand.empty() || m_AccountsCommand == "migrate-accounts"))
	{
		LogError("--password-stdin requires an account provisioning command.");
		return Felis::EApplicationErrorCode::InvalidCommandLineArguments;
	}

	std::string configPath;
	const auto	argumentError = args.GetOrDefault("config", configPath, std::string("Config/server.json"));
	if (argumentError || configPath.empty())
	{
		LogError("--config requires a non-empty file path. Use --config=PATH.");
		return Felis::EApplicationErrorCode::InvalidCommandLineArguments;
	}

	try
	{
		const auto configError = LoadConfiguration(std::filesystem::absolute(configPath).lexically_normal());
		ReturnIf(configError, configError);

		if (!m_AccountsCommand.empty())
		{
			if (!m_AccountsEnabled)
				throw std::runtime_error("Enable the accounts module before running accounts commands");
#if WEBSITE_ENABLE_ACCOUNTS
			m_Accounts->RunCommand(m_AccountsCommand, m_AccountEmail, m_PasswordStdin);
#endif
			return Felis::EApplicationErrorCode::Success;
		}
#if WEBSITE_ENABLE_ACCOUNTS
		if (m_ReservationsCommand && !m_ReservationsEnabled)
			throw std::runtime_error("Enable reservations before running its migration");
#if WEBSITE_ENABLE_RESERVATIONS
		if (m_Reservations)
		{
			m_Reservations->Prepare(m_Accounts->Connection(), m_ReservationsCommand);
			m_Accounts->SetScheduledEmailHooks(m_Reservations->EmailHooks());
		}
#endif
		if (m_ReservationsCommand)
			return Felis::EApplicationErrorCode::Success;
		if (m_Accounts)
			m_Accounts->Start();
#endif
		RegisterHandlers();
	}
	catch (const std::exception& exception)
	{
		LogError("Server initialization failed: ", exception.what());
		return Felis::EApplicationErrorCode::InitializationFailed;
	}

	return Felis::EApplicationErrorCode::Success;
}

Felis::ApplicationError ServerApplication::LoadConfiguration(const std::filesystem::path& configPath)
{
	std::ifstream input(configPath);
	if (!input.is_open())
	{
		LogError("Cannot open server configuration: ", configPath.string());
		return Felis::EApplicationErrorCode::InitializationFailed;
	}

	Json::CharReaderBuilder reader;
	reader["collectComments"] = false;
	reader["failIfExtra"]	  = true;
	reader["rejectDupKeys"]	  = true;

	Json::Value config;
	std::string parseErrors;
	if (!Json::parseFromStream(reader, input, &config, &parseErrors) || input.bad())
	{
		LogError("Cannot read server configuration: ", configPath.string(), ". ", parseErrors);
		return Felis::EApplicationErrorCode::InitializationFailed;
	}

	const auto validation = ValidateConfiguration(config, configPath);
	if (validation)
		return validation;
	const auto configured = ConfigureModules(config, configPath);
	if (configured)
		return configured;
	drogon::app().loadConfigJson(config);
	LogInfo("Loaded server configuration: ", configPath.string());
	return Felis::EApplicationErrorCode::Success;
}

Felis::ApplicationError ServerApplication::ValidateConfiguration(Json::Value&				  config,
																 const std::filesystem::path& configPath) const
{
	if (!config.isObject() || !config["app"].isObject() || !config["listeners"].isArray() ||
		config["listeners"].empty())
	{
		LogError("Server configuration requires an 'app' object and a non-empty 'listeners' array.");
		return Felis::EApplicationErrorCode::InitializationFailed;
	}

	for (const auto& listener : config["listeners"])
	{
		if (!listener.isObject() || !listener["address"].isString() || listener["address"].asString().empty() ||
			!listener["port"].isUInt() || listener["port"].asUInt() == 0 || listener["port"].asUInt() > 65535)
		{
			LogError("Each listener requires an address and an integer port between 1 and 65535.");
			return Felis::EApplicationErrorCode::InitializationFailed;
		}
	}

	// Resolve these paths before Drogon loads them so they do not depend on the working directory.
	for (const char* key : {"document_root", "upload_path"})
	{
		auto& value = config["app"][key];
		if (!value.isString() || value.asString().empty())
		{
			LogError("Server configuration requires a non-empty app.", key, " path.");
			return Felis::EApplicationErrorCode::InitializationFailed;
		}

		auto path = std::filesystem::path(value.asString());
		if (path.is_relative())
		{
			path = configPath.parent_path() / path;
		}

		value = path.lexically_normal().string();
	}

	if (config.isMember("custom_config") && !config["custom_config"].isObject())
	{
		LogError("custom_config must be an object when provided.");
		return Felis::EApplicationErrorCode::InitializationFailed;
	}
	return Felis::EApplicationErrorCode::Success;
}

Felis::ApplicationError ServerApplication::ConfigureModules(const Json::Value&			 config,
															const std::filesystem::path& configPath)
{
	if (config.isMember("custom_config"))
	{
		std::filesystem::path contentPath;
		const auto&			  customConfig = config["custom_config"];

		if (customConfig.isMember("public_content_file"))
		{
			const auto& value = customConfig["public_content_file"];
			if (!value.isString() || value.asString().empty())
			{
				LogError("custom_config.public_content_file must be a non-empty path when provided.");
				return Felis::EApplicationErrorCode::InitializationFailed;
			}

			auto path = std::filesystem::path(value.asString());
			if (path.is_relative())
			{
				path = configPath.parent_path() / path;
			}
			contentPath		= path.lexically_normal();
			m_PublicContent = std::make_shared<PublicContent::Snapshot>(PublicContent::Load(contentPath));
		}
		ConfigureAccounts(config, configPath);
		ConfigureContentEditor(config, configPath, contentPath);
	}

	ConfigureReservations(config, configPath);
	if (m_ReservationsCommand && !m_ReservationsEnabled)
		throw std::runtime_error("Enable reservations before migration");
	return Felis::EApplicationErrorCode::Success;
}

void ServerApplication::ConfigureAccounts(const Json::Value&							config,
										  [[maybe_unused]] const std::filesystem::path& configPath)
{
	const auto& customConfig = config["custom_config"];
	if (!customConfig.isMember("accounts"))
		return;
	const auto& accounts = customConfig["accounts"];
	if (!accounts.isObject() || !accounts["enabled"].isBool())
		throw std::runtime_error("custom_config.accounts requires a boolean enabled field");
	m_AccountsEnabled = accounts["enabled"].asBool();
	if (m_AccountsEnabled)
	{
#if WEBSITE_ENABLE_ACCOUNTS
		m_Accounts = std::make_shared<Accounts::Module>(
			accounts, config["listeners"], configPath.parent_path(), config["app"]["document_root"].asString());
#else
		throw std::runtime_error(
			"Accounts are enabled in configuration but were not compiled; use WEBSITE_ENABLE_ACCOUNTS=ON");
#endif
	}
}

void ServerApplication::ConfigureContentEditor(const Json::Value&							 config,
											   [[maybe_unused]] const std::filesystem::path& configPath,
											   [[maybe_unused]] const std::filesystem::path& contentPath)
{
	const auto& customConfig = config["custom_config"];
	if (!customConfig.isMember("content_editor"))
		return;
	const auto& editor = customConfig["content_editor"];
	if (!editor.isObject() || !editor["enabled"].isBool())
		throw std::runtime_error("custom_config.content_editor requires a boolean enabled field");
	m_ContentEditorEnabled = editor["enabled"].asBool();
	if (m_ContentEditorEnabled)
	{
#if WEBSITE_ENABLE_CONTENT_EDITOR
		if (!m_AccountsEnabled || !m_PublicContent)
			throw std::runtime_error("Content editing requires accounts and a public content file");
		if (!editor["backups_directory"].isString() || editor["backups_directory"].asString().empty() ||
			!editor["currencies"].isArray() || editor["currencies"].empty())
			throw std::runtime_error("Content editing requires backups_directory and allowed currencies");
		auto backups = std::filesystem::path(editor["backups_directory"].asString());
		if (backups.is_relative())
			backups = configPath.parent_path() / backups;
		std::vector<std::string> currencies;
		for (const auto& currency : editor["currencies"])
		{
			if (!currency.isString())
				throw std::runtime_error("Content editor currencies must be strings");
			currencies.push_back(currency.asString());
		}
		// Provisioning commands can run while a server owns the content lock.
		if (m_AccountsCommand.empty() && !m_ReservationsCommand)
			m_ContentEditor = std::make_shared<ContentEditor::ServiceEditor>(contentPath,
																			 config["app"]["document_root"].asString(),
																			 backups,
																			 std::move(currencies),
																			 m_PublicContent);
#else
		throw std::runtime_error("Content editing was not compiled; use WEBSITE_ENABLE_CONTENT_EDITOR=ON");
#endif
	}
}

void ServerApplication::ConfigureReservations(const Json::Value&							config,
											  [[maybe_unused]] const std::filesystem::path& configPath)
{
	if (!config["custom_config"].isMember("reservations"))
		return;
	const auto& reservations = config["custom_config"]["reservations"];
	if (!reservations.isObject() || !reservations["enabled"].isBool())
		throw std::runtime_error("reservations.enabled must be a boolean");
	m_ReservationsEnabled = reservations["enabled"].asBool();
	if (m_ReservationsEnabled)
	{
#if WEBSITE_ENABLE_RESERVATIONS
		if (!m_AccountsEnabled || !m_PublicContent)
			throw std::runtime_error("Reservations require accounts and public service content");
		m_Reservations = std::make_shared<Reservations::Module>(
			reservations, configPath.parent_path(), m_PublicContent, m_Accounts->Settings());
#else
		throw std::runtime_error("Reservations were not compiled; use WEBSITE_ENABLE_RESERVATIONS=ON");
#endif
	}
}

void ServerApplication::RegisterHandlers()
{
#if WEBSITE_ENABLE_RESERVATIONS
	if (m_Reservations)
		m_Reservations->RegisterHandlers(*m_Accounts);
#endif
#if WEBSITE_ENABLE_ACCOUNTS
	if (m_Accounts)
		m_Accounts->RegisterHandlers();
#endif
#if WEBSITE_ENABLE_CONTENT_EDITOR
	if (m_ContentEditor)
		ContentEditor::RegisterHandlers(*m_Accounts, m_ContentEditor);
#endif
	drogon::app().registerHandler(
		"/api/v1/features",
		[enabled = m_AccountsEnabled, editor = m_ContentEditorEnabled, reservations = m_ReservationsEnabled](
			const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback)
		{
			Json::Value features;
			features["accounts"]	  = enabled;
			features["contentEditor"] = editor;
			features["reservations"]  = reservations;
			auto response			  = drogon::HttpResponse::newHttpJsonResponse(features);
			response->addHeader("Cache-Control", "no-store");
			callback(response);
		},
		{drogon::Get});
	if (m_PublicContent)
	{
		PublicContent::RegisterHandlers(m_PublicContent);
	}

	drogon::app().registerHandler(
		"/health",
		[](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& callback)
		{
			Json::Value body;
			body["status"] = "ok";

			auto response = drogon::HttpResponse::newHttpJsonResponse(body);
			response->setStatusCode(drogon::k200OK);
			response->addHeader("Cache-Control", "no-store");
			callback(response);
		},
		{drogon::Get});
}

Felis::ApplicationError ServerApplication::OnRun()
{
	if (m_ShowHelp || !m_AccountsCommand.empty() || m_ReservationsCommand)
	{
		if (m_ShowHelp)
			PrintUsage();
		return Felis::EApplicationErrorCode::Success;
	}

	try
	{
		LogInfo("Starting HTTP server. Static document root: ", drogon::app().getDocumentRoot());

		// Drogon owns the event loop and its default SIGINT/SIGTERM shutdown handlers.
		drogon::app().run();
	}
	catch (const std::exception& exception)
	{
		if (drogon::app().isRunning())
		{
			drogon::app().quit();
		}

		LogError("HTTP server failed: ", exception.what());
		return Felis::EApplicationErrorCode::RuntimeFailed;
	}

	LogInfo("HTTP server stopped.");
	return Felis::EApplicationErrorCode::Success;
}

Felis::ApplicationError ServerApplication::OnDeinit()
{
#if WEBSITE_ENABLE_ACCOUNTS
	if (m_Accounts)
		m_Accounts->Stop();
#endif
	if (!Felis::Logger::GetGlobalLogger().Flush())
	{
		return Felis::EApplicationErrorCode::DeinitializationFailed;
	}

	return Felis::EApplicationErrorCode::Success;
}

void ServerApplication::PrintUsage() const
{
	const auto& programName = GetCommandLineArguments().GetProgramName();

	std::cout << "Usage: " << (programName.empty() ? "WebSiteBackend" : programName) << " [--config=PATH] [--help]\n"
			  << "\n"
			  << "  --config=PATH  Server JSON configuration; default: Config/server.json\n"
			  << "  --help         Show this help without starting the server.\n"
			  << "  --migrate-accounts              Apply the accounts schema and exit.\n"
			  << "  --migrate-reservations          Apply reservation schema and configured resources.\n"
			  << "  --bootstrap-admin=EMAIL         Create the first administrator and exit.\n"
			  << "  --reset-account-password=EMAIL  Local owner password recovery; revokes sessions.\n"
			  << "  --password-stdin                Explicit provisioning input from a pipe.\n"
			  << "\n"
			  << "The configuration file path is relative to the working directory.\n"
			  << "app.document_root, app.upload_path and custom_config.public_content_file\n"
			  << "are relative to the configuration file.\n";
}
