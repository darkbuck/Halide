#include <iostream>
#include <fstream>
#include <memory>

#include "HexagonOffload.h"
#include "Closure.h"
#include "InjectHostDevBufferCopies.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "LLVM_Output.h"
#include "LLVM_Headers.h"
#include "Param.h"
#include "RemoveTrivialForLoops.h"
#include "Substitute.h"

namespace Halide {
namespace Internal {

using std::string;
using std::vector;

// Defined below.
extern std::vector<Instruction> instruction_encodings;

namespace {

using ObjectFile = llvm::object::ELF32LEObjectFile;
using llvm::object::SectionRef;
using llvm::object::SymbolRef;

bool is_section_writable(const SectionRef &s) {
    const auto *shdr = s.getObject()->getSection(s.getRawDataRefImpl());
    return (shdr->sh_flags & llvm::ELF::SHF_WRITE) != 0;
}

void align_with_padding(llvm::SmallVectorImpl<char> &v, size_t alignment) {
    v.resize((v.size() + alignment - 1) & ~(alignment - 1));
}

void do_reloc(char *addr, uint32_t mask, uintptr_t val, bool is_signed, bool verify) {
    uint32_t inst = *((uint32_t *)addr);
    /*
    log_printf("Fixup inside instruction at %lx:\n  %08lx\n",
               (uint32_t)(addr - get_addr(get_section_offset(sec_text))), inst);
    log_printf("val: 0x%08lx\n", (unsigned long)val);
    log_printf("mask: 0x%08lx\n", mask);
    */

    if (!mask) {
        // The mask depends on the instruction. To implement
        // relocations for new instructions see
        // instruction_encodings.txt
        /*
        // First print the bits so I can search for it in the
        // instruction encodings.
        log_printf("Instruction bits: ");
        for (int i = 31; i >=0; i--) {
        log_printf("%d", (int)((inst >> i) & 1));
        }
        log_printf("\n");
        */

        if ((inst & (3 << 14)) == 0) {
            // Some instructions are actually pairs of 16-bit
            // subinstructions. See section 3.7 in the
            // programmer's reference.
            debug(3) << "Duplex\n";

            int iclass = ((inst >> 29) << 1) | ((inst >> 13) & 1);
            /*
                log_printf("Class: %x\n", iclass);
                log_printf("Hi: ");
                for (int i = 28; i >= 16; i--) {
                    log_printf("%d", (int)((inst >> i) & 1));
                }
                log_printf("\n");
                log_printf("Lo: ");
                for (int i = 12; i >= 0; i--) {
                    log_printf("%d", (int)((inst >> i) & 1));
                }
                log_printf("\n");
            */

            // We only know how to do the ones where the high
            // subinstruction is an immediate assignment. (marked
            // as A in table 9-4 in the programmer's reference
            // manual).
            internal_assert(3 <= iclass && iclass <= 7);

            // Pull out the subinstructions. They're the low 13
            // bits of each half-word.
            uint32_t hi = (inst >> 16) & ((1 << 13) - 1);
            uint32_t lo = inst & ((1 << 13) - 1);

            // We only understand the ones where hi starts with 010
            internal_assert((hi >> 10) == 2);

            // Low 6 bits of val go in the following bits.
            mask = 63 << 20;

        } else if ((inst >> 24) == 72) {
            // Example instruction encoding that has this high byte (ignoring bits 1 and 2):
            // 0100 1ii0  000i iiii  PPit tttt  iiii iiii
            debug(3) << "Instruction-specific case A\n";
            mask = 0x061f20ff;
        } else if ((inst >> 24) == 73) {
            // 0100 1ii1  000i iiii  PPii iiii  iiid dddd
            debug(3) << "Instruction-specific case B\n";
            mask = 0x061f3fe0;
        } else if ((inst >> 24) == 120) {
            // 0111 1000  ii-i iiii  PPii iiii  iiid dddd
            debug(3) << "Instruction-specific case C\n";
            mask = 0x00df3fe0;
        } else if ((inst >> 16) == 27209) {
            // 0110 1010  0100 1001  PP-i iiii  i--d dddd
            mask = 0x00001f80;
        } else if ((inst >> 25) == 72) {
            // 1001 0ii0  101s ssss  PPii iiii  iiid dddd
            // 1001 0ii1  000s ssss  PPii iiii  iiid dddd
            mask = 0x06003fe0;
        } else if ((inst >> 24) == 115 || (inst >> 24) == 124) {
            // 0111 0011 -10sssss PP1iiiii iiiddddd
            // 0111 0011 -11sssss PP1iiiii iiiddddd
            // 0111 0011 0uusssss PP0iiiii iiiddddd
            // 0111 0011 1uusssss PP0iiiii iiiddddd
            // 0111 0011 -00sssss PP1iiiii iiiddddd
            // 0111 0011 -01sssss PP1iiiii iiiddddd
            // 0111 1100 0IIIIIII PPIiiiii iiiddddd
            // 0111 0011 -11sssss PP1iiiii iiiddddd
            mask = 0x00001fe0;

        } else {
            internal_error << "Unhandled!\n";
        }
    }

    uintptr_t old_val = val;
    bool consumed_every_bit = false;
    for (int i = 0; i < 32; i++) {
        if (mask & (1 << i)) {
            internal_assert((inst & (1 << i)) == 0);

            // Consume a bit of val
            int next_bit = val & 1;
            if (is_signed) {
                consumed_every_bit |= ((intptr_t)val) == -1;
                val = ((intptr_t)val) >> 1;
            } else {
                val = ((uintptr_t)val) >> 1;
            }
            consumed_every_bit |= (val == 0);
            inst |= (next_bit << i);
        }
    }

    internal_assert(!verify || consumed_every_bit) << "Relocation overflow\n";

    *((uint32_t *)addr) = inst;
}

void do_relocations_for_section(char *base, SectionRef section, char *got_base, std::vector<uint32_t> &global_ofset_table) {
    // Read from the GP register for GP-relative relocations. We
    // need to do this with some inline assembly.
    char *GP = NULL;
    asm ("{%0 = gp}\n" : "=r"(GP) : : );
    if (debug) log_printf("GP = %p\n", GP);

    for (RelocationRef i : section.relocations()) {
        debug(0) << "Relocation of type " << i.getType() << "\n";

        // The location to make a change
        char *fixup_addr = base + section.getOffset() + i.getOffset();
        debug(0) << "Fixup address " << fixup_addr << "\n";

        // We're fixing up a reference to the following symbol
        SymbolRef sym = i.getSymbol();

        StringRef sym_name = *sym.getName();
        debug(0) << "Applies to symbol " << sym_name.str() << "\n";

        char *sym_addr = NULL;
        if (!symbol_is_defined(sym)) {
            if (strncmp(sym_name, "_GLOBAL_OFFSET_TABLE_", 22) == 0) {
                sym_addr = got_base;
            } else {
                // TODO: Implement imported symbols.
            }
            internal_assert(sym_addr) << "Failed to resolve external symbol: " << sym_name << "\n";
        } else {
            section_header_t *sym_sec = get_symbol_section(sym);
            const char *sym_sec_name = get_section_name(sym_sec);
            if (debug) log_printf("Symbol is in section: %s\n", sym_sec_name);

            sym_addr = get_symbol_addr(sym);
            if (debug) log_printf("Symbol is at address: %p\n", sym_addr);
        }

        // Hexagon relocations are specified in section 11.5 in
        // the Hexagon Application Binary Interface spec.

        // Find the symbol's index in the global_offset_table
        int global_offset_table_idx = (int)global_offset_table.size();
        for (int i = 0; i < (int)global_offset_table.size(); i++) {
            if ((elfaddr_t)sym_addr == global_offset_table[i]) {
                global_offset_table_idx = i;
                break;
            }
        }

        // Now we can define the variables from Table 11-5.
        char *S = sym_addr;
        char *P = fixup_addr;
        intptr_t A = rela->r_addend;
        elfaddr_t G = global_offset_table_idx * sizeof(elfaddr_t);

        // Define some constants from table 11-3
        const uint32_t Word32     = 0xffffffff;
        const uint32_t Word16     = 0xffff;
        const uint32_t Word8      = 0xff;
        const uint32_t Word32_B22 = 0x01ff3ffe;
        const uint32_t Word32_B15 = 0x00df20fe;
        const uint32_t Word32_B13 = 0x00202ffe;
        const uint32_t Word32_B9  = 0x003000fe;
        const uint32_t Word32_B7  = 0x00001f18;
        const uint32_t Word32_GP  = 0; // The mask is instruction-specific
        const uint32_t Word32_X26 = 0x0fff3fff;
        const uint32_t Word32_U6  = 0; // The mask is instruction-specific
        const uint32_t Word32_R6  = 0x000007e0;
        const uint32_t Word32_LO  = 0x00c03fff;
        const bool truncate = false, verify = true;
        const bool _unsigned = false, _signed = true;

        bool needs_global_offset_table_entry = false;

        switch (rela->r_type()) {
        case 1:
            // Address to fix up, mask, value, signed, verify
            do_reloc(fixup_addr, Word32_B22, intptr_t(S + A - P) >> 2, _signed, verify);
            break;
        case 2:
            // Untested
            do_reloc(fixup_addr, Word32_B15, intptr_t(S + A - P) >> 2, _signed, verify);
            break;
        case 3:
            // Untested
            do_reloc(fixup_addr, Word32_B7, intptr_t(S + A - P) >> 2, _signed, verify);
            break;
        case 4:
            // Untested
            do_reloc(fixup_addr, Word32_LO, uintptr_t(S + A), _unsigned, truncate);
            break;
        case 5:
            // Untested
            do_reloc(fixup_addr, Word32_LO, uintptr_t(S + A) >> 16, _unsigned, truncate);
            break;
        case 6:
            do_reloc(fixup_addr, Word32, intptr_t(S + A) >> 2, _signed, truncate);
            break;
        case 7:
            // Untested
            do_reloc(fixup_addr, Word16, uintptr_t(S + A), _unsigned, truncate);
            break;
        case 8:
            // Untested
            do_reloc(fixup_addr, Word8, uintptr_t(S + A), _unsigned, truncate);
            break;
        case 9:
            do_reloc(fixup_addr, Word32_GP, uintptr_t(S + A - GP), _unsigned, verify);
            break;
        case 10:
            do_reloc(fixup_addr, Word32_GP, uintptr_t(S + A - GP) >> 1, _unsigned, verify);
            break;
        case 11:
            do_reloc(fixup_addr, Word32_GP, uintptr_t(S + A - GP) >> 2, _unsigned, verify);
            break;
        case 12:
            do_reloc(fixup_addr, Word32_GP, uintptr_t(S + A - GP) >> 3, _unsigned, verify);
            break;
        case 13:
            // Untested
            do_reloc(fixup_addr,   Word32_LO, uintptr_t(S + A) >> 16, _unsigned, truncate);
            do_reloc(fixup_addr+4, Word32_LO, uintptr_t(S + A), _unsigned, truncate);
            break;
        case 14:
            // Untested
            do_reloc(fixup_addr, Word32_B13, intptr_t(S + A - P) >> 2, _signed, verify);
            break;
        case 15:
            // Untested
            do_reloc(fixup_addr, Word32_B9, intptr_t(S + A - P) >> 2, _signed, verify);
            break;
        case 16:
            do_reloc(fixup_addr, Word32_X26, intptr_t(S + A - P) >> 6, _signed, truncate);
            break;
        case 17:
            do_reloc(fixup_addr, Word32_X26, uintptr_t(S + A) >> 6, _unsigned, verify);
            break;
        case 18:
            // Untested
            do_reloc(fixup_addr, Word32_B22, intptr_t(S + A - P) & 0x3f, _signed, verify);
            break;
        case 19:
            // Untested
            do_reloc(fixup_addr, Word32_B15, intptr_t(S + A - P) & 0x3f, _signed, verify);
            break;
        case 20:
            // Untested
            do_reloc(fixup_addr, Word32_B13, intptr_t(S + A - P) & 0x3f, _signed, verify);
            break;
        case 21:
            // Untested
            do_reloc(fixup_addr, Word32_B9, intptr_t(S + A - P) & 0x3f, _signed, verify);
            break;
        case 22:
            // Untested
            do_reloc(fixup_addr, Word32_B7, intptr_t(S + A - P) & 0x3f, _signed, verify);
            break;
        case 23:
            do_reloc(fixup_addr, Word32_U6, uintptr_t(S + A), _unsigned, truncate);
            break;
        case 24:
            do_reloc(fixup_addr, Word32_R6, uintptr_t(S + A), _unsigned, truncate);
            break;
        case 25: // These ones all seem to mean the same thing. Only 30 is tested.
        case 26:
        case 27:
        case 28:
        case 29:
        case 30:
            do_reloc(fixup_addr, Word32_U6, uintptr_t(S + A), _unsigned, truncate);
            break;
        case 31:
            // Untested
            do_reloc(fixup_addr, Word32, intptr_t(S + A - P), _signed, verify);
            break;
        case 65:
            do_reloc(fixup_addr, Word32_U6, uintptr_t(S + A - P), _unsigned, truncate);
            break;
        case 69:
            do_reloc(fixup_addr, Word32_X26, intptr_t(G) >> 6, _signed, truncate);
            needs_global_offset_table_entry = true;
            break;
        case 71:
            do_reloc(fixup_addr, Word32_U6, uintptr_t(G), _unsigned, truncate);
            needs_global_offset_table_entry = true;
            break;

        default:
            internal_error << "Unhandled relocation type " << rela->r_type() << "\n";
        }

        if (needs_global_offset_table_entry &&
            global_offset_table_idx == (int)global_offset_table.size()) {
            // This symbol needs a slot in the global offset table
            global_offset_table.push_back((uint32_t)S);
        }
    }
}

void ld_shared_object(llvm::MemoryBufferRef object_ref, llvm::SmallVectorImpl<char> &shared_object) {
    // We need to rewrite the object into a shared object that is also a valid object.
    std::error_code err;
    ObjectFile obj(object_ref, err);
    if (err) {
        internal_error << "Failed to parse Hexagon object file";
    }
    internal_assert(obj.getELFFile()->getHeader()->e_type == llvm::ELF::ET_REL)
        << "Hexagon object is not relocatable.";

    const char *object_data = object_ref.getBufferStart();
    const size_t object_size = object_ref.getBufferSize();
    const size_t alignment = 4096;

    shared_object.reserve(object_size * 2 + 4096 * 3);
    shared_object.append(object_data, object_data + object_size);
    align_with_padding(shared_object, alignment);

    // Find the range of writable sections.
    const char *writable_begin = object_data + object_size;
    const char *writable_end = object_data;
    for (SectionRef s : obj.sections()) {
        const auto *shdr = obj.getSection(s.getRawDataRefImpl());
        if ((shdr->sh_flags & llvm::ELF::SHF_WRITE) != 0) {
            writable_begin = std::min(writable_begin, object_data + s.sh_offset);
            writable_end = std::max(writable_end, object_data + s.sh_offset + s.sh_size);
        }
    }

    // Find out how far the writable sections are moving.
    intptr_t writable_offset = 0;
    size_t writable_size = 0;
    if (writable_end > writable_begin) {
        writable_size = writable_end - writable_begin;
        writable_offset = object_size - (writable_begin - object_data);

        // Copy over the writable sections.  Copying over the span may
        // also copy a bunch of intermediate junk, e.g. if the first
        // and last sections of the object file are writeable, but in
        // practice we've found that the writeable sections are in one
        // tight cluster. We don't want to copy over the sections
        // individually, because some of them alias.
        // TODO: The aliasing is probably actually a problem, they are
        // likely due to SHT_NOBITS sections (like .bss) which should
        // be expanded such that they do not alias.
        shared_object.append(writable_begin, writable_end);
        align_with_padding(shared_object, alignment);
    }



/*
    llvm::ELF::Elf32_Ehdr header;
    memcpy(header.e_ident, llvm::ELF::ElfMagic, sizeof(header.e_ident));
    header.e_type = llvm::ELF::ET_DYN;
    header.e_machine = llvm::ELF::EM_HEXAGON;
    header.e_version = llvm::ELF::EV_CURRENT;
    header.e_entry = 0;
    header.e_phoff = sizeof(header);
    // header.e_shoff (section header offset) set after we build the rest of the sections.
    header.e_flags = llvm::ELF::EF_HEXAGON_ISA_V60;
    header.e_ehsize = sizeof(header);
    header.e_phentsize = sizeof(llvm::ELF::Elf32_Phdr);
    header.e_phnum = 1;
    header.e_shentsize = sizeof();
    header.e_shnum = ;
    header.e_shstrndx = ;

    llvm::ELF::Elf32_Phdr program_header;
    program_header.p_type = llvm::ELF::PT_LOAD;
    program_header.p_offset = ;
    program_header.p_vaddr = ;
    program_header.p_paddr = 0;
    program_header.p_filesz = ;
    program_header.p_memsz = ;
    // TODO: We need separate program headers for executable and writable sections.
    program_header.p_flags = llvm::ELF::PF_X | llvm::ELF::PF_W | llvm::ELF::PF_R;
    program_header.p_align = 4096;
*/
}

// Replace the parameter objects of loads/stores with a new parameter
// object.
class ReplaceParams : public IRMutator {
    const std::map<std::string, Parameter> &replacements;

