#ifndef HALIDE_ELF_H
#define HALIDE_ELF_H

#include <memory>
#include <vector>
#include <list>
#include <string>
#include <iterator>
#include <limits>

namespace Halide {
namespace Internal {
namespace Elf {

class Object;
class Symbol;
class Section;
class Relocation;

class Symbol {
public:
    enum Binding {
        STB_LOCAL = 0,
        STB_GLOBAL = 1,
        STB_WEAK = 2,
        STB_LOPROC = 13,
        STB_HIPROC = 15,
    };

    enum Type {
        STT_NOTYPE = 0,
        STT_OBJECT = 1,
        STT_FUNC = 2,
        STT_SECTION = 3,
        STT_FILE = 4,
        STT_LOPROC = 13,
        STT_HIPROC = 15,
    };

private:
    std::string name;
    Section *definition = nullptr;
    uint64_t offset = 0;
    uint32_t size = 0;
    Binding binding = STB_LOCAL;
    Type type = STT_NOTYPE;

public:
    Symbol() {}
    Symbol(const std::string &name) : name(name) {}

    const std::string &get_name() const { return name; }
    Section *get_section() { return definition; }
    const Section *get_section() const { return definition; }
    uint64_t get_offset() const { return offset; }
    uint32_t get_size() const { return size; }
    Binding get_binding() const { return binding; }
    Type get_type() const { return type; }
    bool is_defined() const { return definition != nullptr; }

    // Return reference to self to enable chaining of setters.
    Symbol &set_name(const std::string &name) { this->name = name; return *this; }
    Symbol &define(Section *section, uint64_t offset, uint32_t size) {
        this->definition = section;
        this->offset = offset;
        this->size = size;
        return *this;
    }
    Symbol &set_type(Type type) {
        this->type = type;
        return *this;
    }
    Symbol &set_binding(Binding binding) {
        this->binding = binding;
        return *this;
    }
};

class Relocation {
    uint32_t type = 0;
    uint64_t offset = 0;
    int64_t addend = 0;
    Symbol *symbol = nullptr;

public:
    Relocation() {}
    Relocation(uint32_t type, uint64_t offset, int64_t addend, Symbol *symbol)
        : type(type), offset(offset), addend(addend), symbol(symbol) {}

    uint32_t get_type() const { return type; }
    uint64_t get_offset() const { return offset; }
    int64_t get_addend() const { return addend; }
    Symbol *get_symbol() { return symbol; }
    const Symbol *get_symbol() const { return symbol; }

    Relocation &set_type(uint32_t type) { this->type = type; return *this; }
    Relocation &set_offset(uint64_t offset) { this->offset = offset; return *this; }
    Relocation &set_addend(int64_t addend) { this->addend = addend; return *this; }
    Relocation &set_symbol(Symbol *symbol) { this->symbol = symbol; return *this; }
};

class Section {
public:
    enum Type {
        SHT_NULL = 0,
        SHT_PROGBITS = 1,
        SHT_SYMTAB = 2,
        SHT_STRTAB = 3,
        SHT_RELA = 4,
        SHT_HASH = 5,
        SHT_DYNAMIC = 6,
        SHT_NOTE = 7,
        SHT_NOBITS = 8,
        SHT_REL = 9,
        SHT_SHLIB = 10,
        SHT_DYNSYM = 11,
        SHT_LOPROC = 0x70000000,
        SHT_HIPROC = 0x7fffffff,
        SHT_LOUSER = 0x80000000,
        SHT_HIUSER = 0xffffffff,
    };

    enum Flag {
        SHF_WRITE = 0x1,
        SHF_ALLOC = 0x2,
        SHF_EXECINSTR = 0x4,
        SHF_MASKPROC = 0xf0000000,
    };

    typedef std::vector<Relocation> RelocationList;
    typedef RelocationList::iterator relocation_iterator;
    typedef RelocationList::const_iterator const_relocation_iterator;

private:
    std::string name;
    Type type = SHT_NULL;
    uint32_t flags = 0;
    std::vector<char> contents;
    // Sections may have a size larger than the contents.
    uint32_t size = 0;
    uint32_t alignment = 1;
    RelocationList relocations;

public:
    Section() {}
    Section(const std::string &name, Type type) : name(name), type(type) {}

    Section &set_name(const std::string &name) { this->name = name; return *this; }
    Section &set_type(Type type) { this->type = type; return *this; }
    Section &set_flag(Flag flag) { this->flags |= flag; return *this; }
    Section &remove_flag(Flag flag) { this->flags &= ~flag; return *this; }
    Section &set_flags(uint32_t flags) { this->flags = flags; return *this; }
    Section &set_size(uint32_t size) { this->size = size; return *this; }
    Section &set_alignment(uint32_t alignment) { this->alignment = alignment; return *this; }
    Section &set_relocations(std::vector<Relocation> relocations) { this->relocations = std::move(relocations); return *this; }
    template <typename It>
    Section &set_relocations(It begin, It end) { this->relocations.assign(begin, end); return *this; }

