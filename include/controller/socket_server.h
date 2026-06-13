#ifndef SOCKET_SERVER_H
#define SOCKET_SERVER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace controller { class RequestRouter; }

namespace controller {

    using MessageHandler = std::function<std::string(const std::string &)>;

    class SocketServer {
    public:
        SocketServer(uint16_t port, RequestRouter &router);
        ~SocketServer();

        SocketServer(const SocketServer &) = delete;
        SocketServer &operator=(const SocketServer &) = delete;

        bool start();
        void stop();
        bool is_running() const;

        uint16_t port() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace controller

#endif
