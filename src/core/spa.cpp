#include "core/spa.h"

#include "core/log.h"
#include "core/utils.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

extern "C" {
#include "third-party/puff.h"
}

namespace xls {
namespace spa {

namespace {

#pragma pack(push, 1)
struct XdbfHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t entryTableLength;
	uint32_t entryCount;
	uint32_t freeSpaceTableLength;
	uint32_t freeSpaceCount;
};

struct XdbfEntry {
	uint16_t ns;
	uint64_t id;
	uint32_t offset;
	uint32_t length;
};

struct XdbfFreeSpaceEntry {
	uint32_t offset;
	uint32_t length;
};

struct XachHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t size;
	uint16_t count;
};

struct XachEntry {
	uint16_t id;
	uint16_t labelId;
	uint16_t descriptionId;
	uint16_t unachievedId;
	uint32_t imageId;
	uint16_t cred;
	uint8_t pad0[2];
	uint32_t flags;
	uint8_t pad1[16];
};

struct XstrHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t size;
	uint16_t count;
};

struct XstrEntry {
	uint16_t id;
	uint16_t length;
};

struct Xvc2Header {
	uint32_t magic;
	uint32_t version;
	uint32_t size;
};

struct Xvc2Field {
	uint32_t size;
	uint32_t propertyId;
	uint32_t flags;
	uint16_t attributeId;
	uint16_t stringId;
	uint16_t aggregationType;
	uint8_t ordinal;
	uint8_t fieldType;
	uint32_t formatType;
	uint8_t pad[8];
};

struct Xvc2View {
	uint32_t id;
	uint32_t flags;
	uint16_t sharedIndex;
	uint16_t stringId;
	uint8_t pad[4];
};

struct Xpbm {
	uint32_t magic;
	uint32_t version;
	uint32_t size;
	uint32_t contextCount;
	uint32_t propertyCount;
};
#pragma pack(pop)

const uint32_t kMagicXdbf = 0x58444246;
const uint32_t kMagicXach = 0x58414348;
const uint32_t kMagicXstr = 0x58535452;
const uint32_t kMagicXvc2 = 0x58564332;
const uint32_t kMagicXsrc = 0x58535243;
const uint32_t kMagicXthd = 0x58544844;
const uint32_t kMagicXstc = 0x58535443;

