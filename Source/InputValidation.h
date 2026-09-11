#pragma once

#include <algorithm>
#include <string_view>

namespace InputValidation
{
// Shared syntax checks. Callers retain their own field-length rules and errors.
inline bool HasPhoneCharacters(std::string_view value)
{
	return std::all_of(value.begin(),
					   value.end(),
					   [](char c)
					   { return (c >= '0' && c <= '9') || c == '+' || c == ' ' || c == '(' || c == ')' || c == '-'; });
}

inline bool IsDecimalIdentifier(std::string_view value)
{
	return !value.empty() && value.size() <= 18 &&
		   std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
}
} // namespace InputValidation
