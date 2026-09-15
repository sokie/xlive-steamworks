// A small JSON reader for the configuration file. Objects keep insertion order.
#pragma once

#include <stdint.h>
#include <map>
#include <string>
#include <vector>

namespace xls {

class Json {
public:
	enum class Type {
		Null,
		Bool,
		Number,
		String,
		Array,
		Object,
	};

	Json() : m_type(Type::Null), m_bool(false), m_number(0) {}

	static bool Parse(const std::string& text, Json& out, std::string* error);

	Type GetType() const { return m_type; }
	bool IsNull() const { return m_type == Type::Null; }
	bool IsBool() const { return m_type == Type::Bool; }
	bool IsNumber() const { return m_type == Type::Number; }
	bool IsString() const { return m_type == Type::String; }
	bool IsArray() const { return m_type == Type::Array; }
	bool IsObject() const { return m_type == Type::Object; }

	// Missing keys and indexes return a shared null value, so lookups chain safely.
	const Json& operator[](const char* key) const;
	const Json& At(size_t index) const;
	bool Has(const char* key) const;
	size_t Size() const;

	bool GetBool(bool fallback) const { return m_type == Type::Bool ? m_bool : fallback; }
	double GetNumber(double fallback) const { return m_type == Type::Number ? m_number : fallback; }
	int64_t GetInt(int64_t fallback) const;
	uint32_t GetUint32(uint32_t fallback) const { return (uint32_t)GetInt((int64_t)fallback); }
	std::string GetString(const std::string& fallback) const { return m_type == Type::String ? m_string : fallback; }

	const std::vector<std::pair<std::string, Json>>& Items() const { return m_object; }
	const std::vector<Json>& Elements() const { return m_array; }

private:
	struct Parser;

	Type m_type;
	bool m_bool;
	double m_number;
	std::string m_string;
	std::vector<Json> m_array;
	std::vector<std::pair<std::string, Json>> m_object;
};

}
