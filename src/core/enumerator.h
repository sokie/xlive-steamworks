// XEnumerate handles.
#pragma once

#include "xlive/xdefs.h"

#include <memory>
#include <vector>

namespace xls {

class Enumerator {
public:
	virtual ~Enumerator() {}

	// True when data can be read. An enumerator that still waits on Steam returns false and the
	// pump keeps asking.
	virtual bool Ready() { return true; }

	// Copies the next batch into the title's buffer. Returns ERROR_SUCCESS, ERROR_NO_MORE_FILES
	// when nothing is left, or another error.
	virtual DWORD Next(void* buffer, DWORD bufferSize, DWORD* itemsReturned) = 0;

	// Buffer size reported to the title at creation.
	DWORD requiredBufferSize = 0;
};

HANDLE EnumeratorRegister(std::unique_ptr<Enumerator> enumerator);
Enumerator* EnumeratorFind(HANDLE handle);
bool EnumeratorClose(HANDLE handle);
void EnumeratorShutdown();

// Fills a title buffer with fixed-size records from the front and their variable data from the back
class BufferPacker {
public:
	BufferPacker(void* buffer, DWORD size)
		: m_begin((uint8_t*)buffer), m_front((uint8_t*)buffer), m_back((uint8_t*)buffer + size) {}

	// Reserves a record at the front, null when it does not fit.
	void* Front(size_t size)
	{
		if (m_front + size > m_back) {
			return nullptr;
		}
		void* result = m_front;
		m_front += size;
		return result;
	}

	// Reserves variable data at the back, null when it does not fit.
	void* Back(size_t size)
	{
		if (m_back - size < m_front) {
			return nullptr;
		}
		m_back -= size;
		return m_back;
	}

	// Copies a wide string to the back. Returns null when it does not fit.
	wchar_t* BackString(const wchar_t* text)
	{
		size_t bytes = (wcslen(text) + 1) * sizeof(wchar_t);
		wchar_t* slot = (wchar_t*)Back(bytes);
		if (slot) {
			memcpy(slot, text, bytes);
		}
		return slot;
	}

	size_t Remaining() const { return (size_t)(m_back - m_front); }
	uint8_t* FrontPointer() const { return m_front; }
	uint8_t* BackPointer() const { return m_back; }
	void Restore(uint8_t* front, uint8_t* back) { m_front = front; m_back = back; }

private:
	uint8_t* m_begin;
	uint8_t* m_front;
	uint8_t* m_back;
};

}
