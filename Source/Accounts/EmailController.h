#pragma once

#include "Controller.h"

namespace Accounts
{
class EmailController
{
public:
	using Callback = Controller::Callback;
	using SubmitJob = Controller::SubmitJob;

	EmailController(const Configuration& configuration, Controller& accounts);
	void DispatchWebhook(const drogon::HttpRequestPtr& request, Callback callback, const SubmitJob& submit);

	// Invoked by the existing authenticated database handler, after the administrator check.
	static Json::Value DeliveryStatus(Database& database, const Json::Value& query, const std::string& transport);

private:
	Reply ReceiveEvent(AccountStore& store, const std::string& eventId, const Json::Value& body) const;

	// Module retains both controllers until its queued jobs have stopped.
	const Configuration& m_Configuration;
	Controller& m_Accounts;
};
} // namespace Accounts
