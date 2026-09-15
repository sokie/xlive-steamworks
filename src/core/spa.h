// The title's SPA (XDBF) resource, read from the exe: achievements, strings, images, stats views, presence.
#pragma once

#include <windows.h>
#include <stdint.h>
#include <map>
#include <string>
#include <vector>

namespace xls {
namespace spa {

struct Achievement {
	uint16_t id;
	uint16_t labelId;
	uint16_t descriptionId;
	uint16_t unachievedId;
	uint32_t imageId;
	uint16_t cred;
	uint32_t flags;
};

struct StatColumn {
	uint32_t propertyId;
	uint16_t columnId;
	uint8_t type;       // XUSER_DATA_TYPE_*
	uint8_t ordinal;
	bool isSystem;
	uint16_t attributeId;
	std::wstring name;
};

struct StatView {
	uint32_t viewId;
	std::wstring name;
	std::vector<StatColumn> columns;
};

enum class Comparison { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };
enum class Operand { Parameter, Constant, ContextValue };

struct QueryFilter {
	uint32_t attributeId;
	Comparison comparison;
	Operand operandType;
	uint32_t operand;
};

struct Query {
	uint32_t id;
	uint32_t maxResults;
	std::vector<QueryFilter> filters;
	std::vector<uint32_t> returns;
};

// Reads the SPAFILE resource of a module (the exe when null). Strings come from the table for
// language, with the first table as fallback.
bool Load(uint8_t language, HMODULE module = nullptr);
bool Loaded();

uint32_t TitleId();
const std::wstring& TitleName();
uint8_t DefaultLanguage();

const wchar_t* String(uint16_t stringId);

const std::vector<Achievement>& Achievements();
const Achievement* FindAchievement(uint32_t achievementId);

bool Image(uint32_t imageId, const uint8_t** data, size_t* size);

const std::map<uint32_t, StatView>& StatViews();
const StatView* FindStatView(uint32_t viewId);

const Query* FindQuery(uint32_t procedureIndex);
bool Constant(uint32_t constantId, double* value);

bool HasPresence();
bool PresenceStringId(uint32_t presenceValue, uint16_t* stringId);
bool ContextValueStringId(uint32_t contextId, uint32_t value, uint16_t* stringId);
bool ContextDefault(uint32_t contextId, uint32_t* value);

}
}
