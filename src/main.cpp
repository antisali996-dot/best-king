// ============================================================================
// best-king — 独立内存 dump 工具（Paradise 内核驱动专用）
// ----------------------------------------------------------------------------
//  · 硬闸门: 启动即连接 par 内核驱动并做双重验证（自检读 + 目标读）;
//    连接不成功 → 打印原因并退出, 所有 dump 功能一概不启动。
//  · 全内存读通道只经 par 驱动 ioctl, 无任何旁路回退。
//
//  用法: King.sh [--target pkg|pid] [--chunk N] <命令> ...
//    selftest                              驱动连接自检（闸门流程单跑）
//    mods   [过滤]                          模块列表
//    maps   [过滤]                          maps 打印
//    range  <hex起始> <hex长度> <out.bin>   任意区间 dump
//    dumpgame <out.bin> <out.idx>           一键圈选匿名堆区+[anon:.bss]（索引含完整 maps）
//    skilldump <out.txt>                    单位列表+技能槽 一键采样
//  默认目标: com.tencent.tmgp.sgame
// ============================================================================
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <chrono>
#include <unistd.h>
#include "paradise_api.h"

// ---------------- 常量 / 全局 ----------------
static const char* kDefaultTarget = "com.tencent.tmgp.sgame";

static paradise_driver* g_drv = nullptr;   // par 驱动实例（闸门后有效）
static pid_t  g_pid = 0;                   // 目标进程
static size_t g_chunk = 4096;              // 读块大小
static bool   g_chunk_auto = true;         // 是否自动探测

// ---------------- 基础结构 ----------------
struct Region {
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t offset = 0;
    char perms[8] = {0};
    std::string path;
};

static std::string TrimLead(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    return (b == std::string::npos) ? std::string() : s.substr(b);
}

// 进程内直接读 /proc/<pid>/maps（仅用于取地址区间，不是内存读）
static std::vector<Region> ReadMaps(pid_t pid) {
    std::vector<Region> out;
    char fn[64];
    snprintf(fn, sizeof(fn), "/proc/%d/maps", (int)pid);
    FILE* fp = fopen(fn, "r");
    if (!fp) return out;
    char line[640];
    while (fgets(line, sizeof(line), fp)) {
        unsigned long long s = 0, e = 0, off = 0;
        char perms[8] = {0}, path[512] = {0};
        int n = sscanf(line, "%llx-%llx %7s %llx %*s %*s %511[^\n]",
                       &s, &e, &perms[0], &off, path);
        Region r;
        r.start = s;
        r.end = e;
        r.offset = off;
        strncpy(r.perms, perms, 7);
        if (n >= 5) r.path = TrimLead(path);
        out.push_back(std::move(r));
    }
    fclose(fp);
    return out;
}

static bool IsReadable(const Region& r) { return r.perms[0] == 'r'; }

// pidof 兜底（驱动取 pid 失败时）
static pid_t ResolvePidFallback(const std::string& a) {
    if (!a.empty() && a.find_first_not_of("0123456789") == std::string::npos)
        return (pid_t)strtol(a.c_str(), nullptr, 10);
    FILE* fp = popen(("pidof " + a).c_str(), "r");
    if (!fp) return 0;
    long v = 0;
    if (fscanf(fp, "%ld", &v) != 1) v = 0;
    pclose(fp);
    return (pid_t)v;
}

static bool DrvReadAt(uint64_t addr, void* buf, size_t size) {
    if (!g_drv || size == 0) return false;
    return g_drv->read((uintptr_t)addr, buf, size);
}

// 读块自适应探测（65536 → 256 逐档降级）
static void ProbeChunk(uint64_t addr) {
    if (!g_chunk_auto) {
        printf("[*] 读块大小（指定）: %zu\n", g_chunk);
        return;
    }
    static const size_t sizes[] = {65536, 32768, 16384, 8192, 4096, 2048, 1024, 512, 256};
    std::vector<uint8_t> buf(65536);
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        const size_t s = sizes[i];
        if (DrvReadAt(addr, buf.data(), s)) {
            g_chunk = s;
            printf("[*] 读块大小: %zu\n", s);
            return;
        }
    }
    g_chunk = 256;
    printf("[!] 读块探测全部失败, 回落到 256（可能大面积读失败）\n");
}

// ================= 硬闸门: par 驱动连接 =================
// 任一步失败 → 返回非 0, 主程序不启动
static int Gatekeeper(const std::string& target) {
    printf("==================================================\n");
    printf("        best-king · par 驱动硬闸门\n");
    printf("==================================================\n");

    // [1] 连接: 构造驱动（内核 fd 安装，失败则后续读必错）
    printf("[1/4] 连接 par 驱动（内核 ioctl）...\n");
    try {
        g_drv = new paradise_driver();
    } catch (...) {
        g_drv = nullptr;
    }
    if (!g_drv) {
        printf("[X] 驱动对象构造失败（异常/资源不足）\n");
        return 11;
    }

    // [2] 自检读: 经驱动读本进程 canary，验证整条链路
    printf("[2/4] 驱动自检: 读取本进程内存 ...\n");
    volatile uint64_t canary = 0x7061725F4F4B3131ull;   // "par_OK11"
    g_drv->initialize(getpid());
    uint64_t got = 0;
    if (!g_drv->read((uintptr_t)&canary, &got, sizeof(got)) || got != (uint64_t)canary) {
        printf("[X] par 驱动连接失败: 自检读未通过（驱动未刷入/未生效）\n");
        return 11;
    }
    printf("\n[OK] ★ par 驱动连接成功 ★  (自检读通过 canary=0x%016" PRIx64 ")\n\n",
           (uint64_t)canary);

    // [3] 内核层解析目标 pid（pidof 兜底）
    printf("[3/4] 解析目标进程: %s ...\n", target.c_str());
    pid_t pid = g_drv->get_pid(target.c_str());
    if (pid <= 0) pid = ResolvePidFallback(target);
    if (pid <= 0) {
        printf("[X] 目标进程未在运行\n");
        return 11;
    }
    g_pid = pid;
    g_drv->initialize(pid);
    printf("      pid = %d\n", (int)pid);

    // [4] 目标读验证: maps 首个可读区试读 32 字节
    printf("[4/4] 目标进程读验证 ...\n");
    std::vector<Region> maps = ReadMaps(pid);
    uint64_t probeAddr = 0;
    for (size_t i = 0; i < maps.size(); ++i) {
        if (IsReadable(maps[i]) && maps[i].end - maps[i].start >= 0x2000) {
            probeAddr = maps[i].start + 0x100;
            break;
        }
    }
    uint8_t probe[32] = {0};
    if (probeAddr == 0 || !DrvReadAt(probeAddr, probe, sizeof(probe))) {
        printf("[X] 目标读验证失败（驱动未生效 / 进程受保护）\n");
        return 11;
    }

    printf("\n[OK] ★ 前置检查全部通过 ★  目标=%s  pid=%d  读验证=OK\n",
           target.c_str(), (int)pid);
    ProbeChunk(probeAddr);
    printf("\n");
    return 0;
}

// ================= 子命令 =================
static int CmdMods(const std::string& filter) {
    std::vector<Region> maps = ReadMaps(g_pid);
    if (maps.empty()) {
        printf("[!] 读取 maps 失败\n");
        return 1;
    }
    struct Mod { uint64_t base, end; std::string path; int segs; };
    std::vector<Mod> mods;
    for (size_t i = 0; i < maps.size(); ++i) {
        const Region& r = maps[i];
        if (r.path.empty() || r.path[0] != '/') continue;
        if (!filter.empty() && r.path.find(filter) == std::string::npos) continue;
        bool merged = false;
        for (size_t k = 0; k < mods.size(); ++k) {
            if (mods[k].path == r.path) {
                if (r.start < mods[k].base) mods[k].base = r.start;
                if (r.end > mods[k].end) mods[k].end = r.end;
                mods[k].segs++;
                merged = true;
                break;
            }
        }
        if (!merged) {
            Mod m;
            m.base = r.start;
            m.end = r.end;
            m.path = r.path;
            m.segs = 1;
            mods.push_back(std::move(m));
        }
    }
    std::sort(mods.begin(), mods.end(),
              [](const Mod& a, const Mod& b) { return a.base < b.base; });
    printf("%-14s %10s %5s  %s\n", "BASE", "SIZE", "SEGS", "PATH");
    for (size_t i = 0; i < mods.size(); ++i) {
        printf("0x%012" PRIx64 " %8" PRIu64 "K %5d  %s\n",
               mods[i].base, (mods[i].end - mods[i].base) / 1024, mods[i].segs,
               mods[i].path.c_str());
    }
    printf("[*] 共 %zu 个模块\n", mods.size());
    return 0;
}

static int CmdMaps(const std::string& filter) {
    std::vector<Region> maps = ReadMaps(g_pid);
    if (maps.empty()) {
        printf("[!] 读取 maps 失败\n");
        return 1;
    }
    int shown = 0;
    for (size_t i = 0; i < maps.size(); ++i) {
        const Region& r = maps[i];
        if (!filter.empty() && r.path.find(filter) == std::string::npos) continue;
        printf("%012" PRIx64 "-%012" PRIx64 " %s %08" PRIx64 " %s\n",
               r.start, r.end, r.perms, r.offset, r.path.c_str());
        shown++;
    }
    printf("[*] 共 %d 条（总 %zu 条）\n", shown, maps.size());
    return 0;
}

static int CmdRange(uint64_t addr, uint64_t size, const std::string& out) {
    if (size == 0) {
        printf("[!] 长度为 0\n");
        return 2;
    }
    if (size > (8ull << 30)) {   // 防手误(如多打一位): 上限 8GB
        printf("[!] 长度异常（>8GB），已拒绝: 0x%" PRIx64 "\n", size);
        return 2;
    }
    ProbeChunk(addr);
    FILE* f = fopen(out.c_str(), "wb");
    if (!f) {
        printf("[!] 无法写文件: %s\n", out.c_str());
        return 1;
    }
    std::vector<uint8_t> buf(g_chunk);
    uint64_t done = 0, fails = 0;
    while (done < size) {
        size_t n = (size_t)std::min<uint64_t>(g_chunk, size - done);
        if (!DrvReadAt(addr + done, buf.data(), n)) {
            fails += n;
            memset(buf.data(), 0, n);
        }
        if (fwrite(buf.data(), 1, n, f) != n) {
            printf("[!] 写盘失败（空间不足?）: %s\n", out.c_str());
            fclose(f);
            return 1;
        }
        done += n;
        if (done % (64ull << 20) < (uint64_t)g_chunk) {
            printf("    ... %" PRIu64 " / %" PRIu64 " MB\n", done >> 20, size >> 20);
        }
    }
    fclose(f);
    printf("[OK] range 0x%" PRIx64 " + 0x%" PRIx64 " -> %s（读失败 %" PRIu64 " 字节）\n",
           addr, size, out.c_str(), fails);
    return 0;
}