    using IRMutator::visit;

    void visit(const Load *op) {
        auto i = replacements.find(op->name);
        if (i != replacements.end()) {
            expr = Load::make(op->type, op->name, mutate(op->index), op->image,
                              i->second, mutate(op->predicate));
        } else {
            IRMutator::visit(op);
        }
    }

    void visit(const Store *op) {
        auto i = replacements.find(op->name);
        if (i != replacements.end()) {
            stmt = Store::make(op->name, mutate(op->value), mutate(op->index),
                               i->second, mutate(op->predicate));
        } else {
            IRMutator::visit(op);
        }
    }

public:
    ReplaceParams(const std::map<std::string, Parameter> &replacements)
        : replacements(replacements) {}
};

Stmt replace_params(Stmt s, const std::map<std::string, Parameter> &replacements) {
    return ReplaceParams(replacements).mutate(s);
}

class InjectHexagonRpc : public IRMutator {
    std::map<std::string, Expr> state_vars;

    Module device_code;

    // Alignment info for Int(32) variables in scope, so we don't lose
    // the information when creating Hexagon kernels.
    Scope<ModulusRemainder> alignment_info;

    Expr state_var(const std::string& name, Type type) {
        Expr& var = state_vars[name];
        if (!var.defined()) {
            auto storage = Buffer<void *>::make_scalar(name + "_buf");
            storage() = nullptr;
            var = Load::make(type_of<void*>(), storage.name(), 0, storage, Parameter(), const_true());
        }
        return var;
    }

    Expr state_var_ptr(const std::string& name, Type type) {
        Expr var = state_var(name, type);
        return Call::make(Handle(), Call::address_of, {var}, Call::Intrinsic);
    }

    Expr module_state() {
        return state_var("hexagon_module_state", type_of<void*>());
    }

    Expr module_state_ptr() {
        return state_var_ptr("hexagon_module_state", type_of<void*>());
    }

    // Create a Buffer containing the given buffer/size, and return an
    // expression for a pointer to the first element.
    Expr buffer_ptr(const uint8_t* buffer, size_t size, const char* name) {
        Buffer<uint8_t> code((int)size, name);
        memcpy(code.data(), buffer, (int)size);
        Expr ptr_0 = Load::make(type_of<uint8_t>(), name, 0, code, Parameter(), const_true());
        return Call::make(Handle(), Call::address_of, {ptr_0}, Call::Intrinsic);
    }

    using IRMutator::visit;

    void visit(const For *loop) {
        if (loop->device_api != DeviceAPI::Hexagon) {
            IRMutator::visit(loop);
            return;
        }

        // Unrolling or loop partitioning might generate multiple
        // loops with the same name, so we need to make them unique.
        std::string hex_name = unique_name("hex_" + loop->name);

        // After moving this to Hexagon, it doesn't need to be marked
        // Hexagon anymore.
        Stmt body = For::make(loop->name, loop->min, loop->extent, loop->for_type,
                              DeviceAPI::None, loop->body);
        body = remove_trivial_for_loops(body);

        // Build a closure for the device code.
        // TODO: Should this move the body of the loop to Hexagon,
        // or the loop itself? Currently, this moves the loop itself.
        Closure c(body);

        // Make an argument list, and generate a function in the
        // device_code module. The hexagon runtime code expects
        // the arguments to appear in the order of (input buffers,
        // output buffers, input scalars).  Scalars must be last
        // for the scalar arguments to shadow the symbols of the
        // buffer that get generated by CodeGen_LLVM.
        std::vector<LoweredArgument> input_buffers, output_buffers;
        std::map<std::string, Parameter> replacement_params;
        for (const auto& i : c.buffers) {
            if (i.second.write) {
                Argument::Kind kind = Argument::OutputBuffer;
                output_buffers.push_back(LoweredArgument(i.first, kind, i.second.type, i.second.dimensions));
            } else {
                Argument::Kind kind = Argument::InputBuffer;
                input_buffers.push_back(LoweredArgument(i.first, kind, i.second.type, i.second.dimensions));
            }

            // Build a parameter to replace.
            Parameter p(i.second.type, true, i.second.dimensions);
            // Assert that buffers are aligned to one HVX vector.
            const int alignment = 128;
            p.set_host_alignment(alignment);
            // The other parameter constraints are already
            // accounted for by the closure grabbing those
            // arguments, so we only need to provide the host
            // alignment.
            replacement_params[i.first] = p;

            // Add an assert to the body that validates the
            // alignment of the buffer.
            if (!device_code.target().has_feature(Target::NoAsserts)) {
                Expr host_ptr = reinterpret<uint64_t>(Variable::make(Handle(), i.first + ".host"));
                Expr error = Call::make(Int(32), "halide_error_unaligned_host_ptr",
                                        {i.first, alignment}, Call::Extern);
                body = Block::make(AssertStmt::make(host_ptr % alignment == 0, error), body);
            }
        }
        body = replace_params(body, replacement_params);

        std::vector<LoweredArgument> args;
        args.insert(args.end(), input_buffers.begin(), input_buffers.end());
        args.insert(args.end(), output_buffers.begin(), output_buffers.end());
        for (const auto& i : c.vars) {
            LoweredArgument arg(i.first, Argument::InputScalar, i.second, 0);
            if (alignment_info.contains(i.first)) {
                arg.alignment = alignment_info.get(i.first);
            }
            args.push_back(arg);
        }
        device_code.append(LoweredFunc(hex_name, args, body, LoweredFunc::ExternalPlusMetadata));

        // Generate a call to hexagon_device_run.
        std::vector<Expr> arg_sizes;
        std::vector<Expr> arg_ptrs;
        std::vector<Expr> arg_flags;

        for (const auto& i : c.buffers) {
            // The Hexagon runtime expects buffer args to be
            // passed as just the device and host
            // field. CodeGen_Hexagon knows how to unpack buffers
            // passed this way.
            Expr buf = Variable::make(type_of<halide_buffer_t *>(), i.first + ".buffer");
            Expr device = Call::make(UInt(64), Call::buffer_get_device, {buf}, Call::Extern);
            Expr host = Call::make(Handle(), Call::buffer_get_host, {buf}, Call::Extern);
            Expr pseudo_buffer = Call::make(Handle(), Call::make_struct, {device, host}, Call::Intrinsic);
            arg_ptrs.push_back(pseudo_buffer);
            arg_sizes.push_back(Expr((uint64_t)(pseudo_buffer.type().bytes())));

            // In the flags parameter, bit 0 set indicates the
            // buffer is read, bit 1 set indicates the buffer is
            // written. If neither are set, the argument is a scalar.
            int flags = 0;
            if (i.second.read) flags |= 0x1;
            if (i.second.write) flags |= 0x2;
            arg_flags.push_back(flags);
        }
        for (const auto& i : c.vars) {
            Expr arg = Variable::make(i.second, i.first);
            Expr arg_ptr = Call::make(type_of<void *>(), Call::make_struct, {arg}, Call::Intrinsic);
            arg_sizes.push_back(Expr((uint64_t) i.second.bytes()));
            arg_ptrs.push_back(arg_ptr);
            arg_flags.push_back(0x0);
        }

        bool use_shared_object = device_code.target().has_feature(Target::HVX_shared_object);
        // The argument list is terminated with an argument of size 0.
        arg_sizes.push_back(Expr((uint64_t) 0));

        std::string pipeline_name = hex_name + "_argv";
        std::vector<Expr> params;
        params.push_back(use_shared_object);
        params.push_back(module_state());
        params.push_back(pipeline_name);
        params.push_back(state_var_ptr(hex_name, type_of<int>()));
        params.push_back(Call::make(type_of<size_t*>(), Call::make_struct, arg_sizes, Call::Intrinsic));
        params.push_back(Call::make(type_of<void**>(), Call::make_struct, arg_ptrs, Call::Intrinsic));
        params.push_back(Call::make(type_of<int*>(), Call::make_struct, arg_flags, Call::Intrinsic));

        stmt = call_extern_and_assert("halide_hexagon_run", params);
    }

    void visit(const Let *op) {
        if (op->value.type() == Int(32)) {
            alignment_info.push(op->name, modulus_remainder(op->value, alignment_info));
        }

        IRMutator::visit(op);

        if (op->value.type() == Int(32)) {
            alignment_info.pop(op->name);
        }
    }

    void visit(const LetStmt *op) {
        if (op->value.type() == Int(32)) {
            alignment_info.push(op->name, modulus_remainder(op->value, alignment_info));
        }

        IRMutator::visit(op);

        if (op->value.type() == Int(32)) {
            alignment_info.pop(op->name);
        }
    }

public:
    InjectHexagonRpc(const Target &target) : device_code("hexagon", target) {}

