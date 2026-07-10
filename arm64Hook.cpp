#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <map>
#include <vector>
#include <condition_variable>
#include <span>
#include <string_view>
#include <string>
#include <algorithm>
#include <array>
#include <thread>
#include <functional>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if !defined(__aarch64__) && !defined(FORCE_COMPILE)
#error "This library only supports ARM64!"
#endif

namespace {

enum class LogLevel {
    DEBUG = 0,
    INFO  = 1,
    WARN  = 2,
    ERROR = 3,
    FATAL = 4
};

class Logger {
public:
    static Logger& instance() {
        static Logger logger;
        return logger;
    }
    
    bool init(const std::string& dir, const std::string& filename = "hook.log",
              size_t max_size = 10 * 1024 * 1024) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_) {
            fclose(file_);
            file_ = nullptr;
        }

        if (!create_directories(dir)) {
            return false;
        }

        std::string full_path = dir;
        if (!full_path.empty() && full_path.back() != '/') full_path += '/';
        full_path += filename;

        file_ = fopen(full_path.c_str(), "a");
        if (!file_) return false;

        path_ = full_path;
        max_size_ = max_size;
        initialized_ = true;
        return true;
    }

    void log(LogLevel level, const char* file, int line, const char* func, const char* fmt, ...) {
        if (level < min_level_) return;

        std::lock_guard<std::mutex> lock(mutex_);

        char prefix[64];
        time_t now = time(nullptr);
        struct tm* tm_info = localtime(&now);
        strftime(prefix, sizeof(prefix), "%Y-%m-%d %H:%M:%S", tm_info);

        std::thread::id tid = std::this_thread::get_id();
        size_t tid_hash = std::hash<std::thread::id>{}(tid) & 0xffff;

        char msg[2048];
        va_list args;
        va_start(args, fmt);
        int len = vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);
        if (len < 0) {
            msg[0] = '\0';
        } else if ((size_t)len >= sizeof(msg)) {
            msg[sizeof(msg) - 4] = '.';
            msg[sizeof(msg) - 3] = '.';
            msg[sizeof(msg) - 2] = '.';
            msg[sizeof(msg) - 1] = '\0';
        }

        char line_buf[3072];  
        int line_len = snprintf(line_buf, sizeof(line_buf), "[%s][%s][%04zx][%s:%d %s] %s",
                 prefix,
                 level_to_string(level),
                 tid_hash,
                 file, line, func,
                 msg);
        if (line_len < 0 || (size_t)line_len >= sizeof(line_buf)) {
            snprintf(line_buf, sizeof(line_buf), "[LOG] message truncated or format error");
        }

        if (callback_) {
            callback_(static_cast<int>(level), line_buf);
        } else if (initialized_ && file_) {
            rotate_if_needed();
            fprintf(file_, "%s\n", line_buf);
            fflush(file_);
        }
    }

    void set_level(LogLevel level) {
        std::lock_guard<std::mutex> lock(mutex_);
        min_level_ = level;
    }

    void set_callback(void (*cb)(int level, const char* message)) {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = cb;
    }

private:
    Logger() = default;
    ~Logger() { if (file_) fclose(file_); }

    void rotate_if_needed() {
        if (!file_ || max_size_ == 0) return;
        long pos = ftell(file_);
        if (pos >= static_cast<long>(max_size_)) {
            fclose(file_);
            std::string backup = path_ + ".old";
            if (rename(path_.c_str(), backup.c_str()) != 0) {
                std::string tmp_backup = path_ + ".old." + std::to_string(time(nullptr));
                if (rename(path_.c_str(), tmp_backup.c_str()) != 0) {
                    file_ = fopen(path_.c_str(), "w");
                    if (file_) {
                        fclose(file_);
                        file_ = fopen(path_.c_str(), "a");
                    }
                    return;
                }
            }
            file_ = fopen(path_.c_str(), "a");
        }
    }

    static bool create_directories(const std::string& path) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            return S_ISDIR(st.st_mode);
        }

        std::string current;
        std::vector<std::string> parts;
        size_t start = 0, end;
        while ((end = path.find('/', start)) != std::string::npos) {
            if (end != start) {
                parts.push_back(path.substr(start, end - start));
            }
            start = end + 1;
        }
        if (start < path.length()) {
            parts.push_back(path.substr(start));
        }

        current = (path[0] == '/') ? "/" : "";
        for (const auto& part : parts) {
            if (part.empty()) continue;
            if (!current.empty() && current.back() != '/') current += '/';
            current += part;

            if (stat(current.c_str(), &st) == 0) {
                if (!S_ISDIR(st.st_mode)) return false;
                continue;
            }

            if (mkdir(current.c_str(), 0755) != 0) {
                return false;
            }
        }
        return true;
    }

    static const char* level_to_string(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO";
            case LogLevel::WARN:  return "WARN";
            case LogLevel::ERROR: return "ERROR";
            case LogLevel::FATAL: return "FATAL";
            default: return "UNKNOWN";
        }
    }

private:
    FILE* file_ = nullptr;
    std::string path_;
    size_t max_size_ = 0;
    std::mutex mutex_;
    bool initialized_ = false;
    LogLevel min_level_ = LogLevel::INFO;
    void (*callback_)(int level, const char* message) = nullptr;
};

} 

