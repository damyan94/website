#include "Module.h"
#include "Crypto.h"
#include "stdafx.h"

#include <charconv>
#include <cstdlib>
#include <drogon/Cookie.h>
#include <drogon/HttpAppFramework.h>
#include <fstream>
#include <iostream>
#include <regex>
#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace Accounts
{
namespace
{
std::string Required(const Json::Value& object, const char* key)
{
	if (!object[key].isString() || object[key].asString().empty())
		throw std::runtime_error(std::string("accounts requires ") + key);
	return object[key].asString();
}

int Seconds(const Json::Value& object, const char* key, int fallback, int maximum)
{
	if (!object.isMember(key))
		return fallback;
	if (!object[key].isInt() || object[key].asInt() < 60 || object[key].asInt() > maximum)
		throw std::runtime_error(std::string("Invalid accounts timeout: ") + key);
	return object[key].asInt();
}

std::string ReadFile(const std::filesystem::path& path, std::size_t maximum)
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

void Headers(const drogon::HttpResponsePtr& response)
{
	response->addHeader("Cache-Control", "no-store");
	response->addHeader("X-Content-Type-Options", "nosniff");
	response->addHeader("X-Frame-Options", "DENY");
	response->addHeader("Referrer-Policy", "no-referrer");
	response->addHeader("Content-Security-Policy",
						"default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self'; "
						"object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'");
}

Reply Error(int status, const char* message)
{
	Reply reply;
	reply.status		= status;
	reply.body["error"] = message;
	reply.clearCookie	= status == 401;
	return reply;
}

UserListQuery UsersQuery(const drogon::HttpRequestPtr& request)
{
	UserListQuery query;
	const auto&	  parameters = request->getParameters();
	for (const auto* key : {"page", "q", "role", "enabled", "sort", "order"})
		if (parameters.find(key) != parameters.end())
			query.paginated = true;
	if (!query.paginated)
		return query;
	if (parameters.find("after") != parameters.end())
		throw RequestError(400, "Do not combine page and cursor pagination");
	query.search  = request->getParameter("q");
	query.role	  = request->getParameter("role");
	query.enabled = request->getParameter("enabled");
	if (query.search.size() > 400 || query.role.size() > 20 || query.enabled.size() > 5)
		throw RequestError(400, "Invalid user filters");
	if (parameters.find("sort") != parameters.end())
		query.sort = request->getParameter("sort");
	if (parameters.find("order") != parameters.end())
		query.order = request->getParameter("order");
	if (parameters.find("page") != parameters.end())
	{
		const auto& value  = request->getParameter("page");
		const auto	parsed = std::from_chars(value.data(), value.data() + value.size(), query.page);
		if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size())
			throw RequestError(400, "Invalid page number");
	}
	return query;
}
} // namespace

Module::Module(const Json::Value&			settings,
			   const Json::Value&			listeners,
			   const std::filesystem::path& configDirectory,
			   const std::filesystem::path& publicRoot)
{
	const auto	environment = Required(settings, "database_url_env");
	const auto* connection	= std::getenv(environment.c_str());
	if (!connection || !*connection)
		throw std::runtime_error("Accounts database environment variable is not set: " + environment);
	m_Connection = connection;
	ConfigureOrigin(settings, listeners);
	ConfigureResources(settings, configDirectory);
	ConfigureUi(settings);
	ConfigureEmail(settings, configDirectory, publicRoot);
	ConfigureSessions(settings);
}

void Module::ConfigureOrigin(const Json::Value& settings, const Json::Value& listeners)
{
	m_Origin = Required(settings, "public_origin");
	// One canonical origin. No credentials, paths, wildcard hosts or forwarded-header inference.
	const std::regex originPattern(R"(^(https?)://([A-Za-z0-9.-]+)(:[0-9]{1,5})?$)");
	std::smatch		 match;
	if (!std::regex_match(m_Origin, match, originPattern))
		throw std::runtime_error("accounts.public_origin must be a canonical HTTP(S) origin without a path");
	m_Secure		= match[1] == "https";
	m_Authority		= m_Origin.substr(m_Origin.find("://") + 3);
	const auto port = match[3].matched ? std::stoi(match[3].str().substr(1)) : (m_Secure ? 443 : 80);
	if (port < 1 || port > 65535 || (match[3].matched && port == (m_Secure ? 443 : 80)))
		throw std::runtime_error("Invalid origin port; omit the default HTTP(S) port");
	if (settings.isMember("allow_insecure_loopback") && !settings["allow_insecure_loopback"].isBool())
		throw std::runtime_error("accounts.allow_insecure_loopback must be boolean");
	if (!m_Secure)
	{
		if (!settings.get("allow_insecure_loopback", false).asBool() || match[2] != "127.0.0.1")
			throw std::runtime_error("Accounts require HTTPS; HTTP is allowed only for explicit 127.0.0.1 development");
		for (const auto& listener : listeners)
			if (listener["address"].asString() != "127.0.0.1")
				throw std::runtime_error("Insecure accounts require loopback listeners");
		bool matchingListener = false;
		for (const auto& listener : listeners)
			if (listener["port"].asInt() == port && !listener.get("https", false).asBool())
				matchingListener = true;
		if (!matchingListener)
			throw std::runtime_error("The development origin must match a local HTTP listener");
	}
	m_CookieName = std::string(m_Secure ? "__Host-wsb-" : "wsb-dev-") + TokenHash(m_Origin).substr(0, 16);
}