// 一键圈选"分析用"内存: game 侧匿名堆区 + 全部 [anon:.bss]
// 输出: 单个连续 bin + 索引文本（含完整 maps 副本, "#MAP " 前缀行）
static int CmdDumpGame(const std::string& out_bin, const std::string& out_idx, const std::string& out_status) {
    std::vector<Region> maps = ReadMaps(g_pid);
    if (maps.empty()) {
        printf("[!] 读取 maps 失败\n");
        return 1;
    }
    struct Sel { uint64_t start, size; std::string name; };
    std::vector<Sel> sels;
    const uint64_t LIMIT_TOTAL = 6000ull << 20;
    const uint64_t LIMIT_ONE = 2048ull << 20;
    const uint64_t MIN_ONE  = 4ull << 10;     // 4KB (原 256KB 会漏掉小匿名区)
    uint64_t total = 0;
    int skipped = 0;
    for (size_t i = 0; i < maps.size(); ++i) {
        const Region& r = maps[i];
        if (!IsReadable(r)) continue;
        const bool isBss = (r.path.find(".bss") != std::string::npos);
        const bool inWin = (r.start >= 0x7000000000ull && r.start < 0x7C00000000ull);
        if (!isBss && !inWin) continue;
        const bool anonPath = r.path.empty() || r.path[0] == '[';
        // 文件映射: 仅纳入 /lib/arm64/ 下的可写数据段(.data)，跳过代码/只读/dalvik
        const bool isLibData = (r.path.find("/lib/arm64/") != std::string::npos) &&
                               (strchr(r.perms, 'w') != nullptr);
        if (!anonPath && !isLibData) continue;
        uint64_t sz = r.end - r.start;
        if (sz < MIN_ONE) continue;
        if (total >= LIMIT_TOTAL) {
            skipped++;
            continue;
        }
        uint64_t use = std::min<uint64_t>(sz, LIMIT_ONE);
        if (total + use > LIMIT_TOTAL) use = LIMIT_TOTAL - total;
        Sel s;
        s.start = r.start;
        s.size = use;
        s.name = r.path.empty() ? std::string("[anon]") : r.path;
        sels.push_back(std::move(s));
        total += use;
    }
    printf("[*] dumpgame: 选中 %zu 个区, 共 %" PRIu64 " MB%s\n",
           sels.size(), total >> 20, skipped ? "（有区域因总量上限被跳过）" : "");
    for (size_t i = 0; i < sels.size(); ++i) {
        printf("    0x%012" PRIx64 " + %8" PRIu64 " KB  %s\n",
               sels[i].start, sels[i].size >> 10, sels[i].name.c_str());
    }
    if (sels.empty()) {
        printf("[!] 没有符合条件的区间\n");
        return 1;
    }
    ProbeChunk(sels[0].start + 0x100);

    FILE* fb = fopen(out_bin.c_str(), "wb");
    FILE* fi = fopen(out_idx.c_str(), "w");
    FILE* fs = nullptr;
    if (!out_status.empty()) fs = fopen(out_status.c_str(), "w");
    if (!fb || !fi || (!out_status.empty() && !fs)) {
        printf("[!] 无法写文件: %s / %s / %s\n", out_bin.c_str(), out_idx.c_str(), out_status.c_str());
        if (fb) fclose(fb);
        if (fi) fclose(fi);
        if (fs) fclose(fs);
        return 1;
    }
    for (size_t i = 0; i < maps.size(); ++i) {
        fprintf(fi, "#MAP %012" PRIx64 "-%012" PRIx64 " %s %08" PRIx64 " %s\n",
                maps[i].start, maps[i].end, maps[i].perms, maps[i].offset,
                maps[i].path.c_str());
    }
    fprintf(fi, "# off start size name\n");

    std::vector<uint8_t> buf(g_chunk);
    std::vector<std::pair<uint64_t, uint64_t>> failRanges;   // 逐块失败区间 (addr, size)
    uint64_t failDropped = 0;                                // 超过记录上限而未能记录的失败块数
    uint64_t foff = 0, fails = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < sels.size(); ++i) {
        fprintf(fi, "%" PRIu64 " 0x%012" PRIx64 " %" PRIu64 " %s\n",
                foff, sels[i].start, sels[i].size, sels[i].name.c_str());
        uint64_t done = 0;
        while (done < sels[i].size) {
            size_t n = (size_t)std::min<uint64_t>(g_chunk, sels[i].size - done);
            bool ok = DrvReadAt(sels[i].start + done, buf.data(), n);
            if (!ok) {
                fails += n;
                if (failRanges.size() < 200000) failRanges.push_back({sels[i].start + done, (uint64_t)n});
                else failDropped++;
                memset(buf.data(), 0, n);
            }
            if (fs) {
                double tms = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - t0).count();
                fprintf(fs, "{\"addr\":\"0x%012" PRIx64 "\",\"req\":%zu,\"got\":%zu,\"ok\":%d,\"t_ms\":%.1f}\n",
                        sels[i].start + done, n, ok ? n : (size_t)0, ok ? 1 : 0, tms);
            }
            if (fwrite(buf.data(), 1, n, fb) != n) {
                printf("[!] 写盘失败（空间不足?）: %s\n", out_bin.c_str());
                fclose(fb);
                fclose(fi);
                if (fs) fclose(fs);
                return 1;
            }
            done += n;
            foff += n;
            if (foff % (64ull << 20) < (uint64_t)g_chunk) {
                printf("    ... %" PRIu64 " / %" PRIu64 " MB\n", foff >> 20, total >> 20);
            }
        }
        fflush(fb);
    }
    // 逐块失败区间（供离线区分: 未选择 / 失败填零 / 真实零值）
    for (const auto& fr : failRanges) {
        fprintf(fi, "#FAIL %012" PRIx64 " %" PRIu64 "\n", fr.first, fr.second);
    }
    if (failDropped) fprintf(fi, "#FAIL-TRUNC %" PRIu64 "\n", failDropped);
    fclose(fb);
    fclose(fi);
    if (fs) fclose(fs);
    printf("[OK] dumpgame -> %s (%" PRIu64 " MB), 索引 -> %s（读失败 %" PRIu64 " 字节, %zu 块）\n",
           out_bin.c_str(), foff >> 20, out_idx.c_str(), fails, failRanges.size());
    if (fails > 0) printf("[!] 有读失败（已填零，索引含 #FAIL 区间）\n");
    return 0;
}

// ================= HW 观察点扫描 (Plan B: 字段写入者定位) =================
// 用法: hwbp_scan <hexObjAddr> <dur_ms> [wrw|w|x]
// 一次在对象上挂 16 个硬件观察点, 覆盖 actor 已知 component 槽位族,
// 采样 dur_ms 后按点位输出 PC/LR/寄存器现场 (用于离线锁 pos/hp 链)。
static int CmdHwbpScan(uint64_t obj, uint32_t dur_ms, uint32_t mode) {
    if (g_pid <= 0) return 2;
    g_drv->initialize(g_pid);

    static const uint64_t kOffs[] = {
        0x130, 0x138, 0x140, 0x158, 0x170, 0x188, 0x1A0, 0x1B8,
        0x200, 0x218, 0x230, 0x250, 0x268, 0x278, 0x280, 0x2C8
    };
    const size_t N = sizeof(kOffs) / sizeof(kOffs[0]);
    std::vector<paradise_hwbp_point_config> pts(N);
    for (size_t i = 0; i < N; ++i) {
        pts[i].address = obj + kOffs[i];
        pts[i].type = mode ? mode : PARADISE_HWBP_READ_WRITE;
        pts[i].length = 8;
        pts[i].scope = PARADISE_HWBP_ALL_THREADS;
        pts[i].reserved = 0;
    }
    printf("[*] hwbp_scan: obj=0x%" PRIx64 " %dms mode=%u, 挂 16 点\n", obj, dur_ms, mode);
    if (!g_drv->hwbp_set(pts.data(), N)) {
        printf("[X] hwbp_set 失败 (驱动/内核不支持观察点)\n");
        return 1;
    }
    auto sleep_ms = [](unsigned ms) { struct timespec ts{ms / 1000, (ms % 1000) * 1000000}; nanosleep(&ts, nullptr); };
    uint32_t total_hits = 0;
    for (uint32_t waited = 0; waited < dur_ms; waited += 200) {
        sleep_ms(200);
        uint32_t got = 0;
        g_drv->hwbp_get_records(0, 0, nullptr, 0, &got);   // 探计数(容量0)
        // 部分后端 capacity=0 无效: 改为直接读取头部记录
        if (got == 0) {
            static paradise_hwbp_record probe[4];
            uint32_t n0 = 0;
            if (g_drv->hwbp_get_records(0, 0, probe, 4, &n0)) {
                got = n0;
                if (n0 >= 4) got = 4;   // >=4 表示还在持续命中
            }
        }
        total_hits = got;
        if (got > 0) {
            if (got >= 4) break;                       // 已有记录, 提前结束
            break;
        }
    }
    printf("[*] 采样完成, 读取现场 ...\n");
    const uint32_t CAP = 16;
    static paradise_hwbp_record recs[CAP];
    for (size_t i = 0; i < N; ++i) {
        uint32_t n = 0;
        if (!g_drv->hwbp_get_records((uint32_t)i, 0, recs, CAP, &n)) continue;
        if (n == 0) continue;
        printf("---- point[%zu] +0x%" PRIx64 " %u hits ----\n", i, kOffs[i], (unsigned)n);
        for (uint32_t r = 0; r < n && r < CAP; ++r) {
            printf("  hit%u pc=0x%" PRIx64 " lr=0x%" PRIx64
                   " x0=0x%" PRIx64 " x1=0x%" PRIx64 " x2=0x%" PRIx64
                   " x8=0x%" PRIx64 " x19=0x%" PRIx64 " x20=0x%" PRIx64 "\n",
                   r, recs[r].pc, recs[r].lr, recs[r].x0, recs[r].x1, recs[r].x2,
                   recs[r].x8, recs[r].x19, recs[r].x20);
        }
    }
    g_drv->hwbp_remove();
    g_drv->hwbp_clear_records(-1);
    printf(total_hits ? "[OK] hwbp_scan 完成 (有命中, 寄存器已抄录)\n"
                      : "[!] %dms 内无命中 — 对象地址可能未激活/字段无人写\n", dur_ms);
    return 0;
}

