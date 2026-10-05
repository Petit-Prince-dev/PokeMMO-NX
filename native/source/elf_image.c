#include "elf_image.h"
#include "diagnostics.h"
#include "elf_versions.h"
#include <elf.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool readAt(FILE *f, uint64_t size, uint64_t offset, void *out, size_t bytes) {
    return offset <= size && bytes <= size - offset && offset <= LONG_MAX && fseek(f, (long)offset, SEEK_SET) == 0 && fread(out, 1, bytes, f) == bytes;
}

static bool loadedRange(const Elf64_Phdr *programs, unsigned count, uint64_t address, uint64_t bytes) {
    for (unsigned i = 0; i < count; ++i) {
        const Elf64_Phdr *p = &programs[i];
        if (p->p_type == PT_LOAD && address >= p->p_vaddr && address - p->p_vaddr <= p->p_memsz && bytes <= p->p_memsz - (address - p->p_vaddr)) return true;
    }
    return false;
}

void elfReleaseImage(ElfImage *image) {
    free(image->memory);
    memset(image, 0, sizeof(*image));
}

bool elfStageImage(const char *path, ElfImage *image, uintptr_t target_address, ElfImportResolver resolver, void *context, const ElfStageOptions *options) {
    static const ElfStageOptions executable = {0};
    if (!options) options = &executable;
    memset(image, 0, sizeof(*image));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = false;
    Elf64_Ehdr h;
    Elf64_Phdr *programs = NULL;
    Elf64_Shdr *sections = NULL;
    Elf64_Sym *symbols = NULL;
    char *strings = NULL;
    ElfSymbolVersion *versions = NULL;
    uint64_t first = UINT64_MAX, last = 0, symbol_count = 0, load_bias = 0;
    long size;
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < (long)sizeof(h)) goto done;
    if (!readAt(f, size, 0, &h, sizeof(h)) || memcmp(h.e_ident, ELFMAG, SELFMAG) || h.e_ident[EI_CLASS] != ELFCLASS64 || h.e_ident[EI_DATA] != ELFDATA2LSB ||
        h.e_machine != EM_AARCH64 || h.e_type != ET_DYN || h.e_phentsize != sizeof(Elf64_Phdr) || h.e_shentsize != sizeof(Elf64_Shdr) || !h.e_phnum ||
        h.e_phnum > 256 || !h.e_shnum || h.e_shnum > 4096)
        goto done;
    programs = malloc(h.e_phnum * sizeof(*programs));
    sections = malloc(h.e_shnum * sizeof(*sections));
    if (!programs || !sections || !readAt(f, size, h.e_phoff, programs, h.e_phnum * sizeof(*programs)) ||
        !readAt(f, size, h.e_shoff, sections, h.e_shnum * sizeof(*sections)))
        goto done;
    for (unsigned i = 0; i < h.e_phnum; ++i) {
        Elf64_Phdr *p = &programs[i];
        if (p->p_type != PT_LOAD) continue;
        if (p->p_filesz > p->p_memsz || p->p_memsz > UINT64_MAX - p->p_vaddr || p->p_offset > (uint64_t)size || p->p_filesz > (uint64_t)size - p->p_offset)
            goto done;
        if (p->p_vaddr < first) first = p->p_vaddr;
        if (p->p_vaddr + p->p_memsz > last) last = p->p_vaddr + p->p_memsz;
    }
    // Cap staging allocations. The current official image occupies about 101 MiB.
    if (first == UINT64_MAX || last <= first || last - first > 256 * 1024 * 1024) goto done;
    image->bytes = last - first;
    image->minimum_vaddr = first;
    image->memory = calloc(1, image->bytes);
    if (!image->memory) goto done;
    if (target_address && (target_address < first || image->bytes > UINTPTR_MAX - target_address)) goto done;
    load_bias = (target_address ? (uint64_t)target_address : (uint64_t)(uintptr_t)image->memory) - first;
    for (unsigned i = 0; i < h.e_phnum; ++i) {
        Elf64_Phdr *p = &programs[i];
        if (p->p_type == PT_LOAD && !readAt(f, size, p->p_offset, image->memory + p->p_vaddr - first, p->p_filesz)) goto done;
    }
    for (unsigned i = 0; i < h.e_shnum; ++i) {
        Elf64_Shdr *s = &sections[i];
        if (s->sh_type != SHT_DYNSYM) continue;
        if (symbols || s->sh_link >= h.e_shnum || s->sh_entsize != sizeof(Elf64_Sym) || s->sh_size % sizeof(Elf64_Sym) || s->sh_size > 16 * 1024 * 1024)
            goto done;
        Elf64_Shdr *str = &sections[s->sh_link];
        if (str->sh_type != SHT_STRTAB || !str->sh_size || str->sh_size > 16 * 1024 * 1024) goto done;
        symbols = malloc(s->sh_size);
        strings = malloc(str->sh_size);
        if (!symbols || !strings || !readAt(f, size, s->sh_offset, symbols, s->sh_size) || !readAt(f, size, str->sh_offset, strings, str->sh_size)) goto done;
        symbol_count = s->sh_size / sizeof(Elf64_Sym);
        if (options->library) {
            if (symbol_count > UINT32_MAX || str->sh_size > UINT32_MAX) goto done;
            image->symtab_vaddr = s->sh_addr;
            image->strtab_vaddr = str->sh_addr;
            image->symbol_count = (uint32_t)symbol_count;
            image->strtab_bytes = (uint32_t)str->sh_size;
        }
        if (resolver && !elfReadVersions(f, size, sections, h.e_shnum, i, strings, str->sh_size, symbol_count, &versions)) goto done;
        for (uint64_t j = 0; j < symbol_count; ++j) {
            Elf64_Sym *sym = &symbols[j];
            if (sym->st_name >= str->sh_size || !memchr(strings + sym->st_name, 0, str->sh_size - sym->st_name)) goto done;
            if (sym->st_shndx != SHN_UNDEF && !strcmp(strings + sym->st_name, "main")) {
                if (sym->st_shndx == SHN_ABS || !loadedRange(programs, h.e_phnum, sym->st_value, 4)) goto done;
                image->main_vaddr = sym->st_value;
            }
            // Record exports (name, address) so a caller can enter the image by symbol, e.g. graal_create_isolate.
            unsigned kind = ELF64_ST_TYPE(sym->st_info), binding = ELF64_ST_BIND(sym->st_info);
            if (sym->st_shndx != SHN_UNDEF && sym->st_shndx != SHN_ABS && sym->st_value && (kind == STT_FUNC || kind == STT_OBJECT) &&
                (binding == STB_GLOBAL || binding == STB_WEAK) && loadedRange(programs, h.e_phnum, sym->st_value, 0)) {
                size_t length = strlen(strings + sym->st_name);
                if (!length || length >= ELF_EXPORT_NAME_MAX || image->export_count >= ELF_EXPORT_MAX)
                    ++image->export_overflow;
                else {
                    ElfExport *item = &image->exports[image->export_count++];
                    memcpy(item->name, strings + sym->st_name, length + 1);
                    item->vaddr = sym->st_value;
                    item->type = kind;
                }
            }
        }
    }
    if (!symbols || (!options->library && !image->main_vaddr)) goto done;
    if (options->library) {
        // DT_NEEDED names and the initializers, from the .dynamic section.
        for (unsigned i = 0; i < h.e_shnum; ++i) {
            Elf64_Shdr *s = &sections[i];
            if (s->sh_type != SHT_DYNAMIC) continue;
            if (s->sh_size > 65536 || s->sh_size % sizeof(Elf64_Dyn)) goto done;
            Elf64_Dyn *entries = malloc(s->sh_size ? s->sh_size : 1);
            if (!entries || !readAt(f, size, s->sh_offset, entries, s->sh_size)) {
                free(entries);
                goto done;
            }
            for (uint64_t k = 0; k < s->sh_size / sizeof(Elf64_Dyn) && entries[k].d_tag != DT_NULL; ++k) {
                if (entries[k].d_tag == DT_INIT)
                    image->init_vaddr = entries[k].d_un.d_ptr;
                else if (entries[k].d_tag == DT_INIT_ARRAY)
                    image->init_array_vaddr = entries[k].d_un.d_ptr;
                else if (entries[k].d_tag == DT_INIT_ARRAYSZ)
                    image->init_array_bytes = entries[k].d_un.d_val;
                else if (entries[k].d_tag == DT_NEEDED && entries[k].d_un.d_val < image->strtab_bytes && image->needed_count < ELF_NEEDED_MAX) {
                    const char *name = strings + entries[k].d_un.d_val;
                    size_t length = strnlen(name, image->strtab_bytes - entries[k].d_un.d_val);
                    if (length && length < ELF_NEEDED_NAME_MAX) {
                        memcpy(image->needed[image->needed_count], name, length);
                        image->needed[image->needed_count++][length] = 0;
                    }
                }
            }
            free(entries);
        }
    }
    for (unsigned i = 0; i < h.e_shnum; ++i) {
        Elf64_Shdr *s = &sections[i];
        if (s->sh_type != SHT_RELA || !(s->sh_flags & SHF_ALLOC)) continue;
        if (s->sh_link >= h.e_shnum || sections[s->sh_link].sh_type != SHT_DYNSYM || s->sh_entsize != sizeof(Elf64_Rela) || s->sh_size % sizeof(Elf64_Rela) ||
            s->sh_offset > (uint64_t)size || s->sh_size > (uint64_t)size - s->sh_offset)
            goto done;
        for (uint64_t pos = 0; pos < s->sh_size; pos += sizeof(Elf64_Rela)) {
            Elf64_Rela r;
            if (!readAt(f, size, s->sh_offset + pos, &r, sizeof(r))) goto done;
            unsigned kind = ELF64_R_TYPE(r.r_info);
            uint64_t index = ELF64_R_SYM(r.r_info), value;
            if (kind == R_AARCH64_NONE) continue;
            if (!loadedRange(programs, h.e_phnum, r.r_offset, sizeof(value))) goto done;
            if (kind == R_AARCH64_RELATIVE) {
                if (index != 0) goto done;
                value = load_bias + (uint64_t)r.r_addend;
            } else if (kind == R_AARCH64_TLSDESC && options->library && options->tlsdesc_resolver) {
                // A TLS descriptor is two words: the resolver function and the variable's offset from the thread pointer.
                uint64_t symbol_offset = 0;
                if (!loadedRange(programs, h.e_phnum, r.r_offset, 16)) goto done;
                if (index) {
                    if (index >= symbol_count || symbols[index].st_shndx == SHN_UNDEF || ELF64_ST_TYPE(symbols[index].st_info) != STT_TLS) {
                        ++image->unsupported;
                        continue;
                    }
                    symbol_offset = symbols[index].st_value;
                }
                uint64_t pair[2] = {(uint64_t)options->tlsdesc_resolver, options->tls_offset + symbol_offset + (uint64_t)r.r_addend};
                memcpy(image->memory + r.r_offset - first, pair, sizeof(pair));
                ++image->relocated;
                ++image->tls_descriptors;
                continue;
            } else if (kind == R_AARCH64_ABS64 || kind == R_AARCH64_GLOB_DAT || kind == R_AARCH64_JUMP_SLOT) {
                if (index >= symbol_count) goto done;
                Elf64_Sym *sym = &symbols[index];
                if (sym->st_shndx == SHN_UNDEF) {
                    uintptr_t address = 0;
                    ElfImport imported = {strings + sym->st_name, versions ? versions[index].version : NULL, versions ? versions[index].library : NULL,
                                          ELF64_ST_TYPE(sym->st_info), ELF64_ST_BIND(sym->st_info)};
                    bool bound = resolver && resolver(context, &imported, &address) && address;
                    bool weak = resolver && !bound && imported.binding == STB_WEAK;
                    if (bound || weak) {
                        value = (bound ? address : 0) + (uint64_t)r.r_addend;
                        memcpy(image->memory + r.r_offset - first, &value, sizeof(value));
                        if (bound)
                            ++image->resolved;
                        else
                            ++image->weak_null;
                        continue;
                    }
                    if (resolver)
                        diagnosticsTrace("elf.import=UNRESOLVED name=%s version=%s library=%s slot=0x%llx", imported.name,
                                         imported.version ? imported.version : "NONE", imported.library ? imported.library : "NONE",
                                         (unsigned long long)r.r_offset);
                    ++image->unresolved;
                    continue;
                }
                if (sym->st_shndx != SHN_ABS && !loadedRange(programs, h.e_phnum, sym->st_value, 0)) goto done;
                value = (sym->st_shndx == SHN_ABS ? 0 : load_bias) + sym->st_value + (uint64_t)r.r_addend;
            } else {
                ++image->unsupported;
                continue;
            }
            memcpy(image->memory + r.r_offset - first, &value, sizeof(value));
            ++image->relocated;
        }
    }
    ok = true;
done:
    diagnosticsTrace("elf.staging=%s bytes=%llu relocated=%u unresolved_slots=%u unsupported=%u main=0x%llx", ok ? "PASS" : "FAIL",
                     (unsigned long long)image->bytes, image->relocated, image->unresolved, image->unsupported, (unsigned long long)image->main_vaddr);
    if (resolver) diagnosticsTrace("elf.imports.summary=resolved:%u weak_null:%u unresolved:%u", image->resolved, image->weak_null, image->unresolved);
    free(strings);
    free(versions);
    free(symbols);
    free(sections);
    free(programs);
    fclose(f);
    if (!ok) elfReleaseImage(image);
    return ok;
}
