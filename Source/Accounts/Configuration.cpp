#include "Configuration.h"
#include "AccountStore.h"
#include "Crypto.h"
#include "stdafx.h"

#include <cstdlib>
#include <regex>

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

} // namespace

Configuration::Configuration(const Json::Value&			settings,
			   const Json::Value&			listeners,
			   const std::filesystem::path& configDirectory,
			   const std::filesystem::path& publicRoot)
{
	const auto	environment = Required(settings, "database_url_env");
	const auto* databaseConnection = std::getenv(environment.c_str());
	if (!databaseConnection || !*databaseConnection)
		throw std::runtime_error("Accounts database environment variable is not set: " + environment);
	connection = databaseConnection;
	ConfigureOrigin(settings, listeners);
	ConfigureResources(settings, configDirectory);
	ConfigureUi(settings);
	ConfigureEmail(settings, configDirectory, publicRoot);
	ConfigureSessions(settings);
}

void Configuration::ConfigureOrigin(const Json::Value& settings, const Json::Value& listeners)
{
	origin = Required(settings, "public_origin");
	// One canonical origin. No credentials, paths, wildcard hosts or forwarded-header inference.
	const std::regex originPattern(R"(^(https?)://([A-Za-z0-9.-]+)(:[0-9]{1,5})?$)");
	std::smatch		 match;
	if (!std::regex_match(origin, match, originPattern))
		throw std::runtime_error("accounts.public_origin must be a canonical HTTP(S) origin without a path");
	secure		= match[1] == "https";
	authority		= origin.substr(origin.find("://") + 3);
	const auto port = match[3].matched ? std::stoi(match[3].str().substr(1)) : (secure ? 443 : 80);
	if (port < 1 || port > 65535 || (match[3].matched && port == (secure ? 443 : 80)))
		throw std::runtime_error("Invalid origin port; omit the default HTTP(S) port");
	if (settings.isMember("allow_insecure_loopback") && !settings["allow_insecure_loopback"].isBool())
		throw std::runtime_error("accounts.allow_insecure_loopback must be boolean");
	if (!secure)
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
	cookieName = std::string(secure ? "__Host-wsb-" : "wsb-dev-") + TokenHash(origin).substr(0, 16);
}

void Configuration::ConfigureResources(const Json::Value& settings, const std::filesystem::path& configDirectory)
{
	uiRoot			 = (configDirectory / Required(settings, "ui_root")).lexically_normal();
	schemaFile		 = (configDirectory / Required(settings, "schema_file")).lexically_normal();
	customerSchemaFile = settings.isMember("customer_schema_file")
							   ? (configDirectory / Required(settings, "customer_schema_file")).lexically_normal()
							   : schemaFile.parent_path() / "002_customer_email.sql";
	activitySchemaFile = settings.isMember("activity_schema_file")
							   ? (configDirectory / Required(settings, "activity_schema_file")).lexically_normal()
							   : schemaFile.parent_path() / "003_user_activity.sql";
	deliverySchemaFile = settings.isMember("delivery_schema_file")
							   ? (configDirectory / Required(settings, "delivery_schema_file")).lexically_normal()
							   : schemaFile.parent_path() / "004_email_delivery.sql";
}

void Configuration::ConfigureUi(const Json::Value& settings)
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
			store.ui.siteName = plainText(ui["site_name"], 100);
		if (ui.isMember("default_locale"))
		{
			store.ui.defaultLocale = plainText(ui["default_locale"], 2);
			if (store.ui.defaultLocale != "en" && store.ui.defaultLocale != "bg")
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
				store.ui.supportText[locale] = plainText(ui["support_text"][locale], 500);
			}
		}
	}
}

void Configuration::ConfigureEmail(const Json::Value&			 settings,
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
			auto& options	  = store.email;
			options.transport = Required(email, "transport");
			if (options.transport != "local_outbox" && options.transport != "resend")
				throw std::runtime_error("Email transport must be local_outbox or resend");
			options.enabled		 = true;
			options.registration = email["registration_enabled"].asBool();
			options.newsletters	 = email["newsletters_enabled"].asBool();
			options.sender		 = NormalizedEmail(Required(email, "sender"));
			options.origin		 = origin;
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
					if (secure || !resend["test_port"].isInt() || resend["test_port"].asInt() < 1 ||
						resend["test_port"].asInt() > 65535)
						throw std::runtime_error("Resend test_port requires explicit HTTP loopback development");
					options.apiOrigin = "http://127.0.0.1:" + std::to_string(resend["test_port"].asInt());
				}
				else if (!secure)
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

void Configuration::ConfigureSessions(const Json::Value& settings)
{
	store.idleSeconds	   = Seconds(settings, "idle_seconds", 3600, 86400);
	store.absoluteSeconds = Seconds(settings, "absolute_seconds", 43200, 604800);
	if (store.absoluteSeconds < store.idleSeconds)
		throw std::runtime_error("Absolute session lifetime must not be shorter than idle lifetime");
	if (settings.isMember("locales"))
	{
		const auto& locales = settings["locales"];
		if (!locales.isArray() || locales.empty() || locales.size() > 32)
			throw std::runtime_error("accounts.locales must be a nonempty array");
		store.locales.clear();
		for (const auto& locale : locales)
		{
			if (!locale.isString() ||
				!std::regex_match(locale.asString(), std::regex("[A-Za-z]{2,3}(-[A-Za-z0-9]{2,8})*")))
				throw std::runtime_error("Invalid accounts locale");
			store.locales.push_back(locale.asString());
		}
	}
}

} // namespace Accounts
