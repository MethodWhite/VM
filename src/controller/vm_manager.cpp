#include "controller/vm_manager.h"
#include "runtime/manager_runtime.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <unordered_map>

namespace controller {

    struct VMManager::Impl {
        std::unordered_map<uint32_t, runtime::ManageVM *> vms;
        uint32_t next_id = 1;
    };

    VMManager::VMManager() : impl_(std::make_unique<Impl>()) {}

    VMManager::~VMManager() = default;

    uint32_t VMManager::create_vm(const VMConfig &cfg) {
        (void)cfg;
        uint32_t id = impl_->next_id++;
        impl_->vms[id] = nullptr;
        return id;
    }

    bool VMManager::destroy_vm(uint32_t vm_id) {
        return impl_->vms.erase(vm_id) > 0;
    }

    bool VMManager::start_vm(uint32_t vm_id) {
        auto it = impl_->vms.find(vm_id);
        return it != impl_->vms.end();
    }

    bool VMManager::stop_vm(uint32_t vm_id) {
        (void)vm_id;
        return true;
    }

    bool VMManager::pause_vm(uint32_t vm_id) {
        (void)vm_id;
        return true;
    }

    bool VMManager::resume_vm(uint32_t vm_id) {
        (void)vm_id;
        return true;
    }

    VMInstance VMManager::get_vm_info(uint32_t vm_id) const {
        VMInstance inst;
        inst.id = vm_id;
        inst.status = impl_->vms.count(vm_id) ? "created" : "not_found";
        return inst;
    }

    std::vector<VMInstance> VMManager::list_vms() const {
        std::vector<VMInstance> result;
        for (const auto &[id, _] : impl_->vms) {
            (void)_;
            VMInstance inst;
            inst.id = id;
            inst.status = "created";
            result.push_back(inst);
        }
        return result;
    }

    uint32_t VMManager::vm_count() const {
        return static_cast<uint32_t>(impl_->vms.size());
    }

    bool VMManager::load_bytecode(uint32_t vm_id, const std::string &path) {
        (void)vm_id;
        (void)path;
        return true;
    }

    bool VMManager::eval_expression(uint32_t vm_id, const std::string &expr) {
        (void)vm_id;
        (void)expr;
        return true;
    }

} // namespace controller
