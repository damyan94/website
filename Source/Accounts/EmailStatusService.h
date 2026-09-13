#pragma once

#include "MailRepository.h"

namespace Accounts
{
struct EmailStatusSnapshot
{
	std::vector<MailStateCount> counts;
	MailWorkerStatus worker;
	std::vector<MailJobSummary> jobs;
};

// Borrows an Accounts worker's connection; status reads use the caller's transaction.
class EmailStatusService
{
public:
	explicit EmailStatusService(Database& database);

	// A conflicting event ID returns false without committing the callback transaction.
	bool ReceiveEvent(const std::string& eventId,
					  const std::string& type,
					  const std::string& providerId,
					  const std::string& occurred);
	EmailStatusSnapshot ReadStatus(const std::string& before, const std::string& state);

private:
	Database& m_Database;
};
} // namespace Accounts
