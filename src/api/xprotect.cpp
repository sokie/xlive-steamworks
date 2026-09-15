// #5016 - #5021, #5034 - #5039, #5294, #5295, #5347 - #5349: protected data passes through unchanged.
#include "xlive/xfuncs.h"

#include "core/log.h"
#include "core/users.h"

#include <mutex>
#include <set>

namespace {

std::mutex g_mutex;
std::set<XLIVE_PROTECTED_BUFFER*> g_buffers;
std::set<XLIVE_PROTECTED_DATA_INFORMATION*> g_contexts;

bool ValidBuffer(XLIVE_PROTECTED_BUFFER* buffer)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return buffer && g_buffers.count(buffer) > 0;
}

}

// #5016
HRESULT WINAPI XLivePBufferAllocate(DWORD dwSize, XLIVE_PROTECTED_BUFFER** ppBuffer)
{
	XLS_TRACE_FN();
	if (!ppBuffer) {
		return E_POINTER;
	}
	*ppBuffer = nullptr;
	if (!dwSize) {
		return E_INVALIDARG;
	}
	XLIVE_PROTECTED_BUFFER* buffer = (XLIVE_PROTECTED_BUFFER*)calloc(1, sizeof(DWORD) + dwSize);
	if (!buffer) {
		return E_OUTOFMEMORY;
	}
	buffer->dwSize = dwSize;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_buffers.insert(buffer);
	}
	*ppBuffer = buffer;
	return S_OK;
}

// #5017
HRESULT WINAPI XLivePBufferFree(XLIVE_PROTECTED_BUFFER* pBuffer)
{
	XLS_TRACE_FN();
	if (!pBuffer) {
		return E_POINTER;
	}
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_buffers.erase(pBuffer)) {
			return E_HANDLE;
		}
	}
	free(pBuffer);
	return S_OK;
}

// #5018
HRESULT WINAPI XLivePBufferGetByte(XLIVE_PROTECTED_BUFFER* pBuffer, DWORD dwOffset, uint8_t* pbValue)
{
	if (!ValidBuffer(pBuffer) || !pbValue) {
		return E_POINTER;
	}
	if (dwOffset >= pBuffer->dwSize) {
		return E_INVALIDARG;
	}
	*pbValue = (&pBuffer->bData)[dwOffset];
	return S_OK;
}

// #5019
HRESULT WINAPI XLivePBufferSetByte(XLIVE_PROTECTED_BUFFER* pBuffer, DWORD dwOffset, uint8_t bValue)
{
	if (!ValidBuffer(pBuffer)) {
		return E_POINTER;
	}
	if (dwOffset >= pBuffer->dwSize) {
		return E_INVALIDARG;
	}
	(&pBuffer->bData)[dwOffset] = bValue;
	return S_OK;
}

// #5020
HRESULT WINAPI XLivePBufferGetDWORD(XLIVE_PROTECTED_BUFFER* pBuffer, DWORD dwOffset, DWORD* pdwValue)
{
	if (!ValidBuffer(pBuffer) || !pdwValue) {
		return E_POINTER;
	}
	if (dwOffset + sizeof(DWORD) > pBuffer->dwSize) {
		return E_INVALIDARG;
	}
	memcpy(pdwValue, &pBuffer->bData + dwOffset, sizeof(DWORD));
	return S_OK;
}

// #5021
HRESULT WINAPI XLivePBufferSetDWORD(XLIVE_PROTECTED_BUFFER* pBuffer, DWORD dwOffset, DWORD dwValue)
{
	if (!ValidBuffer(pBuffer)) {
		return E_POINTER;
	}
	if (dwOffset + sizeof(DWORD) > pBuffer->dwSize) {
		return E_INVALIDARG;
	}
	memcpy(&pBuffer->bData + dwOffset, &dwValue, sizeof(DWORD));
	return S_OK;
}

// #5294
HRESULT WINAPI XLivePBufferGetByteArray(XLIVE_PROTECTED_BUFFER* pBuffer, DWORD dwOffset, uint8_t* pbData, DWORD cbData)
{
	if (!ValidBuffer(pBuffer) || !pbData) {
		return E_POINTER;
	}
	if (dwOffset + cbData > pBuffer->dwSize) {
		return E_INVALIDARG;
	}
	memcpy(pbData, &pBuffer->bData + dwOffset, cbData);
	return S_OK;
}

// #5295
HRESULT WINAPI XLivePBufferSetByteArray(XLIVE_PROTECTED_BUFFER* pBuffer, DWORD dwOffset, const uint8_t* pbData, DWORD cbData)
{
	if (!ValidBuffer(pBuffer) || !pbData) {
		return E_POINTER;
	}
	if (dwOffset + cbData > pBuffer->dwSize) {
		return E_INVALIDARG;
	}
	memcpy(&pBuffer->bData + dwOffset, pbData, cbData);
	return S_OK;
}

// #5034
HRESULT WINAPI XLiveProtectData(const uint8_t* pInBuffer, DWORD dwInDataSize, uint8_t* pOutBuffer, DWORD* pdwOutDataSize, HANDLE hProtectedData)
{
	XLS_TRACE_FN();
	if (!pInBuffer || !pdwOutDataSize || !hProtectedData || hProtectedData == INVALID_HANDLE_VALUE) {
		return E_POINTER;
	}
	if (!dwInDataSize) {
		return E_INVALIDARG;
	}
	if (!pOutBuffer || *pdwOutDataSize < dwInDataSize) {
		*pdwOutDataSize = dwInDataSize;
		return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
	}
	memcpy(pOutBuffer, pInBuffer, dwInDataSize);
	*pdwOutDataSize = dwInDataSize;
	return S_OK;
}

