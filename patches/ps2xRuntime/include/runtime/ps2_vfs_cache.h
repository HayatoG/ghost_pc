#pragma once
// Cache de leitura do disco emulado na RAM do host (somente leitura).
// Header-only (inline) para nao exigir mudanca no CMake. Ver docs/vfs-ram-cache.md.
//
// Env: PS2X_VFS_CACHE_MB=N (0 desliga; ausente = auto: min(25% RAM livre, 4 GiB), min 64 MiB)
//      PS2X_VFS_CACHE_LOG=1 (estatisticas em stderr a cada ~10 s e ao encerrar)
//      PS2X_VFS_CACHE_BLOCK_KB (padrao 1024), PS2X_VFS_PRELOAD_MB (padrao 16; 0 desliga)
//      PS2X_VFS_CACHE_SKIP=prefixo1,prefixo2 (arquivos em stream que nao passam pelo cache; padrao "MOV")

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <filesystem>

namespace ps2x
{
    // Memoria fisica disponivel (bytes); definida em ps2_vfs.cpp (evita windows.h neste header,
    // que conflita com ThreadNaming.h/raylib nas unidades que o incluem). 0 = desconhecido.
    uint64_t vfsAvailablePhysicalBytes();

    class VfsCache
    {
    public:
        struct Stats
        {
            uint64_t hits = 0, misses = 0, bytesServed = 0, bytesFromDisk = 0, bypassBytes = 0;
            uint64_t prefetchBlocks = 0, evictions = 0, waitNs = 0, used = 0, budget = 0;
        };

        static VfsCache &instance()
        {
            static VfsCache cache;
            return cache;
        }

        // Construcao explicita (testes). budgetBytes==0 desliga o cache (leitura direta).
        VfsCache(uint64_t budgetBytes, size_t blockSize, uint64_t preloadMax, std::vector<std::string> skipPrefixes, bool log)
        {
            init(budgetBytes, blockSize, preloadMax, std::move(skipPrefixes), log);
        }

        ~VfsCache()
        {
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_stop = true;
            }
            m_queueCv.notify_all();
            if (m_worker.joinable())
                m_worker.join();
            if (m_log)
                printStats("final");
        }

        VfsCache(const VfsCache &) = delete;
        VfsCache &operator=(const VfsCache &) = delete;

