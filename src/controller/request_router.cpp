#include "controller/request_router.h"
#include "controller/vm_manager.h"

#include <charconv>
#include <sstream>
#include <string_view>

namespace controller {

    struct RequestRouter::Impl {
        VMManager &mgr;
        explicit Impl(VMManager &mgr_) : mgr(mgr_) {}
    };

    RequestRouter::RequestRouter(VMManager &mgr)
        : impl_(std::make_unique<Impl>(mgr)) {}

    RequestRouter::~RequestRouter() = default;

    bool RequestRouter::parse_request(const std::string_view &raw, Request &out) const {
        // Formato simple: "METHOD [params]"
        size_t space = raw.find(' ');
        if (space == std::string_view::npos) {
            out.method = std::string(raw);
            out.params.clear();
        } else {
            out.method = std::string(raw.substr(0, space));
            out.params = std::string(raw.substr(space + 1));
        }
        out.id = 0;
        return !out.method.empty();
    }

    Response RequestRouter::dispatch_create(const Request &req) {
        Response r;
        r.id = req.id;
        VMConfig cfg;
        cfg.num_schedulers = 1;
        cfg.program_path = req.params;
        uint32_t id = impl_->mgr.create_vm(cfg);
        r.ok = true;
        r.data = "vm_" + std::to_string(id);
        return r;
    }

    Response RequestRouter::dispatch_destroy(const Request &req) {
        Response r;
        r.id = req.id;
        uint32_t id = 0;
        std::from_chars(req.params.data(), req.params.data() + req.params.size(), id);
        r.ok = impl_->mgr.destroy_vm(id);
        if (!r.ok) r.error = "vm not found";
        return r;
    }

    Response RequestRouter::dispatch_start(const Request &req) {
        Response r;
        r.id = req.id;
        uint32_t id = 0;
        std::from_chars(req.params.data(), req.params.data() + req.params.size(), id);
        r.ok = impl_->mgr.start_vm(id);
        if (!r.ok) r.error = "vm not found";
        return r;
    }

    Response RequestRouter::dispatch_stop(const Request &req) {
        Response r;
        r.id = req.id;
        uint32_t id = 0;
        std::from_chars(req.params.data(), req.params.data() + req.params.size(), id);
        r.ok = impl_->mgr.stop_vm(id);
        if (!r.ok) r.error = "vm not found";
        return r;
    }

    Response RequestRouter::dispatch_list(const Request &req) {
        Response r;
        r.id = req.id;
        r.ok = true;
        auto vms = impl_->mgr.list_vms();
        std::ostringstream oss;
        for (const auto &vm : vms) {
            oss << vm.id << " " << vm.status << "\n";
        }
        r.data = oss.str();
        return r;
    }

    Response RequestRouter::dispatch_info(const Request &req) {
        Response r;
        r.id = req.id;
        uint32_t id = 0;
        std::from_chars(req.params.data(), req.params.data() + req.params.size(), id);
        VMInstance inst = impl_->mgr.get_vm_info(id);
        r.ok = inst.status != "not_found";
        if (r.ok) {
            r.data = "vm_" + std::to_string(inst.id) + " " + inst.status;
        } else {
            r.error = "vm not found";
        }
        return r;
    }

    static uint32_t parse_vm_id(const std::string &params, size_t end) {
        uint32_t id = 0;
        std::from_chars(params.data(), params.data() + end, id);
        return id;
    }

    Response RequestRouter::dispatch_eval(const Request &req) {
        Response r;
        r.id = req.id;
        size_t space = req.params.find(' ');
        if (space == std::string::npos) { r.ok = false; r.error = "need vm_id + expr"; return r; }
        uint32_t id = parse_vm_id(req.params, space);
        std::string expr = req.params.substr(space + 1);
        r.ok = impl_->mgr.eval_expression(id, expr);
        if (!r.ok) r.error = "eval failed";
        return r;
    }

    Response RequestRouter::dispatch_load(const Request &req) {
        Response r;
        r.id = req.id;
        size_t space = req.params.find(' ');
        if (space == std::string::npos) { r.ok = false; r.error = "need vm_id + path"; return r; }
        uint32_t id = parse_vm_id(req.params, space);
        std::string path = req.params.substr(space + 1);
        r.ok = impl_->mgr.load_bytecode(id, path);
        if (!r.ok) r.error = "load failed";
        return r;
    }

    Response RequestRouter::dispatch(const Request &req) {
        if (req.method == "create")  return dispatch_create(req);
        if (req.method == "destroy") return dispatch_destroy(req);
        if (req.method == "start")   return dispatch_start(req);
        if (req.method == "stop")    return dispatch_stop(req);
        if (req.method == "list")    return dispatch_list(req);
        if (req.method == "info")    return dispatch_info(req);
        if (req.method == "eval")    return dispatch_eval(req);
        if (req.method == "load")    return dispatch_load(req);

        Response r;
        r.id = req.id;
        r.ok = false;
        r.error = "unknown method: " + req.method;
        return r;
    }

    Response RequestRouter::handle(const std::string_view &raw) {
        Request req;
        if (!parse_request(raw, req)) {
            Response r;
            r.ok = false;
            r.error = "parse error";
            return r;
        }
        return dispatch(req);
    }

} // namespace controller
