// Teste isolado do cache de VFS: bytes identicos ao caminho direto (arquivo sintetico).
#include "runtime/ps2_vfs_cache.h"
#include <cstdio>
#include <random>
#include <thread>

uint64_t ps2x::vfsAvailablePhysicalBytes() { return 8ull << 30; }

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main(int argc, char **argv)
{
    const std::string path = argc > 1 ? argv[1] : "vfs_cache_test.bin";
    const size_t fileSize = 5u * 1024u * 1024u + 12345u;
    std::vector<uint8_t> ref(fileSize);
    std::mt19937 rng(42);
    for (auto &b : ref) b = static_cast<uint8_t>(rng());
    { FILE *f = std::fopen(path.c_str(), "wb"); std::fwrite(ref.data(), 1, ref.size(), f); std::fclose(f); }

    // orcamento pequeno (3 blocos de 256 KiB) forca evicao; skip vazio
    for (int variant = 0; variant < 3; ++variant)
    {
        const uint64_t budget = variant == 0 ? 0u : (variant == 1 ? 768u * 1024u : 64u << 20);
        ps2x::VfsCache cache(budget, 256u * 1024u, variant == 2 ? 16u << 20 : 0u, {"MOV"}, variant == 2);
        const uint32_t id = cache.open(path);
        CHECK(id != 0);
        uint64_t sz = 0; CHECK(cache.size(id, sz) && sz == fileSize);
        std::vector<uint8_t> buf(3u << 20);
        auto check = [&](uint64_t off, size_t n) {
            size_t got = 12345;
            bool ok = cache.readAt(id, off, buf.data(), n, got);
            size_t expect = off >= fileSize ? 0u : std::min<size_t>(n, fileSize - off);
            CHECK(ok); CHECK(got == expect);
            if (expect) CHECK(std::memcmp(buf.data(), ref.data() + off, expect) == 0);
        };
        std::uniform_int_distribution<uint64_t> offd(0, fileSize + 5000);
        std::uniform_int_distribution<size_t> lend(0, 1u << 20);
        for (int i = 0; i < 400; ++i) check(offd(rng), lend(rng));
        for (uint64_t off = 0; off < fileSize; off += 2048) check(off, 2048); // sequencial por setor
        check(0, 0); check(fileSize, 10); check(fileSize - 1, 10); check(fileSize + 100, 10); check(262144 - 5, 10);
        // concorrencia
        std::vector<std::thread> ts;
        for (int t = 0; t < 4; ++t)
            ts.emplace_back([&, t] {
                std::mt19937 r(t); std::vector<uint8_t> b(1u << 20);
                for (int i = 0; i < 300; ++i) {
                    uint64_t off = r() % fileSize; size_t n = r() % (1u << 19); size_t got = 0;
                    bool ok = cache.readAt(id, off, b.data(), n, got);
                    size_t e = std::min<size_t>(n, fileSize - off);
                    if (!ok || got != e || std::memcmp(b.data(), ref.data() + off, e) != 0) { std::printf("FAIL thread\n"); ++fails; }
                }
            });
        for (auto &t : ts) t.join();
        auto s = cache.stats();
        std::printf("variant %d: hits=%llu misses=%llu used=%llu evict=%llu\n", variant, (unsigned long long)s.hits,
                    (unsigned long long)s.misses, (unsigned long long)s.used, (unsigned long long)s.evictions);
        CHECK(s.used <= budget + 256u * 1024u);
    }
    // arquivo inexistente e stream (MOV*)
    { ps2x::VfsCache c(64u << 20, 1u << 20, 0, {"MOV"}, false); CHECK(c.open("nao_existe.bin") == 0); }
    std::remove(path.c_str());
    std::printf(fails ? "FAILED %d\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
