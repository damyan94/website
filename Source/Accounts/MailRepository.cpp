#include "MailRepository.h"
#include "stdafx.h"

namespace Accounts
{
MailRepository::MailRepository(Database& database)
	: m_Database(database)
{
}

unsigned long MailRepository::PendingJobs()
{
	return std::stoul(m_Database.Query(
		"SELECT count(*) FROM accounts.mail_jobs WHERE state IN ('queued','processing')").Get(0, 0));
}

std::string MailRepository::InsertJob(const std::string& kind,
									  const std::string& user,
									  const std::string& challenge,
									  const std::string& recipient,
									  const std::string& subject,
									  const std::string& body)
{
	return m_Database.Query(
		"INSERT INTO accounts.mail_jobs(kind,user_id,challenge_id,recipient,subject,body,expires_at) "
		"VALUES($1,NULLIF($2,'')::bigint,NULLIF($3,'')::bigint,$4,$5,$6,"
		"COALESCE((SELECT expires_at FROM accounts.email_challenges WHERE "
		"id=NULLIF($3,'')::bigint),now()+interval '1 day')) RETURNING id",
		{kind, user, challenge, recipient, subject, body}).Get(0, 0);
}

bool MailRepository::TryLockWorker()
{
	return m_Database.Query(
		"SELECT pg_try_advisory_lock(70123004)").Get(0, 0) == "t";
}

void MailRepository::UpdateHeartbeat(const std::string& error)
{
	m_Database.Query(
		"UPDATE accounts.mail_worker_status SET heartbeat_at=now(),last_error=$1",
		{error});
}

void MailRepository::RecordWorkerError()
{
	m_Database.Query(
		"UPDATE accounts.mail_worker_status SET last_error='delivery_worker_error'");
}

void MailRepository::LockProvider(const std::string& providerId)
{
	m_Database.Query(
		"SELECT pg_advisory_xact_lock(hashtextextended($1,70123005))",
		{providerId});
}

std::optional<MailEventJob> MailRepository::LockProviderJob(const std::string& providerId)
{
	const auto rows = m_Database.Query(
		"SELECT id,recipient FROM accounts.mail_jobs WHERE provider_id=$1 FOR UPDATE",
		{providerId});
	if (!rows.Count())
		return std::nullopt;
	return MailEventJob{rows.Get(0, 0), rows.Get(0, 1)};
}

std::optional<std::string> MailRepository::HighestPriorityEvent(const std::string& providerId)
{
	const auto rows = m_Database.Query(
		"SELECT event_type FROM accounts.mail_events WHERE provider_id=$1 ORDER BY CASE event_type "
		"WHEN 'email.complained' THEN 6 WHEN 'email.bounced' THEN 5 WHEN 'email.suppressed' THEN 4 "
		"WHEN 'email.failed' THEN 3 WHEN 'email.delivered' THEN 2 WHEN 'email.delivery_delayed' THEN 1 "
		"ELSE 0 END DESC,occurred_at DESC LIMIT 1",
		{providerId});
	if (!rows.Count())
		return std::nullopt;
	return rows.Get(0, 0);
}

void MailRepository::UpdateEventStatus(const std::string& id,
									   const std::string& state,
									   const std::string& error)
{
	m_Database.Query(
		"UPDATE accounts.mail_jobs SET state=$2,last_error=$3,body='',request_body=NULL,"
		"unsubscribe_token=NULL,finished_at=now() WHERE id=$1::bigint",
		{id, state, error});
}

void MailRepository::SuppressRecipient(const std::string& recipient,
									   const std::string& reason)
{
	m_Database.Query(
		"INSERT INTO accounts.mail_suppressions(recipient,reason) VALUES($1,$2) "
		"ON CONFLICT(recipient) DO UPDATE SET reason=excluded.reason",
		{recipient, reason});
}

std::optional<MailJob> MailRepository::LockDueJob(const std::string& transport)
{
	const auto rows = m_Database.Query(
		"SELECT id,kind,COALESCE(user_id::text,''),COALESCE(challenge_id::text,''),"
		"COALESCE(subscription_generation::text,''),recipient,subject,body,COALESCE(unsubscribe_token,''),attempts,"
		"expires_at IS NOT NULL AND expires_at<=now(),COALESCE(transport,''),COALESCE(delivery_key,''),"
		"COALESCE(request_body,''),first_attempt_at<now()-interval '23 hours' FROM accounts.mail_jobs "
		"WHERE state IN ('queued','processing') AND available_at<=now() AND "
		"($1='local_outbox' OR (SELECT pause_until<=now() FROM accounts.mail_worker_status)) "
		"ORDER BY (kind='newsletter'),(kind='reminder'),id LIMIT 1 FOR UPDATE SKIP LOCKED",
		{transport});
	if (!rows.Count())
		return std::nullopt;
	return MailJob{rows.Get(0, 0),
				   rows.Get(0, 1),
				   rows.Get(0, 2),
				   rows.Get(0, 3),
				   rows.Get(0, 4),
				   rows.Get(0, 5),
				   rows.Get(0, 6),
				   rows.Get(0, 7),
				   rows.Get(0, 8),
				   std::stoi(rows.Get(0, 9)),
				   rows.Get(0, 10) == "t",
				   rows.Get(0, 11),
				   rows.Get(0, 12),
				   rows.Get(0, 13),
				   rows.Get(0, 14) == "t"};
}

bool MailRepository::IsSuppressed(const std::string& recipient)
{
	return m_Database.Query(
		"SELECT 1 FROM accounts.mail_suppressions WHERE recipient=$1",
		{recipient}).Count() != 0;
}

bool MailRepository::HasCurrentChallenge(const std::string& challenge,
										 bool registration,
										 bool newsletters)
{
	return m_Database.Query(
		"SELECT c.id FROM accounts.email_challenges c "
		"LEFT JOIN accounts.users u ON u.id=c.user_id WHERE c.id=NULLIF($1,'')::bigint AND "
		"c.consumed_at IS NULL "
		"AND c.expires_at>now() AND (c.purpose<>'register' OR $2::boolean) "
		"AND (c.purpose<>'subscribe' OR $3::boolean) "
		"AND (c.user_id IS NULL OR (u.enabled AND u.credential_version=c.credential_version))",
		{challenge, registration ? "true" : "false", newsletters ? "true" : "false"}).Count() != 0;
}

bool MailRepository::HasCurrentSubscription(const std::string& user,
											const std::string& generation,
											const std::string& recipient,
											const std::string& id)
{
	return m_Database.Query(
		"SELECT s.user_id FROM accounts.subscriptions s JOIN accounts.users u ON u.id=s.user_id "
		"WHERE s.user_id=$1::bigint AND s.generation=$2::bigint AND s.email=$3 AND u.email=$3 "
		"AND s.confirmed_at IS NOT NULL AND u.email_verified_at IS NOT NULL AND u.enabled "
		"AND u.locale=(SELECT c.locale FROM accounts.campaigns c JOIN accounts.mail_jobs j ON "
		"j.campaign_id=c.id WHERE j.id=$4::bigint)",
		{user, generation, recipient, id}).Count() != 0;
}

void MailRepository::InsertNewsletterLink(const std::string& tokenHash,
										  const std::string& user,
										  const std::string& generation)
{
	m_Database.Query(
		"INSERT INTO accounts.newsletter_links(token_hash,user_id,generation) "
		"VALUES($1,$2::bigint,$3::bigint)",
		{tokenHash, user, generation});
}

void MailRepository::FinishUnsentJob(const std::string& id,
									 const std::string& state,
									 const std::string& error)
{
	m_Database.Query(
		"UPDATE accounts.mail_jobs SET "
		"state=$2,body='',request_body=NULL,unsubscribe_token=NULL,finished_at=now(),last_error=$3 "
		"WHERE id=$1::bigint",
		{id, state, error});
}

void MailRepository::RecordAttempt(const std::string& id,
								   const std::string& body,
								   const std::string& unsubscribe,
								   const std::string& transport,
								   const std::string& deliveryKey,
								   const std::string& requestBody)
{
	m_Database.Query(
		"UPDATE accounts.mail_jobs SET "
		"state='processing',attempts=attempts+1,available_at=now()+interval '5 minutes',"
		"body=$2,unsubscribe_token=NULLIF($3,''),transport=$4,delivery_key=$5,request_body=NULLIF($6,''),"
		"first_attempt_at=COALESCE(first_attempt_at,now()) WHERE id=$1::bigint",
		{id, body, unsubscribe, transport, deliveryKey, requestBody});
}

void MailRepository::CompleteDelivery(const std::string& id,
									  const std::string& state,
									  const std::string& providerId,
									  int status)
{
	m_Database.Query(
		"UPDATE accounts.mail_jobs SET "
		"state=$2,provider_id=NULLIF($3,''),last_error='',status_code=NULLIF($4,'0')::integer,"
		"body='',request_body=NULL,unsubscribe_token=NULL,finished_at=now() WHERE id=$1::bigint AND "
		"state='processing'",
		{id, state, providerId, std::to_string(status)});
}

void MailRepository::RecordDeliveryFailure(const std::string& id,
										   bool failed,
										   const std::string& error,
										   int status,
										   int delay)
{
	m_Database.Query(
		"UPDATE accounts.mail_jobs SET state=CASE WHEN $2::boolean THEN 'failed' ELSE 'queued' END,"
		"body=CASE WHEN $2::boolean THEN '' ELSE body END,unsubscribe_token=CASE WHEN $2::boolean THEN "
		"NULL ELSE unsubscribe_token END,"
		"request_body=CASE WHEN $2::boolean THEN NULL ELSE request_body END,"
		"finished_at=CASE WHEN $2::boolean THEN now() ELSE NULL "
		"END,last_error=$3,status_code=NULLIF($4,'0')::integer,"
		"available_at=now()+$5::integer*interval '1 second' WHERE id=$1::bigint AND state='processing'",
		{id, failed ? "true" : "false", error, std::to_string(status), std::to_string(delay)});
}

void MailRepository::ExtendProviderPause(int seconds)
{
	m_Database.Query(
		"UPDATE accounts.mail_worker_status SET "
		"pause_until=GREATEST(pause_until,now()+$1::integer*interval '1 second')",
		{std::to_string(seconds)});
}

void MailRepository::InsertProviderEvent(const std::string& eventId,
										 const std::string& providerId,
										 const std::string& type,
										 const std::string& occurred)
{
	m_Database.Query(
		"INSERT INTO accounts.mail_events(event_id,provider_id,event_type,occurred_at) "
		"VALUES($1,$2,$3,$4::timestamptz) "
		"ON CONFLICT(event_id) DO NOTHING",
		{eventId, providerId, type, occurred});
}

MailProviderEvent MailRepository::FindProviderEvent(const std::string& eventId)
{
	const auto rows = m_Database.Query(
		"SELECT provider_id,event_type FROM accounts.mail_events WHERE event_id=$1",
		{eventId});
	return {rows.Get(0, 0), rows.Get(0, 1)};
}

std::vector<MailStateCount> MailRepository::StateCounts()
{
	const auto rows = m_Database.Query(
		"SELECT CASE WHEN state='delivered' AND transport='local_outbox' THEN 'outbox' ELSE state END,count(*) "
		"FROM accounts.mail_jobs GROUP BY 1");
	std::vector<MailStateCount> totals;
	totals.reserve(rows.Count());
	for (int i = 0; i < rows.Count(); ++i)
		totals.push_back({rows.Get(i, 0), rows.Get(i, 1)});
	return totals;
}

MailWorkerStatus MailRepository::WorkerStatus()
{
	const auto rows = m_Database.Query(
		"SELECT COALESCE(to_char(heartbeat_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'),''),"
		"COALESCE(heartbeat_at>now()-interval '60 seconds',false),last_error,pause_until>now(),"
		"to_char(pause_until AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') FROM accounts.mail_worker_status");
	return {rows.Get(0, 0), rows.Get(0, 1) == "t", rows.Get(0, 2), rows.Get(0, 3) == "t", rows.Get(0, 4)};
}

std::vector<MailJobSummary> MailRepository::ListJobsBefore(const std::string& before,
														   const std::string& state)
{
	const auto rows = m_Database.Query(
		"SELECT "
		"id,kind,recipient,subject,state,attempts,COALESCE(transport,''),last_error,COALESCE(status_code::text,''),"
		"COALESCE(provider_id,''),to_char(created_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'),"
		"to_char(available_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') FROM accounts.mail_jobs "
		"WHERE ($1='' OR id<NULLIF($1,'')::bigint) AND ($2='' OR state=$2) ORDER BY id DESC LIMIT 51",
		{before, state});
	std::vector<MailJobSummary> jobs;
	jobs.reserve(rows.Count());
	for (int i = 0; i < rows.Count(); ++i)
		jobs.push_back({rows.Get(i, 0),
					   rows.Get(i, 1),
					   rows.Get(i, 2),
					   rows.Get(i, 3),
					   rows.Get(i, 4),
					   rows.Get(i, 5),
					   rows.Get(i, 6),
					   rows.Get(i, 7),
					   rows.Get(i, 8),
					   rows.Get(i, 9),
					   rows.Get(i, 10),
					   rows.Get(i, 11)});
	return jobs;
}
} // namespace Accounts
