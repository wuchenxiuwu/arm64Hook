#ifndef ARM64_HOOK_HPP
#define ARM64_HOOK_HPP

#include <cstdint>
#include <string_view>
#include <vector>

namespace arm64_hook {

enum class Error {
    SUCCESS = 0,
    INVALID_ARG,
    TARGET_TOO_SHORT,
    TRAMPOLINE_ALLOC,
    RELOCATION_FAILED,
    MPROTECT_FAILED,
    TRAMPOLINE_MISMATCH,
    STATE_INVALID,
    INTERNAL,
    NOT_FOUND,
    ALREADY_EXISTS,
    NOT_INITIALIZED,
    BUFFER_TOO_SMALL,
};

constexpr std::string_view to_string(Error err) noexcept {
    using enum Error;
    switch (err) {
        case SUCCESS:              return "Success";
        case INVALID_ARG:          return "Invalid argument";
        case TARGET_TOO_SHORT:     return "Target function too short";
        case TRAMPOLINE_ALLOC:     return "Trampoline allocation failed";
        case RELOCATION_FAILED:    return "Instruction relocation failed";
        case MPROTECT_FAILED:      return "Memory protection change failed";
        case TRAMPOLINE_MISMATCH:  return "Trampoline pointer mismatch";
        case STATE_INVALID:        return "Invalid trampoline state";
        case INTERNAL:             return "Internal error";
        case NOT_FOUND:            return "Not found";
        case ALREADY_EXISTS:       return "Already exists";
        case NOT_INITIALIZED:      return "Not initialized";
        case BUFFER_TOO_SMALL:     return "Buffer too small";
        default:                   return "Unknown error";
    }
}

using TrampolinePtr = uint32_t*;

Error install(void* target, void* replacement, TrampolinePtr* trampoline_out,
                           std::vector<uint32_t>* orig_insns_out,
                           uint32_t* orig_insn_count_out) noexcept;
Error free_trampoline(void* target, uint32_t* trampoline) noexcept;
Error restore_target(void* target, const std::vector<uint32_t>& orig_insns,
                                   uint32_t orig_insn_count) noexcept;
bool init_log(const char* dir, const char* filename) noexcept;

using LogCallback = void (*)(int level, const char* message);
using OnHookInstalled = void (*)(void* target, void* replacement, void* trampoline);
using OnHookRemoved = void (*)(void* target);

struct GlobalConfig {
    int log_level = 3;
    LogCallback log_callback = nullptr;
};

using HookHandle = void*;
using HookIterator = void*;

Error initialize(const GlobalConfig* config) noexcept;
void shutdown() noexcept;

Error log_init(const char* dir, const char* filename) noexcept;
void log_set_level(int level) noexcept;
void log_set_callback(LogCallback cb) noexcept;

Error hook_install(void* target, void* replacement, void** trampoline,
                                 uint32_t flags, HookHandle* out_handle) noexcept;
Error hook_remove(HookHandle handle) noexcept;
Error hook_remove_by_target(void* target) noexcept;

size_t hook_count() noexcept;
HookIterator hook_iter_create() noexcept;
bool hook_iter_next(HookIterator iter, void** target, void** replacement, void** trampoline) noexcept;
void hook_iter_destroy(HookIterator iter) noexcept;
Error hook_find_trampoline(void* target, void** trampoline) noexcept;

Error hook_set_name(HookHandle handle, const char* name) noexcept;
Error hook_get_name(HookHandle handle, char* buffer, size_t buffer_size) noexcept;

Error hook_set_group_id(HookHandle handle, uint32_t group_id) noexcept;
Error hook_get_group_id(HookHandle handle, uint32_t* out_group_id) noexcept;

Error hook_set_priority(HookHandle handle, uint32_t priority) noexcept;
Error hook_get_priority(HookHandle handle, uint32_t* out_priority) noexcept;

Error hook_set_ext_flags(HookHandle handle, uint64_t ext_flags) noexcept;
Error hook_get_ext_flags(HookHandle handle, uint64_t* out_ext_flags) noexcept;

Error hook_set_enabled(HookHandle handle, bool enabled) noexcept;
Error hook_is_enabled(HookHandle handle, bool* out_enabled) noexcept;

Error hook_increment_call_count(HookHandle handle) noexcept;
Error hook_get_call_count(HookHandle handle, uint64_t* out_count) noexcept;

Error hook_set_user_data(HookHandle handle, void* user_data) noexcept;
void* hook_get_user_data(HookHandle handle) noexcept;

void register_install_callback(OnHookInstalled cb) noexcept;
void register_remove_callback(OnHookRemoved cb) noexcept;

} 

#endif