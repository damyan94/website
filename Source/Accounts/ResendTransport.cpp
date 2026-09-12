#include "ResendTransport.h"
#include "stdafx.h"

namespace Accounts
{
void ResendTransport::Start(const std::string& apiOrigin)
{
	m_HttpLoop = std::make_unique<trantor::EventLoopThread>();
	m_HttpLoop->run();
	m_HttpClient = drogon::HttpClient::newHttpClient(apiOrigin, m_HttpLoop->getLoop(), false, true);
}

void ResendTransport::Stop()
{
	m_HttpClient.reset();
	m_HttpLoop.reset();
}

std::pair<drogon::ReqResult, drogon::HttpResponsePtr> ResendTransport::Send(const std::string& apiKey,
																		const std::string& deliveryKey,
																		const std::string& requestBody,
																		int				   requestSeconds) const
{
	auto request = drogon::HttpRequest::newHttpRequest();
	request->setMethod(drogon::Post);
	request->setPath("/emails");
	request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
	request->addHeader("Authorization", "Bearer " + apiKey);
	request->addHeader("Idempotency-Key", deliveryKey);
	request->setBody(requestBody);
	const auto [result, response] = m_HttpClient->sendRequest(request, requestSeconds);
	return {result, response};
}

std::optional<Json::Value> ResendTransport::DecodeResponse(const drogon::HttpResponsePtr& response)
{
	Json::Value				parsedBody;
	Json::CharReaderBuilder builder;
	builder["collectComments"] = false;
	builder["rejectDupKeys"]   = true;
	builder["failIfExtra"]	   = true;
	builder["stackLimit"]	   = 8;
	std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	std::string						  parseErrors;
	const auto						  bytes = response->body();
	if (bytes.size() <= 32768 &&
		reader->parse(bytes.data(), bytes.data() + bytes.size(), &parsedBody, &parseErrors) &&
		parsedBody.isObject())
		return parsedBody;
	return std::nullopt;
}
} // namespace Accounts