void Module::ConfigureResources(const Json::Value& settings, const std::filesystem::path& configDirectory)
{
	m_UiRoot			 = (configDirectory / Required(settings, "ui_root")).lexically_normal();
	m_SchemaFile		 = (configDirectory / Required(settings, "schema_file")).lexically_normal();
	m_CustomerSchemaFile = settings.isMember("customer_schema_file")
							   ? (configDirectory / Required(settings, "customer_schema_file")).lexically_normal()
							   : m_SchemaFile.parent_path() / "002_customer_email.sql";
	m_ActivitySchemaFile = settings.isMember("activity_schema_file")
							   ? (configDirectory / Required(settings, "activity_schema_file")).lexically_normal()
							   : m_SchemaFile.parent_path() / "003_user_activity.sql";
	m_DeliverySchemaFile = settings.isMember("delivery_schema_file")
							   ? (configDirectory / Required(settings, "delivery_schema_file")).lexically_normal()
							   : m_SchemaFile.parent_path() / "004_email_delivery.sql";
}

void Module::ConfigureUi(const Json::Value& settings)
{
	if (settings.isMember("ui"))
	{
		const auto& ui = settings["ui"];
		if (!ui.isObject())
			throw std::runtime_error("accounts.ui must be an object");
		for (const auto& key : ui.getMemberNames())
			if (key != "site_name" && key != "default_locale" && key != "support_text")
				throw std::runtime_error("Unknown accounts.ui setting");
		const auto plainText = [](const Json::Value& value, int maximum)
		{
			if (!value.isString() || TextLength(value.asString()) < 0 || TextLength(value.asString()) > maximum)
				throw std::runtime_error("Invalid accounts.ui text");
			return value.asString();
		};
		if (ui.isMember("site_name"))
			m_Settings.ui.siteName = plainText(ui["site_name"], 100);
		if (ui.isMember("default_locale"))
		{
			m_Settings.ui.defaultLocale = plainText(ui["default_locale"], 2);
			if (m_Settings.ui.defaultLocale != "en" && m_Settings.ui.defaultLocale != "bg")
				throw std::runtime_error("Admin interface supports en and bg");
		}
		if (ui.isMember("support_text"))
		{
			if (!ui["support_text"].isObject())
				throw std::runtime_error("ui.support_text must be an object");
			for (const auto& locale : ui["support_text"].getMemberNames())
			{
				if (locale != "en" && locale != "bg")
					throw std::runtime_error("Unsupported interface translation");
				m_Settings.ui.supportText[locale] = plainText(ui["support_text"][locale], 500);
			}
		}
	}
}

