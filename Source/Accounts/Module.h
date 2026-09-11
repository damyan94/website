#pragma once

#include "AccountStore.h"
#include <condition_variable>
#include <deque>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace Accounts
{
// The only accounts interface consumed by the application lifecycle.
class Module final : public std::enable_shared_from_this<Module>
{
public:
	Module(const Json::Value&			settings,
		   const Json::Value&			listeners,
		   const std::filesystem::path& configDirectory,
		   const std::filesystem::path& publicRoot);
	~Module();
	void RunCommand(const std::string& command, const std::string& email, bool passwordStdin);
	void Start();
	void Stop();
	void SetScheduledEmailHooks(ScheduledEmailHooks hooks);
	void RegisterHandlers();
	void RegisterProtectedHandler(const char*		 path,
								  drogon::HttpMethod method,
								  std::string		 requiredRole,
								  ProtectedOperation operation,
								  std::size_t		 maximumBodyBytes = 8192);
	void RegisterUiAsset(const char* path, const char* file, const char* contentType);
	void RegisterDatabaseHandler(const char*		path,
								 drogon::HttpMethod method,
								 std::string		requiredRole,
								 DatabaseOperation	operation);

	// Used only during module provisioning/startup, never exposed in public options.
	const std::string& Connection() const
	{
		return m_Connection;
	}

	const StoreSettings& Settings() const
	{
		return m_Settings;
	}

private:
	using Callback = std::function<void(const drogon::HttpResponsePtr&)>;
	using Job	   = std::function<void(AccountStore&)>;

	struct PendingRequest
	{
		Action			   action;
		Json::Value		   body;
		std::string		   token;
		std::string		   csrf;
		std::string		   cursor;
		UserListQuery	   users;
		bool			   mutation;
		ProtectedOperation operation;
		DatabaseOperation  databaseOperation;
		std::string		   requiredRole;
	};

	void		   ConfigureOrigin(const Json::Value& settings, const Json::Value& listeners);
	void		   ConfigureResources(const Json::Value& settings, const std::filesystem::path& configDirectory);
	void		   ConfigureUi(const Json::Value& settings);
	void		   ConfigureEmail(const Json::Value&		   settings,
								  const std::filesystem::path& configDirectory,
								  const std::filesystem::path& publicRoot);
	void		   ConfigureSessions(const Json::Value& settings);
	void		   RegisterEmailDeliveryHandlers();
	Json::Value	   ParseBody(const drogon::HttpRequestPtr& request,
							 bool						   mutation,
							 bool						   databaseOperation,
							 bool						   oneClick,
							 std::size_t				   maximumBodyBytes) const;
	PendingRequest ParseRequest(Action						  action,
								const drogon::HttpRequestPtr& request,
								bool						  databaseOperation,
								bool						  oneClick,
								std::size_t					  maximumBodyBytes);
	void		   CheckRequestLimit(Action						   action,
									 const drogon::HttpRequestPtr& request,
									 const Json::Value&			   body,
									 const std::string&			   token);
	void		   Submit(Job job);
	void		   Execute(AccountStore& store, const PendingRequest& request, const Callback& callback) const;

	void		Dispatch(Action						   action,
						 const drogon::HttpRequestPtr& request,
						 Callback					   callback,
						 ProtectedOperation			   operation		 = {},
						 std::string				   requiredRole		 = {},
						 std::size_t				   maximumBodyBytes	 = 8192,
						 bool						   oneClick			 = false,
						 DatabaseOperation			   databaseOperation = {});
	void		Respond(const Callback& callback, Reply reply) const;
	bool		AllowPasswordWork(const std::string& peer, const std::string& identity);
	std::string ReadPassword(bool fromStdin) const;

	std::string					 m_Connection;
	std::string					 m_Origin;
	std::string					 m_Authority;
	std::string					 m_CookieName;
	std::filesystem::path		 m_UiRoot;
	std::filesystem::path		 m_SchemaFile;
	std::filesystem::path		 m_CustomerSchemaFile;
	std::filesystem::path		 m_ActivitySchemaFile;
	std::filesystem::path		 m_DeliverySchemaFile;
	std::unique_ptr<EmailWorker> m_EmailWorker;
	ScheduledEmailHooks			 m_EmailHooks;
	bool						 m_Secure = true;
	StoreSettings				 m_Settings;
	std::mutex					 m_Mutex;
	std::condition_variable		 m_Condition;
	std::deque<Job>				 m_Jobs;
	std::vector<std::thread>	 m_Workers;
	bool						 m_Stopping = false;

	struct Limit
	{
		std::chrono::steady_clock::time_point expires;
		int									  count = 0;
	};

	std::mutex							   m_LimitMutex;
	std::unordered_map<std::string, Limit> m_Limits;
};
} // namespace Accounts
