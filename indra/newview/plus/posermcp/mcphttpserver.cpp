/**
 * @file mcphttpserver.cpp
 * @brief A minimal MCP server (Streamable HTTP transport, JSON-RPC 2.0) on
 *        the loopback interface. See mcphttpserver.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

// No viewer headers here on purpose (see mcphttpserver.h). Inside the viewer
// the build force-includes the precompiled header; with WIN32_LEAN_AND_MEAN
// that does not pull in the old winsock.h, so winsock2.h below is fine.

#include "mcphttpserver.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <random>
#include <utility>

#if defined(_WIN32)
#   include <winsock2.h>
#   include <ws2tcpip.h>
#   if defined(_MSC_VER)
#       pragma comment(lib, "ws2_32.lib")
#   endif
#else
#   include <arpa/inet.h>
#   include <cerrno>
#   include <fcntl.h>
#   include <netinet/in.h>
#   include <sys/select.h>
#   include <sys/socket.h>
#   include <sys/time.h>
#   include <unistd.h>
#endif

namespace json = boost::json;

namespace
{
    constexpr size_t MAX_BATCH_MESSAGES = 32;   // JSON-RPC messages in one batch (MCP clients send one at a time)

#if defined(_WIN32)
    using socket_t                     = SOCKET;
    constexpr socket_t INVALID_SOCKET_VALUE = INVALID_SOCKET;
    constexpr int      SHUTDOWN_SEND        = SD_SEND;
    constexpr int      SHUTDOWN_BOTH        = SD_BOTH;

    void closeSocket(socket_t s) { ::closesocket(s); }
    bool addressInUse() { return ::WSAGetLastError() == WSAEADDRINUSE; }
#else
    using socket_t                        = int;
    constexpr socket_t INVALID_SOCKET_VALUE = -1;
    constexpr int      SHUTDOWN_SEND        = SHUT_WR;
    constexpr int      SHUTDOWN_BOTH        = SHUT_RDWR;

    void closeSocket(socket_t s) { ::close(s); }
    bool addressInUse() { return errno == EADDRINUSE; }
#endif

#if defined(MSG_NOSIGNAL)
    constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
    constexpr int SEND_FLAGS = 0;
#endif

    socket_t toSocket(std::uintptr_t s) { return static_cast<socket_t>(s); }
    std::uintptr_t fromSocket(socket_t s) { return static_cast<std::uintptr_t>(s); }

    void setNoInherit(socket_t s)
    {
#if defined(_WIN32)
        ::SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
#else
        int flags = ::fcntl(s, F_GETFD);
        if (flags >= 0)
        {
            ::fcntl(s, F_SETFD, flags | FD_CLOEXEC);
        }
#endif
    }

    using Clock = std::chrono::steady_clock;

    void setTimeout(socket_t s, int option, int timeout_ms)
    {
#if defined(_WIN32)
        DWORD value = static_cast<DWORD>(timeout_ms);
        ::setsockopt(s, SOL_SOCKET, option, reinterpret_cast<const char*>(&value), sizeof(value));
#else
        timeval value{};
        value.tv_sec  = timeout_ms / 1000;
        value.tv_usec = (timeout_ms % 1000) * 1000;
        ::setsockopt(s, SOL_SOCKET, option, &value, sizeof(value));
#endif
    }

    void setTimeouts(socket_t s, int timeout_ms)
    {
        setTimeout(s, SO_RCVTIMEO, timeout_ms);
        setTimeout(s, SO_SNDTIMEO, timeout_ms);
#if !defined(_WIN32) && defined(SO_NOSIGPIPE)
        int one = 1;
        ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    }

    // recv() that ends by deadline: each call may wait only for what is left of it (at most
    // max_wait_ms), so a client sending a byte now and then cannot keep the connection, or
    // the thread reading it, for long. -1 (as a failed recv) once the deadline has passed.
    int recvBefore(socket_t s, char* buffer, int size, Clock::time_point deadline, int max_wait_ms)
    {
        const long long left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0)
            return -1;
        // never 0: on Windows a receive timeout of 0 waits for ever
        setTimeout(s, SO_RCVTIMEO, static_cast<int>(std::max<long long>(1, std::min<long long>(left, max_wait_ms))));
        return ::recv(s, buffer, size, 0);
    }

    bool sendAll(socket_t s, const std::string& data)
    {
        size_t sent = 0;
        while (sent < data.size())
        {
            const size_t chunk = std::min<size_t>(data.size() - sent, 64 * 1024);
            const int    n     = ::send(s, data.data() + sent, static_cast<int>(chunk), SEND_FLAGS);
            if (n <= 0)
            {
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    // After shutdown(send): read and discard what the client still sends
    // (briefly) so closing does not reset the connection before the client
    // has read our response.
    // At most a second in all, however the client sends.
    void drainBeforeClose(socket_t s)
    {
        const Clock::time_point deadline = Clock::now() + std::chrono::seconds(1);
        char   buffer[4096];
        size_t total = 0;
        while (total < 256 * 1024)
        {
            const int n = recvBefore(s, buffer, sizeof(buffer), deadline, 500);
            if (n <= 0)
                break;
            total += static_cast<size_t>(n);
        }
    }

    std::string toLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    std::string trim(const std::string& s)
    {
        size_t begin = 0, end = s.size();
        while (begin < end && (s[begin] == ' ' || s[begin] == '\t'))
            ++begin;
        while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t'))
            --end;
        return s.substr(begin, end - begin);
    }

    const char* reasonPhrase(int status)
    {
        switch (status)
        {
            case 200: return "OK";
            case 202: return "Accepted";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 408: return "Request Timeout";
            case 411: return "Length Required";
            case 413: return "Payload Too Large";
            case 415: return "Unsupported Media Type";
            case 431: return "Request Header Fields Too Large";
            case 500: return "Internal Server Error";
            case 501: return "Not Implemented";
            case 503: return "Service Unavailable";
            default:  return "Error";
        }
    }

    struct HttpResponse
    {
        int                                              status = 200;
        std::string                                      body;
        std::vector<std::pair<std::string, std::string>> extraHeaders;
    };

    std::string serializeResponse(const HttpResponse& response)
    {
        std::string out = "HTTP/1.1 " + std::to_string(response.status) + " " + reasonPhrase(response.status) + "\r\n";
        if (!response.body.empty())
        {
            out += "Content-Type: application/json\r\n";
        }
        out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
        out += "Cache-Control: no-store\r\n";
        out += "Connection: close\r\n";
        for (const auto& header : response.extraHeaders)
        {
            out += header.first + ": " + header.second + "\r\n";
        }
        out += "\r\n";
        out += response.body;
        return out;
    }

    // A JSON-RPC error that is not tied to a request id (HTTP-level errors).
    HttpResponse httpError(int status, int code, const std::string& message)
    {
        json::object error;
        error["code"]    = code;
        error["message"] = message;

        json::object body;
        body["jsonrpc"] = "2.0";
        body["id"]      = nullptr;
        body["error"]   = std::move(error);

        HttpResponse response;
        response.status = status;
        response.body   = json::serialize(body);
        return response;
    }

    struct HttpRequest
    {
        std::string                                      method;
        std::string                                      target;
        std::vector<std::pair<std::string, std::string>> headers; // names lower-cased
        std::string                                      body;

        // Returns the header value; sets duplicate if it occurs more than once.
        std::string header(const std::string& name, bool* present = nullptr, bool* duplicate = nullptr) const
        {
            std::string value;
            int         count = 0;
            for (const auto& h : headers)
            {
                if (h.first == name)
                {
                    if (count++ == 0)
                        value = h.second;
                }
            }
            if (present)
                *present = count > 0;
            if (duplicate)
                *duplicate = count > 1;
            return value;
        }
    };

    // Reads one request. On failure fills error_response (status 0 = just close).
    bool readRequest(socket_t s, const plus::mcp::HttpConfig& config, HttpRequest& request, HttpResponse& error_response)
    {
        error_response.status = 0;

        // The whole request (headers and body) within requestTimeoutMs, not only each recv:
        // otherwise a byte every few seconds would hold a connection (of maxConnections) for good.
        const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(config.requestTimeoutMs);

        std::string buffer;
        size_t      header_end = std::string::npos;
        char        chunk[4096];
        while (header_end == std::string::npos)
        {
            const int n = recvBefore(s, chunk, sizeof(chunk), deadline, config.ioTimeoutMs);
            if (n <= 0)
            {
                if (!buffer.empty())
                    error_response = httpError(408, -32000, "Incomplete request");
                return false;
            }
            buffer.append(chunk, static_cast<size_t>(n));
            header_end = buffer.find("\r\n\r\n");
            if (header_end == std::string::npos && buffer.size() > config.maxHeaderBytes)
            {
                error_response = httpError(431, -32000, "Request headers too large");
                return false;
            }
        }
        if (header_end > config.maxHeaderBytes)
        {
            error_response = httpError(431, -32000, "Request headers too large");
            return false;
        }

        // Request line and headers
        size_t line_start = 0;
        bool   first_line = true;
        while (line_start < header_end)
        {
            size_t line_end = buffer.find("\r\n", line_start);
            if (line_end == std::string::npos || line_end > header_end)
                line_end = header_end;
            const std::string line = buffer.substr(line_start, line_end - line_start);
            line_start             = line_end + 2;

            if (first_line)
            {
                first_line       = false;
                const size_t sp1 = line.find(' ');
                const size_t sp2 = sp1 == std::string::npos ? std::string::npos : line.find(' ', sp1 + 1);
                if (sp1 == std::string::npos || sp2 == std::string::npos || line.compare(sp2 + 1, 7, "HTTP/1.") != 0)
                {
                    error_response = httpError(400, -32000, "Malformed request line");
                    return false;
                }
                request.method = line.substr(0, sp1);
                request.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
                continue;
            }

            if (line.empty())
                continue;
            const size_t colon = line.find(':');
            if (colon == std::string::npos || colon == 0 || line[0] == ' ' || line[0] == '\t')
            {
                error_response = httpError(400, -32000, "Malformed header line");
                return false;
            }
            request.headers.emplace_back(toLower(line.substr(0, colon)), trim(line.substr(colon + 1)));
        }

        // Ambiguous framing or identity headers are refused outright.
        for (const char* name : { "host", "origin", "authorization", "content-length", "transfer-encoding" })
        {
            bool duplicate = false;
            request.header(name, nullptr, &duplicate);
            if (duplicate)
            {
                error_response = httpError(400, -32000, std::string("Duplicate ") + name + " header");
                return false;
            }
        }

        bool has_transfer_encoding = false;
        request.header("transfer-encoding", &has_transfer_encoding);
        if (has_transfer_encoding)
        {
            error_response = httpError(501, -32000, "Transfer-Encoding is not supported; send Content-Length");
            return false;
        }

        size_t      content_length = 0;
        bool        has_length     = false;
        std::string length_text    = request.header("content-length", &has_length);
        if (has_length)
        {
            if (length_text.empty() || length_text.size() > 10 ||
                !std::all_of(length_text.begin(), length_text.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
            {
                error_response = httpError(400, -32000, "Invalid Content-Length");
                return false;
            }
            content_length = static_cast<size_t>(std::stoull(length_text));
        }
        else if (request.method == "POST")
        {
            error_response = httpError(411, -32000, "Content-Length required");
            return false;
        }

        if (content_length > config.maxBodyBytes)
        {
            error_response = httpError(413, -32000, "Request body too large");
            return false;
        }

        request.body = buffer.substr(header_end + 4);
        while (request.body.size() < content_length)
        {
            const int n = recvBefore(s, chunk, sizeof(chunk), deadline, config.ioTimeoutMs);
            if (n <= 0)
            {
                error_response = httpError(408, -32000, "Incomplete request body");
                return false;
            }
            request.body.append(chunk, static_cast<size_t>(n));
        }
        if (request.body.size() > content_length)
        {
            // One request per connection: no pipelining.
            request.body.resize(content_length);
        }
        return true;
    }

    // A JSON-RPC error raised while handling a request.
    struct RpcError
    {
        int         code;
        std::string message;
    };

    json::object makeErrorResponse(const json::value& id, int code, const std::string& message)
    {
        json::object error;
        error["code"]    = code;
        error["message"] = message;

        json::object response;
        response["jsonrpc"] = "2.0";
        response["id"]      = id;
        response["error"]   = std::move(error);
        return response;
    }

    json::object serializeContent(const plus::mcp::Content& content)
    {
        json::object item;
        if (content.type == plus::mcp::Content::Type::Image)
        {
            item["type"]     = "image";
            item["data"]     = plus::mcp::base64Encode(content.data);
            item["mimeType"] = content.mimeType;
        }
        else
        {
            item["type"] = "text";
            item["text"] = content.text;
        }
        return item;
    }
}

namespace plus::mcp
{
    // ---------------------------------------------------------------------
    // Content / ToolResult

    Content Content::makeText(std::string text)
    {
        Content content;
        content.type = Type::Text;
        content.text = std::move(text);
        return content;
    }

    Content Content::makeImage(std::string bytes, std::string mime_type)
    {
        Content content;
        content.type     = Type::Image;
        content.data     = std::move(bytes);
        content.mimeType = std::move(mime_type);
        return content;
    }

    ToolResult ToolResult::text(std::string text)
    {
        ToolResult result;
        result.content.push_back(Content::makeText(std::move(text)));
        return result;
    }

    ToolResult ToolResult::error(std::string message)
    {
        ToolResult result = text(std::move(message));
        result.isError    = true;
        return result;
    }

    ToolResult ToolResult::json(const boost::json::value& value)
    {
        return text(boost::json::serialize(value));
    }

    // ---------------------------------------------------------------------
    // helpers

    const std::vector<std::string>& supportedProtocolVersions()
    {
        static const std::vector<std::string> versions{ "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05" };
        return versions;
    }

    std::string generateToken(size_t bytes)
    {
        static const char hex[] = "0123456789abcdef";
        std::random_device device;
        std::string        token;
        token.reserve(bytes * 2);
        unsigned int word = 0;
        for (size_t i = 0; i < bytes; ++i)
        {
            if (i % 4 == 0)
                word = device();
            const unsigned int byte = (word >> ((i % 4) * 8)) & 0xffu;
            token += hex[byte >> 4];
            token += hex[byte & 0x0fu];
        }
        return token;
    }

    std::string base64Encode(const std::string& data)
    {
        static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((data.size() + 2) / 3) * 4);
        size_t i = 0;
        for (; i + 2 < data.size(); i += 3)
        {
            const unsigned int n = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8) |
                                   static_cast<unsigned char>(data[i + 2]);
            out += alphabet[(n >> 18) & 63];
            out += alphabet[(n >> 12) & 63];
            out += alphabet[(n >> 6) & 63];
            out += alphabet[n & 63];
        }
        const size_t rest = data.size() - i;
        if (rest == 1)
        {
            const unsigned int n = static_cast<unsigned char>(data[i]) << 16;
            out += alphabet[(n >> 18) & 63];
            out += alphabet[(n >> 12) & 63];
            out += "==";
        }
        else if (rest == 2)
        {
            const unsigned int n = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8);
            out += alphabet[(n >> 18) & 63];
            out += alphabet[(n >> 12) & 63];
            out += alphabet[(n >> 6) & 63];
            out += '=';
        }
        return out;
    }

    bool isAllowedHost(const std::string& host, uint16_t port)
    {
        const std::string value  = toLower(trim(host));
        const std::string suffix = ":" + std::to_string(port);
        return value == "127.0.0.1" + suffix || value == "localhost" + suffix;
    }

    bool isAllowedOrigin(const std::string& origin)
    {
        const std::string value = toLower(trim(origin));
        std::string       rest;
        if (value.rfind("http://", 0) == 0)
            rest = value.substr(7);
        else if (value.rfind("https://", 0) == 0)
            rest = value.substr(8);
        else
            return false; // includes "null" (sandboxed frames, file://)

        std::string host = rest;
        std::string port;
        if (!rest.empty() && rest[0] == '[')
        {
            const size_t close = rest.find(']');
            if (close == std::string::npos)
                return false;
            host = rest.substr(0, close + 1);
            if (close + 1 < rest.size())
            {
                if (rest[close + 1] != ':')
                    return false;
                port = rest.substr(close + 2);
            }
        }
        else
        {
            const size_t colon = rest.find(':');
            if (colon != std::string::npos)
            {
                host = rest.substr(0, colon);
                port = rest.substr(colon + 1);
            }
        }

        if (rest.find('/') != std::string::npos)
            return false;
        if (!port.empty() && (port.size() > 5 || !std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c) != 0; })))
            return false;

        return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
    }

    bool isAuthorized(const std::string& authorization, const std::string& token)
    {
        if (token.empty())
            return false;

        const std::string value = trim(authorization);
        if (value.size() < 7 || toLower(value.substr(0, 7)) != "bearer ")
            return false;
        const std::string presented = trim(value.substr(7));

        // Constant time with respect to the content of both strings.
        const size_t length = std::max(presented.size(), token.size());
        unsigned int diff   = static_cast<unsigned int>(presented.size() ^ token.size());
        for (size_t i = 0; i < length; ++i)
        {
            const unsigned char a = i < presented.size() ? static_cast<unsigned char>(presented[i]) : 0;
            const unsigned char b = i < token.size() ? static_cast<unsigned char>(token[i]) : 0;
            diff |= static_cast<unsigned int>(a ^ b);
        }
        return diff == 0;
    }

    // ---------------------------------------------------------------------
    // Dispatcher

    Dispatcher::Dispatcher(ServerInfo info, std::vector<Tool> tools) : mInfo(std::move(info)), mTools(std::move(tools))
    {
    }

    Dispatcher::Reply Dispatcher::handlePost(const std::string& body) const
    {
        Reply reply;

        boost::system::error_code ec;
        json::value               message = json::parse(body, ec);
        if (ec)
        {
            reply.status = 400;
            reply.body   = json::serialize(makeErrorResponse(nullptr, -32700, "Parse error: " + ec.message()));
            return reply;
        }

        if (message.is_array())
        {
            const json::array& batch = message.get_array();
            if (batch.empty())
            {
                reply.status = 400;
                reply.body   = json::serialize(makeErrorResponse(nullptr, -32600, "Invalid Request: empty batch"));
                return reply;
            }
            // Each tool call of a batch waits for the main thread in turn: a long batch would
            // hold the connection (and queue viewer work) for a long time.
            if (batch.size() > MAX_BATCH_MESSAGES)
            {
                reply.status = 400;
                reply.body   = json::serialize(makeErrorResponse(
                    nullptr, -32600, "Invalid Request: a batch may have at most " + std::to_string(MAX_BATCH_MESSAGES) + " messages"));
                return reply;
            }

            json::array responses;
            for (const json::value& item : batch)
            {
                json::value response = handleMessage(item);
                if (!response.is_null())
                    responses.push_back(std::move(response));
            }

            if (responses.empty())
            {
                reply.status = 202;
                return reply;
            }
            reply.body = json::serialize(responses);
            return reply;
        }

        json::value response = handleMessage(message);
        if (response.is_null())
        {
            reply.status = 202;
            return reply;
        }
        reply.body = json::serialize(response);
        return reply;
    }

    json::value Dispatcher::handleMessage(const json::value& message) const
    {
        if (!message.is_object())
            return makeErrorResponse(nullptr, -32600, "Invalid Request: not an object");

        const json::object& object = message.get_object();

        json::value id      = nullptr;
        const bool  has_id  = object.contains("id");
        if (has_id)
        {
            const json::value& raw_id = object.at("id");
            if (raw_id.is_string() || raw_id.is_int64() || raw_id.is_uint64() || raw_id.is_double())
                id = raw_id;
            else if (!raw_id.is_null())
                return makeErrorResponse(nullptr, -32600, "Invalid Request: bad id");
        }

        const json::value* version = object.if_contains("jsonrpc");
        if (!version || !version->is_string() || version->get_string() != "2.0")
            return makeErrorResponse(id, -32600, "Invalid Request: jsonrpc must be \"2.0\"");

        const json::value* method = object.if_contains("method");
        if (!method)
        {
            // A response to a request we sent (we never send any): ignore.
            return nullptr;
        }
        if (!method->is_string())
            return makeErrorResponse(id, -32600, "Invalid Request: method must be a string");

        const std::string method_name(method->get_string());
        if (!has_id)
        {
            // Notification (notifications/initialized, notifications/cancelled, ...).
            return nullptr;
        }

        const json::value* params = object.if_contains("params");
        try
        {
            json::object response;
            response["jsonrpc"] = "2.0";
            response["id"]      = id;
            response["result"]  = handleRequest(method_name, params ? *params : json::value(json::object()));
            return response;
        }
        catch (const RpcError& error)
        {
            return makeErrorResponse(id, error.code, error.message);
        }
        catch (const std::exception& e)
        {
            return makeErrorResponse(id, -32603, std::string("Internal error: ") + e.what());
        }
    }

    json::value Dispatcher::handleRequest(const std::string& method, const json::value& params) const
    {
        if (method == "initialize")
        {
            std::string requested;
            if (params.is_object())
            {
                const json::value* version = params.get_object().if_contains("protocolVersion");
                if (version && version->is_string())
                    requested = std::string(version->get_string());
            }

            const auto& versions   = supportedProtocolVersions();
            std::string negotiated = versions.front();
            if (std::find(versions.begin(), versions.end(), requested) != versions.end())
                negotiated = requested;

            json::object tools_capability;
            tools_capability["listChanged"] = false;
            json::object capabilities;
            capabilities["tools"] = std::move(tools_capability);

            json::object server_info;
            server_info["name"]    = mInfo.name;
            server_info["version"] = mInfo.version;

            json::object result;
            result["protocolVersion"] = negotiated;
            result["capabilities"]    = std::move(capabilities);
            result["serverInfo"]      = std::move(server_info);
            if (!mInfo.instructions.empty())
                result["instructions"] = mInfo.instructions;
            return result;
        }

        if (method == "ping")
            return json::object();

        if (method == "tools/list")
            return toolsList();

        if (method == "tools/call")
            return toolsCall(params);

        throw RpcError{ -32601, "Method not found: " + method };
    }

    json::object Dispatcher::toolsList() const
    {
        json::array tools;
        for (const Tool& tool : mTools)
        {
            json::object entry;
            entry["name"] = tool.name;
            if (!tool.title.empty())
                entry["title"] = tool.title;
            entry["description"] = tool.description;
            entry["inputSchema"] = tool.inputSchema;
            if (tool.readOnly)
            {
                json::object annotations;
                annotations["readOnlyHint"] = true;
                entry["annotations"]        = std::move(annotations);
            }
            tools.push_back(std::move(entry));
        }

        json::object result;
        result["tools"] = std::move(tools);
        return result;
    }

    json::object Dispatcher::toolsCall(const json::value& params) const
    {
        if (!params.is_object())
            throw RpcError{ -32602, "Invalid params: expected an object" };

        const json::object& object = params.get_object();
        const json::value*  name   = object.if_contains("name");
        if (!name || !name->is_string())
            throw RpcError{ -32602, "Invalid params: missing tool name" };

        json::object       arguments;
        const json::value* raw_arguments = object.if_contains("arguments");
        if (raw_arguments && !raw_arguments->is_null())
        {
            if (!raw_arguments->is_object())
                throw RpcError{ -32602, "Invalid params: arguments must be an object" };
            arguments = raw_arguments->get_object();
        }

        const auto tool = std::find_if(mTools.begin(), mTools.end(), [&](const Tool& t) { return t.name == name->get_string(); });
        if (tool == mTools.end())
            throw RpcError{ -32602, "Unknown tool: " + std::string(name->get_string()) };

        ToolResult result;
        try
        {
            result = tool->handler ? tool->handler(arguments) : ToolResult::error("Tool has no handler");
        }
        catch (const std::exception& e)
        {
            result = ToolResult::error(std::string("Tool failed: ") + e.what());
        }

        json::array content;
        for (const Content& item : result.content)
            content.push_back(serializeContent(item));

        json::object response;
        response["content"] = std::move(content);
        response["isError"] = result.isError;
        return response;
    }

    // ---------------------------------------------------------------------
    // HttpServer

    struct HttpServer::Connection
    {
        std::thread       thread;
        std::uintptr_t    socket = 0;
        std::atomic<bool> done{ false };
    };

    HttpServer::HttpServer(HttpConfig config, std::shared_ptr<const Dispatcher> dispatcher)
        : mConfig(std::move(config)),
          mDispatcher(std::move(dispatcher)),
          mToken(mConfig.token),
          mListenSocket(fromSocket(INVALID_SOCKET_VALUE))
    {
    }

    HttpServer::~HttpServer()
    {
        stop();
    }

    void HttpServer::setToken(const std::string& token)
    {
        std::lock_guard<std::mutex> lock(mTokenMutex);
        mToken = token;
    }

    std::string HttpServer::currentToken() const
    {
        std::lock_guard<std::mutex> lock(mTokenMutex);
        return mToken;
    }

    bool HttpServer::start(std::string& error)
    {
        if (mRunning)
            return true;
        mBindFailed = false;

        if (!mDispatcher)
        {
            error = "no dispatcher";
            return false;
        }

#if defined(_WIN32)
        WSADATA wsa_data;
        if (::WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
        {
            error = "WSAStartup failed";
            return false;
        }
        mSocketsInitialized = true;

        socket_t listen_socket = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
#else
        socket_t listen_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
        if (listen_socket == INVALID_SOCKET_VALUE)
        {
            error = "cannot create a socket";
            stop();
            return false;
        }
        setNoInherit(listen_socket);

        int one = 1;
#if defined(_WIN32)
        // Nobody else may bind the same port while we hold it.
        ::setsockopt(listen_socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof(one));
#else
        ::setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif

        sockaddr_in address{};
        address.sin_family      = AF_INET;
        address.sin_port        = htons(mConfig.port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only, never INADDR_ANY

        if (::bind(listen_socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        {
            error = addressInUse() ? "port " + std::to_string(mConfig.port) + " is already in use"
                                   : "cannot bind 127.0.0.1:" + std::to_string(mConfig.port);
            mBindFailed = true;
            closeSocket(listen_socket);
            stop();
            return false;
        }

        if (::listen(listen_socket, 8) != 0)
        {
            error = "listen() failed";
            closeSocket(listen_socket);
            stop();
            return false;
        }

        mListenSocket = fromSocket(listen_socket);
        mStopping     = false;
        try
        {
            mAcceptThread = std::thread([this]() { acceptLoop(); });
        }
        catch (const std::exception& e)
        {
            error = std::string("cannot start thread: ") + e.what();
            stop();
            return false;
        }

        mRunning = true;
        return true;
    }

    void HttpServer::stop()
    {
        mStopping = true;

        if (mAcceptThread.joinable())
            mAcceptThread.join();

        if (toSocket(mListenSocket) != INVALID_SOCKET_VALUE)
        {
            closeSocket(toSocket(mListenSocket));
            mListenSocket = fromSocket(INVALID_SOCKET_VALUE);
        }

        {
            // Unblock connection threads stuck in recv/send.
            std::lock_guard<std::mutex> lock(mConnectionsMutex);
            for (auto& connection : mConnections)
                ::shutdown(toSocket(connection->socket), SHUTDOWN_BOTH);
        }
        reapConnections(true);

#if defined(_WIN32)
        if (mSocketsInitialized)
        {
            ::WSACleanup();
            mSocketsInitialized = false;
        }
#endif
        mRunning = false;
    }

    void HttpServer::reapConnections(bool all)
    {
        std::list<std::unique_ptr<Connection>> finished;
        {
            std::lock_guard<std::mutex> lock(mConnectionsMutex);
            for (auto it = mConnections.begin(); it != mConnections.end();)
            {
                if (all || (*it)->done)
                {
                    finished.push_back(std::move(*it));
                    it = mConnections.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // The connection thread never closes its socket; we do, after the
        // join, so the handle cannot be reused while someone still uses it.
        for (auto& connection : finished)
        {
            if (connection->thread.joinable())
                connection->thread.join();
            closeSocket(toSocket(connection->socket));
        }
    }

    void HttpServer::acceptLoop()
    {
        const socket_t listen_socket = toSocket(mListenSocket);

        while (!mStopping)
        {
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(listen_socket, &read_set);
            timeval timeout{};
            timeout.tv_sec  = 0;
            timeout.tv_usec = 200 * 1000;

            const int ready = ::select(static_cast<int>(listen_socket) + 1, &read_set, nullptr, nullptr, &timeout);
            if (ready <= 0)
            {
                if (ready < 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                reapConnections(false);
                continue;
            }

            sockaddr_in peer{};
#if defined(_WIN32)
            int peer_length = sizeof(peer);
#else
            socklen_t peer_length = sizeof(peer);
#endif
            const socket_t client = ::accept(listen_socket, reinterpret_cast<sockaddr*>(&peer), &peer_length);
            if (client == INVALID_SOCKET_VALUE)
                continue;
            setNoInherit(client);

            // Only loopback peers (127.0.0.0/8). We listen on 127.0.0.1 so
            // anything else would be a stack oddity; refuse it anyway.
            if (peer.sin_family != AF_INET || (ntohl(peer.sin_addr.s_addr) >> 24) != 127)
            {
                closeSocket(client);
                continue;
            }

            reapConnections(false);

            bool too_many = false;
            {
                std::lock_guard<std::mutex> lock(mConnectionsMutex);
                too_many = mConnections.size() >= mConfig.maxConnections;
                if (!too_many)
                {
                    auto        connection = std::make_unique<Connection>();
                    Connection* raw        = connection.get();
                    raw->socket            = fromSocket(client);
                    try
                    {
                        raw->thread = std::thread([this, raw]() { serveConnection(raw); });
                        mConnections.push_back(std::move(connection));
                    }
                    catch (const std::exception&)
                    {
                        closeSocket(client);
                    }
                }
            }

            if (too_many)
            {
                // Closed at once, without reading what the client sends: this is the accept
                // thread, which stop() waits for. The short answer fits the empty send buffer.
                setTimeouts(client, 1000);
                sendAll(client, serializeResponse(httpError(503, -32000, "Too many connections")));
                ::shutdown(client, SHUTDOWN_SEND);
                closeSocket(client);
            }
        }
    }

    void HttpServer::serveConnection(Connection* connection)
    {
        const socket_t s = toSocket(connection->socket);
        try
        {
            handleConnection(s);
        }
        catch (const std::exception&)
        {
            // e.g. bad_alloc; never let an exception end the process from here
            sendAll(s, serializeResponse(httpError(500, -32603, "Internal error")));
            ::shutdown(s, SHUTDOWN_SEND);
        }
        connection->done = true;
    }

    void HttpServer::handleConnection(std::uintptr_t socket)
    {
        const socket_t s = toSocket(socket);
        setTimeouts(s, mConfig.ioTimeoutMs);

        HttpRequest  request;
        HttpResponse response;
        const bool   read = readRequest(s, mConfig, request, response);
        setTimeouts(s, mConfig.ioTimeoutMs); // readRequest() shortened the receive timeout
        if (!read)
        {
            if (response.status != 0)
                sendAll(s, serializeResponse(response));
            ::shutdown(s, SHUTDOWN_SEND);
            // The client may still be sending (e.g. an oversized body): read
            // a little so closing does not reset the connection under it.
            drainBeforeClose(s);
            return;
        }

        bool        has_origin = false;
        std::string origin     = request.header("origin", &has_origin);
        std::string path       = request.target.substr(0, request.target.find('?'));

        if (!isAllowedHost(request.header("host"), mConfig.port))
        {
            response = httpError(403, -32000, "Forbidden: Host must be 127.0.0.1:" + std::to_string(mConfig.port) +
                                                  " or localhost:" + std::to_string(mConfig.port));
        }
        else if (has_origin && !isAllowedOrigin(origin))
        {
            response = httpError(403, -32000, "Forbidden: Origin not allowed");
        }
        else if (!isAuthorized(request.header("authorization"), currentToken()))
        {
            response = httpError(401, -32001, "Unauthorized: send \"Authorization: Bearer <token>\"");
            response.extraHeaders.emplace_back("WWW-Authenticate", "Bearer realm=\"sl-viewer-poser\"");
        }
        else if (path != mConfig.path)
        {
            response = httpError(404, -32000, "Not found; the MCP endpoint is " + mConfig.path);
        }
        else if (request.method == "POST")
        {
            const std::string content_type = toLower(request.header("content-type"));
            if (content_type.rfind("application/json", 0) != 0)
            {
                response = httpError(415, -32000, "Content-Type must be application/json");
            }
            else
            {
                Dispatcher::Reply reply = mDispatcher->handlePost(request.body);
                response.status         = reply.status;
                response.body           = std::move(reply.body);
            }
        }
        else
        {
            // No server-initiated SSE stream and no sessions: only POST.
            response = httpError(405, -32000, "Method not allowed; use POST");
            response.extraHeaders.emplace_back("Allow", "POST");
        }

        // The whole request was read, so closing right away cannot reset it.
        sendAll(s, serializeResponse(response));
        ::shutdown(s, SHUTDOWN_SEND);
    }
}
