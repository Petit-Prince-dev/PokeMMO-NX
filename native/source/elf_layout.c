#include "elf_layout.h"
#include "diagnostics.h"
#include <elf.h>
#include <limits.h>
#include <string.h>

static bool readAt(FILE *file, uint64_t size, uint64_t offset, void *out, size_t bytes) {
    return offset <= size && bytes <= size - offset && offset <= LONG_MAX && fseek(file, (long)offset, SEEK_SET) == 0 && fread(out, 1, bytes, file) == bytes;
}

bool elfReadNeeded(const char *path, char *names, unsigned name_bytes, unsigned max, unsigned *count) {
    *count = 0;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool ok = false;
    Elf64_Ehdr header;
    Elf64_Phdr loads[16];
    unsigned load_count = 0;
    uint64_t dynamic_offset = 0, dynamic_bytes = 0, string_table = 0, table_offset = 0;
    unsigned needed_offsets[32], needed_count = 0;
    bool mapped = false;
    long size;
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < (long)sizeof(header)) goto done;
    if (!readAt(file, size, 0, &header, sizeof(header)) || memcmp(header.e_ident, ELFMAG, SELFMAG) || header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_phentsize != sizeof(Elf64_Phdr) || !header.e_phnum || header.e_phnum > 256)
        goto done;
    for (unsigned i = 0; i < header.e_phnum; ++i) {
        Elf64_Phdr program;
        if (!readAt(file, size, header.e_phoff + i * sizeof(program), &program, sizeof(program))) goto done;
        if (program.p_type == PT_LOAD && load_count < 16) loads[load_count++] = program;
        if (program.p_type == PT_DYNAMIC) {
            dynamic_offset = program.p_offset;
            dynamic_bytes = program.p_filesz;
        }
    }
    if (!dynamic_bytes || dynamic_bytes > 65536) goto done;
    for (uint64_t at = 0; at + sizeof(Elf64_Dyn) <= dynamic_bytes; at += sizeof(Elf64_Dyn)) {
        Elf64_Dyn entry;
        if (!readAt(file, size, dynamic_offset + at, &entry, sizeof(entry))) goto done;
        if (entry.d_tag == DT_NULL) break;
        if (entry.d_tag == DT_STRTAB) string_table = entry.d_un.d_ptr;
        if (entry.d_tag == DT_NEEDED && needed_count < 32) needed_offsets[needed_count++] = (unsigned)entry.d_un.d_val;
    }
    for (unsigned i = 0; i < load_count; ++i)
        if (string_table >= loads[i].p_vaddr && string_table - loads[i].p_vaddr < loads[i].p_filesz) {
            table_offset = loads[i].p_offset + (string_table - loads[i].p_vaddr);
            mapped = true;
        }
    if (!mapped) goto done;
    for (unsigned i = 0; i < needed_count && *count < max; ++i) {
        char *slot = names + (size_t)*count * name_bytes;
        memset(slot, 0, name_bytes);
        uint64_t at = table_offset + needed_offsets[i];
        unsigned length = 0;
        while (length + 1 < name_bytes) {
            char c;
            if (!readAt(file, size, at + length, &c, 1) || !c) break;
            slot[length++] = c;
        }
        if (length) ++*count;
    }
    ok = true;
done:
    fclose(file);
    return ok;
}

