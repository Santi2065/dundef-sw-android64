#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <pthread.h>
#include <time.h>

#include <atomic>
#include <cerrno>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core.h"

extern GuestLib g_lib;
extern std::atomic<u64> g_svc_count;

// ---------------------------------------------------------------- thunks
namespace {
constexpr u32 kRetSvc = 0xFFFFFF;
constexpr u32 kNoSvc = 0xFFFFFFFF;
constexpr u32 kMaxThunks = 8192;

struct Thunk {
    Handler h;
    const char* name;
    bool reenters;  // may call back into the guest: must run outside Jit::Run
};
std::vector<Thunk> thunks;
u32 thunk_area = 0, ret_stub = 0;
std::mutex thunk_lock;
std::unordered_map<std::string, Handler> imports;
std::unordered_map<std::string, u32> data_imports;

void init_thunks() {
    if (thunk_area) return;
    thunk_area = mem::g(mem::alloc_aligned(16, kMaxThunks * 8));
    ret_stub = mem::g(mem::alloc_aligned(16, 8));
    mem::wr32(ret_stub, 0xEF000000 | kRetSvc);  // svc #RET
    mem::wr32(ret_stub + 4, 0xE12FFF1E);         // bx lr (never reached)
    thunks.reserve(kMaxThunks);
}
}  // namespace

u32 make_thunk(Handler h, const char* name, bool reenters) {
    std::lock_guard lk(thunk_lock);
    init_thunks();
    u32 id = thunks.size();
    if (id >= kMaxThunks) fatal("out of thunks");
    thunks.push_back({h, name ? strdup(name) : "?", reenters});
    u32 a = thunk_area + id * 8;
    mem::wr32(a, 0xEF000000 | id);  // svc #id
    mem::wr32(a + 4, 0xE12FFF1E);    // bx lr
    return a;
}

void register_import(const char* name, Handler h) { imports[name] = h; }
void register_data(const char* name, u32 a) { data_imports[name] = a; }

u32 import_address(const char* name) {
    if (auto it = data_imports.find(name); it != data_imports.end()) return it->second;
    if (auto it = imports.find(name); it != imports.end()) return make_thunk(it->second, name, !strcmp(name, "qsort"));
    return 0;
}

// ---------------------------------------------------------------- guest threads
namespace {
struct GuestThread final : Dynarmic::A32::UserCallbacks {
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    u32 svc = kNoSvc;
    u32 stack_top = 0, stack_base = 0, errno_addr = 0, tid = 0;
    int depth = 0;

    explicit GuestThread(u32 id) : tid(id) {
        constexpr u32 kStack = 8u << 20;
        stack_base = mem::g(mem::alloc_aligned(16, kStack));
        stack_top = stack_base + kStack - 64;
        errno_addr = mem::g(mem::calloc(1, 4));
        Dynarmic::A32::UserConfig cfg;
        cfg.callbacks = this;
        cfg.processor_id = id;
        cfg.arch_version = Dynarmic::A32::ArchVersion::v7;  // code is v5TE but shipped on v7 cores
        cfg.fastmem_pointer = 0;                             // identity-mapped guest memory
        cfg.enable_cycle_counting = false;
        cfg.define_unpredictable_behaviour = true;
        cfg.always_little_endian = true;
        cfg.code_cache_size = 64u << 20;
        jit = std::make_unique<Dynarmic::A32::Jit>(cfg);
    }
    ~GuestThread() override {
        jit.reset();
        mem::free(mem::h(stack_base));
        mem::free(mem::h(errno_addr));
    }

