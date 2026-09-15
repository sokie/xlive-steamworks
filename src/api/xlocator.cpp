// #5230 - #5238 XLocator: an advertised server is a public lobby tagged as a locator entry.
// The advertiser must be a Steam client, a headless dedicated server is not covered.
#include "xlive/xfuncs.h"
#include "api/xlive.h"

#include "core/config.h"
#include "core/enumerator.h"
#include "core/log.h"
#include "core/net.h"
#include "core/overlapped.h"
#include "core/steam.h"
#include "core/users.h"
#include "core/utils.h"

#include <memory>
#include <mutex>

namespace {

std::mutex g_mutex;
CSteamID g_advertisedLobby;
std::shared_ptr<xls::SteamCall<LobbyCreated_t>> g_pendingCreate;

std::string Key(const char* name)
{
	return xls::Cfg().lobbyKeyPrefix + name;
}

std::string PropertyKey(DWORD propertyId)
{
	return xls::FormatA("%sp%08x", xls::Cfg().lobbyKeyPrefix.c_str(), propertyId);
}

void WriteProperties(CSteamID lobby, DWORD count, const XUSER_PROPERTY* properties)
{
	for (DWORD i = 0; i < count; i++) {
		xls::StoredProperty stored;
		const XUSER_DATA& value = properties[i].value;
		switch (value.type) {
			case XUSER_DATA_TYPE_INT32: stored.Set(value.type, &value.nData, 4); break;
			case XUSER_DATA_TYPE_INT64: stored.Set(value.type, &value.i64Data, 8); break;
			case XUSER_DATA_TYPE_DOUBLE: stored.Set(value.type, &value.dblData, 8); break;
			case XUSER_DATA_TYPE_FLOAT: stored.Set(value.type, &value.fData, 4); break;
			case XUSER_DATA_TYPE_DATETIME: stored.Set(value.type, &value.ftData, 8); break;
			case XUSER_DATA_TYPE_UNICODE: stored.Set(value.type, value.string.pwszData, value.string.cbData); break;
			case XUSER_DATA_TYPE_BINARY: stored.Set(value.type, value.binary.pbData, value.binary.cbData); break;
			default: continue;
		}
		xls::SteamMatchmaking()->SetLobbyData(lobby, PropertyKey(properties[i].dwPropertyId).c_str(), stored.ToString().c_str());
	}
}

struct ServerEntry {
	XLOCATOR_SEARCHRESULT result;
	std::vector<XUSER_PROPERTY> properties;
	std::vector<xls::StoredProperty> values;
};

bool ReadServer(CSteamID lobby, ServerEntry& entry)
{
	ISteamMatchmaking* matchmaking = xls::SteamMatchmaking();
	if (strcmp(matchmaking->GetLobbyData(lobby, Key("locator").c_str()), "1") != 0) {
		return false;
	}
	memset(&entry.result, 0, sizeof(entry.result));
	CSteamID owner = matchmaking->GetLobbyOwner(lobby);
	entry.result.serverID = xls::XuidFromSteamId(owner);
	entry.result.dwServerType = (DWORD)strtoul(matchmaking->GetLobbyData(lobby, Key("type").c_str()), nullptr, 10);
	xls::NetXnaddrForSteamId(owner, &entry.result.serverAddress);
	xls::HexDecode(matchmaking->GetLobbyData(lobby, Key("xnkid").c_str()), entry.result.xnkid.ab, sizeof(entry.result.xnkid.ab));
	xls::HexDecode(matchmaking->GetLobbyData(lobby, Key("xnkey").c_str()), entry.result.xnkey.ab, sizeof(entry.result.xnkey.ab));
	entry.result.dwMaxPublicSlots = (DWORD)strtoul(matchmaking->GetLobbyData(lobby, Key("pub").c_str()), nullptr, 10);
	entry.result.dwMaxPrivateSlots = (DWORD)strtoul(matchmaking->GetLobbyData(lobby, Key("priv").c_str()), nullptr, 10);
	entry.result.dwFilledPublicSlots = (DWORD)strtoul(matchmaking->GetLobbyData(lobby, Key("fpub").c_str()), nullptr, 10);
	entry.result.dwFilledPrivateSlots = (DWORD)strtoul(matchmaking->GetLobbyData(lobby, Key("fpriv").c_str()), nullptr, 10);

	int count = matchmaking->GetLobbyDataCount(lobby);
	std::string prefix = xls::Cfg().lobbyKeyPrefix + "p";
	for (int i = 0; i < count; i++) {
		char key[k_nMaxLobbyKeyLength] = {};
		char value[k_cubChatMetadataMax] = {};
		if (!matchmaking->GetLobbyDataByIndex(lobby, i, key, sizeof(key), value, sizeof(value))) {
			continue;
		}
		if (strncmp(key, prefix.c_str(), prefix.size()) != 0) {
			continue;
		}
		DWORD propertyId = (DWORD)strtoul(key + prefix.size(), nullptr, 16);
		xls::StoredProperty stored;
		if (!stored.FromString((uint8_t)XPROPERTYTYPEFROMID(propertyId), value)) {
			continue;
		}
		entry.values.push_back(stored);
		XUSER_PROPERTY property = {};
		property.dwPropertyId = propertyId;
		entry.properties.push_back(property);
	}
	for (size_t i = 0; i < entry.properties.size(); i++) {
		entry.values[i].Fill(entry.properties[i].value);
	}
	entry.result.cProperties = (DWORD)entry.properties.size();
	return true;
}

class LocatorEnumerator : public xls::Enumerator {
public:
	std::shared_ptr<xls::SteamCall<LobbyMatchList_t>> request;
	std::vector<uint64_t> onlyServers;
	std::vector<ServerEntry> servers;
	bool loaded = false;
	size_t index = 0;
	DWORD batchSize = 0;

