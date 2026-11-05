#pragma once
// Etapa 4 do plano de threads (ghost_pc/docs/threading-design.md): VU1 numa thread propria, estilo MTVU.
//
// PS2X_THREADS=vu liga. O EE (thread do jogo) publica num anel SPSC, em ordem de programa, tudo o que a VU1 e o
// arbitro do GIF fariam: UNPACK (com payload e o snapshot dos registradores do VIF1), MPG, MSCAL/MSCNT e os
// submit/drain do arbitro do GIF (PATH2/PATH3). A thread VU1 executa os itens na mesma ordem; os PATH1 do XGKICK vao
// direto ao arbitro dentro da thread VU1. Como o arbitro so recebe chamadas da thread VU1 enquanto o modo esta ativo,
// a sequencia submit/drain que o GS ve e a mesma do modo serial, por construcao.
//
// O EE espera o anel esvaziar (waitIdle) antes de qualquer acesso direto ao GS (metodos publicos do frontend), aos
// registradores privilegiados do GS, a memoria da VU1 por MMIO/DMA e no VBlankStart. Header-only (sem .cpp novo: nada
// de reconfigure do CMake). Nao incluir em headers do runner.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace ps2_vu_thread
{
    enum MsgType : uint32_t
    {
        MsgPad = 0,
        MsgGifSubmit = 1, // a = path, b = directHl, payload = packet
        MsgGifDrain = 2,
        MsgMpg = 3,       // a = dest byte address, payload = code
        MsgUnpack = 4,    // payload = handler-defined struct + source bytes
        MsgCall = 5,      // a = startPC, b = top | itop << 16, c = flags (bit0 = MSCNT)
        MsgCount = 6
    };

    struct MsgHeader
    {
        uint32_t type;
        uint32_t size; // payload bytes (sem o cabecalho)
        uint32_t a;
        uint32_t b;
        uint32_t c;
        uint32_t pad[3];
    };
    static_assert(sizeof(MsgHeader) == 32, "header size");

    using Handler = void (*)(const MsgHeader &, const uint8_t *payload);

    struct State
    {
        std::atomic<bool> active{false};
        std::atomic<bool> stop{false};
        std::thread::id eeTid{};
        std::thread::id vuTid{};
        uint8_t *ring = nullptr;
        uint64_t capacity = 0;
        alignas(64) std::atomic<uint64_t> head{0}; // bytes publicados (EE)
        alignas(64) std::atomic<uint64_t> tail{0}; // bytes consumidos e executados (VU)
        alignas(64) std::atomic<bool> consumerSleeping{false};
        std::mutex mutex;
        std::condition_variable cv;
        Handler handlers[MsgCount]{};
        void (*afterIdle)() = nullptr; // EE: copia o estado autoritativo da VU de volta (ROW)
        // contadores
        std::atomic<uint64_t> waitsIdle{0};
        std::atomic<uint64_t> waitsByReason[8]{};
        std::atomic<uint64_t> waitsFull{0};
        std::atomic<uint64_t> msgs{0};
        std::atomic<uint64_t> dtStops{0};
        std::atomic<uint64_t> waitIdleNs{0};   // EE parado em waitIdle
        std::atomic<uint64_t> waitFullNs{0};   // EE parado por anel cheio
        std::atomic<uint64_t> vuBusyNs{0};     // thread VU1 executando itens
        std::atomic<uint64_t> callMsgs{0};
        bool lockstep = false;
        int initState = 0; // 0 = nao decidido, 1 = desligado, 2 = ligado
    };

    inline State g_state;

    inline bool envRequested()
    {
        const char *v = std::getenv("PS2X_THREADS");
        if (!v)
            return false;
        return std::strstr(v, "vu") != nullptr || std::strstr(v, "all") != nullptr;
    }

    inline bool onEe()
    {
        return g_state.active.load(std::memory_order_relaxed) && std::this_thread::get_id() == g_state.eeTid;
    }

    inline void consumerLoop()
    {
        State &s = g_state;
        s.vuTid = std::this_thread::get_id();
        uint64_t tail = s.tail.load(std::memory_order_relaxed);
        for (;;)
        {
            uint64_t head = s.head.load(std::memory_order_acquire);
            if (head == tail)
            {
                int spins = 0;
                while ((head = s.head.load(std::memory_order_acquire)) == tail)
                {
                    if (s.stop.load(std::memory_order_relaxed))
                        return;
                    if (++spins < 4000)
                    {
                        std::this_thread::yield();
                        continue;
                    }
                    std::unique_lock<std::mutex> lock(s.mutex);
                    s.consumerSleeping.store(true, std::memory_order_seq_cst);
                    if (s.head.load(std::memory_order_seq_cst) == tail)
                        s.cv.wait_for(lock, std::chrono::microseconds(500));
                    s.consumerSleeping.store(false, std::memory_order_relaxed);
                }
            }
            const auto busyStart = std::chrono::steady_clock::now();
            while (tail != head)
            {
                const uint64_t off = tail % s.capacity;
                MsgHeader hdr{};
                if (s.capacity - off >= sizeof(MsgHeader))
                    std::memcpy(&hdr, s.ring + off, sizeof(hdr));
                uint64_t total = sizeof(MsgHeader) + ((static_cast<uint64_t>(hdr.size) + 15u) & ~15ull);
                if (hdr.type == MsgPad)
                {
                    total = s.capacity - off;
                }
                else if (hdr.type < MsgCount && s.handlers[hdr.type])
                {
                    s.handlers[hdr.type](hdr, s.ring + off + sizeof(MsgHeader));
                }
                tail += total;
                s.tail.store(tail, std::memory_order_release);
            }
            s.vuBusyNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - busyStart).count()),
                                 std::memory_order_relaxed);
        }
    }

    // Chamado pelo EE no primeiro MSCAL. Retorna true se o modo assincrono esta ativo.
    inline bool ensureStarted()
    {
        State &s = g_state;
        if (s.initState == 2)
            return true;
        if (s.initState == 1)
            return false;
        if (!envRequested())
        {
            s.initState = 1;
            return false;
        }
        uint64_t mb = 32;
        if (const char *v = std::getenv("PS2X_VU_THREAD_RING_MB"))
        {
            const long parsed = std::strtol(v, nullptr, 10);
            if (parsed >= 1 && parsed <= 1024)
                mb = static_cast<uint64_t>(parsed);
        }
        s.capacity = mb << 20;
        s.ring = new uint8_t[s.capacity];
        s.eeTid = std::this_thread::get_id();
        s.lockstep = std::getenv("PS2X_VU_THREAD_LOCKSTEP") != nullptr;
        s.initState = 2;
        std::thread(consumerLoop).detach();
        s.active.store(true, std::memory_order_release);
        std::fprintf(stdout, "[vu-thread] PS2X_THREADS=vu: VU1 + arbitro do GIF numa thread propria, anel=%llu MiB\n",
                     static_cast<unsigned long long>(mb));
        std::fflush(stdout);
        return true;
    }

    // Acorda a thread VU1 se ela dorme. Chamado so quando o EE precisa dela (espera, anel cheio) ou quando o atraso
    // passa de kWakeBytes: um notify por item custava uma troca de contexto por item (~5x mais lento, medido).
    inline void wakeConsumer()
    {
        State &s = g_state;
        if (s.consumerSleeping.load(std::memory_order_seq_cst))
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.cv.notify_one();
        }
    }

    constexpr uint64_t kWakeBytes = 64u * 1024u;

    enum WaitReason : int
    {
        WaitGsFrontend = 0,
        WaitVblank = 1,
        WaitGsPriv = 2,
        WaitVuMem = 3,
        WaitArbiterEmpty = 4,
        WaitDtSync = 5,
        WaitLockstep = 6
    };

    inline void waitIdle(int reason = WaitGsFrontend)
    {
        State &s = g_state;
        if (!onEe())
            return;
        const uint64_t head = s.head.load(std::memory_order_relaxed);
        if (s.tail.load(std::memory_order_acquire) != head)
        {
            s.waitsIdle.fetch_add(1, std::memory_order_relaxed);
            s.waitsByReason[reason & 7].fetch_add(1, std::memory_order_relaxed);
            const auto t0 = std::chrono::steady_clock::now();
            wakeConsumer();
            int spins = 0;
            while (s.tail.load(std::memory_order_acquire) != head)
            {
                if (++spins > 64)
                    std::this_thread::yield();
            }
            s.waitIdleNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - t0).count()),
                                   std::memory_order_relaxed);
        }
        if (s.afterIdle)
            s.afterIdle();
    }

    // Reserva espaco contiguo para cabecalho + payload, devolve o ponteiro do payload. Somente EE.
    inline uint8_t *beginPush(MsgHeader hdr)
    {
        State &s = g_state;
        const uint64_t payload = (static_cast<uint64_t>(hdr.size) + 15u) & ~15ull;
        const uint64_t total = sizeof(MsgHeader) + payload;
        if (total > s.capacity / 2)
        {
            std::fprintf(stderr, "[vu-thread] mensagem grande demais (%llu bytes)\n", static_cast<unsigned long long>(total));
            std::abort();
        }
        uint64_t head = s.head.load(std::memory_order_relaxed);
        for (;;)
        {
            const uint64_t off = head % s.capacity;
            const uint64_t toEnd = s.capacity - off;
            const uint64_t need = (toEnd < total) ? toEnd + total : total;
            if (head + need - s.tail.load(std::memory_order_acquire) > s.capacity)
            {
                s.waitsFull.fetch_add(1, std::memory_order_relaxed);
                const auto t0 = std::chrono::steady_clock::now();
                wakeConsumer();
                // histerese: espera o anel cair a metade
                while (head - s.tail.load(std::memory_order_acquire) > s.capacity / 2)
                    std::this_thread::yield();
                std::this_thread::yield();
                s.waitFullNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - t0).count()),
                                       std::memory_order_relaxed);
                continue;
            }
            if (toEnd < total)
            {
                if (toEnd >= sizeof(MsgHeader))
                {
                    MsgHeader pad{};
                    pad.type = MsgPad;
                    std::memcpy(s.ring + off, &pad, sizeof(pad));
                }
                head += toEnd;
                s.head.store(head, std::memory_order_release);
                continue;
            }
            std::memcpy(s.ring + off, &hdr, sizeof(hdr));
            return s.ring + off + sizeof(MsgHeader);
        }
    }

    inline void endPush(const MsgHeader &hdr)
    {
        State &s = g_state;
        const uint64_t total = sizeof(MsgHeader) + ((static_cast<uint64_t>(hdr.size) + 15u) & ~15ull);
        const uint64_t newHead = s.head.load(std::memory_order_relaxed) + total;
        s.head.store(newHead, std::memory_order_seq_cst);
        s.msgs.fetch_add(1, std::memory_order_relaxed);
        if (newHead - s.tail.load(std::memory_order_relaxed) >= kWakeBytes)
            wakeConsumer();
        if (s.lockstep)
            waitIdle(WaitLockstep);
    }

    inline void push(uint32_t type, uint32_t a, uint32_t b, uint32_t c, const void *p1, uint32_t n1,
                     const void *p2 = nullptr, uint32_t n2 = 0)
    {
        MsgHeader hdr{};
        hdr.type = type;
        hdr.size = n1 + n2;
        hdr.a = a;
        hdr.b = b;
        hdr.c = c;
        uint8_t *dst = beginPush(hdr);
        if (n1)
            std::memcpy(dst, p1, n1);
        if (n2)
            std::memcpy(dst + n1, p2, n2);
        endPush(hdr);
    }
} // namespace ps2_vu_thread