// ---------------- bpauto: 自动取根->L1->recarray->wrapper[0]->挂点 ----------------
// 用法: bpauto <dur_ms> [mode]   (需在战斗中, 对象字段才有更新产生命中)
static int CmdBpAuto(uint32_t dur_ms, uint32_t mode) {
    if (g_pid <= 0) return 2;
    g_drv->initialize(g_pid);

    // [1] 定 libGameCore bss: 文件段最后一段末尾后的第一个 [anon:.bss]
    std::vector<Region> maps = ReadMaps(g_pid);
    uint64_t bss = 0;
    for (size_t i = 0; i < maps.size(); ++i) {
        if (maps[i].path.find("libGameCore.so") == std::string::npos) continue;
        for (size_t j = i + 1; j < maps.size(); ++j) {
            if (maps[j].path.find(".bss") != std::string::npos &&
                maps[j].start >= maps[i].end) { bss = maps[j].start; break; }
        }
        if (bss) break;
    }
    if (!bss) { printf("[X] 未找到 libGameCore .bss\n"); return 1; }
    printf("[*] bss = 0x%" PRIx64 "\n", bss);

    // [2] bss 自证家族找 engine_root (root[0]==root, 最多扫 4MB)
    uint64_t root = 0;
    const size_t CHUNK = 0x40000;                       // 256KB, 不越 bss 边界
    std::vector<uint8_t> blob(CHUNK);
    for (uint64_t off = 0; off < (8ull << 20); off += CHUNK) {
        if (!DrvReadAt(bss + off, blob.data(), CHUNK)) continue;
        const uint64_t* U = reinterpret_cast<const uint64_t*>(blob.data());
        for (size_t i = 0; i < CHUNK / 8; ++i) {
            uint64_t v = U[i] & 0xFFFFFFFFFFFFull;
            if (v < 0x7000000000ull) continue;
            uint64_t w0 = 0;
            if (!DrvReadAt(v, &w0, 8)) continue;
            if ((w0 & 0xFFFFFFFFFFFFull) != v) continue;
            // root+0x3D0 -> L1, 验证 L1+0x58/0x5C == 16/10
            uint64_t l1v = 0;
            if (!DrvReadAt(v + 0x3D0, &l1v, 8)) continue;
            l1v &= 0xFFFFFFFFFFFFull;
            uint32_t a = 0, b2 = 0, recv = 0;
            if (DrvReadAt(l1v + 0x58, &a, 4) && a == 16 &&
                DrvReadAt(l1v + 0x5C, &b2, 4) && b2 == 10) {
                if (DrvReadAt(l1v + 0x40, &recv, 8)) {
                    root = v; break;
                }
            }
        }
        if (root) break;
    }
    if (!root) { printf("[X] engine_root 自证未命中\n"); return 1; }
    printf("[*] root = 0x%" PRIx64 "\n", root);

    uint64_t l1 = 0; DrvReadAt(root + 0x3D0, &l1, 8); l1 &= 0xFFFFFFFFFFFFull;
    printf("[*] L1 = 0x%" PRIx64 "\n", l1);
    uint64_t rec = 0; DrvReadAt(l1 + 0x40, &rec, 8); rec &= 0xFFFFFFFFFFFFull;
    printf("[*] recarray = 0x%" PRIx64 "\n", rec);

    uint64_t wrap0 = 0;
    for (int i = 0; i < 10; ++i) {
        uint32_t p = 0;
        if (!DrvReadAt(rec + 0x20ull * i + 0x08, &p, 4) || !p) continue;
        wrap0 = (0x73ull << 32) | p;
        break;
    }
    if (!wrap0) { printf("[X] wrapper[0] 为空 (未在对局内?)\n"); return 1; }
    printf("[*] wrapper[0] = 0x%" PRIx64 "\n", wrap0);

    // [3] 深探: 读 W+0x188 指针 -> P; 读得到则挂 P 的值字段并打印现值
    uint64_t sv = 0;
    if (DrvReadAt(wrap0 + 0x188, &sv, 8)) {
        uint64_t P = sv & 0xFFFFFFFFFFFFull;
        if (P >= 0x7000000000ull) {
            printf("[*] W+0x188 -> P = 0x%" PRIx64 "\n", P);
            uint8_t pb[16] = {0};
            if (DrvReadAt(P + 0xA8, pb, 16)) {
                uint32_t x = 0, z = 0;
                memcpy(&x, pb, 4); memcpy(&z, pb + 8, 4);
                printf("[*] P+0xA8..B7 = ");
                for (int i = 0; i < 16; ++i) printf("%02x", pb[i]);
                printf("  (int32@0=%d @8=%d → 米(%d,%d))\n", (int)x, (int)z, (int)x/100, (int)z/100);
                if (x || z) {
                    uint32_t mode2 = mode ? mode : PARADISE_HWBP_READ_WRITE;
                    static const uint64_t kT[] = { 0xA8, 0xB0 };
                    std::vector<paradise_hwbp_point_config> pts(2);
                    for (size_t i = 0; i < 2; ++i) {
                        pts[i].address = P + kT[i];
                        pts[i].type = mode2;
                        pts[i].length = 8;
                        pts[i].scope = PARADISE_HWBP_ALL_THREADS;
                        pts[i].reserved = 0;
                    }
                    printf("[*] 挂点 P+0xA8/P+0xB0, 采样 %ums\n", dur_ms);
                    if (g_drv->hwbp_set(pts.data(), 2)) {
                        struct timespec ts{dur_ms / 1000, (dur_ms % 1000) * 1000000};
                        nanosleep(&ts, nullptr);
                        static paradise_hwbp_record recs[8];
                        for (size_t i = 0; i < 2; ++i) {
                            uint32_t n = 0;
                            if (!g_drv->hwbp_get_records((uint32_t)i, 0, recs, 8, &n)) continue;
                            printf("---- P+0x%llx hits=%u ----\n", (unsigned long long)kT[i], (unsigned)n);
                            for (uint32_t r = 0; r < n && r < 8; ++r)
                                printf("  hit%u pc=0x%" PRIx64 " lr=0x%" PRIx64 " x0=0x%" PRIx64 " x1=0x%" PRIx64
                                       " x2=0x%" PRIx64 " x8=0x%" PRIx64 " x19=0x%" PRIx64 " x20=0x%" PRIx64 "\n",
                                       r, recs[r].pc, recs[r].lr, recs[r].x0, recs[r].x1, recs[r].x2,
                                       recs[r].x8, recs[r].x19, recs[r].x20);
                        }
                        g_drv->hwbp_remove();
                        g_drv->hwbp_clear_records(-1);
                        return 0;
                    }
                }
            }
            // P 读不到: 落入通用 16 点位扫描 (挂 wrapper 本体)
        }
    }
    printf("[*] 深读不可用 → 回落 16 点位扫描\n");
    return CmdHwbpScan(wrap0, dur_ms, mode);
}

// ---------------- bpfollow: 读 W+0x188 指针 -> 挂点在指针目标 P+0xA8/P+0xB0/P+0x60 ----------------
// 用法: bpfollow <hexWrapper> <dur_ms> [mode]   (战斗中挂点命中率极高频)
static int CmdBpFollow(uint64_t W, uint32_t dur_ms, uint32_t mode) {
    if (g_pid <= 0) return 2;
    g_drv->initialize(g_pid);
    uint64_t sv = 0;
    if (!DrvReadAt(W + 0x188, &sv, 8)) {
        printf("[X] 读 0x%" PRIx64 "+0x188 失败 (隐藏区/未激活)\n", W); return 1;
    }
    uint64_t P = sv & 0xFFFFFFFFFFFFull;
    printf("[*] W=0x%" PRIx64 "  W+0x188=0x%" PRIx64 " \n", W, sv);
    if (P < 0x7000000000ull) { printf("[X] P 非法\n"); return 1; }
    printf("[*] P = 0x%" PRIx64 " (via W+0x188)\n", P);

    // 现值读取 (可能失败——失败也继续挂点)
    uint8_t posb[16] = {0};
    if (DrvReadAt(P + 0xA8, posb, 16)) {
        uint32_t x, z, hpv, hmv;
        memcpy(&x, posb, 4); memcpy(&z, posb + 8, 4);
        memcpy(&hpv, posb + 0, 4);
        printf("[*] 当前 P+0xA8..B7 = ");
        for (int i = 0; i < 16; ++i) printf("%02x", posb[i]);
        printf("  (int32@0=%d, @8=%d)\n", (int)x, (int)z);
    } else {
        printf("[!] P+0xA8 现值读失败 (仍可挂点)\n");
    }

    // 挂点: P+0xA8 / P+0xB0 (pos对) / P+0x60 / P+0x10 (alt 链)
    static const uint64_t kTargets[] = { 0xA8, 0xB0, 0x60, 0x10 };
    const size_t N = sizeof(kTargets) / sizeof(kTargets[0]);
    std::vector<paradise_hwbp_point_config> pts(N);
    for (size_t i = 0; i < N; ++i) {
        pts[i].address = P + kTargets[i];
        pts[i].type = mode ? mode : PARADISE_HWBP_READ_WRITE;
        pts[i].length = 8;
        pts[i].scope = PARADISE_HWBP_ALL_THREADS;
        pts[i].reserved = 0;
    }
    printf("[*] bpfollow: 挂 %d 点 (P+0xA8/0xB0/0x60/0x10, mode=%u)\n", (int)N, mode);
    if (!g_drv->hwbp_set(pts.data(), N)) { printf("[X] hwbp_set 失败\n"); return 1; }
    struct timespec ts{dur_ms / 1000, (dur_ms % 1000) * 1000000};
    nanosleep(&ts, nullptr);

    const uint32_t CAP = 12;
    static paradise_hwbp_record recs[CAP];
    uint32_t total = 0;
    for (size_t i = 0; i < N; ++i) {
        uint32_t n = 0;
        if (!g_drv->hwbp_get_records((uint32_t)i, 0, recs, CAP, &n)) continue;
        printf("---- P+0x%llx hits=%u ----\n", (unsigned long long)kTargets[i], (unsigned)n);
        for (uint32_t r = 0; r < n && r < CAP; ++r) {
            total += n;
            printf("  hit%u pc=0x%" PRIx64 " lr=0x%" PRIx64
                   " x0=0x%" PRIx64 " x1=0x%" PRIx64 " x2=0x%" PRIx64
                   " x8=0x%" PRIx64 " x19=0x%" PRIx64 " x20=0x%" PRIx64 "\n",
                   r, recs[r].pc, recs[r].lr, recs[r].x0, recs[r].x1, recs[r].x2,
                   recs[r].x8, recs[r].x19, recs[r].x20);
        }
    }
    g_drv->hwbp_remove();
    g_drv->hwbp_clear_records(-1);
    if (total == 0)
        printf("[!] %ums 无命中 — P 未被写 (对象可能未活)\n", dur_ms);
    else
        printf("[OK] bpfollow 完成 (有命中)\n");
    return 0;
}

// ---------------- bpwar: 观察 recarray 10 个槽位 (+0x08/+0x0C 交替), 覆盖换局时机 ----------------
// 用法: bpwar <dur_ms> [mode]  (挂上后什么都不用做, 等一场对局从载入到战斗全程, 打印每个新 wrapper 写入点)
static int CmdBpWar(uint32_t dur_ms, uint32_t mode) {
    if (g_pid <= 0) return 2;
    g_drv->initialize(g_pid);
    std::vector<Region> maps = ReadMaps(g_pid);
    uint64_t bss = 0;
    for (size_t i = 0; i < maps.size(); ++i) {
        if (maps[i].path.find("libGameCore.so") == std::string::npos) continue;
        for (size_t j = i + 1; j < maps.size(); ++j) {
            if (maps[j].path.find(".bss") != std::string::npos &&
                maps[j].start >= maps[i].end) { bss = maps[j].start; break; }
        }
        if (bss) break;
    }
    if (!bss) { printf("[X] 未找到 libGameCore .bss\n"); return 1; }

    uint64_t root = 0;
    const size_t CHUNK = 0x40000;
    std::vector<uint8_t> blob(CHUNK);
    for (uint64_t off = 0; off < (8ull << 20); off += CHUNK) {
        if (!DrvReadAt(bss + off, blob.data(), CHUNK)) continue;
        const uint64_t* U = reinterpret_cast<const uint64_t*>(blob.data());
        for (size_t i = 0; i < CHUNK / 8; ++i) {
            uint64_t v = U[i] & 0xFFFFFFFFFFFFull;
            if (v < 0x7000000000ull) continue;
            uint64_t w0 = 0, l1v = 0;
            if (!DrvReadAt(v, &w0, 8)) continue;
            if ((w0 & 0xFFFFFFFFFFFFull) != v) continue;
            if (!DrvReadAt(v + 0x3D0, &l1v, 8)) continue;
            l1v &= 0xFFFFFFFFFFFFull;
            uint32_t a = 0, b2 = 0, recv = 0;
            if (DrvReadAt(l1v + 0x58, &a, 4) && a == 16 &&
                DrvReadAt(l1v + 0x5C, &b2, 4) && b2 == 10 &&
                DrvReadAt(l1v + 0x40, &recv, 8)) { root = v; break; }
        }
        if (root) break;
    }
    if (!root) { printf("[X] engine_root 自证未命中\n"); return 1; }
    uint64_t l1 = 0; DrvReadAt(root + 0x3D0, &l1, 8); l1 &= 0xFFFFFFFFFFFFull;
    uint64_t rec = 0; DrvReadAt(l1 + 0x40, &rec, 8); rec &= 0xFFFFFFFFFFFFull;
    printf("[*] bpwar: root=0x%" PRIx64 " recarray=0x%" PRIx64 " dur=%ums mode=%u\n",
           root, rec, dur_ms, mode);

    // 16 点: k=0..9 的 +0x08 (写新 wrapper 指针), +k=0..5 的 +0x0C (写 b400 系头)
    std::vector<paradise_hwbp_point_config> pts(16);
    for (size_t i = 0; i < 16; ++i) {
        uint64_t o = (i < 10) ? (0x20ull * i + 0x08) : (0x20ull * (i - 10) + 0x0C);
        pts[i].address = rec + o;
        pts[i].type = mode ? mode : PARADISE_HWBP_READ_WRITE;
        pts[i].length = 8;
        pts[i].scope = PARADISE_HWBP_ALL_THREADS;
        pts[i].reserved = 0;
    }
    printf("[*] 挂 16 点 (slot+0x08 ×10, slot+0x0C ×6) ... 开始观察 (此期间请从 载入→战斗 打完整一局)\n");
    if (!g_drv->hwbp_set(pts.data(), 16)) { printf("[X] hwbp_set 失败\n"); return 1; }
    struct timespec ts{dur_ms / 1000, (dur_ms % 1000) * 1000000};
    nanosleep(&ts, nullptr);

    const uint32_t CAP = 16;
    static paradise_hwbp_record recs[CAP];
    uint32_t grand = 0;
    for (size_t i = 0; i < 16; ++i) {
        uint32_t n = 0;
        if (!g_drv->hwbp_get_records((uint32_t)i, 0, recs, CAP, &n)) continue;
        uint64_t o = (i < 10) ? (0x20ull * i + 0x08) : (0x20ull * (i - 10) + 0x0C);
        printf("---- rec+0x%03llx hits=%u ----\n", (unsigned long long)o, (unsigned)n);
        for (uint32_t r = 0; r < n && r < CAP; ++r) {
            grand += 1;
            // x2 通常 = 写入的值 (新 wrapper 指针), x0 = 槽位地址
            printf("  hit%u pc=0x%" PRIx64 " x0=0x%" PRIx64 " x1=0x%" PRIx64 " x2=0x%" PRIx64
                   " x3=0x%" PRIx64 " x8=0x%" PRIx64 " lr=0x%" PRIx64 "\n",
                   r, recs[r].pc, recs[r].x0, recs[r].x1, recs[r].x2,
                   recs[r].x3, recs[r].x8, recs[r].lr);
        }
    }
    g_drv->hwbp_remove();
    g_drv->hwbp_clear_records(-1);
    printf(grand ? "[OK] bpwar 完成 (命中 %u)\n" : "[!] %ums 无命中 (整段时间不在换局?\n", dur_ms);
    return 0;
}

