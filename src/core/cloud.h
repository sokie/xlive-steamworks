// Per-user files: Steam Cloud when enabled for app and account, else a folder next to the exe.
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace xls {

struct CloudFileInfo {
	std::string name;
	int32_t size;
	int64_t timestamp; // Unix seconds.
};

bool CloudAvailable();
bool CloudExists(const std::string& name);
bool CloudRead(const std::string& name, std::vector<uint8_t>& data);
bool CloudWrite(const std::string& name, const void* data, size_t size);
bool CloudDelete(const std::string& name);
int64_t CloudTimestamp(const std::string& name);
// Every file whose name starts with prefix.
std::vector<CloudFileInfo> CloudList(const std::string& prefix);

}