#define LOG_ALWAYS(fmt, ...) \
    Logger::instance().log(LogLevel::INFO, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) \
    Logger::instance().log(LogLevel::ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#ifndef NDEBUG
#define LOG_DEBUG(fmt, ...) \
    Logger::instance().log(LogLevel::DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) ((void)0)
#endif

static constexpr size_t kPageSize = 4096;
static constexpr uint32_t kNopOp = 0xd503201f;
static constexpr uint32_t kBrX17  = 0xd61f0220;
static constexpr uint32_t kBlrX17 = 0xd63f0220;
static constexpr size_t kMaxOrigScan = 128;

static constexpr uint32_t kBOp       = 0x14000000;
static constexpr uint32_t kBlOp      = 0x94000000;
static constexpr uint32_t kBOpMask   = 0xfc000000;
static constexpr uint32_t kBCondOp   = 0x54000000;
static constexpr uint32_t kBCondMask = 0xff000010;
static constexpr uint32_t kCbzOp     = 0x34000000;
static constexpr uint32_t kCbnzOp    = 0x35000000;
static constexpr uint32_t kCbMask    = 0x7f000000;
static constexpr uint32_t kTbzOp     = 0x36000000;
static constexpr uint32_t kTbnzOp    = 0x37000000;
static constexpr uint32_t kTbMask    = 0x7f000000;
static constexpr uint32_t kLdrLitOp  = 0x18000000;
static constexpr uint32_t kLdrLitMask = 0xbf000000;
static constexpr uint32_t kLdrswLitOp = 0x98000000;
static constexpr uint32_t kLdrswMask = 0xff000000;
static constexpr uint32_t kPrfmLitOp = 0xd8000000;
static constexpr uint32_t kLdrVecLitOp = 0x1c000000;
static constexpr uint32_t kLdrVecMask = 0x3f000000;
static constexpr uint32_t kAdrOp     = 0x10000000;
static constexpr uint32_t kAdrpOp    = 0x90000000;
static constexpr uint32_t kAdrMask   = 0x9f000000;

namespace arm64_hook {

using HookHandle = void*;
using HookIterator = void*;

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

} 

constexpr uint64_t MakeMask(int high, int low) noexcept {
    if (high < low || high < 0 || low < 0 || high >= 64) {
        return 0;
    }
    return ((1ULL << (high - low + 1)) - 1) << low;
}

constexpr uint32_t ExtractBits(uint32_t value, int high, int low) noexcept {
    if (high < low || high < 0 || low < 0 || high >= 32) {
        return 0;
    }
    return (value & MakeMask(high, low)) >> low;
}

constexpr int64_t SignExtend(uint64_t value, int bits) noexcept {
    if (bits <= 0 || bits > 64) {
        return 0;
    }
    return static_cast<int64_t>(value << (64 - bits)) >> (64 - bits);
}

static uint32_t MakeLdrLiteral(int rt, int byte_offset) noexcept {
    uint32_t imm19 = (static_cast<uint32_t>(byte_offset) >> 2) & 0x7ffff;
    return 0x58000000 | (imm19 << 5) | rt;
}

static bool SetPageRWX(void* addr, size_t size) noexcept {
    uintptr_t start = reinterpret_cast<uintptr_t>(addr);
    uintptr_t end = start + size;
    uintptr_t page_start = start & ~(kPageSize - 1);
    uintptr_t page_end = (end + kPageSize - 1) & ~(kPageSize - 1);
    return mprotect(reinterpret_cast<void*>(page_start), page_end - page_start,
                    PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}

static bool SetPageRX(void* addr, size_t size) noexcept {
    uintptr_t start = reinterpret_cast<uintptr_t>(addr);
    uintptr_t end = start + size;
    uintptr_t page_start = start & ~(kPageSize - 1);
    uintptr_t page_end = (end + kPageSize - 1) & ~(kPageSize - 1);
    return mprotect(reinterpret_cast<void*>(page_start), page_end - page_start,
                    PROT_READ | PROT_EXEC) == 0;
}

class MappedMemory {
public:
    MappedMemory() = default;
    ~MappedMemory() { Reset(); }

    MappedMemory(const MappedMemory&) = delete;
    MappedMemory& operator=(const MappedMemory&) = delete;

    MappedMemory(MappedMemory&& other) noexcept
        : ptr_(std::exchange(other.ptr_, nullptr)), size_(other.size_) {}

    MappedMemory& operator=(MappedMemory&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr_ = std::exchange(other.ptr_, nullptr);
            size_ = other.size_;
        }
        return *this;
    }

    bool Allocate(size_t min_size) noexcept {
        Reset();
        size_t page_count = (min_size + kPageSize - 1) / kPageSize;
        size_ = page_count * kPageSize;
        void* p = mmap(nullptr, size_, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            size_ = 0;
            return false;
        }
        ptr_ = static_cast<uint32_t*>(p);
        return true;
    }

    void Reset() noexcept {
        if (ptr_) {
            munmap(ptr_, size_);
            ptr_ = nullptr;
            size_ = 0;
        }
    }

    uint32_t* get() const noexcept { return ptr_; }
    size_t size() const noexcept { return size_; }

private:
    uint32_t* ptr_ = nullptr;
    size_t size_ = 0;
};

enum class FixupType : uint8_t {
    B_BL,
    COND_B,
    CBZ,
    TBZ,
    LDR_LIT,
    ADR,
    ADRP,
};

struct FixupEntry {
    uint32_t* fix_addr;
    int64_t target_orig_pc;
    FixupType type;
};

class RelocationContext {
public:
    RelocationContext(std::span<uint32_t> orig_insns,
                      std::span<uint32_t> trampoline) noexcept
        : orig_span_(orig_insns), trampoline_span_(trampoline) {}

    bool ProcessNextInstruction() noexcept {
        if (orig_index_ >= kMaxOrigScan) {
            LOG_ERROR("Original instruction scan exceeded limit");
            return false;
        }

        uint32_t insn = orig_span_[orig_index_];
        uint32_t* orig_ptr = orig_span_.data() + orig_index_;
        uint32_t* new_ptr = trampoline_cur_;

        auto new_insns = ProcessOne(insn, orig_ptr, new_ptr);
        if (new_insns.empty()) {
            LOG_ERROR("Relocation failed, instruction at %p", orig_ptr);
            return false;
        }

        if (trampoline_cur_ - trampoline_span_.data() + new_insns.size() > trampoline_span_.size()) {
            LOG_ERROR("Trampoline space insufficient");
            return false;
        }

        orig_to_new_[reinterpret_cast<intptr_t>(orig_ptr)] = new_ptr;

        std::memcpy(trampoline_cur_, new_insns.data(), new_insns.size() * sizeof(uint32_t));
        trampoline_cur_ += new_insns.size();

        orig_insns_.push_back(insn);
        orig_index_++;
        return true;
    }

    size_t GetOrigProcessedCount() const noexcept { return orig_index_; }
    size_t GetTrampolineInsnCount() const noexcept { return trampoline_cur_ - trampoline_span_.data(); }
    uint32_t* GetTrampolineEnd() const noexcept { return trampoline_cur_; }
    const std::vector<uint32_t>& GetOriginalInstructions() const noexcept { return orig_insns_; }

    bool LastInsnIsControlTransfer() const noexcept {
        if (orig_insns_.empty()) return false;
        uint32_t last = orig_insns_.back();
        if ((last & 0xfffffc1f) == 0xd65f0000) return true;
        if ((last & kBOpMask) == kBOp) return true;
        if ((last & 0xfffffc00) == 0xd61f0000) return true;
        if ((last & 0xfffffc00) == 0xd63f0000) return true;
        return false;
    }

    void ApplyFixups() noexcept {
        for (const auto& fix : fixups_) {
            uint32_t* fix_addr = fix.fix_addr;
            int64_t target_orig = fix.target_orig_pc;
            FixupType type = fix.type;

            int64_t target_new;
            auto it = orig_to_new_.find(target_orig);
            if (it != orig_to_new_.end()) {
                target_new = reinterpret_cast<int64_t>(it->second);
            } else {
                LOG_ERROR("ApplyFixups: cannot find mapping for target address %p", reinterpret_cast<void*>(target_orig));
                target_new = target_orig;
            }

            int64_t fix_pc = reinterpret_cast<int64_t>(fix_addr);
            int64_t offset_insns = (target_new - fix_pc) >> 2;

            uint32_t old = *fix_addr;
            uint32_t new_insn = old;

            switch (type) {
                case FixupType::B_BL:
                    new_insn = (old & 0xfc000000) | (offset_insns & 0x03ffffff);
                    break;
                case FixupType::COND_B:
                case FixupType::CBZ:
                case FixupType::LDR_LIT:
                    new_insn = (old & 0xff00001f) | ((offset_insns << 5) & 0x00ffffe0);
                    break;
                case FixupType::TBZ:
                    new_insn = (old & 0xfff8001f) | ((offset_insns << 5) & 0x003fffff);
                    break;
                case FixupType::ADR: {
                    int64_t offset_bytes = target_new - fix_pc;
                    uint32_t imm = static_cast<uint32_t>(offset_bytes & 0x1fffff);
                    uint32_t immlo = imm & 3;
                    uint32_t immhi = (imm >> 2) & 0x7ffff;
                    new_insn = (old & 0x9f00001f)
                             | (immhi << 5)
                             | (immlo << 29);
                    break;
                }
                case FixupType::ADRP: {
                    int64_t fix_page = fix_pc & ~0xfff;
                    int64_t target_page = target_new & ~0xfff;
                    int64_t page_delta = (target_page - fix_page) >> 12;
                    uint32_t imm = static_cast<uint32_t>(page_delta & 0x1fffff);
                    uint32_t immlo = imm & 3;
                    uint32_t immhi = (imm >> 2) & 0x7ffff;
                    new_insn = (old & 0x9f00001f)
                             | (immhi << 5)
                             | (immlo << 29);
                    break;
                }
                default:
                    LOG_ERROR("Unknown fixup type %d", static_cast<int>(type));
                    continue;
            }
            *fix_addr = new_insn;
        }
    }

private:
    std::span<uint32_t> orig_span_;
    std::vector<uint32_t> orig_insns_;
    size_t orig_index_ = 0;

    std::span<uint32_t> trampoline_span_;
    uint32_t* trampoline_cur_ = trampoline_span_.data();

    std::map<intptr_t, uint32_t*> orig_to_new_;
    std::vector<FixupEntry> fixups_;

    bool IsInOrigRange(int64_t addr) const noexcept {
        intptr_t start = reinterpret_cast<intptr_t>(orig_span_.data());
        intptr_t end = start + kMaxOrigScan * 4;
        return addr >= start && addr < end;
    }

    void AddFixup(uint32_t* fix_addr, int64_t target_orig_pc, FixupType type) noexcept {
        fixups_.push_back({fix_addr, target_orig_pc, type});
    }

    static int64_t ExtractImm26(uint32_t insn) noexcept {
        return SignExtend(ExtractBits(insn, 25, 0), 26) << 2;
    }
    static int64_t ExtractImm19(uint32_t insn) noexcept {
        return SignExtend(ExtractBits(insn, 23, 5), 19) << 2;
    }
    static int64_t ExtractImm14(uint32_t insn) noexcept {
        return SignExtend(ExtractBits(insn, 18, 5), 14) << 2;
    }
    static int64_t ExtractAdrImm(uint32_t insn) noexcept {
        uint32_t low = ExtractBits(insn, 30, 29);
        uint32_t high = ExtractBits(insn, 23, 5);
        return SignExtend((high << 2) | low, 21);
    }

    static void AlignToBoundary(std::vector<uint32_t>& out, uint32_t* cur_pc, size_t boundary) noexcept {
        uintptr_t addr = reinterpret_cast<uintptr_t>(cur_pc + out.size());
        size_t mask = boundary - 1;
        if ((addr & mask) != 0) {
            size_t need = (boundary - (addr & mask)) / 4;
            out.insert(out.end(), need, kNopOp);
        }
    }

    std::vector<uint32_t> EmitAbsoluteBranch(int64_t target, bool is_bl,
                                                           uint32_t* new_pc) noexcept {
        std::vector<uint32_t> result;
        AlignToBoundary(result, new_pc, 8);

        if (is_bl) {
            uint32_t adr_insn = kAdrOp | (30 << 0);
            uint32_t imm = result.size() * 4 + 16;
            adr_insn |= (ExtractBits(imm, 20, 2) << 5) | (ExtractBits(imm, 1, 0) << 29);
            result.push_back(adr_insn);
        }

        result.push_back(MakeLdrLiteral(17, 8));
        result.push_back(is_bl ? kBlrX17 : kBrX17);
        result.push_back(static_cast<uint32_t>(target & 0xffffffff));
        result.push_back(static_cast<uint32_t>((target >> 32) & 0xffffffff));
        return result;
    }

    std::vector<uint32_t> EmitAbsoluteCondBranch(int64_t target, uint32_t cond_insn,
                                                                uint32_t* new_pc) noexcept {
        std::vector<uint32_t> result;
        uint32_t cond = ExtractBits(cond_insn, 3, 0);
        uint32_t cond_op = kBCondOp | (cond << 0);

        size_t cond_pos = result.size();
        result.push_back(0);

        auto abs_seq = EmitAbsoluteBranch(target, false, new_pc + result.size() + 1);
        size_t abs_len = abs_seq.size();

        cond_op |= (abs_len << 5) & 0x00ffffe0;
        result[cond_pos] = cond_op;
        result.insert(result.end(), abs_seq.begin(), abs_seq.end());
        return result;
    }

    std::vector<uint32_t> EmitCmpBranch(int64_t target, uint32_t insn,
                                                       uint32_t* new_pc) noexcept {
        std::vector<uint32_t> result;
        uint32_t rt = ExtractBits(insn, 4, 0);
        bool is_cbz = ((insn & kCbMask) == kCbzOp);

        result.push_back(0xf100001f | (rt << 5));

        uint32_t cond_op = is_cbz ? (kBCondOp | 0) : (kBCondOp | 1);

        size_t cond_pos = result.size();
        result.push_back(0);

        auto abs_seq = EmitAbsoluteBranch(target, false, new_pc + result.size() + 1);
        size_t abs_len = abs_seq.size();

        cond_op |= (abs_len << 5) & 0x00ffffe0;
        result[cond_pos] = cond_op;
        result.insert(result.end(), abs_seq.begin(), abs_seq.end());
        return result;
    }

    std::vector<uint32_t> EmitTestBranch(int64_t target, uint32_t insn,
                                                        uint32_t* new_pc) noexcept {
        std::vector<uint32_t> result;
        uint32_t rt = ExtractBits(insn, 4, 0);
        uint32_t bit_pos = ExtractBits(insn, 23, 19);
        bool is_tbz = ((insn & kTbMask) == kTbzOp);

        uint32_t cond_op = is_tbz ? kTbzOp : kTbnzOp;
        cond_op |= (bit_pos << 19) | (rt << 0);

        size_t cond_pos = result.size();
        result.push_back(0);

        auto abs_seq = EmitAbsoluteBranch(target, false, new_pc + result.size() + 1);
        size_t abs_len = abs_seq.size();

        cond_op |= (abs_len << 5) & 0x00ffffe0;
        result[cond_pos] = cond_op;
        result.insert(result.end(), abs_seq.begin(), abs_seq.end());
        return result;
    }

    std::vector<uint32_t> EmitLiteralLoad(uint32_t ld_insn, int64_t target,
                                                         uint32_t data_size,
                                                         uint32_t* new_pc) noexcept {
        std::vector<uint32_t> result;
        AlignToBoundary(result, new_pc, data_size);

        uint32_t rt = ExtractBits(ld_insn, 4, 0);
        uint32_t ldr_new = MakeLdrLiteral(rt, 8);

        result.push_back(ldr_new);
        result.push_back(kBOp | ((data_size / 4) & 0x3ffffff));

        const uint8_t* src = reinterpret_cast<const uint8_t*>(&target);
        for (uint32_t i = 0; i < data_size; i += 4) {
            uint32_t word;
            std::memcpy(&word, src + i, 4);
            result.push_back(word);
        }
        return result;
    }

    std::vector<uint32_t> EmitPrfmLiteral(int64_t target, uint32_t prfop,
                                                         uint32_t* new_pc) noexcept {
        std::vector<uint32_t> result;
        AlignToBoundary(result, new_pc, 8);

        result.push_back(MakeLdrLiteral(17, 8));
        uint32_t prfm_reg = 0xf8a00000 | (17 << 5) | prfop;
        result.push_back(prfm_reg);
        result.push_back(static_cast<uint32_t>(target & 0xffffffff));
        result.push_back(static_cast<uint32_t>((target >> 32) & 0xffffffff));
        return result;
    }

    std::vector<uint32_t> ProcessOne(uint32_t insn, uint32_t* orig_pc,
                                                    uint32_t* new_pc) noexcept {
        int64_t pc = reinterpret_cast<int64_t>(orig_pc);
        int64_t new_pc_val = reinterpret_cast<int64_t>(new_pc);

        if ((insn & kBOpMask) == kBOp || (insn & kBOpMask) == kBlOp) {
            int64_t target = pc + ExtractImm26(insn);
            bool is_bl = ((insn & kBOpMask) == kBlOp);
            if (IsInOrigRange(target)) {
                uint32_t placeholder = insn & 0xfc000000;
                AddFixup(new_pc, target, FixupType::B_BL);
                return {placeholder};
            } else {
                int64_t offset = target - new_pc_val;
                if (std::llabs(offset) < 0x200000000LL) {
                    uint32_t new_insn = (insn & kBOpMask) | ((offset >> 2) & 0x03ffffff);
                    return {new_insn};
                } else {
                    return EmitAbsoluteBranch(target, is_bl, new_pc);
                }
            }
        }

        if ((insn & kLdrLitMask) == kLdrLitOp) {
            int64_t target = pc + ExtractImm19(insn);
            uint32_t size = (insn & (1 << 30)) ? 8 : 4;
            if (IsInOrigRange(target)) {
                uint32_t placeholder = insn & 0xff00001f;
                AddFixup(new_pc, target, FixupType::LDR_LIT);
                return {placeholder};
            } else {
                int64_t offset = target - new_pc_val;
                if (std::llabs(offset) < 0x400000LL) {
                    uint32_t new_insn = (insn & 0xff00001f) | ((offset >> 2) << 5);
                    return {new_insn};
                } else {
                    return EmitLiteralLoad(insn, target, size, new_pc);
                }
            }
        }
        if ((insn & kLdrswMask) == kLdrswLitOp) {
            int64_t target = pc + ExtractImm19(insn);
            if (IsInOrigRange(target)) {
                uint32_t placeholder = insn & 0xff00001f;
                AddFixup(new_pc, target, FixupType::LDR_LIT);
                return {placeholder};
            } else {
                int64_t offset = target - new_pc_val;
                if (std::llabs(offset) < 0x400000LL) {
                    uint32_t new_insn = (insn & 0xff00001f) | ((offset >> 2) << 5);
                    return {new_insn};
                } else {
                    return EmitLiteralLoad(insn, target, 8, new_pc);
                }
            }
        }
        if ((insn & kLdrVecMask) == kLdrVecLitOp) {
            int64_t target = pc + ExtractImm19(insn);
            if (IsInOrigRange(target)) {
                uint32_t placeholder = insn & 0xff00001f;
                AddFixup(new_pc, target, FixupType::LDR_LIT);
                return {placeholder};
            } else {
                int64_t offset = target - new_pc_val;
                if (std::llabs(offset) < 0x400000LL) {
                    uint32_t new_insn = (insn & 0xff00001f) | ((offset >> 2) << 5);
                    return {new_insn};
                } else {
                    return EmitLiteralLoad(insn, target, 16, new_pc);
                }
            }
        }

        if ((insn & kBCondMask) == kBCondOp) {
            int64_t target = pc + ExtractImm19(insn);
            if (IsInOrigRange(target)) {
                uint32_t placeholder = insn & 0xff00001f;
                AddFixup(new_pc, target, FixupType::COND_B);
                return {placeholder};
            } else {
                int64_t offset = target - new_pc_val;
                if (std::llabs(offset) < 0x400000LL) {
                    uint32_t new_insn = (insn & 0xff00001f) | ((offset >> 2) << 5);
                    return {new_insn};
                } else {
                    return EmitAbsoluteCondBranch(target, insn, new_pc);
                }
            }
        }

        if ((insn & kCbMask) == kCbzOp || (insn & kCbMask) == kCbnzOp) {
            int64_t target = pc + ExtractImm19(insn);
            if (IsInOrigRange(target)) {
                uint32_t placeholder = insn & 0xff00001f;
                AddFixup(new_pc, target, FixupType::CBZ);
                return {placeholder};
            } else {
                int64_t offset = target - new_pc_val;
                if (std::llabs(offset) < 0x400000LL) {
                    uint32_t new_insn = (insn & 0xff00001f) | ((offset >> 2) << 5);
                    return {new_insn};
                } else {
                    return EmitCmpBranch(target, insn, new_pc);
                }
            }
        }

        if ((insn & kAdrMask) == kAdrOp) {
            int64_t imm = ExtractAdrImm(insn);
            int64_t target = pc + imm;
            if (IsInOrigRange(target)) {
                uint32_t placeholder = insn & 0x9f00001f;
                AddFixup(new_pc, target, FixupType::ADR);
                return {placeholder};
            } else {
                int64_t offset = target - new_pc_val;
                if (std::llabs(offset) < 0x100000LL) {
                    uint32_t new_imm = offset & 0x1fffff;
                    uint32_t new_insn = (insn & 0x9f00001f)
                                       | (ExtractBits(new_imm, 20, 2) << 5)
                                       | (ExtractBits(new_imm, 1, 0) << 29);
                    return {new_insn};
                } else {
                    return EmitLiteralLoad(insn, target, 8, new_pc);
                }
            }
        }

        if ((insn & kAdrMask) == kAdrpOp) {
            int64_t imm = ExtractAdrImm(insn);
            int64_t target_page = (pc & ~0xfff) + (imm << 12);
            if (IsInOrigRange(target_page)) {
                uint32_t placeholder = insn & 0x9f00001f;
                AddFixup(new_pc, target_page, FixupType::ADRP);
                return {placeholder};
            } else {
                int64_t new_pc_page = new_pc_val & ~0xfff;
                int64_t page_delta = (target_page - new_pc_page) >> 12;
                if (page_delta >= -0x100000LL && page_delta <= 0x100000LL) {
                    uint32_t new_imm = page_delta & 0x1fffff;
                    uint32_t new_insn = (insn & 0x9f00001f)
                                       | (ExtractBits(new_imm, 20, 2) << 5)
                                       | (ExtractBits(new_imm, 1, 0) << 29);
                    return {new_insn};
                } else {
                    return EmitLiteralLoad(insn, target_page, 8, new_pc);
                }
            }
        }

        if ((insn & kTbMask) == kTbzOp || (insn & kTbMask) == kTbnzOp) {
            int64_t target = pc + ExtractImm14(insn);
            if (IsInOrigRange(target)) {
                uint32_t placeholder = insn & 0xfff8001f;
                AddFixup(new_pc, target, FixupType::TBZ);
                return {placeholder};
            } else {
                int64_t offset = target - new_pc_val;
                if (std::llabs(offset) < 0x20000LL) {
                    uint32_t new_insn = (insn & 0xfff8001f) | ((offset >> 2) << 5);
                    return {new_insn};
                } else {
                    return EmitTestBranch(target, insn, new_pc);
                }
            }
        }

        if ((insn & 0xff000000) == kPrfmLitOp) {
            int64_t target = pc + ExtractImm19(insn);
            if (IsInOrigRange(target)) {
                uint32_t placeholder = insn & 0xff00001f;
                AddFixup(new_pc, target, FixupType::LDR_LIT);
                return {placeholder};
            } else {
                int64_t offset = target - new_pc_val;
                if (std::llabs(offset) < 0x400000LL) {
                    uint32_t new_insn = (insn & 0xff00001f) | ((offset >> 2) << 5);
                    return {new_insn};
                } else {
                    uint32_t prfop = ExtractBits(insn, 4, 0);
                    return EmitPrfmLiteral(target, prfop, new_pc);
                }
            }
        }

        return {insn};
    }
};

class DynamicTrampolineBuilder {
public:
    DynamicTrampolineBuilder(std::span<uint32_t> target, std::span<uint32_t> trampoline) noexcept
        : ctx_(target, trampoline) {}

    size_t Build(size_t min_coverage) noexcept {
        while (ctx_.GetOrigProcessedCount() < min_coverage) {
            if (!ctx_.ProcessNextInstruction()) {
                LOG_ERROR("Instruction processing failed, processed %zu", ctx_.GetOrigProcessedCount());
                return 0;
            }

            if (ctx_.LastInsnIsControlTransfer()) {
                LOG_DEBUG("Encountered control transfer instruction, stopping generation, processed %zu", ctx_.GetOrigProcessedCount());
                break;
            }

            if (ctx_.GetOrigProcessedCount() >= kMaxOrigScan) {
                LOG_ERROR("Instruction processing exceeded %zu, possible infinite loop", kMaxOrigScan);
                return 0;
            }
        }

        if (ctx_.GetOrigProcessedCount() < min_coverage) {
            LOG_ERROR("Cannot obtain enough instructions, need %zu, got %zu",
                      min_coverage, ctx_.GetOrigProcessedCount());
            return 0;
        }

        ctx_.ApplyFixups();
        return ctx_.GetTrampolineInsnCount();
    }

    uint32_t* GetTrampolineEnd() const noexcept { return ctx_.GetTrampolineEnd(); }
    size_t GetOrigProcessedCount() const noexcept { return ctx_.GetOrigProcessedCount(); }
    const std::vector<uint32_t>& GetOriginalInstructions() const noexcept { return ctx_.GetOriginalInstructions(); }

private:
    RelocationContext ctx_;
};

enum class TrampolineState {
    CONSTRUCTING,
    READY
};

struct TrampolineInfo {
    MappedMemory memory;
    unsigned int refcount = 0;
    TrampolineState state = TrampolineState::CONSTRUCTING;
    std::unique_ptr<std::condition_variable> cv;

    TrampolineInfo() : cv(std::make_unique<std::condition_variable>()) {}
    TrampolineInfo(TrampolineInfo&& other) noexcept
        : memory(std::move(other.memory)),
          refcount(other.refcount),
          state(other.state),
          cv(std::move(other.cv)) {}
    TrampolineInfo& operator=(TrampolineInfo&& other) noexcept {
        if (this != &other) {
            memory = std::move(other.memory);
            refcount = other.refcount;
            state = other.state;
            cv = std::move(other.cv);
        }
        return *this;
    }
};

static std::map<void*, TrampolineInfo> g_trampoline_map;
static std::mutex g_trampoline_mutex;

template<typename Builder>
static uint32_t* GetOrCreateTrampoline(void* target, size_t estimated_bytes,
                                                       Builder&& builder) noexcept {
    std::unique_lock lock(g_trampoline_mutex);

    while (true) {
        auto it = g_trampoline_map.find(target);
        if (it != g_trampoline_map.end()) {
            if (it->second.state == TrampolineState::READY) {
                it->second.refcount++;
                return it->second.memory.get();
            } else {
                it->second.cv->wait(lock);
                continue;
            }
        } else {
            TrampolineInfo info;
            if (!info.memory.Allocate(estimated_bytes)) {
                int err_no = errno;
                LOG_ERROR("mmap trampoline failed: %s", std::strerror(err_no));
                return nullptr;
            }

            auto [new_it, inserted] = g_trampoline_map.emplace(target, std::move(info));
            if (!inserted) {
                LOG_ERROR("map insertion failed");
                return nullptr;
            }

            bool success = builder(new_it->second.memory.get(), new_it->second.memory.size());
            if (!success) {
                g_trampoline_map.erase(target);
                return nullptr;
            }

            new_it->second.state = TrampolineState::READY;
            new_it->second.cv->notify_all();
            return new_it->second.memory.get();
        }
    }
}

namespace arm64_hook {

Error free_trampoline(void* target, uint32_t* trampoline) noexcept;

Error install(void* target, void* replacement, TrampolinePtr* trampoline_out,
                           std::vector<uint32_t>* orig_insns_out,
                           uint32_t* orig_insn_count_out) noexcept {
    if (!target || !replacement) {
        return Error::INVALID_ARG;
    }

    uint32_t* target_ptr = static_cast<uint32_t*>(target);
    bool need_trampoline = (trampoline_out != nullptr);

    int64_t pc_offset = (reinterpret_cast<int64_t>(replacement) -
                         reinterpret_cast<int64_t>(target_ptr)) >> 2;
    const int64_t kBRange = 0x02000000;
    int min_coverage;
    if (std::llabs(pc_offset) < kBRange) {
        min_coverage = 1;
    } else {
        min_coverage = 4;
        if ((reinterpret_cast<uintptr_t>(target_ptr + 2) & 7) != 0) {
            min_coverage = 5;
        }
    }

    std::vector<uint32_t> orig_saved;
    uint32_t* trampoline_ptr = nullptr;
    size_t actual_coverage = 0;

    if (need_trampoline) {
        size_t estimated_insns = min_coverage * 8 + 16;
        size_t estimated_bytes = estimated_insns * sizeof(uint32_t);

        auto builder = [&](uint32_t* tramp_ptr, size_t alloc_size) -> bool {
            size_t max_insns = alloc_size / sizeof(uint32_t);
            std::span<uint32_t> target_span(target_ptr, kMaxOrigScan);
            std::span<uint32_t> tramp_span(tramp_ptr, max_insns);
            DynamicTrampolineBuilder dyn_builder(target_span, tramp_span);
            size_t generated = dyn_builder.Build(min_coverage);
            if (generated == 0) {
                LOG_ERROR("Dynamic trampoline generation failed");
                return false;
            }

            orig_saved = dyn_builder.GetOriginalInstructions();
            actual_coverage = orig_saved.size();

            uint32_t* after_tramp = dyn_builder.GetTrampolineEnd();
            uint32_t* rest_start = target_ptr + actual_coverage;
            int64_t rest_offset = reinterpret_cast<int64_t>(rest_start) -
                                  reinterpret_cast<int64_t>(after_tramp);
            int64_t rest_insn_offset = rest_offset >> 2;

            size_t used_insns = after_tramp - tramp_ptr;
            if (std::llabs(rest_insn_offset) < kBRange) {
                if (used_insns + 1 > max_insns) return false;
                *after_tramp++ = kBOp | (rest_insn_offset & 0x3ffffff);
            } else {
                while ((reinterpret_cast<uintptr_t>(after_tramp) & 7) != 0) {
                    if (used_insns + 1 > max_insns) return false;
                    *after_tramp++ = kNopOp;
                    used_insns++;
                }
                if (used_insns + 4 > max_insns) return false;
                *after_tramp++ = MakeLdrLiteral(17, 8);
                *after_tramp++ = kBrX17;
                std::memcpy(after_tramp, &rest_start, sizeof(int64_t));
                after_tramp += 2;
            }

            __sync_synchronize();
            __builtin___clear_cache(reinterpret_cast<char*>(tramp_ptr),
                                     reinterpret_cast<char*>(after_tramp));

            if (!SetPageRX(tramp_ptr, (after_tramp - tramp_ptr) * 4)) {
                LOG_ERROR("Cannot set trampoline to RX");
                return false;
            }
            return true;
        };

        trampoline_ptr = GetOrCreateTrampoline(target, estimated_bytes, builder);
        if (!trampoline_ptr) {
            LOG_ERROR("Trampoline acquisition failed");
            return Error::TRAMPOLINE_ALLOC;
        }
        *trampoline_out = trampoline_ptr;
    } else {
        std::array<uint32_t, 256> temp_buffer;
        std::span<uint32_t> target_span(target_ptr, kMaxOrigScan);
        std::span<uint32_t> temp_span(temp_buffer);
        DynamicTrampolineBuilder verifier(target_span, temp_span);
        size_t generated = verifier.Build(min_coverage);
        if (generated == 0) {
            LOG_ERROR("Target function cannot be safely overwritten");
            return Error::TARGET_TOO_SHORT;
        }
        orig_saved = verifier.GetOriginalInstructions();
        actual_coverage = orig_saved.size();
    }

    if (!SetPageRWX(target_ptr, actual_coverage * 4)) {
        LOG_ERROR("Cannot set target memory to RWX");
        if (need_trampoline && trampoline_ptr) {
            auto err = free_trampoline(target, trampoline_ptr);
            if (err != Error::SUCCESS) {
                LOG_ERROR("Cleanup trampoline failed: %s", to_string(err).data());
            }
        }
        return Error::MPROTECT_FAILED;
    }

    uint32_t* cur = target_ptr;
    if (min_coverage == 1) {
        *cur++ = kBOp | (pc_offset & 0x3ffffff);
    } else {
        if (min_coverage == 5) *cur++ = kNopOp;
        *cur++ = MakeLdrLiteral(17, 8);
        *cur++ = kBrX17;
        std::memcpy(cur, &replacement, sizeof(replacement));
        cur += 2;
    }

    std::fill(cur, target_ptr + actual_coverage, kNopOp);

    __sync_synchronize();
    __builtin___clear_cache(reinterpret_cast<char*>(target_ptr),
                             reinterpret_cast<char*>(target_ptr + actual_coverage));

    if (!SetPageRX(target_ptr, actual_coverage * 4)) {
        LOG_ERROR("Critical error: cannot restore target memory permissions, attempting rollback...");
        if (SetPageRWX(target_ptr, actual_coverage * 4)) {
            std::copy(orig_saved.begin(), orig_saved.end(), target_ptr);
            __sync_synchronize();
            __builtin___clear_cache(reinterpret_cast<char*>(target_ptr),
                                     reinterpret_cast<char*>(target_ptr + actual_coverage));
            if (!SetPageRX(target_ptr, actual_coverage * 4)) {
                LOG_ERROR("Still cannot restore memory permissions after rollback");
            }
        } else {
            LOG_ERROR("Cannot set RWX permissions for rollback");
        }
        if (need_trampoline && trampoline_ptr) {
            auto err = free_trampoline(target, trampoline_ptr);
            if (err != Error::SUCCESS) {
                LOG_ERROR("Cleanup trampoline failed: %s", to_string(err).data());
            }
        }
        return Error::MPROTECT_FAILED;
    }

    LOG_ALWAYS("Hook installed successfully: %p -> %p, trampoline %p, overwrote %zu instructions",
                target, replacement, trampoline_ptr, actual_coverage);

    if (orig_insns_out) {
        *orig_insns_out = orig_saved;
    }
    if (orig_insn_count_out) {
        *orig_insn_count_out = static_cast<uint32_t>(actual_coverage);
    }

    return Error::SUCCESS;
}

Error restore_target(void* target, const std::vector<uint32_t>& orig_insns,
                                   uint32_t orig_insn_count) noexcept {
    if (!target) return Error::INVALID_ARG;
    if (orig_insns.empty() || orig_insn_count == 0) return Error::INVALID_ARG;

    uint32_t* target_ptr = static_cast<uint32_t*>(target);

    if (!SetPageRWX(target_ptr, orig_insn_count * 4)) {
        LOG_ERROR("restore_target: cannot set target memory to RWX");
        return Error::MPROTECT_FAILED;
    }

    std::copy(orig_insns.begin(), orig_insns.begin() + orig_insn_count, target_ptr);

    __sync_synchronize();
    __builtin___clear_cache(reinterpret_cast<char*>(target_ptr),
                             reinterpret_cast<char*>(target_ptr + orig_insn_count));

    if (!SetPageRX(target_ptr, orig_insn_count * 4)) {
        LOG_ERROR("restore_target: cannot restore target memory to RX");
    }

    LOG_ALWAYS("restore_target: successfully restored %u original instructions at %p", orig_insn_count, target);
    return Error::SUCCESS;
}

Error free_trampoline(void* target, uint32_t* trampoline) noexcept {
    if (!target || !trampoline) return Error::INVALID_ARG;

    std::lock_guard lock(g_trampoline_mutex);

    auto it = g_trampoline_map.find(target);
    if (it == g_trampoline_map.end() || it->second.memory.get() != trampoline) {
        LOG_ERROR("Release failed: pointer mismatch");
        return Error::TRAMPOLINE_MISMATCH;
    }

    if (it->second.state != TrampolineState::READY) {
        LOG_ERROR("Release failed: invalid state");
        return Error::STATE_INVALID;
    }

    it->second.refcount--;
    LOG_DEBUG("Release reference: target %p, remaining %u", target, it->second.refcount);

    if (it->second.refcount == 0) {
        g_trampoline_map.erase(it);
        LOG_ALWAYS("Trampoline fully released: target %p", target);
    }
    return Error::SUCCESS;
}

bool init_log(const char* dir, const char* filename) noexcept {
    if (!dir || !filename) return false;
    return Logger::instance().init(dir, filename);
}

using LogCallback = void (*)(int level, const char* message);
using OnHookInstalled = void (*)(void* target, void* replacement, void* trampoline);
using OnHookRemoved = void (*)(void* target);

struct GlobalConfig {
    int log_level = 3;
    LogCallback log_callback = nullptr;
};

struct HookHandleImpl {
    void* target;
    void* replacement;
    uint32_t* trampoline;
    void* user_data;
    uint32_t flags;
    bool enabled;
    time_t install_time;
    uint64_t call_count;
    char name[64];
    uint32_t group_id;
    uint32_t priority;
    uint64_t ext_flags;
    std::vector<uint32_t> orig_insns;
    uint32_t orig_insn_count = 0;
};

struct HookIteratorImpl {
    std::map<void*, std::vector<HookHandleImpl*>>::const_iterator outer_it;
    std::vector<HookHandleImpl*>::const_iterator inner_it;
    bool end;
};

class HookManager {
public:
    static HookManager& instance() {
        static HookManager mgr;
        return mgr;
    }

    Error initialize(const GlobalConfig* config) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_) return Error::ALREADY_EXISTS;
        if (config) {
            if (config->log_level >= 0 && config->log_level <= 4)
                Logger::instance().set_level(static_cast<LogLevel>(config->log_level));
            if (config->log_callback)
                Logger::instance().set_callback(config->log_callback);
        }
        initialized_ = true;
        return Error::SUCCESS;
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [target, vec] : hooks_) {
            for (auto* h : vec) {
                if (h->trampoline) {
                    auto err = free_trampoline(target, h->trampoline);
                    if (err != Error::SUCCESS) {
                        LOG_ERROR("shutdown cleanup trampoline failed: %s", to_string(err).data());
                    }
                }
                delete h;
            }
        }
        hooks_.clear();
        initialized_ = false;
    }

    Error install(void* target, void* replacement, void** trampoline_out, uint32_t flags, HookHandleImpl** out_handle) {
        if (!target || !replacement || !out_handle) return Error::INVALID_ARG;
        if (!initialized_) return Error::NOT_INITIALIZED;

        std::lock_guard<std::mutex> lock(mutex_);

        if (hooks_.find(target) != hooks_.end()) {
            LOG_ERROR("install: target %p already has hook installed, duplicate hook not supported", target);
            return Error::ALREADY_EXISTS;
        }

        uint32_t* tramp = nullptr;
        std::vector<uint32_t> orig_insns;
        uint32_t orig_insn_count = 0;
        Error err = arm64_hook::install(target, replacement, &tramp, &orig_insns, &orig_insn_count);
        if (err != Error::SUCCESS) return err;

        HookHandleImpl* handle = nullptr;
        try {
            handle = new HookHandleImpl{
                .target = target,
                .replacement = replacement,
                .trampoline = tramp,
                .user_data = nullptr,
                .flags = flags,
                .enabled = true,
                .install_time = time(nullptr),
                .call_count = 0,
                .name = {0},
                .group_id = 0,
                .priority = 0,
                .ext_flags = 0,
                .orig_insns = std::move(orig_insns),
                .orig_insn_count = orig_insn_count
            };
        } catch (const std::bad_alloc&) {
            (void)free_trampoline(target, tramp);
            return Error::TRAMPOLINE_ALLOC;
        } catch (...) {
            (void)free_trampoline(target, tramp);
            return Error::INTERNAL;
        }

        hooks_[target].push_back(handle);

        if (on_installed_) on_installed_(target, replacement, tramp);
        if (trampoline_out) *trampoline_out = tramp;
        *out_handle = handle;
        return Error::SUCCESS;
    }

    Error remove(HookHandleImpl* handle) {
        if (!handle) return Error::INVALID_ARG;
        if (!initialized_) return Error::NOT_INITIALIZED;

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = hooks_.find(handle->target);
        if (it == hooks_.end()) return Error::NOT_FOUND;
        auto& vec = it->second;
        auto pos = std::find(vec.begin(), vec.end(), handle);
        if (pos == vec.end()) return Error::NOT_FOUND;

        void* target = handle->target;
        std::vector<uint32_t> orig_insns = std::move(handle->orig_insns);
        uint32_t orig_insn_count = handle->orig_insn_count;

        if (!orig_insns.empty() && orig_insn_count > 0) {
            auto err = restore_target(target, orig_insns, orig_insn_count);
            if (err != Error::SUCCESS) {
                LOG_ERROR("remove: failed to restore original instructions: %s", to_string(err).data());
            }
        }

        if (handle->trampoline) {
            auto err = free_trampoline(handle->target, handle->trampoline);
            if (err != Error::SUCCESS) {
                LOG_ERROR("remove: cleanup trampoline failed: %s", to_string(err).data());
            }
        }
        vec.erase(pos);
        delete handle;
        if (vec.empty()) hooks_.erase(it);

        if (on_removed_) on_removed_(target);
        return Error::SUCCESS;
    }

    Error remove_by_target(void* target) {
        if (!target) return Error::INVALID_ARG;
        if (!initialized_) return Error::NOT_INITIALIZED;

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = hooks_.find(target);
        if (it == hooks_.end()) return Error::NOT_FOUND;

        for (auto* handle : it->second) {
            if (handle->trampoline) {
                auto err = free_trampoline(target, handle->trampoline);
                if (err != Error::SUCCESS) {
                    LOG_ERROR("remove_by_target: cleanup trampoline failed: %s", to_string(err).data());
                }
            }
            delete handle;
        }
        hooks_.erase(it);
        if (on_removed_) on_removed_(target);
        return Error::SUCCESS;
    }

    Error find_trampoline(void* target, void** trampoline_out) {
        if (!target || !trampoline_out) return Error::INVALID_ARG;
        if (!initialized_) return Error::NOT_INITIALIZED;

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = hooks_.find(target);
        if (it == hooks_.end() || it->second.empty()) return Error::NOT_FOUND;
        *trampoline_out = it->second[0]->trampoline;
        return Error::SUCCESS;
    }

    size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t total = 0;
        for (const auto& [_, vec] : hooks_) total += vec.size();
        return total;
    }

    HookIteratorImpl* iter_create() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* iter = new HookIteratorImpl;
        iter->outer_it = hooks_.begin();
        if (iter->outer_it != hooks_.end()) {
            iter->inner_it = iter->outer_it->second.begin();
            iter->end = false;
        } else {
            iter->end = true;
        }
        return iter;
    }

    bool iter_next(HookIteratorImpl* iter, void** target, void** replacement, void** trampoline) {
        if (!iter || iter->end) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (iter->outer_it == hooks_.end()) {
            iter->end = true;
            return false;
        }
        auto* handle = *(iter->inner_it);
        if (target) *target = handle->target;
        if (replacement) *replacement = handle->replacement;
        if (trampoline) *trampoline = handle->trampoline;
        ++(iter->inner_it);
        if (iter->inner_it == iter->outer_it->second.end()) {
            ++(iter->outer_it);
            if (iter->outer_it != hooks_.end())
                iter->inner_it = iter->outer_it->second.begin();
            else
                iter->end = true;
        }
        return true;
    }

    void iter_destroy(HookIteratorImpl* iter) { delete iter; }

    Error set_name(HookHandleImpl* handle, const char* name) {
        if (!handle || !name) return Error::INVALID_ARG;
        strncpy(handle->name, name, sizeof(handle->name) - 1);
        handle->name[sizeof(handle->name) - 1] = '\0';
        return Error::SUCCESS;
    }

    Error get_name(HookHandleImpl* handle, char* buffer, size_t buffer_size) {
        if (!handle || !buffer) return Error::INVALID_ARG;
        if (buffer_size == 0) return Error::BUFFER_TOO_SMALL;
        size_t len = strlen(handle->name);
        if (buffer_size <= len) return Error::BUFFER_TOO_SMALL;
        std::strncpy(buffer, handle->name, buffer_size - 1);
        buffer[buffer_size - 1] = '\0';
        return Error::SUCCESS;
    }

    Error set_group_id(HookHandleImpl* handle, uint32_t group_id) {
        if (!handle) return Error::INVALID_ARG;
        handle->group_id = group_id;
        return Error::SUCCESS;
    }

    Error get_group_id(HookHandleImpl* handle, uint32_t* out_group_id) {
        if (!handle || !out_group_id) return Error::INVALID_ARG;
        *out_group_id = handle->group_id;
        return Error::SUCCESS;
    }

    Error set_priority(HookHandleImpl* handle, uint32_t priority) {
        if (!handle) return Error::INVALID_ARG;
        handle->priority = priority;
        return Error::SUCCESS;
    }

    Error get_priority(HookHandleImpl* handle, uint32_t* out_priority) {
        if (!handle || !out_priority) return Error::INVALID_ARG;
        *out_priority = handle->priority;
        return Error::SUCCESS;
    }

    Error set_ext_flags(HookHandleImpl* handle, uint64_t ext_flags) {
        if (!handle) return Error::INVALID_ARG;
        handle->ext_flags = ext_flags;
        return Error::SUCCESS;
    }

    Error get_ext_flags(HookHandleImpl* handle, uint64_t* out_ext_flags) {
        if (!handle || !out_ext_flags) return Error::INVALID_ARG;
        *out_ext_flags = handle->ext_flags;
        return Error::SUCCESS;
    }

    Error set_enabled(HookHandleImpl* handle, bool enabled) {
        if (!handle) return Error::INVALID_ARG;
        handle->enabled = enabled;
        return Error::SUCCESS;
    }

    Error is_enabled(HookHandleImpl* handle, bool* out_enabled) {
        if (!handle || !out_enabled) return Error::INVALID_ARG;
        *out_enabled = handle->enabled;
        return Error::SUCCESS;
    }

    Error increment_call_count(HookHandleImpl* handle) {
        if (!handle) return Error::INVALID_ARG;
        handle->call_count++;
        return Error::SUCCESS;
    }

    Error get_call_count(HookHandleImpl* handle, uint64_t* out_count) {
        if (!handle || !out_count) return Error::INVALID_ARG;
        *out_count = handle->call_count;
        return Error::SUCCESS;
    }

    Error set_user_data(HookHandleImpl* handle, void* user_data) {
        if (!handle) return Error::INVALID_ARG;
        handle->user_data = user_data;
        return Error::SUCCESS;
    }

    void* get_user_data(HookHandleImpl* handle) {
        return handle ? handle->user_data : nullptr;
    }

    void register_install_callback(OnHookInstalled cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        on_installed_ = cb;
    }

    void register_remove_callback(OnHookRemoved cb) {
        std::lock_guard<std::mutex> lock(mutex_);
        on_removed_ = cb;
    }

