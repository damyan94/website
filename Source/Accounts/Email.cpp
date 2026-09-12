#include "Email.h"
#include "Crypto.h"
#include "MailRepository.h"
#include "stdafx.h"
#include <charconv>
#include <json/json.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <sstream>

namespace Accounts
{
std::string QueueEmail(Database&		  database,
					   const std::string& recipient,
					   const std::string& subject,
					   const std::string& body,
					   const std::string& user,
					   const std::string& challenge)
{
	MailRepository repository(database);
	if (repository.PendingJobs() >= 10000)
		throw std::runtime_error("Email queue is full");
	return repository.InsertJob(challenge.empty() ? "notice" : "challenge", user, challenge, recipient, subject, body);
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
	MailRepository repository(database);
	// Serialize with acceptance and early callbacks, even before the job has a provider ID.
	repository.LockProvider(providerId);
	const auto job = repository.LockProviderJob(providerId);
	if (!job)
		return;
	// Terminal failures/complaints dominate delivery, which dominates transient events.
	// Repeated and out-of-order callbacks cannot downgrade the result.
	const auto event = repository.HighestPriorityEvent(providerId);
	if (!event)
		return;
	const auto type		= *event;
	const auto state	= type == "email.complained"							 ? "complained"
						  : type == "email.bounced"								 ? "bounced"
						  : type == "email.suppressed" || type == "email.failed" ? "failed"
						  : type == "email.delivered"							 ? "delivered"
						  : type == "email.delivery_delayed"					 ? "delayed"
																				 : "accepted";
	const bool suppress = type == "email.complained" || type == "email.bounced" || type == "email.suppressed";
	repository.UpdateEventStatus(
		job->id, state, suppress || type == "email.failed" || type == "email.delivery_delayed" ? type : "");
	if (suppress)
		repository.SuppressRecipient(job->recipient, type);
}

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
	if (!MailRepository(*database).TryLockWorker())
		database.reset(); // Another backend delivers for this database; take over when its lock is released.
	if (m_Settings.transport == "local_outbox")
	{
		m_LocalOutboxTransport.Start(m_Settings.outbox);
	}
	else
	{
		m_ResendTransport.Start(m_Settings.apiOrigin);
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
	m_ResendTransport.Stop();
	m_LocalOutboxTransport.Stop();
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
				if (!MailRepository(*database).TryLockWorker())
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
				MailRepository(*database).UpdateHeartbeat(schedulerError);
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
					MailRepository(*database).RecordWorkerError();
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
	MailRepository repository(database);
	std::string id, subject, body, recipient, kind, unsubscribe, deliveryKey, requestBody;
	int			attempts = 0;
	Json::Value message;
	{
		Transaction transaction(database);
		const auto	row = repository.LockDueJob(m_Settings.transport);
		if (!row)
		{
			transaction.Commit();
			return false;
		}
		id					 = row->id;
		kind				 = row->kind;
		recipient			 = row->recipient;
		subject				 = row->subject;
		body				 = row->body;
		unsubscribe			 = row->unsubscribe;
		attempts			 = row->attempts;
		deliveryKey			 = row->deliveryKey;
		requestBody			 = row->requestBody;
		bool		eligible = !row->expired;
		std::string reason	 = eligible ? "ineligible" : "expired";
		if (!row->transport.empty() && row->transport != m_Settings.transport)
		{
			eligible = false;
			reason	 = "transport_changed";
		}
		if (repository.IsSuppressed(recipient))
		{
			eligible = false;
			reason	 = "recipient_suppressed";
		}
		if (kind == "reminder")
			eligible = eligible && m_Hooks.eligible && m_Hooks.eligible(database, id);
		if (kind == "challenge")
			eligible =
				eligible &&
				repository.HasCurrentChallenge(row->challenge, m_Settings.registration, m_Settings.newsletters);
		if (kind == "newsletter")
		{
			eligible =
				eligible && m_Settings.newsletters &&
				repository.HasCurrentSubscription(row->user, row->generation, recipient, id);
			if (eligible && unsubscribe.empty())
			{
				unsubscribe = RandomToken();
				repository.InsertNewsletterLink(TokenHash(unsubscribe), row->user, row->generation);
				body += "\n\nUnsubscribe / Отписване:\n" + m_Settings.origin +
						"/email#action=unsubscribe&token=" + unsubscribe;
			}
		}
		const bool retryExpired = m_Settings.transport == "resend" && row->retryWindowExpired;
		if (!eligible || attempts >= 5 || retryExpired)
		{
			repository.FinishUnsentJob(id,
									   eligible ? "failed" : "skipped",
									   retryExpired ? "retry_window_expired"
									   : eligible	? "attempts_exhausted"
													: reason);
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
		repository.RecordAttempt(id, body, unsubscribe, m_Settings.transport, deliveryKey, requestBody);
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
			m_LocalOutboxTransport.Deliver(m_Settings.outbox / (id + ".json"), message);
			accepted = true;
		}
		else
		{
			const auto [result, response] =
				m_ResendTransport.Send(m_Settings.apiKey, deliveryKey, requestBody, m_Settings.requestSeconds);
			if (result != drogon::ReqResult::Ok || !response)
				error = result == drogon::ReqResult::Timeout ? "provider_timeout" : "provider_connection_failed";
			else
			{
				status = response->statusCode();
				const auto json = ResendTransport::DecodeResponse(response);
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
			repository.LockProvider(providerId);
		repository.CompleteDelivery(
			id, m_Settings.transport == "resend" ? "accepted" : "delivered", providerId, status);
		if (!providerId.empty())
			ApplyEmailEvents(database, providerId);
	}
	else
	{
		const bool failed = !retry || attempts + 1 >= 5;
		repository.RecordDeliveryFailure(id, failed, error, status, delay);
		if (status == 429 || status == 401 || status == 403 || status >= 500)
			repository.ExtendProviderPause(status == 401 || status == 403 ? 300 : delay);
	}
	transaction.Commit();
	return true;
#endif
}
} // namespace Accounts