// ---------------- bplive: 全自动 (换局检测 -> 新wrapper -> P现值+观察点 -> live pos/hp) ----------------
// 用法: bplive [dur_ms] [mode]  开着它打一整局即可; 有任一 hero 字段更新就打印现值+写入者
static int CmdBpLive(uint32_t dur_ms, uint32_t mode) {
    if (g_pid <= 0) return 2;
    g_drv->initialize(g_pid);
    std::vector<Region> maps = ReadMaps(g_pid);
    uint64_t bss = 0;
    for (size_t i = 0; i < maps.size(); ++i) {
        if (maps[i].path.find("libGameCore.so") == std::string::npos) continue;
        for (size_t j = i + 1; j < maps.size(); ++j) {
            if (maps[j].path.find(".bss") != std::string::npos &&
                maps[j].start >= maps[i].end) { bss = maps[j].start; break; }
        }
        if (bss) break;
    }
    if (!bss) { printf("[X] 未找到 .bss\n"); return 1; }
    uint64_t root = 0;
    const size_t CHUNK = 0x40000;
    std::vector<uint8_t> blob(CHUNK);
    for (uint64_t off = 0; off < (8ull << 20) && !root; off += CHUNK) {
        if (!DrvReadAt(bss + off, blob.data(), CHUNK)) continue;
        const uint64_t* U = reinterpret_cast<const uint64_t*>(blob.data());
        for (size_t i = 0; i < CHUNK / 8; ++i) {
            uint64_t v = U[i] & 0xFFFFFFFFFFFFull;
            if (v < 0x7000000000ull) continue;
            uint64_t w0 = 0, l1v = 0;
            if (!DrvReadAt(v, &w0, 8)) continue;
            if ((w0 & 0xFFFFFFFFFFFFull) != v) continue;
            if (!DrvReadAt(v + 0x3D0, &l1v, 8)) continue;
            l1v &= 0xFFFFFFFFFFFFull;
            uint32_t a = 0, b2 = 0, recv = 0;
            if (DrvReadAt(l1v + 0x58, &a, 4) && a == 16 &&
                DrvReadAt(l1v + 0x5C, &b2, 4) && b2 == 10 &&
                DrvReadAt(l1v + 0x40, &recv, 8)) { root = v; break; }
        }
    }
    if (!root) { printf("[X] root 自证未命中\n"); return 1; }
    uint64_t l1 = 0; DrvReadAt(root + 0x3D0, &l1, 8); l1 &= 0xFFFFFFFFFFFFull;
    uint64_t rec = 0; DrvReadAt(l1 + 0x40, &rec, 8); rec &= 0xFFFFFFFFFFFFull;
    printf("[*] bplive: root=0x%" PRIx64 " L1=0x%" PRIx64 " recarray=0x%" PRIx64
           " dur=%ums mode=%u\n", root, l1, rec, dur_ms, mode);

    uint32_t prev[10] = {0};
    for (int k = 0; k < 10; ++k)
        DrvReadAt(rec + 0x20ull * k + 0x08, &prev[k], 4);
    printf("[*] baseline wrappointers: ");
    for (int k = 0; k < 10; ++k) printf("%08x ", prev[k]);
    printf("\n");

    // 挂 16 点在 recarray 槽位 (捕获换局写入)
    std::vector<paradise_hwbp_point_config> pts(16);
    for (size_t i = 0; i < 16; ++i) {
        uint64_t o = (i < 10) ? (0x20ull * i + 0x08) : (0x20ull * (i - 10) + 0x0C);
        pts[i].address = rec + o;
        pts[i].type = mode ? mode : PARADISE_HWBP_READ_WRITE;
        pts[i].length = 8;
        pts[i].scope = PARADISE_HWBP_ALL_THREADS;
        pts[i].reserved = 0;
    }
    if (!g_drv->hwbp_set(pts.data(), 16)) { printf("[X] hwbp_set 失败\n"); return 1; }
    printf("[*] recarray 16 点已挂 (等换局/首次指针写入). 开始轮询...\n");

    const uint32_t POLL = 2000;
    bool captured = false;
    uint64_t liveW = 0, liveP = 0;
    uint32_t loops = dur_ms / POLL;
    for (uint32_t it = 0; it < loops; ++it) {
        struct timespec ts{POLL / 1000, (POLL % 1000) * 1000000};
        nanosleep(&ts, nullptr);
        uint32_t cur[10]; int nch = 0; uint64_t chW = 0;
        for (int k = 0; k < 10; ++k) {
            uint32_t v = 0;
            DrvReadAt(rec + 0x20ull * k + 0x08, &v, 4);
            cur[k] = v;
            if (v != prev[k]) {
                printf("[chg] slot%d ptr %08x -> %08x (W=0x%llx)\n", k, prev[k], v,
                       (unsigned long long)((0x73ull << 32) | v));
                if (!chW) chW = (0x73ull << 32) | v;
                nch++;
            }
        }
        if (nch > 0 && !captured && chW) {
            // 深读 chW+0x188
            uint64_t sv = 0;
            if (DrvReadAt(chW + 0x188, &sv, 8)) {
                uint64_t P = sv & 0xFFFFFFFFFFFFull;
                if (P >= 0x7000000000ull) {
                    printf("[POINTER] W=0x%" PRIx64 " ->+0x188 P=0x%" PRIx64 "\n", chW, P);
                    uint8_t pb[16] = {0};
                    if (DrvReadAt(P + 0xA8, pb, 16)) {
                        uint32_t x = 0, z = 0;
                        memcpy(&x, pb, 4); memcpy(&z, pb + 8, 4);
                        printf("[POINTER] P+0xA8..B7 = ");
                        for (int i = 0; i < 16; ++i) printf("%02x", pb[i]);
                        printf("  (int32@0=%d @8=%d 米(%d.%02d,%d.%02d))\n",
                               (int)x, (int)z, (int)x/100, abs((int)x)%100, (int)z/100, abs((int)z)%100);
                    } else {
                        printf("[!] P+0xA8 现值读失败\n");
                    }
                    // 挂 P 的 2 点并取写入者
                    uint32_t m2 = mode ? mode : PARADISE_HWBP_READ_WRITE;
                    static const uint64_t kT[] = { 0xA8, 0xB0 };
                    std::vector<paradise_hwbp_point_config> pts2(2);
                    for (size_t i = 0; i < 2; ++i) {
                        pts2[i].address = P + kT[i];
                        pts2[i].type = m2; pts2[i].length = 8;
                        pts2[i].scope = PARADISE_HWBP_ALL_THREADS;
                        pts2[i].reserved = 0;
                    }
                    if (g_drv->hwbp_set(pts2.data(), 2)) {
                        printf("[*] 已切换到 P+0xA8/P+0xB0 观察点\n");
                        liveW = chW; liveP = P;
                        captured = true;
                        continue;   // 本次轮询结束, 下一轮开始 live pos 打印
                    }
                }
            } else {
                printf("[!] 深读 chW+0x188 失败 (页面仍隐藏/未映射), 继续轮询...\n");
            }
        }
        // live pos 打印
        if (captured && liveP) {
            uint8_t pb[16] = {0};
            if (DrvReadAt(liveP + 0xA8, pb, 16)) {
                uint32_t x = 0, z = 0;
                memcpy(&x, pb, 4); memcpy(&z, pb + 8, 4);
                printf("[live t=%ums] pos_raw=(%d,%d) 米(%.2f,%.2f)\n",
                       (it + 1) * POLL, (int)x, (int)z, (double)x / 100.0, (double)z / 100.0);
            }
            // 命中过后读一次记录 (写入者 PC 只打前几次)
            static bool printed_once = false;
            if (!printed_once) {
                static paradise_hwbp_record recs[8];
                for (size_t i = 0; i < 2; ++i) {
                    uint32_t n = 0;
                    if (g_drv->hwbp_get_records((uint32_t)i, 0, recs, 8, &n) && n) {
                        for (uint32_t r = 0; r < n && r < 8; ++r)
                            printf("[writer] P+0x%llx hit pc=0x%" PRIx64 " lr=0x%" PRIx64
                                   " x0=0x%" PRIx64 " x1=0x%" PRIx64 " x2=0x%" PRIx64
                                   " x8=0x%" PRIx64 " x19=0x%" PRIx64 " x20=0x%" PRIx64 "\n",
                                   (unsigned long long)(i ? 0xB0 : 0xA8), recs[r].pc, recs[r].lr,
                                   recs[r].x0, recs[r].x1, recs[r].x2,
                                   recs[r].x8, recs[r].x19, recs[r].x20);
                    }
                }
                printed_once = true;
            }
        }
        memcpy(prev, cur, sizeof(prev));
    }
    g_drv->hwbp_remove();
    g_drv->hwbp_clear_records(-1);
    printf(captured ? "[OK] bplive 完成 (捕获并导出现值+写入者)\n"
                    : "[!] %ums 无换局/无指针更新 (要覆盖一场完整载入->战斗)\n", dur_ms);
    return 0;
}

// ---------------- bpdig: 读 W+0xD8 内容块 -> 16点覆盖 P+0x18..0x98 → 抓全部字段写入者 ----------------
static int CmdBpDig(uint64_t W, uint32_t dur_ms, uint32_t mode) {
    if (g_pid <= 0) return 2;
    g_drv->initialize(g_pid);
    uint64_t sv = 0;
    if (!DrvReadAt(W + 0xD8, &sv, 8)) { printf("[X] 读 W+0xD8 失败\n"); return 1; }
    uint64_t P = sv & 0xFFFFFFFFFFFFull;
    if (P < 0x7000000000ull) { printf("[X] P 非法 (%#x)\n", (unsigned)sv); return 1; }
    printf("[*] W=0x%" PRIx64 " W+0xD8 -> P = 0x%" PRIx64 "\n", W, P);
    printf("[*] P 头部: ");
    for (int i = 0; i < 8; ++i) {
        uint64_t v; DrvReadAt(P + i * 8, &v, 8);
        printf("%016llx ", (unsigned long long)v);
    }
    printf("\n");
    const uint32_t NPTS = 16;
    std::vector<paradise_hwbp_point_config> pts(NPTS);
    for (size_t i = 0; i < NPTS; ++i) {
        pts[i].address = P + 0x18ull + 8ull * i;
        pts[i].type = mode ? mode : PARADISE_HWBP_READ_WRITE;
        pts[i].length = 8;
        pts[i].scope = PARADISE_HWBP_ALL_THREADS;
        pts[i].reserved = 0;
    }
    printf("[*] bpdig: 挂 P+0x18..0x98 16点 mode=%u, 采样 %dms...\n", mode, dur_ms);
    if (!g_drv->hwbp_set(pts.data(), NPTS)) { printf("[X] hwbp_set 失败\n"); return 1; }
    struct timespec ts{dur_ms / 1000, (dur_ms % 1000) * 1000000};
    nanosleep(&ts, nullptr);
    const uint32_t CAP = 12;
    static paradise_hwbp_record recs[CAP];
    uint32_t total = 0;
    for (size_t i = 0; i < NPTS; ++i) {
        uint32_t n = 0;
        if (!g_drv->hwbp_get_records((uint32_t)i, 0, recs, CAP, &n)) continue;
        if (!n) continue;
        printf("---- P+0x%03llx hits=%u ----\n", (unsigned long long)(0x18 + 8 * i), (unsigned)n);
        for (uint32_t r = 0; r < n && r < CAP; ++r) {
            total++;
            printf("  pc=0x%" PRIx64 " lr=0x%" PRIx64 " x0=0x%" PRIx64 " x1=0x%" PRIx64
                   " x2=0x%" PRIx64 " x3=0x%" PRIx64 " x8=0x%" PRIx64 " x19=0x%" PRIx64 " x20=0x%" PRIx64 "\n",
                   recs[r].pc, recs[r].lr, recs[r].x0, recs[r].x1, recs[r].x2,
                   recs[r].x3, recs[r].x8, recs[r].x19, recs[r].x20);
        }
    }
    g_drv->hwbp_remove();
    g_drv->hwbp_clear_records(-1);
    printf(total ? "[OK] bpdig 完成 (命中 %u)\n" : "[!] %ums 无命中\n", dur_ms);
    return 0;
}