    Section &set_contents(std::vector<char> contents) { this->contents = std::move(contents); return *this; }
    template <typename T>
    Section &set_contents(const std::vector<T> &contents) {
        this->contents.assign((const char *)contents.data(), (const char *)(contents.data() + contents.size()));
        return *this;
    }
    template <typename It>
    Section &set_contents(It begin, It end) { this->contents.assign(begin, end); return *this; }
    template <typename It>
    Section &append_contents(It begin, It end) {
        this->contents.insert(this->contents.end(), begin, end);
        return *this;
    }
    template <typename T>
    Section &append_contents(const T& x) {
        return append_contents((const char *)&x, (const char *)(&x + 1));
    }

    const std::string &get_name() const { return name; }
    Type get_type() const { return type; }
    uint32_t get_flags() const { return flags; }
    bool is_alloc() const { return (flags & SHF_ALLOC) != 0; }
    bool is_writable() const { return (flags & SHF_WRITE) != 0; }
    const std::vector<char> &get_contents() const { return contents; }
    uint32_t get_size() const { return std::max(size, (uint32_t)contents.size()); }
    uint32_t get_alignment() const { return alignment; }


    relocation_iterator relocations_begin() { return relocations.begin(); }
    relocation_iterator relocations_end() { return relocations.end(); }
    const_relocation_iterator relocations_begin() const { return relocations.begin(); }
    const_relocation_iterator relocations_end() const { return relocations.end(); }
    size_t relocations_size() const { return relocations.size(); }
    void add_relocation(const Relocation &reloc) { relocations.push_back(reloc); }
};

class Linker {
public:
    virtual void add_plt_entry(Symbol &sym, Section &plt, Section &got) = 0;
    virtual void do_relocations(const Section &section, Section &got) = 0;
};

class Object {
public:
    enum Type {
        ET_NONE = 0,
        ET_REL = 1,
        ET_EXEC = 2,
        ET_DYN = 3,
        ET_CORE = 4,
        ET_LOPROC = 0xff00,
        ET_HIPROC = 0xffff,
    };

    typedef std::list<Section> SectionList;
    typedef typename SectionList::iterator section_iterator;
    typedef typename SectionList::const_iterator const_section_iterator;

    typedef std::list<Symbol> SymbolList;
    typedef typename SymbolList::iterator symbol_iterator;
    typedef typename SymbolList::const_iterator const_symbol_iterator;

private:
    SectionList sections;
    SymbolList symbols;

    Type type = ET_NONE;
    uint16_t machine = 0;
    uint32_t version = 0;
    uint64_t entry = 0;
    uint32_t flags = 0;

    Object(const Object &);
    void operator = (const Object &);

public:
    Object() {}

    Type get_type() const { return type; }
    uint16_t get_machine() const { return machine; }
    uint32_t get_version() const { return version; }
    uint64_t get_entry() const { return entry; }
    uint32_t get_flags() const { return flags; }

    Object &set_type(Type type) { this->type = type; return *this; }
    Object &set_machine(uint16_t machine) { this->machine = machine; return *this; }
    Object &set_version(uint32_t version) { this->version = version; return *this; }
    Object &set_entry(uint64_t entry) { this->entry = entry; return *this; }
    Object &set_flags(uint32_t flags) { this->flags = flags; return *this; }

    static std::unique_ptr<Object> parse_object(const char *data, size_t size);

    std::vector<char> write_shared_object(Linker *linker);

    void dump();

    // Sections.
    section_iterator sections_begin() { return sections.begin(); }
    section_iterator sections_end() { return sections.end(); }
    const_section_iterator sections_begin() const { return sections.begin(); }
    const_section_iterator sections_end() const { return sections.end(); }
    size_t sections_size() const { return sections.size(); }
    section_iterator find_section(const std::string &name);

    section_iterator add_section(const std::string &name, Section::Type type);
    section_iterator add_relocation_section(const Section &for_section);
    section_iterator erase_section(section_iterator i) { return sections.erase(i); }

    section_iterator merge_sections(const std::vector<section_iterator> &sections);
    section_iterator merge_text_sections();

    // Symbols.
    symbol_iterator symbols_begin() { return symbols.begin(); }
    symbol_iterator symbols_end() { return symbols.end(); }
    const_symbol_iterator symbols_begin() const { return symbols.begin(); }
    const_symbol_iterator symbols_end() const { return symbols.end(); }
    size_t symbols_size() const { return symbols.size(); }
    symbol_iterator find_symbol(const std::string &name);

    symbol_iterator add_symbol(const std::string &name);
};

}  // namespace Elf
}  // namespace Internal
}  // namespace Halide

#endif