private:
    HookManager() = default;
    ~HookManager() = default;

    mutable std::mutex mutex_;
    bool initialized_ = false;
    std::map<void*, std::vector<HookHandleImpl*>> hooks_;
    OnHookInstalled on_installed_ = nullptr;
    OnHookRemoved on_removed_ = nullptr;
};

Error initialize(const GlobalConfig* config) {
    return HookManager::instance().initialize(config);
}

void shutdown() {
    HookManager::instance().shutdown();
}

Error log_init(const char* dir, const char* filename) {
    bool ok = init_log(dir, filename);
    return ok ? Error::SUCCESS : Error::INTERNAL;
}

void log_set_level(int level) {
    if (level >= 0 && level <= 4)
        Logger::instance().set_level(static_cast<LogLevel>(level));
}

void log_set_callback(LogCallback cb) {
    Logger::instance().set_callback(cb);
}

Error hook_install(void* target, void* replacement, void** trampoline, uint32_t flags, HookHandle* out_handle) {
    HookHandleImpl* handle = nullptr;
    Error err = HookManager::instance().install(target, replacement, trampoline, flags, &handle);
    if (err == Error::SUCCESS && out_handle)
        *out_handle = reinterpret_cast<HookHandle>(handle);
    return err;
}

Error hook_remove(HookHandle handle) {
    return HookManager::instance().remove(reinterpret_cast<HookHandleImpl*>(handle));
}

