#ifndef VM_MANAGER_H
#define VM_MANAGER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace runtime { class ManageVM; }

namespace controller {

    struct VMConfig {
        std::string  program_path;
        std::string  program_args;
        uint32_t     num_schedulers = 1;
        bool         enable_jit    = false;
        bool         enable_dist   = false;
    };

    struct VMInstance {
        uint32_t     id = 0;
        std::string  status;   // "running", "paused", "stopped"
        uint32_t     schedulers = 0;
        uint64_t     uptime_ms = 0;
    };

    class VMManager {
    public:
        VMManager();
        ~VMManager();

        VMManager(const VMManager &) = delete;
        VMManager &operator=(const VMManager &) = delete;

        uint32_t create_vm(const VMConfig &cfg);
        bool     destroy_vm(uint32_t vm_id);
        bool     start_vm(uint32_t vm_id);
        bool     stop_vm(uint32_t vm_id);
        bool     pause_vm(uint32_t vm_id);
        bool     resume_vm(uint32_t vm_id);

        VMInstance   get_vm_info(uint32_t vm_id) const;
        std::vector<VMInstance> list_vms() const;

        bool     load_bytecode(uint32_t vm_id, const std::string &path);
        bool     eval_expression(uint32_t vm_id, const std::string &expr);

        uint32_t vm_count() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace controller

#endif