inline uint16_t Be16(uint16_t v) { return _byteswap_ushort(v); }
inline uint32_t Be32(uint32_t v) { return _byteswap_ulong(v); }
inline uint64_t Be64(uint64_t v) { return _byteswap_uint64(v); }
inline uint32_t Be32At(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

struct ImageBlob {
	const uint8_t* data;
	size_t size;
};

bool g_loaded = false;
uint32_t g_titleId = 0;
uint8_t g_defaultLanguage = 1;
std::wstring g_titleName;
std::map<uint16_t, std::wstring> g_strings;
std::vector<Achievement> g_achievements;
std::map<uint32_t, ImageBlob> g_images;
std::map<uint32_t, StatView> g_views;
std::map<uint32_t, Query> g_queries;
std::map<uint32_t, double> g_constants;
std::map<uint32_t, uint16_t> g_presenceModes;
std::map<uint32_t, std::map<uint32_t, uint16_t>> g_contextValues;
std::map<uint32_t, uint32_t> g_contextDefaults;
std::map<uint32_t, std::wstring> g_viewNames;
std::map<uint32_t, std::wstring> g_propertyNames;

// --- XLAST (XSRC) ------------------------------------------------------------------------------

struct XmlTag {
	std::string name;
	std::map<std::string, std::string> attributes;
	bool closing = false;
	bool selfClosing = false;
};

// Steps through the tags of a document. Text between tags is skipped: the XLAST keeps every value
// in attributes.
class XmlScanner {
public:
	explicit XmlScanner(const std::string& text) : m_text(text) {}

	bool Next(XmlTag& tag)
	{
		while (true) {
			size_t open = m_text.find('<', m_pos);
			if (open == std::string::npos) {
				return false;
			}
			if (m_text.compare(open, 4, "<!--") == 0) {
				size_t end = m_text.find("-->", open + 4);
				m_pos = end == std::string::npos ? m_text.size() : end + 3;
				continue;
			}
			if (m_text.compare(open, 2, "<?") == 0 || m_text.compare(open, 2, "<!") == 0) {
				size_t end = m_text.find('>', open);
				m_pos = end == std::string::npos ? m_text.size() : end + 1;
				continue;
			}
			size_t close = FindTagEnd(open);
			if (close == std::string::npos) {
				return false;
			}
			ParseTag(m_text.substr(open + 1, close - open - 1), tag);
			m_pos = close + 1;
			return true;
		}
	}

private:
	size_t FindTagEnd(size_t open) const
	{
		char quote = 0;
		for (size_t i = open + 1; i < m_text.size(); i++) {
			char c = m_text[i];
			if (quote) {
				if (c == quote) quote = 0;
			}
			else if (c == '"' || c == '\'') {
				quote = c;
			}
			else if (c == '>') {
				return i;
			}
		}
		return std::string::npos;
	}

	static std::string Unescape(const std::string& value)
	{
		if (value.find('&') == std::string::npos) {
			return value;
		}
		std::string out;
		for (size_t i = 0; i < value.size(); i++) {
			if (value[i] != '&') {
				out.push_back(value[i]);
				continue;
			}
			size_t semi = value.find(';', i);
			if (semi == std::string::npos) {
				out.push_back(value[i]);
				continue;
			}
			std::string entity = value.substr(i + 1, semi - i - 1);
			if (entity == "amp") out.push_back('&');
			else if (entity == "lt") out.push_back('<');
			else if (entity == "gt") out.push_back('>');
			else if (entity == "quot") out.push_back('"');
			else if (entity == "apos") out.push_back('\'');
			else if (!entity.empty() && entity[0] == '#') {
				uint32_t codepoint = entity[1] == 'x' ? (uint32_t)strtoul(entity.c_str() + 2, nullptr, 16) : (uint32_t)strtoul(entity.c_str() + 1, nullptr, 10);
				wchar_t wide[3] = {};
				if (codepoint < 0x10000) {
					wide[0] = (wchar_t)codepoint;
				}
				else {
					codepoint -= 0x10000;
					wide[0] = (wchar_t)(0xD800 + (codepoint >> 10));
					wide[1] = (wchar_t)(0xDC00 + (codepoint & 0x3FF));
				}
				out += WideToUtf8(wide);
			}
			else {
				out.push_back('&');
				continue;
			}
			i = semi;
		}
		return out;
	}

	static void ParseTag(const std::string& body, XmlTag& tag)
	{
		tag = XmlTag();
		size_t pos = 0;
		if (!body.empty() && body[0] == '/') {
			tag.closing = true;
			pos = 1;
		}
		while (pos < body.size() && isspace((unsigned char)body[pos])) pos++;
		size_t nameStart = pos;
		while (pos < body.size() && !isspace((unsigned char)body[pos]) && body[pos] != '/' && body[pos] != '>') pos++;
		tag.name = body.substr(nameStart, pos - nameStart);
		while (pos < body.size()) {
			while (pos < body.size() && isspace((unsigned char)body[pos])) pos++;
			if (pos >= body.size()) break;
			if (body[pos] == '/') {
				tag.selfClosing = true;
				break;
			}
			size_t keyStart = pos;
			while (pos < body.size() && body[pos] != '=' && !isspace((unsigned char)body[pos])) pos++;
			std::string key = body.substr(keyStart, pos - keyStart);
			while (pos < body.size() && isspace((unsigned char)body[pos])) pos++;
			if (pos >= body.size() || body[pos] != '=') {
				tag.attributes[key] = std::string();
				continue;
			}
			pos++;
			while (pos < body.size() && isspace((unsigned char)body[pos])) pos++;
			if (pos >= body.size()) break;
			char quote = body[pos];
			if (quote != '"' && quote != '\'') {
				size_t valueStart = pos;
				while (pos < body.size() && !isspace((unsigned char)body[pos])) pos++;
				tag.attributes[key] = Unescape(body.substr(valueStart, pos - valueStart));
				continue;
			}
			pos++;
			size_t valueStart = pos;
			while (pos < body.size() && body[pos] != quote) pos++;
			tag.attributes[key] = Unescape(body.substr(valueStart, pos - valueStart));
			pos++;
		}
	}

	const std::string& m_text;
	size_t m_pos = 0;
};

uint32_t AttrNumber(const XmlTag& tag, const char* name, uint32_t fallback = 0)
{
	auto it = tag.attributes.find(name);
	if (it == tag.attributes.end()) {
		return fallback;
	}
	return (uint32_t)strtoul(it->second.c_str(), nullptr, 0);
}

const std::string* Attr(const XmlTag& tag, const char* name)
{
	auto it = tag.attributes.find(name);
	return it == tag.attributes.end() ? nullptr : &it->second;
}

Comparison ComparisonFromText(const std::string* text)
{
	if (text) {
		if (*text == "!=") return Comparison::NotEqual;
		if (*text == "<") return Comparison::Less;
		if (*text == "<=") return Comparison::LessEqual;
		if (*text == ">") return Comparison::Greater;
		if (*text == ">=") return Comparison::GreaterEqual;
	}
	return Comparison::Equal;
}

Operand OperandFromText(const std::string* text)
{
	if (text) {
		if (*text == "Constant") return Operand::Constant;
		if (*text == "ContextValue") return Operand::ContextValue;
	}
	return Operand::Parameter;
}

void ReadXlast(const std::string& xml)
{
	XmlScanner scanner(xml);
	XmlTag tag;
	uint32_t currentContext = 0;
	bool inContext = false;
	Query* currentQuery = nullptr;
	std::vector<std::pair<uint32_t, uint32_t>> returns;
	while (scanner.Next(tag)) {
		if (tag.closing) {
			if (tag.name == "Context") {
				inContext = false;
			}
			else if (tag.name == "Query" && currentQuery) {
				std::sort(returns.begin(), returns.end());
				for (const auto& entry : returns) {
					currentQuery->returns.push_back(entry.second);
				}
				returns.clear();
				currentQuery = nullptr;
			}
			continue;
		}
		if (tag.name == "StatsView" || tag.name == "Property") {
			const std::string* friendly = Attr(tag, "friendlyName");
			if (friendly && !friendly->empty()) {
				std::map<uint32_t, std::wstring>& target = tag.name == "StatsView" ? g_viewNames : g_propertyNames;
				uint32_t id = AttrNumber(tag, "id");
				if (!target.count(id)) {
					target[id] = Utf8ToWide(friendly->c_str());
				}
			}
		}
		else if (tag.name == "Constant") {
			const std::string* value = Attr(tag, "value");
			if (value) {
				g_constants[AttrNumber(tag, "id")] = strtod(value->c_str(), nullptr);
			}
		}
		else if (tag.name == "PresenceMode") {
			uint32_t value = AttrNumber(tag, "contextValue");
			if (!g_presenceModes.count(value)) {
				g_presenceModes[value] = (uint16_t)AttrNumber(tag, "stringId");
			}
		}
		else if (tag.name == "Context") {
			currentContext = AttrNumber(tag, "id");
			inContext = !tag.selfClosing;
			const std::string* fallback = Attr(tag, "defaultValue");
			if (fallback && !g_contextDefaults.count(currentContext)) {
				g_contextDefaults[currentContext] = (uint32_t)strtoul(fallback->c_str(), nullptr, 0);
			}
			g_contextValues[currentContext];
		}
		else if (tag.name == "ContextValue" && inContext) {
			std::map<uint32_t, uint16_t>& values = g_contextValues[currentContext];
			uint32_t value = AttrNumber(tag, "value");
			if (!values.count(value)) {
				values[value] = (uint16_t)AttrNumber(tag, "stringId");
			}
		}
		else if (tag.name == "Query") {
			uint32_t id = AttrNumber(tag, "id");
			currentQuery = &g_queries[id];
			currentQuery->id = id;
			currentQuery->maxResults = AttrNumber(tag, "maxResults");
			currentQuery->filters.clear();
			currentQuery->returns.clear();
			if (tag.selfClosing) {
				currentQuery = nullptr;
			}
		}
		else if (tag.name == "Filter" && currentQuery) {
			const std::string* left = Attr(tag, "left");
			const std::string* right = Attr(tag, "right");
			const std::string* leftType = Attr(tag, "leftType");
			if (!left || !right || (leftType && *leftType != "Attribute")) {
				continue;
			}
			QueryFilter filter;
			filter.attributeId = (uint32_t)strtoul(left->c_str(), nullptr, 0);
			filter.comparison = ComparisonFromText(Attr(tag, "op"));
			filter.operandType = OperandFromText(Attr(tag, "rightType"));
			filter.operand = (uint32_t)strtoul(right->c_str(), nullptr, 0);
			currentQuery->filters.push_back(filter);
		}
		else if (tag.name == "Return" && currentQuery) {
			const std::string* id = Attr(tag, "id");
			if (id) {
				uint32_t ordinal = AttrNumber(tag, "ordinal", (uint32_t)returns.size());
				returns.emplace_back(ordinal, (uint32_t)strtoul(id->c_str(), nullptr, 0));
			}
		}
	}
}

// XSRC: magic, version, size, filenameLen, filename[], uncompressedSize, compressedSize, gzip[].
bool ReadXsrc(const uint8_t* xsrc, size_t length)
{
	if (length < 24 || Be32At(xsrc) != kMagicXsrc) {
		return false;
	}
	uint32_t filenameLength = Be32At(xsrc + 12);
	if (filenameLength > 1024 || (size_t)16 + filenameLength + 8 > length) {
		return false;
	}
	const uint8_t* cursor = xsrc + 16 + filenameLength;
	uint32_t uncompressedSize = Be32At(cursor);
	uint32_t compressedSize = Be32At(cursor + 4);
	cursor += 8;
	if (!uncompressedSize || uncompressedSize > 0x4000000 || compressedSize < 18 || compressedSize > length - (size_t)(cursor - xsrc)) {
		return false;
	}
	if (cursor[0] != 0x1f || cursor[1] != 0x8b || cursor[2] != 0x08) {
		return false;
	}
	uint8_t flags = cursor[3];
	size_t header = 10;
	if (flags & 0x04) {
		if (header + 2 > compressedSize) return false;
		header += 2 + ((uint32_t)cursor[header] | ((uint32_t)cursor[header + 1] << 8));
	}
	if (flags & 0x08) {
		while (header < compressedSize && cursor[header]) header++;
		header++;
	}
	if (flags & 0x10) {
		while (header < compressedSize && cursor[header]) header++;
		header++;
	}
	if (flags & 0x02) {
		header += 2;
	}
	if (header + 8 > compressedSize) {
		return false;
	}

	std::vector<uint8_t> inflated(uncompressedSize);
	unsigned long destLength = uncompressedSize;
	unsigned long sourceLength = compressedSize - header;
	int rc = puff(inflated.data(), &destLength, cursor + header, &sourceLength);
	if (rc != 0) {
		XLS_LOG_WARN("spa: XSRC inflate failed (%d).", rc);
		return false;
	}

	// The XLAST is UTF-16 with a byte order mark.
	std::string utf8;
	if (destLength >= 2 && inflated[0] == 0xFF && inflated[1] == 0xFE) {
		utf8 = WideToUtf8((const wchar_t*)(inflated.data() + 2), (int)((destLength - 2) / sizeof(wchar_t)));
	}
	else if (destLength >= 3 && inflated[0] == 0xEF && inflated[1] == 0xBB && inflated[2] == 0xBF) {
		utf8.assign((const char*)inflated.data() + 3, destLength - 3);
	}
	else if (destLength >= 2 && inflated[1] == 0) {
		utf8 = WideToUtf8((const wchar_t*)inflated.data(), (int)(destLength / sizeof(wchar_t)));
	}
	else {
		utf8.assign((const char*)inflated.data(), destLength);
	}
	ReadXlast(utf8);
	return true;
}

// XVC2: shared views (each: column fields, row fields, an XPBM block) then the view table.
void ReadXvc2(const uint8_t* xvc2, const uint8_t* end)
{
	if ((size_t)(end - xvc2) < sizeof(Xvc2Header) || Be32(((const Xvc2Header*)xvc2)->magic) != kMagicXvc2) {
		return;
	}
	const uint8_t* cursor = xvc2 + sizeof(Xvc2Header);
	auto take = [&cursor, end](size_t bytes) -> const uint8_t* {
		if ((size_t)(end - cursor) < bytes) {
			return nullptr;
		}
		const uint8_t* at = cursor;
		cursor += bytes;
		return at;
	};

	const uint8_t* at = take(2);
	if (!at) {
		return;
	}
	uint16_t sharedCount = Be16(*(const uint16_t*)at);
	std::vector<std::vector<StatColumn>> shared;
	for (uint16_t i = 0; i < sharedCount; i++) {
		at = take(2 + 2 + 8);
		if (!at) return;
		uint16_t columnCount = Be16(*(const uint16_t*)at);
		uint16_t rowCount = Be16(*(const uint16_t*)(at + 2));
		std::vector<StatColumn> columns;
		for (uint16_t c = 0; c < columnCount; c++) {
			at = take(sizeof(Xvc2Field));
			if (!at) return;
			const Xvc2Field* field = (const Xvc2Field*)at;
			StatColumn column;
			column.propertyId = Be32(field->propertyId);
			column.columnId = (uint16_t)(column.propertyId & 0x7FFF);
			column.type = (uint8_t)((column.propertyId >> 28) & 0xF);
			column.isSystem = (column.propertyId & 0x8000) != 0;
			column.attributeId = Be16(field->attributeId);
			column.ordinal = field->ordinal;
			auto friendly = g_propertyNames.find(column.propertyId);
			if (friendly != g_propertyNames.end() && !friendly->second.empty()) {
				column.name = friendly->second;
			}
			else {
				column.name = String(Be16(field->stringId));
			}
			columns.push_back(column);
		}
		if (!take((size_t)rowCount * sizeof(Xvc2Field))) return;
		at = take(sizeof(Xpbm));
		if (!at) return;
		const Xpbm* xpbm = (const Xpbm*)at;
		uint64_t xpbmBytes = ((uint64_t)Be32(xpbm->contextCount) + Be32(xpbm->propertyCount)) * 4;
		if (xpbmBytes > (uint64_t)(end - cursor)) return;
		cursor += (size_t)xpbmBytes;
		std::sort(columns.begin(), columns.end(), [](const StatColumn& a, const StatColumn& b) { return a.ordinal < b.ordinal; });
		shared.push_back(std::move(columns));
	}
	at = take(2);
	if (!at) return;
	uint16_t viewCount = Be16(*(const uint16_t*)at);
	for (uint16_t i = 0; i < viewCount; i++) {
		at = take(sizeof(Xvc2View));
		if (!at) return;
		const Xvc2View* entry = (const Xvc2View*)at;
		uint16_t sharedIndex = Be16(entry->sharedIndex);
		if (sharedIndex >= shared.size()) {
			continue;
		}
		StatView view;
		view.viewId = Be32(entry->id);
		auto friendly = g_viewNames.find(view.viewId);
		view.name = friendly != g_viewNames.end() && !friendly->second.empty() ? friendly->second : String(Be16(entry->stringId));
		view.columns = shared[sharedIndex];
		g_views[view.viewId] = std::move(view);
	}
}

}