void Module::ConfigureEmail(const Json::Value&			 settings,
							const std::filesystem::path& configDirectory,
							const std::filesystem::path& publicRoot)
{
	if (settings.isMember("customer_email"))
	{
		const auto& email = settings["customer_email"];
		if (!email.isObject() || !email["enabled"].isBool())
			throw std::runtime_error("customer_email requires boolean enabled");
		if (email["enabled"].asBool())
		{
			if (!email["registration_enabled"].isBool() || !email["newsletters_enabled"].isBool())
				throw std::runtime_error("Configure registration_enabled and newsletters_enabled");
			auto& options	  = m_Settings.email;
			options.transport = Required(email, "transport");
			if (options.transport != "local_outbox" && options.transport != "resend")
				throw std::runtime_error("Email transport must be local_outbox or resend");
			options.enabled		 = true;
			options.registration = email["registration_enabled"].asBool();
			options.newsletters	 = email["newsletters_enabled"].asBool();
			options.sender		 = NormalizedEmail(Required(email, "sender"));
			options.origin		 = m_Origin;
			options.footer		 = Required(email, "footer");
			if (TextLength(options.footer) < 1 || TextLength(options.footer) > 500)
				throw std::runtime_error("Invalid newsletter footer");
			if (options.transport == "resend")
			{
				const auto& resend = email["resend"];
				if (!resend.isObject())
					throw std::runtime_error("Configure customer_email.resend");
				for (const auto& key : resend.getMemberNames())
					if (key != "api_key_env" && key != "webhook_secret_env" && key != "request_seconds" &&
						key != "test_port")
						throw std::runtime_error("Unknown Resend setting");
				auto secret = [&](const char* key)
				{
					const auto name = Required(resend, key);
					if (!std::regex_match(name, std::regex("[A-Za-z_][A-Za-z0-9_]{0,127}")))
						throw std::runtime_error("Invalid email environment variable name");
					const auto* value = std::getenv(name.c_str());
					if (!value || !*value || std::string_view(value).size() > 512 || TextLength(value) < 1)
						throw std::runtime_error("Email environment variable is missing or invalid: " + name);
					return std::string(value);
				};
				options.apiKey = secret("api_key_env");
				if (options.apiKey.find_first_not_of(
						"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
					throw std::runtime_error("Invalid Resend API key");
				options.webhookKey = DecodeWebhookSecret(secret("webhook_secret_env"));
				if (resend.isMember("request_seconds"))
				{
					if (!resend["request_seconds"].isInt() || resend["request_seconds"].asInt() < 1 ||
						resend["request_seconds"].asInt() > 30)
						throw std::runtime_error("Resend request_seconds must be between 1 and 30");
					options.requestSeconds = resend["request_seconds"].asInt();
				}
				if (resend.isMember("test_port"))
				{
					if (m_Secure || !resend["test_port"].isInt() || resend["test_port"].asInt() < 1 ||
						resend["test_port"].asInt() > 65535)
						throw std::runtime_error("Resend test_port requires explicit HTTP loopback development");
					options.apiOrigin = "http://127.0.0.1:" + std::to_string(resend["test_port"].asInt());
				}
				else if (!m_Secure)
					throw std::runtime_error("Real email delivery requires an HTTPS public_origin");
				return;
			}
			const auto path = configDirectory / Required(email, "outbox_directory");
			options.outbox	= std::filesystem::weakly_canonical(path);
			const auto root = std::filesystem::weakly_canonical(publicRoot);
			if (std::filesystem::is_symlink(path) ||
				std::mismatch(root.begin(), root.end(), options.outbox.begin(), options.outbox.end()).first ==
					root.end())
				throw std::runtime_error("Email outbox must be private and outside the public root");
		}
	}
}

void Module::ConfigureSessions(const Json::Value& settings)
{
	m_Settings.idleSeconds	   = Seconds(settings, "idle_seconds", 3600, 86400);
	m_Settings.absoluteSeconds = Seconds(settings, "absolute_seconds", 43200, 604800);
	if (m_Settings.absoluteSeconds < m_Settings.idleSeconds)
		throw std::runtime_error("Absolute session lifetime must not be shorter than idle lifetime");
	if (settings.isMember("locales"))
	{
		const auto& locales = settings["locales"];
		if (!locales.isArray() || locales.empty() || locales.size() > 32)
			throw std::runtime_error("accounts.locales must be a nonempty array");
		m_Settings.locales.clear();
		for (const auto& locale : locales)
		{
			if (!locale.isString() ||
				!std::regex_match(locale.asString(), std::regex("[A-Za-z]{2,3}(-[A-Za-z0-9]{2,8})*")))
				throw std::runtime_error("Invalid accounts locale");
			m_Settings.locales.push_back(locale.asString());
		}
	}
}

Module::~Module()
{
	Stop();
}

std::string Module::ReadPassword(bool fromStdin) const
{
	std::string password;
	if (fromStdin)
	{
		// Explicit pipe mode is for provisioning/tests; never a command-line password.
		std::getline(std::cin, password);
		return password;
	}
#ifdef _WIN32
	const auto handle = GetStdHandle(STD_INPUT_HANDLE);
	DWORD	   mode	  = 0;
	if (!GetConsoleMode(handle, &mode) || !SetConsoleMode(handle, mode & ~ENABLE_ECHO_INPUT))
		throw std::runtime_error("Use an interactive terminal or --password-stdin");

	struct Restore
	{
		HANDLE handle;
		DWORD  mode;

		~Restore()
		{
			SetConsoleMode(handle, mode);
		}
	} restore{handle, mode};
#else
	termios original{};
	if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &original) != 0)
		throw std::runtime_error("Use an interactive terminal or --password-stdin");
	auto hidden = original;
	hidden.c_lflag &= ~ECHO;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0)
		throw std::runtime_error("Cannot disable terminal echo");

	struct Restore
	{
		termios value;

		~Restore()
		{
			tcsetattr(STDIN_FILENO, TCSAFLUSH, &value);
		}
	} restore{original};
