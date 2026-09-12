#pragma once

#include <filesystem>
#include <json/json.h>

namespace Accounts
{
class LocalOutboxTransport
{
public:
	LocalOutboxTransport() = default;
	~LocalOutboxTransport();

	LocalOutboxTransport(const LocalOutboxTransport&)			 = delete;
	LocalOutboxTransport& operator=(const LocalOutboxTransport&) = delete;

	void Start(const std::filesystem::path& directory);
	void Stop();
	void Deliver(const std::filesystem::path& path, const Json::Value& message) const;

private:
	int m_Lock = -1;
};
} // namespace Accounts