    // only reached when fastmem faulted: check before touching the address
    void check(u32 a) {
        if (a < 0x10000)
            fatal("guest null access at %08x: pc=%08x lr=%08x r0=%08x r1=%08x r2=%08x r3=%08x (lib base %08x)", a,
                  jit->Regs()[15], jit->Regs()[14], jit->Regs()[0], jit->Regs()[1], jit->Regs()[2], jit->Regs()[3],
                  g_lib.base);
    }
    template <class T> T rd(u32 a) { check(a); T v; memcpy(&v, mem::h(a), sizeof v); return v; }
    template <class T> void wr(u32 a, T v) { check(a); memcpy(mem::h(a), &v, sizeof v); }
    u8 MemoryRead8(u32 a) override { return rd<u8>(a); }
    u16 MemoryRead16(u32 a) override { return rd<u16>(a); }
    u32 MemoryRead32(u32 a) override { return rd<u32>(a); }
    u64 MemoryRead64(u32 a) override { return rd<u64>(a); }
    void MemoryWrite8(u32 a, u8 v) override { wr(a, v); }
    void MemoryWrite16(u32 a, u16 v) override { wr(a, v); }
    void MemoryWrite32(u32 a, u32 v) override { wr(a, v); }
    void MemoryWrite64(u32 a, u64 v) override { wr(a, v); }
    void InterpreterFallback(u32 pc, size_t) override { fatal("interpreter fallback at %08x", pc); }
    // Leaf imports (most of them: libc, GL) run right here inside the JIT callback; only calls
    // that can re-enter the guest leave Jit::Run first, since Run is not reentrant.
    void CallSVC(u32 swi) override {
        if (swi < thunks.size() && thunks[swi].h && !thunks[swi].reenters) {
            dispatch(swi);
            return;
        }
        svc = swi;
        jit->HaltExecution();
    }
    void dispatch(u32 swi) {
        const Thunk& t = thunks[swi];
        if (!t.h) fatal("guest called unimplemented %s (lr=%08x)", t.name, jit->Regs()[14]);
        g_svc_count.fetch_add(1, std::memory_order_relaxed);
        Ctx c{jit->Regs()};
        t.h(c);
        mem::wr32(errno_addr, errno);
    }
    void ExceptionRaised(u32 pc, Dynarmic::A32::Exception e) override {
        fatal("guest exception %d at pc=%08x lr=%08x", static_cast<int>(e), pc, jit->Regs()[14]);
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 0; }

    void run() {
        for (;;) {
            svc = kNoSvc;
            jit->Run();
            if (svc == kRetSvc) return;
            if (svc == kNoSvc) fatal("jit stopped without svc, pc=%08x", jit->Regs()[15]);
            dispatch(svc);
        }
    }
};

std::atomic<u32> next_tid{1};
}  // namespace
std::atomic<u64> g_svc_count{0};
namespace {
thread_local std::unique_ptr<GuestThread> tls_thread;
thread_local u32 tls_preassigned_tid = 0;

GuestThread& cur() {
    if (!tls_thread) {
        u32 id = tls_preassigned_tid ? tls_preassigned_tid : next_tid++;
        tls_thread = std::make_unique<GuestThread>(id);
    }
    return *tls_thread;
}
}  // namespace

u32 guest_errno_addr() { return cur().errno_addr; }
u32 guest_thread_id() { return cur().tid; }

u64 call_guest(u32 fn, const u32* args, size_t nargs) {
    GuestThread& t = cur();
    auto& jit = *t.jit;
    std::array<u32, 16> regs{};
    std::array<u32, 64> ext{};
    u32 cpsr = 0, fpscr = 0;
    bool nested = t.depth > 0;
    if (nested) {
        regs = jit.Regs();
        ext = jit.ExtRegs();
        cpsr = jit.Cpsr();
        fpscr = jit.Fpscr();
    }
    u32 sp = nested ? (regs[13] - 256) & ~7u : t.stack_top;
    u32 nstack = nargs > 4 ? nargs - 4 : 0;
    sp = (sp - nstack * 4) & ~7u;
    for (u32 i = 0; i < nargs; i++) {
        if (i < 4) jit.Regs()[i] = args[i];
        else mem::wr32(sp + 4 * (i - 4), args[i]);
    }
    jit.Regs()[13] = sp;
    jit.Regs()[14] = ret_stub;
    jit.Regs()[15] = fn & ~1u;
    jit.SetCpsr(0x10 | ((fn & 1) << 5));  // user mode, Thumb if bit0
    jit.SetFpscr(0);

    t.depth++;
    t.run();
    t.depth--;
    u64 r = jit.Regs()[0] | (u64(jit.Regs()[1]) << 32);
    if (nested) {
        jit.Regs() = regs;
        jit.ExtRegs() = ext;
        jit.SetCpsr(cpsr);
        jit.SetFpscr(fpscr);
    }
    return r;
}

