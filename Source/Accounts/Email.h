#pragma once

#include "Database.h"
#include "LocalOutboxTransport.h"
#include "ResendTransport.h"
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>

namespace Accounts
{
struct EmailSettings
{
	bool				  enabled	   = false;
	bool				  registration = false;
	bool				  newsletters  = false;
	std::string			  origin;
	std::string			  sender;
	std::string			  footer;
	std::filesystem::path outbox;
	std::string			  transport = "local_outbox";
	std::string			  apiKey;
	std::string			  webhookKey;
	std::string			  apiOrigin		 = "https://api.resend.com";
	int					  requestSeconds = 10;
};

// Call within the business transaction: queueing and the account change commit together.
std::string QueueEmail(Database&		  database,
					   const std::string& recipient,
					   const std::string& subject,
					   const std::string& body,
					   const std::string& user		= "",
					   const std::string& challenge = "");

// Optional modules supply scheduling/eligibility without coupling accounts to their schema.
struct ScheduledEmailHooks
{
	std::function<void(Database&)>					   enqueue;
	std::function<bool(Database&, const std::string&)> eligible;
};

std::string DecodeWebhookSecret(const std::string& secret);
bool		VerifyEmailWebhook(const std::string& key,
							   const std::string& id,
							   const std::string& timestamp,
							   const std::string& signature,
							   std::string_view	  body);
// Caller owns a transaction. Events can arrive before the sending request completes.
void		ApplyEmailEvents(Database& database, const std::string& providerId);

// Claims commit before transport I/O. External retries retain their exact payload and key.
class EmailWorker
{
public:
	EmailWorker(std::string connection, EmailSettings settings, ScheduledEmailHooks hooks = {});
	~EmailWorker();
	void Start();
	void Stop();

private:
	void									  Run(std::unique_ptr<Database> database);
	std::string								  m_Connection;
	EmailSettings							  m_Settings;
	ScheduledEmailHooks						  m_Hooks;
	ResendTransport							  m_ResendTransport;
	std::mutex								  m_Mutex;
	std::condition_variable					  m_Condition;
	bool									  m_Stopping = false;
	std::thread								  m_Worker;
	LocalOutboxTransport					  m_LocalOutboxTransport;
};
} // namespace Accounts