Error hook_remove_by_target(void* target) {
    return HookManager::instance().remove_by_target(target);
}

size_t hook_count() {
    return HookManager::instance().count();
}

HookIterator hook_iter_create() {
    return reinterpret_cast<HookIterator>(HookManager::instance().iter_create());
}

bool hook_iter_next(HookIterator iter, void** target, void** replacement, void** trampoline) {
    return HookManager::instance().iter_next(reinterpret_cast<HookIteratorImpl*>(iter), target, replacement, trampoline);
}

void hook_iter_destroy(HookIterator iter) {
    HookManager::instance().iter_destroy(reinterpret_cast<HookIteratorImpl*>(iter));
}

Error hook_find_trampoline(void* target, void** trampoline) {
    return HookManager::instance().find_trampoline(target, trampoline);
}

Error hook_set_name(HookHandle handle, const char* name) {
    return HookManager::instance().set_name(reinterpret_cast<HookHandleImpl*>(handle), name);
}

Error hook_get_name(HookHandle handle, char* buffer, size_t buffer_size) {
    return HookManager::instance().get_name(reinterpret_cast<HookHandleImpl*>(handle), buffer, buffer_size);
}

Error hook_set_group_id(HookHandle handle, uint32_t group_id) {
    return HookManager::instance().set_group_id(reinterpret_cast<HookHandleImpl*>(handle), group_id);
}

