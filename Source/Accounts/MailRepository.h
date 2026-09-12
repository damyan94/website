#pragma once

#include "Database.h"
#include <optional>

namespace Accounts
{
struct MailEventJob
{
	std::string id;
	std::string recipient;
};

struct MailJob
{
	std::string id;
	std::string kind;
	std::string user;
	std::string challenge;
	std::string generation;
	std::string recipient;
	std::string subject;
	std::string body;
	std::string unsubscribe;
	int attempts;
	bool expired;
	std::string transport;
	std::string deliveryKey;
	std::string requestBody;
	bool retryWindowExpired;
};

struct MailProviderEvent
{
	std::string providerId;
	std::string type;
};

struct MailStateCount
{
	std::string state;
	std::string count;
};

struct MailWorkerStatus
{
	std::string heartbeatAt;
	bool healthy;
	std::string error;
	bool paused;
	std::string pauseUntil;
};

// Keep the existing string-valued identifiers, counters and UTC timestamp text.
struct MailJobSummary
{
	std::string id;
	std::string kind;
	std::string recipient;
	std::string subject;
	std::string state;
	std::string attempts;
	std::string transport;
	std::string error;
	std::string statusCode;
	std::string providerId;
	std::string createdAt;
	std::string availableAt;
};

// Borrows the caller's connection. The caller owns transactions and delivery decisions.
class MailRepository
{
public:
	explicit MailRepository(Database& database);

	unsigned long PendingJobs();
	std::string InsertJob(const std::string& kind,
						  const std::string& user,
						  const std::string& challenge,
						  const std::string& recipient,
						  const std::string& subject,
						  const std::string& body);

	bool TryLockWorker();
	void UpdateHeartbeat(const std::string& error);
	void RecordWorkerError();

	void LockProvider(const std::string& providerId);
	std::optional<MailEventJob> LockProviderJob(const std::string& providerId);
	std::optional<std::string> HighestPriorityEvent(const std::string& providerId);
	void UpdateEventStatus(const std::string& id, const std::string& state, const std::string& error);
	void SuppressRecipient(const std::string& recipient, const std::string& reason);

	std::optional<MailJob> LockDueJob(const std::string& transport);
	bool IsSuppressed(const std::string& recipient);
	bool HasCurrentChallenge(const std::string& challenge, bool registration, bool newsletters);
	bool HasCurrentSubscription(const std::string& user,
								const std::string& generation,
								const std::string& recipient,
								const std::string& id);
	void InsertNewsletterLink(const std::string& tokenHash,
							  const std::string& user,
							  const std::string& generation);
	void FinishUnsentJob(const std::string& id, const std::string& state, const std::string& error);
	void RecordAttempt(const std::string& id,
					   const std::string& body,
					   const std::string& unsubscribe,
					   const std::string& transport,
					   const std::string& deliveryKey,
					   const std::string& requestBody);
	void CompleteDelivery(const std::string& id,
						  const std::string& state,
						  const std::string& providerId,
						  int status);
	void RecordDeliveryFailure(const std::string& id,
							   bool failed,
							   const std::string& error,
							   int status,
							   int delay);
	void ExtendProviderPause(int seconds);

	void InsertProviderEvent(const std::string& eventId,
							 const std::string& providerId,
							 const std::string& type,
							 const std::string& occurred);
	MailProviderEvent FindProviderEvent(const std::string& eventId);

	std::vector<MailStateCount> StateCounts();
	MailWorkerStatus WorkerStatus();
	std::vector<MailJobSummary> ListJobsBefore(const std::string& before, const std::string& state);

private:
	Database& m_Database;
};
} // namespace Accounts
