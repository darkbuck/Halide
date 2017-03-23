#include "Elf.h"
#include "Debug.h"
#include "Util.h"
#include "Error.h"
#include "LLVM_Headers.h"

#include <map>
#include <memory>
#include <fstream>

namespace Halide {
namespace Internal {
namespace Elf {

// http://www.skyfree.org/linux/references/ELF_Format.pdf

enum Type {
    PT_NULL = 0,
    PT_LOAD = 1,
    PT_DYNAMIC = 2,
    PT_INTERP = 3,
    PT_NOTE = 4,
    PT_SHLIB = 5,
    PT_PHDR = 6,
    PT_LOPROC = 0x70000000,
    PT_HIPROC = 0x7fffffff,
};

enum Flag {
    PF_X = 1,
    PF_W = 2,
    PF_R = 4,
    PF_MASKOS = 0x0ff00000,
    PF_MASKPROC = 0xf0000000,
};

enum {
    DT_NULL = 0,
    DT_NEEDED = 1,
    DT_PLTRELSZ = 2,
    DT_PLTGOT = 3,
    DT_HASH = 4,
    DT_STRTAB = 5,
    DT_SYMTAB = 6,
    DT_RELA = 7,
    DT_RELASZ = 8,
    DT_RELAENT = 9,
    DT_STRSZ = 10,
    DT_SYMENT = 11,
    DT_INIT = 12,
    DT_FINI = 13,
    DT_SONAME = 14,
    DT_RPATH = 15,
    DT_SYMBOLIC = 16,
    DT_REL = 17,
    DT_RELSZ = 18,
    DT_RELENT = 19,
    DT_PLTREL = 20,
    DT_DEBUG = 21,
    DT_TEXTREL = 22,
    DT_JMPREL = 23,
    DT_LOPROC = 0x70000000,
    DT_HIPROC = 0x7fffffff,
};

static const char elf_magic[] = { 0x7f, 'E', 'L', 'F' };

template <int bits>
struct Types;

template <>
struct Types<32> {
    typedef uint32_t addr_t;
    typedef int32_t addr_off_t;
};

template <typename T>
struct Ehdr {
    typedef typename T::addr_t addr_t;
    typedef typename T::addr_off_t addr_off_t;

    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    addr_t e_entry;
    addr_t e_phoff;
    addr_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};


template <typename T>
struct Phdr {
    typedef typename T::addr_t addr_t;
    typedef typename T::addr_off_t addr_off_t;

    uint32_t p_type;
    uint32_t p_offset;
    addr_t p_vaddr;
    addr_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
};

template <typename T>
struct Shdr {
    typedef typename T::addr_t addr_t;
    typedef typename T::addr_off_t addr_off_t;

    uint32_t sh_name;
    uint32_t sh_type;
    addr_t sh_flags;
    addr_t sh_addr;
    addr_t sh_offset;
    addr_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    addr_t sh_addralign;
    addr_t sh_entsize;
};

template <typename T>
struct Rel {
    typedef typename T::addr_t addr_t;
    typedef typename T::addr_off_t addr_off_t;

    addr_t r_offset;
    addr_t r_info;

    uint32_t r_type() const {
        if (sizeof(addr_t) == 8) {
            return r_info & 0xffffffff;
        } else {
            return r_info & 0xff;
        }
    }

    uint32_t r_sym() const {
        if (sizeof(addr_t) == 8) {
            return (uint64_t)r_info >> 32;
        } else {
            return r_info >> 8;
        }
    }

    static addr_t make_info(uint32_t type, uint32_t sym) {
        if (sizeof(addr_t) == 8) {
            return (uint64_t)type | ((uint64_t)sym << 32);
        } else {
            return (type & 0xff) | (sym << 8);
        }
    }

    void set_r_type(uint32_t type) {
        r_info = make_info(type, r_sym());
    }

    void set_r_sym(uint32_t sym) {
        r_info = make_info(r_type(), sym);
    }

    Rel(addr_t offset, addr_t info)
        : r_offset(offset), r_info(info) {}