// ---------------- bpsweep: 对指定对象全域做 16点×分块 移窗扫描 ----------------
static int CmdBpSweep(uint64_t obj, uint64_t span, uint32_t per_ms, uint32_t mode) {
    if (g_pid <= 0) return 2;
    g_drv->initialize(g_pid);
    uint64_t stripe = 0x80ull;   // 16 点 × 8B = 128B = 0x80
    const uint32_t NPTS = 16;
    std::vector<paradise_hwbp_point_config> pts(NPTS);
    uint32_t total = 0;
    for (uint64_t baseOff = 0; baseOff < span; baseOff += stripe) {
        for (size_t i = 0; i < NPTS; ++i) {
            pts[i].address = obj + baseOff + i * 8;
            pts[i].type = mode ? mode : PARADISE_HWBP_READ_WRITE;
            pts[i].length = 8;
            pts[i].scope = PARADISE_HWBP_ALL_THREADS;
            pts[i].reserved = 0;
        }
        if (!g_drv->hwbp_set(pts.data(), NPTS)) { printf("[X] hwbp_set 失败 @+%llx\n", (unsigned long long)baseOff); break; }
        struct timespec ts{per_ms / 1000, (per_ms % 1000) * 1000000};
        nanosleep(&ts, nullptr);
        static paradise_hwbp_record recs[4];
        for (size_t i = 0; i < NPTS; ++i) {
            uint32_t n = 0;
            if (!g_drv->hwbp_get_records((uint32_t)i, 0, recs, 4, &n)) continue;
            for (uint32_t r = 0; r < n && r < 4; ++r) {
                total += n;
                printf("obj+0x%04llx %s[hit%u] pc=0x%" PRIx64 " x0=0x%" PRIx64 " x1=0x%" PRIx64
                       " x2=0x%" PRIx64 " x8=0x%" PRIx64 " lr=0x%" PRIx64 "\n",
                       (unsigned long long)(baseOff + i * 8),
                       "", r, recs[r].pc, recs[r].x0, recs[r].x1, recs[r].x2, recs[r].x8, recs[r].lr);
            }
        }
        g_drv->hwbp_clear_records(-1);
    }
    g_drv->hwbp_remove();
    if (total) printf("[OK] bpsweep 完成 (命中 %u)\n", (unsigned)total);
    else printf("[!] 全域窗口无命中\n");
    return 0;
}

// ---------------- hwbp_selftest: 端到端验证驱动观察点是否真实工作 ----------------
static int CmdHwbpSelfTest(void) {
    if (g_pid > 0) g_drv->initialize(g_pid);
    volatile uint64_t myvar = 0;    // 会把读写落在自己栈/静态区的变量
    pid_t save = 0;
    // 用自己进程测试, 还原目标 pid
    FILE* f = fopen("/proc/self/stat", "r");
    if (f) { long v = 0; if (fscanf(f, "%ld", &v) == 1) save = (pid_t)v; fclose(f); }
    g_drv->initialize(save ? save : getpid());
    paradise_hwbp_point_config pc{}; pc.address = (uintptr_t)&myvar;
    pc.type = PARADISE_HWBP_READ_WRITE; pc.length = 8;
    pc.scope = PARADISE_HWBP_ALL_THREADS; pc.reserved = 0;
    if (!g_drv->hwbp_set(&pc, 1)) { printf("[X] hwbp_set 失败\n"); return 1; }
    myvar = 0x11112222;             // 触发一次写
    uint64_t tmp = myvar;           // 触发一次读
    printf("[*] 已写已读 0x%llx (期望 hit≥1)\n", (unsigned long long)tmp);
    struct timespec ts{0, 300 * 1000000};
    nanosleep(&ts, nullptr);
    static paradise_hwbp_record recs[8];
    uint32_t n = 0;
    int ok = g_drv->hwbp_get_records(0, 0, recs, 8, &n);
    printf("[*] get_records rc=%d count=%u\n", ok, (unsigned)n);
    for (uint32_t i = 0; i < n; ++i)
        printf("  hit%u pc=0x%" PRIx64 " x0=0x%" PRIx64 " lr=0x%" PRIx64 "\n", i, recs[i].pc, recs[i].x0, recs[i].lr);
    g_drv->hwbp_remove(); g_drv->hwbp_clear_records(-1);
    if (g_pid > 0) g_drv->initialize(g_pid);   // 还原目标
    printf(n ? "[OK] hwbp 链路正常 (自证命中 %u)\n" : "[X] hwbp 未产生记录 — 驱动观察点未生效\n", (unsigned)n);
    return n ? 0 : 2;
}

static bool DrvWriteAt(uint64_t addr, const void* buf, size_t size) {
    if (!g_drv || size == 0) return false;
    return g_drv->write((uintptr_t)addr, (void*)buf, size);
}

// ---- 写内存命令族 (读回自动验证; 测试建议: 读原值->写回同值->读回) ----
static int CmdRd(uint64_t addr, size_t len) {
    if (len == 0) { printf("[!] 长度 0\n"); return 2; }
    if (len > 4096) { printf("[!] 长度上限 4096\n"); return 2; }
    std::vector<uint8_t> buf(len);
    if (!DrvReadAt(addr, buf.data(), len)) { printf("[X] 读失败 %#llx\n", (unsigned long long)addr); return 1; }
    printf("[OK] rd %#llx + %zu:\n", (unsigned long long)addr, len);
    for (size_t i = 0; i < len; ++i) {
        if (i % 16 == 0) printf("  %04zx: ", i);
        printf("%02x ", buf[i]);
        if (i % 16 == 15 || i + 1 == len) printf("\n");
    }
    return 0;
}
static int CmdWriteVerify(const char* tag, uint64_t addr, const void* p, size_t n) {
    if (!DrvWriteAt(addr, p, n)) { printf("[X] %s 写失败 @%#llx\n", tag, (unsigned long long)addr); return 1; }
    std::vector<uint8_t> rb(n);
    bool ver = DrvReadAt(addr, rb.data(), n) && memcmp(rb.data(), p, n) == 0;
    printf("[OK] %s @%#llx + %zu 字节 读回=%s\n", tag, (unsigned long long)addr, n, ver ? "一致" : "不一致!");
    return ver ? 0 : 3;
}
static int CmdPoke(const std::string& addrS, const std::string& hexS) {
    uint64_t addr = strtoull(addrS.c_str(), nullptr, 16);
    if (hexS.empty() || (hexS.size() % 2)) { printf("[!] 字节串需偶数长度十六进制, 例: poke 0x1234 0102a0ff\n"); return 2; }
    if (hexS.size() > 8192) { printf("[!] 字节串上限 4096 字节\n"); return 2; }
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> buf;
    buf.reserve(hexS.size() / 2);
    for (size_t i = 0; i < hexS.size(); i += 2) {
        int hi = hexval(hexS[i]), lo = hexval(hexS[i + 1]);
        if (hi < 0 || lo < 0) { printf("[!] 非法十六进制字符\n"); return 2; }
        buf.push_back((uint8_t)((hi << 4) | lo));
    }
    return CmdWriteVerify("poke", addr, buf.data(), buf.size());
}
static int CmdW32(const std::string& addrS, const std::string& valS) {
    uint64_t addr = strtoull(addrS.c_str(), nullptr, 16);
    char* end = nullptr;
    unsigned long long v = strtoull(valS.c_str(), &end, 0);
    if (!end || end == valS.c_str() || *end) { printf("[!] 值解析失败: %s\n", valS.c_str()); return 2; }
    if (v > 0xFFFFFFFFull) { printf("[!] 值超出 u32 范围: %s\n", valS.c_str()); return 2; }
    uint32_t vv = (uint32_t)v;
    return CmdWriteVerify("w32", addr, &vv, 4);
}
static int CmdW64(const std::string& addrS, const std::string& valS) {
    uint64_t addr = strtoull(addrS.c_str(), nullptr, 16);
    char* end = nullptr;
    uint64_t v = strtoull(valS.c_str(), &end, 0);
    if (!end || end == valS.c_str() || *end) { printf("[!] 值解析失败: %s\n", valS.c_str()); return 2; }
    return CmdWriteVerify("w64", addr, &v, 8);
}
static int CmdWf(const std::string& addrS, const std::string& valS) {
    uint64_t addr = strtoull(addrS.c_str(), nullptr, 16);
    char* end = nullptr;
    float f = strtof(valS.c_str(), &end);
    if (!end || end == valS.c_str() || *end) { printf("[!] 浮点解析失败: %s\n", valS.c_str()); return 2; }
    return CmdWriteVerify("wf", addr, &f, 4);
}

// ---- 值扫描族 (scan/sscan): 多轮收缩定位字段 ----
static const uint32_t SCAN_MAX_CAND = 1u << 20;   // 1M 条候选 = 16MB 输出上限
static const uint32_t SCAN_MAGIC = 0x31534B42u;   // "BKS1"

struct ScanCand { uint64_t addr, raw; };

static int ScanTypeSize(const std::string& t) {
    if (t == "i32" || t == "u32" || t == "f32") return 4;
    if (t == "i16" || t == "u16") return 2;
    if (t == "i8" || t == "u8") return 1;
    return 0;
}

static void ScanWriteFile(const std::string& outS, int ts, const std::vector<ScanCand>& cands) {
    FILE* fp = fopen(outS.c_str(), "wb");
    if (!fp) { printf("[X] 无法写: %s\n", outS.c_str()); return; }
    uint32_t magic = SCAN_MAGIC, tcode = (uint32_t)ts;
    uint64_t cnt = cands.size();
    fwrite(&magic, 4, 1, fp);
    fwrite(&tcode, 4, 1, fp);
    fwrite(&cnt, 8, 1, fp);
    for (size_t i = 0; i < cands.size(); ++i) {
        fwrite(&cands[i].addr, 8, 1, fp);
        fwrite(&cands[i].raw, 8, 1, fp);
    }
    fclose(fp);
}

