#pragma once

#include "Accounts/Module.h"
#include "Configuration.h"
#include "Controller.h"
#include "ReservationService.h"

namespace Reservations
{
// Site-specific schedules are configuration; appointments and their immutable
// commercial/contact snapshots are private database records.
class Module : public std::enable_shared_from_this<Module>
{
public:
	Module(Json::Value								settings,
		   std::filesystem::path					configDirectory,
		   std::shared_ptr<PublicContent::Snapshot> content,
		   Accounts::StoreSettings					accounts);
	void						  Prepare(const std::string& connection, bool migrate);
	void						  RegisterHandlers(Accounts::Module& accounts);
	Accounts::ScheduledEmailHooks EmailHooks();

private:
	Configuration m_Configuration;
	ReservationService m_Service;
	Controller m_Controller;
};
} // namespace Reservations