	bool Ready() override
	{
		if (!request) {
			return true;
		}
		if (!request->Poll()) {
			return false;
		}
		if (!loaded) {
			loaded = true;
			if (!request->failed) {
				for (uint32 i = 0; i < request->result.m_nLobbiesMatching; i++) {
					CSteamID lobby = xls::SteamMatchmaking()->GetLobbyByIndex((int)i);
					ServerEntry entry;
					if (!ReadServer(lobby, entry)) {
						continue;
					}
					if (!onlyServers.empty() && std::find(onlyServers.begin(), onlyServers.end(), entry.result.serverID) == onlyServers.end()) {
						continue;
					}
					servers.push_back(std::move(entry));
				}
			}
		}
		return true;
	}

	DWORD Next(void* buffer, DWORD bufferSize, DWORD* itemsReturned) override
	{
		*itemsReturned = 0;
		if (index >= servers.size()) {
			return ERROR_NO_MORE_FILES;
		}
		xls::BufferPacker packer(buffer, bufferSize);
		DWORD count = 0;
		while (index < servers.size() && count < batchSize) {
			ServerEntry& server = servers[index];
			uint8_t* front = packer.FrontPointer();
			uint8_t* back = packer.BackPointer();
			XLOCATOR_SEARCHRESULT* out = (XLOCATOR_SEARCHRESULT*)packer.Front(sizeof(XLOCATOR_SEARCHRESULT));
			if (!out) {
				break;
			}
			*out = server.result;
			out->cProperties = 0;
			out->pProperties = nullptr;
			bool fits = true;
			if (!server.properties.empty()) {
				XUSER_PROPERTY* properties = (XUSER_PROPERTY*)packer.Back(server.properties.size() * sizeof(XUSER_PROPERTY));
				fits = properties != nullptr;
				for (size_t i = 0; fits && i < server.properties.size(); i++) {
					properties[i] = server.properties[i];
					const xls::StoredProperty& stored = server.values[i];
					if (stored.type == XUSER_DATA_TYPE_UNICODE || stored.type == XUSER_DATA_TYPE_BINARY) {
						uint8_t* data = (uint8_t*)packer.Back(stored.raw.size());
						if (!data) {
							fits = false;
							break;
						}
						memcpy(data, stored.raw.data(), stored.raw.size());
						if (stored.type == XUSER_DATA_TYPE_UNICODE) {
							properties[i].value.string.pwszData = (LPWSTR)data;
						}
						else {
							properties[i].value.binary.pbData = data;
						}
					}
				}
				if (fits) {
					out->cProperties = (DWORD)server.properties.size();
					out->pProperties = properties;
				}
			}
			if (!fits) {
				packer.Restore(front, back);
				break;
			}
			count++;
			index++;
		}
		*itemsReturned = count;
		return count ? ERROR_SUCCESS : ERROR_INSUFFICIENT_BUFFER;
	}
};

}

// #5236
HRESULT WINAPI XLocatorServiceInitialize(XLOCATOR_INIT_INFO* pInitInfo, HANDLE* phService)
{
	XLS_TRACE_FN();
	if (phService) {
		*phService = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	}
	return S_OK;
}

// #5237
HRESULT WINAPI XLocatorServiceUnInitialize(HANDLE hService)
{
	XLS_TRACE_FN();
	if (hService && hService != INVALID_HANDLE_VALUE) {
		CloseHandle(hService);
	}
	return S_OK;
}