#endif
	std::cout << "Password (15–128 characters): " << std::flush;
	std::getline(std::cin, password);
	std::string confirmation;
	std::cout << "\nRepeat password: " << std::flush;
	std::getline(std::cin, confirmation);
	std::cout << '\n';
	if (!ConstantEqual(password, confirmation))
		throw std::runtime_error("Passwords do not match");
	return password;
}

void Module::RunCommand(const std::string& command, const std::string& email, bool passwordStdin)
{
	if (command == "migrate-accounts")
	{
		Database database(m_Connection);
		// Serialize the entire version decision across concurrent provisioners.
		database.Query("SELECT pg_advisory_lock(70123001)");
		// Earlier migrations are immutable; apply only the missing versions.
		if (database.Query("SELECT to_regclass('accounts.schema_version') IS NOT NULL").Get(0, 0) != "t")
			database.Script(ReadFile(m_SchemaFile, 1024 * 1024));
		auto version = database.Query("SELECT version FROM accounts.schema_version");
		if (version.Count() == 1 && version.Get(0, 0) == "1")
			database.Script(ReadFile(m_CustomerSchemaFile, 1024 * 1024));
		version = database.Query("SELECT version FROM accounts.schema_version");
		if (version.Count() == 1 && version.Get(0, 0) == "2")
			database.Script(ReadFile(m_ActivitySchemaFile, 1024 * 1024));
		version = database.Query("SELECT version FROM accounts.schema_version");
		if (version.Count() == 1 && version.Get(0, 0) == "3")
			database.Script(ReadFile(m_DeliverySchemaFile, 1024 * 1024));
		database.CheckSchema();
		std::cout << "Accounts schema ready.\n";
		return;
	}
	AccountStore store(m_Connection, m_Settings);
	const auto	 password = ReadPassword(passwordStdin);
	if (command == "bootstrap-admin")
		store.Bootstrap(email, password);
	else if (command == "reset-account-password")
		store.ResetPassword(email, password);
	else
		throw std::runtime_error("Unknown accounts command");
	std::cout << "Account operation completed.\n";
}

void Module::SetScheduledEmailHooks(ScheduledEmailHooks hooks)
{
	if (m_EmailWorker)
		throw std::logic_error("Configure scheduled email before starting accounts");
	m_EmailHooks = std::move(hooks);
}

void Module::Start()
{
	m_Settings.dummyPasswordHash = HashPassword(RandomToken());
	// Connect before registering routes: an explicitly enabled but broken module
	// fails startup instead of silently presenting an unauthenticated alternative.
	std::vector<std::unique_ptr<AccountStore>> stores;
	for (int i = 0; i < 2; ++i)
		stores.push_back(std::make_unique<AccountStore>(m_Connection, m_Settings));
	for (auto& store : stores)
	{
		m_Workers.emplace_back(
			[this, store = std::move(store)]() mutable
			{
				for (;;)
				{
					Job job;
					{
						std::unique_lock lock(m_Mutex);
						m_Condition.wait(lock, [this] { return m_Stopping || !m_Jobs.empty(); });
						if (m_Stopping)
							return;
						job = std::move(m_Jobs.front());
						m_Jobs.pop_front();
					}
					job(*store);
				}
			});
	}
	if (m_Settings.email.enabled)
	{
		m_EmailWorker = std::make_unique<EmailWorker>(m_Connection, m_Settings.email, m_EmailHooks);
		m_EmailWorker->Start();
	}
}

void Module::Stop()
{
	if (m_EmailWorker)
		m_EmailWorker->Stop();
	{
		std::lock_guard lock(m_Mutex);
		m_Stopping = true;
		m_Jobs.clear();
	}
	m_Condition.notify_all();
	for (auto& worker : m_Workers)
		if (worker.joinable())
			worker.join();
	m_Workers.clear();
}

