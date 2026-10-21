#ifndef HTTP_PARSER_HPP
#define HTTP_PARSER_HPP

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>

namespace http_protocol {
enum class ParseResult {
    Ok,
    NeedMore,
    Bad,
    TooLarge
};

struct Request {
    std::string method;
    std::string target;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    size_t content_length = 0;
    bool close = false;
};

class HttpParser {
public:
    //解析请求报头,放入Request里面
    static ParseResult tryParseRequest(const std::string& buffer,
                                      Request& request,
                                      size_t& consumed) {
        consumed = 0;
        const size_t header_end = buffer.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            return buffer.size() > kMaxHeaderBytes
                       ? ParseResult::TooLarge
                       : ParseResult::NeedMore;
        }

        const size_t header_length = header_end + 4;
        if (header_length > kMaxHeaderBytes) return ParseResult::TooLarge;

        std::string lines[kMaxHeaderLines + 1];
        size_t line_count = 0;
        size_t pos = 0;
        while (pos < header_end) {
            size_t end = buffer.find("\r\n", pos);
            if (end == std::string::npos || end > header_end) end = header_end;
            if (line_count >= kMaxHeaderLines + 1) return ParseResult::Bad;
            lines[line_count++] = buffer.substr(pos, end - pos);
            pos = end + 2;
        }
        if (line_count == 0 || lines[0].empty()) return ParseResult::Bad;
        if (line_count - 1 > kMaxHeaderLines) return ParseResult::Bad;

        const std::string& request_line = lines[0];
        const size_t first_space = request_line.find(' ');
        if (first_space == std::string::npos || first_space == 0) {
            return ParseResult::Bad;
        }
        const size_t second_space = request_line.find(' ', first_space + 1);
        if (second_space == std::string::npos ||
            second_space == first_space + 1 ||
            request_line.find(' ', second_space + 1) != std::string::npos) {
            return ParseResult::Bad;
        }

        request.method = request_line.substr(0, first_space);
        request.target =
            request_line.substr(first_space + 1, second_space - first_space - 1);
        request.version = request_line.substr(second_space + 1);
        for (unsigned char ch : request.method) {
            if (!isTokenChar(ch)) return ParseResult::Bad;
        }
        if (request.target.empty() || request.target[0] != '/') {
            return ParseResult::Bad;
        }
        for (unsigned char ch : request.target) {
            if (ch <= 0x20 || ch == 0x7f) return ParseResult::Bad;
        }
        if (request.version != "HTTP/1.1" && request.version != "HTTP/1.0") {
            return ParseResult::Bad;
        }

        request.headers.clear();
        request.content_length = 0;
        request.close = false;
        bool has_content_length = false;
        bool has_transfer_encoding = false;

        for (size_t i = 1; i < line_count; ++i) {
            const std::string& line = lines[i];
            if (line.empty()) return ParseResult::Bad;
            const size_t colon = line.find(':');
            if (colon == std::string::npos || colon == 0) {
                return ParseResult::Bad;
            }

            const std::string raw_key = line.substr(0, colon);
            for (unsigned char ch : raw_key) {
                if (!isTokenChar(ch)) return ParseResult::Bad;
            }
            const std::string key = toLower(raw_key);
            const std::string value = trim(line.substr(colon + 1));
            for (unsigned char ch : value) {
                if ((ch < 0x20 && ch != '\t') || ch == 0x7f) {
                    return ParseResult::Bad;
                }
            }

            if (key == "content-length") {
                if (has_content_length || value.empty()) return ParseResult::Bad;
                size_t parsed = 0;
                for (unsigned char ch : value) {
                    if (ch < '0' || ch > '9') return ParseResult::Bad;
                    const size_t digit = static_cast<size_t>(ch - '0');
                    if (parsed > (std::numeric_limits<size_t>::max() - digit) / 10) {
                        return ParseResult::Bad;
                    }
                    parsed = parsed * 10 + digit;
                }
                request.content_length = parsed;
                has_content_length = true;
            } else if (key == "transfer-encoding") {
                has_transfer_encoding = true;
            }

            std::unordered_map<std::string, std::string>::iterator existing =
                request.headers.find(key);
            if (existing == request.headers.end()) {
                request.headers.emplace(key, value);
            } else {
                existing->second += ", ";
                existing->second += value;
            }
        }

        if (has_transfer_encoding || request.content_length > 0) {
            return ParseResult::Bad;
        }