    Rel(addr_t offset, uint32_t type, uint32_t sym)
        : r_offset(offset), r_info(make_info(type, sym)) {}
};

template <typename T>
struct Rela : public Rel<T> {
    typedef typename T::addr_t addr_t;
    typedef typename T::addr_off_t addr_off_t;

    addr_off_t r_addend;

    Rela(addr_t offset, addr_t info, addr_off_t addend)
        : Rel<T>(offset, info), r_addend(addend) {}

    Rela(addr_t offset, uint32_t type, uint32_t sym, addr_off_t addend)
        : Rel<T>(offset, type, sym), r_addend(addend) {}
};

template <typename T>
struct Sym;

template <>
struct Sym<Types<32>> {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t st_info;
    uint8_t st_other;
    uint16_t st_shndx;

    uint8_t get_binding() const { return st_info >> 4; }
    uint8_t get_type() const { return st_info & 0xf; }

    static uint8_t make_info(uint8_t binding, uint8_t type) {
        return (binding << 4) | (type & 0xf);
    }

    void set_binding(uint8_t binding) {
        st_info = make_info(binding, get_type());
    }
    void set_type(uint8_t type) {
        st_info = make_info(get_binding(), type);
    }
};

template <typename T>
struct Dyn {
    typedef typename T::addr_t addr_t;
    typedef typename T::addr_off_t addr_off_t;

    uint32_t d_tag;
    union {
        uint32_t d_val;
        addr_t d_ptr;
    };
};

class StringTable {
    // TODO: We could be smarter and find substrings in the existing table.
    std::map<std::string, uint32_t> cache;

public:
    std::vector<char> table;

    StringTable() {
        // For our cache to work, we need something in the table to
        // start with so index 0 isn't valid.
        table.push_back(0);
    }