// #5238
HRESULT WINAPI XLocatorCreateKey(XNKID* pxnkid, XNKEY* pxnkey)
{
	XLS_TRACE_FN();
	if (!pxnkid || !pxnkey) {
		return E_POINTER;
	}
	xls::NetCreateKey(pxnkid, pxnkey);
	pxnkid->ab[0] = (pxnkid->ab[0] & 0x1F) | XNET_XNKID_ONLINE_SERVER;
	return S_OK;
}

// #5230
HRESULT WINAPI XLocatorServerAdvertise(DWORD dwUserIndex, DWORD dwServerType, XNKID xnkid, XNKEY xnkey, DWORD dwMaxPublicSlots, DWORD dwMaxPrivateSlots, DWORD dwFilledPublicSlots, DWORD dwFilledPrivateSlots, DWORD cProperties, XUSER_PROPERTY* pProperties, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return HRESULT_FROM_WIN32(ERROR_NO_SUCH_USER);
	}
	if (!xls::SteamReady()) {
		return HRESULT_FROM_WIN32(ERROR_NOT_LOGGED_ON);
	}
	if (cProperties && !pProperties) {
		return E_INVALIDARG;
	}
	std::string xnkidHex = xls::HexEncode(xnkid.ab, sizeof(xnkid.ab));
	std::string xnkeyHex = xls::HexEncode(xnkey.ab, sizeof(xnkey.ab));
	std::vector<XUSER_PROPERTY> properties(pProperties, pProperties + cProperties);
	std::vector<xls::StoredProperty> owned(cProperties);
	for (DWORD i = 0; i < cProperties; i++) {
		const XUSER_DATA& value = pProperties[i].value;
		switch (value.type) {
			case XUSER_DATA_TYPE_UNICODE: owned[i].Set(value.type, value.string.pwszData, value.string.cbData); owned[i].Fill(properties[i].value); break;
			case XUSER_DATA_TYPE_BINARY: owned[i].Set(value.type, value.binary.pbData, value.binary.cbData); owned[i].Fill(properties[i].value); break;
			default: break;
		}
	}

	auto apply = [=](CSteamID lobby) {
		ISteamMatchmaking* matchmaking = xls::SteamMatchmaking();
		matchmaking->SetLobbyData(lobby, Key("locator").c_str(), "1");
		matchmaking->SetLobbyData(lobby, Key("title").c_str(), xls::FormatA("%u", xls::TitleId()).c_str());
		matchmaking->SetLobbyData(lobby, Key("type").c_str(), xls::FormatA("%u", dwServerType).c_str());
		matchmaking->SetLobbyData(lobby, Key("xnkid").c_str(), xnkidHex.c_str());
		matchmaking->SetLobbyData(lobby, Key("xnkey").c_str(), xnkeyHex.c_str());
		matchmaking->SetLobbyData(lobby, Key("pub").c_str(), xls::FormatA("%u", dwMaxPublicSlots).c_str());
		matchmaking->SetLobbyData(lobby, Key("priv").c_str(), xls::FormatA("%u", dwMaxPrivateSlots).c_str());
		matchmaking->SetLobbyData(lobby, Key("fpub").c_str(), xls::FormatA("%u", dwFilledPublicSlots).c_str());
		matchmaking->SetLobbyData(lobby, Key("fpriv").c_str(), xls::FormatA("%u", dwFilledPrivateSlots).c_str());
		std::vector<XUSER_PROPERTY> copy = properties;
		std::vector<xls::StoredProperty> values = owned;
		for (size_t i = 0; i < copy.size(); i++) {
			if (values[i].type != XUSER_DATA_TYPE_NULL) {
				values[i].Fill(copy[i].value);
			}
		}
		WriteProperties(lobby, (DWORD)copy.size(), copy.data());
	};

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_advertisedLobby.IsValid()) {
			apply(g_advertisedLobby);
			DWORD result = xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
			return result == ERROR_IO_PENDING ? HRESULT_FROM_WIN32(ERROR_IO_PENDING) : S_OK;
		}
		if (!g_pendingCreate) {
			g_pendingCreate = std::make_shared<xls::SteamCall<LobbyCreated_t>>();
			g_pendingCreate->Start(xls::SteamMatchmaking()->CreateLobby(k_ELobbyTypePublic, 250));
		}
	}
	auto pending = g_pendingCreate;
	DWORD result = xls::RunAsync(pOverlapped, [pending, apply](XOVERLAPPED* overlapped) {
		if (!pending->Poll()) {
			return false;
		}
		std::lock_guard<std::mutex> lock(g_mutex);
		if (pending->failed || pending->result.m_eResult != k_EResultOK) {
			g_pendingCreate.reset();
			xls::OverlappedComplete(overlapped, ERROR_FUNCTION_FAILED);
			return true;
		}
		g_advertisedLobby = CSteamID(pending->result.m_ulSteamIDLobby);
		g_pendingCreate.reset();
		apply(g_advertisedLobby);
		XLS_LOG_INFO("xlocator: advertising as lobby %llu.", g_advertisedLobby.ConvertToUint64());
		xls::OverlappedComplete(overlapped, ERROR_SUCCESS);
		return true;
	});
	return result == ERROR_IO_PENDING ? HRESULT_FROM_WIN32(ERROR_IO_PENDING) : (result == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(result));
}

