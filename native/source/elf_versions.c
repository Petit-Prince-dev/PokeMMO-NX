#include "elf_versions.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool readSection(FILE *file, uint64_t bytes, const Elf64_Shdr *section, void *out) {
    return section->sh_offset <= bytes && section->sh_size <= bytes - section->sh_offset && section->sh_offset <= LONG_MAX &&
           fseek(file, (long)section->sh_offset, SEEK_SET) == 0 && fread(out, 1, section->sh_size, file) == section->sh_size;
}
static const char *stringAt(const char *strings, size_t bytes, uint32_t offset) {
    return offset < bytes && memchr(strings + offset, 0, bytes - offset) ? strings + offset : NULL;
}

bool elfReadVersions(FILE *file, uint64_t file_bytes, const Elf64_Shdr *sections, unsigned section_count, unsigned dynsym_index, const char *strings,
                     size_t string_bytes, size_t symbol_count, ElfSymbolVersion **out) {
    const Elf64_Shdr *versym = NULL, *verneed = NULL;
    *out = NULL;
    for (unsigned i = 0; i < section_count; ++i) {
        if (sections[i].sh_type == SHT_GNU_versym) {
            if (versym || sections[i].sh_link != dynsym_index) return false;
            versym = &sections[i];
        }
        if (sections[i].sh_type == SHT_GNU_verneed) {
            if (verneed || sections[i].sh_link != sections[dynsym_index].sh_link) return false;
            verneed = &sections[i];
        }
    }
    if (!versym) return !verneed;
    if (versym->sh_size != symbol_count * sizeof(Elf64_Half)) return false;
    bool ok = false;
    Elf64_Half *indices = malloc(versym->sh_size);
    unsigned char *requirements = NULL;
    ElfSymbolVersion *versions = calloc(symbol_count, sizeof(*versions));
    struct {
        unsigned index;
        const char *version, *library;
    } names[256];
    unsigned name_count = 0;
    if (!indices || !versions || !readSection(file, file_bytes, versym, indices)) goto done;
    if (verneed) {
        if (verneed->sh_size > 65536 || verneed->sh_size < sizeof(Elf64_Verneed) || !verneed->sh_info || verneed->sh_info > 256) goto done;
        requirements = malloc(verneed->sh_size);
        if (!requirements || !readSection(file, file_bytes, verneed, requirements)) goto done;
        uint64_t pos = 0;
        for (unsigned i = 0; i < verneed->sh_info; ++i) {
            Elf64_Verneed need;
            if (pos > verneed->sh_size || sizeof(need) > verneed->sh_size - pos) goto done;
            memcpy(&need, requirements + pos, sizeof(need));
            const char *library = stringAt(strings, string_bytes, need.vn_file);
            if (need.vn_version != 1 || !library || !*library || !need.vn_cnt || need.vn_cnt > 256 || need.vn_aux < sizeof(need) ||
                need.vn_aux > verneed->sh_size - pos)
                goto done;
            uint64_t auxpos = pos + need.vn_aux;
            for (unsigned j = 0; j < need.vn_cnt; ++j) {
                Elf64_Vernaux aux;
                if (auxpos > verneed->sh_size || sizeof(aux) > verneed->sh_size - auxpos || name_count == 256) goto done;
                memcpy(&aux, requirements + auxpos, sizeof(aux));
                const char *version = stringAt(strings, string_bytes, aux.vna_name);
                unsigned index = aux.vna_other & 0x7fff;
                if (!version || !*version || index < 2) goto done;
                for (unsigned k = 0; k < name_count; ++k)
                    if (names[k].index == index) goto done;
                names[name_count].index = index;
                names[name_count].version = version;
                names[name_count++].library = library;
                if (j + 1 == need.vn_cnt) {
                    if (aux.vna_next) goto done;
                } else {
                    if (aux.vna_next < sizeof(aux) || aux.vna_next > verneed->sh_size - auxpos) goto done;
                    auxpos += aux.vna_next;
                }
            }
            if (i + 1 == verneed->sh_info) {
                if (need.vn_next) goto done;
            } else {
                if (need.vn_next < sizeof(need) || need.vn_next > verneed->sh_size - pos) goto done;
                pos += need.vn_next;
            }
        }
    }
    for (size_t i = 0; i < symbol_count; ++i) {
        unsigned index = indices[i] & 0x7fff;
        for (unsigned j = 0; j < name_count; ++j)
            if (index == names[j].index) versions[i] = (ElfSymbolVersion){names[j].version, names[j].library};
        // Defined symbols may refer to VERDEF; only undefined imports need VERNEED.
        if (index > 1 && !versions[i].version) {
            Elf64_Sym symbol;
            const Elf64_Shdr *s = &sections[dynsym_index];
            uint64_t offset = s->sh_offset + i * sizeof(symbol);
            if (offset > file_bytes || sizeof(symbol) > file_bytes - offset || offset > LONG_MAX || fseek(file, (long)offset, SEEK_SET) ||
                fread(&symbol, 1, sizeof(symbol), file) != sizeof(symbol) || symbol.st_shndx == SHN_UNDEF)
                goto done;
        }
    }
    *out = versions;
    versions = NULL;
    ok = true;
done:
    free(requirements);
    free(indices);
    free(versions);
    return ok;
}
