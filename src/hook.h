#pragma once
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace hook {

struct Ctx {
    std::uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    std::uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    std::uint64_t rflags;
};
using Handler = void (*)(Ctx*);

bool InitArena(std::uintptr_t lo, std::uintptr_t hi, std::size_t size = 0x4000);
std::uintptr_t ArenaBase();

class Emitter {
public:
    explicit Emitter(std::size_t reserve = 256) { buf_.reserve(reserve); }
    Emitter& b(std::initializer_list<std::uint8_t> bytes) { buf_.insert(buf_.end(), bytes); return *this; }
    Emitter& raw(const std::uint8_t* p, std::size_t n) { buf_.insert(buf_.end(), p, p + n); return *this; }
    Emitter& u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) buf_.push_back((v >> (8 * i)) & 0xFF); return *this; }
    Emitter& u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) buf_.push_back((v >> (8 * i)) & 0xFF); return *this; }
    Emitter& jmpAbs(std::uint64_t to) { return b({0xFF, 0x25, 0, 0, 0, 0}).u64(to); }
    Emitter& callAbs(std::uint64_t to) { return b({0xFF, 0x15, 2, 0, 0, 0, 0xEB, 8}).u64(to); }
    const std::vector<std::uint8_t>& bytes() const { return buf_; }
    std::size_t size() const { return buf_.size(); }
private:
    std::vector<std::uint8_t> buf_;
};

std::uintptr_t Commit(const Emitter& e);
bool PatchJump(std::uintptr_t site, std::size_t len, std::uintptr_t stub);
bool WriteMem(std::uintptr_t addr, const void* bytes, std::size_t n);
bool InstallCtxHook(std::uintptr_t site, std::size_t len, Handler h,
                    const std::uint8_t* displaced, std::size_t dlen);
bool InstallDetour(std::uintptr_t site, std::size_t len, void* detourFn,
                   void** outOrigTrampoline, const std::uint8_t* displaced, std::size_t dlen);
void RemoveAllHooks();

} // namespace hook