    uint32_t get(const std::string &str) {
        uint32_t &index = cache[str];
        if (index == 0) {
            index = table.size();
            table.insert(table.end(), str.begin(), str.end());
            table.push_back(0);
        }
        return index;
    }
};

const char *assert_string_valid(const char *name, const char *data, size_t size) {
    internal_assert(data <= name && name + strlen(name) + 1 <= data + size);
    return name;
}

template <typename T>
std::unique_ptr<Object> parse_object_internal(const char *data, size_t size) {
    Ehdr<T> header = *(const Ehdr<T> *)data;
    internal_assert(memcmp(header.e_ident, elf_magic, sizeof(elf_magic)) == 0);
    internal_assert(header.e_type == Object::ET_REL ||
                    header.e_type == Object::ET_DYN);

    std::unique_ptr<Object> obj(new Object());
    obj->set_type((Object::Type)header.e_type)
        .set_machine(header.e_machine)
        .set_version(header.e_version)
        .set_entry(header.e_entry)
        .set_flags(header.e_flags);

    auto get_section_header = [&](int idx) -> const Shdr<T>* {
        const char *at = data + header.e_shoff + idx*header.e_shentsize;
        internal_assert(data <= at && at + sizeof(Shdr<T>) <= data + size);
        return (const Shdr<T> *)at;
    };

    // Find the string table.
    const char *strings = nullptr;
    for (int i = 0; i < header.e_shnum; i++) {
        const Shdr<T> *sh = get_section_header(i);
        if (sh->sh_type == Section::SHT_STRTAB) {
            internal_assert(!strings);
            strings = data + sh->sh_offset;
            internal_assert(data <= strings && strings + sh->sh_size <= data + size);
        }
    }
    internal_assert(strings);

    // Load the rest of the sections.
    std::map<int, Section *> section_map;
    for (int i = 0; i < header.e_shnum; i++) {
        const Shdr<T> *sh = get_section_header(i);
        if (sh->sh_type != Section::SHT_SYMTAB && sh->sh_type != Section::SHT_STRTAB &&
            sh->sh_type != Section::SHT_REL && sh->sh_type != Section::SHT_RELA) {
            const char *name = assert_string_valid(&strings[sh->sh_name], data, size);
            auto section = obj->add_section(name, (Section::Type)sh->sh_type);
            section->set_flags(sh->sh_flags)
                .set_size(sh->sh_size)
                .set_alignment(sh->sh_addralign);
            if (sh->sh_type != Section::SHT_NULL) {
                const char *sh_data = data + sh->sh_offset;
                internal_assert(data <= sh_data && sh_data + sh->sh_size <= data + size);
                section->set_contents(sh_data, sh_data + sh->sh_size);
            }
            section_map[i] = &*section;
        }
    }

    // Find and load the symbols.
    std::map<int, Symbol *> symbol_map;
    for (int i = 0; i < header.e_shnum; i++) {
        const Shdr<T> *sh = get_section_header(i);
        if (sh->sh_type == Section::SHT_SYMTAB) {
            internal_assert(sh->sh_entsize == sizeof(Sym<T>));
            for (int j = 0; j < sh->sh_size / sizeof(Sym<T>); ++j) {
                const char *sym_ptr = data + sh->sh_offset + j*sizeof(Sym<T>);
                internal_assert(data <= sym_ptr && sym_ptr + sizeof(Sym<T>) <= data + size);
                const Sym<T> &sym = *(const Sym<T> *)sym_ptr;
                const char *name = assert_string_valid(&strings[sym.st_name], data, size);
                auto symbol = obj->add_symbol(name);
                symbol->set_type((Symbol::Type)sym.get_type())
                    .set_binding((Symbol::Binding)sym.get_binding());
                if (sym.st_shndx != 0) {
                    symbol->define(section_map[sym.st_shndx], sym.st_value, sym.st_size);
                }
                symbol_map[j] = &*symbol;
            }
        }
    }

    // Load relocations.
    for (int i = 0; i < header.e_shnum; i++) {
        const Shdr<T> *sh = get_section_header(i);
        internal_assert(sh->sh_type != Section::SHT_REL) << "Section::SHT_REL not supported\n";
        if (sh->sh_type == Section::SHT_RELA) {
            const char *name = assert_string_valid(&strings[sh->sh_name], data, size);
            internal_assert(strncmp(name, ".rela.", 6) == 0);
            internal_assert(sh->sh_entsize == sizeof(Rela<T>)) << sh->sh_entsize << " " << sizeof(Rela<T>);
            auto to_relocate = obj->find_section(name + 5);
            internal_assert(to_relocate != obj->sections_end());
            //internal_assert(&*to_relocate == section_map[sh->sh_link]);
            for (int i = 0; i < sh->sh_size / sh->sh_entsize; i++) {
                const char *rela_ptr = data + sh->sh_offset + i*sh->sh_entsize;
                internal_assert(data <= rela_ptr && rela_ptr + sizeof(Rela<T>) <= data + size);
                const Rela<T> &rela = *(const Rela<T> *)rela_ptr;
                Relocation reloc;
                reloc.set_type(rela.r_type())
                    .set_offset(rela.r_offset)
                    .set_addend(rela.r_addend)
                    .set_symbol(symbol_map[rela.r_sym()]);
                to_relocate->add_relocation(reloc);
            }
        }
    }

    return obj;
}

Object::symbol_iterator Object::add_symbol(const std::string &name) {
    syms.emplace_back(name);
    return std::prev(syms.end());
}

Object::section_iterator Object::add_section(const std::string &name, Section::Type type) {
    secs.emplace_back(name, type);
    return std::prev(secs.end());
}

Object::section_iterator Object::find_section(const std::string &name) {
    for (section_iterator i = sections_begin(); i != sections_end(); ++i) {
        if (i->get_name() == name) {
            return i;
        }
    }
    return sections_end();
}

std::unique_ptr<Object> Object::parse_object(const char *data, size_t size) {
    return parse_object_internal<Types<32>>(data, size);
}

template <typename T>
void append_object(std::vector<char> &buf, const T &data) {
    buf.insert(buf.end(), (const char *)&data, (const char *)(&data + 1));
}

template <typename It>
void append(std::vector<char> &buf, It begin, It end) {
    buf.reserve(buf.size() + std::distance(begin, end)*sizeof(*begin));
    for (It i = begin; i != end; i++) {
        append_object(buf, *i);
    }
}

void append_zeros(std::vector<char> &buf, size_t count) {
    buf.insert(buf.end(), count, (char)0);
}

void append_padding(std::vector<char> &buf, size_t alignment) {
    buf.resize((buf.size() + alignment - 1) & ~(alignment - 1));
}

Object::section_iterator Object::merge_sections(const std::vector<section_iterator> &to_merge) {
    internal_assert(!to_merge.empty());
    section_iterator merged = *to_merge.begin();

    std::vector<char> contents = merged->get_contents();

    // Make a new .text section to merge all of the text sections into.
    for (auto i = to_merge.begin() + 1; i != to_merge.end(); ++i) {
        section_iterator s = *i;
        internal_assert(s->get_type() == merged->get_type());

        // Make the new text section have an alignment that
        // satisfies all sections. This should be gcd, not max,
        // but we assume that all of the alignments are powers of
        // 2.
        uint32_t alignment = std::max(merged->get_alignment(), s->get_alignment());
        merged->set_alignment(alignment);

        append_padding(contents, alignment);
        // The offset of the section in the new merged section.
        uint64_t offset = contents.size();
        append(contents, s->get_contents().begin(), s->get_contents().end());

        for (auto j = s->relocations_begin(); j != s->relocations_end(); j++) {
            Elf::Relocation reloc = *j;
            reloc.set_offset(reloc.get_offset() + offset);
            merged->add_relocation(reloc);
        }

        // Find all of the symbols that were defined in this section, and update them.
        for (auto j = symbols_begin(); j != symbols_end(); j++) {
            if (j->get_section() == &*s) {
                j->define(&*merged, j->get_offset() + offset, j->get_size());
            }
        }
    }

    merged->set_contents(contents.begin(), contents.end());

    // Remove all of the sections we removed.
    for (auto i = to_merge.begin() + 1; i != to_merge.end(); ++i) {
        erase_section(*i);
    }

    return merged;
}

Object::section_iterator Object::merge_text_sections() {
    std::vector<section_iterator> text_sections;
    for (auto i = sections_begin(); i != sections_end(); i++) {
        if (i->get_type() == Section::SHT_PROGBITS && starts_with(i->get_name(), ".text")) {
            text_sections.push_back(i);
        }
    }
    section_iterator text = merge_sections(text_sections);
    text->set_name(".text");
    return text;
}

template <typename T, typename U>
T safe_cast(U x) {
    internal_assert(std::numeric_limits<T>::min() <= x && x <= std::numeric_limits<T>::max());
    return static_cast<T>(x);
}

template <typename T>
struct ObjectWriter {
    std::vector<char> output;
    Ehdr<T> ehdr;
    std::array<Phdr<T>, 3> phdrs;
};

template <typename T>
std::vector<char> write_shared_object_internal(Object &obj, Linker *linker) {
    // The buffer we will be writing to.
    std::vector<char> output;

    // Declare the things we need to put in the shared object.
    Ehdr<T> ehdr;
    std::array<Phdr<T>, 3> phdrs;
    memset(&ehdr, 0, sizeof(ehdr));
    memset(&phdrs[0], 0, sizeof(phdrs));
    auto &text_phdr = phdrs[0];
    auto &data_phdr = phdrs[1];
    //auto &dyn_phdr = phdrs[2];

    StringTable strings;

    std::vector<Shdr<T>> shdrs;
    Shdr<T> sh_null;
    memset(&sh_null, 0, sizeof(sh_null));
    shdrs.push_back(sh_null);

    // We also need a mapping of section objects to section headers.
    std::map<const Section *, uint16_t> section_idxs;

    // Define a helper function to write a section to the shared
    // object, making a section header for it.
    auto write_section = [&](const Section &s, uint32_t entsize = 0) {
        uint64_t alignment = s.get_alignment();
        append_padding(output, alignment);
        uint64_t offset = output.size();
        const std::vector<char> &contents = s.get_contents();
        append(output, contents.begin(), contents.end());
        append_padding(output, alignment);

        Shdr<T> shdr;
        shdr.sh_name = strings.get(s.get_name());
        shdr.sh_type = s.get_type();
        shdr.sh_flags = s.get_flags();
        shdr.sh_offset = offset;
        shdr.sh_addr = offset;
        shdr.sh_size = s.get_size();
        shdr.sh_addralign = alignment;

        shdr.sh_link = 0;
        shdr.sh_info = 0;
        shdr.sh_entsize = entsize;

        uint16_t shndx = safe_cast<uint16_t>(shdrs.size());
        section_idxs[&s] = shndx;
        shdrs.push_back(shdr);
        return shndx;
    };

    // And a helper to get the offset we've given a section.
    auto get_section_offset = [&](const Section &s) -> uint64_t {
        uint16_t idx = section_idxs[&s];
        return shdrs[idx].sh_offset;
    };

    // We need to define the GOT symbol.
    Section got(".got", Section::SHT_PROGBITS);
    got.set_alignment(4);
    Symbol got_sym("_GLOBAL_OFFSET_TABLE_");
    got_sym.define(&got, 0, 0);

    // Since we can't change the object, start a map of all of the symbols that we can mutate.
    std::map<const Symbol *, const Symbol *> symbols;
    symbols[&got_sym] = &got_sym;
    for (const Symbol &i : obj.symbols()) {
        if (i.get_name() == "_GLOBAL_OFFSET_TABLE_") {
            symbols[&i] = &got_sym;
        } else {
            symbols[&i] = &i;
        }
    }

    auto get_symbol = [&](const Relocation &r) {
        const Symbol *sym = r.get_symbol();
        auto i = symbols.find(sym);
        internal_assert(i != symbols.end()) << sym->get_name() << "\n";
        if (i != symbols.end()) return i->second;
        return sym;
    };

    auto needs_plt_entry = [&](const Relocation &r) {
        const Symbol *s = get_symbol(r);
        if (s->is_defined()) {
            return false;
        }

        if (s->get_type() != Symbol::STT_NOTYPE) {
            return false;
        }

        return linker->needs_plt_entry(r);
    };

    // We need to build the PLT, so it can be positioned along with
    // the rest of the text sections.
    Section plt(".plt", Section::SHT_PROGBITS);
    plt.set_flags(Section::SHF_ALLOC | Section::SHF_EXECINSTR);
    std::list<Symbol> plt_symbols;
    std::map<const Symbol *, const Symbol *> plt_defs;
    // Hack: We're defining the global offset table, so it shouldn't be treated as an external symbol.
    plt_defs[&got_sym] = &got_sym;
    linker->init_plt_section(plt, got);
    for (const Section &s : obj.sections()) {
        for (const Relocation &r : s.relocations()) {
            if (!needs_plt_entry(r)) {
                continue;
            }

            const Symbol *sym = get_symbol(r);
            const Symbol *& plt_def = plt_defs[sym];
            if (plt_def) {
                // We already made a PLT entry for this symbol.
                continue;
            }

            debug(0) << "Defining PLT entry for " << sym->get_name() << "\n";
            plt_symbols.push_back(linker->add_plt_entry(*sym, plt, got, got_sym));

            plt_def = &plt_symbols.back();
            symbols[plt_def] = plt_def;
        }
    }

    auto get_symbol_offset = [&](const Symbol &sym) -> uint64_t {
        if (sym.is_defined()) {
            return get_section_offset(*sym.get_section()) + sym.get_offset();
        }
        return 0;
    };

    // Start placing the sections into the shared object.

    // Leave room for the header, and program headers at the beginning of the file.
    append_zeros(output, sizeof(ehdr));
    append_zeros(output, sizeof(phdrs[0])*3);

    // We need to perform the relocations. To do that, we need to position the sections
    // where they will go in the final shared object.
    text_phdr.p_type = PT_LOAD;
    text_phdr.p_flags = PF_X | PF_R;
    text_phdr.p_offset = 0;
    text_phdr.p_align = 4096;

    uint16_t plt_idx = write_section(plt);
    plt_idx = plt_idx;
    for (const Section &s : obj.sections()) {
        if (s.is_alloc() && !s.is_writable()) {
            write_section(s);
        }
    }
    append_padding(output, 4096);
    text_phdr.p_filesz = output.size() - text_phdr.p_offset;

    data_phdr.p_type = PT_LOAD;
    data_phdr.p_flags = PF_W | PF_R;
    data_phdr.p_offset = output.size();
    data_phdr.p_align = 4096;
    for (const Section &s : obj.sections()) {
        if (s.is_alloc() && s.is_writable()) {
            write_section(s);
        }
    }

    /// Now that we've written the sections that define symbols, we
    // can generate the symbol table.
    Section symtab(".symtab", Section::SHT_SYMTAB);
    symtab.set_alignment(4);
    std::vector<Sym<T>> syms;
    Sym<T> undef_sym;
    memset(&undef_sym, 0, sizeof(undef_sym));
    syms.push_back(undef_sym);
    std::map<const Symbol *, uint16_t> symbol_idxs;
    for (const auto &i : symbols) {
        const Symbol *s = i.second;
        uint64_t value = s->get_offset();
        // In shared objects, the symbol value is a virtual address,
        // not a section offset.
        if (s->is_defined()) {
            value += get_section_offset(*s->get_section());
        }
        Sym<T> sym;
        sym.st_name = strings.get(s->get_name());
        sym.st_value = value;
        sym.st_size = s->get_size();
        sym.set_type(s->get_type());
        sym.set_binding(s->get_binding());
        sym.st_other = 0;
        sym.st_shndx = section_idxs[s->get_section()];

        symbol_idxs[s] = syms.size();
        syms.push_back(sym);
    }
    symtab.set_contents(syms);
    uint16_t symtab_idx = write_section(symtab, sizeof(syms[0]));
    symtab_idx = symtab_idx;

    auto do_relocations = [&](const Section &s) {
        debug(0) << "Processing relocations for section " << s.get_name() << "\n";
        for (const Relocation &r : s.relocations()) {
            const Symbol *sym = get_symbol(r);
            debug(0) << "Processing relocation of type " << r.get_type()
                     << " for symbol " << sym->get_name() << "\n";
            if (needs_plt_entry(r)) {
                // This relocation is a function call, we need to use the PLT entry for this symbol.
                auto plt_def = plt_defs.find(sym);
                internal_assert(plt_def != plt_defs.end());
                sym = plt_def->second;
            }

            uint64_t fixup_offset = get_section_offset(s) + r.get_offset();
            char *fixup_addr = output.data() + fixup_offset;
            uint64_t sym_offset = get_symbol_offset(*sym);
            if (sym_offset == 0) {
                // This is an external symbol. We need to add a GOT
                // slot with a relocation for this symbol, and perform
                // the relocation with the GOT slot as the symbol.
                sym_offset = linker->add_got_entry(got, *sym);
            }
            linker->relocate(fixup_offset, fixup_addr, r.get_type(), sym_offset, r.get_addend(), got);
        }
    };

    // Now that we've generated the symbol table, we can do relocations.
    do_relocations(plt);
    for (const Section &s : obj.sections()) {
        do_relocations(s);
    }

    auto write_relocation_section = [&](const Section &s) {
        uint64_t alignment = 8;
        append_padding(output, alignment);
        uint64_t offset = output.size();
        for (const Relocation &r : s.relocations()) {
            uint64_t i_offset = offset + r.get_offset();
            Rela<T> rela(i_offset, r.get_type(), symbol_idxs[get_symbol(r)], r.get_addend());
            append_object(output, rela);
        }
        uint64_t size = output.size() - offset;
        append_padding(output, alignment);

        Shdr<T> shdr;
        shdr.sh_name = strings.get(".rela" + s.get_name());
        shdr.sh_type = Section::SHT_RELA;
        shdr.sh_flags = 0;
        shdr.sh_offset = offset;
        shdr.sh_addr = offset;
        shdr.sh_size = size;
        shdr.sh_addralign = alignment;

        shdr.sh_link = symtab_idx;
        shdr.sh_info = section_idxs[&s];
        shdr.sh_entsize = sizeof(Rela<T>);

        uint16_t shndx = safe_cast<uint16_t>(shdrs.size());
        section_idxs[&s] = shndx;
        shdrs.push_back(shdr);
        return shndx;
    };

    uint16_t got_idx = write_section(got);
    write_relocation_section(got);
    got_idx = got_idx;

    Section strtab(".strtab", Section::SHT_STRTAB);
    strings.get(strtab.get_name());
    strtab.set_contents(strings.table);
    uint16_t strtab_idx = write_section(strtab);

    append_padding(output, 4096);
    data_phdr.p_filesz = output.size() - data_phdr.p_offset;

    // Setup the section headers.
    shdrs[symtab_idx].sh_link = strtab_idx;

    // Write the section header table.
    ehdr.e_shoff = output.size();
    ehdr.e_shnum = shdrs.size();
    ehdr.e_shentsize = sizeof(shdrs[0]);
    for (auto &i : shdrs) {
        append_object(output, i);
    }

    // Now go back and write the headers.
    memcpy(ehdr.e_ident, elf_magic, 4);
    ehdr.e_ident[4] = 1;
    ehdr.e_ident[5] = 1;
    ehdr.e_type = Object::ET_DYN;
    ehdr.e_machine = obj.get_machine();
    ehdr.e_ehsize = sizeof(ehdr);
    ehdr.e_version = obj.get_version();
    ehdr.e_entry = obj.get_entry();
    ehdr.e_flags = obj.get_flags();
    ehdr.e_phoff = sizeof(ehdr);
    ehdr.e_phentsize = sizeof(phdrs[0]);
    ehdr.e_phnum = phdrs.size();
    ehdr.e_shstrndx = strtab_idx;

    memcpy(output.data(), &ehdr, sizeof(ehdr));
    for (auto &i : phdrs) {
        i.p_vaddr = i.p_offset;
        i.p_paddr = i.p_offset;
        i.p_memsz = i.p_filesz;
    }
    memcpy(output.data() + ehdr.e_phoff, phdrs.data(), sizeof(phdrs));

    std::ofstream debug_output("/tmp/debug.so");
    debug_output.write(output.data(), output.size());
    debug_output.flush();

    auto test = Object::parse_object(output.data(), output.size());
    test->dump();
/*
    std::error_code ec;
    llvm::MemoryBufferRef objbuf(llvm::StringRef(output.data(), output.size()), "");
    llvm::object::ELF32LEObjectFile asdf(objbuf, ec);

    for (auto i : asdf.symbols()) {
        debug(0) << i.getName()->str() << "\n";
    }
*/
    return output;

/*
    Section dynamic(".dynamic", Section::SHT_DYNAMIC);
    uint16_t dynamic_idx = add_section(&dynamic);
    uint16_t strtab_idx = add_section(&strtab);
    uint16_t symtab_idx = add_section(&symtab);

    // These sections need the sh_link header entry to point to the string table.
    shdrs[dynamic_idx].sh_link = strtab_idx;
    shdrs[symtab_idx].sh_link = strtab_idx;

    // These sections need the sh_link header entry to point to the symbol table.
    //shdrs[hash_idx].sh_link = symtab_idx];
    for (auto i : rel_sections) {
        shdrs[section_indices[&*i]].sh_link = symtab_idx;
    }
*/
    /*
    std::vector<Dyn<T>> dyn;
    auto make_dyn = [](int32_t tag, uint32_t val = 0) {
        HexagonBinary::Dyn dyn;
        dyn.d_tag = tag;
        dyn.d_val = val;
        return dyn;
    };

    // TODO: Elf docs claim this is required...
    dyn.push_back(make_dyn(DT_HASH, 0));

    // Address of the string table.
    strtab->set_offset(offset);
    strtab->set_type(SHT_STRTAB);
    offset = (offset + strtab->get_size() + section_alignment - 1) & ~(section_alignment - 1);
    dyn.push_back(make_dyn(DT_STRTAB, strtab->get_offset()));
    dyn.push_back(make_dyn(DT_STRSZ, strtab->get_size()));

    // Address of the symbol table.
    symtab->set_offset(offset);
    symtab->set_type(SHT_SYMTAB);
    symtab->shdr.sh_link = strtab->get_index();
    symtab->shdr.sh_entsize = sizeof(HexagonBinary::Sym);
    for (auto i = obj.symbols_begin(); i != obj.symbols_end(); i++) {
        append(symtab->contents, i->sym);
    }
    symtab->shdr.sh_size = symtab->contents.size();
    offset = (offset + symtab->get_size() + section_alignment - 1) & ~(section_alignment - 1);
    dyn.push_back(make_dyn(DT_SYMTAB, symtab->get_offset()));
    dyn.push_back(make_dyn(DT_SYMENT, symtab->shdr.sh_entsize));

    // Offset to the GOT.
    Object::section_iterator got_section = obj.find_section(".got");
    if (got_section != obj.sections_end()) {
        dyn.push_back(make_dyn(DT_PLTGOT, got_section->get_offset()));
    }

    // Null terminator.
    dyn.push_back(make_dyn(DT_NULL));

    dynamic->set_contents(dyn);

    HexagonBinary::Phdr dynamic_header;
    dynamic_header.p_type = PT_DYNAMIC;
    dynamic_header.p_offset = offset;
    dynamic_header.p_flags = PF_R;
    dynamic_header.p_filesz = dyn.size()*sizeof(dyn[0]);
    dynamic_header.p_memsz = dynamic_header.p_filesz;


    // We're done modifying the data in the shared object. Now, we need to build the headers.
    std::vector<char> output;

    HexagonBinary::Ehdr header = obj.get_header();
    header.e_type = ET_DYN;
    internal_assert(header.e_machine == 164);  // Hexagon
    header.e_entry = 0;
    header.e_phoff = sizeof(header);
    header.e_shoff = header.e_phoff + obj.phdrs.size()*sizeof(HexagonBinary::Phdr);
    header.e_phentsize = sizeof(HexagonBinary::Phdr);
    header.e_phnum = obj.phdrs.size();
    header.e_shentsize = sizeof(HexagonBinary::Shdr);
    header.e_shnum = obj.sections_size();
    header.e_shstrndx = strtab->get_index();


    output.reserve(offset);
    append(output, header);
    for (auto i = obj.phdrs.begin(); i != obj.phdrs.end(); i++) {
        i->p_vaddr = i->p_offset;
        i->p_paddr = 0;
        i->p_align = program_alignment;
        append(output, *i);
    }
    debug(0) << header.e_shnum << "\n";
    for (auto i = obj.sections_begin(); i != obj.sections_end(); i++) {
        debug(0) << i->get_name() << " " << i->get_size() << "\n";
        i->shdr.sh_addr = i->shdr.sh_offset;
        append(output, i->shdr);
    }
    append_padding(output, program_alignment);
    uint32_t min_section_offset = output.size();

    output.resize(offset);
    for (auto i = obj.sections_begin(); i != obj.sections_end(); i++) {
        if (i->get_type() != SHT_NULL) {
            internal_assert(i->get_offset() >= min_section_offset && i->get_offset() + i->contents.size() <= output.size())
                << i->get_name() << " has offset " << i->get_offset() << "\n";
            memcpy(&output[i->get_offset()], i->contents.data(), i->contents.size());
        } else {
            debug(0) << "Not writing null section " << i->get_name() << "\n";
        }
    }
    */
}

std::vector<char> Object::write_shared_object(Linker *linker) {
    return write_shared_object_internal<Types<32>>(*this, linker);
}


void Object::dump() {
    debug(0) << sections_size() << " sections:\n";
    int count = 0;
    for (auto i = sections_begin(); i != sections_end(); i++) {
        debug(0) << count++ << ": " << i->get_name() << " " << i->get_size() << "\n";
    }
    debug(0) << "\n";

    debug(0) << symbols_size() << " symbols:\n";
    count = 0;
    for (auto i = symbols_begin(); i != symbols_end(); i++) {
        debug(0) << count++ << ": " << i->get_name() << " ";
        if (i->get_section()) {
            debug(0) << i->get_section()->get_name() << " " << i->get_offset() << " " << i->get_size() << " ";
        }
        debug(0) << "\n";
    }
    debug(0) << "\n";
}


}  // namespace Elf
}  // namespace Internal
}  // namespace Halide