bool Module::AllowPasswordWork(const std::string& peer, const std::string& identity)
{
	std::lock_guard lock(m_LimitMutex);
	const auto		now = std::chrono::steady_clock::now();
	for (auto it = m_Limits.begin(); it != m_Limits.end();)
		if (it->second.expires <= now)
			it = m_Limits.erase(it);
		else
			++it;
	const std::pair<std::string, int> limits[] = {
		{"global", 120}, {"peer:" + peer, 60}, {"identity:" + TokenHash(identity), 10}};
	for (const auto& [key, maximum] : limits)
	{
		auto it = m_Limits.find(key);
		if (it != m_Limits.end() && it->second.count >= maximum)
			return false;
		if (it == m_Limits.end() && m_Limits.size() >= 2048)
			return false;
	}
	for (const auto& [key, maximum] : limits)
	{
		(void)maximum;
		auto [it, inserted] = m_Limits.try_emplace(key, Limit{now + std::chrono::seconds(60), 0});
		(void)inserted;
		++it->second.count;
	}
	return true;
}

void Module::Respond(const Callback& callback, Reply reply) const
{
	auto response = drogon::HttpResponse::newHttpJsonResponse(reply.body);
	response->setStatusCode(static_cast<drogon::HttpStatusCode>(reply.status));
	Headers(response);
	if (reply.status == 429 || reply.status == 503)
		response->addHeader("Retry-After", "60");
	if (!reply.sessionToken.empty() || reply.clearCookie)
	{
		drogon::Cookie cookie(m_CookieName, reply.clearCookie ? "" : reply.sessionToken);
		cookie.setPath("/");
		cookie.setHttpOnly(true);
		cookie.setSecure(m_Secure);
		cookie.setSameSite(drogon::Cookie::SameSite::kLax);
		cookie.setMaxAge(reply.clearCookie ? 0 : m_Settings.absoluteSeconds);
		response->addCookie(cookie);
	}
	callback(response);
}

void Module::Dispatch(Action						action,
					  const drogon::HttpRequestPtr& request,
					  Callback						callback,
					  ProtectedOperation			operation,
					  std::string					requiredRole,
					  std::size_t					maximumBodyBytes,
					  bool							oneClick,
					  DatabaseOperation				databaseOperation)
{
	try
	{
		auto pending			  = ParseRequest(action, request, bool(databaseOperation), oneClick, maximumBodyBytes);
		pending.operation		  = std::move(operation);
		pending.databaseOperation = std::move(databaseOperation);
		pending.requiredRole	  = std::move(requiredRole);
		Submit([this, pending = std::move(pending), callback](AccountStore& store)
			   { Execute(store, pending, callback); });
	}
	catch (const RequestError& error)
	{
		Respond(callback, Error(error.status, error.what()));
	}
	catch (const std::exception&)
	{
		Respond(callback, Error(400, "Invalid account request"));
	}
}

Json::Value Module::ParseBody(const drogon::HttpRequestPtr& request,
							  bool							mutation,
							  bool							databaseOperation,
							  bool							oneClick,
							  std::size_t					maximumBodyBytes) const
{
	if (request->getHeader("host") != m_Authority ||
		(!oneClick && request->getHeader("sec-fetch-site") == "cross-site"))
		throw RequestError(403, "Request origin is not allowed");
	Json::Value body(Json::objectValue);
	if (databaseOperation && !mutation)
	{
		if (request->getParameters().size() > 10)
			throw RequestError(400, "Too many query fields");
		for (const auto& [key, value] : request->getParameters())
		{
			if (key.size() > 40 || value.size() > 400)
				throw RequestError(400, "Query field too long");
			body[key] = value;
		}
	}
	if (oneClick)
	{
		if (request->body() != "List-Unsubscribe=One-Click" ||
			request->getHeader("content-type") != "application/x-www-form-urlencoded")
			throw RequestError(400, "Invalid unsubscribe request");
		body["token"] = request->getParameter("token");
	}
	else if (mutation)
	{
		if (request->getHeader("origin") != m_Origin || request->getHeader("x-accounts-request") != "1")
			throw RequestError(403, "Request origin is not allowed");
		const auto& type = request->getHeader("content-type");
		if (type != "application/json" && type != "application/json; charset=utf-8")
			throw RequestError(415, "Use application/json");
		const auto bytes = request->body();
		if (bytes.size() > maximumBodyBytes)
			throw RequestError(413, "Request body is too large");
		Json::CharReaderBuilder builder;
		builder["collectComments"]	   = false;
		builder["allowComments"]	   = false;
		builder["allowTrailingCommas"] = false;
		builder["rejectDupKeys"]	   = true;
		builder["failIfExtra"]		   = true;
		builder["stackLimit"]		   = 8;
		std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
		std::string						  errors;
		if (!reader->parse(bytes.data(), bytes.data() + bytes.size(), &body, &errors) || !body.isObject())
			throw RequestError(400, "Invalid JSON object");
	}
	return body;
}