        // Abre (ou reaproveita) um arquivo; devolve id != 0, ou 0 em falha.
        uint32_t open(const std::string &path)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto known = m_byPath.find(path);
            if (known != m_byPath.end())
            {
                ++m_files[known->second]->refs;
                return known->second;
            }
            auto file = std::make_shared<File>();
            if (!file->openRaw(path))
                return 0u;
            file->id = m_nextId++;
            file->refs = 1;
            file->bypass = m_budget == 0u || isStream(path);
            const uint32_t id = file->id;
            const bool preload = !file->bypass && m_preloadMax != 0u && file->size <= m_preloadMax &&
                                 file->size <= m_budget / 4u;
            m_byPath.emplace(path, id);
            m_files.emplace(id, file);
            if (preload)
                for (uint64_t b = 0; b * m_blockSize < file->size; ++b)
                    enqueue(id, b);
            return id;
        }

        void release(uint32_t id)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_files.find(id);
            if (it == m_files.end() || --it->second->refs > 0)
                return;
            // Os blocos ficam no cache (disco somente leitura); so o handle e liberado.
            // O arquivo continua registrado por caminho para reuso; handle fecha no destrutor.
        }

        bool size(uint32_t id, uint64_t &out) const
        {
            const auto file = find(id);
            if (!file)
                return false;
            out = file->size;
            return true;
        }

        // Semantica de fseek+fread: offset alem do EOF devolve 0 bytes e true; falso so em erro de E/S.
        bool readAt(uint32_t id, uint64_t offset, void *dst, size_t size, size_t &bytesRead)
        {
            bytesRead = 0u;
            const auto file = find(id);
            if (!file || (!dst && size != 0u))
                return false;
            if (size == 0u)
                return true;
            if (offset >= file->size)
                return true;
            size = static_cast<size_t>(std::min<uint64_t>(size, file->size - offset));

            if (file->bypass)
            {
                const bool ok = timedRaw(*file, offset, dst, size, bytesRead);
                m_bypassBytes += bytesRead;
                return ok;
            }

            const bool sequential = file->lastEnd.load() == offset;
            auto *out = static_cast<uint8_t *>(dst);
            size_t done = 0u;
            while (done < size)
            {
                const uint64_t pos = offset + done;
                const uint64_t idx = pos / m_blockSize;
                const size_t inBlock = static_cast<size_t>(pos % m_blockSize);
                bool hit = false;
                const auto block = getBlock(*file, idx, false, hit);
                size_t chunk = 0u;
                if (block)
                {
                    chunk = std::min<size_t>(size - done, block->data.size() - inBlock);
                    std::memcpy(out + done, block->data.data() + inBlock, chunk);
                }
                else
                {
                    // Falha de E/S no bloco: leitura direta do restante (preserva erro/EOF antigo).
                    size_t got = 0u;
                    const bool ok = timedRaw(*file, pos, out + done, size - done, got);
                    done += got;
                    bytesRead = done;
                    m_bytesServed += got;
                    return ok;
                }
                done += chunk;
            }
            bytesRead = done;
            m_bytesServed += done;
            file->lastEnd = offset + done;
            if (sequential)
                prefetchAfter(*file, (offset + done - 1u) / m_blockSize);
            return true;
        }

        Stats stats() const
        {
            Stats s;
            s.hits = m_hits;
            s.misses = m_misses;
            s.bytesServed = m_bytesServed;
            s.bytesFromDisk = m_bytesFromDisk;
            s.bypassBytes = m_bypassBytes;
            s.prefetchBlocks = m_prefetchBlocks;
            s.evictions = m_evictions;
            s.waitNs = m_waitNs;
            s.budget = m_budget;
            std::lock_guard<std::mutex> lock(m_blockMutex);
            s.used = m_used;
            return s;
        }

        uint64_t budget() const { return m_budget; }
        size_t blockSize() const { return m_blockSize; }

        static uint64_t autoBudget()
        {
            const uint64_t avail = vfsAvailablePhysicalBytes();
            const uint64_t mib = 1024u * 1024u;
            return std::max<uint64_t>(64u * mib, std::min<uint64_t>(avail / 4u, 4096u * mib));
        }

    private:
        struct File
        {
            uint32_t id = 0;
            int refs = 0;
            bool bypass = false;
            uint64_t size = 0;
            std::atomic<uint64_t> lastEnd{~0ull};
            std::FILE *stream = nullptr;
            std::mutex ioMutex; // fseek+fread atomicos por arquivo

            ~File()
            {
                if (stream)
                    std::fclose(stream);
            }

            bool openRaw(const std::string &path)
            {
                const std::filesystem::path p{path};
#if defined(_WIN32)
                stream = ::_wfopen(p.c_str(), L"rb");
#else
                stream = std::fopen(p.string().c_str(), "rb");
#endif
                std::error_code ec;
                size = std::filesystem::file_size(p, ec);
                return stream != nullptr && !ec;
            }

            // Leitura posicional; false em erro de E/S, EOF devolve o que houver (como fseek+fread).
            bool rawRead(uint64_t offset, void *dst, size_t count, size_t &got)
            {
                got = 0u;
                std::lock_guard<std::mutex> lock(ioMutex);
#if defined(_WIN32)
                if (_fseeki64(stream, static_cast<int64_t>(offset), SEEK_SET) != 0)
                    return false;
#else
                if (fseeko(stream, static_cast<off_t>(offset), SEEK_SET) != 0)
                    return false;
#endif
                got = std::fread(dst, 1u, count, stream);
                if (got < count && std::ferror(stream))
                {
                    std::clearerr(stream);
                    return false;
                }
                return true;
            }
        };

        struct Block
        {
            std::vector<uint8_t> data;
            bool ready = false;
            bool failed = false;
            std::list<uint64_t>::iterator lruIt;
            bool inLru = false;
        };

        VfsCache()
        {
            uint64_t budget = autoBudget();
            if (const char *env = std::getenv("PS2X_VFS_CACHE_MB"))
                if (*env)
                    budget = std::strtoull(env, nullptr, 10) * 1024u * 1024u;
            size_t block = 1024u * 1024u;
            if (const char *env = std::getenv("PS2X_VFS_CACHE_BLOCK_KB"))
                if (const uint64_t kb = std::strtoull(env, nullptr, 10); kb >= 64u && kb <= 16384u)
                    block = static_cast<size_t>(kb) * 1024u;
            uint64_t preload = 16u * 1024u * 1024u;
            if (const char *env = std::getenv("PS2X_VFS_PRELOAD_MB"))
                if (*env)
                    preload = std::strtoull(env, nullptr, 10) * 1024u * 1024u;
            std::vector<std::string> skip{"MOV"};
            if (const char *env = std::getenv("PS2X_VFS_CACHE_SKIP"))
            {
                skip.clear();
                std::string cur;
                for (const char *p = env;; ++p)
                {
                    if (*p == ',' || *p == '\0')
                    {
                        if (!cur.empty())
                            skip.push_back(cur);
                        cur.clear();
                        if (*p == '\0')
                            break;
                    }
                    else
                        cur.push_back(*p);
                }
            }
            const char *log = std::getenv("PS2X_VFS_CACHE_LOG");
            init(budget, block, preload, std::move(skip), log && *log && *log != '0');
        }

        void init(uint64_t budgetBytes, size_t blockSize, uint64_t preloadMax, std::vector<std::string> skip, bool log)
        {
            m_budget = budgetBytes;
            m_blockSize = std::max<size_t>(blockSize, 4096u);
            m_preloadMax = preloadMax;
            m_skip = std::move(skip);
            for (auto &s : m_skip)
                for (auto &c : s)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            m_log = log;
            if (m_budget != 0u)
                m_worker = std::thread([this] { workerLoop(); });
            if (m_log)
                std::fprintf(stderr, "[vfs-cache] budget=%llu MiB block=%zu KiB preload<=%llu MiB\n",
                             static_cast<unsigned long long>(m_budget >> 20), m_blockSize >> 10,
                             static_cast<unsigned long long>(m_preloadMax >> 20));
        }

        bool isStream(const std::string &path) const
        {
            size_t cut = path.find_last_of("/\\");
            std::string name = path.substr(cut == std::string::npos ? 0u : cut + 1u);
            for (auto &c : name)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            for (const auto &prefix : m_skip)
                if (name.compare(0, prefix.size(), prefix) == 0)
                    return true;
            return false;
        }

        std::shared_ptr<File> find(uint32_t id) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_files.find(id);
            return it == m_files.end() ? nullptr : it->second;
        }

        bool timedRaw(File &file, uint64_t offset, void *dst, size_t size, size_t &got)
        {
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = file.rawRead(offset, dst, size, got);
            m_waitNs += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            m_bytesFromDisk += got;
            return ok;
        }

        static uint64_t key(uint32_t id, uint64_t idx) { return (static_cast<uint64_t>(id) << 40) | idx; }

        // Devolve o bloco pronto (carregando-o se preciso), ou nullptr em falha de E/S.
        std::shared_ptr<Block> getBlock(File &file, uint64_t idx, bool prefetch, bool &hit)
        {
            const uint64_t k = key(file.id, idx);
            std::unique_lock<std::mutex> lock(m_blockMutex);
            const auto found = m_blocks.find(k);
            if (found != m_blocks.end())
            {
                auto block = found->second;
                if (block->ready)
                {
                    hit = true;
                    if (!prefetch)
                    {
                        ++m_hits;
                        touch(*block, k);
                    }
                    return block;
                }
                if (prefetch)
                    return nullptr;
                ++m_misses;
                const auto t0 = std::chrono::steady_clock::now();
                m_blockCv.wait(lock, [&] { return block->ready || block->failed; });
                m_waitNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                      std::chrono::steady_clock::now() - t0)
                                                      .count());
                return block->failed ? nullptr : block;
            }
            hit = false;
            if (prefetch)
                ++m_prefetchBlocks;
            else
                ++m_misses;
            auto block = std::make_shared<Block>();
            m_blocks.emplace(k, block);
            lock.unlock();

            const uint64_t start = idx * m_blockSize;
            const size_t len = static_cast<size_t>(std::min<uint64_t>(m_blockSize, file.size - start));
            block->data.resize(len);
            size_t got = 0u;
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = file.rawRead(start, block->data.data(), len, got) && got == len;
            if (!prefetch)
                m_waitNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                      std::chrono::steady_clock::now() - t0)
                                                      .count());
            m_bytesFromDisk += got;

            lock.lock();
            if (!ok)
            {
                block->failed = true;
                m_blocks.erase(k);
            }
            else
            {
                while (m_used + len > m_budget && !m_lru.empty())
                {
                    const uint64_t victim = m_lru.back();
                    m_lru.pop_back();
                    const auto v = m_blocks.find(victim);
                    if (v != m_blocks.end())
                    {
                        m_used -= v->second->data.size();
                        v->second->inLru = false;
                        m_blocks.erase(v);
                        ++m_evictions;
                    }
                }
                block->ready = true;
                m_used += len;
                m_lru.push_front(k);
                block->lruIt = m_lru.begin();
                block->inLru = true;
            }
            lock.unlock();
            m_blockCv.notify_all();
            return ok ? block : nullptr;
        }

        void touch(Block &block, uint64_t k)
        {
            if (block.inLru && block.lruIt != m_lru.begin())
            {
                m_lru.erase(block.lruIt);
                m_lru.push_front(k);
                block.lruIt = m_lru.begin();
            }
        }

        void prefetchAfter(File &file, uint64_t lastIdx)
        {
            const uint64_t blocksInBudget = m_budget / m_blockSize;
            const uint64_t depth = std::min<uint64_t>(4u, blocksInBudget / 4u);
            for (uint64_t i = 1; i <= depth; ++i)
            {
                const uint64_t next = lastIdx + i;
                if (next * m_blockSize >= file.size)
                    break;
                enqueue(file.id, next);
            }
        }

        void enqueue(uint32_t id, uint64_t idx)
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_queue.size() >= 256u)
                return;
            m_queue.emplace_back(id, idx);
            m_queueCv.notify_one();
        }

        void workerLoop()
        {
            auto lastLog = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> lock(m_queueMutex);
            while (!m_stop)
            {
                if (m_queue.empty())
                {
                    m_queueCv.wait_for(lock, std::chrono::seconds(2));
                }
                else
                {
                    const auto job = m_queue.front();
                    m_queue.pop_front();
                    lock.unlock();
                    if (const auto file = find(job.first))
                    {
                        bool hit = false;
                        if (job.second * m_blockSize < file->size)
                            (void)getBlock(*file, job.second, true, hit);
                    }
                    lock.lock();
                }
                if (m_log && std::chrono::steady_clock::now() - lastLog > std::chrono::seconds(10))
                {
                    lastLog = std::chrono::steady_clock::now();
                    lock.unlock();
                    printStats("periodic");
                    lock.lock();
                }
            }
        }

        void printStats(const char *tag) const
        {
            const Stats s = stats();
            std::fprintf(stderr,
                         "[vfs-cache] %s hits=%llu misses=%llu served=%llu MiB disk=%llu MiB bypass=%llu MiB "
                         "prefetch=%llu evict=%llu used=%llu/%llu MiB wait=%.1f ms\n",
                         tag, static_cast<unsigned long long>(s.hits), static_cast<unsigned long long>(s.misses),
                         static_cast<unsigned long long>(s.bytesServed >> 20),
                         static_cast<unsigned long long>(s.bytesFromDisk >> 20),
                         static_cast<unsigned long long>(s.bypassBytes >> 20),
                         static_cast<unsigned long long>(s.prefetchBlocks), static_cast<unsigned long long>(s.evictions),
                         static_cast<unsigned long long>(s.used >> 20), static_cast<unsigned long long>(s.budget >> 20),
                         static_cast<double>(s.waitNs) / 1.0e6);
        }

        uint64_t m_budget = 0;
        size_t m_blockSize = 1u << 20;
        uint64_t m_preloadMax = 0;
        std::vector<std::string> m_skip;
        bool m_log = false;

        mutable std::mutex m_mutex; // registro de arquivos
        std::unordered_map<std::string, uint32_t> m_byPath;
        std::unordered_map<uint32_t, std::shared_ptr<File>> m_files;
        uint32_t m_nextId = 1;

        mutable std::mutex m_blockMutex; // blocos + LRU
        std::condition_variable m_blockCv;
        std::unordered_map<uint64_t, std::shared_ptr<Block>> m_blocks;
        std::list<uint64_t> m_lru;
        uint64_t m_used = 0;

        std::mutex m_queueMutex;
        std::condition_variable m_queueCv;
        std::deque<std::pair<uint32_t, uint64_t>> m_queue;
        bool m_stop = false;
        std::thread m_worker;

        std::atomic<uint64_t> m_hits{0}, m_misses{0}, m_bytesServed{0}, m_bytesFromDisk{0}, m_bypassBytes{0};
        std::atomic<uint64_t> m_prefetchBlocks{0}, m_evictions{0}, m_waitNs{0};
    };
} // namespace ps2x