        bool connection_close = false;
        bool connection_keep_alive = false;
        const std::unordered_map<std::string, std::string>::const_iterator connection =
            request.headers.find("connection");
        if (connection != request.headers.end()) {
            const std::string& value = connection->second;
            size_t token_start = 0;
            while (token_start <= value.size()) {
                size_t comma = value.find(',', token_start);
                if (comma == std::string::npos) comma = value.size();
                const std::string token =
                    toLower(trim(value.substr(token_start, comma - token_start)));
                if (token == "close") connection_close = true;
                if (token == "keep-alive") connection_keep_alive = true;
                token_start = comma + 1;
            }
        }

        if (request.version == "HTTP/1.1") {
            request.close = connection_close;
        } else {
            request.close = connection_close || !connection_keep_alive;
        }

        consumed = header_length;
        return ParseResult::Ok;
    }
    //分离path和query
    static void splitTarget(const std::string& target,
                            std::string& path,
                            std::string& query) {
        const size_t query_start = target.find('?');
        if (query_start == std::string::npos) {
            path = target;
            query.clear();
            return;
        }
        path = target.substr(0, query_start);
        query = target.substr(query_start + 1);
    }
    //分离query
    static bool parseQuery(
        const std::string& query,
        std::unordered_map<std::string, std::string>& params) {
        size_t pos = 0;
        while (pos < query.size()) {
            size_t ampersand = query.find('&', pos);
            if (ampersand == std::string::npos) ampersand = query.size();

            const std::string pair = query.substr(pos, ampersand - pos);
            const size_t equals = pair.find('=');
            const std::string raw_key =
                equals == std::string::npos ? pair : pair.substr(0, equals);
            const std::string raw_value =
                equals == std::string::npos ? std::string() : pair.substr(equals + 1);

            std::string key;
            std::string value;
            if (!urlDecode(raw_key, key) || !urlDecode(raw_value, value)) {
                return false;
            }
            if (!key.empty()) params[key] = value;
            pos = ampersand + 1;
        }
        return true;
    }
    //转换成json串
    static std::string jsonEscape(const std::string& value) {
        static const char kHex[] = "0123456789abcdef";
        std::string escaped;
        escaped.reserve(value.size());
        for (unsigned char ch : value) {
            switch (ch) {
                case '"': escaped += "\\\""; break;
                case '\\': escaped += "\\\\"; break;
                case '\b': escaped += "\\b"; break;
                case '\f': escaped += "\\f"; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default:
                    if (ch < 0x20) {
                        escaped += "\\u00";
                        escaped.push_back(kHex[ch >> 4]);
                        escaped.push_back(kHex[ch & 0x0f]);
                    } else {
                        escaped.push_back(static_cast<char>(ch));
                    }
            }
        }
        return escaped;
    }

private:
    static const size_t kMaxHeaderBytes = 8192;
    static const size_t kMaxHeaderLines = 100;

    static std::string toLower(std::string value) {
        for (char& ch : value) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return value;
    }
    //去掉字符串开头和结尾的空格
    static std::string trim(const std::string& value) {
        const size_t begin = value.find_first_not_of(" \t");
        if (begin == std::string::npos) return std::string();
        const size_t end = value.find_last_not_of(" \t");
        return value.substr(begin, end - begin + 1);
    }
    //检查符号合法性
    static bool isTokenChar(unsigned char ch) {
        if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z')) {
            return true;
        }
        switch (ch) {
            case '!': case '#': case '$': case '%': case '&': case '\'':
            case '*': case '+': case '-': case '.': case '^': case '_':
            case '`': case '|': case '~':
                return true;
            default:
                return false;
        }
    }
    //转义写法还原成原始字符
    static bool urlDecode(const std::string& input, std::string& output) {
        output.clear();
        output.reserve(input.size());

        const auto hexValue = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            return -1;
        };

        for (size_t i = 0; i < input.size(); ++i) {
            if (input[i] == '+') {
                output.push_back(' ');
            } else if (input[i] == '%') {
                if (i + 2 >= input.size()) return false;
                const int high = hexValue(input[i + 1]);
                const int low = hexValue(input[i + 2]);
                if (high < 0 || low < 0) return false;
                output.push_back(static_cast<char>((high << 4) | low));
                i += 2;
            } else {
                output.push_back(input[i]);
            }
        }
        return true;
    }
};
}  // namespace http_protocol

#endif  // HTTP_PARSER_HPP
