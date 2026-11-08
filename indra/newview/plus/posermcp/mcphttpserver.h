/**
 * @file mcphttpserver.h
 * @brief A minimal MCP server (Model Context Protocol, "Streamable HTTP"
 *        transport, JSON-RPC 2.0) that listens on the loopback interface only.
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

// This file and mcphttpserver.cpp only use the C++ standard library,
// boost::json and the OS socket API -- no viewer headers -- so the protocol
// layer can be built and tested on its own (tools/poser-mcp/harness in the
// sl-viewer repository).
//
// Threading: the server runs an accept thread plus one thread per connection
// (a few at most). Tool handlers are called on those connection threads; a
// handler that needs the viewer must hand the work to the main thread itself.
//
// Security (the endpoint can move the user's avatar and take screenshots):
//  - binds 127.0.0.1 only and drops connections whose peer is not loopback;
//  - requires "Host: 127.0.0.1:<port>" or "Host: localhost:<port>";
//  - rejects any Origin that is not http(s)://localhost or 127.0.0.1
//    (browsers send Origin; this blocks DNS rebinding from web pages);
//  - requires "Authorization: Bearer <token>" (constant-time compare);
//  - plain HTTP/1.1, one request per connection, bounded header/body sizes.

#ifndef PLUS_POSERMCP_MCPHTTPSERVER_H
#define PLUS_POSERMCP_MCPHTTPSERVER_H

#include <boost/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace plus::mcp
{
    // One item of a tool result ("content" array of a tools/call result).
    struct Content
    {
        enum class Type { Text, Image };

        Type        type = Type::Text;
        std::string text;      // Type::Text
        std::string data;      // Type::Image: raw bytes (base64-encoded on the wire)
        std::string mimeType;  // Type::Image, e.g. "image/png"

        static Content makeText(std::string text);
        static Content makeImage(std::string bytes, std::string mime_type);
    };

    struct ToolResult
    {
        std::vector<Content> content;
        bool                 isError = false;

        static ToolResult text(std::string text);
        static ToolResult error(std::string message);
        // Serializes value as compact JSON text content.
        static ToolResult json(const boost::json::value& value);
    };

    struct Tool
    {
        std::string          name;
        std::string          title;
        std::string          description;
        boost::json::object  inputSchema; // JSON Schema of the "arguments" object
        bool                 readOnly = false;
        std::function<ToolResult(const boost::json::object& arguments)> handler;
    };

    struct ServerInfo
    {
        std::string name;
        std::string version;
        std::string instructions; // returned by initialize, shown to the model
    };

    // JSON-RPC 2.0 / MCP message layer. No I/O; safe to call from several
    // threads at once (it is immutable after construction; the tool handlers
    // must be thread-safe themselves).
    class Dispatcher
    {
    public:
        Dispatcher(ServerInfo info, std::vector<Tool> tools);

        struct Reply
        {
            int         status = 200; // HTTP status
            std::string body;         // JSON, empty for 202
        };

        // Handles the body of one HTTP POST: a JSON-RPC request, notification
        // or response, or a batch (array) of them.
        Reply handlePost(const std::string& body) const;

        const std::vector<Tool>& tools() const { return mTools; }

    private:
        // Returns a response object, or null for a notification / response.
        boost::json::value handleMessage(const boost::json::value& message) const;
        boost::json::value handleRequest(const std::string& method, const boost::json::value& params) const;
        boost::json::object toolsList() const;
        boost::json::object toolsCall(const boost::json::value& params) const;

        ServerInfo        mInfo;
        std::vector<Tool> mTools;
    };

    struct HttpConfig
    {
        uint16_t    port = 0;
        std::string token;
        std::string path            = "/mcp";
        size_t      maxHeaderBytes  = 16 * 1024;
        size_t      maxBodyBytes    = 1024 * 1024;
        size_t      maxConnections  = 8;
        int         ioTimeoutMs     = 15000;  // per recv/send call
        int         requestTimeoutMs = 10000; // reading one whole request (headers and body)
    };

    // HTTP front end. start() binds 127.0.0.1:<port>; stop() closes the
    // listening socket and every open connection and joins all threads.
    class HttpServer
    {
    public:
        HttpServer(HttpConfig config, std::shared_ptr<const Dispatcher> dispatcher);
        ~HttpServer();

        HttpServer(const HttpServer&) = delete;
        HttpServer& operator=(const HttpServer&) = delete;

        // Returns false and a human-readable reason on failure (e.g. port in use).
        bool start(std::string& error);
        void stop();
        bool isRunning() const { return mRunning; }
        // After a failed start(): the port could not be bound (in use, or refused for
        // it). The other failures (Winsock, a socket, listen(), a thread) leave the
        // port to nobody else.
        bool bindFailed() const { return mBindFailed; }

        // Replaces the bearer token; takes effect for the next request.
        void setToken(const std::string& token);

        uint16_t port() const { return mConfig.port; }

    private:
        struct Connection;

        void acceptLoop();
        void serveConnection(Connection* connection);
        void handleConnection(std::uintptr_t socket);
        void reapConnections(bool all);
        std::string currentToken() const;

        HttpConfig                        mConfig;
        std::shared_ptr<const Dispatcher> mDispatcher;

        mutable std::mutex mTokenMutex;
        std::string        mToken;

        std::atomic<bool> mRunning{ false };
        std::atomic<bool> mStopping{ false };
        std::uintptr_t    mListenSocket;
        std::thread       mAcceptThread;
        bool              mSocketsInitialized = false;
        bool              mBindFailed         = false;

        std::mutex                               mConnectionsMutex;
        std::list<std::unique_ptr<Connection>>   mConnections;
    };

    // ---- helpers, public for tests ----

    // Random token from the OS CSPRNG (std::random_device: RtlGenRandom on
    // MSVC, /dev/urandom elsewhere), hex encoded.
    std::string generateToken(size_t bytes = 32);

    std::string base64Encode(const std::string& data);

    // Host header must name the loopback endpoint we listen on.
    bool isAllowedHost(const std::string& host, uint16_t port);

    // Origin header (if the client sent one) must be a local page.
    bool isAllowedOrigin(const std::string& origin);

    // "Bearer <token>", constant-time comparison.
    bool isAuthorized(const std::string& authorization, const std::string& token);

    // MCP protocol versions this server speaks, newest first.
    const std::vector<std::string>& supportedProtocolVersions();
}

#endif // PLUS_POSERMCP_MCPHTTPSERVER_H
