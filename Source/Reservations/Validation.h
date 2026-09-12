#pragma once

#include "Accounts/Crypto.h"
#include "Accounts/Types.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <initializer_list>

namespace Reservations::Validation
{
using Accounts::RequestError;

inline void Keys(const Json::Value&				 value,
		  std::initializer_list<const char*> required,
		  std::initializer_list<const char*> optional = {})
{
	if (!value.isObject())
		throw RequestError(400, "Expected an object");
	for (const auto* key : required)
		if (!value.isMember(key))
			throw RequestError(400, "Missing field");
	for (const auto& key : value.getMemberNames())
		if (std::find(required.begin(), required.end(), key) == required.end() &&
			std::find(optional.begin(), optional.end(), key) == optional.end())
			throw RequestError(400, "Unexpected field");
}

inline std::string Text(const Json::Value& value, int minimum = 1, int maximum = 100)
{
	if (!value.isString())
		throw RequestError(400, "Expected text");
	auto	  text = value.asString();
	const int size = Accounts::TextLength(text);
	if (size < minimum || size > maximum)
		throw RequestError(400, "Invalid text length or encoding");
	return text;
}

inline int Number(const Json::Value& value, int minimum, int maximum)
{
	if (!value.isInt() || value.asInt() < minimum || value.asInt() > maximum)
		throw RequestError(400, "Invalid number");
	return value.asInt();
}

inline int QueryNumber(const Json::Value& value, int minimum, int maximum)
{
	const auto text	  = Text(value, 1, 9);
	int		   result = 0;
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
	if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || result < minimum || result > maximum)
		throw RequestError(400, "Invalid query number");
	return result;
}

inline std::string Id(const Json::Value& value)
{
	auto id = Text(value, 1, 64);
	if (!std::all_of(id.begin(),
					 id.end(),
					 [](char c) {
						 return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
								c == '-' || c == '_';
					 }))
		throw RequestError(400, "Invalid identifier");
	return id;
}

inline std::string Date(const Json::Value& value)
{
	const auto date = Text(value, 10, 10);
	if (date[4] != '-' || date[7] != '-')
		throw RequestError(400, "Use YYYY-MM-DD");
	auto	   part = [&](int from, int size) { return QueryNumber(date.substr(from, size), 0, 9999); };
	const auto year = part(0, 4), month = part(5, 2), day = part(8, 2);
	if (year < 2000 || year > 2100 ||
		!std::chrono::year_month_day(std::chrono::year(year), std::chrono::month(month), std::chrono::day(day)).ok())
		throw RequestError(400, "Invalid date");
	return date;
}

inline int Minute(const Json::Value& value)
{
	const auto time = Text(value, 5, 5);
	if (time[2] != ':')
		throw RequestError(400, "Use HH:MM");
	return QueryNumber(time.substr(0, 2), 0, 23) * 60 + QueryNumber(time.substr(3, 2), 0, 59);
}

} // namespace Reservations::Validation
