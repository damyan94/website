#pragma once

namespace Accounts
{
class Database;
class LocalOutboxTransport;
class ResendTransport;
struct EmailSettings;
struct ScheduledEmailHooks;

// Borrows worker-owned dependencies; each delivery uses the worker's current connection.
class EmailDeliveryService
{
public:
	EmailDeliveryService(const EmailSettings& settings,
						 const ScheduledEmailHooks& hooks,
						 const ResendTransport& resend,
						 const LocalOutboxTransport& localOutbox);

	// Claims commit before external I/O; reconciliation uses a separate transaction.
	bool DeliverOne(Database& database);

private:
	const EmailSettings& m_Settings;
	const ScheduledEmailHooks& m_Hooks;
	const ResendTransport& m_ResendTransport;
	const LocalOutboxTransport& m_LocalOutboxTransport;
};
} // namespace Accounts
