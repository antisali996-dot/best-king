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
static int CmdDumpGame(const std::string& out_bin, const std::string& out_idx) {
    std::vector<Region> maps = ReadMaps(g_pid);
    if (maps.empty()) {
        printf("[!] 读取 maps 失败\n");
        return 1;
    }
    struct Sel { uint64_t start, size; std::string name; };
    std::vector<Sel> sels;
    const uint64_t LIMIT_TOTAL = 1500ull << 20;
    const uint64_t LIMIT_ONE = 256ull << 20;
    uint64_t total = 0;
    int skipped = 0;
    for (size_t i = 0; i < maps.size(); ++i) {
        const Region& r = maps[i];
        if (!IsReadable(r)) continue;
        const bool isBss = (r.path.find(".bss") != std::string::npos);
        const bool inWin = (r.start >= 0x7000000000ull && r.start < 0x7C00000000ull);
        if (!isBss && !inWin) continue;
        if (!r.path.empty() && r.path[0] != '[') continue;
        uint64_t sz = r.end - r.start;
        if (sz < (1ull << 20)) continue;
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
    if (!fb || !fi) {
        printf("[!] 无法写文件: %s / %s\n", out_bin.c_str(), out_idx.c_str());
        if (fb) fclose(fb);
        if (fi) fclose(fi);
        return 1;
    }
    for (size_t i = 0; i < maps.size(); ++i) {
        fprintf(fi, "#MAP %012" PRIx64 "-%012" PRIx64 " %s %08" PRIx64 " %s\n",
                maps[i].start, maps[i].end, maps[i].perms, maps[i].offset,
                maps[i].path.c_str());
    }
    fprintf(fi, "# off start size name\n");

    std::vector<uint8_t> buf(g_chunk);
    uint64_t foff = 0, fails = 0;
    for (size_t i = 0; i < sels.size(); ++i) {
        fprintf(fi, "%" PRIu64 " 0x%012" PRIx64 " %" PRIu64 " %s\n",
                foff, sels[i].start, sels[i].size, sels[i].name.c_str());
        uint64_t done = 0;
        while (done < sels[i].size) {
            size_t n = (size_t)std::min<uint64_t>(g_chunk, sels[i].size - done);
            if (!DrvReadAt(sels[i].start + done, buf.data(), n)) {
                fails += n;
                memset(buf.data(), 0, n);
            }
            if (fwrite(buf.data(), 1, n, fb) != n) {
                printf("[!] 写盘失败（空间不足?）: %s\n", out_bin.c_str());
                fclose(fb);
                fclose(fi);
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
    fclose(fb);
    fclose(fi);
    printf("[OK] dumpgame -> %s (%" PRIu64 " MB), 索引 -> %s（读失败 %" PRIu64 " 字节）\n",
           out_bin.c_str(), foff >> 20, out_idx.c_str(), fails);
    if (fails > 0) printf("[!] 有读失败（已填零）\n");
    return 0;
}

// ================= 用法 =================
static void Usage() {
    printf("best-king · 独立内存 dump 工具（Paradise 驱动专用, 硬闸门）\n");
    printf("用法: King.sh [--target pkg|pid] [--chunk N] <命令> ...\n");
    printf("  无参数                              自动模式: 闸门 + dumpgame 一键（MT直接运行用这个）\n");
    printf("  selftest                            驱动连接自检（闸门流程单跑）\n");
    printf("  mods   [过滤]                        模块列表\n");
    printf("  maps   [过滤]                        maps 打印\n");
    printf("  range  <hex址> <hex长> <out.bin>     任意区间 dump\n");
    printf("  dumpgame <out.bin> <out.idx>         一键圈选匿名堆区+bss（索引含完整 maps）\n");
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
        const int rc = CmdDumpGame("/data/adb/JAY/game.bin", "/data/adb/JAY/game.idx");
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
        return CmdDumpGame(args[1], args[2]);
    }
    Usage();
    return 2;
}
