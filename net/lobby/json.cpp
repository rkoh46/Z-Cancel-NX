// SPDX-License-Identifier: GPL-3.0-or-later
#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace znxlobby
{

namespace
{
const Json kNull;

struct Parser
{
    const char* p;
    const char* end;
    std::string error;
    int depth = 0;

    void ws()
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            ++p;
    }

    bool fail(const char* what)
    {
        if (error.empty())
            error = what;
        return false;
    }

    static void appendUtf8(std::string& out, uint32_t cp)
    {
        if (cp < 0x80)
            out.push_back(static_cast<char>(cp));
        else if (cp < 0x800)
        {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
        else if (cp < 0x10000)
        {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
        else
        {
            out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }

    bool hex4(uint32_t& v)
    {
        if (end - p < 4)
            return fail("truncated \\u escape");
        v = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }

    bool string(std::string& out)
    {
        ++p; // opening quote
        while (p < end)
        {
            const char c = *p++;
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20)
                return fail("control character in string");
            if (c != '\\')
            {
                out.push_back(c);
                continue;
            }
            if (p >= end)
                break;
            const char e = *p++;
            switch (e)
            {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u':
            {
                uint32_t cp = 0;
                if (!hex4(cp))
                    return false;
                if (cp >= 0xd800 && cp < 0xdc00)
                {
                    uint32_t lo = 0;
                    if (end - p >= 6 && p[0] == '\\' && p[1] == 'u')
                    {
                        p += 2;
                        if (!hex4(lo))
                            return false;
                    }
                    if (lo >= 0xdc00 && lo < 0xe000)
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    else
                        cp = 0xfffd;
                }
                else if (cp >= 0xdc00 && cp < 0xe000)
                    cp = 0xfffd;
                appendUtf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool literal(const char* word)
    {
        const size_t n = std::strlen(word);
        if (size_t(end - p) < n || std::memcmp(p, word, n) != 0)
            return fail("bad literal");
        p += n;
        return true;
    }

    bool value(Json& out)
    {
        ws();
        if (p >= end)
            return fail("unexpected end");
        if (++depth > 64)
            return fail("nesting too deep");
        bool ok = true;
        switch (*p)
        {
        case '{':
        {
            out = Json::object();
            ++p;
            ws();
            if (p < end && *p == '}')
            {
                ++p;
                break;
            }
            for (;;)
            {
                ws();
                if (p >= end || *p != '"')
                {
                    ok = fail("expected key");
                    break;
                }
                std::string key;
                if (!string(key))
                {
                    ok = false;
                    break;
                }
                ws();
                if (p >= end || *p != ':')
                {
                    ok = fail("expected ':'");
                    break;
                }
                ++p;
                Json v;
                if (!value(v))
                {
                    ok = false;
                    break;
                }
                out.set(key.c_str(), std::move(v));
                ws();
                if (p < end && *p == ',')
                {
                    ++p;
                    continue;
                }
                if (p < end && *p == '}')
                {
                    ++p;
                    break;
                }
                ok = fail("expected ',' or '}'");
                break;
            }
            break;
        }
        case '[':
        {
            out = Json::array();
            ++p;
            ws();
            if (p < end && *p == ']')
            {
                ++p;
                break;
            }
            for (;;)
            {
                Json v;
                if (!value(v))
                {
                    ok = false;
                    break;
                }
                out.push(std::move(v));
                ws();
                if (p < end && *p == ',')
                {
                    ++p;
                    continue;
                }
                if (p < end && *p == ']')
                {
                    ++p;
                    break;
                }
                ok = fail("expected ',' or ']'");
                break;
            }
            break;
        }
        case '"':
        {
            std::string s;
            ok = string(s);
            out = Json::string(std::move(s));
            break;
        }
        case 't': ok = literal("true"); out = Json::boolean(true); break;
        case 'f': ok = literal("false"); out = Json::boolean(false); break;
        case 'n': ok = literal("null"); out = Json(); break;
        default:
        {
            // RFC 8259 number grammar, converted with strtod on a bounded copy.
            const char* start = p;
            if (p < end && *p == '-') ++p;
            if (p >= end || !(*p >= '0' && *p <= '9'))
            {
                ok = fail("bad value");
                break;
            }
            while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' ||
                               *p == '+' || *p == '-'))
                ++p;
            std::string text(start, p);
            char* stop = nullptr;
            const double v = std::strtod(text.c_str(), &stop);
            if (stop == nullptr || *stop != '\0')
            {
                ok = fail("bad number");
                break;
            }
            out = Json::number(v);
            break;
        }
        }
        --depth;
        return ok;
    }
};

void dumpString(std::string& out, const std::string& s)
{
    out.push_back('"');
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
                out.push_back(static_cast<char>(c));
        }
    }
    out.push_back('"');
}
} // namespace

const Json& Json::operator[](const char* key) const
{
    if (m_type != Object)
        return kNull;
    for (const auto& kv : m_obj)
        if (kv.first == key)
            return kv.second;
    return kNull;
}

const Json& Json::at(size_t index) const
{
    return (m_type == Array && index < m_arr.size()) ? m_arr[index] : kNull;
}

bool Json::has(const char* key) const
{
    if (m_type != Object)
        return false;
    for (const auto& kv : m_obj)
        if (kv.first == key)
            return true;
    return false;
}

int Json::toInt(int def) const
{
    if (m_type != Number || !std::isfinite(m_num))
        return def;
    // QJsonValue::toInt: only an exact integer in int range converts.
    if (m_num != std::floor(m_num) || m_num < -2147483648.0 || m_num > 2147483647.0)
        return def;
    return static_cast<int>(m_num);
}

uint64_t Json::toU64(uint64_t def) const
{
    if (m_type != Number || !std::isfinite(m_num) || m_num < 0 || m_num >= 18446744073709551616.0)
        return def;
    return static_cast<uint64_t>(m_num);
}

int64_t Json::toI64(int64_t def) const
{
    if (m_type != Number || !std::isfinite(m_num) || m_num < -9223372036854775808.0 ||
        m_num >= 9223372036854775808.0)
        return def;
    return static_cast<int64_t>(m_num);
}

Json& Json::set(const char* key, Json value)
{
    if (m_type != Object)
    {
        *this = object();
    }
    for (auto& kv : m_obj)
    {
        if (kv.first == key)
        {
            kv.second = std::move(value);
            return *this;
        }
    }
    m_obj.emplace_back(key, std::move(value));
    return *this;
}

Json& Json::push(Json value)
{
    if (m_type != Array)
        *this = array();
    m_arr.push_back(std::move(value));
    return *this;
}

void Json::dumpTo(std::string& out) const
{
    switch (m_type)
    {
    case Null: out += "null"; break;
    case Bool: out += m_bool ? "true" : "false"; break;
    case Number:
    {
        char buf[40];
        if (!std::isfinite(m_num))
            std::snprintf(buf, sizeof(buf), "null");
        else if (m_num == std::floor(m_num) && std::fabs(m_num) < 9007199254740992.0)
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(m_num));
        else
            std::snprintf(buf, sizeof(buf), "%.17g", m_num);
        out += buf;
        break;
    }
    case String: dumpString(out, m_str); break;
    case Array:
        out.push_back('[');
        for (size_t i = 0; i < m_arr.size(); ++i)
        {
            if (i) out.push_back(',');
            m_arr[i].dumpTo(out);
        }
        out.push_back(']');
        break;
    case Object:
        out.push_back('{');
        for (size_t i = 0; i < m_obj.size(); ++i)
        {
            if (i) out.push_back(',');
            dumpString(out, m_obj[i].first);
            out.push_back(':');
            m_obj[i].second.dumpTo(out);
        }
        out.push_back('}');
        break;
    }
}

std::string Json::dump() const
{
    std::string out;
    dumpTo(out);
    return out;
}

bool Json::parse(const std::string& text, Json& out, std::string* error)
{
    Parser parser{text.data(), text.data() + text.size(), {}};
    Json v;
    bool ok = parser.value(v);
    if (ok)
    {
        parser.ws();
        if (parser.p != parser.end)
            ok = parser.fail("trailing data");
    }
    if (!ok)
    {
        if (error)
            *error = parser.error;
        return false;
    }
    out = std::move(v);
    return true;
}

} // namespace znxlobby