    Stmt inject(Stmt s) {
        s = mutate(s);

        // Skip if there are no device kernels.
        if (device_code.functions().empty()) {
            return s;
        }

        // Compile the device code
        debug(1) << "Hexagon device code module: " << device_code << "\n";

        llvm::LLVMContext context;
        std::unique_ptr<llvm::Module> llvm_module(compile_module_to_llvm_module(device_code, context));

        llvm::SmallVector<char, 4096> object;
        llvm::raw_svector_ostream object_stream(object);
        compile_llvm_module_to_object(*llvm_module, object_stream);

        if (debug::debug_level() >= 2) {
            debug(2) << "Hexagon device code assembly: " << "\n";
            llvm::SmallString<4096> assembly;
            llvm::raw_svector_ostream assembly_stream(assembly);
            compile_llvm_module_to_assembly(*llvm_module, assembly_stream);
            debug(2) << assembly.c_str() << "\n";
        }

        llvm::SmallVector<char, 4096> shared_object;
        ld_shared_object(llvm::MemoryBufferRef(llvm::StringRef(object.data(), object.size()), ""), shared_object);

        // Wrap the statement in calls to halide_initialize_kernels.
        size_t code_size = shared_object.size();
        Expr code_ptr = buffer_ptr(reinterpret_cast<uint8_t*>(&shared_object[0]), code_size, "hexagon_code");

        Stmt init_kernels = call_extern_and_assert("halide_hexagon_initialize_kernels",
                                                   {module_state_ptr(), code_ptr,
                                                    Expr((uint64_t)code_size),
                                                    Expr((uint32_t)0)});
        return Block::make(init_kernels, s);
    }
};

}

Stmt inject_hexagon_rpc(Stmt s, const Target &host_target) {
    // Make a new target for the device module.
    Target target(Target::NoOS, Target::Hexagon, 32);

    // These feature flags are propagated from the host target to the
    // device module.
    //
    // TODO: We'd like Target::Debug to be in this list too, but trunk
    // llvm currently disagrees with hexagon clang as to what
    // constitutes valid debug info.
    static const Target::Feature shared_features[] = {
        Target::Profile,
        Target::NoAsserts,
        Target::HVX_64,
        Target::HVX_128,
        Target::HVX_v62,
    };
    for (Target::Feature i : shared_features) {
        if (host_target.has_feature(i)) {
            target = target.with_feature(i);
        }
    }

    InjectHexagonRpc injector(target);
    s = injector.inject(s);
    return s;
}

// From Qualcomm
std::vector<Instruction> instruction_encodings = {
{ "p3=sp1loop0(#r7:2,#U10)",// instruction: J2_ploop1si
 0xffe00000, // instruction mask
 0x69a00000, // compare mask
 0x1f18, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=memubh(Rt32<<#3+#U6)",// instruction: L4_loadbzw4_ur_expand_shamt_3
 0xffe03080, // instruction mask
 0x9ca03080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "memd(gp+#u16:3)=Rtt32",// instruction: S2_storerdgp
 0xf9e00000, // instruction mask
 0x48c00000, // compare mask
 0x61f20ff, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.gt(Rs16,#-1); if (p1.new) jump:t #r9:2",// instruction: J4_cmpgtn1_tp1_jump_t
 0xffc02300, // instruction mask
 0x13802100, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "p3=sp2loop0(#r7:2,Rs32)",// instruction: J2_ploop2sr
 0xffe00000, // instruction mask
 0x60c00000, // compare mask
 0x1f18, // bitmask
 0x0 // isDuplex
},
{ "p3=sp2loop0(#r7:2,#U10)",// instruction: J2_ploop2si
 0xffe00000, // instruction mask
 0x69c00000, // compare mask
 0x1f18, // bitmask
 0x0 // isDuplex
},
{ "memb(Rs32+#u6:0)|=Rt32",// instruction: L4_or_memopb_io
 0xff602060, // instruction mask
 0x3e000060, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memuh(Rs32+#s11:1)",// instruction: L2_loadruh_io
 0xf9e00000, // instruction mask
 0x91600000, // compare mask
 0x6003fe0, // bitmask
 0x0 // isDuplex
},
{ "memb(Rs32+#u6:0)-=Rt32",// instruction: L4_sub_memopb_io
 0xff602060, // instruction mask
 0x3e000020, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "loop1(#r7:2,Rs32)",// instruction: J2_loop1r
 0xffe00000, // instruction mask
 0x60200000, // compare mask
 0x1f18, // bitmask
 0x0 // isDuplex
},
{ "loop1(#r7:2,#U10)",// instruction: J2_loop1i
 0xffe00000, // instruction mask
 0x69200000, // compare mask
 0x1f18, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=combine(Rs32,#s8)",// instruction: A4_combineri
 0xff602000, // instruction mask
 0x73002000, // compare mask
 0x1fe0, // bitmask
 0x0 // isDuplex
},
{ "if (!Pt4.new) Rd32=memh(#u6)",// instruction: L4_ploadrhfnew_abs
 0xffe03880, // instruction mask
 0x9f403880, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "memh(gp+#u16:1)=Rt32",// instruction: S2_storerhgp
 0xf9e00000, // instruction mask
 0x48400000, // compare mask
 0x61f20ff, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4) memb(#u6)=Rt32",// instruction: S4_pstorerbt_abs
 0xffe02084, // instruction mask
 0xaf000080, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=membh(Rt32<<#0+#U6)",// instruction: L4_loadbsw4_ur_expand_shamt_0
 0xffe03080, // instruction mask
 0x9ce01000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Re16=#U6 ; Rdd8=combine(#3,#u2)",// instruction: X2_AUTOJOIN_SA1_seti_SA1_combine3i
 0xfc003d18, // instruction mask
 0x28003c18, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Pd4=cmp.gt(Rs32,#s10)",// instruction: C2_cmpgti
 0xffc0001c, // instruction mask
 0x75400000, // compare mask
 0x203fe0, // bitmask
 0x0 // isDuplex
},
{ "Rd16=#u6 ; if (!p0.new) dealloc_return:nt",// instruction: X2_AUTOJOIN_SA1_seti_SL2_return_fnew
 0xfc003fc7, // instruction mask
 0x48003f47, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Ryy32=memb_fifo(Rs32+#s11:0)",// instruction: L2_loadalignb_io
 0xf9e00000, // instruction mask
 0x90800000, // compare mask
 0x6003fe0, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#s7) ; if (!p0) jumpr Lr",// instruction: X2_AUTOJOIN_SA1_addi_SL2_jumpr31_f
 0xf8003fc7, // instruction mask
 0x40003fc5, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "Rd16=#U6 ; memh(Rs16+#u3:1)=Rt16",// instruction: X2_AUTOJOIN_SA1_seti_SS2_storeh_io
 0xfc003800, // instruction mask
 0x68002000, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "memw(Rs32+#u6:2)|=Rt32",// instruction: L4_or_memopw_io
 0xff602060, // instruction mask
 0x3e400060, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "memb(Rs32+#u6:0)+=Rt32",// instruction: L4_add_memopb_io
 0xff602060, // instruction mask
 0x3e000000, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; Rdd8=combine(#3,#u2)",// instruction: X2_AUTOJOIN_SA1_addi_SA1_combine3i
 0xf8003d18, // instruction mask
 0x20003c18, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; Rdd8=combine(#2,#u2)",// instruction: X2_AUTOJOIN_SA1_addi_SA1_combine2i
 0xf8003d18, // instruction mask
 0x20003c10, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (!Pt4.new) Rd32=memb(Rs32+#u6:0)",// instruction: L2_ploadrbfnew_io
 0xffe02000, // instruction mask
 0x47000000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "if (cmp.gt(Ns8.new,Rt32)) jump:t #r9:2",// instruction: J4_cmpgt_t_jumpnv_t
 0xffc02000, // instruction mask
 0x20802000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#s7) ; if (p0) dealloc_return",// instruction: X2_AUTOJOIN_SA1_addi_SL2_return_t
 0xf8003fc7, // instruction mask
 0x40003f44, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (Pv4.new) memh(Rs32+#u6:1)=Nt8.new",// instruction: S4_pstorerhnewtnew_io
 0xffe01804, // instruction mask
 0x42a00800, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "Re16=#U6 ; Rd16=add(Rs16,#1)",// instruction: X2_AUTOJOIN_SA1_seti_SA1_inc
 0xfc003f00, // instruction mask
 0x28003100, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "memw(Re32=#U6)=Rt32",// instruction: S4_storeri_ap
 0xffe02080, // instruction mask
 0xab800080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4.new) memh(Rs32+#u6:1)=#S6",// instruction: S4_storeirhfnew_io
 0xffe00000, // instruction mask
 0x39a00000, // compare mask
 0x201f, // bitmask
 0x0 // isDuplex
},
{ "p1=tstbit(Rs16,#0); if (p1.new) jump:t #r9:2",// instruction: J4_tstbit0_tp1_jump_t
 0xffc02300, // instruction mask
 0x13802300, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "loop0(#r7:2,Rs32)",// instruction: J2_loop0r
 0xffe00000, // instruction mask
 0x60000000, // compare mask
 0x1f18, // bitmask
 0x0 // isDuplex
},
{ "loop0(#r7:2,#U10)",// instruction: J2_loop0i
 0xffe00000, // instruction mask
 0x69000000, // compare mask
 0x1f18, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.gtu(Rs16,#U5); if (!p1.new) jump:t #r9:2",// instruction: J4_cmpgtui_fp1_jump_t
 0xffc02000, // instruction mask
 0x13402000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Re16=#U6 ; Rd16=memw(Rs16+#u4:2)",// instruction: X2_AUTOJOIN_SA1_seti_SL1_loadri_io
 0xfc003000, // instruction mask
 0x48000000, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Re16=#U6 ; if (p0.new) Rd16=#0",// instruction: X2_AUTOJOIN_SA1_seti_SA1_clrtnew
 0xfc003e70, // instruction mask
 0x28003a40, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Re16=#U6 ; Rd16=add(Rs16,#-1)",// instruction: X2_AUTOJOIN_SA1_seti_SA1_dec
 0xfc003f00, // instruction mask
 0x28003300, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Rd32=+mpyi(Rs32,#u8)",// instruction: M2_mpysip
 0xff802000, // instruction mask
 0xe0000000, // compare mask
 0x1fe0, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4) memw(Rs32+#u6:2)=#S6",// instruction: S4_storeirif_io
 0xffe00000, // instruction mask
 0x38c00000, // compare mask
 0x201f, // bitmask
 0x0 // isDuplex
},
{ "Rx32=sub(#u8,lsr(Rx32,#U5))",// instruction: S4_subi_lsr_ri
 0xff000016, // instruction mask
 0xde000016, // compare mask
 0xe020e8, // bitmask
 0x0 // isDuplex
},
{ "memh(Re32=#U6)=Nt8.new",// instruction: S4_storerhnew_ap
 0xffe03880, // instruction mask
 0xaba00880, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; Rd16=memw(Sp+#u5:2)",// instruction: X2_AUTOJOIN_SA1_addi_SL2_loadri_sp
 0xf8003e00, // instruction mask
 0x40003c00, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "memh(Rs32+#u6:1)=clrbit(#U5)",// instruction: L4_iand_memoph_io
 0xff602060, // instruction mask
 0x3f200040, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "if (!Pt4.new) Rd32=memub(#u6)",// instruction: L4_ploadrubfnew_abs
 0xffe03880, // instruction mask
 0x9f203880, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "if (!cmp.gt(Ns8.new,Rt32)) jump:nt #r9:2",// instruction: J4_cmpgt_f_jumpnv_nt
 0xffc02000, // instruction mask
 0x20c00000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "memh(gp+#u16:1)=Nt8.new",// instruction: S2_storerhnewgp
 0xf9e01800, // instruction mask
 0x48a00800, // compare mask
 0x61f20ff, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=memubh(Rs32+#s11:2)",// instruction: L2_loadbzw4_io
 0xf9e00000, // instruction mask
 0x90a00000, // compare mask
 0x6003fe0, // bitmask
 0x0 // isDuplex
},
{ "if (!Pu4.new) Rd32=add(Rs32,#s8)",// instruction: A2_paddifnew
 0xff802000, // instruction mask
 0x74802000, // compare mask
 0x1fe0, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.gt(Rs16,Rt16); if (p1.new) jump:t #r9:2",// instruction: J4_cmpgt_tp1_jump_t
 0xffc03000, // instruction mask
 0x14803000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "memw(gp+#u16:2)=Nt8.new",// instruction: S2_storerinewgp
 0xf9e01800, // instruction mask
 0x48a01000, // compare mask
 0x61f20ff, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memub(gp+#u16:0)",// instruction: L2_loadrubgp
 0xf9e00000, // instruction mask
 0x49200000, // compare mask
 0x61f3fe0, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=memd(Rt32<<#2+#U6)",// instruction: L4_loadrd_ur_expand_shamt_2
 0xffe03080, // instruction mask
 0x9dc03000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=memd(Rt32<<#3+#U6)",// instruction: L4_loadrd_ur_expand_shamt_3
 0xffe03080, // instruction mask
 0x9dc03080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=memd(Rt32<<#0+#U6)",// instruction: L4_loadrd_ur_expand_shamt_0
 0xffe03080, // instruction mask
 0x9dc01000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=memd(Rt32<<#1+#U6)",// instruction: L4_loadrd_ur_expand_shamt_1
 0xffe03080, // instruction mask
 0x9dc01080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.gtu(Rs16,#U5); if (!p0.new) jump:t #r9:2",// instruction: J4_cmpgtui_fp0_jump_t
 0xffc02000, // instruction mask
 0x11402000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rd32=#s16",// instruction: A2_tfrsi
 0xff000000, // instruction mask
 0x78000000, // compare mask
 0xdf3fe0, // bitmask
 0x0 // isDuplex
},
{ "memb(Ru32<<#3+#U6)=Rt32",// instruction: S4_storerb_ur_expand_shamt_3
 0xffe020c0, // instruction mask
 0xad0020c0, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memb(Ru32<<#2+#U6)=Rt32",// instruction: S4_storerb_ur_expand_shamt_2
 0xffe020c0, // instruction mask
 0xad002080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memb(Ru32<<#1+#U6)=Rt32",// instruction: S4_storerb_ur_expand_shamt_1
 0xffe020c0, // instruction mask
 0xad0000c0, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memb(Ru32<<#0+#U6)=Rt32",// instruction: S4_storerb_ur_expand_shamt_0
 0xffe020c0, // instruction mask
 0xad000080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memw(Rs32+#s11:2)=Nt8.new",// instruction: S2_storerinew_io
 0xf9e01800, // instruction mask
 0xa1a01000, // compare mask
 0x60020ff, // bitmask
 0x0 // isDuplex
},
{ "memb(Rs32+#u6:0)=clrbit(#U5)",// instruction: L4_iand_memopb_io
 0xff602060, // instruction mask
 0x3f000040, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4.new) memh(#u6)=Rt.H32",// instruction: S4_pstorerffnew_abs
 0xffe02084, // instruction mask
 0xaf602084, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4) memh(#u6)=Rt32",// instruction: S4_pstorerhf_abs
 0xffe02084, // instruction mask
 0xaf400084, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4) memw(Rs32+#u6:2)=Nt8.new",// instruction: S2_pstorerinewf_io
 0xffe01804, // instruction mask
 0x44a01000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "memh(Rs32+#u6:1)+=Rt32",// instruction: L4_add_memoph_io
 0xff602060, // instruction mask
 0x3e200000, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; Rd16=add(Rs16,#-1)",// instruction: X2_AUTOJOIN_SA1_addi_SA1_dec
 0xf8003f00, // instruction mask
 0x20003300, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "p1=tstbit(Rs16,#0); if (p1.new) jump:nt #r9:2",// instruction: J4_tstbit0_tp1_jump_nt
 0xffc02300, // instruction mask
 0x13800300, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "memb(Ru32<<#0+#U6)=Nt8.new",// instruction: S4_storerbnew_ur_expand_shamt_0
 0xffe038c0, // instruction mask
 0xada00080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; Rd16=memuh(Rs16+#u3:1)",// instruction: X2_AUTOJOIN_SA1_addi_SL2_loadruh_io
 0xf8003800, // instruction mask
 0x40002800, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "memb(Ru32<<#3+#U6)=Nt8.new",// instruction: S4_storerbnew_ur_expand_shamt_3
 0xffe038c0, // instruction mask
 0xada020c0, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; memd(Sp+#s6:3)=Rtt8",// instruction: X2_AUTOJOIN_SA1_addi_SS2_stored_sp
 0xf8003e00, // instruction mask
 0x60002a00, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "Rd32=add(#u6,mpyi(Rs32,Rt32))",// instruction: M4_mpyrr_addi
 0xff800000, // instruction mask
 0xd7000000, // compare mask
 0x6020e0, // bitmask
 0x0 // isDuplex
},
{ "Rd16=#u6 ; Rx16=add(Rx16,Rs16)",// instruction: X2_AUTOJOIN_SA1_seti_SA1_addrx
 0xfc003f00, // instruction mask
 0x28003800, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "p1=cmp.gt(Rs16,#-1); if (!p1.new) jump:nt #r9:2",// instruction: J4_cmpgtn1_fp1_jump_nt
 0xffc02300, // instruction mask
 0x13c00100, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memuh(Re32=#U6)",// instruction: L4_loadruh_ap
 0xffe03000, // instruction mask
 0x9b601000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4.new) memw(Rs32+#u6:2)=Rt32",// instruction: S4_pstoreritnew_io
 0xffe00004, // instruction mask
 0x42800000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "Rd16=#U6 ; memb(Rs16+#u4:0)=Rt16",// instruction: X2_AUTOJOIN_SA1_seti_SS1_storeb_io
 0xfc003000, // instruction mask
 0x68001000, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Rd16=#u6 ; if (p0) dealloc_return",// instruction: X2_AUTOJOIN_SA1_seti_SL2_return_t
 0xfc003fc7, // instruction mask
 0x48003f44, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; memw(Rs16+#u4:2)=Rt16",// instruction: X2_AUTOJOIN_SA1_addi_SS1_storew_io
 0xf8003000, // instruction mask
 0x60000000, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "p0=tstbit(Rs16,#0); if (p0.new) jump:t #r9:2",// instruction: J4_tstbit0_tp0_jump_t
 0xffc02300, // instruction mask
 0x11802300, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rd16=#u6 ; if (!p0) dealloc_return",// instruction: X2_AUTOJOIN_SA1_seti_SL2_return_f
 0xfc003fc7, // instruction mask
 0x48003f45, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; if (!p0.new) Rd16=#0",// instruction: X2_AUTOJOIN_SA1_addi_SA1_clrfnew
 0xf8003e70, // instruction mask
 0x20003a50, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "Rx16=add(Rx16,#s7) ; jumpr Lr",// instruction: X2_AUTOJOIN_SA1_addi_SL2_jumpr31
 0xf8003fc4, // instruction mask
 0x40003fc0, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (cmp.eq(Ns8.new,Rt32)) jump:nt #r9:2",// instruction: J4_cmpeq_t_jumpnv_nt
 0xffc02000, // instruction mask
 0x20000000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rx32|=and(Rs32,#s10)",// instruction: S4_or_andi
 0xffc00000, // instruction mask
 0xda000000, // compare mask
 0x203fe0, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; Rd16=and(Rs16,#1)",// instruction: X2_AUTOJOIN_SA1_addi_SA1_and1
 0xf8003f00, // instruction mask
 0x20003200, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (Rs32<=#0) jump:nt #r13:2",// instruction: J2_jumprltez
 0xffc01000, // instruction mask
 0x61c00000, // compare mask
 0x202ffe, // bitmask
 0x0 // isDuplex
},
{ "Rd16=#U6 ; memw(Rs16+#u4:2)=#0",// instruction: X2_AUTOJOIN_SA1_seti_SS2_storewi0
 0xfc003f00, // instruction mask
 0x68003000, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Rd16=#U6 ; memw(Rs16+#u4:2)=#1",// instruction: X2_AUTOJOIN_SA1_seti_SS2_storewi1
 0xfc003f00, // instruction mask
 0x68003100, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Rdd32=memubh(Rt32<<#1+#U6)",// instruction: L4_loadbzw4_ur_expand_shamt_1
 0xffe03080, // instruction mask
 0x9ca01080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "if (cmp.gt(Ns8.new,#U5)) jump:t #r9:2",// instruction: J4_cmpgti_t_jumpnv_t
 0xffc02000, // instruction mask
 0x24802000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Re16=#U6 ; Rd16=add(Sp,#u6:2)",// instruction: X2_AUTOJOIN_SA1_seti_SA1_addsp
 0xfc003c00, // instruction mask
 0x28002c00, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Ryy32=memb_fifo(Rt32<<#3+#U6)",// instruction: L4_loadalignb_ur_expand_shamt_3
 0xffe03080, // instruction mask
 0x9c803080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.gt(Rs16,Rt16); if (!p1.new) jump:t #r9:2",// instruction: J4_cmpgt_fp1_jump_t
 0xffc03000, // instruction mask
 0x14c03000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rd32=add(Rs32,sub(#s6,Ru32))",// instruction: S4_subaddi
 0xff800000, // instruction mask
 0xdb800000, // compare mask
 0x6020e0, // bitmask
 0x0 // isDuplex
},
{ "Rd16=#U6 ; memd(Sp+#s6:3)=Rtt8",// instruction: X2_AUTOJOIN_SA1_seti_SS2_stored_sp
 0xfc003e00, // instruction mask
 0x68002a00, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "p3=sp1loop0(#r7:2,Rs32)",// instruction: J2_ploop1sr
 0xffe00000, // instruction mask
 0x60a00000, // compare mask
 0x1f18, // bitmask
 0x0 // isDuplex
},
{ "memw(Rs32+#u6:2)&=Rt32",// instruction: L4_and_memopw_io
 0xff602060, // instruction mask
 0x3e400040, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#s7) ; if (p0) jumpr Lr",// instruction: X2_AUTOJOIN_SA1_addi_SL2_jumpr31_t
 0xf8003fc7, // instruction mask
 0x40003fc4, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (cmp.gtu(Ns8.new,Rt32)) jump:nt #r9:2",// instruction: J4_cmpgtu_t_jumpnv_nt
 0xffc02000, // instruction mask
 0x21000000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4) memd(Rs32+#u6:3)=Rtt32",// instruction: S2_pstorerdt_io
 0xffe00004, // instruction mask
 0x40c00000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; Rdd8=combine(Rs16,#0)",// instruction: X2_AUTOJOIN_SA1_addi_SA1_combinerz
 0xf8003d08, // instruction mask
 0x20003d08, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "memb(Ru32<<#2+#U6)=Nt8.new",// instruction: S4_storerbnew_ur_expand_shamt_2
 0xffe038c0, // instruction mask
 0xada02080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "Rd32=and(Rs32,#s10)",// instruction: A2_andir
 0xffc00000, // instruction mask
 0x76000000, // compare mask
 0x203fe0, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.eq(Rs16,Rt16); if (p0.new) jump:nt #r9:2",// instruction: J4_cmpeq_tp0_jump_nt
 0xffc03000, // instruction mask
 0x14000000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (Pu4.new) jump:nt #r15:2",// instruction: J2_jumptnew
 0xff201800, // instruction mask
 0x5c000800, // compare mask
 0xdf20fe, // bitmask
 0x0 // isDuplex
},
{ "Rd16=#U6 ; memw(Sp+#u5:2)=Rt16",// instruction: X2_AUTOJOIN_SA1_seti_SS2_storew_sp
 0xfc003e00, // instruction mask
 0x68002800, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Rd32=cmp.eq(Rs32,#s8)",// instruction: A4_rcmpeqi
 0xff602000, // instruction mask
 0x73402000, // compare mask
 0x1fe0, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4) memh(Rs32+#u6:1)=#S6",// instruction: S4_storeirht_io
 0xffe00000, // instruction mask
 0x38200000, // compare mask
 0x201f, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=combine(#s8,#U6)",// instruction: A4_combineii
 0xff800000, // instruction mask
 0x7c800000, // compare mask
 0x1f2000, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=combine(#s8,Rs32)",// instruction: A4_combineir
 0xff602000, // instruction mask
 0x73202000, // compare mask
 0x1fe0, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.eq(Rs16,#-1); if (!p1.new) jump:nt #r9:2",// instruction: J4_cmpeqn1_fp1_jump_nt
 0xffc02300, // instruction mask
 0x13c00000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (cmp.eq(Ns8.new,#U5)) jump:t #r9:2",// instruction: J4_cmpeqi_t_jumpnv_t
 0xffc02000, // instruction mask
 0x24002000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; memh(Rs16+#u3:1)=Rt16",// instruction: X2_AUTOJOIN_SA1_addi_SS2_storeh_io
 0xf8003800, // instruction mask
 0x60002000, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (Pv4.new) memw(Rs32+#u6:2)=#S6",// instruction: S4_storeiritnew_io
 0xffe00000, // instruction mask
 0x39400000, // compare mask
 0x201f, // bitmask
 0x0 // isDuplex
},
{ "memh(Ru32<<#0+#U6)=Rt32",// instruction: S4_storerh_ur_expand_shamt_0
 0xffe020c0, // instruction mask
 0xad400080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "Rx32=or(#u8,asl(Rx32,#U5))",// instruction: S4_ori_asl_ri
 0xff000016, // instruction mask
 0xde000002, // compare mask
 0xe020e8, // bitmask
 0x0 // isDuplex
},
{ "if (cmp.gtu(Rt32,Ns8.new)) jump:t #r9:2",// instruction: J4_cmpltu_t_jumpnv_t
 0xffc02000, // instruction mask
 0x22002000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (!Pu4.new) jump:nt #r15:2",// instruction: J2_jumpfnew
 0xff201800, // instruction mask
 0x5c200800, // compare mask
 0xdf20fe, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4) memw(Rs32+#u6:2)=Nt8.new",// instruction: S2_pstorerinewt_io
 0xffe01804, // instruction mask
 0x40a01000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.eq(Rs16,#-1); if (p1.new) jump:nt #r9:2",// instruction: J4_cmpeqn1_tp1_jump_nt
 0xffc02300, // instruction mask
 0x13800000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.gtu(Rs16,Rt16); if (!p1.new) jump:nt #r9:2",// instruction: J4_cmpgtu_fp1_jump_nt
 0xffc03000, // instruction mask
 0x15401000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4) memh(Rs32+#u6:1)=Nt8.new",// instruction: S2_pstorerhnewf_io
 0xffe01804, // instruction mask
 0x44a00800, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "Rx32=or(Ru32,and(Rx32,#s10))",// instruction: S4_or_andix
 0xffc00000, // instruction mask
 0xda400000, // compare mask
 0x203fe0, // bitmask
 0x0 // isDuplex
},
{ "p0=tstbit(Rs16,#0); if (!p0.new) jump:nt #r9:2",// instruction: J4_tstbit0_fp0_jump_nt
 0xffc02300, // instruction mask
 0x11c00300, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "p0=tstbit(Rs16,#0); if (p0.new) jump:nt #r9:2",// instruction: J4_tstbit0_tp0_jump_nt
 0xffc02300, // instruction mask
 0x11800300, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (!Pt4) Rd32=memw(#u6)",// instruction: L4_ploadrif_abs
 0xffe03880, // instruction mask
 0x9f802880, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; Rd16=add(Sp,#u6:2)",// instruction: X2_AUTOJOIN_SA1_addi_SA1_addsp
 0xf8003c00, // instruction mask
 0x20002c00, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "Ryy32=memb_fifo(Rt32<<#2+#U6)",// instruction: L4_loadalignb_ur_expand_shamt_2
 0xffe03080, // instruction mask
 0x9c803000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.eq(Rs16,#U5); if (!p0.new) jump:nt #r9:2",// instruction: J4_cmpeqi_fp0_jump_nt
 0xffc02000, // instruction mask
 0x10400000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Pd4=!cmp.gtu(Rs32,#u9)",// instruction: C4_cmplteui
 0xffe0001c, // instruction mask
 0x75800010, // compare mask
 0x3fe0, // bitmask
 0x0 // isDuplex
},
{ "Rx32=add(#u8,lsr(Rx32,#U5))",// instruction: S4_addi_lsr_ri
 0xff000016, // instruction mask
 0xde000014, // compare mask
 0xe020e8, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.eq(Rs16,#-1); if (!p0.new) jump:t #r9:2",// instruction: J4_cmpeqn1_fp0_jump_t
 0xffc02300, // instruction mask
 0x11c02000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Pd4=cmph.gt(Rs32,#s8)",// instruction: A4_cmphgti
 0xff600018, // instruction mask
 0xdd200008, // compare mask
 0x1fe0, // bitmask
 0x0 // isDuplex
},
{ "Rx32=sub(#u8,asl(Rx32,#U5))",// instruction: S4_subi_asl_ri
 0xff000016, // instruction mask
 0xde000006, // compare mask
 0xe020e8, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4) memh(Rs32+#u6:1)=Rt.H32",// instruction: S2_pstorerff_io
 0xffe00004, // instruction mask
 0x44600000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "if (!Pt4) Rdd32=memd(#u6)",// instruction: L4_ploadrdf_abs
 0xffe03880, // instruction mask
 0x9fc02880, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "Re16=#U6 ; Rdd8=combine(Rs16,#0)",// instruction: X2_AUTOJOIN_SA1_seti_SA1_combinerz
 0xfc003d08, // instruction mask
 0x28003d08, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "memh(Ru32<<#3+#U6)=Nt8.new",// instruction: S4_storerhnew_ur_expand_shamt_3
 0xffe038c0, // instruction mask
 0xada028c0, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memh(Ru32<<#1+#U6)=Nt8.new",// instruction: S4_storerhnew_ur_expand_shamt_1
 0xffe038c0, // instruction mask
 0xada008c0, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "Rd16=#u6 ; dealloc_return",// instruction: X2_AUTOJOIN_SA1_seti_SL2_return
 0xfc003fc4, // instruction mask
 0x48003f40, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "memb(gp+#u16:0)=Nt8.new",// instruction: S2_storerbnewgp
 0xf9e01800, // instruction mask
 0x48a00000, // compare mask
 0x61f20ff, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.gt(Rs16,Rt16); if (p0.new) jump:t #r9:2",// instruction: J4_cmpgt_tp0_jump_t
 0xffc03000, // instruction mask
 0x14802000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rd16=#U6 ; p0=cmp.eq(Rs16,#u2)",// instruction: X2_AUTOJOIN_SA1_seti_SA1_cmpeqi
 0xfc003f00, // instruction mask
 0x28003900, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Rd16=#U6 ; allocframe(#u5:3)",// instruction: X2_AUTOJOIN_SA1_seti_SS2_allocframe
 0xfc003e00, // instruction mask
 0x68003c00, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; if (p0) Rd16=#0",// instruction: X2_AUTOJOIN_SA1_addi_SA1_clrt
 0xf8003e70, // instruction mask
 0x20003a60, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "Rd32=memubh(Rt32<<#1+#U6)",// instruction: L4_loadbzw2_ur_expand_shamt_1
 0xffe03080, // instruction mask
 0x9c601080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memubh(Rt32<<#0+#U6)",// instruction: L4_loadbzw2_ur_expand_shamt_0
 0xffe03080, // instruction mask
 0x9c601000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memubh(Rt32<<#3+#U6)",// instruction: L4_loadbzw2_ur_expand_shamt_3
 0xffe03080, // instruction mask
 0x9c603080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memubh(Rt32<<#2+#U6)",// instruction: L4_loadbzw2_ur_expand_shamt_2
 0xffe03080, // instruction mask
 0x9c603000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#s7) ; Rd16=Rs16",// instruction: X2_AUTOJOIN_SA1_addi_SA1_tfr
 0xf8003f00, // instruction mask
 0x20003000, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (!Pt4.new) Rd32=memw(#u6)",// instruction: L4_ploadrifnew_abs
 0xffe03880, // instruction mask
 0x9f803880, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; Rd16=add(Rs16,#1)",// instruction: X2_AUTOJOIN_SA1_addi_SA1_inc
 0xf8003f00, // instruction mask
 0x20003100, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (!Pt4.new) Rd32=memub(Rs32+#u6:0)",// instruction: L2_ploadrubfnew_io
 0xffe02000, // instruction mask
 0x47200000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4) memd(#u6)=Rtt32",// instruction: S4_pstorerdt_abs
 0xffe02084, // instruction mask
 0xafc00080, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "Re16=#u6 ; Rd16=zxth(Rs16)",// instruction: X2_AUTOJOIN_SA1_seti_SA1_zxth
 0xfc003f00, // instruction mask
 0x28003600, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Ry16=add(Ry16,#s7) ; Rx16=add(Rx16,Rs16)",// instruction: X2_AUTOJOIN_SA1_addi_SA1_addrx
 0xf8003f00, // instruction mask
 0x20003800, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "memh(Rs32+#s11:1)=Rt.H32",// instruction: S2_storerf_io
 0xf9e00000, // instruction mask
 0xa1600000, // compare mask
 0x60020ff, // bitmask
 0x0 // isDuplex
},
{ "Re16=#U6 ; if (p0) Rd16=#0",// instruction: X2_AUTOJOIN_SA1_seti_SA1_clrt
 0xfc003e70, // instruction mask
 0x28003a60, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "memb(Rs32+#s11:0)=Rt32",// instruction: S2_storerb_io
 0xf9e00000, // instruction mask
 0xa1000000, // compare mask
 0x60020ff, // bitmask
 0x0 // isDuplex
},
{ "Re16=#U6 ; if (!p0) Rd16=#0",// instruction: X2_AUTOJOIN_SA1_seti_SA1_clrf
 0xfc003e70, // instruction mask
 0x28003a70, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Rx32=and(#u8,lsr(Rx32,#U5))",// instruction: S4_andi_lsr_ri
 0xff000016, // instruction mask
 0xde000010, // compare mask
 0xe020e8, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4.new) Rd32=memuh(#u6)",// instruction: L4_ploadruhtnew_abs
 0xffe03880, // instruction mask
 0x9f603080, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4.new) memb(#u6)=Nt8.new",// instruction: S4_pstorerbnewtnew_abs
 0xffe03884, // instruction mask
 0xafa02080, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; Rd16=memb(Rs16+#u3:0)",// instruction: X2_AUTOJOIN_SA1_addi_SL2_loadrb_io
 0xf8003800, // instruction mask
 0x40003000, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (Rs32!=#0) jump:nt #r13:2",// instruction: J2_jumprz
 0xffc01000, // instruction mask
 0x61000000, // compare mask
 0x202ffe, // bitmask
 0x0 // isDuplex
},
{ "Rx32-=mpyi(Rs32,#u8)",// instruction: M2_macsin
 0xff802000, // instruction mask
 0xe1800000, // compare mask
 0x1fe0, // bitmask
 0x0 // isDuplex
},
{ "if (cmp.gt(Ns8.new,#U5)) jump:nt #r9:2",// instruction: J4_cmpgti_t_jumpnv_nt
 0xffc02000, // instruction mask
 0x24800000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.gt(Rs16,#-1); if (p0.new) jump:t #r9:2",// instruction: J4_cmpgtn1_tp0_jump_t
 0xffc02300, // instruction mask
 0x11802100, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=memd(Rs32+#s11:3)",// instruction: L2_loadrd_io
 0xf9e00000, // instruction mask
 0x91c00000, // compare mask
 0x6003fe0, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4.new) memh(#u6)=Nt8.new",// instruction: S4_pstorerhnewtnew_abs
 0xffe03884, // instruction mask
 0xafa02880, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.gtu(Rs16,Rt16); if (p0.new) jump:nt #r9:2",// instruction: J4_cmpgtu_tp0_jump_nt
 0xffc03000, // instruction mask
 0x15000000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rx32+=add(Rs32,#s8)",// instruction: M2_accii
 0xff802000, // instruction mask
 0xe2000000, // compare mask
 0x1fe0, // bitmask
 0x0 // isDuplex
},
{ "if (!cmp.gt(Ns8.new,#-1)) jump:nt #r9:2",// instruction: J4_cmpgtn1_f_jumpnv_nt
 0xffc02000, // instruction mask
 0x26c00000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "memw(Rs32+#u6:2)+=Rt32",// instruction: L4_add_memopw_io
 0xff602060, // instruction mask
 0x3e400000, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4) memw(Rs32+#u6:2)=#S6",// instruction: S4_storeirit_io
 0xffe00000, // instruction mask
 0x38400000, // compare mask
 0x201f, // bitmask
 0x0 // isDuplex
},
{ "Rx32=and(#u8,asl(Rx32,#U5))",// instruction: S4_andi_asl_ri
 0xff000016, // instruction mask
 0xde000000, // compare mask
 0xe020e8, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.eq(Rs16,#-1); if (p1.new) jump:t #r9:2",// instruction: J4_cmpeqn1_tp1_jump_t
 0xffc02300, // instruction mask
 0x13802000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rd16=Rs16 ; jump #r9:2",// instruction: J4_jumpsetr
 0xff000000, // instruction mask
 0x17000000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rd16=#U6 ; jump #r9:2",// instruction: J4_jumpseti
 0xff000000, // instruction mask
 0x16000000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rd16=#u6 ; if (!p0.new) jumpr:nt Lr",// instruction: X2_AUTOJOIN_SA1_seti_SL2_jumpr31_fnew
 0xfc003fc7, // instruction mask
 0x48003fc7, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "if (Pt4) Rd32=memw(#u6)",// instruction: L4_ploadrit_abs
 0xffe03880, // instruction mask
 0x9f802080, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4) memw(#u6)=Nt8.new",// instruction: S4_pstorerinewf_abs
 0xffe03884, // instruction mask
 0xafa01084, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "Rd32=add(Rs32,#s16)",// instruction: A2_addi
 0xf0000000, // instruction mask
 0xb0000000, // compare mask
 0xfe03fe0, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4.new) memb(Rs32+#u6:0)=#S6",// instruction: S4_storeirbfnew_io
 0xffe00000, // instruction mask
 0x39800000, // compare mask
 0x201f, // bitmask
 0x0 // isDuplex
},
{ "if (!Pt4) Rd32=memuh(Rs32+#u6:1)",// instruction: L2_ploadruhf_io
 0xffe02000, // instruction mask
 0x45600000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "Rd16=#u6 ; Rx16=add(Rs16,Rx16)",// instruction: X2_AUTOJOIN_SA1_seti_SA1_addrx_commuted
 0xfc003f00, // instruction mask
 0x28003800, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "if (Pt4) Rd32=memuh(#u6)",// instruction: L4_ploadruht_abs
 0xffe03880, // instruction mask
 0x9f602080, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4.new) memb(#u6)=Rt32",// instruction: S4_pstorerbfnew_abs
 0xffe02084, // instruction mask
 0xaf002084, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "call #r22:2",// instruction: J2_call
 0xfe000001, // instruction mask
 0x5a000000, // compare mask
 0x1ff3ffe, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; if (!p0) Rd16=#0",// instruction: X2_AUTOJOIN_SA1_addi_SA1_clrf
 0xf8003e70, // instruction mask
 0x20003a70, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "memw(Rs32+#u6:2)-=#U5",// instruction: L4_isub_memopw_io
 0xff602060, // instruction mask
 0x3f400020, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.gt(Rs16,#U5); if (p0.new) jump:t #r9:2",// instruction: J4_cmpgti_tp0_jump_t
 0xffc02000, // instruction mask
 0x10802000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=membh(Rt32<<#3+#U6)",// instruction: L4_loadbsw4_ur_expand_shamt_3
 0xffe03080, // instruction mask
 0x9ce03080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=membh(Rt32<<#1+#U6)",// instruction: L4_loadbsw4_ur_expand_shamt_1
 0xffe03080, // instruction mask
 0x9ce01080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; memw(Sp+#u5:2)=Rt16",// instruction: X2_AUTOJOIN_SA1_addi_SS2_storew_sp
 0xf8003e00, // instruction mask
 0x60002800, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (!Pv4.new) memh(Rs32+#u6:1)=Rt32",// instruction: S4_pstorerhfnew_io
 0xffe00004, // instruction mask
 0x46400000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#s7) ; if (!p0) dealloc_return",// instruction: X2_AUTOJOIN_SA1_addi_SL2_return_f
 0xf8003fc7, // instruction mask
 0x40003f45, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (!Pv4) memh(#u6)=Rt.H32",// instruction: S4_pstorerff_abs
 0xffe02084, // instruction mask
 0xaf600084, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "Rd32=membh(Rt32<<#2+#U6)",// instruction: L4_loadbsw2_ur_expand_shamt_2
 0xffe03080, // instruction mask
 0x9c203000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rd32=membh(Rt32<<#3+#U6)",// instruction: L4_loadbsw2_ur_expand_shamt_3
 0xffe03080, // instruction mask
 0x9c203080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "p0=tstbit(Rs16,#0); if (!p0.new) jump:t #r9:2",// instruction: J4_tstbit0_fp0_jump_t
 0xffc02300, // instruction mask
 0x11c02300, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4.new) Rdd32=memd(#u6)",// instruction: L4_ploadrdtnew_abs
 0xffe03880, // instruction mask
 0x9fc03080, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "if (!Pt4) Rd32=memh(#u6)",// instruction: L4_ploadrhf_abs
 0xffe03880, // instruction mask
 0x9f402880, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "if (!cmp.gt(Ns8.new,Rt32)) jump:t #r9:2",// instruction: J4_cmpgt_f_jumpnv_t
 0xffc02000, // instruction mask
 0x20c02000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.eq(Rs16,#-1); if (!p0.new) jump:nt #r9:2",// instruction: J4_cmpeqn1_fp0_jump_nt
 0xffc02300, // instruction mask
 0x11c00000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (!Pt4) Rd32=memub(Rs32+#u6:0)",// instruction: L2_ploadrubf_io
 0xffe02000, // instruction mask
 0x45200000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.gt(Rs16,#-1); if (!p0.new) jump:nt #r9:2",// instruction: J4_cmpgtn1_fp0_jump_nt
 0xffc02300, // instruction mask
 0x11c00100, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4) memw(#u6)=Rt32",// instruction: S4_pstorerif_abs
 0xffe02084, // instruction mask
 0xaf800084, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "if (cmp.gt(Ns8.new,#-1)) jump:nt #r9:2",// instruction: J4_cmpgtn1_t_jumpnv_nt
 0xffc02000, // instruction mask
 0x26800000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "memb(Rs32+#u6:0)&=Rt32",// instruction: L4_and_memopb_io
 0xff602060, // instruction mask
 0x3e000040, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4) memh(Rs32+#u6:1)=Rt32",// instruction: S2_pstorerhf_io
 0xffe00004, // instruction mask
 0x44400000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=combine(#s8,#S8)",// instruction: A2_combineii
 0xff800000, // instruction mask
 0x7c000000, // compare mask
 0x1fe0, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4) memb(#u6)=Nt8.new",// instruction: S4_pstorerbnewt_abs
 0xffe03884, // instruction mask
 0xafa00080, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "Ryy32=memh_fifo(Rt32<<#0+#U6)",// instruction: L4_loadalignh_ur_expand_shamt_0
 0xffe03080, // instruction mask
 0x9c401000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Pd4=cmpb.gtu(Rs32,#u7)",// instruction: A4_cmpbgtui
 0xff601018, // instruction mask
 0xdd400000, // compare mask
 0xfe0, // bitmask
 0x0 // isDuplex
},
{ "Re16=#U6 ; Rd16=#-1",// instruction: X2_AUTOJOIN_SA1_seti_SA1_setin1
 0xfc003e40, // instruction mask
 0x28003a00, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "memw(Ru32<<#1+#U6)=Nt8.new",// instruction: S4_storerinew_ur_expand_shamt_1
 0xffe038c0, // instruction mask
 0xada010c0, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memw(Ru32<<#0+#U6)=Nt8.new",// instruction: S4_storerinew_ur_expand_shamt_0
 0xffe038c0, // instruction mask
 0xada01080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memw(Ru32<<#3+#U6)=Nt8.new",// instruction: S4_storerinew_ur_expand_shamt_3
 0xffe038c0, // instruction mask
 0xada030c0, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memw(Ru32<<#2+#U6)=Nt8.new",// instruction: S4_storerinew_ur_expand_shamt_2
 0xffe038c0, // instruction mask
 0xada03080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "if (cmp.gt(Ns8.new,Rt32)) jump:nt #r9:2",// instruction: J4_cmpgt_t_jumpnv_nt
 0xffc02000, // instruction mask
 0x20800000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4) memb(Rs32+#u6:0)=Nt8.new",// instruction: S2_pstorerbnewf_io
 0xffe01804, // instruction mask
 0x44a00000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "if (cmp.eq(Ns8.new,Rt32)) jump:t #r9:2",// instruction: J4_cmpeq_t_jumpnv_t
 0xffc02000, // instruction mask
 0x20002000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.gt(Rs16,#U5); if (!p1.new) jump:t #r9:2",// instruction: J4_cmpgti_fp1_jump_t
 0xffc02000, // instruction mask
 0x12c02000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4.new) memh(#u6)=Nt8.new",// instruction: S4_pstorerhnewfnew_abs
 0xffe03884, // instruction mask
 0xafa02884, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memubh(Re32=#U6)",// instruction: L4_loadbzw2_ap
 0xffe03000, // instruction mask
 0x9a601000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rx32|=or(Rs32,#s10)",// instruction: S4_or_ori
 0xffc00000, // instruction mask
 0xda800000, // compare mask
 0x203fe0, // bitmask
 0x0 // isDuplex
},
{ "if (!Pt4.new) Rd32=memuh(Rs32+#u6:1)",// instruction: L2_ploadruhfnew_io
 0xffe02000, // instruction mask
 0x47600000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "Rd32=add(#u6,mpyi(Rs32,#U6))",// instruction: M4_mpyri_addi
 0xff000000, // instruction mask
 0xd8000000, // compare mask
 0x6020e0, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4) Rd32=memh(Rs32+#u6:1)",// instruction: L2_ploadrht_io
 0xffe02000, // instruction mask
 0x41400000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "Rd32=add(Ru32,mpyi(Rs32,#u6))",// instruction: M4_mpyri_addr
 0xff800000, // instruction mask
 0xdf800000, // compare mask
 0x6020e0, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4) memw(Rs32+#u6:2)=Rt32",// instruction: S2_pstorerit_io
 0xffe00004, // instruction mask
 0x40800000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4) Rd32=memub(#u6)",// instruction: L4_ploadrubt_abs
 0xffe03880, // instruction mask
 0x9f202080, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "Rd16=#u6 ; if (p0.new) jumpr:nt Lr",// instruction: X2_AUTOJOIN_SA1_seti_SL2_jumpr31_tnew
 0xfc003fc7, // instruction mask
 0x48003fc6, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; Rd16=memw(Rs16+#u4:2)",// instruction: X2_AUTOJOIN_SA1_addi_SL1_loadri_io
 0xf8003000, // instruction mask
 0x40000000, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "Rd32=add(pc,#u6)",// instruction: C4_addipc
 0xffff0000, // instruction mask
 0x6a490000, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "if (cmp.eq(Ns8.new,#U5)) jump:nt #r9:2",// instruction: J4_cmpeqi_t_jumpnv_nt
 0xffc02000, // instruction mask
 0x24000000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.eq(Rs16,#U5); if (p1.new) jump:t #r9:2",// instruction: J4_cmpeqi_tp1_jump_t
 0xffc02000, // instruction mask
 0x12002000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rd32=membh(Rt32<<#0+#U6)",// instruction: L4_loadbsw2_ur_expand_shamt_0
 0xffe03080, // instruction mask
 0x9c201000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rd32=membh(Rt32<<#1+#U6)",// instruction: L4_loadbsw2_ur_expand_shamt_1
 0xffe03080, // instruction mask
 0x9c201080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "if (!cmp.gtu(Ns8.new,Rt32)) jump:nt #r9:2",// instruction: J4_cmpgtu_f_jumpnv_nt
 0xffc02000, // instruction mask
 0x21400000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (Rs32<=#0) jump:t #r13:2",// instruction: J2_jumprltezpt
 0xffc01000, // instruction mask
 0x61c01000, // compare mask
 0x202ffe, // bitmask
 0x0 // isDuplex
},
{ "if (!Pu4) call #r15:2",// instruction: J2_callf
 0xff200800, // instruction mask
 0x5d200000, // compare mask
 0xdf20fe, // bitmask
 0x0 // isDuplex
},
{ "if (Pu4) call #r15:2",// instruction: J2_callt
 0xff200800, // instruction mask
 0x5d000000, // compare mask
 0xdf20fe, // bitmask
 0x0 // isDuplex
},
{ "Rd16=#u6 ; jumpr Lr",// instruction: X2_AUTOJOIN_SA1_seti_SL2_jumpr31
 0xfc003fc4, // instruction mask
 0x48003fc0, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "if (!Pt4) Rdd32=memd(Rs32+#u6:3)",// instruction: L2_ploadrdf_io
 0xffe02000, // instruction mask
 0x45c00000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "if (tstbit(Ns8.new,#0)) jump:t #r9:2",// instruction: J4_tstbit0_t_jumpnv_t
 0xffc02000, // instruction mask
 0x25802000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memub(Re32=#U6)",// instruction: L4_loadrub_ap
 0xffe03000, // instruction mask
 0x9b201000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "p1=tstbit(Rs16,#0); if (!p1.new) jump:nt #r9:2",// instruction: J4_tstbit0_fp1_jump_nt
 0xffc02300, // instruction mask
 0x13c00300, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rx32=or(#u8,lsr(Rx32,#U5))",// instruction: S4_ori_lsr_ri
 0xff000016, // instruction mask
 0xde000012, // compare mask
 0xe020e8, // bitmask
 0x0 // isDuplex
},
{ "Rx32-=add(Rs32,#s8)",// instruction: M2_naccii
 0xff802000, // instruction mask
 0xe2800000, // compare mask
 0x1fe0, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memw(Rt32<<#3+#U6)",// instruction: L4_loadri_ur_expand_shamt_3
 0xffe03080, // instruction mask
 0x9d803080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memw(Rt32<<#2+#U6)",// instruction: L4_loadri_ur_expand_shamt_2
 0xffe03080, // instruction mask
 0x9d803000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memw(Rt32<<#1+#U6)",// instruction: L4_loadri_ur_expand_shamt_1
 0xffe03080, // instruction mask
 0x9d801080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.eq(Rs16,Rt16); if (!p0.new) jump:t #r9:2",// instruction: J4_cmpeq_fp0_jump_t
 0xffc03000, // instruction mask
 0x14402000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memub(Rt32<<#3+#U6)",// instruction: L4_loadrub_ur_expand_shamt_3
 0xffe03080, // instruction mask
 0x9d203080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "memb(Re32=#U6)=Rt32",// instruction: S4_storerb_ap
 0xffe02080, // instruction mask
 0xab000080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; allocframe(#u5:3)",// instruction: X2_AUTOJOIN_SA1_addi_SS2_allocframe
 0xf8003e00, // instruction mask
 0x60003c00, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "Rd32=or(Rs32,#s10)",// instruction: A2_orir
 0xffc00000, // instruction mask
 0x76800000, // compare mask
 0x203fe0, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memb(Rs32+#s11:0)",// instruction: L2_loadrb_io
 0xf9e00000, // instruction mask
 0x91000000, // compare mask
 0x6003fe0, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.eq(Rs16,Rt16); if (!p0.new) jump:nt #r9:2",// instruction: J4_cmpeq_fp0_jump_nt
 0xffc03000, // instruction mask
 0x14400000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memh(Re32=#U6)",// instruction: L4_loadrh_ap
 0xffe03000, // instruction mask
 0x9b401000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.eq(Rs16,Rt16); if (p1.new) jump:t #r9:2",// instruction: J4_cmpeq_tp1_jump_t
 0xffc03000, // instruction mask
 0x14003000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4) Rdd32=memd(#u6)",// instruction: L4_ploadrdt_abs
 0xffe03880, // instruction mask
 0x9fc02080, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4.new) memb(Rs32+#u6:0)=Nt8.new",// instruction: S4_pstorerbnewtnew_io
 0xffe01804, // instruction mask
 0x42a00000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "memb(Rs32+#s11:0)=Nt8.new",// instruction: S2_storerbnew_io
 0xf9e01800, // instruction mask
 0xa1a00000, // compare mask
 0x60020ff, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.gt(Rs16,Rt16); if (!p1.new) jump:nt #r9:2",// instruction: J4_cmpgt_fp1_jump_nt
 0xffc03000, // instruction mask
 0x14c01000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Pd4=!cmp.gt(Rs32,#s10)",// instruction: C4_cmpltei
 0xffc0001c, // instruction mask
 0x75400010, // compare mask
 0x203fe0, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.gtu(Rs16,Rt16); if (!p0.new) jump:t #r9:2",// instruction: J4_cmpgtu_fp0_jump_t
 0xffc03000, // instruction mask
 0x15402000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (!tstbit(Ns8.new,#0)) jump:t #r9:2",// instruction: J4_tstbit0_f_jumpnv_t
 0xffc02000, // instruction mask
 0x25c02000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (!cmp.gtu(Ns8.new,#U5)) jump:t #r9:2",// instruction: J4_cmpgtui_f_jumpnv_t
 0xffc02000, // instruction mask
 0x25402000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.gt(Rs16,#U5); if (!p0.new) jump:nt #r9:2",// instruction: J4_cmpgti_fp0_jump_nt
 0xffc02000, // instruction mask
 0x10c00000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "memb(Rs32+#u6:0)-=#U5",// instruction: L4_isub_memopb_io
 0xff602060, // instruction mask
 0x3f000020, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "if (!Pt4) Rd32=memw(Rs32+#u6:2)",// instruction: L2_ploadrif_io
 0xffe02000, // instruction mask
 0x45800000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4.new) Rd32=memw(#u6)",// instruction: L4_ploadritnew_abs
 0xffe03880, // instruction mask
 0x9f803080, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memb(Rt32<<#0+#U6)",// instruction: L4_loadrb_ur_expand_shamt_0
 0xffe03080, // instruction mask
 0x9d001000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memb(Rt32<<#1+#U6)",// instruction: L4_loadrb_ur_expand_shamt_1
 0xffe03080, // instruction mask
 0x9d001080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memb(Rt32<<#2+#U6)",// instruction: L4_loadrb_ur_expand_shamt_2
 0xffe03080, // instruction mask
 0x9d003000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memb(Rt32<<#3+#U6)",// instruction: L4_loadrb_ur_expand_shamt_3
 0xffe03080, // instruction mask
 0x9d003080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memw(Rt32<<#0+#U6)",// instruction: L4_loadri_ur_expand_shamt_0
 0xffe03080, // instruction mask
 0x9d801000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "if (cmp.gt(Rt32,Ns8.new)) jump:t #r9:2",// instruction: J4_cmplt_t_jumpnv_t
 0xffc02000, // instruction mask
 0x21802000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rd32=membh(Rs32+#s11:1)",// instruction: L2_loadbsw2_io
 0xf9e00000, // instruction mask
 0x90200000, // compare mask
 0x6003fe0, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4.new) memh(#u6)=Rt.H32",// instruction: S4_pstorerftnew_abs
 0xffe02084, // instruction mask
 0xaf602080, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "memb(Ru32<<#1+#U6)=Nt8.new",// instruction: S4_storerbnew_ur_expand_shamt_1
 0xffe038c0, // instruction mask
 0xada000c0, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memh(Re32=#U6)=Rt32",// instruction: S4_storerh_ap
 0xffe02080, // instruction mask
 0xab400080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4) memb(Rs32+#u6:0)=Rt32",// instruction: S2_pstorerbf_io
 0xffe00004, // instruction mask
 0x44000000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "memd(Ru32<<#1+#U6)=Rtt32",// instruction: S4_storerd_ur_expand_shamt_1
 0xffe020c0, // instruction mask
 0xadc000c0, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memd(Ru32<<#0+#U6)=Rtt32",// instruction: S4_storerd_ur_expand_shamt_0
 0xffe020c0, // instruction mask
 0xadc00080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memd(Ru32<<#3+#U6)=Rtt32",// instruction: S4_storerd_ur_expand_shamt_3
 0xffe020c0, // instruction mask
 0xadc020c0, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memd(Ru32<<#2+#U6)=Rtt32",// instruction: S4_storerd_ur_expand_shamt_2
 0xffe020c0, // instruction mask
 0xadc02080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "if (Pu4) jump:nt #r15:2",// instruction: J2_jumpt
 0xff201800, // instruction mask
 0x5c000000, // compare mask
 0xdf20fe, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4) memb(Rs32+#u6:0)=Nt8.new",// instruction: S2_pstorerbnewt_io
 0xffe01804, // instruction mask
 0x40a00000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "Ryy32=memh_fifo(Rt32<<#2+#U6)",// instruction: L4_loadalignh_ur_expand_shamt_2
 0xffe03080, // instruction mask
 0x9c403000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Re16=#U6 ; Rd16=#u6",// instruction: X2_AUTOJOIN_SA1_seti_SA1_seti
 0xfc003c00, // instruction mask
 0x28002800, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "if (!Pv4.new) memb(#u6)=Nt8.new",// instruction: S4_pstorerbnewfnew_abs
 0xffe03884, // instruction mask
 0xafa02084, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4) Rd32=memb(Rs32+#u6:0)",// instruction: L2_ploadrbt_io
 0xffe02000, // instruction mask
 0x41000000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4.new) memh(Rs32+#u6:1)=Rt.H32",// instruction: S4_pstorerftnew_io
 0xffe00004, // instruction mask
 0x42600000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "memw(Rs32+#u6:2)-=Rt32",// instruction: L4_sub_memopw_io
 0xff602060, // instruction mask
 0x3e400020, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "memb(Rs32+#u6:0)+=#U5",// instruction: L4_iadd_memopb_io
 0xff602060, // instruction mask
 0x3f000000, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "if (!cmp.gt(Ns8.new,#U5)) jump:t #r9:2",// instruction: J4_cmpgti_f_jumpnv_t
 0xffc02000, // instruction mask
 0x24c02000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Ry16=add(Ry16,#S7) ; Rx16=add(Rx16,#s7)",// instruction: X2_AUTOJOIN_SA1_addi_SA1_addi
 0xf8003800, // instruction mask
 0x20002000, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "Rdd32=memubh(Re32=#U6)",// instruction: L4_loadbzw4_ap
 0xffe03000, // instruction mask
 0x9aa01000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.eq(Rs16,#U5); if (!p1.new) jump:nt #r9:2",// instruction: J4_cmpeqi_fp1_jump_nt
 0xffc02000, // instruction mask
 0x12400000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4.new) Rd32=memh(#u6)",// instruction: L4_ploadrhtnew_abs
 0xffe03880, // instruction mask
 0x9f403080, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.gtu(Rs16,#U5); if (p0.new) jump:nt #r9:2",// instruction: J4_cmpgtui_tp0_jump_nt
 0xffc02000, // instruction mask
 0x11000000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4) memb(Rs32+#u6:0)=Rt32",// instruction: S2_pstorerbt_io
 0xffe00004, // instruction mask
 0x40000000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4.new) Rd32=memb(Rs32+#u6:0)",// instruction: L2_ploadrbtnew_io
 0xffe02000, // instruction mask
 0x43000000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=membh(Re32=#U6)",// instruction: L4_loadbsw4_ap
 0xffe03000, // instruction mask
 0x9ae01000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4) memb(#u6)=Rt32",// instruction: S4_pstorerbf_abs
 0xffe02084, // instruction mask
 0xaf000084, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "memh(Rs32+#u6:1)&=Rt32",// instruction: L4_and_memoph_io
 0xff602060, // instruction mask
 0x3e200040, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.gt(Rs16,#U5); if (!p0.new) jump:t #r9:2",// instruction: J4_cmpgti_fp0_jump_t
 0xffc02000, // instruction mask
 0x10c02000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "memh(Ru32<<#2+#U6)=Nt8.new",// instruction: S4_storerhnew_ur_expand_shamt_2
 0xffe038c0, // instruction mask
 0xada02880, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memh(Ru32<<#0+#U6)=Nt8.new",// instruction: S4_storerhnew_ur_expand_shamt_0
 0xffe038c0, // instruction mask
 0xada00880, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "if (!cmp.gt(Ns8.new,#-1)) jump:t #r9:2",// instruction: J4_cmpgtn1_f_jumpnv_t
 0xffc02000, // instruction mask
 0x26c02000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4) Rd32=memuh(Rs32+#u6:1)",// instruction: L2_ploadruht_io
 0xffe02000, // instruction mask
 0x41600000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.gt(Rs16,#-1); if (p1.new) jump:nt #r9:2",// instruction: J4_cmpgtn1_tp1_jump_nt
 0xffc02300, // instruction mask
 0x13800100, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4.new) memb(Rs32+#u6:0)=Nt8.new",// instruction: S4_pstorerbnewfnew_io
 0xffe01804, // instruction mask
 0x46a00000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#s7) ; Rd16=sxth(Rs16)",// instruction: X2_AUTOJOIN_SA1_addi_SA1_sxth
 0xf8003f00, // instruction mask
 0x20003400, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (Pv4.new) memh(Rs32+#u6:1)=Rt32",// instruction: S4_pstorerhtnew_io
 0xffe00004, // instruction mask
 0x42400000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "if (!cmp.gtu(Rt32,Ns8.new)) jump:nt #r9:2",// instruction: J4_cmpltu_f_jumpnv_nt
 0xffc02000, // instruction mask
 0x22400000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.eq(Rs16,#U5); if (!p1.new) jump:t #r9:2",// instruction: J4_cmpeqi_fp1_jump_t
 0xffc02000, // instruction mask
 0x12402000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4.new) memb(Rs32+#u6:0)=#S6",// instruction: S4_storeirbtnew_io
 0xffe00000, // instruction mask
 0x39000000, // compare mask
 0x201f, // bitmask
 0x0 // isDuplex
},
{ "memh(Rs32+#u6:1)+=#U5",// instruction: L4_iadd_memoph_io
 0xff602060, // instruction mask
 0x3f200000, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "Pd4=cmph.gtu(Rs32,#u7)",// instruction: A4_cmphgtui
 0xff601018, // instruction mask
 0xdd400008, // compare mask
 0xfe0, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=memubh(Rt32<<#2+#U6)",// instruction: L4_loadbzw4_ur_expand_shamt_2
 0xffe03080, // instruction mask
 0x9ca03000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=memubh(Rt32<<#0+#U6)",// instruction: L4_loadbzw4_ur_expand_shamt_0
 0xffe03080, // instruction mask
 0x9ca01000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4.new) memw(Rs32+#u6:2)=Rt32",// instruction: S4_pstorerifnew_io
 0xffe00004, // instruction mask
 0x46800000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#s7) ; if (!p0.new) jumpr:nt Lr",// instruction: X2_AUTOJOIN_SA1_addi_SL2_jumpr31_fnew
 0xf8003fc7, // instruction mask
 0x40003fc7, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "Rd32=memw(Rs32+#s11:2)",// instruction: L2_loadri_io
 0xf9e00000, // instruction mask
 0x91800000, // compare mask
 0x6003fe0, // bitmask
 0x0 // isDuplex
},
{ "Ry16=add(Ry16,#s7) ; Rx16=add(Rs16,Rx16)",// instruction: X2_AUTOJOIN_SA1_addi_SA1_addrx_commuted
 0xf8003f00, // instruction mask
 0x20003800, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "Rd32=!cmp.eq(Rs32,#s8)",// instruction: A4_rcmpneqi
 0xff602000, // instruction mask
 0x73602000, // compare mask
 0x1fe0, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4) Rd32=memb(#u6)",// instruction: L4_ploadrbt_abs
 0xffe03880, // instruction mask
 0x9f002080, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memh(Rs32+#s11:1)",// instruction: L2_loadrh_io
 0xf9e00000, // instruction mask
 0x91400000, // compare mask
 0x6003fe0, // bitmask
 0x0 // isDuplex
},
{ "memd(Re32=#U6)=Rtt32",// instruction: S4_storerd_ap
 0xffe02080, // instruction mask
 0xabc00080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4.new) memw(#u6)=Nt8.new",// instruction: S4_pstorerinewfnew_abs
 0xffe03884, // instruction mask
 0xafa03084, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memb(Re32=#U6)",// instruction: L4_loadrb_ap
 0xffe03000, // instruction mask
 0x9b001000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "if (!cmp.gt(Ns8.new,#U5)) jump:nt #r9:2",// instruction: J4_cmpgti_f_jumpnv_nt
 0xffc02000, // instruction mask
 0x24c00000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (!Pt4) Rd32=memb(Rs32+#u6:0)",// instruction: L2_ploadrbf_io
 0xffe02000, // instruction mask
 0x45000000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4.new) memh(Rs32+#u6:1)=Nt8.new",// instruction: S4_pstorerhnewfnew_io
 0xffe01804, // instruction mask
 0x46a00800, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "Re16=#u6 ; Rd16=sxtb(Rs16)",// instruction: X2_AUTOJOIN_SA1_seti_SA1_sxtb
 0xfc003f00, // instruction mask
 0x28003500, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Re16=#u6 ; Rd16=sxth(Rs16)",// instruction: X2_AUTOJOIN_SA1_seti_SA1_sxth
 0xfc003f00, // instruction mask
 0x28003400, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "if (!cmp.gt(Rt32,Ns8.new)) jump:nt #r9:2",// instruction: J4_cmplt_f_jumpnv_nt
 0xffc02000, // instruction mask
 0x21c00000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rd16=#U6 ; memw(Rs16+#u4:2)=Rt16",// instruction: X2_AUTOJOIN_SA1_seti_SS1_storew_io
 0xfc003000, // instruction mask
 0x68000000, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "if (!Pv4.new) memw(#u6)=Rt32",// instruction: S4_pstorerifnew_abs
 0xffe02084, // instruction mask
 0xaf802084, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.gt(Rs16,#-1); if (!p0.new) jump:t #r9:2",// instruction: J4_cmpgtn1_fp0_jump_t
 0xffc02300, // instruction mask
 0x11c02100, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.gt(Rs16,#U5); if (p1.new) jump:nt #r9:2",// instruction: J4_cmpgti_tp1_jump_nt
 0xffc02000, // instruction mask
 0x12800000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "memw(Rs32+#u6:2)+=#U5",// instruction: L4_iadd_memopw_io
 0xff602060, // instruction mask
 0x3f400000, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4.new) memb(Rs32+#u6:0)=Rt32",// instruction: S4_pstorerbtnew_io
 0xffe00004, // instruction mask
 0x42000000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; memb(Rs16+#u4:0)=#1",// instruction: X2_AUTOJOIN_SA1_addi_SS2_storebi1
 0xf8003f00, // instruction mask
 0x60003300, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "p1=cmp.gt(Rs16,#U5); if (!p1.new) jump:nt #r9:2",// instruction: J4_cmpgti_fp1_jump_nt
 0xffc02000, // instruction mask
 0x12c00000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "immext(#u26:6)",// instruction: A4_ext
 0xf0000000, // instruction mask
 0x0, // compare mask
 0xfff3fff, // bitmask
 0x0 // isDuplex
},
{ "Ryy32=memh_fifo(Re32=#U6)",// instruction: L4_loadalignh_ap
 0xffe03000, // instruction mask
 0x9a401000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.eq(Rs16,#U5); if (p0.new) jump:t #r9:2",// instruction: J4_cmpeqi_tp0_jump_t
 0xffc02000, // instruction mask
 0x10002000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Pd4=cmp.gtu(Rs32,#u9)",// instruction: C2_cmpgtui
 0xffe0001c, // instruction mask
 0x75800000, // compare mask
 0x3fe0, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memuh(Rt32<<#0+#U6)",// instruction: L4_loadruh_ur_expand_shamt_0
 0xffe03080, // instruction mask
 0x9d601000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memuh(Rt32<<#1+#U6)",// instruction: L4_loadruh_ur_expand_shamt_1
 0xffe03080, // instruction mask
 0x9d601080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memuh(Rt32<<#2+#U6)",// instruction: L4_loadruh_ur_expand_shamt_2
 0xffe03080, // instruction mask
 0x9d603000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memuh(Rt32<<#3+#U6)",// instruction: L4_loadruh_ur_expand_shamt_3
 0xffe03080, // instruction mask
 0x9d603080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Re16=#U6 ; Rd16=memw(Sp+#u5:2)",// instruction: X2_AUTOJOIN_SA1_seti_SL2_loadri_sp
 0xfc003e00, // instruction mask
 0x48003c00, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "if (!Pv4.new) memh(#u6)=Rt32",// instruction: S4_pstorerhfnew_abs
 0xffe02084, // instruction mask
 0xaf402084, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "Ryy32=memh_fifo(Rt32<<#1+#U6)",// instruction: L4_loadalignh_ur_expand_shamt_1
 0xffe03080, // instruction mask
 0x9c401080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Ryy32=memh_fifo(Rt32<<#3+#U6)",// instruction: L4_loadalignh_ur_expand_shamt_3
 0xffe03080, // instruction mask
 0x9c403080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4.new) memh(Rs32+#u6:1)=Rt.H32",// instruction: S4_pstorerffnew_io
 0xffe00004, // instruction mask
 0x46600000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4.new) Rd32=memub(#u6)",// instruction: L4_ploadrubtnew_abs
 0xffe03880, // instruction mask
 0x9f203080, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; Rdd8=combine(#0,Rs16)",// instruction: X2_AUTOJOIN_SA1_addi_SA1_combinezr
 0xf8003d08, // instruction mask
 0x20003d00, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (!Pv4) memh(Rs32+#u6:1)=#S6",// instruction: S4_storeirhf_io
 0xffe00000, // instruction mask
 0x38a00000, // compare mask
 0x201f, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.eq(Rs16,#-1); if (p0.new) jump:nt #r9:2",// instruction: J4_cmpeqn1_tp0_jump_nt
 0xffc02300, // instruction mask
 0x11800000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "p1=tstbit(Rs16,#0); if (!p1.new) jump:t #r9:2",// instruction: J4_tstbit0_fp1_jump_t
 0xffc02300, // instruction mask
 0x13c02300, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=membh(Rt32<<#2+#U6)",// instruction: L4_loadbsw4_ur_expand_shamt_2
 0xffe03080, // instruction mask
 0x9ce03000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=membh(Rs32+#s11:2)",// instruction: L2_loadbsw4_io
 0xf9e00000, // instruction mask
 0x90e00000, // compare mask
 0x6003fe0, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4.new) memd(Rs32+#u6:3)=Rtt32",// instruction: S4_pstorerdfnew_io
 0xffe00004, // instruction mask
 0x46c00000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4) memh(#u6)=Rt32",// instruction: S4_pstorerht_abs
 0xffe02084, // instruction mask
 0xaf400080, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "memw(Re32=#U6)=Nt8.new",// instruction: S4_storerinew_ap
 0xffe03880, // instruction mask
 0xaba01080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "if (cmp.gtu(Ns8.new,#U5)) jump:nt #r9:2",// instruction: J4_cmpgtui_t_jumpnv_nt
 0xffc02000, // instruction mask
 0x25000000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "memh(Rs32+#u6:1)-=#U5",// instruction: L4_isub_memoph_io
 0xff602060, // instruction mask
 0x3f200020, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4) memd(#u6)=Rtt32",// instruction: S4_pstorerdf_abs
 0xffe02084, // instruction mask
 0xafc00084, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "if (!cmp.eq(Ns8.new,#U5)) jump:nt #r9:2",// instruction: J4_cmpeqi_f_jumpnv_nt
 0xffc02000, // instruction mask
 0x24400000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Re16=#U6 ; Rd16=memub(Rs16+#u4:0)",// instruction: X2_AUTOJOIN_SA1_seti_SL1_loadrub_io
 0xfc003000, // instruction mask
 0x48001000, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; Rdd8=memd(Sp+#u5:3)",// instruction: X2_AUTOJOIN_SA1_addi_SL2_loadrd_sp
 0xf8003f00, // instruction mask
 0x40003e00, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "Rd32=memh(gp+#u16:1)",// instruction: L2_loadrhgp
 0xf9e00000, // instruction mask
 0x49400000, // compare mask
 0x61f3fe0, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4) memh(#u6)=Nt8.new",// instruction: S4_pstorerhnewt_abs
 0xffe03884, // instruction mask
 0xafa00880, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memb(gp+#u16:0)",// instruction: L2_loadrbgp
 0xf9e00000, // instruction mask
 0x49000000, // compare mask
 0x61f3fe0, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.gt(Rs16,#U5); if (p1.new) jump:t #r9:2",// instruction: J4_cmpgti_tp1_jump_t
 0xffc02000, // instruction mask
 0x12802000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memuh(gp+#u16:1)",// instruction: L2_loadruhgp
 0xf9e00000, // instruction mask
 0x49600000, // compare mask
 0x61f3fe0, // bitmask
 0x0 // isDuplex
},
{ "if (!Pt4.new) Rd32=memh(Rs32+#u6:1)",// instruction: L2_ploadrhfnew_io
 0xffe02000, // instruction mask
 0x47400000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "memh(Re32=#U6)=Rt.H32",// instruction: S4_storerf_ap
 0xffe02080, // instruction mask
 0xab600080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=memd(gp+#u16:3)",// instruction: L2_loadrdgp
 0xf9e00000, // instruction mask
 0x49c00000, // compare mask
 0x61f3fe0, // bitmask
 0x0 // isDuplex
},
{ "Rdd32=memd(Re32=#U6)",// instruction: L4_loadrd_ap
 0xffe03000, // instruction mask
 0x9bc01000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "if (!Pt4) Rd32=memb(#u6)",// instruction: L4_ploadrbf_abs
 0xffe03880, // instruction mask
 0x9f002880, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4) memh(Rs32+#u6:1)=Nt8.new",// instruction: S2_pstorerhnewt_io
 0xffe01804, // instruction mask
 0x40a00800, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.gt(Rs16,Rt16); if (p0.new) jump:nt #r9:2",// instruction: J4_cmpgt_tp0_jump_nt
 0xffc03000, // instruction mask
 0x14800000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "memh(Rs32+#u6:1)=setbit(#U5)",// instruction: L4_ior_memoph_io
 0xff602060, // instruction mask
 0x3f200060, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "memb(gp+#u16:0)=Rt32",// instruction: S2_storerbgp
 0xf9e00000, // instruction mask
 0x48000000, // compare mask
 0x61f20ff, // bitmask
 0x0 // isDuplex
},
{ "Pd4=!cmp.eq(Rs32,#s10)",// instruction: C4_cmpneqi
 0xffc0001c, // instruction mask
 0x75000010, // compare mask
 0x203fe0, // bitmask
 0x0 // isDuplex
},
{ "if (!Pt4) Rd32=memuh(#u6)",// instruction: L4_ploadruhf_abs
 0xffe03880, // instruction mask
 0x9f602880, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4.new) memd(Rs32+#u6:3)=Rtt32",// instruction: S4_pstorerdtnew_io
 0xffe00004, // instruction mask
 0x42c00000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.eq(Rs16,#-1); if (p0.new) jump:t #r9:2",// instruction: J4_cmpeqn1_tp0_jump_t
 0xffc02300, // instruction mask
 0x11802000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4.new) Rdd32=memd(Rs32+#u6:3)",// instruction: L2_ploadrdtnew_io
 0xffe02000, // instruction mask
 0x43c00000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "Rd32=membh(Re32=#U6)",// instruction: L4_loadbsw2_ap
 0xffe03000, // instruction mask
 0x9a201000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.gtu(Rs16,#U5); if (!p0.new) jump:nt #r9:2",// instruction: J4_cmpgtui_fp0_jump_nt
 0xffc02000, // instruction mask
 0x11400000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (!cmp.gtu(Rt32,Ns8.new)) jump:t #r9:2",// instruction: J4_cmpltu_f_jumpnv_t
 0xffc02000, // instruction mask
 0x22402000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (!cmp.gtu(Ns8.new,#U5)) jump:nt #r9:2",// instruction: J4_cmpgtui_f_jumpnv_nt
 0xffc02000, // instruction mask
 0x25400000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (Rs32>=#0) jump:nt #r13:2",// instruction: J2_jumprgtez
 0xffc01000, // instruction mask
 0x61400000, // compare mask
 0x202ffe, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4.new) memw(Rs32+#u6:2)=Nt8.new",// instruction: S4_pstorerinewtnew_io
 0xffe01804, // instruction mask
 0x42a01000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "Pd4=cmp.eq(Rs32,#s10)",// instruction: C2_cmpeqi
 0xffc0001c, // instruction mask
 0x75000000, // compare mask
 0x203fe0, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4.new) memd(#u6)=Rtt32",// instruction: S4_pstorerdtnew_abs
 0xffe02084, // instruction mask
 0xafc02080, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4) memh(#u6)=Nt8.new",// instruction: S4_pstorerhnewf_abs
 0xffe03884, // instruction mask
 0xafa00884, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "Rx32+=mpyi(Rs32,#u8)",// instruction: M2_macsip
 0xff802000, // instruction mask
 0xe1000000, // compare mask
 0x1fe0, // bitmask
 0x0 // isDuplex
},
{ "if (Rs32!=#0) jump:t #r13:2",// instruction: J2_jumprzpt
 0xffc01000, // instruction mask
 0x61001000, // compare mask
 0x202ffe, // bitmask
 0x0 // isDuplex
},
{ "memw(gp+#u16:2)=Rt32",// instruction: S2_storerigp
 0xf9e00000, // instruction mask
 0x48800000, // compare mask
 0x61f20ff, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.gtu(Rs16,Rt16); if (!p1.new) jump:t #r9:2",// instruction: J4_cmpgtu_fp1_jump_t
 0xffc03000, // instruction mask
 0x15403000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.gtu(Rs16,Rt16); if (!p0.new) jump:nt #r9:2",// instruction: J4_cmpgtu_fp0_jump_nt
 0xffc03000, // instruction mask
 0x15400000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4.new) memw(Rs32+#u6:2)=Nt8.new",// instruction: S4_pstorerinewfnew_io
 0xffe01804, // instruction mask
 0x46a01000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; if (p0.new) Rd16=#0",// instruction: X2_AUTOJOIN_SA1_addi_SA1_clrtnew
 0xf8003e70, // instruction mask
 0x20003a40, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "Rd32=add(Rs32,add(Ru32,#s6))",// instruction: S4_addaddi
 0xff800000, // instruction mask
 0xdb000000, // compare mask
 0x6020e0, // bitmask
 0x0 // isDuplex
},
{ "Re16=#U6 ; Rdd8=memd(Sp+#u5:3)",// instruction: X2_AUTOJOIN_SA1_seti_SL2_loadrd_sp
 0xfc003f00, // instruction mask
 0x48003e00, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "p0=cmp.eq(Rs16,Rt16); if (p0.new) jump:t #r9:2",// instruction: J4_cmpeq_tp0_jump_t
 0xffc03000, // instruction mask
 0x14002000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.gt(Rs16,#-1); if (p0.new) jump:nt #r9:2",// instruction: J4_cmpgtn1_tp0_jump_nt
 0xffc02300, // instruction mask
 0x11800100, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4) memd(Rs32+#u6:3)=Rtt32",// instruction: S2_pstorerdf_io
 0xffe00004, // instruction mask
 0x44c00000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "if (!Pt4.new) Rd32=memb(#u6)",// instruction: L4_ploadrbfnew_abs
 0xffe03880, // instruction mask
 0x9f003880, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "Re16=#U6 ; Rd16=and(Rs16,#1)",// instruction: X2_AUTOJOIN_SA1_seti_SA1_and1
 0xfc003f00, // instruction mask
 0x28003200, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "p1=cmp.gtu(Rs16,Rt16); if (p1.new) jump:nt #r9:2",// instruction: J4_cmpgtu_tp1_jump_nt
 0xffc03000, // instruction mask
 0x15001000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.eq(Rs16,#U5); if (p1.new) jump:nt #r9:2",// instruction: J4_cmpeqi_tp1_jump_nt
 0xffc02000, // instruction mask
 0x12000000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "memw(Rs32+#u6:2)=clrbit(#U5)",// instruction: L4_iand_memopw_io
 0xff602060, // instruction mask
 0x3f400040, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4) memw(#u6)=Rt32",// instruction: S4_pstorerit_abs
 0xffe02084, // instruction mask
 0xaf800080, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4) memb(Rs32+#u6:0)=#S6",// instruction: S4_storeirbf_io
 0xffe00000, // instruction mask
 0x38800000, // compare mask
 0x201f, // bitmask
 0x0 // isDuplex
},
{ "Ryy32=memb_fifo(Rt32<<#1+#U6)",// instruction: L4_loadalignb_ur_expand_shamt_1
 0xffe03080, // instruction mask
 0x9c801080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Ryy32=memb_fifo(Rt32<<#0+#U6)",// instruction: L4_loadalignb_ur_expand_shamt_0
 0xffe03080, // instruction mask
 0x9c801000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.gt(Rs16,#-1); if (!p1.new) jump:t #r9:2",// instruction: J4_cmpgtn1_fp1_jump_t
 0xffc02300, // instruction mask
 0x13c02100, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (!Pt4.new) Rd32=memuh(#u6)",// instruction: L4_ploadruhfnew_abs
 0xffe03880, // instruction mask
 0x9f603880, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "Re16=#u6 ; Rd16=Rs16",// instruction: X2_AUTOJOIN_SA1_seti_SA1_tfr
 0xfc003f00, // instruction mask
 0x28003000, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "if (!cmp.gtu(Ns8.new,Rt32)) jump:t #r9:2",// instruction: J4_cmpgtu_f_jumpnv_t
 0xffc02000, // instruction mask
 0x21402000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rd16=#U6 ; memb(Rs16+#u4:0)=#1",// instruction: X2_AUTOJOIN_SA1_seti_SS2_storebi1
 0xfc003f00, // instruction mask
 0x68003300, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Rd16=#U6 ; memb(Rs16+#u4:0)=#0",// instruction: X2_AUTOJOIN_SA1_seti_SS2_storebi0
 0xfc003f00, // instruction mask
 0x68003200, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "if (cmp.eq(Ns8.new,#-1)) jump:nt #r9:2",// instruction: J4_cmpeqn1_t_jumpnv_nt
 0xffc02000, // instruction mask
 0x26000000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4) memh(Rs32+#u6:1)=Rt.H32",// instruction: S2_pstorerft_io
 0xffe00004, // instruction mask
 0x40600000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#s7) ; Rd16=zxth(Rs16)",// instruction: X2_AUTOJOIN_SA1_addi_SA1_zxth
 0xf8003f00, // instruction mask
 0x20003600, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; Rd16=and(Rs16,#255)",// instruction: X2_AUTOJOIN_SA1_addi_SA1_zxtb
 0xf8003f00, // instruction mask
 0x20003700, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (!tstbit(Ns8.new,#0)) jump:nt #r9:2",// instruction: J4_tstbit0_f_jumpnv_nt
 0xffc02000, // instruction mask
 0x25c00000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (!Pt4.new) Rd32=memw(Rs32+#u6:2)",// instruction: L2_ploadrifnew_io
 0xffe02000, // instruction mask
 0x47800000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "Re16=#U6 ; Rd16=memb(Rs16+#u3:0)",// instruction: X2_AUTOJOIN_SA1_seti_SL2_loadrb_io
 0xfc003800, // instruction mask
 0x48003000, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "memw(Rs32+#u6:2)=setbit(#U5)",// instruction: L4_ior_memopw_io
 0xff602060, // instruction mask
 0x3f400060, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "memb(Rs32+#u6:0)=#S8",// instruction: S4_storeirb_io
 0xfe600000, // instruction mask
 0x3c000000, // compare mask
 0x207f, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4) memb(Rs32+#u6:0)=#S6",// instruction: S4_storeirbt_io
 0xffe00000, // instruction mask
 0x38000000, // compare mask
 0x201f, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.gtu(Rs16,#U5); if (!p1.new) jump:nt #r9:2",// instruction: J4_cmpgtui_fp1_jump_nt
 0xffc02000, // instruction mask
 0x13400000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (!Pu4) jump:nt #r15:2",// instruction: J2_jumpf
 0xff201800, // instruction mask
 0x5c200000, // compare mask
 0xdf20fe, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4.new) memb(Rs32+#u6:0)=Rt32",// instruction: S4_pstorerbfnew_io
 0xffe00004, // instruction mask
 0x46000000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#s7) ; Rd16=sxtb(Rs16)",// instruction: X2_AUTOJOIN_SA1_addi_SA1_sxtb
 0xf8003f00, // instruction mask
 0x20003500, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (!Pt4) Rd32=memh(Rs32+#u6:1)",// instruction: L2_ploadrhf_io
 0xffe02000, // instruction mask
 0x45400000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4.new) memb(#u6)=Rt32",// instruction: S4_pstorerbtnew_abs
 0xffe02084, // instruction mask
 0xaf002080, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "Re16=#U6 ; Rd16=and(Rs16,#255)",// instruction: X2_AUTOJOIN_SA1_seti_SA1_zxtb
 0xfc003f00, // instruction mask
 0x28003700, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "memb(Re32=#U6)=Nt8.new",// instruction: S4_storerbnew_ap
 0xffe03880, // instruction mask
 0xaba00080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "Rd32=mux(Pu4,#s8,Rs32)",// instruction: C2_muxri
 0xff802000, // instruction mask
 0x73800000, // compare mask
 0x1fe0, // bitmask
 0x0 // isDuplex
},
{ "if (!Pt4) Rd32=memub(#u6)",// instruction: L4_ploadrubf_abs
 0xffe03880, // instruction mask
 0x9f202880, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.gt(Rs16,#U5); if (p0.new) jump:nt #r9:2",// instruction: J4_cmpgti_tp0_jump_nt
 0xffc02000, // instruction mask
 0x10800000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4) memw(Rs32+#u6:2)=Rt32",// instruction: S2_pstorerif_io
 0xffe00004, // instruction mask
 0x44800000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "if (!Pv4) memb(#u6)=Nt8.new",// instruction: S4_pstorerbnewf_abs
 0xffe03884, // instruction mask
 0xafa00084, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "if (!Pu4.new) jump:t #r15:2",// instruction: J2_jumpfnewpt
 0xff201800, // instruction mask
 0x5c201800, // compare mask
 0xdf20fe, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4) Rdd32=memd(Rs32+#u6:3)",// instruction: L2_ploadrdt_io
 0xffe02000, // instruction mask
 0x41c00000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "if (!cmp.eq(Ns8.new,Rt32)) jump:t #r9:2",// instruction: J4_cmpeq_f_jumpnv_t
 0xffc02000, // instruction mask
 0x20402000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "memh(Rs32+#s11:1)=Rt32",// instruction: S2_storerh_io
 0xf9e00000, // instruction mask
 0xa1400000, // compare mask
 0x60020ff, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.gt(Rs16,Rt16); if (!p0.new) jump:nt #r9:2",// instruction: J4_cmpgt_fp0_jump_nt
 0xffc03000, // instruction mask
 0x14c00000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#s7) ; if (!p0.new) dealloc_return:nt",// instruction: X2_AUTOJOIN_SA1_addi_SL2_return_fnew
 0xf8003fc7, // instruction mask
 0x40003f47, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (!cmp.eq(Ns8.new,#-1)) jump:nt #r9:2",// instruction: J4_cmpeqn1_f_jumpnv_nt
 0xffc02000, // instruction mask
 0x26400000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; Rd16=#u6",// instruction: X2_AUTOJOIN_SA1_addi_SA1_seti
 0xf8003c00, // instruction mask
 0x20002800, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (Pu4.new) Rd32=#s12",// instruction: C2_cmovenewit
 0xff902000, // instruction mask
 0x7e002000, // compare mask
 0xf1fe0, // bitmask
 0x0 // isDuplex
},
{ "if (!Pu4.new) Rd32=#s12",// instruction: C2_cmovenewif
 0xff902000, // instruction mask
 0x7e802000, // compare mask
 0xf1fe0, // bitmask
 0x0 // isDuplex
},
{ "if (cmp.gtu(Rt32,Ns8.new)) jump:nt #r9:2",// instruction: J4_cmpltu_t_jumpnv_nt
 0xffc02000, // instruction mask
 0x22000000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (cmp.gtu(Ns8.new,Rt32)) jump:t #r9:2",// instruction: J4_cmpgtu_t_jumpnv_t
 0xffc02000, // instruction mask
 0x21002000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memub(Rt32<<#2+#U6)",// instruction: L4_loadrub_ur_expand_shamt_2
 0xffe03080, // instruction mask
 0x9d203000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "if (cmp.eq(Ns8.new,#-1)) jump:t #r9:2",// instruction: J4_cmpeqn1_t_jumpnv_t
 0xffc02000, // instruction mask
 0x26002000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Re16=#U6 ; Rd16=memh(Rs16+#u3:1)",// instruction: X2_AUTOJOIN_SA1_seti_SL2_loadrh_io
 0xfc003800, // instruction mask
 0x48002000, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "if (!Pu4) Rd32=#s12",// instruction: C2_cmoveif
 0xff902000, // instruction mask
 0x7e800000, // compare mask
 0xf1fe0, // bitmask
 0x0 // isDuplex
},
{ "memh(Ru32<<#3+#U6)=Rt.H32",// instruction: S4_storerf_ur_expand_shamt_3
 0xffe020c0, // instruction mask
 0xad6020c0, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memh(Ru32<<#2+#U6)=Rt.H32",// instruction: S4_storerf_ur_expand_shamt_2
 0xffe020c0, // instruction mask
 0xad602080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memh(Ru32<<#1+#U6)=Rt.H32",// instruction: S4_storerf_ur_expand_shamt_1
 0xffe020c0, // instruction mask
 0xad6000c0, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memh(Ru32<<#0+#U6)=Rt.H32",// instruction: S4_storerf_ur_expand_shamt_0
 0xffe020c0, // instruction mask
 0xad600080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "if (Pu4) Rd32=#s12",// instruction: C2_cmoveit
 0xff902000, // instruction mask
 0x7e000000, // compare mask
 0xf1fe0, // bitmask
 0x0 // isDuplex
},
{ "if (cmp.gtu(Ns8.new,#U5)) jump:t #r9:2",// instruction: J4_cmpgtui_t_jumpnv_t
 0xffc02000, // instruction mask
 0x25002000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (Rs32==#0) jump:t #r13:2",// instruction: J2_jumprnzpt
 0xffc01000, // instruction mask
 0x61801000, // compare mask
 0x202ffe, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4.new) memw(#u6)=Nt8.new",// instruction: S4_pstorerinewtnew_abs
 0xffe03884, // instruction mask
 0xafa03080, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#s7) ; dealloc_return",// instruction: X2_AUTOJOIN_SA1_addi_SL2_return
 0xf8003fc4, // instruction mask
 0x40003f40, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (Pt4) Rd32=memh(#u6)",// instruction: L4_ploadrht_abs
 0xffe03880, // instruction mask
 0x9f402080, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4.new) Rd32=memub(Rs32+#u6:0)",// instruction: L2_ploadrubtnew_io
 0xffe02000, // instruction mask
 0x43200000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "if (!Pt4.new) Rdd32=memd(#u6)",// instruction: L4_ploadrdfnew_abs
 0xffe03880, // instruction mask
 0x9fc03880, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memub(Rs32+#s11:0)",// instruction: L2_loadrub_io
 0xf9e00000, // instruction mask
 0x91200000, // compare mask
 0x6003fe0, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4.new) Rd32=memuh(Rs32+#u6:1)",// instruction: L2_ploadruhtnew_io
 0xffe02000, // instruction mask
 0x43600000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.eq(Rs16,#U5); if (!p0.new) jump:t #r9:2",// instruction: J4_cmpeqi_fp0_jump_t
 0xffc02000, // instruction mask
 0x10402000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (Pu4) Rd32=add(Rs32,#s8)",// instruction: A2_paddit
 0xff802000, // instruction mask
 0x74000000, // compare mask
 0x1fe0, // bitmask
 0x0 // isDuplex
},
{ "if (!Pu4) Rd32=add(Rs32,#s8)",// instruction: A2_paddif
 0xff802000, // instruction mask
 0x74800000, // compare mask
 0x1fe0, // bitmask
 0x0 // isDuplex
},
{ "memw(Rs32+#s11:2)=Rt32",// instruction: S2_storeri_io
 0xf9e00000, // instruction mask
 0xa1800000, // compare mask
 0x60020ff, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.gtu(Rs16,Rt16); if (p1.new) jump:t #r9:2",// instruction: J4_cmpgtu_tp1_jump_t
 0xffc03000, // instruction mask
 0x15003000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.gt(Rs16,Rt16); if (!p0.new) jump:t #r9:2",// instruction: J4_cmpgt_fp0_jump_t
 0xffc03000, // instruction mask
 0x14c02000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (!Pu4) jump:t #r15:2",// instruction: J2_jumpfpt
 0xff201800, // instruction mask
 0x5c201000, // compare mask
 0xdf20fe, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4.new) memw(#u6)=Rt32",// instruction: S4_pstoreritnew_abs
 0xffe02084, // instruction mask
 0xaf802080, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "memw(Ru32<<#0+#U6)=Rt32",// instruction: S4_storeri_ur_expand_shamt_0
 0xffe020c0, // instruction mask
 0xad800080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memw(Ru32<<#1+#U6)=Rt32",// instruction: S4_storeri_ur_expand_shamt_1
 0xffe020c0, // instruction mask
 0xad8000c0, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memw(Ru32<<#2+#U6)=Rt32",// instruction: S4_storeri_ur_expand_shamt_2
 0xffe020c0, // instruction mask
 0xad802080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memw(Ru32<<#3+#U6)=Rt32",// instruction: S4_storeri_ur_expand_shamt_3
 0xffe020c0, // instruction mask
 0xad8020c0, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4.new) memh(#u6)=Rt32",// instruction: S4_pstorerhtnew_abs
 0xffe02084, // instruction mask
 0xaf402080, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4.new) Rd32=memh(Rs32+#u6:1)",// instruction: L2_ploadrhtnew_io
 0xffe02000, // instruction mask
 0x43400000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4) Rd32=memub(Rs32+#u6:0)",// instruction: L2_ploadrubt_io
 0xffe02000, // instruction mask
 0x41200000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "Re16=#U6 ; Rdd8=combine(#0,Rs16)",// instruction: X2_AUTOJOIN_SA1_seti_SA1_combinezr
 0xfc003d08, // instruction mask
 0x28003d00, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; Rd16=memh(Rs16+#u3:1)",// instruction: X2_AUTOJOIN_SA1_addi_SL2_loadrh_io
 0xf8003800, // instruction mask
 0x40002000, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (!cmp.eq(Ns8.new,#U5)) jump:t #r9:2",// instruction: J4_cmpeqi_f_jumpnv_t
 0xffc02000, // instruction mask
 0x24402000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; p0=cmp.eq(Rs16,#u2)",// instruction: X2_AUTOJOIN_SA1_addi_SA1_cmpeqi
 0xf8003f00, // instruction mask
 0x20003900, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "memh(Rs32+#u6:1)=#S8",// instruction: S4_storeirh_io
 0xfe600000, // instruction mask
 0x3c200000, // compare mask
 0x207f, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4.new) Rd32=memb(#u6)",// instruction: L4_ploadrbtnew_abs
 0xffe03880, // instruction mask
 0x9f003080, // compare mask
 0x1f0100, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4) Rd32=memw(Rs32+#u6:2)",// instruction: L2_ploadrit_io
 0xffe02000, // instruction mask
 0x41800000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "memw(Rs32+#u6:2)=#S8",// instruction: S4_storeiri_io
 0xfe600000, // instruction mask
 0x3c400000, // compare mask
 0x207f, // bitmask
 0x0 // isDuplex
},
{ "if (!cmp.eq(Ns8.new,Rt32)) jump:nt #r9:2",// instruction: J4_cmpeq_f_jumpnv_nt
 0xffc02000, // instruction mask
 0x20400000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (Pu4.new) jump:t #r15:2",// instruction: J2_jumptnewpt
 0xff201800, // instruction mask
 0x5c001800, // compare mask
 0xdf20fe, // bitmask
 0x0 // isDuplex
},
{ "memh(Ru32<<#1+#U6)=Rt32",// instruction: S4_storerh_ur_expand_shamt_1
 0xffe020c0, // instruction mask
 0xad4000c0, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memh(Ru32<<#3+#U6)=Rt32",// instruction: S4_storerh_ur_expand_shamt_3
 0xffe020c0, // instruction mask
 0xad4020c0, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "memh(Ru32<<#2+#U6)=Rt32",// instruction: S4_storerh_ur_expand_shamt_2
 0xffe020c0, // instruction mask
 0xad402080, // compare mask
 0x3f, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; Rd16=memub(Rs16+#u4:0)",// instruction: X2_AUTOJOIN_SA1_addi_SL1_loadrub_io
 0xf8003000, // instruction mask
 0x40001000, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (!Pv4.new) memd(#u6)=Rtt32",// instruction: S4_pstorerdfnew_abs
 0xffe02084, // instruction mask
 0xafc02084, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "Rd16=#u6 ; if (p0.new) dealloc_return:nt",// instruction: X2_AUTOJOIN_SA1_seti_SL2_return_tnew
 0xfc003fc7, // instruction mask
 0x48003f46, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "memh(Rs32+#u6:1)|=Rt32",// instruction: L4_or_memoph_io
 0xff602060, // instruction mask
 0x3e200060, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "Ryy32=memh_fifo(Rs32+#s11:1)",// instruction: L2_loadalignh_io
 0xf9e00000, // instruction mask
 0x90400000, // compare mask
 0x6003fe0, // bitmask
 0x0 // isDuplex
},
{ "Ryy32=memb_fifo(Re32=#U6)",// instruction: L4_loadalignb_ap
 0xffe03000, // instruction mask
 0x9a801000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memh(Rt32<<#2+#U6)",// instruction: L4_loadrh_ur_expand_shamt_2
 0xffe03080, // instruction mask
 0x9d403000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memh(Rt32<<#3+#U6)",// instruction: L4_loadrh_ur_expand_shamt_3
 0xffe03080, // instruction mask
 0x9d403080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memh(Rt32<<#0+#U6)",// instruction: L4_loadrh_ur_expand_shamt_0
 0xffe03080, // instruction mask
 0x9d401000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memh(Rt32<<#1+#U6)",// instruction: L4_loadrh_ur_expand_shamt_1
 0xffe03080, // instruction mask
 0x9d401080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memw(gp+#u16:2)",// instruction: L2_loadrigp
 0xf9e00000, // instruction mask
 0x49800000, // compare mask
 0x61f3fe0, // bitmask
 0x0 // isDuplex
},
{ "Rd32=mux(Pu4,#s8,#S8)",// instruction: C2_muxii
 0xfe000000, // instruction mask
 0x7a000000, // compare mask
 0x1fe0, // bitmask
 0x0 // isDuplex
},
{ "Rd32=mux(Pu4,Rs32,#s8)",// instruction: C2_muxir
 0xff802000, // instruction mask
 0x73000000, // compare mask
 0x1fe0, // bitmask
 0x0 // isDuplex
},
{ "memh(Rs32+#u6:1)-=Rt32",// instruction: L4_sub_memoph_io
 0xff602060, // instruction mask
 0x3e200020, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
{ "if (!cmp.eq(Ns8.new,#-1)) jump:t #r9:2",// instruction: J4_cmpeqn1_f_jumpnv_t
 0xffc02000, // instruction mask
 0x26402000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; Rd16=#-1",// instruction: X2_AUTOJOIN_SA1_addi_SA1_setin1
 0xf8003e40, // instruction mask
 0x20003a00, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "p1=cmp.eq(Rs16,Rt16); if (!p1.new) jump:nt #r9:2",// instruction: J4_cmpeq_fp1_jump_nt
 0xffc03000, // instruction mask
 0x14401000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.gt(Rs16,Rt16); if (p1.new) jump:nt #r9:2",// instruction: J4_cmpgt_tp1_jump_nt
 0xffc03000, // instruction mask
 0x14801000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rd32=sub(#s10,Rs32)",// instruction: A2_subri
 0xffc00000, // instruction mask
 0x76400000, // compare mask
 0x203fe0, // bitmask
 0x0 // isDuplex
},
{ "Re16=#U6 ; if (!p0.new) Rd16=#0",// instruction: X2_AUTOJOIN_SA1_seti_SA1_clrfnew
 0xfc003e70, // instruction mask
 0x28003a50, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "memh(Rs32+#s11:1)=Nt8.new",// instruction: S2_storerhnew_io
 0xf9e01800, // instruction mask
 0xa1a00800, // compare mask
 0x60020ff, // bitmask
 0x0 // isDuplex
},
{ "if (Pt4.new) Rd32=memw(Rs32+#u6:2)",// instruction: L2_ploadritnew_io
 0xffe02000, // instruction mask
 0x43800000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.eq(Rs16,#-1); if (!p1.new) jump:t #r9:2",// instruction: J4_cmpeqn1_fp1_jump_t
 0xffc02300, // instruction mask
 0x13c02000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (Pu4) jump:t #r15:2",// instruction: J2_jumptpt
 0xff201800, // instruction mask
 0x5c001000, // compare mask
 0xdf20fe, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#s7) ; deallocframe",// instruction: X2_AUTOJOIN_SA1_addi_SL2_deallocframe
 0xf8003fc4, // instruction mask
 0x40003f00, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (!Pv4.new) memw(Rs32+#u6:2)=#S6",// instruction: S4_storeirifnew_io
 0xffe00000, // instruction mask
 0x39c00000, // compare mask
 0x201f, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.gtu(Rs16,#U5); if (p1.new) jump:t #r9:2",// instruction: J4_cmpgtui_tp1_jump_t
 0xffc02000, // instruction mask
 0x13002000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.gtu(Rs16,Rt16); if (p0.new) jump:t #r9:2",// instruction: J4_cmpgtu_tp0_jump_t
 0xffc03000, // instruction mask
 0x15002000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4) memh(Rs32+#u6:1)=Rt32",// instruction: S2_pstorerht_io
 0xffe00004, // instruction mask
 0x40400000, // compare mask
 0x20f8, // bitmask
 0x0 // isDuplex
},
{ "if (Rs32==#0) jump:nt #r13:2",// instruction: J2_jumprnz
 0xffc01000, // instruction mask
 0x61800000, // compare mask
 0x202ffe, // bitmask
 0x0 // isDuplex
},
{ "Rd16=#u6 ; deallocframe",// instruction: X2_AUTOJOIN_SA1_seti_SL2_deallocframe
 0xfc003fc4, // instruction mask
 0x48003f00, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Re16=#U6 ; Rd16=memuh(Rs16+#u3:1)",// instruction: X2_AUTOJOIN_SA1_seti_SL2_loadruh_io
 0xfc003800, // instruction mask
 0x48002800, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "if (Rs32>=#0) jump:t #r13:2",// instruction: J2_jumprgtezpt
 0xffc01000, // instruction mask
 0x61401000, // compare mask
 0x202ffe, // bitmask
 0x0 // isDuplex
},
{ "Rd16=#u6 ; if (!p0) jumpr Lr",// instruction: X2_AUTOJOIN_SA1_seti_SL2_jumpr31_f
 0xfc003fc7, // instruction mask
 0x48003fc5, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Re16=#U6 ; Rdd8=combine(#0,#u2)",// instruction: X2_AUTOJOIN_SA1_seti_SA1_combine0i
 0xfc003d18, // instruction mask
 0x28003c00, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Rd16=#u6 ; if (p0) jumpr Lr",// instruction: X2_AUTOJOIN_SA1_seti_SL2_jumpr31_t
 0xfc003fc7, // instruction mask
 0x48003fc4, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; memb(Rs16+#u4:0)=#0",// instruction: X2_AUTOJOIN_SA1_addi_SS2_storebi0
 0xf8003f00, // instruction mask
 0x60003200, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (Pv4) memh(#u6)=Rt.H32",// instruction: S4_pstorerft_abs
 0xffe02084, // instruction mask
 0xaf600080, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.eq(Rs16,Rt16); if (p1.new) jump:nt #r9:2",// instruction: J4_cmpeq_tp1_jump_nt
 0xffc03000, // instruction mask
 0x14001000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.eq(Rs16,#U5); if (p0.new) jump:nt #r9:2",// instruction: J4_cmpeqi_tp0_jump_nt
 0xffc02000, // instruction mask
 0x10000000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; Rdd8=combine(#1,#u2)",// instruction: X2_AUTOJOIN_SA1_addi_SA1_combine1i
 0xf8003d18, // instruction mask
 0x20003c08, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "Rx16=add(Rx16,#s7) ; if (p0.new) jumpr:nt Lr",// instruction: X2_AUTOJOIN_SA1_addi_SL2_jumpr31_tnew
 0xf8003fc7, // instruction mask
 0x40003fc6, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "memh(gp+#u16:1)=Rt.H32",// instruction: S2_storerfgp
 0xf9e00000, // instruction mask
 0x48600000, // compare mask
 0x61f20ff, // bitmask
 0x0 // isDuplex
},
{ "jump #r22:2",// instruction: J2_jump
 0xfe000000, // instruction mask
 0x58000000, // compare mask
 0x1ff3ffe, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memubh(Rs32+#s11:1)",// instruction: L2_loadbzw2_io
 0xf9e00000, // instruction mask
 0x90600000, // compare mask
 0x6003fe0, // bitmask
 0x0 // isDuplex
},
{ "p3=sp3loop0(#r7:2,#U10)",// instruction: J2_ploop3si
 0xffe00000, // instruction mask
 0x69e00000, // compare mask
 0x1f18, // bitmask
 0x0 // isDuplex
},
{ "if (!Pt4.new) Rdd32=memd(Rs32+#u6:3)",// instruction: L2_ploadrdfnew_io
 0xffe02000, // instruction mask
 0x47c00000, // compare mask
 0x7e0, // bitmask
 0x0 // isDuplex
},
{ "p3=sp3loop0(#r7:2,Rs32)",// instruction: J2_ploop3sr
 0xffe00000, // instruction mask
 0x60e00000, // compare mask
 0x1f18, // bitmask
 0x0 // isDuplex
},
{ "if (cmp.gt(Rt32,Ns8.new)) jump:nt #r9:2",// instruction: J4_cmplt_t_jumpnv_nt
 0xffc02000, // instruction mask
 0x21800000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; memw(Rs16+#u4:2)=#1",// instruction: X2_AUTOJOIN_SA1_addi_SS2_storewi1
 0xf8003f00, // instruction mask
 0x60003100, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; memw(Rs16+#u4:2)=#0",// instruction: X2_AUTOJOIN_SA1_addi_SS2_storewi0
 0xf8003f00, // instruction mask
 0x60003000, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (tstbit(Ns8.new,#0)) jump:nt #r9:2",// instruction: J4_tstbit0_t_jumpnv_nt
 0xffc02000, // instruction mask
 0x25800000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.eq(Rs16,Rt16); if (!p1.new) jump:t #r9:2",// instruction: J4_cmpeq_fp1_jump_t
 0xffc03000, // instruction mask
 0x14403000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Re16=#U6 ; Rdd8=combine(#1,#u2)",// instruction: X2_AUTOJOIN_SA1_seti_SA1_combine1i
 0xfc003d18, // instruction mask
 0x28003c08, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; Rdd8=combine(#0,#u2)",// instruction: X2_AUTOJOIN_SA1_addi_SA1_combine0i
 0xf8003d18, // instruction mask
 0x20003c00, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "memd(Rs32+#s11:3)=Rtt32",// instruction: S2_storerd_io
 0xf9e00000, // instruction mask
 0xa1c00000, // compare mask
 0x60020ff, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memub(Rt32<<#0+#U6)",// instruction: L4_loadrub_ur_expand_shamt_0
 0xffe03080, // instruction mask
 0x9d201000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memub(Rt32<<#1+#U6)",// instruction: L4_loadrub_ur_expand_shamt_1
 0xffe03080, // instruction mask
 0x9d201080, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4) memw(#u6)=Nt8.new",// instruction: S4_pstorerinewt_abs
 0xffe03884, // instruction mask
 0xafa01080, // compare mask
 0x30078, // bitmask
 0x0 // isDuplex
},
{ "Rx32=add(#u8,asl(Rx32,#U5))",// instruction: S4_addi_asl_ri
 0xff000016, // instruction mask
 0xde000004, // compare mask
 0xe020e8, // bitmask
 0x0 // isDuplex
},
{ "Re16=#U6 ; Rdd8=combine(#2,#u2)",// instruction: X2_AUTOJOIN_SA1_seti_SA1_combine2i
 0xfc003d18, // instruction mask
 0x28003c10, // compare mask
 0x3f00000, // bitmask
 0x1 // isDuplex
},
{ "Pd4=cmph.eq(Rs32,#s8)",// instruction: A4_cmpheqi
 0xff600018, // instruction mask
 0xdd000008, // compare mask
 0x1fe0, // bitmask
 0x0 // isDuplex
},
{ "Rd32=memw(Re32=#U6)",// instruction: L4_loadri_ap
 0xffe03000, // instruction mask
 0x9b801000, // compare mask
 0xf60, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#s7) ; if (p0.new) dealloc_return:nt",// instruction: X2_AUTOJOIN_SA1_addi_SL2_return_tnew
 0xf8003fc7, // instruction mask
 0x40003f46, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (!cmp.gt(Rt32,Ns8.new)) jump:t #r9:2",// instruction: J4_cmplt_f_jumpnv_t
 0xffc02000, // instruction mask
 0x21c02000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (Pv4.new) memh(Rs32+#u6:1)=#S6",// instruction: S4_storeirhtnew_io
 0xffe00000, // instruction mask
 0x39200000, // compare mask
 0x201f, // bitmask
 0x0 // isDuplex
},
{ "p0=cmp.gtu(Rs16,#U5); if (p0.new) jump:t #r9:2",// instruction: J4_cmpgtui_tp0_jump_t
 0xffc02000, // instruction mask
 0x11002000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "if (cmp.gt(Ns8.new,#-1)) jump:t #r9:2",// instruction: J4_cmpgtn1_t_jumpnv_t
 0xffc02000, // instruction mask
 0x26802000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "Rx16=add(Rx16,#S7) ; memb(Rs16+#u4:0)=Rt16",// instruction: X2_AUTOJOIN_SA1_addi_SS1_storeb_io
 0xf8003000, // instruction mask
 0x60001000, // compare mask
 0x7f00000, // bitmask
 0x1 // isDuplex
},
{ "if (Pu4.new) Rd32=add(Rs32,#s8)",// instruction: A2_padditnew
 0xff802000, // instruction mask
 0x74002000, // compare mask
 0x1fe0, // bitmask
 0x0 // isDuplex
},
{ "p1=cmp.gtu(Rs16,#U5); if (p1.new) jump:nt #r9:2",// instruction: J4_cmpgtui_tp1_jump_nt
 0xffc02000, // instruction mask
 0x13000000, // compare mask
 0x3000fe, // bitmask
 0x0 // isDuplex
},
{ "memb(Rs32+#u6:0)=setbit(#U5)",// instruction: L4_ior_memopb_io
 0xff602060, // instruction mask
 0x3f000060, // compare mask
 0x1f80, // bitmask
 0x0 // isDuplex
},
};

}  // namespace Internal
}  // namespace Halide
