#include "Provisioning.h"
#include "AccountStore.h"
#include "Crypto.h"
#include "Resources.h"
#include "stdafx.h"

#include <iostream>
#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace Accounts::Provisioning
{
namespace
{
std::string ReadPassword(bool fromStdin)
{
	std::string password;
	if (fromStdin)
	{
		// Explicit pipe mode is for provisioning/tests; never a command-line password.
		std::getline(std::cin, password);
		return password;
	}
#ifdef _WIN32
	const auto handle = GetStdHandle(STD_INPUT_HANDLE);
	DWORD	   mode	  = 0;
	if (!GetConsoleMode(handle, &mode) || !SetConsoleMode(handle, mode & ~ENABLE_ECHO_INPUT))
		throw std::runtime_error("Use an interactive terminal or --password-stdin");

	struct Restore
	{
		HANDLE handle;
		DWORD  mode;

		~Restore()
		{
			SetConsoleMode(handle, mode);
		}
	} restore{handle, mode};
#else
	termios original{};
	if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &original) != 0)
		throw std::runtime_error("Use an interactive terminal or --password-stdin");
	auto hidden = original;
	hidden.c_lflag &= ~ECHO;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0)
		throw std::runtime_error("Cannot disable terminal echo");

	struct Restore
	{
		termios value;

		~Restore()
		{
			tcsetattr(STDIN_FILENO, TCSAFLUSH, &value);
		}
	} restore{original};
#endif
	std::cout << "Password (15–128 characters): " << std::flush;
	std::getline(std::cin, password);
	std::string confirmation;
	std::cout << "\nRepeat password: " << std::flush;
	std::getline(std::cin, confirmation);
	std::cout << '\n';
	if (!ConstantEqual(password, confirmation))
		throw std::runtime_error("Passwords do not match");
	return password;
}

} // namespace

void RunCommand(const Configuration& configuration, const std::string& command, const std::string& email, bool passwordStdin)
{
	if (command == "migrate-accounts")
	{
		Database database(configuration.connection);
		// Serialize the entire version decision across concurrent provisioners.
		database.Query("SELECT pg_advisory_lock(70123001)");
		// Earlier migrations are immutable; apply only the missing versions.
		if (database.Query("SELECT to_regclass('accounts.schema_version') IS NOT NULL").Get(0, 0) != "t")
			database.Script(ReadResource(configuration.schemaFile, 1024 * 1024));
		auto version = database.Query("SELECT version FROM accounts.schema_version");
		if (version.Count() == 1 && version.Get(0, 0) == "1")
			database.Script(ReadResource(configuration.customerSchemaFile, 1024 * 1024));
		version = database.Query("SELECT version FROM accounts.schema_version");
		if (version.Count() == 1 && version.Get(0, 0) == "2")
			database.Script(ReadResource(configuration.activitySchemaFile, 1024 * 1024));
		version = database.Query("SELECT version FROM accounts.schema_version");
		if (version.Count() == 1 && version.Get(0, 0) == "3")
			database.Script(ReadResource(configuration.deliverySchemaFile, 1024 * 1024));
		database.CheckSchema();
		std::cout << "Accounts schema ready.\n";
		return;
	}
	AccountStore store(configuration.connection, configuration.store);
	const auto	 password = ReadPassword(passwordStdin);
	if (command == "bootstrap-admin")
		store.Bootstrap(email, password);
	else if (command == "reset-account-password")
		store.ResetPassword(email, password);
	else
		throw std::runtime_error("Unknown accounts command");
	std::cout << "Account operation completed.\n";
}

} // namespace Accounts::Provisioning
