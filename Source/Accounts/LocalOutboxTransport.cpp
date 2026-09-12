#include "LocalOutboxTransport.h"
#include "Crypto.h"
#include "stdafx.h"
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace Accounts
{
namespace
{
#ifndef _WIN32
class File
{
public:
	explicit File(int descriptor)
		: fd(descriptor)
	{
		if (fd < 0)
			throw std::runtime_error("Email outbox is unavailable");
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

void Sync(int fd)
{
	while (::fsync(fd) != 0)
		if (errno != EINTR)
			throw std::runtime_error("Email outbox sync failed");
}
#endif
} // namespace

LocalOutboxTransport::~LocalOutboxTransport()
{
	Stop();
}

void LocalOutboxTransport::Start(const std::filesystem::path& directory)
{
#ifdef _WIN32
	(void)directory;
	throw std::runtime_error("The local email outbox currently requires a POSIX host");
#else
	std::filesystem::create_directories(directory);
	std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
	File lock(
		::open((directory / ".worker.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
	if (::flock(lock.fd, LOCK_EX | LOCK_NB) != 0)
		throw std::runtime_error("Another email worker owns this outbox");
	m_Lock = std::exchange(lock.fd, -1);
#endif
}

void LocalOutboxTransport::Stop()
{
#ifndef _WIN32
	if (m_Lock >= 0)
	{
		::close(m_Lock);
		m_Lock = -1;
	}
#endif
}

void LocalOutboxTransport::Deliver(const std::filesystem::path& path, const Json::Value& message) const
{
#ifdef _WIN32
	(void)path;
	(void)message;
	throw std::runtime_error("The local email outbox currently requires a POSIX host");
#else
	Json::StreamWriterBuilder writer;
	writer["indentation"] = "  ";
	writer["emitUTF8"]	  = true;
	const auto bytes	  = Json::writeString(writer, message) + '\n';
	const auto temp		  = path.parent_path() / (".mail-" + RandomToken() + ".tmp");
	File	   file(::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));

	struct Cleanup
	{
		std::filesystem::path path;

		~Cleanup()
		{
			::unlink(path.c_str());
		}
	} cleanup{temp};

	std::size_t offset = 0;
	while (offset < bytes.size())
	{
		const auto written = ::write(file.fd, bytes.data() + offset, bytes.size() - offset);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			throw std::runtime_error("Email outbox write failed");
		offset += written;
	}
	Sync(file.fd);
	if (::rename(temp.c_str(), path.c_str()) != 0)
		throw std::runtime_error("Email outbox rename failed");
	File directory(::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
	Sync(directory.fd);
#endif
}
} // namespace Accounts