Module::PendingRequest Module::ParseRequest(Action						  action,
											const drogon::HttpRequestPtr& request,
											bool						  databaseOperation,
											bool						  oneClick,
											std::size_t					  maximumBodyBytes)
{
	const bool mutation = action != Action::Me && action != Action::ListUsers && action != Action::EmailOptions &&
						  action != Action::ListCampaigns;
	PendingRequest parsed;
	parsed.action	= action;
	parsed.mutation = mutation;
	parsed.body		= ParseBody(request, mutation, databaseOperation, oneClick, maximumBodyBytes);
	auto token		= request->getCookie(m_CookieName);
	auto csrf		= request->getHeader("x-csrf-token");
	if (token.size() > 64 || csrf.size() > 64)
		throw RequestError(400, "Invalid request token");
	CheckRequestLimit(action, request, parsed.body, token);
	auto cursor = request->getParameter("after");
	if (cursor.size() > 18)
		throw RequestError(400, "Invalid pagination cursor");
	auto users	  = action == Action::ListUsers ? UsersQuery(request) : UserListQuery{};
	parsed.token  = std::move(token);
	parsed.csrf	  = std::move(csrf);
	parsed.cursor = std::move(cursor);
	parsed.users  = std::move(users);
	return parsed;
}

void Module::CheckRequestLimit(Action						 action,
							   const drogon::HttpRequestPtr& request,
							   const Json::Value&			 body,
							   const std::string&			 token)
{
	if (action == Action::Login || action == Action::CreateUser || action == Action::ChangePassword ||
		action == Action::RegisterEmail || action == Action::ForgotPassword || action == Action::CompleteRegistration ||
		action == Action::CompleteReset || action == Action::ConfirmEmail || action == Action::VerifyEmail ||
		action == Action::ChangeEmail || action == Action::NewsletterPreference || action == Action::Unsubscribe ||
		action == Action::PreviewCampaign || action == Action::SendCampaign)
	{
		const bool address =
			action == Action::Login || action == Action::RegisterEmail || action == Action::ForgotPassword;
		const auto identity = address		  ? NormalizedEmail(body.get("email", "").asString())
							  : token.empty() ? body.get("token", request->peerAddr().toIp()).asString()
											  : token;
		if (!AllowPasswordWork(request->peerAddr().toIp(), identity))
			throw RequestError(429, "Too many attempts; try again in one minute");
	}
}

void Module::Submit(Job job)
{
	{
		std::lock_guard lock(m_Mutex);
		if (m_Stopping || m_Jobs.size() >= 64)
			throw RequestError(503, "Accounts are busy; try again shortly");
		m_Jobs.emplace_back(std::move(job));
	}
	m_Condition.notify_one();
}

void Module::Execute(AccountStore& store, const PendingRequest& request, const Callback& callback) const
{
	const auto& [action, body, token, csrf, cursor, users, mutation, operation, databaseOperation, requiredRole] =
		request;
	try
	{
		if (databaseOperation)
		{
			Respond(callback, store.RunDatabaseOperation(token, csrf, mutation, requiredRole, body, databaseOperation));
			return;
		}
		Respond(callback,
				operation ? store.RunProtected(token, csrf, mutation, requiredRole, body, operation)
						  : store.Handle(action, body, token, csrf, cursor, users));
	}
	catch (const RequestError& error)
	{
		Respond(callback, Error(error.status, error.what()));
	}
	catch (const DatabaseError& error)
	{
		if (databaseOperation)
		{
			Respond(callback,
					error.sqlState == "23P01" || error.sqlState == "23505"
						? Error(409, "Reservation conflict; refresh availability")
						: Error(503, "Reservations are temporarily unavailable"));
			return;
		}
		Respond(callback,
				error.sqlState == "23505" ? Error(409, "An account with that email already exists")
										  : Error(503, "Accounts are temporarily unavailable"));
	}
	catch (const std::exception&)
	{
		Respond(callback, Error(500, "Account operation failed"));
	}
}