Error hook_get_group_id(HookHandle handle, uint32_t* out_group_id) {
    return HookManager::instance().get_group_id(reinterpret_cast<HookHandleImpl*>(handle), out_group_id);
}

Error hook_set_priority(HookHandle handle, uint32_t priority) {
    return HookManager::instance().set_priority(reinterpret_cast<HookHandleImpl*>(handle), priority);
}

Error hook_get_priority(HookHandle handle, uint32_t* out_priority) {
    return HookManager::instance().get_priority(reinterpret_cast<HookHandleImpl*>(handle), out_priority);
}

Error hook_set_ext_flags(HookHandle handle, uint64_t ext_flags) {
    return HookManager::instance().set_ext_flags(reinterpret_cast<HookHandleImpl*>(handle), ext_flags);
}

Error hook_get_ext_flags(HookHandle handle, uint64_t* out_ext_flags) {
    return HookManager::instance().get_ext_flags(reinterpret_cast<HookHandleImpl*>(handle), out_ext_flags);
}

Error hook_set_enabled(HookHandle handle, bool enabled) {
    return HookManager::instance().set_enabled(reinterpret_cast<HookHandleImpl*>(handle), enabled);
}

Error hook_is_enabled(HookHandle handle, bool* out_enabled) {
    return HookManager::instance().is_enabled(reinterpret_cast<HookHandleImpl*>(handle), out_enabled);
}

Error hook_increment_call_count(HookHandle handle) {
    return HookManager::instance().increment_call_count(reinterpret_cast<HookHandleImpl*>(handle));
}

Error hook_get_call_count(HookHandle handle, uint64_t* out_count) {
    return HookManager::instance().get_call_count(reinterpret_cast<HookHandleImpl*>(handle), out_count);
}

Error hook_set_user_data(HookHandle handle, void* user_data) {
    return HookManager::instance().set_user_data(reinterpret_cast<HookHandleImpl*>(handle), user_data);
}

void* hook_get_user_data(HookHandle handle) {
    return HookManager::instance().get_user_data(reinterpret_cast<HookHandleImpl*>(handle));
}

void register_install_callback(OnHookInstalled cb) {
    HookManager::instance().register_install_callback(cb);
}

void register_remove_callback(OnHookRemoved cb) {
    HookManager::instance().register_remove_callback(cb);
}

} 