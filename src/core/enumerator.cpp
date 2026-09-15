#include "core/enumerator.h"

#include "core/log.h"

#include <map>
#include <mutex>

namespace xls {

namespace {

std::mutex g_mutex;
std::map<HANDLE, std::unique_ptr<Enumerator>> g_enumerators;

}

HANDLE EnumeratorRegister(std::unique_ptr<Enumerator> enumerator)
{
	// A real kernel object, so a title that closes the handle with CloseHandle instead of
	// XCloseHandle does not close something else's handle by accident.
	HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!handle) {
		return nullptr;
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	g_enumerators[handle] = std::move(enumerator);
	return handle;
}

Enumerator* EnumeratorFind(HANDLE handle)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	auto it = g_enumerators.find(handle);
	return it == g_enumerators.end() ? nullptr : it->second.get();
}

bool EnumeratorClose(HANDLE handle)
{
	std::unique_ptr<Enumerator> owned;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_enumerators.find(handle);
		if (it == g_enumerators.end()) {
			return false;
		}
		owned = std::move(it->second);
		g_enumerators.erase(it);
	}
	CloseHandle(handle);
	return true;
}

void EnumeratorShutdown()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	for (auto& entry : g_enumerators) {
		CloseHandle(entry.first);
	}
	g_enumerators.clear();
}

}
