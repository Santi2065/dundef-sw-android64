// Runs the 2012 armeabi (32-bit) DunDef UE3 library inside a 64-bit process.
// Guest memory is identity-mapped in the low 4GB: a guest pointer IS a host pointer,
// so most libc/GL calls pass straight through; only 32/64-bit layout differences need code.
#pragma once
#include <android/log.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <tuple>
#include <type_traits>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s32 = int32_t;
using s16 = int16_t;

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ddport", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "ddport", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ddport", __VA_ARGS__)
[[noreturn]] void fatal(const char* fmt, ...);

// ---------------------------------------------------------------- memory.cpp
namespace mem {
void init();
void* alloc(size_t n);
void* alloc_aligned(size_t align, size_t n);
void* calloc(size_t n, size_t sz);
void* realloc(void* p, size_t n);
void free(void* p);
u32 strdup(const char* s);  // copy a host string into guest memory

inline u32 g(const void* p) {
    auto v = reinterpret_cast<uintptr_t>(p);
    if (v >> 32) fatal("host pointer %p leaked to guest", p);
    return static_cast<u32>(v);
}
template <class T = void>
inline T* h(u32 a) { return reinterpret_cast<T*>(static_cast<uintptr_t>(a)); }
inline u32 rd32(u32 a) { u32 v; memcpy(&v, h(a), 4); return v; }
inline void wr32(u32 a, u32 v) { memcpy(h(a), &v, 4); }
}  // namespace mem

struct GuestLib {
    u32 base = 0, exidx = 0, exidx_count = 0;
    u32 symtab = 0, strtab = 0, nsyms = 0;
    u32 init_array = 0, init_count = 0;
    u32 code_end = 0;  // end of the executable segment
    u32 sym(const char* name) const;
};
GuestLib load_guest(const char* path);
void run_guest_constructors(const GuestLib& lib);

// ---------------------------------------------------------------- cpu.cpp
// A guest call into the host lands here with the guest registers.
struct Ctx {
    std::array<u32, 16>& r;
    u32 arg(int i) const { return i < 4 ? r[i] : mem::rd32(r[13] + 4 * (i - 4)); }
    void ret(u32 v) { r[0] = v; }
    void ret64(u64 v) { r[0] = static_cast<u32>(v); r[1] = static_cast<u32>(v >> 32); }
};
using Handler = void (*)(Ctx&);

u32 make_thunk(Handler h, const char* name, bool reenters = false);  // guest address of an "svc #n; bx lr" stub
void register_import(const char* name, Handler h);
void hook_guest(u32 addr, Handler h, const char* name);
u32 import_address(const char* name);  // 0 if nobody provides it
void register_data(const char* name, u32 guest_addr);

u64 call_guest(u32 fn, const u32* args, size_t n);
inline u64 call_guest(u32 fn, std::initializer_list<u32> args) { return call_guest(fn, args.begin(), args.size()); }
u32 guest_errno_addr();  // per-thread errno slot in guest memory
u32 guest_thread_id();   // 32-bit id the guest sees as pthread_t
void invalidate_guest_code(u32 addr, u32 len);  // after patching code this thread may have run
extern std::atomic<u64> g_svc_count;  // guest->host calls, for the perf log
extern bool g_profile;
void log_svc_top();
void guest_profile_init(const GuestLib& lib);
void log_guest_top();  // debug.ddport.profile 2: per-function instruction counts

// ---------------------------------------------------------------- module init
void init_libc();
void init_gl();
void init_pthread();
void init_softfp(const GuestLib& lib);

// ---------------------------------------------------------------- generic wrappers
// Reads AAPCS (softfp) arguments in order: 32-bit words in r0-r3 then stack,
// 64-bit values aligned to an even word.
struct ArgReader {
    Ctx& c;
    int i = 0;
    u32 w() { return c.arg(i++); }
    u64 dw() {
        i = (i + 1) & ~1;
        u64 lo = c.arg(i++);
        u64 hi = c.arg(i++);
        return lo | (hi << 32);
    }
};

template <typename T>
T read_arg(ArgReader& rd) {
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<T>(static_cast<uintptr_t>(rd.w()));
    } else if constexpr (std::is_same_v<T, float>) {
        u32 b = rd.w(); float f; memcpy(&f, &b, 4); return f;
    } else if constexpr (std::is_same_v<T, double>) {
        u64 b = rd.dw(); double d; memcpy(&d, &b, 8); return d;
    } else if constexpr (std::is_same_v<T, long long> || std::is_same_v<T, unsigned long long>) {
        return static_cast<T>(rd.dw());
    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        // long/ssize_t/off_t are 32-bit on the guest: sign-extend
        return static_cast<T>(static_cast<s32>(rd.w()));
    } else {
        return static_cast<T>(rd.w());
    }
}

template <typename R>
void write_ret(Ctx& c, R v) {
    if constexpr (std::is_pointer_v<R>) {
        c.ret(mem::g(v));
    } else if constexpr (std::is_same_v<R, float>) {
        u32 b; memcpy(&b, &v, 4); c.ret(b);
    } else if constexpr (std::is_same_v<R, double>) {
        u64 b; memcpy(&b, &v, 8); c.ret64(b);
    } else if constexpr (std::is_same_v<R, long long> || std::is_same_v<R, unsigned long long>) {
        c.ret64(static_cast<u64>(v));
    } else {
        c.ret(static_cast<u32>(v));
    }
}

template <typename F> struct FnTraits;
template <typename R, typename... A> struct FnTraits<R (*)(A...)> {
    using Ret = R;
    using Args = std::tuple<A...>;
};

template <auto F>
void wrap(Ctx& c) {
    using T = FnTraits<decltype(F)>;
    ArgReader rd{c};
    // braced init guarantees left-to-right evaluation of the reads
    auto args = [&]<typename... A>(std::tuple<A...>*) { return std::tuple<A...>{read_arg<A>(rd)...}; }(
        static_cast<typename T::Args*>(nullptr));
    if constexpr (std::is_void_v<typename T::Ret>) {
        std::apply(F, args);
    } else {
        write_ret(c, std::apply(F, args));
    }
}

// ---------------------------------------------------------------- guest varargs
// Either the AAPCS argument list of the current call, or a guest va_list (a pointer).
struct VaReader {
    ArgReader* ar = nullptr;
    u32 ap = 0;
    u32 w() {
        if (ar) return ar->w();
        u32 v = mem::rd32(ap);
        ap += 4;
        return v;
    }
    u64 dw() {
        if (ar) return ar->dw();
        ap = (ap + 7) & ~7u;
        u64 v = mem::rd32(ap) | (u64(mem::rd32(ap + 4)) << 32);
        ap += 8;
        return v;
    }
};

#define IMPORT(name) register_import(#name, wrap<static_cast<decltype(&::name)>(&::name)>)
#define IMPORT_T(name, type) register_import(#name, wrap<static_cast<std::add_pointer_t<type>>(&::name)>)
