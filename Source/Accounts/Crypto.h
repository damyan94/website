#pragma once

#include <string>
#include <string_view>

namespace Accounts
{
std::string RandomToken();
std::string TokenHash(std::string_view token);
bool		ConstantEqual(std::string_view left, std::string_view right);
std::string HashPassword(std::string_view password);
bool		VerifyPassword(std::string_view password, const std::string& encoded);
// Returns -1 for invalid UTF-8. Counts Unicode scalar values, not bytes.
int	 TextLength(std::string_view text);
bool ValidPassword(std::string_view password);
} // namespace Accounts
