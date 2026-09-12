#include "Email.h"
#include "Crypto.h"
#include "EmailDeliveryService.h"
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
	EmailDeliveryService delivery(m_Settings, m_Hooks, m_ResendTransport, m_LocalOutboxTransport);
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
			delivery.DeliverOne(*database);
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
} // namespace Accounts
