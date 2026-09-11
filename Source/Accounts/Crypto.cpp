#include "Crypto.h"
#include "stdafx.h"

#include <argon2.h>
#include <array>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace Accounts
{
namespace
{
std::string Hex(const unsigned char* data, std::size_t size)
{
	constexpr char Digits[] = "0123456789abcdef";
	std::string	   result(size * 2, '0');
	for (std::size_t i = 0; i < size; ++i)
	{
		result[i * 2]	  = Digits[data[i] >> 4];
		result[i * 2 + 1] = Digits[data[i] & 15];
	}
	return result;
}
} // namespace

std::string RandomToken()
{
	std::array<unsigned char, 32> bytes{};
	if (RAND_bytes(bytes.data(), bytes.size()) != 1)
		throw std::runtime_error("Random generator failed");
	return Hex(bytes.data(), bytes.size());
}

std::string TokenHash(std::string_view token)
{
	std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
	unsigned int							   length = 0;
	if (EVP_Digest(token.data(), token.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1)
		throw std::runtime_error("Token digest failed");
	return Hex(digest.data(), length);
}

bool ConstantEqual(std::string_view left, std::string_view right)
{
	return left.size() == right.size() && CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

std::string HashPassword(std::string_view password)
{
	std::array<unsigned char, 16> salt{};
	if (RAND_bytes(salt.data(), salt.size()) != 1)
		throw std::runtime_error("Random generator failed");
	std::array<char, 256> encoded{};
	// OWASP's Argon2id baseline: 19 MiB, two iterations, one lane.
	if (argon2id_hash_encoded(2,
							  19456,
							  1,
							  password.data(),
							  password.size(),
							  salt.data(),
							  salt.size(),
							  32,
							  encoded.data(),
							  encoded.size()) != ARGON2_OK)
		throw std::runtime_error("Password hashing failed");
	return encoded.data();
}

bool VerifyPassword(std::string_view password, const std::string& encoded)
{
	return argon2id_verify(encoded.c_str(), password.data(), password.size()) == ARGON2_OK;
}

int TextLength(std::string_view text)
{
	int count = 0;
	for (std::size_t i = 0; i < text.size(); ++count)
	{
		auto first = static_cast<unsigned char>(text[i++]);
		if (first < 0x20 || first == 0x7f)
			return -1;
		if (first < 0x80)
			continue;
		unsigned int value	 = 0;
		int			 extra	 = 0;
		unsigned int minimum = 0;
		if (first >= 0xc2 && first <= 0xdf)
		{
			value	= first & 31;
			extra	= 1;
			minimum = 0x80;
		}
		else if (first >= 0xe0 && first <= 0xef)
		{
			value	= first & 15;
			extra	= 2;
			minimum = 0x800;
		}
		else if (first >= 0xf0 && first <= 0xf4)
		{
			value	= first & 7;
			extra	= 3;
			minimum = 0x10000;
		}
		else
			return -1;
		while (extra--)
		{
			if (i >= text.size())
				return -1;
			auto next = static_cast<unsigned char>(text[i++]);
			if ((next & 0xc0) != 0x80)
				return -1;
			value = (value << 6) | (next & 63);
		}
		if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
			return -1;
	}
	return count;
}

bool ValidPassword(std::string_view password)
{
	const auto length = TextLength(password);
	return length >= 15 && length <= 128;
}
} // namespace Accounts
