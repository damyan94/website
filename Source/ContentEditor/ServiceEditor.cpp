#include "ServiceEditor.h"
#include "Accounts/Crypto.h"
#include "Accounts/Module.h"
#include "stdafx.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <regex>
#include <set>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ContentEditor
{
namespace
{
using Error						   = Accounts::RequestError;
constexpr std::size_t MaximumBytes = 1024 * 1024;

std::string Serialize(const Json::Value& value, bool pretty = false)
{
	Json::StreamWriterBuilder writer;
	writer["indentation"] = pretty ? "  " : "";
	writer["emitUTF8"]	  = true;
	return Json::writeString(writer, value);
}

std::string Revision(const Json::Value& document)
{
	return Accounts::TokenHash(Serialize(document));
}

bool Beneath(const std::filesystem::path& path, const std::filesystem::path& parent)
{
	const auto mismatch = std::mismatch(parent.begin(), parent.end(), path.begin(), path.end());
	return mismatch.first == parent.end();
}

bool Identifier(const Json::Value& value)
{
	static const std::regex pattern("[a-z0-9][a-z0-9-]{0,63}");
	return value.isString() && std::regex_match(value.asString(), pattern);
}

void Text(const Json::Value& value, int minimum, int maximum, bool multiline = false)
{
	if (!value.isString())
		throw Error(400, "Content fields must be text");
	auto text = value.asString();
	if (multiline)
		for (auto& character : text)
			if (character == '\n' || character == '\r' || character == '\t')
				character = ' ';
	const auto length = Accounts::TextLength(text);
	if (length < minimum || length > maximum)
		throw Error(400, "Content text length or UTF-8 encoding is invalid");
	if (minimum && text.find_first_not_of(' ') == std::string::npos)
		throw Error(400, "Required text cannot be blank");
}

void Localized(const Json::Value& value, const Json::Value& document, int maximum, bool multiline = false)
{
	if (!value.isObject() || value.size() != document["locales"].size())
		throw Error(400, "Supply text for each site language");
	for (const auto& locale : document["locales"])
		Text(value[locale.asString()], 1, maximum, multiline);
}

bool BookingUrl(const Json::Value& value)
{
	if (!value.isString())
		return false;
	const auto url = value.asString();
	if (url.size() > 2048 || url.find_first_of("\\\r\n\t ") != std::string::npos || Accounts::TextLength(url) < 1)
		return false;
	static const std::regex phone(R"(tel:\+?[0-9]{3,30})");
	if (std::regex_match(url, phone))
		return true;
	if (!url.starts_with("https://"))
		return false;
	const auto				end		  = url.find_first_of("/?#", 8);
	const auto				authority = url.substr(8, end == std::string::npos ? end : end - 8);
	static const std::regex host(R"([A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?(:[0-9]{1,5})?)");
	if (!std::regex_match(authority, host))
		return false;
	const auto colon = authority.find(':');
	if (colon != std::string::npos)
	{
		const auto port = std::stoi(authority.substr(colon + 1));
		if (port < 1 || port > 65535)
			return false;
	}
	return true;
}

void Sync(int descriptor)
{
	while (::fsync(descriptor) != 0)
		if (errno != EINTR)
			throw Error(503, "Content storage could not be synchronized; reload before retrying");
}

class File
{
public:
	File(const std::filesystem::path& path, int flags)
		: fd(::open(path.c_str(), flags | O_CLOEXEC | O_NOFOLLOW, 0600))
	{
		if (fd < 0)
			throw Error(503, "Content storage is unavailable");
	}

	~File()
	{
		if (fd >= 0)
			::close(fd);
	}

	File(const File&)			 = delete;
	File& operator=(const File&) = delete;
	int	  fd;
};

void AtomicReplace(const std::filesystem::path& destination,
				   const std::string&			bytes,
				   const std::function<void()>& afterRename = {})
{
	const auto temporary = destination.parent_path() / (".content-" + Accounts::RandomToken() + ".tmp");
	File	   file(temporary, O_WRONLY | O_CREAT | O_EXCL);

	struct RemoveTemporary
	{
		std::filesystem::path path;

		~RemoveTemporary()
		{
			::unlink(path.c_str());
		}
	} cleanup{temporary};

	std::size_t written = 0;
	while (written < bytes.size())
	{
		const auto count = ::write(file.fd, bytes.data() + written, bytes.size() - written);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			throw Error(503, "Content could not be saved");
		written += count;
	}
	Sync(file.fd);
	if (::rename(temporary.c_str(), destination.c_str()) != 0)
		throw Error(503, "Content could not be replaced");
	// Publish before directory fsync: if that final durability check fails, memory
	// still agrees with the file that was replaced. The client must reload state.
	if (afterRename)
		afterRename();
	File directory(destination.parent_path(), O_RDONLY | O_DIRECTORY);
	Sync(directory.fd);
}
} // namespace

ServiceEditor::ServiceEditor(std::filesystem::path					  source,
							 const std::filesystem::path&			  publicRoot,
							 std::filesystem::path					  backups,
							 std::vector<std::string>				  currencies,
							 std::shared_ptr<PublicContent::Snapshot> snapshot)
	: m_Source(std::filesystem::weakly_canonical(source)),
	  m_Backups(std::filesystem::weakly_canonical(backups)),
	  m_Currencies(std::move(currencies)),
	  m_Snapshot(std::move(snapshot))
{
	const auto root = std::filesystem::weakly_canonical(publicRoot);
	if (!m_Snapshot || std::filesystem::is_symlink(source) || std::filesystem::is_symlink(backups) ||
		Beneath(m_Source, root) || Beneath(m_Backups, root) || Beneath(m_Source, m_Backups))
		throw std::runtime_error("Content source and backups must be private paths outside the public root");
	if (m_Currencies.empty())
		throw std::runtime_error("Content editor needs allowed currencies");
	for (const auto& currency : m_Currencies)
		if (!std::regex_match(currency, std::regex("[A-Z]{3}")))
			throw std::runtime_error("Invalid content editor currency");
	File lockFile(m_Source.string() + ".editor.lock", O_RDWR | O_CREAT);
	if (::flock(lockFile.fd, LOCK_EX | LOCK_NB) != 0)
		throw std::runtime_error("Another editor owns this content file");
	// Read under file ownership: a previous server may have published between
	// the application's initial load and this process acquiring the lock.
	const auto document = ReadDocument();
	ValidateDocument(document);
	std::filesystem::create_directories(m_Backups);
	std::filesystem::permissions(m_Backups, std::filesystem::perms::owner_all);
	m_Snapshot->Publish(std::make_shared<const std::string>(Serialize(document)));
	m_Lock = std::exchange(lockFile.fd, -1);
}

ServiceEditor::~ServiceEditor()
{
	if (m_Lock >= 0)
		::close(m_Lock);
}

Json::Value ServiceEditor::ReadDocument() const
{
	try
	{
		if (std::filesystem::is_symlink(m_Source))
			throw std::runtime_error("Content source became a symlink");
		const auto								bytes = PublicContent::Load(m_Source);
		Json::CharReaderBuilder					builder;
		const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
		Json::Value								document;
		std::string								errors;
		if (!reader->parse(bytes.data(), bytes.data() + bytes.size(), &document, &errors))
			throw std::runtime_error("Content parse failed");
		return document;
	}
	catch (const std::exception&)
	{
		throw Error(503, "The content file is unavailable or invalid; no changes were saved");
	}
}

void ServiceEditor::ValidateService(const Json::Value& service, const Json::Value& document) const
{
	if (!service.isObject() || !Identifier(service["id"]))
		throw Error(400, "Invalid service ID");
	Localized(service["title"], document, 200);
	Localized(service["description"], document, 4000, true);
	bool categoryFound = false;
	for (const auto& category : document["categories"])
		if (service["category"] == category["id"])
			categoryFound = true;
	if (!categoryFound)
		throw Error(400, "Choose an existing category");
	if (service.isMember("available") && !service["available"].isBool())
		throw Error(400, "Availability must be a boolean");
	if (!BookingUrl(service["bookingUrl"]))
		throw Error(400, "Use an HTTPS booking URL or tel: followed by a phone number");
	const auto& variants = service["variants"];
	if (!variants.isArray() || variants.size() > 20)
		throw Error(400, "A service can have up to 20 duration options");
	std::set<int> durations;
	std::string	  serviceCurrency;
	for (const auto& variant : variants)
	{
		if (!variant.isObject() || variant.size() != 3 || !variant.isMember("durationMinutes") ||
			!variant.isMember("priceMinor") || !variant.isMember("currency"))
			throw Error(400, "Unexpected duration option fields");
		if (!variant["durationMinutes"].isInt() || variant["durationMinutes"].asInt() < 1 ||
			variant["durationMinutes"].asInt() > 1440 || !durations.insert(variant["durationMinutes"].asInt()).second)
			throw Error(400, "Durations must be unique whole minutes from 1 to 1440");
		if (!variant["currency"].isString())
			throw Error(400, "Invalid currency");
		const auto currency = variant["currency"].asString();
		if (std::find(m_Currencies.begin(), m_Currencies.end(), currency) == m_Currencies.end() ||
			(!serviceCurrency.empty() && currency != serviceCurrency))
			throw Error(400, "Use one allowed currency per service");
		serviceCurrency = currency;
		if (!variant["priceMinor"].isNull() &&
			(!variant["priceMinor"].isInt64() || variant["priceMinor"].asInt64() < 0 ||
			 variant["priceMinor"].asInt64() > 100000000))
			throw Error(400, "Price must be a nonnegative integer in minor units, or null");
	}
}

void ServiceEditor::ValidateDocument(const Json::Value& document) const
{
	if (document["schemaVersion"] != 1 || !document["locales"].isArray() || document["locales"].empty() ||
		document["locales"].size() > 32 || !document["defaultLocale"].isString() || !document["categories"].isArray() ||
		document["categories"].empty() || document["categories"].size() > 100 || !document["services"].isArray() ||
		document["services"].size() > 1000)
		throw Error(400, "Unsupported service content document");
	std::set<std::string> locales;
	for (const auto& locale : document["locales"])
	{
		if (!locale.isString() ||
			!std::regex_match(locale.asString(), std::regex("[A-Za-z]{2,3}(-[A-Za-z0-9]{2,8})*")) ||
			!locales.insert(locale.asString()).second)
			throw Error(400, "Invalid content languages");
	}
	if (!locales.contains(document["defaultLocale"].asString()))
		throw Error(400, "Default language is missing");
	std::set<std::string> categories;
	for (const auto& category : document["categories"])
	{
		if (!Identifier(category["id"]) || !categories.insert(category["id"].asString()).second)
			throw Error(400, "Invalid or duplicate category ID");
		Localized(category["title"], document, 200);
	}
	std::set<std::string> ids;
	for (const auto& service : document["services"])
	{
		ValidateService(service, document);
		if (!ids.insert(service["id"].asString()).second)
			throw Error(400, "Duplicate service ID");
	}
}

Json::Value ServiceEditor::View(const Json::Value& document) const
{
	Json::Value result(Json::objectValue);
	result["revision"] = Revision(document);
	for (const auto* key : {"locales", "defaultLocale", "categories", "services"})
		result[key] = document[key];
	result["currencies"] = Json::arrayValue;
	for (const auto& currency : m_Currencies)
		result["currencies"].append(currency);
	const auto& business = document["business"];
	const auto	phone	 = business.isObject() && business["phone"].isString() ? business["phone"].asString() : "";
	result["defaultBookingUrl"] = BookingUrl(Json::Value("tel:" + phone)) ? "tel:" + phone : "";
	return result;
}

Json::Value ServiceEditor::Read()
{
	std::lock_guard lock(m_Mutex);
	const auto		document = ReadDocument();
	ValidateDocument(document);
	return View(document);
}

Json::Value ServiceEditor::Save(const std::string& actor, const Json::Value& request, bool create)
{
	std::lock_guard lock(m_Mutex);
	if (!request.isObject() || request.size() != 2 || !request["revision"].isString() || !request["service"].isObject())
		throw Error(400, "Supply a revision and service");
	auto document = ReadDocument();
	ValidateDocument(document);
	const auto previous	   = document;
	const auto oldRevision = Revision(document);
	if (request["revision"].asString() != oldRevision)
		throw Error(409, "Content changed elsewhere; reload the menu before saving");
	const auto& input	 = request["service"];
	const char* fields[] = {"id", "title", "description", "category", "variants", "available", "bookingUrl"};
	if (input.size() != 7)
		throw Error(400, "Unexpected or missing service fields");
	for (const auto* field : fields)
		if (!input.isMember(field))
			throw Error(400, "Unexpected or missing service fields");
	if (!input["id"].isString() || !input["available"].isBool())
		throw Error(400, "Invalid service ID or availability");
	Json::Value		 edited;
	Json::ArrayIndex index = document["services"].size();
	if (create)
	{
		if (!input["id"].asString().empty())
			throw Error(400, "New service IDs are assigned by the server");
		edited["id"] = "service-" + Accounts::RandomToken().substr(0, 24);
	}
	else
	{
		for (Json::ArrayIndex i = 0; i < document["services"].size(); ++i)
			if (document["services"][i]["id"] == input["id"])
			{
				index = i;
				break;
			}
		if (index == document["services"].size())
			throw Error(404, "Service not found");
		edited = document["services"][index];
	}
	for (const auto* field : fields)
		if (std::string_view(field) != "id")
			edited[field] = input[field];
	ValidateService(edited, document);
	if (create)
		document["services"].append(edited);
	else
		document["services"][index] = edited;
	ValidateDocument(document);
	const auto compact = std::make_shared<const std::string>(Serialize(document));
	const auto pretty  = Serialize(document, true) + '\n';
	if (pretty.size() > MaximumBytes)
		throw Error(413, "The content document would exceed 1 MiB");
	auto result			= View(document);
	result["serviceId"] = edited["id"];
	if (result["revision"].asString() == oldRevision)
		return result;
	std::size_t		backupCount = 0;
	std::error_code storageError;
	const auto		backups = std::filesystem::directory_iterator(m_Backups, storageError);
	if (storageError)
		throw Error(503, "Content backup storage is unavailable; no changes were saved");
	for (const auto& entry : backups)
		if (entry.path().extension() == ".json" && ++backupCount >= 512)
			throw Error(503, "The backup archive is full; ask the site owner to archive older copies");
	const auto	backupId = Accounts::RandomToken().substr(0, 32);
	Json::Value backup(Json::objectValue);
	backup["document"]			  = previous;
	backup["previousRevision"]	  = oldRevision;
	backup["replacementRevision"] = result["revision"];
	backup["actorId"]			  = actor;
	backup["createdAtUnixMs"]	  = Json::Int64(
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
			.count());
	// Backups record publication attempts. An interrupted/failed attempt may leave
	// a backup without a replacement; the source file remains authoritative.
	AtomicReplace(m_Backups / (backupId + ".json"), Serialize(backup, true) + '\n');
	AtomicReplace(m_Source, pretty, [this, compact] { m_Snapshot->Publish(compact); });
	result["backupId"] = backupId;
	return result;
}

void RegisterHandlers(Accounts::Module& accounts, std::shared_ptr<ServiceEditor> editor)
{
	accounts.RegisterProtectedHandler("/api/v1/admin/content/services",
									  drogon::Get,
									  "admin",
									  [editor](const Accounts::Identity&, const Json::Value&)
									  {
										  Accounts::Reply reply;
										  reply.body = editor->Read();
										  return reply;
									  });
	for (const auto method : {drogon::Patch, drogon::Post})
		accounts.RegisterProtectedHandler(
			"/api/v1/admin/content/services",
			method,
			"admin",
			[editor, method](const Accounts::Identity& identity, const Json::Value& body)
			{
				Accounts::Reply reply;
				reply.body	 = editor->Save(identity.id, body, method == drogon::Post);
				reply.status = method == drogon::Post ? 201 : 200;
				return reply;
			},
			256 * 1024);
	accounts.RegisterUiAsset("/accounts-assets/content.js", "content.js", "text/javascript; charset=utf-8");
}
} // namespace ContentEditor
