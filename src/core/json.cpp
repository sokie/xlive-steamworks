#include "core/json.h"

#include <math.h>
#include <stdlib.h>

namespace xls {

struct Json::Parser {
	const std::string& text;
	size_t pos = 0;
	std::string error;

	explicit Parser(const std::string& t) : text(t) {}

	bool Fail(const char* message)
	{
		if (error.empty()) {
			error = message;
			error += " at offset " + std::to_string(pos);
		}
		return false;
	}

	void SkipWhitespace()
	{
		while (pos < text.size()) {
			char c = text[pos];
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
				pos++;
			}
			else if (c == '/' && pos + 1 < text.size() && text[pos + 1] == '/') {
				while (pos < text.size() && text[pos] != '\n') {
					pos++;
				}
			}
			else if (c == '/' && pos + 1 < text.size() && text[pos + 1] == '*') {
				pos += 2;
				while (pos + 1 < text.size() && !(text[pos] == '*' && text[pos + 1] == '/')) {
					pos++;
				}
				pos += 2;
			}
			else {
				break;
			}
		}
	}

	bool ParseValue(Json& out)
	{
		SkipWhitespace();
		if (pos >= text.size()) {
			return Fail("unexpected end of input");
		}
		char c = text[pos];
		if (c == '{') return ParseObject(out);
		if (c == '[') return ParseArray(out);
		if (c == '"') {
			out.m_type = Type::String;
			return ParseString(out.m_string);
		}
		if (c == 't' && text.compare(pos, 4, "true") == 0) {
			out.m_type = Type::Bool;
			out.m_bool = true;
			pos += 4;
			return true;
		}
		if (c == 'f' && text.compare(pos, 5, "false") == 0) {
			out.m_type = Type::Bool;
			out.m_bool = false;
			pos += 5;
			return true;
		}
		if (c == 'n' && text.compare(pos, 4, "null") == 0) {
			out.m_type = Type::Null;
			pos += 4;
			return true;
		}
		return ParseNumber(out);
	}