bool Load(uint8_t language, HMODULE module)
{
	g_loaded = false;
	HRSRC resource = FindResourceW(module, L"SPAFILE", L"RT_RCDATA");
	if (!resource) {
		XLS_LOG_WARN("spa: the module has no SPAFILE resource.");
		return false;
	}
	HGLOBAL loaded = LoadResource(module, resource);
	const uint8_t* data = loaded ? (const uint8_t*)LockResource(loaded) : nullptr;
	DWORD size = SizeofResource(module, resource);
	if (!data || size < sizeof(XdbfHeader)) {
		XLS_LOG_ERROR("spa: SPAFILE resource could not be read.");
		return false;
	}
	const uint8_t* end = data + size;
	const XdbfHeader* header = (const XdbfHeader*)data;
	if (Be32(header->magic) != kMagicXdbf) {
		XLS_LOG_ERROR("spa: SPAFILE is not an XDBF file.");
		return false;
	}
	uint32_t entryCount = Be32(header->entryCount);
	const XdbfEntry* entries = (const XdbfEntry*)(data + sizeof(XdbfHeader));
	const uint8_t* body = data + sizeof(XdbfHeader) + (size_t)Be32(header->entryTableLength) * sizeof(XdbfEntry) + (size_t)Be32(header->freeSpaceTableLength) * sizeof(XdbfFreeSpaceEntry);
	if ((const uint8_t*)&entries[entryCount] > end || body > end) {
		XLS_LOG_ERROR("spa: SPAFILE entry table runs past the resource.");
		return false;
	}

	auto section = [&](const XdbfEntry& entry, const uint8_t** out, size_t* outLength) -> bool {
		const uint8_t* start = body + Be32(entry.offset);
		size_t length = Be32(entry.length);
		if (start < data || start > end || length > (size_t)(end - start)) {
			return false;
		}
		*out = start;
		*outLength = length;
		return true;
	};

	const uint8_t* xach = nullptr;
	const uint8_t* xstr = nullptr;
	const uint8_t* xstrFallback = nullptr;
	const uint8_t* xvc2 = nullptr;
	const uint8_t* xvc2End = nullptr;
	const uint8_t* xsrc = nullptr;
	size_t xsrcLength = 0;
	g_images.clear();

	for (uint32_t i = 0; i < entryCount; i++) {
		const XdbfEntry& entry = entries[i];
		uint16_t ns = Be16(entry.ns);
		uint64_t id = Be64(entry.id);
		const uint8_t* start = nullptr;
		size_t length = 0;
		if (!section(entry, &start, &length)) {
			XLS_LOG_WARN("spa: entry ns %u id 0x%llx lies outside the resource, skipped.", ns, id);
			continue;
		}
		if (ns == 1) {
			if (id == kMagicXach && !xach) xach = start;
			else if (id == kMagicXvc2 && !xvc2) { xvc2 = start; xvc2End = start + length; }
			else if (id == kMagicXsrc && !xsrc) { xsrc = start; xsrcLength = length; }
			else if (id == kMagicXthd && length >= 16 && Be32At(start) == kMagicXthd) g_titleId = Be32At(start + 12);
			else if (id == kMagicXstc && length >= 16 && Be32At(start) == kMagicXstc) g_defaultLanguage = (uint8_t)Be32At(start + 12);
		}
		else if (ns == 2) {
			g_images[(uint32_t)id] = ImageBlob{ start, length };
		}
		else if (ns == 3) {
			if (!xstrFallback) xstrFallback = start;
			if (id == language && !xstr) xstr = start;
		}
	}

	if (!xstr) {
		xstr = xstrFallback;
	}
	g_strings.clear();
	if (xstr && (size_t)(end - xstr) >= sizeof(XstrHeader) && Be32(((const XstrHeader*)xstr)->magic) == kMagicXstr) {
		uint16_t count = Be16(((const XstrHeader*)xstr)->count);
		const uint8_t* cursor = xstr + sizeof(XstrHeader);
		for (uint16_t i = 0; i < count && (size_t)(end - cursor) >= sizeof(XstrEntry); i++) {
			const XstrEntry* entry = (const XstrEntry*)cursor;
			uint16_t id = Be16(entry->id);
			uint16_t length = Be16(entry->length);
			cursor += sizeof(XstrEntry);
			if ((size_t)(end - cursor) < length) {
				break;
			}
			g_strings[id] = Utf8ToWide((const char*)cursor, length);
			cursor += length;
		}
	}
	else {
		XLS_LOG_WARN("spa: no string table found.");
	}

	g_achievements.clear();
	if (xach && (size_t)(end - xach) >= sizeof(XachHeader) && Be32(((const XachHeader*)xach)->magic) == kMagicXach) {
		uint16_t count = Be16(((const XachHeader*)xach)->count);
		const XachEntry* items = (const XachEntry*)(xach + sizeof(XachHeader));
		for (uint16_t i = 0; i < count && (const uint8_t*)&items[i + 1] <= end; i++) {
			Achievement achievement;
			achievement.id = Be16(items[i].id);
			achievement.labelId = Be16(items[i].labelId);
			achievement.descriptionId = Be16(items[i].descriptionId);
			achievement.unachievedId = Be16(items[i].unachievedId);
			achievement.imageId = Be32(items[i].imageId);
			achievement.cred = Be16(items[i].cred);
			achievement.flags = Be32(items[i].flags);
			g_achievements.push_back(achievement);
		}
	}
	else {
		XLS_LOG_WARN("spa: no XACH achievement table found.");
	}

	g_queries.clear();
	g_constants.clear();
	g_presenceModes.clear();
	g_contextValues.clear();
	g_contextDefaults.clear();
	g_viewNames.clear();
	g_propertyNames.clear();
	if (xsrc) {
		ReadXsrc(xsrc, xsrcLength);
	}

	g_views.clear();
	if (xvc2) {
		ReadXvc2(xvc2, xvc2End);
	}

	auto title = g_strings.find(0x8000);
	g_titleName = title != g_strings.end() ? title->second : std::wstring();

	g_loaded = true;
	XLS_LOG_INFO("spa: title 0x%08x \"%ls\": %zu achievements, %zu strings, %zu images, %zu stats views, %zu queries, %zu presence modes.",
		g_titleId, g_titleName.c_str(), g_achievements.size(), g_strings.size(), g_images.size(), g_views.size(), g_queries.size(), g_presenceModes.size());
	return true;
}

