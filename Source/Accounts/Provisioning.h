#pragma once

#include "Configuration.h"

namespace Accounts::Provisioning
{
void RunCommand(const Configuration& configuration,
				const std::string& command,
				const std::string& email,
				bool passwordStdin);
}
