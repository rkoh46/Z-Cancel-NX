// SPDX-License-Identifier: GPL-3.0-or-later
/* A small JSON value, reader and writer for the lobby's WebSocket messages (RFC 8259). Numbers are
 * kept as doubles, as Qt's QJsonValue does on RMG-K's side: the lobby's ids are integers below
 * 2^53, which a double holds exactly. No exceptions, no locale dependence. */
#ifndef ZNX_LOBBY_JSON_H
#define ZNX_LOBBY_JSON_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace znxlobby
{

class Json
{
  public:
    enum Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    static Json null() { return Json(); }
    static Json boolean(bool v) { Json j; j.m_type = Bool; j.m_bool = v; return j; }
    static Json number(double v) { Json j; j.m_type = Number; j.m_num = v; return j; }
    static Json integer(int64_t v) { return number(static_cast<double>(v)); }
    static Json uinteger(uint64_t v) { return number(static_cast<double>(v)); }
    static Json string(std::string v) { Json j; j.m_type = String; j.m_str = std::move(v); return j; }
    static Json array() { Json j; j.m_type = Array; return j; }
    static Json object() { Json j; j.m_type = Object; return j; }

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Null; }
    bool isObject() const { return m_type == Object; }
    bool isArray() const { return m_type == Array; }

    /* Lookups never fail: a missing key or index gives a null value, and the typed getters
     * return their default for a value of another type (QJsonValue's behaviour). */
    const Json& operator[](const char* key) const;
    const Json& at(size_t index) const;
    bool has(const char* key) const;
    size_t size() const { return m_type == Array ? m_arr.size() : m_type == Object ? m_obj.size() : 0; }

    std::string str(const char* def = "") const { return m_type == String ? m_str : std::string(def); }
    double num(double def = 0) const { return m_type == Number ? m_num : def; }
    int toInt(int def = 0) const;
    uint64_t toU64(uint64_t def = 0) const;
    int64_t toI64(int64_t def = 0) const;
    bool toBool(bool def = false) const { return m_type == Bool ? m_bool : def; }

    /* Building. set() replaces an existing key. */
    Json& set(const char* key, Json value);
    Json& push(Json value);

    const std::vector<std::pair<std::string, Json>>& items() const { return m_obj; }
    const std::vector<Json>& elements() const { return m_arr; }

    std::string dump() const;
    static bool parse(const std::string& text, Json& out, std::string* error = nullptr);

  private:
    void dumpTo(std::string& out) const;

    Type m_type = Null;
    bool m_bool = false;
    double m_num = 0;
    std::string m_str;
    std::vector<Json> m_arr;
    std::vector<std::pair<std::string, Json>> m_obj;
};

} // namespace znxlobby

#endif