void Module::RegisterHandlers()
{
	const auto self = shared_from_this();
	const auto route =
		[self](const char* path, drogon::HttpMethod method, Action action, std::size_t maximumBodyBytes = 8192)
	{
		drogon::app().registerHandler(
			path,
			[self, action, maximumBodyBytes](const drogon::HttpRequestPtr& request, Callback&& callback)
			{ self->Dispatch(action, request, std::move(callback), {}, "", maximumBodyBytes); },
			{method});
	};
	route("/api/v1/auth/login", drogon::Post, Action::Login);
	route("/api/v1/auth/logout", drogon::Post, Action::Logout);
	route("/api/v1/me", drogon::Get, Action::Me);
	route("/api/v1/me", drogon::Patch, Action::UpdateMe);
	route("/api/v1/me/password", drogon::Post, Action::ChangePassword);
	route("/api/v1/admin/users", drogon::Get, Action::ListUsers);
	route("/api/v1/admin/users", drogon::Post, Action::CreateUser);
	route("/api/v1/admin/users", drogon::Patch, Action::UpdateUser);
	route("/api/v1/auth/options", drogon::Get, Action::EmailOptions);
	if (m_Settings.email.enabled)
	{
		RegisterEmailDeliveryHandlers();
		route("/api/v1/auth/forgot-password", drogon::Post, Action::ForgotPassword);
		route("/api/v1/auth/reset-password", drogon::Post, Action::CompleteReset);
		route("/api/v1/auth/confirm-email", drogon::Post, Action::ConfirmEmail);
		route("/api/v1/me/verify-email", drogon::Post, Action::VerifyEmail);
		route("/api/v1/me/email", drogon::Post, Action::ChangeEmail);
		if (m_Settings.email.registration)
		{
			route("/api/v1/auth/register", drogon::Post, Action::RegisterEmail);
			route("/api/v1/auth/complete-registration", drogon::Post, Action::CompleteRegistration);
		}
		if (m_Settings.email.newsletters)
		{
			route("/api/v1/me/newsletter", drogon::Post, Action::NewsletterPreference);
			route("/api/v1/newsletter/unsubscribe", drogon::Post, Action::Unsubscribe);
			route("/api/v1/admin/newsletters", drogon::Get, Action::ListCampaigns);
			route("/api/v1/admin/newsletters/preview", drogon::Post, Action::PreviewCampaign, 32768);
			route("/api/v1/admin/newsletters/send", drogon::Post, Action::SendCampaign);
			drogon::app().registerHandler(
				"/api/v1/newsletter/one-click",
				[self](const drogon::HttpRequestPtr& request, Callback&& callback)
				{ self->Dispatch(Action::Unsubscribe, request, std::move(callback), {}, "", 128, true); },
				{drogon::Post});
			RegisterUiAsset("/accounts-assets/newsletters.js", "newsletters.js", "text/javascript; charset=utf-8");
		}
		RegisterUiAsset("/email", "email.html", "text/html; charset=utf-8");
		RegisterUiAsset("/accounts-assets/email.js", "email.js", "text/javascript; charset=utf-8");
	}

	const struct
	{
		const char* path;
		const char* file;
		const char* type;
	} assets[] = {{"/admin", "index.html", "text/html; charset=utf-8"},
				  {"/profile", "index.html", "text/html; charset=utf-8"},
				  {"/accounts-assets/admin.js", "admin.js", "text/javascript; charset=utf-8"},
				  {"/accounts-assets/api.js", "api.js", "text/javascript; charset=utf-8"},
				  {"/accounts-assets/public-account.js", "public-account.js", "text/javascript; charset=utf-8"},
				  {"/accounts-assets/i18n.js", "i18n.js", "text/javascript; charset=utf-8"},
				  {"/accounts-assets/modules.js", "modules.js", "text/javascript; charset=utf-8"},
				  {"/accounts-assets/profile.js", "profile.js", "text/javascript; charset=utf-8"},
				  {"/accounts-assets/users.js", "users.js", "text/javascript; charset=utf-8"},
				  {"/accounts-assets/admin.css", "admin.css", "text/css; charset=utf-8"}};

	for (const auto& asset : assets)
		RegisterUiAsset(asset.path, asset.file, asset.type);
}