static int CmdScan(const std::string& outS, uint64_t start, uint64_t len,
                   const std::string& type, const std::string& v1s, const std::string& v2s) {
    const int ts = ScanTypeSize(type);
    if (!ts) { printf("[!] 类型: i32/u32/f32/i16/u16/i8/u8\n"); return 2; }
    if (len == 0) { printf("[!] 长度 0\n"); return 2; }
    char* e1 = nullptr; char* e2 = nullptr;
    double d1 = strtod(v1s.c_str(), &e1);
    double d2 = strtod(v2s.c_str(), &e2);
    if (!e1 || e1 == v1s.c_str() || !e2 || e2 == v2s.c_str()) { printf("[!] 值解析失败\n"); return 2; }
    if (d1 > d2) { double t = d1; d1 = d2; d2 = t; }
    int64_t i1 = (int64_t)d1, i2 = (int64_t)d2;
    uint64_t u1 = (d1 < 0) ? 0 : (uint64_t)d1;
    uint64_t u2 = (d2 < 0) ? 0 : (uint64_t)d2;
    float f1 = (float)d1, f2 = (float)d2;

    std::vector<ScanCand> cands;
    std::vector<uint8_t> buf((size_t)std::min<uint64_t>(len, 65536));
    uint64_t off = 0, failBytes = 0;
    while (off < len) {
        size_t want = (size_t)std::min<uint64_t>((uint64_t)buf.size(), len - off);
        if (!DrvReadAt(start + off, buf.data(), want)) {
            failBytes += want;
            off += want;
            continue;
        }
        for (size_t p = 0; p + (size_t)ts <= want; p += (size_t)ts) {
            uint64_t raw = 0;
            memcpy(&raw, buf.data() + p, (size_t)ts);
            bool hit = false;
            if (type == "f32") {
                float f; uint32_t r = (uint32_t)raw; memcpy(&f, &r, 4);
                hit = (f == f) && f >= f1 && f <= f2;
            } else if (type == "i32") { int32_t s = (int32_t)(uint32_t)raw; hit = (int64_t)s >= i1 && (int64_t)s <= i2; }
            else if (type == "u32") { uint32_t u = (uint32_t)raw; hit = (uint64_t)u >= u1 && (uint64_t)u <= u2; }
            else if (type == "i16") { int16_t s = (int16_t)(uint16_t)raw; hit = (int64_t)s >= i1 && (int64_t)s <= i2; }
            else if (type == "u16") { uint16_t u = (uint16_t)raw; hit = (uint64_t)u >= u1 && (uint64_t)u <= u2; }
            else if (type == "i8") { int8_t s = (int8_t)(uint8_t)raw; hit = (int64_t)s >= i1 && (int64_t)s <= i2; }
            else if (type == "u8") { uint8_t u = (uint8_t)raw; hit = (uint64_t)u >= u1 && (uint64_t)u <= u2; }
            if (hit) {
                if (cands.size() >= SCAN_MAX_CAND) {
                    printf("[!] 候选超过上限 %u, 请缩小区域或值域\n", (unsigned)SCAN_MAX_CAND);
                    return 1;
                }
                ScanCand c; c.addr = start + off + p; c.raw = raw;
                cands.push_back(c);
            }
        }
        off += want;
    }
    ScanWriteFile(outS, ts, cands);
    printf("[OK] scan %s [%#llx,+%#llx] => %zu 候选 -> %s (读失败 %#llx 字节)\n",
           type.c_str(), (unsigned long long)start, (unsigned long long)len,
           cands.size(), outS.c_str(), (unsigned long long)failBytes);
    return 0;
}

static int CmdSScan(const std::string& inS, const std::string& outS,
                    const std::string& type, const std::string& mode) {
    const int ts = ScanTypeSize(type);
    if (!ts) { printf("[!] 类型: i32/u32/f32/i16/u16/i8/u8\n"); return 2; }
    if (mode != "same" && mode != "changed" && mode != "dec" && mode != "inc") {
        printf("[!] 模式: same/changed/dec/inc\n"); return 2;
    }
    FILE* fp = fopen(inS.c_str(), "rb");
    if (!fp) { printf("[X] 无法读: %s\n", inS.c_str()); return 1; }
    uint32_t magic = 0, tcode = 0; uint64_t cnt = 0;
    if (fread(&magic, 4, 1, fp) != 1 || fread(&tcode, 4, 1, fp) != 1 || fread(&cnt, 8, 1, fp) != 1 ||
        magic != SCAN_MAGIC || (int)tcode != ts) {
        printf("[X] 文件格式不符或类型不一致\n"); fclose(fp); return 1;
    }
    if (cnt > SCAN_MAX_CAND) { printf("[X] 记录数超上限, 文件异常\n"); fclose(fp); return 1; }
    std::vector<ScanCand> keep;
    uint64_t readFails = 0;
    for (uint64_t i = 0; i < cnt; ++i) {
        ScanCand c;
        if (fread(&c.addr, 8, 1, fp) != 1 || fread(&c.raw, 8, 1, fp) != 1) break;
        uint64_t cur = 0;
        if (!DrvReadAt(c.addr, &cur, (size_t)ts)) { readFails++; continue; }
        bool ok = false;
        if (type == "f32") {
            float fo, fn; uint32_t ro = (uint32_t)c.raw, rn = (uint32_t)cur;
            memcpy(&fo, &ro, 4); memcpy(&fn, &rn, 4);
            if (mode == "same") ok = (ro == rn);
            else if (mode == "changed") ok = (ro != rn);
            else if (mode == "dec") ok = (fn < fo);
            else ok = (fn > fo);
        } else {
            bool isNeg = (type == "i32" || type == "i16" || type == "i8");
            bool same = (c.raw == cur);

            auto asI = [&](uint64_t v) -> int64_t {
                if (type == "i32") return (int64_t)(int32_t)(uint32_t)v;
                if (type == "i16") return (int64_t)(int16_t)(uint16_t)v;
                if (type == "i8") return (int64_t)(int8_t)(uint8_t)v;
                if (ts == 4) return (int64_t)(uint32_t)v;
                if (ts == 2) return (int64_t)(uint16_t)v;
                return (int64_t)(uint8_t)v;
            };
            if (mode == "same") ok = same;
            else if (mode == "changed") ok = !same;
            else {
                int64_t a = asI(c.raw), b = asI(cur);
                if (isNeg) ok = (mode == "dec") ? (b < a) : (b > a);
                else {
                    uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
                    ok = (mode == "dec") ? (ub < ua) : (ub > ua);
                }
            }
        }
        if (ok) keep.push_back({c.addr, cur});
    }
    fclose(fp);
    ScanWriteFile(outS, ts, keep);
    printf("[OK] sscan %s %s: %llu -> %zu (读失败 %llu) -> %s\n",
           type.c_str(), mode.c_str(), (unsigned long long)cnt, keep.size(),
           (unsigned long long)readFails, outS.c_str());
    return 0;
}

// ---------------- skilldump: 单位列表 + 技能槽 一键采样 ----------------
// 用法: skilldump <out.txt>
// 链（本样本验证）:
//   il2cpp.bss + 0x3A0FC0 -> SingletonClass (+0xB8) -> StaticFields (+0x0) -> ActorManager
//   ActorManager + {0x18 Hero,0x20 Organ,0x28 Tower,0x30 Soldier,0x38 Dragon,0x48 BuffMonster} -> List
//   List+0x8 -> Array ; List+0x10 -> count ; Array+0x10 -> capacity
//   record = Array + 0x18 + i*0x10 ; record+0x0 handle ; record+0x8 -> ActorLinker
//   ActorLinker+0x8 == handle ; +0x4A8/+0x4AC ids ; +0x4C4 3xfloat (渲染坐标)
//   英雄技能槽: ActorLinker+0x28 -> SkillLinkerComponent ; +0x28 -> slot array ; +0x10 len
//   slot = array + 0x18 + i*8 ; +0x30 type +0x38 lvl +0x5C cd +0x60 cdmax +0x64 maxcd +0x74 ready
// 读失败一律落 "ERR" 占位, 不写成 0 (与项目既有约定一致)。
static const uint32_t kMaxRec   = 512;
static const uint32_t kMaxSlots = 32;
static const uint64_t kIl2cppBssSlot    = 0x3A0FC0;
static const uint64_t kIl2cppBssMinSize = 0x3A0FC8;

static bool RdQ(const uint64_t a, uint64_t* v) { return DrvReadAt(a, v, 8); }
static bool RdU32(const uint64_t a, uint32_t* v) { return DrvReadAt(a, v, 4); }
static bool RdI32(const uint64_t a, int32_t* v) { return DrvReadAt(a, v, 4); }
static bool RdF32(const uint64_t a, float* v) { return DrvReadAt(a, v, 4); }
static bool RdU8(const uint64_t a, uint8_t* v) { return DrvReadAt(a, v, 1); }

static std::string NowStr() {
    char ts[64] = {0};
    time_t t = time(nullptr);
    struct tm tmv;
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    return std::string(ts);
}