// #5035
HRESULT WINAPI XLiveUnprotectData(const uint8_t* pInBuffer, DWORD dwInDataSize, uint8_t* pOutBuffer, DWORD* pdwOutDataSize, HANDLE* phProtectedData)
{
	XLS_TRACE_FN();
	if (!phProtectedData) {
		return E_POINTER;
	}
	*phProtectedData = nullptr;
	if (!pInBuffer || !pdwOutDataSize) {
		return E_POINTER;
	}
	if (!dwInDataSize) {
		return E_INVALIDARG;
	}
	if (!pOutBuffer || *pdwOutDataSize < dwInDataSize) {
		*pdwOutDataSize = dwInDataSize;
		return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
	}
	XLIVE_PROTECTED_DATA_INFORMATION* context = new XLIVE_PROTECTED_DATA_INFORMATION();
	context->cbSize = sizeof(*context);
	context->dwFlags = xls::UserOnline(0) ? 0 : XLIVE_PROTECTED_DATA_FLAG_OFFLINE_ONLY;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_contexts.insert(context);
	}
	*phProtectedData = context;
	memcpy(pOutBuffer, pInBuffer, dwInDataSize);
	*pdwOutDataSize = dwInDataSize;
	return S_OK;
}

// #5036
HRESULT WINAPI XLiveCreateProtectedDataContext(const XLIVE_PROTECTED_DATA_INFORMATION* pInfo, HANDLE* phProtectedData)
{
	XLS_TRACE_FN();
	if (!phProtectedData) {
		return E_POINTER;
	}
	*phProtectedData = nullptr;
	if (!pInfo || pInfo->cbSize != sizeof(XLIVE_PROTECTED_DATA_INFORMATION)) {
		return E_INVALIDARG;
	}
	XLIVE_PROTECTED_DATA_INFORMATION* context = new XLIVE_PROTECTED_DATA_INFORMATION(*pInfo);
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_contexts.insert(context);
	}
	*phProtectedData = context;
	return S_OK;
}

// #5037
HRESULT WINAPI XLiveQueryProtectedDataInformation(HANDLE hProtectedData, XLIVE_PROTECTED_DATA_INFORMATION* pInfo)
{
	XLS_TRACE_FN();
	if (!hProtectedData || hProtectedData == INVALID_HANDLE_VALUE || !pInfo) {
		return E_POINTER;
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	XLIVE_PROTECTED_DATA_INFORMATION* context = (XLIVE_PROTECTED_DATA_INFORMATION*)hProtectedData;
	if (!g_contexts.count(context)) {
		return E_HANDLE;
	}
	pInfo->dwFlags = context->dwFlags;
	pInfo->cbSize = sizeof(*pInfo);
	return S_OK;
}

// #5038
HRESULT WINAPI XLiveCloseProtectedDataContext(HANDLE hProtectedData)
{
	XLS_TRACE_FN();
	if (!hProtectedData || hProtectedData == INVALID_HANDLE_VALUE) {
		return E_POINTER;
	}
	XLIVE_PROTECTED_DATA_INFORMATION* context = (XLIVE_PROTECTED_DATA_INFORMATION*)hProtectedData;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_contexts.erase(context)) {
			return E_HANDLE;
		}
	}
	delete context;
	return S_OK;
}

// #5039
HRESULT WINAPI XLiveVerifyDataFile(LPCWSTR lpszFileName)
{
	XLS_TRACE_FN();
	if (!lpszFileName) {
		return E_POINTER;
	}
	return GetFileAttributesW(lpszFileName) == INVALID_FILE_ATTRIBUTES ? HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) : S_OK;
}

// #5347
HRESULT WINAPI XLiveProtectedLoadLibrary(HANDLE hContentAccess, void* pvReserved, LPCWSTR lpszModuleFileName, DWORD dwFlags, HMODULE* phModule)
{
	XLS_TRACE_FN();
	if (!lpszModuleFileName || !phModule) {
		return E_POINTER;
	}
	*phModule = LoadLibraryExW(lpszModuleFileName, nullptr, dwFlags);
	if (!*phModule) {
		return HRESULT_FROM_WIN32(GetLastError());
	}
	return S_OK;
}

// #5348
HRESULT WINAPI XLiveProtectedCreateFile(HANDLE hContentAccess, void* pvReserved, LPCWSTR lpszFileName, DWORD dwDesiredAccess, DWORD dwShareMode, SECURITY_ATTRIBUTES* lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE* phFile)
{
	XLS_TRACE_FN();
	if (!lpszFileName || !phFile) {
		return E_POINTER;
	}
	*phFile = CreateFileW(lpszFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, nullptr);
	if (*phFile == INVALID_HANDLE_VALUE) {
		return HRESULT_FROM_WIN32(GetLastError());
	}
	return S_OK;
}

// #5349
HRESULT WINAPI XLiveProtectedVerifyFile(HANDLE hContentAccess, void* pvReserved, LPCWSTR lpszFileName)
{
	XLS_TRACE_FN();
	if (!lpszFileName) {
		return E_POINTER;
	}
	return GetFileAttributesW(lpszFileName) == INVALID_FILE_ATTRIBUTES ? HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) : S_OK;
}
