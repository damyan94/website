#include "Email.h"
#include "Crypto.h"
#include "stdafx.h"
#include <charconv>
#include <json/json.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <sstream>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace Accounts
{
std::string QueueEmail(Database&		  database,
					   const std::string& recipient,
					   const std::string& subject,
					   const std::string& body,
					   const std::string& user,
					   const std::string& challenge)
{
	if (std::stoul(database.Query("SELECT count(*) FROM accounts.mail_jobs WHERE state IN ('queued','processing')")
					   .Get(0, 0)) >= 10000)
		throw std::runtime_error("Email queue is full");
	return database
		.Query("INSERT INTO accounts.mail_jobs(kind,user_id,challenge_id,recipient,subject,body,expires_at) "
			   "VALUES($1,NULLIF($2,'')::bigint,NULLIF($3,'')::bigint,$4,$5,$6,"
			   "COALESCE((SELECT expires_at FROM accounts.email_challenges WHERE "
			   "id=NULLIF($3,'')::bigint),now()+interval '1 day')) RETURNING id",
			   {challenge.empty() ? "notice" : "challenge", user, challenge, recipient, subject, body})
		.Get(0, 0);
}

std::string DecodeWebhookSecret(const std::string& secret)
{
	if (!secret.starts_with("whsec_"))
		throw std::runtime_error("Invalid email webhook secret");
	auto encoded = secret.substr(6);
	if (encoded.size() < 22 || encoded.size() > 128 || encoded.size() % 4 == 1 ||
		encoded.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") !=
			std::string::npos)
		throw std::runtime_error("Invalid email webhook secret");
	// Accept standard Base64 with or without trailing padding, as secret tooling may omit it.
	const auto padding = encoded.find('=');
	if (padding != std::string::npos && (encoded.size() % 4 != 0 || encoded.size() - padding > 2 ||
										 encoded.find_first_not_of('=', padding) != std::string::npos))
		throw std::runtime_error("Invalid email webhook secret");
	while (encoded.size() % 4 != 0)
		encoded += '=';
	std::string decoded(encoded.size(), '\0');
	int			size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(decoded.data()),
							   reinterpret_cast<const unsigned char*>(encoded.data()),
							   encoded.size());
	if (size < 0)
		throw std::runtime_error("Invalid email webhook secret");
	if (encoded.ends_with('='))
		--size;
	if (encoded.ends_with("=="))
		--size;
	if (size < 16)
		throw std::runtime_error("Invalid email webhook secret");
	decoded.resize(size);
	return decoded;
}