	bool ParseNumber(Json& out)
	{
		size_t start = pos;
		// Hex is accepted because title ids and property ids are written that way.
		if (pos + 1 < text.size() && text[pos] == '0' && (text[pos + 1] == 'x' || text[pos + 1] == 'X')) {
			pos += 2;
			size_t digits = pos;
			while (pos < text.size() && isxdigit((unsigned char)text[pos])) {
				pos++;
			}
			if (pos == digits) {
				return Fail("bad hex number");
			}
			out.m_type = Type::Number;
			out.m_number = (double)strtoull(text.c_str() + digits, nullptr, 16);
			return true;
		}
		if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) {
			pos++;
		}
		while (pos < text.size() && (isdigit((unsigned char)text[pos]) || text[pos] == '.' || text[pos] == 'e' || text[pos] == 'E' || text[pos] == '-' || text[pos] == '+')) {
			pos++;
		}
		if (pos == start) {
			return Fail("unexpected character");
		}
		out.m_type = Type::Number;
		out.m_number = strtod(text.c_str() + start, nullptr);
		return true;
	}

	static void AppendUtf8(std::string& out, uint32_t codepoint)
	{
		if (codepoint < 0x80) {
			out.push_back((char)codepoint);
		}
		else if (codepoint < 0x800) {
			out.push_back((char)(0xC0 | (codepoint >> 6)));
			out.push_back((char)(0x80 | (codepoint & 0x3F)));
		}
		else if (codepoint < 0x10000) {
			out.push_back((char)(0xE0 | (codepoint >> 12)));
			out.push_back((char)(0x80 | ((codepoint >> 6) & 0x3F)));
			out.push_back((char)(0x80 | (codepoint & 0x3F)));
		}
		else {
			out.push_back((char)(0xF0 | (codepoint >> 18)));
			out.push_back((char)(0x80 | ((codepoint >> 12) & 0x3F)));
			out.push_back((char)(0x80 | ((codepoint >> 6) & 0x3F)));
			out.push_back((char)(0x80 | (codepoint & 0x3F)));
		}
	}

	bool ParseHex4(uint32_t& value)
	{
		if (pos + 4 > text.size()) {
			return Fail("bad unicode escape");
		}
		value = 0;
		for (int i = 0; i < 4; i++) {
			char c = text[pos++];
			value <<= 4;
			if (c >= '0' && c <= '9') value |= (uint32_t)(c - '0');
			else if (c >= 'a' && c <= 'f') value |= (uint32_t)(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') value |= (uint32_t)(c - 'A' + 10);
			else return Fail("bad unicode escape");
		}
		return true;
	}

	bool ParseString(std::string& out)
	{
		if (text[pos] != '"') {
			return Fail("expected string");
		}
		pos++;
		out.clear();
		while (pos < text.size()) {
			char c = text[pos++];
			if (c == '"') {
				return true;
			}
			if (c != '\\') {
				out.push_back(c);
				continue;
			}
			if (pos >= text.size()) {
				break;
			}
			char e = text[pos++];
			switch (e) {
				case '"': out.push_back('"'); break;
				case '\\': out.push_back('\\'); break;
				case '/': out.push_back('/'); break;
				case 'b': out.push_back('\b'); break;
				case 'f': out.push_back('\f'); break;
				case 'n': out.push_back('\n'); break;
				case 'r': out.push_back('\r'); break;
				case 't': out.push_back('\t'); break;
				case 'u': {
					uint32_t codepoint = 0;
					if (!ParseHex4(codepoint)) {
						return false;
					}
					if (codepoint >= 0xD800 && codepoint <= 0xDBFF && pos + 6 <= text.size() && text[pos] == '\\' && text[pos + 1] == 'u') {
						pos += 2;
						uint32_t low = 0;
						if (!ParseHex4(low)) {
							return false;
						}
						codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
					}
					AppendUtf8(out, codepoint);
					break;
				}
				default:
					return Fail("bad escape");
			}
		}
		return Fail("unterminated string");
	}

	bool ParseArray(Json& out)
	{
		out.m_type = Type::Array;
		pos++;
		SkipWhitespace();
		if (pos < text.size() && text[pos] == ']') {
			pos++;
			return true;
		}
		while (true) {
			Json element;
			if (!ParseValue(element)) {
				return false;
			}
			out.m_array.push_back(std::move(element));
			SkipWhitespace();
			if (pos >= text.size()) {
				return Fail("unterminated array");
			}
			if (text[pos] == ',') {
				pos++;
				SkipWhitespace();
				if (pos < text.size() && text[pos] == ']') {
					pos++;
					return true;
				}
				continue;
			}
			if (text[pos] == ']') {
				pos++;
				return true;
			}
			return Fail("expected , or ]");
		}
	}

	bool ParseObject(Json& out)
	{
		out.m_type = Type::Object;
		pos++;
		SkipWhitespace();
		if (pos < text.size() && text[pos] == '}') {
			pos++;
			return true;
		}
		while (true) {
			SkipWhitespace();
			std::string key;
			if (pos >= text.size() || !ParseString(key)) {
				return Fail("expected object key");
			}
			SkipWhitespace();
			if (pos >= text.size() || text[pos] != ':') {
				return Fail("expected :");
			}
			pos++;
			Json value;
			if (!ParseValue(value)) {
				return false;
			}
			out.m_object.emplace_back(std::move(key), std::move(value));
			SkipWhitespace();
			if (pos >= text.size()) {
				return Fail("unterminated object");
			}
			if (text[pos] == ',') {
				pos++;
				SkipWhitespace();
				if (pos < text.size() && text[pos] == '}') {
					pos++;
					return true;
				}
				continue;
			}
			if (text[pos] == '}') {
				pos++;
				return true;
			}
			return Fail("expected , or }");
		}
	}
};

bool Json::Parse(const std::string& text, Json& out, std::string* error)
{
	Parser parser(text);
	// Skip a UTF-8 byte order mark.
	if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) {
		parser.pos = 3;
	}
	out = Json();
	if (!parser.ParseValue(out)) {
		if (error) {
			*error = parser.error;
		}
		return false;
	}
	parser.SkipWhitespace();
	if (parser.pos != text.size()) {
		if (error) {
			*error = "trailing characters at offset " + std::to_string(parser.pos);
		}
		return false;
	}
	return true;
}

const Json& Json::operator[](const char* key) const
{
	static const Json null;
	if (m_type != Type::Object || !key) {
		return null;
	}
	for (const auto& item : m_object) {
		if (item.first == key) {
			return item.second;
		}
	}
	return null;
}

const Json& Json::At(size_t index) const
{
	static const Json null;
	if (m_type != Type::Array || index >= m_array.size()) {
		return null;
	}
	return m_array[index];
}

bool Json::Has(const char* key) const
{
	if (m_type != Type::Object || !key) {
		return false;
	}
	for (const auto& item : m_object) {
		if (item.first == key) {
			return true;
		}
	}
	return false;
}

size_t Json::Size() const
{
	if (m_type == Type::Array) return m_array.size();
	if (m_type == Type::Object) return m_object.size();
	return 0;
}

int64_t Json::GetInt(int64_t fallback) const
{
	if (m_type == Type::Number) {
		return (int64_t)m_number;
	}
	if (m_type == Type::String && !m_string.empty()) {
		char* end = nullptr;
		long long value = strtoll(m_string.c_str(), &end, 0);
		if (end && *end == 0) {
			return value;
		}
	}
	return fallback;
}

}