// ---------------------------------------------------------------- pthread for the guest
// Old 32-bit bionic mutex/cond are a single int (zero = statically initialised), so we
// lazily hang a real host object off that word. The host objects live in guest memory
// so the pointer fits in 32 bits.
namespace {
template <class T, class Init>
T* lazy_obj(u32 word, Init init) {
    auto* w = mem::h<std::atomic<u32>>(word);
    u32 v = w->load(std::memory_order_acquire);
    if (v >= 0x10000) return mem::h<T>(v);
    auto* o = static_cast<T*>(mem::alloc_aligned(16, sizeof(T)));
    init(o);
    if (w->compare_exchange_strong(v, mem::g(o))) return o;
    mem::free(o);  // lost the race, use the winner
    return mem::h<T>(v);
}
pthread_mutex_t* host_mutex(u32 m) {
    // ponytail: every guest mutex is recursive (old bionic attrs aren't imported); safe superset
    return lazy_obj<pthread_mutex_t>(m, [](pthread_mutex_t* o) {
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(o, &a);
    });
}
pthread_cond_t* host_cond(u32 c) {
    return lazy_obj<pthread_cond_t>(c, [](pthread_cond_t* o) { pthread_cond_init(o, nullptr); });
}

struct ThreadRec {
    pthread_t host;
    u32 ret = 0;
};
std::mutex threads_lock;
std::map<u32, ThreadRec> threads;

struct StartInfo {
    u32 fn, arg, tid;
};
void* thread_main(void* p) {
    auto* si = static_cast<StartInfo*>(p);
    tls_preassigned_tid = si->tid;
    u32 ret = static_cast<u32>(call_guest(si->fn, {si->arg}));
    {
        std::lock_guard lk(threads_lock);
        threads[si->tid].ret = ret;
    }
    delete si;
    return nullptr;
}

void ts32_to_host(u32 g, timespec* ts) {
    ts->tv_sec = static_cast<s32>(mem::rd32(g));
    ts->tv_nsec = static_cast<s32>(mem::rd32(g + 4));
}
}  // namespace