static int CmdSkillDump(const std::string& outPath) {
    if (g_pid <= 0) return 2;
    g_drv->initialize(g_pid);
    std::vector<Region> maps = ReadMaps(g_pid);
    if (maps.empty()) { printf("[!] maps 读取失败\n"); return 1; }
    const uint64_t MASK48 = 0xFFFFFFFFFFFFull;
    // 真实镜像基址: libil2cpp 的 offset=0 段中取最小者（排除保护层整文件只读映射）
    uint64_t imgBase = 0, imgMin = ~0ull;
    for (const auto& r : maps) {
        if (r.path.find("libil2cpp") == std::string::npos) continue;
        if (r.offset != 0) continue;
        uint64_t sz = r.end - r.start;
        if (sz < imgMin) { imgMin = sz; imgBase = r.start; }
    }
    // .bss: 紧跟 libil2cpp 段之后的 [anon:.bss]
    uint64_t bss = 0, bssSz = 0;
    for (size_t i = 1; i < maps.size(); ++i) {
        if (maps[i].path.find(".bss") == std::string::npos) continue;
        if (maps[i].start != maps[i - 1].end) continue;
        if (maps[i - 1].path.find("libil2cpp") == std::string::npos) continue;
        bss = maps[i].start;
        bssSz = maps[i].end - maps[i].start;
        break;
    }
    printf("[*] il2cpp image=0x%" PRIx64 " bss=0x%" PRIx64 " size=0x%" PRIx64 "\n", imgBase, bss, bssSz);
    if (!imgBase || !bss) { printf("[X] 未定位 libil2cpp 镜像/BSS\n"); return 1; }
    if (bssSz < kIl2cppBssMinSize) {
        printf("[X] bss 尺寸 0x%" PRIx64 " < 0x%" PRIx64 "（链路超出 BSS，快照不完整）\n", bssSz, kIl2cppBssMinSize);
        return 1;
    }
    uint64_t v = 0;
    if (!RdQ(bss + kIl2cppBssSlot, &v)) { printf("[X] 读 bss+0x%llX 失败\n", (unsigned long long)kIl2cppBssSlot); return 1; }
    uint64_t cls = v & MASK48;
    if (!cls || !RdQ(cls + 0xB8, &v)) { printf("[X] SingletonClass 链断\n"); return 1; }
    uint64_t sf = v & MASK48;
    if (!sf || !RdQ(sf, &v)) { printf("[X] StaticFields 链断\n"); return 1; }
    uint64_t am = v & MASK48;
    if (!am) { printf("[X] ActorManager 为空（不在对局？）\n"); return 1; }
    printf("[*] ActorManager=0x%" PRIx64 "\n", am);
    FILE* fo = fopen(outPath.c_str(), "w");
    if (!fo) { printf("[X] 无法写 %s\n", outPath.c_str()); return 1; }
    // 相机缓存帧号（一致性参考）: bss+0x38A140 -> class -> +0xB8 -> sf -> +0 -> cam -> +0x58 -> cache
    uint32_t camFrame = 0;
    uint8_t camValid = 0;
    bool camOk = false, camValidOk = false;
    {
        uint64_t c = 0;
        if (RdQ(bss + 0x38A140, &c)) {
            c &= MASK48;
            uint64_t sf2 = 0;
            if (c && RdQ(c + 0xB8, &sf2)) {
                sf2 &= MASK48;
                uint64_t cam = 0;
                if (sf2 && RdQ(sf2, &cam)) {
                    cam &= MASK48;
                    uint64_t cache = 0;
                    if (cam && RdQ(cam + 0x58, &cache)) {
                        cache &= MASK48;
                        if (cache && RdU32(cache + 0x48, &camFrame)) camOk = true;
                        if (cache) camValidOk = RdU8(cache + 0x50, &camValid);
                    }
                }
            }
        }
    }
    char camFrameStr[32], camValidStr[16];
    if (camOk && camValidOk) snprintf(camFrameStr, sizeof(camFrameStr), "%u", camFrame);
    else snprintf(camFrameStr, sizeof(camFrameStr), "ERR");
    if (camOk && camValidOk) snprintf(camValidStr, sizeof(camValidStr), "%u", (unsigned)camValid);
    else snprintf(camValidStr, sizeof(camValidStr), "ERR");
    std::string tStart = NowStr();
    fprintf(fo, "# skilldump pid=%d img=0x%" PRIx64 " bss=0x%" PRIx64 " bss_size=0x%" PRIx64
                " start=%s cam_frame=%s cam_valid=%s\n",
            (int)g_pid, imgBase, bss, bssSz, tStart.c_str(), camFrameStr, camValidStr);
    fprintf(fo, "# offsets slot=0x%" PRIx64 " list=(0x18,0x20,0x28,0x30,0x38,0x48) rec_stride=0x10\n",
            kIl2cppBssSlot);
    fprintf(fo, "# offsets record: handle+0x0 linker+0x8 ; linker: ids+0x4A8/+0x4AC pos+0x4C4\n");
    fprintf(fo, "# offsets slots: comp=linker+0x28 array=comp+0x28 len+0x10 entry=array+0x18+i*8"
                " fields=(0x30,0x38,0x5C,0x60,0x64,0x74)\n");
    struct LDef { uint64_t off; const char* name; bool dumpSlots; };
    const LDef lists[] = {
        {0x18, "Hero", true}, {0x20, "Organ", false}, {0x28, "Tower", false},
        {0x30, "Soldier", false}, {0x38, "Dragon", false}, {0x48, "BuffMonster", false},
    };
    int totalRec = 0, chainFails = 0, listFails = 0;
    for (const auto& L : lists) {
        uint64_t lst = 0;
        if (!RdQ(am + L.off, &lst)) {
            fprintf(fo, "LIST %s FAIL(am+0x%llX)\n", L.name, (unsigned long long)L.off);
            listFails++; continue;
        }
        lst &= MASK48;
        if (!lst) { fprintf(fo, "LIST %s EMPTY(null)\n", L.name); continue; }
        uint64_t arr = 0;
        if (!RdQ(lst + 0x8, &arr)) {
            fprintf(fo, "LIST %s list=0x%" PRIx64 " FAIL(array)\n", L.name, lst);
            listFails++; continue;
        }
        arr &= MASK48;
        uint32_t cnt = 0;
        if (!RdU32(lst + 0x10, &cnt)) {
            fprintf(fo, "LIST %s list=0x%" PRIx64 " FAIL(count)\n", L.name, lst);
            listFails++; continue;
        }
        if (!arr) {
            fprintf(fo, "LIST %s list=0x%" PRIx64 " array=NULL count=%u (skip)\n", L.name, lst, cnt);
            listFails++; continue;
        }
        uint64_t cap = 0;
        if (!RdQ(arr + 0x10, &cap)) {
            fprintf(fo, "LIST %s list=0x%" PRIx64 " array=0x%" PRIx64 " cap=ERR (skip)\n", L.name, lst, arr);
            listFails++; continue;
        }
        if (cnt > cap || cap > 65536) {
            fprintf(fo, "LIST %s INVALID count=%u cap=%" PRIu64 " (skip)\n", L.name, cnt, cap);
            listFails++; continue;
        }
        fprintf(fo, "LIST %s list=0x%" PRIx64 " array=0x%" PRIx64 " count=%u capacity=%" PRIu64 "\n",
                L.name, lst, arr, cnt, cap);
        uint32_t nrec = cnt;
        if (nrec > kMaxRec) {
            fprintf(fo, "  [!] %s count=%u > %u，已截断\n", L.name, cnt, kMaxRec);
            nrec = kMaxRec;
        }
        for (uint32_t i = 0; i < nrec; ++i) {
            uint64_t rec = arr + 0x18 + (uint64_t)i * 0x10;
            uint32_t handle = 0;
            if (!RdU32(rec, &handle)) {
                fprintf(fo, "  %s[%u] handle=ERR\n", L.name, i);
                chainFails++; continue;
            }
            uint64_t lk = 0;
            if (!RdQ(rec + 0x8, &lk)) {
                fprintf(fo, "  %s[%u] handle=%u linker=ERR\n", L.name, i, handle);
                chainFails++; continue;
            }
            lk &= MASK48;
            if (!lk) {
                fprintf(fo, "  %s[%u] handle=%u linker=NULL\n", L.name, i, handle);
                continue;
            }
            uint32_t lkHandle = 0;
            int ok = (RdU32(lk + 0x8, &lkHandle) && lkHandle == handle) ? 1 : 0;
            uint32_t id1 = 0, id2 = 0;
            bool okId = RdU32(lk + 0x4A8, &id1) && RdU32(lk + 0x4AC, &id2);
            float px = 0, py = 0, pz = 0;
            bool okPos = RdF32(lk + 0x4C4, &px) && RdF32(lk + 0x4C8, &py) && RdF32(lk + 0x4CC, &pz);
            bool posFinite = okPos && std::isfinite(px) && std::isfinite(py) && std::isfinite(pz)
                             && std::fabs(px) < 10000 && std::fabs(py) < 10000 && std::fabs(pz) < 10000;
            char idbuf[64], posbuf[96];
            if (ok && okId) snprintf(idbuf, sizeof(idbuf), "(%u,%u)", id1, id2);
            else snprintf(idbuf, sizeof(idbuf), "ERR");
            if (ok && posFinite) snprintf(posbuf, sizeof(posbuf), "(%.3f,%.3f,%.3f)", px, py, pz);
            else snprintf(posbuf, sizeof(posbuf), "ERR");
            fprintf(fo, "  %s[%u] handle=%u linker=0x%" PRIx64 " ok=%d id=%s pos=%s\n",
                    L.name, i, handle, lk, ok, idbuf, posbuf);
            totalRec++;
            if (L.dumpSlots && ok) {
                uint64_t comp = 0;
                if (!RdQ(lk + 0x28, &comp)) {
                    fprintf(fo, "    SLOTS FAIL(component)\n");
                } else {
                    comp &= MASK48;
                    uint64_t slots = 0;
                    if (!comp || !RdQ(comp + 0x28, &slots)) {
                        fprintf(fo, "    SLOTS FAIL(array)\n");
                    } else {
                        slots &= MASK48;
                        uint32_t len = 0;
                        if (!slots || !RdU32(slots + 0x10, &len)) {
                            fprintf(fo, "    SLOTS len=ERR\n");
                        } else {
                            uint32_t n2 = len;
                            if (n2 > kMaxSlots) {
                                fprintf(fo, "    [!] SLOTS len=%u > %u，已截断\n", len, kMaxSlots);
                                n2 = kMaxSlots;
                            }
                            fprintf(fo, "    SLOTS array=0x%" PRIx64 " len=%u\n", slots, len);
                            for (uint32_t k = 0; k < n2; ++k) {
                                uint64_t e = 0;
                                if (!RdQ(slots + 0x18 + (uint64_t)k * 8, &e)) {
                                    fprintf(fo, "      slot[%u] entry=ERR\n", k);
                                    continue;
                                }
                                e &= MASK48;
                                if (!e) { fprintf(fo, "      slot[%u] entry=NULL\n", k); continue; }
                                uint32_t st = 0, lv = 0;
                                int32_t cd = 0, cdm = 0, mx = 0;
                                uint8_t ready = 0;
                                bool okS = RdU32(e + 0x30, &st) && RdU32(e + 0x38, &lv)
                                        && RdI32(e + 0x5C, &cd) && RdI32(e + 0x60, &cdm)
                                        && RdI32(e + 0x64, &mx) && RdU8(e + 0x74, &ready);
                                if (okS)
                                    fprintf(fo, "      slot[%u] type=%u lvl=%u cd=%d cdmax=%d maxcd=%d ready=%u\n",
                                            k, st, lv, cd, cdm, mx, (unsigned)ready);
                                else
                                    fprintf(fo, "      slot[%u] ERR\n", k);
                            }
                        }
                    }
                }
            }
        }
        fflush(fo);
    }
    fprintf(fo, "# summary records=%d chain_fails=%d list_fails=%d end=%s\n",
            totalRec, chainFails, listFails, NowStr().c_str());
    fflush(fo);
    fclose(fo);
    if (totalRec == 0 || listFails >= 6) {
        printf("[!] skilldump 无有效记录 (list_fails=%d, chain_fails=%d) — 可能不在对局或链路未就绪\n",
               listFails, chainFails);
        return 3;
    }
    printf("[OK] skilldump -> %s (%d records, chain_fails=%d)\n", outPath.c_str(), totalRec, chainFails);
    return 0;
}

// ---------------- framesnap: 投影数据快照 ----------------
// 用法: framesnap <out.json>
// 输出: 采样时间 / cam_frame+valid / VP 矩阵(16 float) / 英雄与塔的渲染坐标
static int CmdFrameSnap(const std::string& outPath) {
    if (g_pid <= 0) return 2;
    g_drv->initialize(g_pid);
    std::vector<Region> maps = ReadMaps(g_pid);
    if (maps.empty()) { printf("[!] maps 读取失败\n"); return 1; }
    const uint64_t M48 = 0xFFFFFFFFFFFFull;
    uint64_t bss = 0, bssSz = 0;
    for (size_t i = 1; i < maps.size(); ++i) {
        if (maps[i].path.find(".bss") == std::string::npos) continue;
        if (maps[i].start != maps[i - 1].end) continue;
        if (maps[i - 1].path.find("libil2cpp") == std::string::npos) continue;
        bss = maps[i].start;
        bssSz = maps[i].end - maps[i].start;
        break;
    }
    if (!bss || bssSz < kIl2cppBssMinSize) { printf("[X] libil2cpp BSS 不可用\n"); return 1; }
    auto follow = [&](uint64_t base, const std::vector<uint64_t>& offs) -> uint64_t {
        uint64_t v = 0;
        if (!RdQ(base, &v)) return 0;
        uint64_t cur = v & M48;
        for (uint64_t off : offs) {
            if (!cur || !RdQ(cur + off, &v)) return 0;
            cur = v & M48;
        }
        return cur;
    };
    const uint64_t camSystem = follow(bss + 0x38A140, {0xB8, 0x0});
    // CameraCache = *(CameraSystem + 0x58)  (直读，勿多跳)
    uint64_t cache = 0;
    if (camSystem) {
        uint64_t cv = 0;
        if (RdQ(camSystem + 0x58, &cv)) cache = cv & M48;
    }
    uint32_t camFrame = 0;
    uint8_t camValid = 0;
    int frameOk = 0, validOk = 0, vpOk = 0;
    float vp[16];
    memset(vp, 0, sizeof(vp));
    if (cache) {
        frameOk = RdU32(cache + 0x48, &camFrame) ? 1 : 0;
        validOk = RdU8(cache + 0x50, &camValid) ? 1 : 0;
        vpOk = DrvReadAt(cache + 0x8, vp, 64) ? 1 : 0;
    }
    const uint64_t am = follow(bss + 0x3A0FC0, {0xB8, 0x0});
    FILE* fo = fopen(outPath.c_str(), "w");
    if (!fo) { printf("[X] 无法写 %s\n", outPath.c_str()); return 1; }
    fprintf(fo, "{\"time\":\"%s\",\"pid\":%d,", NowStr().c_str(), (int)g_pid);
    if (frameOk && validOk)
        fprintf(fo, "\"cam_frame\":%u,\"cam_valid\":%u,", camFrame, (unsigned)camValid);
    else
        fprintf(fo, "\"cam_frame\":null,\"cam_valid\":null,");
    fprintf(fo, "\"vp\":[");
    bool vpFinite = vpOk;
    if (vpOk) {
        for (int i = 0; i < 16; ++i) {
            if (!std::isfinite(vp[i])) { vpFinite = false; break; }
        }
    }
    if (vpFinite) {
        for (int i = 0; i < 16; ++i) fprintf(fo, "%s%.9g", i ? "," : "", vp[i]);
    } else {
        fprintf(fo, "null");
    }
    fprintf(fo, "],\"lists\":{");
    struct LDef { uint64_t off; const char* name; };
    const LDef lists[] = {{0x18, "Hero"}, {0x28, "Tower"}, {0x30, "Soldier"}, {0x48, "BuffMonster"}};
    bool firstList = true;
    for (const auto& L : lists) {
        uint64_t lst = 0, arr = 0;
        uint32_t cnt = 0;
        uint64_t cap = 0;
        bool ok = false;
        if (am && RdQ(am + L.off, &lst)) {
            lst &= M48;
            if (lst && RdQ(lst + 0x8, &arr)) {
                arr &= M48;
                ok = arr && RdU32(lst + 0x10, &cnt) && RdQ(arr + 0x10, &cap)
                     && cap >= cnt && cap <= 65536 && cnt <= kMaxRec;
            }
        }
        if (!firstList) fprintf(fo, ",");
        firstList = false;
        fprintf(fo, "\"%s\":[", L.name);
        if (ok) {
            bool firstRec = true;
            for (uint32_t i = 0; i < cnt && i < kMaxRec; ++i) {
                uint64_t rec = arr + 0x18 + (uint64_t)i * 0x10;
                uint32_t handle = 0;
                uint64_t lk = 0;
                if (!RdU32(rec, &handle) || !RdQ(rec + 0x8, &lk)) continue;
                lk &= M48;
                uint32_t lh = 0;
                if (!lk || !RdU32(lk + 0x8, &lh) || lh != handle) continue;
                uint32_t id1 = 0, id2 = 0;
                float px = 0, py = 0, pz = 0;
                bool okId = RdU32(lk + 0x4A8, &id1) && RdU32(lk + 0x4AC, &id2);
                bool okPos = RdF32(lk + 0x4C4, &px) && RdF32(lk + 0x4C8, &py) && RdF32(lk + 0x4CC, &pz);
                bool posFinite = okPos && std::isfinite(px) && std::isfinite(py) && std::isfinite(pz)
                                 && std::fabs(px) < 10000 && std::fabs(py) < 10000 && std::fabs(pz) < 10000;
                fprintf(fo, "%s{\"idx\":%u,\"id\":[%u,%u],\"pos\":", firstRec ? "" : ",", i,
                        okId ? id1 : 0, okId ? id2 : 0);
                if (posFinite) fprintf(fo, "[%.3f,%.3f,%.3f]", px, py, pz);
                else fprintf(fo, "null");
                fprintf(fo, "}");
                firstRec = false;
            }
        }
        fprintf(fo, "]");
    }
    fprintf(fo, "}}\n");
    fclose(fo);
    printf("[OK] framesnap -> %s (vp=%d frame=%d valid=%d)\n", outPath.c_str(), vpFinite ? 1 : 0, frameOk, validOk);
    return 0;
}

