#ifndef REQUEST_ROUTER_H
#define REQUEST_ROUTER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace controller { class VMManager; }

namespace controller {

    struct Request {
        std::string method;
        std::string params;
        uint64_t    id = 0;
    };

    struct Response {
        uint64_t    id = 0;
        bool        ok = false;
        std::string data;
        std::string error;
    };

    class RequestRouter {
    public:
        explicit RequestRouter(VMManager &mgr);
        ~RequestRouter();

        RequestRouter(const RequestRouter &) = delete;
        RequestRouter &operator=(const RequestRouter &) = delete;

        bool parse_request(const std::string_view &raw, Request &out) const;
        Response dispatch(const Request &req);

        // Conveniencia: parse + dispatch en un solo paso
        Response handle(const std::string_view &raw);

    private:
        Response dispatch_create(const Request &req);
        Response dispatch_destroy(const Request &req);
        Response dispatch_start(const Request &req);
        Response dispatch_stop(const Request &req);
        Response dispatch_list(const Request &req);
        Response dispatch_info(const Request &req);
        Response dispatch_eval(const Request &req);
        Response dispatch_load(const Request &req);

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace controller

#endif