void init_pthread() {
    register_import("pthread_mutex_init", [](Ctx& c) {
        mem::wr32(c.arg(0), 0);
        host_mutex(c.arg(0));
        c.ret(0);
    });
    register_import("pthread_mutex_destroy", [](Ctx& c) {
        u32 v = mem::rd32(c.arg(0));
        if (v >= 0x10000) {
            pthread_mutex_destroy(mem::h<pthread_mutex_t>(v));
            mem::free(mem::h(v));
        }
        mem::wr32(c.arg(0), 0);
        c.ret(0);
    });
    register_import("pthread_mutex_lock", [](Ctx& c) { c.ret(pthread_mutex_lock(host_mutex(c.arg(0)))); });
    register_import("pthread_mutex_unlock", [](Ctx& c) { c.ret(pthread_mutex_unlock(host_mutex(c.arg(0)))); });
    register_import("pthread_cond_init", [](Ctx& c) {
        mem::wr32(c.arg(0), 0);
        host_cond(c.arg(0));
        c.ret(0);
    });
    register_import("pthread_cond_destroy", [](Ctx& c) {
        u32 v = mem::rd32(c.arg(0));
        if (v >= 0x10000) {
            pthread_cond_destroy(mem::h<pthread_cond_t>(v));
            mem::free(mem::h(v));
        }
        mem::wr32(c.arg(0), 0);
        c.ret(0);
    });
    register_import("pthread_cond_signal", [](Ctx& c) { c.ret(pthread_cond_signal(host_cond(c.arg(0)))); });
    register_import("pthread_cond_broadcast", [](Ctx& c) { c.ret(pthread_cond_broadcast(host_cond(c.arg(0)))); });
    register_import("pthread_cond_wait", [](Ctx& c) {
        c.ret(pthread_cond_wait(host_cond(c.arg(0)), host_mutex(c.arg(1))));
    });
    register_import("pthread_cond_timedwait", [](Ctx& c) {
        timespec ts;
        ts32_to_host(c.arg(2), &ts);
        c.ret(pthread_cond_timedwait(host_cond(c.arg(0)), host_mutex(c.arg(1)), &ts));
    });

    register_import("pthread_key_create", [](Ctx& c) {
        pthread_key_t k;
        int r = pthread_key_create(&k, nullptr);  // ponytail: guest TLS destructors are not run
        mem::wr32(c.arg(0), static_cast<u32>(k));
        c.ret(r);
    });
    IMPORT(pthread_key_delete);
    register_import("pthread_getspecific", [](Ctx& c) {
        c.ret(static_cast<u32>(reinterpret_cast<uintptr_t>(pthread_getspecific(c.arg(0)))));
    });
    register_import("pthread_setspecific", [](Ctx& c) {
        c.ret(pthread_setspecific(c.arg(0), reinterpret_cast<void*>(static_cast<uintptr_t>(c.arg(1)))));
    });

    register_import("pthread_self", [](Ctx& c) { c.ret(guest_thread_id()); });
    register_import("pthread_create", [](Ctx& c) {
        u32 tid = next_tid++;
        auto* si = new StartInfo{c.arg(2), c.arg(3), tid};
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1u << 20);  // host side only runs the JIT + thunks
        std::lock_guard lk(threads_lock);
        int r = pthread_create(&threads[tid].host, &attr, thread_main, si);
        pthread_attr_destroy(&attr);
        if (r) {
            threads.erase(tid);
            delete si;
        } else {
            mem::wr32(c.arg(0), tid);
        }
        c.ret(r);
    });
    register_import("pthread_join", [](Ctx& c) {
        pthread_t h;
        {
            std::lock_guard lk(threads_lock);
            auto it = threads.find(c.arg(0));
            if (it == threads.end()) return c.ret(ESRCH);
            h = it->second.host;
        }
        int r = pthread_join(h, nullptr);
        std::lock_guard lk(threads_lock);
        if (c.arg(1)) mem::wr32(c.arg(1), threads[c.arg(0)].ret);
        threads.erase(c.arg(0));
        c.ret(r);
    });
    register_import("pthread_detach", [](Ctx& c) {
        std::lock_guard lk(threads_lock);
        auto it = threads.find(c.arg(0));
        if (it == threads.end()) return c.ret(ESRCH);
        c.ret(pthread_detach(it->second.host));
    });

    // old bionic atomics: cmpxchg returns 0 on success, the others return the old value
    register_import("__atomic_cmpxchg", [](Ctx& c) {
        s32 expected = c.arg(0);
        c.ret(!mem::h<std::atomic<s32>>(c.arg(2))->compare_exchange_strong(expected, c.arg(1)));
    });
    register_import("__atomic_swap", [](Ctx& c) { c.ret(mem::h<std::atomic<s32>>(c.arg(1))->exchange(c.arg(0))); });
    register_import("__atomic_inc", [](Ctx& c) { c.ret(mem::h<std::atomic<s32>>(c.arg(0))->fetch_add(1)); });
    register_import("__atomic_dec", [](Ctx& c) { c.ret(mem::h<std::atomic<s32>>(c.arg(0))->fetch_sub(1)); });
}