// ================= 用法 =================
static void Usage() {    printf("best-king · 独立内存 dump 工具（Paradise 驱动专用, 硬闸门）\n");
    printf("用法: King.sh [--target pkg|pid] [--chunk N] <命令> ...\n");
    printf("  无参数                              自动模式: 闸门 + dumpgame 一键（MT直接运行用这个）\n");
    printf("  selftest                            驱动连接自检（闸门流程单跑）\n");
    printf("  mods   [过滤]                        模块列表\n");
    printf("  maps   [过滤]                        maps 打印\n");
    printf("  range  <hex址> <hex长> <out.bin>     任意区间 dump\n");
    printf("  dumpgame <out.bin> <out.idx> [read-status.jsonl]  一键圈选匿名堆区+bss（可选逐块读取状态）\n");
    printf("  hwbp_scan <hexAddr> <dur_ms> [m]     16点观察点扫描(0x130..0x2C8) m:2=w 3=rw 4=x\n");
    printf("  bpauto [dur_ms] [m]                  自动取根->挂点->采样 (战中跑这条) m:2=w 3=rw 4=x\n");
    printf("  bpfollow <hexW> <dur_ms> [m]         读W+0x188指针->挂指针目标 P+0xA8/0xB0\n");
    printf("  bpdig <hexW> <dur_ms> [m]            W+0xD8 内容块 -> P+0x18..0x98 16观察点\n");
    printf("  bpsweep <hexObj> <hexSpan> <每区ms> [m]  16点×分块移窗扫对象全域\n");
    printf("  rd     <hex址> <hex长>               驱动读并十六进制打印(≤4096)\n");
    printf("  poke   <hex址> <hex字节串>           驱动写原始字节+读回验证\n");
    printf("  w32    <hex址> <值>                  驱动写 u32 (值支持 0x/十进制)+读回\n");
    printf("  w64    <hex址> <值>                  驱动写 u64 +读回\n");
    printf("  wf     <hex址> <浮点>                驱动写 float +读回\n");
    printf("  scan   <out.bin> <hex址> <hex长> <类型> <下限> <上限>  值扫描 (i32/u32/f32/i16/u16/i8/u8)\n");
    printf("  sscan  <in.bin> <out.bin> <类型> <模式>              复扫收缩 (same/changed/dec/inc)\n");
    printf("  skilldump <out.txt>                  单位列表(英雄/塔/兵/Buff)+技能槽 一键采样\n");
    printf("  framesnap <out.json>                 投影快照: VP矩阵+英雄/塔渲染坐标+时间/frame\n");
    printf("  bplive [dur_ms] [m]                  ★全自动: 开着打一局, 自动等到换局→新wrapper→live pos/hp+写入者\n");
    printf("默认目标: %s\n", kDefaultTarget);
    printf("示例: King.sh dumpgame /data/adb/JAY/game.bin /data/adb/JAY/game.idx\n");
}

int main(int argc, char** argv) {
    std::vector<std::string> args;
    std::string target = kDefaultTarget;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--target" && i + 1 < argc) { target = argv[++i]; continue; }
        if (a == "--chunk" && i + 1 < argc) {
            g_chunk = (size_t)strtoul(argv[++i], nullptr, 0);
            if (g_chunk < 256) g_chunk = 256;
            g_chunk_auto = false;
            continue;
        }
        args.push_back(a);
    }
    if (!args.empty() && (args[0] == "help" || args[0] == "-h" || args[0] == "--help")) {
        Usage();
        return 0;
    }

    // MT管理器"运行"不带参数 → 自动模式: 自动获取王者进程 + 自动 dump（一键）
    if (args.empty()) {
        printf("[*] 自动模式: 自动获取「王者荣耀」进程 → 自动 dump（无需任何参数）\n");
        const int g = Gatekeeper(target);
        if (g != 0) {
            printf("\n[X] 已阻断: par 驱动未连接或目标进程不可用, 主程序不启动（code=%d）\n", g);
            return g;
        }
        printf("[*] 自动 dump → /data/adb/JAY/game.bin + game.idx（对局内执行效果最佳）\n\n");
        const int rc = CmdDumpGame("/data/adb/JAY/game.bin", "/data/adb/JAY/game.idx", "/data/adb/JAY/game.read.jsonl");
        printf("\n[完成] dumpgame rc=%d\n", rc);
        if (rc == 0) printf("[完成] 把 /data/adb/JAY/game.bin 和 game.idx 发我即可\n");
        return rc;
    }

    // ===== 硬闸门: 不通过则后面一行都不执行 =====
    const int gate = Gatekeeper(target);
    if (gate != 0) {
        printf("\n[X] 已阻断: par 驱动未连接或目标进程不可用, 主程序不启动（code=%d）\n", gate);
        return gate;
    }

    const std::string& cmd = args[0];
    if (cmd == "selftest") {
        printf("[OK] selftest: par 驱动链路正常\n");
        return 0;
    }
    if (cmd == "mods") return CmdMods(args.size() > 1 ? args[1] : "");
    if (cmd == "maps") return CmdMaps(args.size() > 1 ? args[1] : "");
    if (cmd == "range") {
        if (args.size() < 4) { Usage(); return 2; }
        return CmdRange(strtoull(args[1].c_str(), nullptr, 16),
                        strtoull(args[2].c_str(), nullptr, 16), args[3]);
    }
    if (cmd == "dumpgame") {
        if (args.size() < 3) { Usage(); return 2; }
        return CmdDumpGame(args[1], args[2], args.size() > 3 ? args[3] : "");
    }
    if (cmd == "framesnap") {
        if (args.size() < 2) { Usage(); return 2; }
        return CmdFrameSnap(args[1]);
    }
    if (cmd == "hwbp_scan") {
        if (args.size() < 3) { Usage(); return 2; }
        uint32_t mode = PARADISE_HWBP_READ_WRITE;
        if (args.size() > 3) mode = (uint32_t)strtoul(args[3].c_str(), nullptr, 0);
        return CmdHwbpScan(strtoull(args[1].c_str(), nullptr, 16),
                           (uint32_t)strtoul(args[2].c_str(), nullptr, 10), mode);
    }
    if (cmd == "bpauto") {
        uint32_t mode = PARADISE_HWBP_READ_WRITE;
        if (args.size() > 2) mode = (uint32_t)strtoul(args[2].c_str(), nullptr, 0);
        uint32_t dur = args.size() > 1 ? (uint32_t)strtoul(args[1].c_str(), nullptr, 10) : 20000;
        return CmdBpAuto(dur, mode);
    }
    if (cmd == "bpfollow") {
        if (args.size() < 2) { Usage(); return 2; }
        uint32_t mode = PARADISE_HWBP_READ_WRITE;
        if (args.size() > 3) mode = (uint32_t)strtoul(args[3].c_str(), nullptr, 0);
        uint32_t dur = args.size() > 2 ? (uint32_t)strtoul(args[2].c_str(), nullptr, 10) : 20000;
        return CmdBpFollow(strtoull(args[1].c_str(), nullptr, 16), dur, mode);
    }
    if (cmd == "bpwar") {
        uint32_t mode = PARADISE_HWBP_READ_WRITE;
        if (args.size() > 2) mode = (uint32_t)strtoul(args[2].c_str(), nullptr, 0);
        uint32_t dur = args.size() > 1 ? (uint32_t)strtoul(args[1].c_str(), nullptr, 10) : 300000;
        return CmdBpWar(dur, mode);
    }
    if (cmd == "bplive") {
        uint32_t mode = PARADISE_HWBP_READ_WRITE;
        if (args.size() > 2) mode = (uint32_t)strtoul(args[2].c_str(), nullptr, 0);
        uint32_t dur = args.size() > 1 ? (uint32_t)strtoul(args[1].c_str(), nullptr, 10) : 600000;
        return CmdBpLive(dur, mode);
    }
    if (cmd == "bpdig") {
        if (args.size() < 2) { Usage(); return 2; }
        uint32_t mode = PARADISE_HWBP_READ_WRITE;
        if (args.size() > 3) mode = (uint32_t)strtoul(args[3].c_str(), nullptr, 0);
        uint32_t dur = args.size() > 2 ? (uint32_t)strtoul(args[2].c_str(), nullptr, 10) : 10000;
        return CmdBpDig(strtoull(args[1].c_str(), nullptr, 16), dur, mode);
    }
    if (cmd == "bpsweep") {
        if (args.size() < 4) { Usage(); return 2; }
        uint32_t mode = PARADISE_HWBP_READ_WRITE;
        if (args.size() > 4) mode = (uint32_t)strtoul(args[4].c_str(), nullptr, 0);
        uint32_t per_ms = ((args.size() > 3) ? (uint32_t)strtoul(args[3].c_str(), nullptr, 10) : 300);
        return CmdBpSweep(strtoull(args[1].c_str(), nullptr, 16), strtoull(args[2].c_str(), nullptr, 16), per_ms, mode);
    }
    if (cmd == "rd") {
        if (args.size() < 3) { Usage(); return 2; }
        return CmdRd(strtoull(args[1].c_str(), nullptr, 16), (size_t)strtoull(args[2].c_str(), nullptr, 16));
    }
    if (cmd == "poke") {
        if (args.size() < 3) { Usage(); return 2; }
        return CmdPoke(args[1], args[2]);
    }
    if (cmd == "w32") {
        if (args.size() < 3) { Usage(); return 2; }
        return CmdW32(args[1], args[2]);
    }
    if (cmd == "w64") {
        if (args.size() < 3) { Usage(); return 2; }
        return CmdW64(args[1], args[2]);
    }
    if (cmd == "wf") {
        if (args.size() < 3) { Usage(); return 2; }
        return CmdWf(args[1], args[2]);
    }
    if (cmd == "scan") {
        if (args.size() < 7) { Usage(); return 2; }
        return CmdScan(args[1], strtoull(args[2].c_str(), nullptr, 16),
                       strtoull(args[3].c_str(), nullptr, 16), args[4], args[5], args[6]);
    }
    if (cmd == "sscan") {
        if (args.size() < 5) { Usage(); return 2; }
        return CmdSScan(args[1], args[2], args[3], args[4]);
    }
    if (cmd == "skilldump") {
        if (args.size() < 2) { Usage(); return 2; }
        return CmdSkillDump(args[1]);
    }
    if (cmd == "hwbp_selftest") return CmdHwbpSelfTest();
    Usage();
    return 2;
}
