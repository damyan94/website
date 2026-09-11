#pragma once

#include "AccountStore.h"
#include "Configuration.h"
#include "Controller.h"
#include <condition_variable>
#include <deque>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

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
		return m_Configuration.connection;
	}

	const StoreSettings& Settings() const
	{
		return m_Configuration.store;
	}

private:
	using Callback = Controller::Callback;
	using Job	   = Controller::Job;

	void		   RegisterEmailDeliveryHandlers();
	void		   Submit(Job job);

	void		Dispatch(Action						   action,
						 const drogon::HttpRequestPtr& request,
						 Callback					   callback,
						 ProtectedOperation			   operation		 = {},
						 std::string				   requiredRole		 = {},
						 std::size_t				   maximumBodyBytes	 = 8192,
						 bool						   oneClick			 = false,
						 DatabaseOperation			   databaseOperation = {});

	Configuration				 m_Configuration;
	Controller					 m_Controller;
	std::unique_ptr<EmailWorker> m_EmailWorker;
	ScheduledEmailHooks			 m_EmailHooks;
	std::mutex					 m_Mutex;
	std::condition_variable		 m_Condition;
	std::deque<Job>				 m_Jobs;
	std::vector<std::thread>	 m_Workers;
	bool						 m_Stopping = false;

};
} // namespace Accounts