void Module::RegisterEmailDeliveryHandlers()
{
	RegisterUiAsset("/accounts-assets/mail.js", "mail.js", "text/javascript; charset=utf-8");
	RegisterDatabaseHandler(
		"/api/v1/admin/mail",
		drogon::Get,
		"admin",
		[transport = m_Settings.email.transport](Database& database, const Identity&, const Json::Value& query)
		{
			Reply reply;
			reply.body = EmailDeliveryStatus(database, query, transport);
			return reply;
		});
	if (m_Settings.email.transport != "resend")
		return;
	const auto self = shared_from_this();
	drogon::app().registerHandler(
		"/api/v1/email/webhook/resend",
		[self](const drogon::HttpRequestPtr& request, Callback&& callback)
		{
			try
			{
				const auto& id = request->getHeader("svix-id");
				if (request->getHeader("host") != self->m_Authority ||
					!VerifyEmailWebhook(self->m_Settings.email.webhookKey,
										id,
										request->getHeader("svix-timestamp"),
										request->getHeader("svix-signature"),
										request->body()))
					throw RequestError(400, "Invalid email webhook");
				Json::CharReaderBuilder builder;
				builder["collectComments"]	   = false;
				builder["allowComments"]	   = false;
				builder["allowTrailingCommas"] = false;
				builder["rejectDupKeys"]	   = true;
				builder["failIfExtra"]		   = true;
				builder["stackLimit"]		   = 8;
				std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
				Json::Value						  body;
				std::string						  errors;
				const auto						  bytes = request->body();
				if (!reader->parse(bytes.data(), bytes.data() + bytes.size(), &body, &errors) || !body.isObject())
					throw RequestError(400, "Invalid email webhook");
				self->Submit(
					[self, id, body = std::move(body), callback](AccountStore& store)
					{
						try
						{
							self->Respond(callback, store.ReceiveEmailEvent(id, body));
						}
						catch (const RequestError& error)
						{
							self->Respond(callback, Error(error.status, error.what()));
						}
						catch (const DatabaseError& error)
						{
							self->Respond(callback,
										  error.sqlState.starts_with("22")
											  ? Error(400, "Invalid email event")
											  : Error(503, "Email event temporarily unavailable"));
						}
						catch (const std::exception&)
						{
							self->Respond(callback, Error(503, "Email event temporarily unavailable"));
						}
					});
			}
			catch (const RequestError& error)
			{
				self->Respond(callback, Error(error.status, error.what()));
			}
			catch (const std::exception&)
			{
				self->Respond(callback, Error(400, "Invalid email webhook"));
			}
		},
		{drogon::Post});
}

void Module::RegisterUiAsset(const char* path, const char* file, const char* contentType)
{
	auto data = ReadFile(m_UiRoot / file, 1024 * 1024);
	drogon::app().registerHandler(
		path,
		[data = std::move(data), type = std::string(contentType)](const drogon::HttpRequestPtr&, Callback&& callback)
		{
			auto response = drogon::HttpResponse::newHttpResponse();
			response->setContentTypeString(type);
			response->setBody(data);
			Headers(response);
			callback(response);
		},
		{drogon::Get});
}

void Module::RegisterProtectedHandler(const char*		 path,
									  drogon::HttpMethod method,
									  std::string		 requiredRole,
									  ProtectedOperation operation,
									  std::size_t		 maximumBodyBytes)
{
	const auto self = shared_from_this();
	drogon::app().registerHandler(
		path,
		[self, method, requiredRole = std::move(requiredRole), operation = std::move(operation), maximumBodyBytes](
			const drogon::HttpRequestPtr& request, Callback&& callback)
		{
			self->Dispatch(method == drogon::Get ? Action::Me : Action::UpdateMe,
						   request,
						   std::move(callback),
						   operation,
						   requiredRole,
						   maximumBodyBytes);
		},
		{method});
}

void Module::RegisterDatabaseHandler(const char*		path,
									 drogon::HttpMethod method,
									 std::string		requiredRole,
									 DatabaseOperation	operation)
{
	const auto self = shared_from_this();
	drogon::app().registerHandler(
		path,
		[self, method, requiredRole = std::move(requiredRole), operation = std::move(operation)](
			const drogon::HttpRequestPtr& request, Callback&& callback)
		{
			self->Dispatch(method == drogon::Get ? Action::Me : Action::UpdateMe,
						   request,
						   std::move(callback),
						   {},
						   requiredRole,
						   8192,
						   false,
						   operation);
		},
		{method});
}
} // namespace Accounts