// #5231
HRESULT WINAPI XLocatorServerUnAdvertise(DWORD dwUserIndex, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return HRESULT_FROM_WIN32(ERROR_NO_SUCH_USER);
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_advertisedLobby.IsValid() && xls::SteamReady()) {
		xls::SteamMatchmaking()->LeaveLobby(g_advertisedLobby);
	}
	g_advertisedLobby = k_steamIDNil;
	DWORD result = xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
	return result == ERROR_IO_PENDING ? HRESULT_FROM_WIN32(ERROR_IO_PENDING) : S_OK;
}

// #5233
HRESULT WINAPI XLocatorGetServiceProperty(DWORD dwUserIndex, DWORD cProperties, XUSER_PROPERTY* pProperties, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return HRESULT_FROM_WIN32(ERROR_NO_SUCH_USER);
	}
	if (cProperties && !pProperties) {
		return E_INVALIDARG;
	}
	// No service tracks these counts
	for (DWORD i = 0; i < cProperties; i++) {
		pProperties[i].value.type = XUSER_DATA_TYPE_INT32;
		pProperties[i].value.nData = 0;
	}
	DWORD result = xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
	return result == ERROR_IO_PENDING ? HRESULT_FROM_WIN32(ERROR_IO_PENDING) : S_OK;
}

namespace {

DWORD CreateLocatorEnumerator(DWORD count, const std::vector<uint64_t>& onlyServers, DWORD* bufferSize, HANDLE* handle)
{
	auto enumerator = std::make_unique<LocatorEnumerator>();
	enumerator->batchSize = count ? count : 1;
	enumerator->onlyServers = onlyServers;
	if (xls::SteamReady()) {
		ISteamMatchmaking* matchmaking = xls::SteamMatchmaking();
		matchmaking->AddRequestLobbyListStringFilter(Key("locator").c_str(), "1", k_ELobbyComparisonEqual);
		matchmaking->AddRequestLobbyListStringFilter(Key("title").c_str(), xls::FormatA("%u", xls::TitleId()).c_str(), k_ELobbyComparisonEqual);
		matchmaking->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
		matchmaking->AddRequestLobbyListResultCountFilter((int)(count > 50 ? count : 50));
		enumerator->request = std::make_shared<xls::SteamCall<LobbyMatchList_t>>();
		enumerator->request->Start(matchmaking->RequestLobbyList());
	}
	// Each result carries its properties so the per-item size includes room for them.
	*bufferSize = enumerator->batchSize * (sizeof(XLOCATOR_SEARCHRESULT) + 32 * (sizeof(XUSER_PROPERTY) + 64));
	enumerator->requiredBufferSize = *bufferSize;
	*handle = xls::EnumeratorRegister(std::move(enumerator));
	return *handle ? ERROR_SUCCESS : ERROR_FUNCTION_FAILED;
}

}

// #5234
DWORD WINAPI XLocatorCreateServerEnumerator(DWORD dwUserIndex, DWORD cItems, DWORD cRequiredPropertyIds, const DWORD* pRequiredPropertyIds, DWORD cFilterGroupItems, const XLOCATOR_FILTER_GROUP* pFilterGroups, DWORD cSorters, const XLOCATOR_SORTER* pSorters, DWORD* pcbBuffer, HANDLE* phEnum)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!cItems || !pcbBuffer || !phEnum) {
		return ERROR_INVALID_PARAMETER;
	}
	return CreateLocatorEnumerator(cItems, std::vector<uint64_t>(), pcbBuffer, phEnum);
}

// #5235
DWORD WINAPI XLocatorCreateServerEnumeratorByIDs(DWORD dwUserIndex, DWORD cItems, DWORD cRequiredPropertyIds, const DWORD* pRequiredPropertyIds, DWORD cIDs, const uint64_t* pIDs, DWORD* pcbBuffer, HANDLE* phEnum)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!cItems || !cIDs || !pIDs || !pcbBuffer || !phEnum) {
		return ERROR_INVALID_PARAMETER;
	}
	return CreateLocatorEnumerator(cItems, std::vector<uint64_t>(pIDs, pIDs + cIDs), pcbBuffer, phEnum);
}