bool elfReadLayout(const char *path, ElfLayout *layout, bool library) {
    memset(layout, 0, sizeof(*layout));
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool ok = false, entry_valid = false;
    uint64_t first = UINT64_MAX, first_page = UINT64_MAX, last_page = 0;
    Elf64_Ehdr header;
    long size;
    if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < (long)sizeof(header)) goto done;
    if (!readAt(file, size, 0, &header, sizeof(header)) || memcmp(header.e_ident, ELFMAG, SELFMAG) || header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_machine != EM_AARCH64 || header.e_type != ET_DYN || header.e_phentsize != sizeof(Elf64_Phdr) ||
        !header.e_phnum || header.e_phnum > 256 || header.e_phoff > (uint64_t)size || header.e_phnum * sizeof(Elf64_Phdr) > (uint64_t)size - header.e_phoff)
        goto done;
    layout->entry = header.e_entry;
    layout->program_header_offset = header.e_phoff;
    layout->program_header_count = header.e_phnum;
    layout->alignment = ELF_PAGE_SIZE;
    for (unsigned i = 0; i < header.e_phnum; ++i) {
        Elf64_Phdr program;
        if (!readAt(file, size, header.e_phoff + i * sizeof(program), &program, sizeof(program))) goto done;
        if (program.p_type == PT_TLS && library) {
            if (layout->has_tls || program.p_filesz > program.p_memsz || program.p_offset > (uint64_t)size ||
                program.p_filesz > (uint64_t)size - program.p_offset || program.p_memsz > 4096 * 4 ||
                (program.p_align && (program.p_align & (program.p_align - 1))))
                goto done;
            layout->has_tls = true;
            layout->tls_file_offset = program.p_offset;
            layout->tls_file_bytes = program.p_filesz;
            layout->tls_memory_bytes = program.p_memsz;
            layout->tls_alignment = program.p_align ? program.p_align : 1;
            continue;
        }
        if (program.p_type != PT_LOAD || !program.p_memsz) continue;
        if (layout->count == ELF_MAX_LOAD_SEGMENTS || program.p_filesz > program.p_memsz || program.p_offset > (uint64_t)size ||
            program.p_filesz > (uint64_t)size - program.p_offset || program.p_memsz > UINT64_MAX - program.p_vaddr || !(program.p_flags & PF_R) ||
            (program.p_flags & ~(PF_R | PF_W | PF_X)) || ((program.p_flags & PF_W) && (program.p_flags & PF_X)))
            goto done;
        uint64_t alignment = program.p_align;
        if (alignment > 1 && ((alignment & (alignment - 1)) || alignment > 0x200000 || (program.p_vaddr % alignment) != (program.p_offset % alignment)))
            goto done;
        if (alignment > layout->alignment) layout->alignment = alignment;
        uint64_t end = program.p_vaddr + program.p_memsz;
        if (end > UINT64_MAX - (ELF_PAGE_SIZE - 1)) goto done;
        uint64_t begin_page = program.p_vaddr & ~(uint64_t)(ELF_PAGE_SIZE - 1);
        uint64_t end_page = (end + ELF_PAGE_SIZE - 1) & ~(uint64_t)(ELF_PAGE_SIZE - 1);
        for (unsigned j = 0; j < layout->count; ++j) {
            ElfLoadSegment *other = &layout->segments[j];
            if (begin_page < other->page_vaddr + other->page_bytes && other->page_vaddr < end_page) goto done;
        }
        layout->segments[layout->count++] =
            (ElfLoadSegment){program.p_vaddr, program.p_filesz, program.p_memsz, begin_page, end_page - begin_page, program.p_flags};
        if (program.p_vaddr < first) first = program.p_vaddr;
        if (begin_page < first_page) first_page = begin_page;
        if (end_page > last_page) last_page = end_page;
        if ((program.p_flags & PF_X) && header.e_entry >= program.p_vaddr && header.e_entry - program.p_vaddr < program.p_filesz) entry_valid = true;
    }
    if (!layout->count || (!entry_valid && !library) || last_page <= first_page || last_page - first_page > 256 * 1024 * 1024) goto done;
    // Place the lowest page at an address preserving every PT_LOAD alignment.
    if (first_page % layout->alignment) goto done;
    layout->first_vaddr = first;
    layout->minimum_vaddr = first_page;
    layout->span = last_page - first_page;
    ok = true;
done:
    diagnosticsTrace("elf.layout=%s segments=%u span=%llu alignment=%llu entry=0x%llx", ok ? "PASS" : "FAIL", layout->count, (unsigned long long)layout->span,
                     (unsigned long long)layout->alignment, (unsigned long long)layout->entry);
    fclose(file);
    return ok;
}
