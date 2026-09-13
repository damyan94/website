#pragma once

#include "AccountStore.h"
#include "Configuration.h"
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace Accounts
{
class Controller
{
public:
	using Callback = std::function<void(const drogon::HttpResponsePtr&)>;
	using Job = std::function<void(AccountStore&)>;
	using SubmitJob = std::function<void(Job)>;

	explicit Controller(const Configuration& configuration);

	void		Dispatch(Action						   action,
						 const drogon::HttpRequestPtr& request,
						 Callback					   callback,
						 const SubmitJob&			   submit,
						 ProtectedOperation			   operation		 = {},
						 std::string				   requiredRole		 = {},
						 std::size_t				   maximumBodyBytes	 = 8192,
						 bool						   oneClick			 = false,
						 DatabaseOperation			   databaseOperation = {});
	void Respond(const Callback& callback, Reply reply) const;
	static Reply Error(int status, const char* message);
	static void RespondWithAsset(const std::string& data, const std::string& type, const Callback& callback);

private:
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
	void Execute(AccountStore& store, const PendingRequest& request, const Callback& callback) const;
	bool AllowPasswordWork(const std::string& peer, const std::string& identity);

	// Module owns the configuration and joins its workers before destroying the controller.
	const Configuration& m_Configuration;

	struct Limit
	{
		std::chrono::steady_clock::time_point expires;
		int									  count = 0;
	};

	std::mutex							   m_LimitMutex;
	std::unordered_map<std::string, Limit> m_Limits;
};
} // namespace Accounts
