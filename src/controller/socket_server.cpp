#include "controller/socket_server.h"
#include "controller/request_router.h"

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket(fd) ::close(fd)
#endif

namespace controller {

    struct SocketServer::Impl {
        uint16_t       port;
        RequestRouter &router;
        SOCKET         listen_fd = INVALID_SOCKET;
        std::atomic<bool> running{false};
        std::thread    accept_thread;

        static constexpr size_t BUF_SIZE = 65536;

        Impl(uint16_t p, RequestRouter &r) : port(p), router(r) {}

        void accept_loop() {
            char buf[BUF_SIZE];
            while (running) {
                struct sockaddr_in client_addr;
                socklen_t addr_len = sizeof(client_addr);
                SOCKET client = ::accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
                if (client == INVALID_SOCKET) {
                    if (running) {
                        // Error aceptable si estamos deteniendo
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    continue;
                }

                // Leer mensaje
                ssize_t n = ::recv(client, buf, BUF_SIZE - 1, 0);
                if (n > 0) {
                    buf[n] = '\0';
                    std::string_view request(buf, static_cast<size_t>(n));
                    Response resp = router.handle(request);
                    std::string response = resp.ok
                        ? "OK " + resp.data + "\n"
                        : "ERR " + resp.error + "\n";
                    ::send(client, response.data(), response.size(), 0);
                }
                closesocket(client);
            }
        }
    };

    SocketServer::SocketServer(uint16_t port, RequestRouter &router)
        : impl_(std::make_unique<Impl>(port, router)) {}

    SocketServer::~SocketServer() { stop(); }

    bool SocketServer::start() {
        if (impl_->running) return true;

#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif

        impl_->listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (impl_->listen_fd == INVALID_SOCKET) return false;

        int opt = 1;
        ::setsockopt(impl_->listen_fd, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char *>(&opt), sizeof(opt));

        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(impl_->port);

        if (::bind(impl_->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
            closesocket(impl_->listen_fd);
            impl_->listen_fd = INVALID_SOCKET;
            return false;
        }

        if (::listen(impl_->listen_fd, 5) == SOCKET_ERROR) {
            closesocket(impl_->listen_fd);
            impl_->listen_fd = INVALID_SOCKET;
            return false;
        }

        impl_->running = true;
        impl_->accept_thread = std::thread(&Impl::accept_loop, impl_.get());
        return true;
    }

    void SocketServer::stop() {
        impl_->running = false;
        if (impl_->listen_fd != INVALID_SOCKET) {
            closesocket(impl_->listen_fd);
            impl_->listen_fd = INVALID_SOCKET;
        }
        if (impl_->accept_thread.joinable()) {
            impl_->accept_thread.join();
        }
#ifdef _WIN32
        WSACleanup();
#endif
    }

    bool SocketServer::is_running() const { return impl_->running; }

    uint16_t SocketServer::port() const { return impl_->port; }

} // namespace controller