bool Loaded() { return g_loaded; }
uint32_t TitleId() { return g_titleId; }
const std::wstring& TitleName() { return g_titleName; }
uint8_t DefaultLanguage() { return g_defaultLanguage; }

const wchar_t* String(uint16_t stringId)
{
	auto it = g_strings.find(stringId);
	return it == g_strings.end() ? L"" : it->second.c_str();
}

const std::vector<Achievement>& Achievements() { return g_achievements; }

const Achievement* FindAchievement(uint32_t achievementId)
{
	for (const Achievement& achievement : g_achievements) {
		if (achievement.id == achievementId) {
			return &achievement;
		}
	}
	return nullptr;
}

bool Image(uint32_t imageId, const uint8_t** data, size_t* size)
{
	auto it = g_images.find(imageId);
	if (it == g_images.end()) {
		return false;
	}
	if (data) *data = it->second.data;
	if (size) *size = it->second.size;
	return true;
}

const std::map<uint32_t, StatView>& StatViews() { return g_views; }

const StatView* FindStatView(uint32_t viewId)
{
	auto it = g_views.find(viewId);
	return it == g_views.end() ? nullptr : &it->second;
}

const Query* FindQuery(uint32_t procedureIndex)
{
	auto it = g_queries.find(procedureIndex);
	return it == g_queries.end() ? nullptr : &it->second;
}

bool Constant(uint32_t constantId, double* value)
{
	auto it = g_constants.find(constantId);
	if (it == g_constants.end()) {
		return false;
	}
	*value = it->second;
	return true;
}

bool HasPresence() { return !g_presenceModes.empty(); }

bool PresenceStringId(uint32_t presenceValue, uint16_t* stringId)
{
	auto it = g_presenceModes.find(presenceValue);
	if (it == g_presenceModes.end()) {
		return false;
	}
	*stringId = it->second;
	return true;
}

bool ContextValueStringId(uint32_t contextId, uint32_t value, uint16_t* stringId)
{
	auto context = g_contextValues.find(contextId);
	if (context == g_contextValues.end()) {
		return false;
	}
	auto entry = context->second.find(value);
	if (entry == context->second.end()) {
		return false;
	}
	*stringId = entry->second;
	return true;
}

bool ContextDefault(uint32_t contextId, uint32_t* value)
{
	auto it = g_contextDefaults.find(contextId);
	if (it == g_contextDefaults.end()) {
		return false;
	}
	*value = it->second;
	return true;
}

}
}
