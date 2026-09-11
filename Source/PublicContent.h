#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

namespace PublicContent
{

// Only pass a file deliberately prepared for public access. Every field is exposed.
// Loads and serializes once at startup; throws on invalid or oversized content.
std::string Load(const std::filesystem::path& path);

// Public reads retain a complete immutable document while a publisher swaps in
// its replacement. Publishing never requires file I/O on the HTTP event loop.
class Snapshot
{
public:
	explicit Snapshot(std::string json);
	std::shared_ptr<const std::string> Get() const noexcept;
	void							   Publish(std::shared_ptr<const std::string> json) noexcept;

private:
	// Use the shared_ptr atomic free functions for GCC 11's standard library,
	// which does not yet provide atomic<shared_ptr<T>>.
	std::shared_ptr<const std::string> m_Json;
};

void RegisterHandlers(std::shared_ptr<Snapshot> snapshot);

// The handler owns an immutable snapshot; it performs no file I/O per request.
void RegisterHandlers(std::string json);

} // namespace PublicContent
