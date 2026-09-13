#include "EmailStatusService.h"
#include "Email.h"
#include "stdafx.h"

namespace Accounts
{
EmailStatusService::EmailStatusService(Database& database)
	: m_Database(database)
{
}

bool EmailStatusService::ReceiveEvent(const std::string& eventId,
									 const std::string& type,
									 const std::string& providerId,
									 const std::string& occurred)
{
	m_Database.ReconnectIfNeeded();
	Transaction transaction(m_Database);
	MailRepository repository(m_Database);
	repository.LockProvider(providerId);
	repository.InsertProviderEvent(eventId, providerId, type, occurred);
	const auto stored = repository.FindProviderEvent(eventId);
	if (stored.providerId != providerId || stored.type != type)
		return false;
	ApplyEmailEvents(m_Database, providerId);
	transaction.Commit();
	return true;
}

EmailStatusSnapshot EmailStatusService::ReadStatus(const std::string& before, const std::string& state)
{
	MailRepository repository(m_Database);
	return {repository.StateCounts(), repository.WorkerStatus(), repository.ListJobsBefore(before, state)};
}
} // namespace Accounts
