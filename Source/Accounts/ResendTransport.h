#pragma once

#include <drogon/HttpClient.h>
#include <json/json.h>
#include <optional>
#include <string>
#include <trantor/net/EventLoopThread.h>
#include <utility>

namespace Accounts
{
class ResendTransport
{
public:
	void Start(const std::string& apiOrigin);
	void Stop();

	std::pair<drogon::ReqResult, drogon::HttpResponsePtr> Send(const std::string& apiKey,
															const std::string& deliveryKey,
															const std::string& requestBody,
															int				   requestSeconds) const;
	static std::optional<Json::Value> DecodeResponse(const drogon::HttpResponsePtr& response);

private:
	std::unique_ptr<trantor::EventLoopThread> m_HttpLoop;
	drogon::HttpClientPtr					m_HttpClient;
};
} // namespace Accounts
