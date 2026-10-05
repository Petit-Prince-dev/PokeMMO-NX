#include "crash_recorder.h"

// Appends " name+0xoffset" when the value falls inside a known region (return addresses, code pointers).
static void annotate(FILE *out, uint64_t value, const CrashRegion *regions, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        if (regions[i].span && value >= regions[i].base && value - regions[i].base < regions[i].span) {
            fprintf(out, " %s+0x%llx", regions[i].name, (unsigned long long)(value - regions[i].base));
            return;
        }
}
static const char *crashDescriptionName(uint32_t description) {
    switch (description) {
        case 0x100: return "InstructionAbort";
        case 0x101: return "Other";
        case 0x102: return "MisalignedPC";
        case 0x103: return "MisalignedSP";
        case 0x104: return "Trap";
        case 0x106: return "SError";
        case 0x301: return "BadSVC";
        default: return "Unknown";
    }
}

void crashFormat(FILE *out, const CrashContext *c, const CrashRegion *regions, unsigned region_count, CrashReader read) {
    fprintf(out, "crash.exception=0x%x name=%s esr=0x%x far=0x%llx pstate=0x%x afsr0=0x%x afsr1=0x%x\n", c->description, crashDescriptionName(c->description),
            c->esr, (unsigned long long)c->far, c->pstate, c->afsr0, c->afsr1);
    fprintf(out, "crash.pc=0x%llx", (unsigned long long)c->pc);
    annotate(out, c->pc, regions, region_count);
    fputc('\n', out);
    fprintf(out, "crash.lr=0x%llx", (unsigned long long)c->lr);
    annotate(out, c->lr, regions, region_count);
    fputc('\n', out);
    fprintf(out, "crash.sp=0x%llx fp=0x%llx\n", (unsigned long long)c->sp, (unsigned long long)c->fp);
    for (unsigned i = 0; i < region_count; ++i)
        fprintf(out, "crash.region=%s base=0x%llx span=0x%llx\n", regions[i].name, (unsigned long long)regions[i].base, (unsigned long long)regions[i].span);
    for (unsigned i = 0; i < 29; i += 4) {
        fprintf(out, "crash.regs");
        for (unsigned j = i; j < i + 4 && j < 29; ++j) fprintf(out, " x%u=0x%llx", j, (unsigned long long)c->x[j]);
        fputc('\n', out);
    }
    if (read) {
        // Frame-pointer chain: each frame holds {previous fp, return address}; the stack grows down, so
        // the chain must climb. Stop at the first unreadable, misaligned or non-increasing frame.
        uint64_t fp = c->fp, previous = c->sp;
        for (unsigned depth = 0; depth < CRASH_MAX_FRAMES && fp && !(fp & 15) && fp > previous; ++depth) {
            uint64_t frame[2];
            if (!read((uintptr_t)fp, frame, sizeof(frame))) break;
            fprintf(out, "crash.frame=%u fp=0x%llx return=0x%llx", depth, (unsigned long long)fp, (unsigned long long)frame[1]);
            annotate(out, frame[1], regions, region_count);
            fputc('\n', out);
            previous = fp;
            fp = frame[0];
        }
        for (unsigned i = 0; i < CRASH_STACK_WORDS; ++i) {
            uint64_t value;
            if (!read((uintptr_t)(c->sp + 8u * i), &value, sizeof(value))) break;
            fprintf(out, "crash.stack=%u addr=0x%llx value=0x%llx", i, (unsigned long long)(c->sp + 8u * i), (unsigned long long)value);
            annotate(out, value, regions, region_count);
            fputc('\n', out);
        }
    }
    fputs("crash.end\n", out);
    fflush(out);
}
