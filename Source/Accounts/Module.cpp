#include "Module.h"
#include "Crypto.h"
#include "Provisioning.h"
#include "Resources.h"
#include "stdafx.h"

#include <drogon/HttpAppFramework.h>

namespace Accounts
{
Module::Module(const Json::Value&			settings,
			   const Json::Value&			listeners,
			   const std::filesystem::path& configDirectory,
			   const std::filesystem::path& publicRoot)
	: m_Configuration(settings, listeners, configDirectory, publicRoot),
	  m_Controller(m_Configuration)
{
}

Module::~Module()
{
	Stop();
}

void Module::RunCommand(const std::string& command, const std::string& email, bool passwordStdin)
{
	Provisioning::RunCommand(m_Configuration, command, email, passwordStdin);
}

void Module::SetScheduledEmailHooks(ScheduledEmailHooks hooks)
{
	if (m_EmailWorker)
		throw std::logic_error("Configure scheduled email before starting accounts");
	m_EmailHooks = std::move(hooks);
}

void Module::Start()
{
	m_Configuration.store.dummyPasswordHash = HashPassword(RandomToken());
	// Connect before registering routes: an explicitly enabled but broken module
	// fails startup instead of silently presenting an unauthenticated alternative.
	std::vector<std::unique_ptr<AccountStore>> stores;
	for (int i = 0; i < 2; ++i)
		stores.push_back(std::make_unique<AccountStore>(m_Configuration.connection, m_Configuration.store));
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
	if (m_Configuration.store.email.enabled)
	{
		m_EmailWorker = std::make_unique<EmailWorker>(m_Configuration.connection, m_Configuration.store.email, m_EmailHooks);
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

void Module::Dispatch(Action						action,
					  const drogon::HttpRequestPtr& request,
					  Callback						callback,
					  ProtectedOperation			operation,
					  std::string					requiredRole,
					  std::size_t					maximumBodyBytes,
					  bool							oneClick,
					  DatabaseOperation				databaseOperation)
{
	m_Controller.Dispatch(action,
						  request,
						  std::move(callback),
						  [this](Job job) { Submit(std::move(job)); },
						  std::move(operation),
						  std::move(requiredRole),
						  maximumBodyBytes,
						  oneClick,
						  std::move(databaseOperation));
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
	if (m_Configuration.store.email.enabled)
	{
		RegisterEmailDeliveryHandlers();
		route("/api/v1/auth/forgot-password", drogon::Post, Action::ForgotPassword);
		route("/api/v1/auth/reset-password", drogon::Post, Action::CompleteReset);
		route("/api/v1/auth/confirm-email", drogon::Post, Action::ConfirmEmail);
		route("/api/v1/me/verify-email", drogon::Post, Action::VerifyEmail);
		route("/api/v1/me/email", drogon::Post, Action::ChangeEmail);
		if (m_Configuration.store.email.registration)
		{
			route("/api/v1/auth/register", drogon::Post, Action::RegisterEmail);
			route("/api/v1/auth/complete-registration", drogon::Post, Action::CompleteRegistration);
		}
		if (m_Configuration.store.email.newsletters)
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
		[transport = m_Configuration.store.email.transport](Database& database, const Identity&, const Json::Value& query)
		{
			Reply reply;
			reply.body = EmailDeliveryStatus(database, query, transport);
			return reply;
		});
	if (m_Configuration.store.email.transport != "resend")
		return;
	const auto self = shared_from_this();
	drogon::app().registerHandler(
		"/api/v1/email/webhook/resend",
		[self](const drogon::HttpRequestPtr& request, Callback&& callback)
		{
			self->m_Controller.DispatchWebhook(
				request,
				std::move(callback),
				[self](Job job)
				{
					// Retain the module for the webhook job, as the original handler did.
					self->Submit([self, job = std::move(job)](AccountStore& store) { job(store); });
				});
		},
		{drogon::Post});
}

void Module::RegisterUiAsset(const char* path, const char* file, const char* contentType)
{
	auto data = ReadResource(m_Configuration.uiRoot / file, 1024 * 1024);
	drogon::app().registerHandler(
		path,
		[data = std::move(data), type = std::string(contentType)](const drogon::HttpRequestPtr&, Callback&& callback)
		{
			Controller::RespondWithAsset(data, type, callback);
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
