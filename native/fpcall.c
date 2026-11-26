// fpcall: EXPERIMENT. Call a libgf_hal.so function inside the Goodix fingerprint HAL process via ptrace.
//   fpcall <pid> <func_offset_hex> <expect_8bytes_hex> [arg1]
// The function is called as f(dev, arg1). dev is the gf_fingerprint_device* that fingerprint.goodix.so
// keeps behind its GOT slot +0x6868 (the same value it passes to gf_hal_authenticate/cancel/...).
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#define GOODIX_DEV_GOT 0x6868

static uint64_t map_base(int pid, const char *lib) {
    char p[64], line[512];
    snprintf(p, sizeof(p), "/proc/%d/maps", pid);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    uint64_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        uint64_t s, off;
        char path[256] = "";
        if (sscanf(line, "%" SCNx64 "-%*" SCNx64 " %*s %" SCNx64 " %*s %*s %255s", &s, &off, path) < 2) continue;
        const char *b = strrchr(path, '/');
        if (b && !strcmp(b + 1, lib) && off == 0) { base = s; break; }
    }
    fclose(f);
    return base;
}

static int memfd = -1;
static int rd(uint64_t a, void *buf, size_t n) { return pread64(memfd, buf, n, (off64_t)a) == (ssize_t)n ? 0 : -1; }

static int getregs(int tid, struct user_pt_regs *r) {
    struct iovec io = {r, sizeof(*r)};
    return (int)ptrace(PTRACE_GETREGSET, tid, (void *)NT_PRSTATUS, &io);
}
static int setregs(int tid, struct user_pt_regs *r) {
    struct iovec io = {r, sizeof(*r)};
    return (int)ptrace(PTRACE_SETREGSET, tid, (void *)NT_PRSTATUS, &io);
}
static int getfp(int tid, struct user_fpsimd_state *r) {
    struct iovec io = {r, sizeof(*r)};
    return (int)ptrace(PTRACE_GETREGSET, tid, (void *)NT_PRFPREG, &io);
}
static int setfp(int tid, struct user_fpsimd_state *r) {
    struct iovec io = {r, sizeof(*r)};
    return (int)ptrace(PTRACE_SETREGSET, tid, (void *)NT_PRFPREG, &io);
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s pid func_off expect_hex [arg1]\n", argv[0]); return 2; }
    int pid = atoi(argv[1]);
    uint64_t foff = strtoull(argv[2], 0, 16);
    uint64_t arg1 = argc > 4 ? strtoull(argv[4], 0, 0) : 0;

    uint64_t hal = map_base(pid, "libgf_hal.so"), glue = map_base(pid, "fingerprint.goodix.so");
    if (!hal || !glue) { fprintf(stderr, "libs not mapped (hal=%" PRIx64 " glue=%" PRIx64 ")\n", hal, glue); return 1; }
    char mp[64];
    snprintf(mp, sizeof(mp), "/proc/%d/mem", pid);
    memfd = open(mp, O_RDONLY);
    if (memfd < 0) { perror("open mem"); return 1; }

    uint64_t func = hal + foff;
    uint8_t code[8], want[8];
    for (int i = 0; i < 8; i++) sscanf(argv[3] + 2 * i, "%2hhx", &want[i]);
    if (rd(func, code, 8) || memcmp(code, want, 8)) { fprintf(stderr, "prologue mismatch at %" PRIx64 "\n", func); return 1; }
    uint64_t slot = 0, dev = 0;
    if (rd(glue + GOODIX_DEV_GOT, &slot, 8) || !slot || rd(slot, &dev, 8) || !dev) {
        fprintf(stderr, "dev not found (slot=%" PRIx64 ")\n", slot);
        return 1;
    }
    printf("hal=%" PRIx64 " func=%" PRIx64 " dev=%" PRIx64 " arg1=%" PRIu64 "\n", hal, func, dev, arg1);

    if (ptrace(PTRACE_SEIZE, pid, 0, 0) || ptrace(PTRACE_INTERRUPT, pid, 0, 0)) { perror("seize"); return 1; }
    int st;
    if (waitpid(pid, &st, __WALL) < 0) { perror("wait"); return 1; }

    struct user_pt_regs saved, r;
    struct user_fpsimd_state fsaved;
    if (getregs(pid, &saved) || getfp(pid, &fsaved)) { perror("getregs"); ptrace(PTRACE_DETACH, pid, 0, 0); return 1; }
    r = saved;
    r.regs[0] = dev;
    r.regs[1] = arg1;
    r.regs[30] = 0;  // return to NULL -> SIGSEGV stop we catch
    r.sp = (saved.sp - 0x400) & ~0xfULL;
    r.pc = func;
    setregs(pid, &r);
    ptrace(PTRACE_CONT, pid, 0, 0);

    int rc = 1;
    for (;;) {
        if (waitpid(pid, &st, __WALL) < 0) { perror("wait2"); break; }
        if (!WIFSTOPPED(st)) { fprintf(stderr, "target died\n"); return 1; }
        getregs(pid, &r);
        if (WSTOPSIG(st) == SIGSEGV && r.pc == 0) {
            printf("returned %" PRId64 " (0x%" PRIx64 ")\n", (int64_t)(int32_t)r.regs[0], r.regs[0]);
            rc = 0;
            break;
        }
        // another signal/stop during the call: let the call continue, drop the signal
        fprintf(stderr, "stop sig=%d pc=%" PRIx64 ", continuing\n", WSTOPSIG(st), r.pc);
        if (WSTOPSIG(st) == SIGSEGV) break;  // real crash inside the call: restore and bail
        ptrace(PTRACE_CONT, pid, 0, 0);
    }
    setregs(pid, &saved);
    setfp(pid, &fsaved);
    ptrace(PTRACE_DETACH, pid, 0, 0);
    return rc;
}