bool VerifyEmailWebhook(const std::string& key,
						const std::string& id,
						const std::string& timestamp,
						const std::string& signature,
						std::string_view   body)
{
	if (key.empty() || id.empty() || id.size() > 128 || timestamp.empty() || timestamp.size() > 12 ||
		signature.size() > 1024 || body.size() > 32768 ||
		id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
		return false;
	long long  seconds = 0;
	const auto parsed  = std::from_chars(timestamp.data(), timestamp.data() + timestamp.size(), seconds);
	const auto now =
		std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	if (parsed.ec != std::errc() || parsed.ptr != timestamp.data() + timestamp.size() || seconds < now - 300 ||
		seconds > now + 300)
		return false;
	const auto	  signedBytes = id + "." + timestamp + "." + std::string(body);
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int  size = 0;
	if (!HMAC(EVP_sha256(),
			  key.data(),
			  key.size(),
			  reinterpret_cast<const unsigned char*>(signedBytes.data()),
			  signedBytes.size(),
			  digest,
			  &size))
		return false;
	std::string expected(4 * ((size + 2) / 3) + 1, '\0');
	expected.resize(EVP_EncodeBlock(reinterpret_cast<unsigned char*>(expected.data()), digest, size));
	std::istringstream signatures(signature);
	std::string		   value;
	bool			   valid = false;
	while (signatures >> value)
		if (value.starts_with("v1,"))
			valid |= ConstantEqual(value.substr(3), expected);
	return valid;
}

void ApplyEmailEvents(Database& database, const std::string& providerId)
{
	// Serialize with acceptance and early callbacks, even before the job has a provider ID.
	database.Query("SELECT pg_advisory_xact_lock(hashtextextended($1,70123005))", {providerId});
	const auto job =
		database.Query("SELECT id,recipient FROM accounts.mail_jobs WHERE provider_id=$1 FOR UPDATE", {providerId});
	if (!job.Count())
		return;
	// Terminal failures/complaints dominate delivery, which dominates transient events.
	// Repeated and out-of-order callbacks cannot downgrade the result.
	const auto event =
		database.Query("SELECT event_type FROM accounts.mail_events WHERE provider_id=$1 ORDER BY CASE event_type "
					   "WHEN 'email.complained' THEN 6 WHEN 'email.bounced' THEN 5 WHEN 'email.suppressed' THEN 4 "
					   "WHEN 'email.failed' THEN 3 WHEN 'email.delivered' THEN 2 WHEN 'email.delivery_delayed' THEN 1 "
					   "ELSE 0 END DESC,occurred_at DESC LIMIT 1",
					   {providerId});
	if (!event.Count())
		return;
	const auto type		= event.Get(0, 0);
	const auto state	= type == "email.complained"							 ? "complained"
						  : type == "email.bounced"								 ? "bounced"
						  : type == "email.suppressed" || type == "email.failed" ? "failed"
						  : type == "email.delivered"							 ? "delivered"
						  : type == "email.delivery_delayed"					 ? "delayed"
																				 : "accepted";
	const bool suppress = type == "email.complained" || type == "email.bounced" || type == "email.suppressed";
	database.Query(
		"UPDATE accounts.mail_jobs SET state=$2,last_error=$3,body='',request_body=NULL,"
		"unsubscribe_token=NULL,finished_at=now() WHERE id=$1::bigint",
		{job.Get(0, 0), state, suppress || type == "email.failed" || type == "email.delivery_delayed" ? type : ""});
	if (suppress)
		database.Query("INSERT INTO accounts.mail_suppressions(recipient,reason) VALUES($1,$2) "
					   "ON CONFLICT(recipient) DO UPDATE SET reason=excluded.reason",
					   {job.Get(0, 1), type});
}

namespace
{
#ifndef _WIN32
class File
{
public:
	explicit File(int descriptor)
		: fd(descriptor)
	{
		if (fd < 0)
			throw std::runtime_error("Email outbox is unavailable");
	}

	~File()
	{
		if (fd >= 0)
			::close(fd);
	}

	File(const File&)			 = delete;
	File& operator=(const File&) = delete;
	int	  fd;
};

void Sync(int fd)
{
	while (::fsync(fd) != 0)
		if (errno != EINTR)
			throw std::runtime_error("Email outbox sync failed");
}

void WriteMessage(const std::filesystem::path& path, const Json::Value& message)
{
	Json::StreamWriterBuilder writer;
	writer["indentation"] = "  ";
	writer["emitUTF8"]	  = true;
	const auto bytes	  = Json::writeString(writer, message) + '\n';
	const auto temp		  = path.parent_path() / (".mail-" + RandomToken() + ".tmp");
	File	   file(::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));

	struct Cleanup
	{
		std::filesystem::path path;

		~Cleanup()
		{
			::unlink(path.c_str());
		}
	} cleanup{temp};

	std::size_t offset = 0;
	while (offset < bytes.size())
	{
		const auto written = ::write(file.fd, bytes.data() + offset, bytes.size() - offset);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			throw std::runtime_error("Email outbox write failed");
		offset += written;
	}
	Sync(file.fd);
	if (::rename(temp.c_str(), path.c_str()) != 0)
		throw std::runtime_error("Email outbox rename failed");
	File directory(::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
	Sync(directory.fd);
}
#endif
} // namespace

EmailWorker::EmailWorker(std::string connection, EmailSettings settings, ScheduledEmailHooks hooks)
	: m_Connection(std::move(connection)),
	  m_Settings(std::move(settings)),
	  m_Hooks(std::move(hooks))
{
}

EmailWorker::~EmailWorker()
{
	Stop();
}

void EmailWorker::Start()
{
#ifdef _WIN32
	throw std::runtime_error("The local email outbox currently requires a POSIX host");
#else
	auto database = std::make_unique<Database>(m_Connection);
	database->CheckSchema();
	if (database->Query("SELECT pg_try_advisory_lock(70123004)").Get(0, 0) != "t")
		database.reset(); // Another backend delivers for this database; take over when its lock is released.
	if (m_Settings.transport == "local_outbox")
	{
		std::filesystem::create_directories(m_Settings.outbox);
		std::filesystem::permissions(m_Settings.outbox, std::filesystem::perms::owner_all);
		File lock(
			::open((m_Settings.outbox / ".worker.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
		if (::flock(lock.fd, LOCK_EX | LOCK_NB) != 0)
			throw std::runtime_error("Another email worker owns this outbox");
		m_Lock = std::exchange(lock.fd, -1);
	}
	else
	{
		m_HttpLoop = std::make_unique<trantor::EventLoopThread>();
		m_HttpLoop->run();
		m_HttpClient = drogon::HttpClient::newHttpClient(m_Settings.apiOrigin, m_HttpLoop->getLoop(), false, true);
	}
	m_Worker = std::thread([this, database = std::move(database)]() mutable { Run(std::move(database)); });
#endif
}

void EmailWorker::Stop()
{
	{
		std::lock_guard lock(m_Mutex);
		m_Stopping = true;
	}
	m_Condition.notify_all();
	if (m_Worker.joinable())
		m_Worker.join();
	m_HttpClient.reset();
	m_HttpLoop.reset();
#ifndef _WIN32
	if (m_Lock >= 0)
	{
		::close(m_Lock);
		m_Lock = -1;
	}
#endif
}

void EmailWorker::Run(std::unique_ptr<Database> database)
{
	auto		nextHeartbeat = std::chrono::steady_clock::time_point::min();
	auto		nextSchedule  = nextHeartbeat;
	std::string schedulerError;
	bool		ownsLock = bool(database);
	for (;;)
	{
		{
			std::unique_lock lock(m_Mutex);
			if (m_Condition.wait_for(lock, std::chrono::milliseconds(200), [this] { return m_Stopping; }))
				return;
		}
		try
		{
			if (!database)
			{
				database = std::make_unique<Database>(m_Connection);
				database->CheckSchema();
				if (database->Query("SELECT pg_try_advisory_lock(70123004)").Get(0, 0) != "t")
					throw std::runtime_error("Another email worker owns this database");
				ownsLock = true;
			}
			const auto now = std::chrono::steady_clock::now();
			if (now >= nextSchedule && m_Hooks.enqueue)
			{
				nextSchedule = now + std::chrono::seconds(30);
				try
				{
					m_Hooks.enqueue(*database);
					schedulerError.clear();
				}
				catch (const std::exception&)
				{
					schedulerError = "reminder_scheduler_failed";
				}
			}
			DeliverOne(*database);
			if (now >= nextHeartbeat)
			{
				database->Query("UPDATE accounts.mail_worker_status SET heartbeat_at=now(),last_error=$1",
								{schedulerError});
				nextHeartbeat = now + std::chrono::seconds(10);
			}
		}
		catch (const std::exception&)
		{
			// Delivery retries are durable. Never log bodies, addresses or tokens.
			if (database && ownsLock)
			{
				try
				{
					database->Query("UPDATE accounts.mail_worker_status SET last_error='delivery_worker_error'");
				}
				catch (...)
				{ /* An unavailable database is visible through the stale heartbeat. */
				}
			}
			database.reset();
			ownsLock	  = false;
			nextHeartbeat = std::chrono::steady_clock::time_point::min();
			std::unique_lock lock(m_Mutex);
			if (m_Condition.wait_for(lock, std::chrono::seconds(2), [this] { return m_Stopping; }))
				return;
		}
	}
}

bool EmailWorker::DeliverOne(Database& database)
{
#ifdef _WIN32
	(void)database;
	return false;
#else
	std::string id, subject, body, recipient, kind, unsubscribe, deliveryKey, requestBody;
	int			attempts = 0;
	Json::Value message;
	{
		Transaction transaction(database);
		const auto	row = database.Query(
			 "SELECT id,kind,COALESCE(user_id::text,''),COALESCE(challenge_id::text,''),"
			 "COALESCE(subscription_generation::text,''),recipient,subject,body,COALESCE(unsubscribe_token,''),attempts,"
			 "expires_at IS NOT NULL AND expires_at<=now(),COALESCE(transport,''),COALESCE(delivery_key,''),"
			 "COALESCE(request_body,''),first_attempt_at<now()-interval '23 hours' FROM accounts.mail_jobs "
			 "WHERE state IN ('queued','processing') AND available_at<=now() AND "
			 "($1='local_outbox' OR (SELECT pause_until<=now() FROM accounts.mail_worker_status)) "
			 "ORDER BY (kind='newsletter'),(kind='reminder'),id LIMIT 1 FOR UPDATE SKIP LOCKED",
			 {m_Settings.transport});
		if (!row.Count())
		{
			transaction.Commit();
			return false;
		}
		id					 = row.Get(0, 0);
		kind				 = row.Get(0, 1);
		recipient			 = row.Get(0, 5);
		subject				 = row.Get(0, 6);
		body				 = row.Get(0, 7);
		unsubscribe			 = row.Get(0, 8);
		attempts			 = std::stoi(row.Get(0, 9));
		deliveryKey			 = row.Get(0, 12);
		requestBody			 = row.Get(0, 13);
		bool		eligible = row.Get(0, 10) != "t";
		std::string reason	 = eligible ? "ineligible" : "expired";
		if (!row.Get(0, 11).empty() && row.Get(0, 11) != m_Settings.transport)
		{
			eligible = false;
			reason	 = "transport_changed";
		}
		if (database.Query("SELECT 1 FROM accounts.mail_suppressions WHERE recipient=$1", {recipient}).Count())
		{
			eligible = false;
			reason	 = "recipient_suppressed";
		}
		if (kind == "reminder")
			eligible = eligible && m_Hooks.eligible && m_Hooks.eligible(database, id);
		if (kind == "challenge")
			eligible =
				eligible &&
				database
					.Query("SELECT c.id FROM accounts.email_challenges c "
						   "LEFT JOIN accounts.users u ON u.id=c.user_id WHERE c.id=NULLIF($1,'')::bigint AND "
						   "c.consumed_at IS NULL "
						   "AND c.expires_at>now() AND (c.purpose<>'register' OR $2::boolean) "
						   "AND (c.purpose<>'subscribe' OR $3::boolean) "
						   "AND (c.user_id IS NULL OR (u.enabled AND u.credential_version=c.credential_version))",
						   {row.Get(0, 3),
							m_Settings.registration ? "true" : "false",
							m_Settings.newsletters ? "true" : "false"})
					.Count();
		if (kind == "newsletter")
		{
			eligible =
				eligible && m_Settings.newsletters &&
				database
					.Query("SELECT s.user_id FROM accounts.subscriptions s JOIN accounts.users u ON u.id=s.user_id "
						   "WHERE s.user_id=$1::bigint AND s.generation=$2::bigint AND s.email=$3 AND u.email=$3 "
						   "AND s.confirmed_at IS NOT NULL AND u.email_verified_at IS NOT NULL AND u.enabled "
						   "AND u.locale=(SELECT c.locale FROM accounts.campaigns c JOIN accounts.mail_jobs j ON "
						   "j.campaign_id=c.id WHERE j.id=$4::bigint)",
						   {row.Get(0, 2), row.Get(0, 4), recipient, id})
					.Count();
			if (eligible && unsubscribe.empty())
			{
				unsubscribe = RandomToken();
				database.Query("INSERT INTO accounts.newsletter_links(token_hash,user_id,generation) "
							   "VALUES($1,$2::bigint,$3::bigint)",
							   {TokenHash(unsubscribe), row.Get(0, 2), row.Get(0, 4)});
				body += "\n\nUnsubscribe / Отписване:\n" + m_Settings.origin +
						"/email#action=unsubscribe&token=" + unsubscribe;
			}
		}
		const bool retryExpired = m_Settings.transport == "resend" && row.Get(0, 14) == "t";
		if (!eligible || attempts >= 5 || retryExpired)
		{
			database.Query("UPDATE accounts.mail_jobs SET "
						   "state=$2,body='',request_body=NULL,unsubscribe_token=NULL,finished_at=now(),last_error=$3 "
						   "WHERE id=$1::bigint",
						   {id,
							eligible ? "failed" : "skipped",
							retryExpired ? "retry_window_expired"
							: eligible	 ? "attempts_exhausted"
										 : reason});
			transaction.Commit();
			return true;
		}
		message["id"]	   = id;
		message["kind"]	   = kind;
		message["from"]	   = m_Settings.sender;
		message["to"]	   = recipient;
		message["subject"] = subject;
		message["text"]	   = body;
		if (!unsubscribe.empty())
		{
			message["headers"]["List-Unsubscribe"] =
				"<" + m_Settings.origin + "/api/v1/newsletter/one-click?token=" + unsubscribe + ">";
			message["headers"]["List-Unsubscribe-Post"] = "List-Unsubscribe=One-Click";
		}
		if (deliveryKey.empty())
			deliveryKey = RandomToken();
		if (requestBody.empty() && m_Settings.transport == "resend")
		{
			auto payload = message;
			payload.removeMember("id");
			payload.removeMember("kind");
			payload["to"] = Json::arrayValue;
			payload["to"].append(recipient);
			Json::StreamWriterBuilder writer;
			writer["indentation"] = "";
			requestBody			  = Json::writeString(writer, payload);
		}
		database.Query(
			"UPDATE accounts.mail_jobs SET "
			"state='processing',attempts=attempts+1,available_at=now()+interval '5 minutes',"
			"body=$2,unsubscribe_token=NULLIF($3,''),transport=$4,delivery_key=$5,request_body=NULLIF($6,''),"
			"first_attempt_at=COALESCE(first_attempt_at,now()) WHERE id=$1::bigint",
			{id, body, unsubscribe, m_Settings.transport, deliveryKey, requestBody});
		transaction.Commit();
	}
	// No SQL transaction is held during network/filesystem I/O.
	bool		accepted = false, retry = true;
	int			status = 0, delay = 30 * (1 << attempts);
	std::string providerId, error;
	try
	{
		if (m_Settings.transport == "local_outbox")
		{
			WriteMessage(m_Settings.outbox / (id + ".json"), message);
			accepted = true;
		}
		else
		{
			auto request = drogon::HttpRequest::newHttpRequest();
			request->setMethod(drogon::Post);
			request->setPath("/emails");
			request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
			request->addHeader("Authorization", "Bearer " + m_Settings.apiKey);
			request->addHeader("Idempotency-Key", deliveryKey);
			request->setBody(requestBody);
			const auto [result, response] = m_HttpClient->sendRequest(request, m_Settings.requestSeconds);
			if (result != drogon::ReqResult::Ok || !response)
				error = result == drogon::ReqResult::Timeout ? "provider_timeout" : "provider_connection_failed";
			else
			{
				status = response->statusCode();
				Json::Value				parsedBody;
				Json::CharReaderBuilder builder;
				builder["collectComments"] = false;
				builder["rejectDupKeys"]   = true;
				builder["failIfExtra"]	   = true;
				builder["stackLimit"]	   = 8;
				std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
				std::string						  parseErrors;
				const auto						  bytes = response->body();
				const auto*						  json =
					  bytes.size() <= 32768 &&
							  reader->parse(bytes.data(), bytes.data() + bytes.size(), &parsedBody, &parseErrors) &&
							  parsedBody.isObject()
											  ? &parsedBody
											  : nullptr;
				if (status >= 200 && status < 300)
				{
					if (json && (*json)["id"].isString())
						providerId = (*json)["id"].asString();
					accepted =
						!providerId.empty() && providerId.size() <= 128 &&
						providerId.find_first_not_of(
							"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos;
					if (!accepted)
						error = "provider_invalid_response";
				}
				else
				{
					const auto name = json && (*json)["name"].isString() ? (*json)["name"].asString() : "";
					retry			= status == 408 || status == 429 || status >= 500 ||
							(status == 409 && name == "concurrent_idempotent_requests");
					error = status == 401 || status == 403 ? "provider_configuration"
							: status == 429				   ? "provider_rate_limited"
							: status >= 500				   ? "provider_unavailable"
														   : "provider_rejected";
					if (name == "daily_quota_exceeded" || name == "monthly_quota_exceeded")
						error = "provider_quota_exceeded";
					const auto& after	= response->getHeader("retry-after");
					int			seconds = 0;
					const auto	parsed	= std::from_chars(after.data(), after.data() + after.size(), seconds);
					if (parsed.ec == std::errc() && parsed.ptr == after.data() + after.size() && seconds > 0)
						delay = std::max(delay, std::min(seconds, 86400));
					if (error == "provider_quota_exceeded")
						delay = std::max(delay, 3600);
				}
			}
		}
	}
	catch (const std::exception&)
	{
		error = m_Settings.transport == "local_outbox" ? "outbox_unavailable" : "provider_connection_failed";
	}
	Transaction transaction(database);
	if (accepted)
	{
		if (!providerId.empty())
			database.Query("SELECT pg_advisory_xact_lock(hashtextextended($1,70123005))", {providerId});
		database.Query(
			"UPDATE accounts.mail_jobs SET "
			"state=$2,provider_id=NULLIF($3,''),last_error='',status_code=NULLIF($4,'0')::integer,"
			"body='',request_body=NULL,unsubscribe_token=NULL,finished_at=now() WHERE id=$1::bigint AND "
			"state='processing'",
			{id, m_Settings.transport == "resend" ? "accepted" : "delivered", providerId, std::to_string(status)});
		if (!providerId.empty())
			ApplyEmailEvents(database, providerId);
	}
	else
	{
		const bool failed = !retry || attempts + 1 >= 5;
		database.Query("UPDATE accounts.mail_jobs SET state=CASE WHEN $2::boolean THEN 'failed' ELSE 'queued' END,"
					   "body=CASE WHEN $2::boolean THEN '' ELSE body END,unsubscribe_token=CASE WHEN $2::boolean THEN "
					   "NULL ELSE unsubscribe_token END,"
					   "request_body=CASE WHEN $2::boolean THEN NULL ELSE request_body END,"
					   "finished_at=CASE WHEN $2::boolean THEN now() ELSE NULL "
					   "END,last_error=$3,status_code=NULLIF($4,'0')::integer,"
					   "available_at=now()+$5::integer*interval '1 second' WHERE id=$1::bigint AND state='processing'",
					   {id, failed ? "true" : "false", error, std::to_string(status), std::to_string(delay)});
		if (status == 429 || status == 401 || status == 403 || status >= 500)
			database.Query("UPDATE accounts.mail_worker_status SET "
						   "pause_until=GREATEST(pause_until,now()+$1::integer*interval '1 second')",
						   {std::to_string(status == 401 || status == 403 ? 300 : delay)});
	}
	transaction.Commit();
	return true;
#endif
}
} // namespace Accounts
