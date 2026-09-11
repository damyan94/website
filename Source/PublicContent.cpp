#include "stdafx.h"

#include "PublicContent.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpResponse.h>
#include <json/json.h>

namespace PublicContent
{

std::string Load(const std::filesystem::path& path)
{
	constexpr std::size_t MaxContentBytes = 1024 * 1024;
	if (!std::filesystem::is_regular_file(path))
	{
		throw std::runtime_error("Public content must be a regular file: " + path.string());
	}

	std::ifstream input(path, std::ios::binary);
	if (!input.is_open())
	{
		throw std::runtime_error("Cannot open public content: " + path.string());
	}

	// A bounded read also handles a file growing while it is being loaded.
	std::string bytes(MaxContentBytes + 1, '\0');
	input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	if (input.bad())
	{
		throw std::runtime_error("Cannot read public content: " + path.string());
	}
	bytes.resize(static_cast<std::size_t>(input.gcount()));
	if (bytes.size() > MaxContentBytes)
	{
		throw std::runtime_error("Public content exceeds the 1 MiB limit: " + path.string());
	}

	Json::CharReaderBuilder builder;
	builder["collectComments"]	   = false;
	builder["allowComments"]	   = false;
	builder["allowTrailingCommas"] = false;
	builder["failIfExtra"]		   = true;
	builder["rejectDupKeys"]	   = true;
	builder["stackLimit"]		   = 64;
	const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	Json::Value								content;
	std::string								errors;
	if (!reader->parse(bytes.data(), bytes.data() + bytes.size(), &content, &errors) || !content.isObject())
	{
		throw std::runtime_error("Public content requires a valid JSON object: " + path.string() + ". " + errors);
	}

	Json::StreamWriterBuilder writer;
	writer["indentation"] = "";
	writer["emitUTF8"]	  = true;
	return Json::writeString(writer, content);
}

void RegisterHandlers(std::string json)
{
	RegisterHandlers(std::make_shared<Snapshot>(std::move(json)));
}

Snapshot::Snapshot(std::string json)
	: m_Json(std::make_shared<const std::string>(std::move(json)))
{
}

std::shared_ptr<const std::string> Snapshot::Get() const noexcept
{
	return std::atomic_load(&m_Json);
}

void Snapshot::Publish(std::shared_ptr<const std::string> json) noexcept
{
	std::atomic_store(&m_Json, std::move(json));
}

void RegisterHandlers(std::shared_ptr<Snapshot> snapshot)
{
	drogon::app().registerHandler(
		"/api/v1/site",
		[snapshot = std::move(snapshot)](const drogon::HttpRequestPtr&,
										 std::function<void(const drogon::HttpResponsePtr&)>&& callback)
		{
			auto response = drogon::HttpResponse::newHttpResponse();
			response->setStatusCode(drogon::k200OK);
			response->setContentTypeString("application/json; charset=utf-8");
			response->setBody(*snapshot->Get());
			response->addHeader("Cache-Control", "no-store");
			response->addHeader("X-Content-Type-Options", "nosniff");
			callback(response);
		},
		{drogon::Get, drogon::Head});
}

} // namespace PublicContent
