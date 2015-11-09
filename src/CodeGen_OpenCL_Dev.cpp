#include <sstream>
#include <algorithm>

#include "CodeGen_OpenCL_Dev.h"
#include "CodeGen_Internal.h"
#include "Debug.h"
#include "IROperator.h"
#include "IRMutator.h"

namespace Halide {
namespace Internal {

using std::ostringstream;
using std::string;
using std::vector;
using std::sort;

static ostringstream nil;

// OpenCL doesn't support vectors of bools, this mutator rewrites IR
// to use signed integer vectors instead. This means that all logical
// ops are re-written to be bitwise ops. This then requires that
// condition of select nodes be converted back to boolean (by
// comparing the rewritten expression with zero). The OpenCL C codegen
// then just omits (via peepholing) the extra conversion ops (casts,
// NE with zero, etc.) because OpenCL C's ops return/consume the types
// these conversions produce.
class EliminateBoolVectors : public IRMutator {
private:
    using IRMutator::visit;

    template <typename T>
    void visit_comparison(const T* op) {
        Expr a = mutate(op->a);
        Expr b = mutate(op->b);
        Type t = a.type();

        // Ensure both a and b have the same type (if this is a vector
        // comparison). This should only be necessary if the operands are
        // integer vectors (promoted from bool vectors).
        if (t.width > 1 && t.bits != b.type().bits) {
            internal_assert(t.code == Type::Int && b.type().code == Type::Int);

            t.bits = std::max(t.bits, b.type().bits);
            if (t != a.type()) {
                a = Cast::make(t, a);
            }
            if (t != b.type()) {
                b = Cast::make(t, b);
            }
        }

        if (!a.same_as(op->a) || !b.same_as(op->b)) {
            expr = T::make(a, b);
        } else {
            expr = op;
        }

        if (t.width > 1) {
            // To represent bool vectors, OpenCL uses vectors of signed
            // integers with the same width as the types being compared.
            t.code = Type::Int;
            expr = Cast::make(t, expr);
        }
    }

    void visit(const EQ *op) { visit_comparison(op); }
    void visit(const NE *op) { visit_comparison(op); }
    void visit(const LT *op) { visit_comparison(op); }
    void visit(const LE *op) { visit_comparison(op); }
    void visit(const GT *op) { visit_comparison(op); }
    void visit(const GE *op) { visit_comparison(op); }

    template <typename T>
    void visit_logical_binop(const T* op, const std::string& bitwise_op) {
        Expr a = mutate(op->a);
        Expr b = mutate(op->b);

        Type ta = a.type();
        Type tb = b.type();
        if (ta.width > 1) {
            // Ensure that both a and b have the same type.
            Type t = ta;
            t.bits = std::max(ta.bits, tb.bits);
            if (t != a.type()) {
                a = Cast::make(t, a);
            }
            if (t != b.type()) {
                b = Cast::make(t, b);
            }
            // Replace logical operation with bitwise operation.
            expr = Call::make(t, bitwise_op, {a, b}, Call::Intrinsic);
        } else if (!a.same_as(op->a) || !b.same_as(op->b)) {
            expr = T::make(a, b);
        } else {
            expr = op;
        }
    }

    void visit(const Or *op) {
        visit_logical_binop(op, Call::bitwise_or);
    }

    void visit(const And *op) {
        visit_logical_binop(op, Call::bitwise_and);
    }

    void visit(const Not *op) {
        Expr a = mutate(op->a);
        if (a.type().width > 1) {
            // Replace logical operation with bitwise operation.
            expr = Call::make(a.type(), Call::bitwise_not, {a}, Call::Intrinsic);
        } else if (!a.same_as(op->a)) {
            expr = Not::make(a);
        } else {
            expr = op;
        }
    }

    void visit(const Select *op) {
        Expr cond = mutate(op->condition);
        Expr true_value = mutate(op->true_value);
        Expr false_value = mutate(op->false_value);
        Type cond_ty = cond.type();
        if (cond_ty.width > 1) {
            // If the condition is a vector, it should be a vector of
            // ints, so rewrite it to compare to 0.
            internal_assert(cond_ty.code == Type::Int);

            // OpenCL's select function requires that all 3 operands
            // have the same width.
            internal_assert(true_value.type().bits == false_value.type().bits);
            if (true_value.type().bits != cond_ty.bits) {
                cond_ty.bits = true_value.type().bits;
                cond = Cast::make(cond_ty, cond);
            }

            // To make the Select op legal, convert it back to a
            // vector of bool by comparing with zero.
            expr = Select::make(NE::make(cond, make_zero(cond_ty)), true_value, false_value);
        } else if (!cond.same_as(op->condition) ||
                   !true_value.same_as(op->true_value) ||
                   !false_value.same_as(op->false_value)) {
            expr = Select::make(cond, true_value, false_value);
        } else {
            expr = op;
        }
    }

    void visit(const Broadcast *op) {
        Expr value = mutate(op->value);
        if (op->type.bits == 1) {
            expr = Broadcast::make(-Cast::make(Int(8), value), op->width);
        } else if (!value.same_as(op->value)) {
            expr = Broadcast::make(value, op->width);
        } else {
            expr = op;
        }
    }
};

Stmt eliminate_bool_vectors(Stmt s) {
    EliminateBoolVectors eliminator;
    return eliminator.mutate(s);
}

CodeGen_OpenCL_Dev::CodeGen_OpenCL_Dev(Target t) :
    clc(src_stream), target(t) {
}

string CodeGen_OpenCL_Dev::CodeGen_OpenCL_C::print_type(Type type) {
    ostringstream oss;
    if (type.is_float()) {
        if (type.bits == 16) {
            oss << "half";
        } else if (type.bits == 32) {
            oss << "float";
        } else if (type.bits == 64) {
            oss << "double";
        } else {
            user_error << "Can't represent a float with this many bits in OpenCL C: " << type << "\n";
        }

    } else {
        if (type.is_uint() && type.bits > 1) oss << 'u';
        switch (type.bits) {
        case 1:
            internal_assert(type.width == 1) << "Encountered vector of bool\n";
            oss << "bool";
            break;
        case 8:
            oss << "char";
            break;
        case 16:
            oss << "short";
            break;
        case 32:
            oss << "int";
            break;
        case 64:
            oss << "long";
            break;
        default:
            user_error << "Can't represent an integer with this many bits in OpenCL C: " << type << "\n";
        }
    }
    if (type.width != 1) {
        switch (type.width) {
        case 2:
        case 3:
        case 4:
        case 8:
        case 16:
            oss << type.width;
            break;
        default:
            user_error <<  "Unsupported vector width in OpenCL C: " << type << "\n";
        }
    }
    return oss.str();
}

string CodeGen_OpenCL_Dev::CodeGen_OpenCL_C::print_reinterpret(Type type, Expr e) {
    ostringstream oss;
    oss << "as_" << print_type(type) << "(" << print_expr(e) << ")";
    return oss.str();
}



namespace {
string simt_intrinsic(const string &name) {
    if (ends_with(name, ".__thread_id_x")) {
        return "get_local_id(0)";
    } else if (ends_with(name, ".__thread_id_y")) {
        return "get_local_id(1)";
    } else if (ends_with(name, ".__thread_id_z")) {
        return "get_local_id(2)";
    } else if (ends_with(name, ".__thread_id_w")) {
        return "get_local_id(3)";
    } else if (ends_with(name, ".__block_id_x")) {
        return "get_group_id(0)";
    } else if (ends_with(name, ".__block_id_y")) {
        return "get_group_id(1)";
    } else if (ends_with(name, ".__block_id_z")) {
        return "get_group_id(2)";
    } else if (ends_with(name, ".__block_id_w")) {
        return "get_group_id(3)";
    }
    internal_error << "simt_intrinsic called on bad variable name: " << name << "\n";
    return "";
}
}

void CodeGen_OpenCL_Dev::CodeGen_OpenCL_C::visit(const For *loop) {
    if (is_gpu_var(loop->name)) {
        internal_assert(loop->for_type == ForType::Parallel) << "kernel loop must be parallel\n";
        internal_assert(is_zero(loop->min));

        do_indent();
        stream << print_type(Int(32)) << " " << print_name(loop->name)
               << " = " << simt_intrinsic(loop->name) << ";\n";

        loop->body.accept(this);

    } else {
        user_assert(loop->for_type != ForType::Parallel) << "Cannot use parallel loops inside OpenCL kernel\n";
        CodeGen_C::visit(loop);
    }
}

void CodeGen_OpenCL_Dev::CodeGen_OpenCL_C::visit(const Ramp *op) {
    string id_base = print_expr(op->base);
    string id_stride = print_expr(op->stride);

    ostringstream rhs;
    rhs << id_base << " + " << id_stride << " * ("
        << print_type(op->type.vector_of(op->width)) << ")(0";
    // Note 0 written above.
    for (int i = 1; i < op->width; ++i) {
        rhs << ", " << i;
    }
    rhs << ")";
    print_assignment(op->type.vector_of(op->width), rhs.str());
}

void CodeGen_OpenCL_Dev::CodeGen_OpenCL_C::visit(const Broadcast *op) {
    string id_value = print_expr(op->value);

    print_assignment(op->type.vector_of(op->width), id_value);
}

namespace {
// Mapping of integer vector indices to OpenCL ".s" syntax.
const char * vector_elements = "0123456789ABCDEF";

// If e is a ramp expression with stride 1, return the base, otherwise undefined.
Expr is_ramp1(Expr e) {
    const Ramp *r = e.as<Ramp>();
    if (r == NULL) {
        return Expr();
    }

    const IntImm *i = r->stride.as<IntImm>();
    if (i != NULL && i->value == 1) {
        return r->base;
    }

    return Expr();
}
}


string CodeGen_OpenCL_Dev::CodeGen_OpenCL_C::get_memory_space(const string &buf) {
    return "__address_space_" + print_name(buf);
}

void CodeGen_OpenCL_Dev::CodeGen_OpenCL_C::visit(const Call *op) {
    if (op->call_type != Call::Intrinsic) {
        CodeGen_C::visit(op);
        return;
    }
    if (op->name == Call::interleave_vectors) {
        int op_width = op->type.width;
        internal_assert(op->args.size() > 0);
        int arg_width = op->args[0].type().width;
        if (op->args.size() == 1) {
            // 1 argument, just do a simple assignment
            internal_assert(op_width == arg_width);
            print_assignment(op->type, print_expr(op->args[0]));
        } else if (op->args.size() == 2) {
            // 2 arguments, set the .even to the first arg and the
            // .odd to the second arg
            internal_assert(op->args[1].type().width == arg_width);
            internal_assert(op_width / 2 == arg_width);
            string a1 = print_expr(op->args[0]);
            string a2 = print_expr(op->args[1]);
            id = unique_name('_');
            do_indent();
            stream << print_type(op->type) << " " << id << ";\n";
            do_indent();
            stream << id << ".even = " << a1 << ";\n";
            do_indent();
            stream << id << ".odd = " << a2 << ";\n";
        } else {
            // 3+ arguments, interleave via a vector literal
            // selecting the appropriate elements of the args
            int dest_width = op->type.width;
            internal_assert(dest_width <= 16);
            int num_args = op->args.size();
            vector<string> arg_exprs(num_args);
            for (int i = 0; i < num_args; i++) {
                internal_assert(op->args[i].type().width == arg_width);
                arg_exprs[i] = print_expr(op->args[i]);
            }
            internal_assert(num_args * arg_width >= dest_width);
            id = unique_name('_');
            do_indent();
            stream << print_type(op->type) << " " << id;
            stream << " = (" << print_type(op->type) << ")(";
            for (int i = 0; i < dest_width; i++) {
                int arg = i % num_args;
                int arg_idx = i / num_args;
                internal_assert(arg_idx <= arg_width);
                stream << arg_exprs[arg] << ".s" << vector_elements[arg_idx];
                if (i != dest_width - 1) {
                    stream << ", ";
                }
            }
            stream << ");\n";
        }
    } else if (op->name == Call::image_load) {
        // image_load(<image name>, <buffer>, <x>, <x-extent>, <y>,
        // <y-extent>, <c>, <c-extent>)
        internal_assert(op->args.size() == 6 || op->args.size() == 8); // 2D and 3D or is it always normalized to 3D?
        // string_imm is the name of the image being read from
        const StringImm *string_imm = op->args[0].as<StringImm>();
        if (!string_imm) {
            internal_assert(op->args[0].as<Broadcast>());
            string_imm = op->args[0].as<Broadcast>()->value.as<StringImm>();
        }
        internal_assert(string_imm);
#if 0
        bool is_2d_array = op->args.size() == 6;
        string arg1 = print_expr(op->args[1]);
        string arg2 = print_expr(op->args[2]);
        string arg3 = is_2d_array ? print_expr(op->args[3]) : "";
        Type arg_type = op->args[1].type();
        vector<string> results;
        Type return_type(Type::TypeCode::Int, 32, 4);
        // If doing a vector read_image, flatten into a sequence of
        // read_image calls
        for (int i = 0; i < arg_type.width; i++) {
            string x = arg1;
            string y = arg2;
            string z = arg3;
            if (arg_type.is_vector()) {
                ostringstream x_i;
                x_i << arg1 << ".s" << i;
                x = print_assignment(arg_type.element_of(), x_i.str());
                ostringstream y_i;
                y_i << arg2 << ".s" << i;
                y = print_assignment(arg_type.element_of(), y_i.str());
                if (is_2d_array) {
                    ostringstream z_i;
                    z_i << arg3 << ".s" << i;
                    z = print_assignment(arg_type.element_of(), z_i.str());
                }
            }
            // Codegen the read_image call
            ostringstream rhs;
            rhs << "read_image";
            if (op->type.is_float()) {
                rhs << "f";
                return_type.code = Type::TypeCode::Float;
            } else if (op->type.is_int()) {
                rhs << "i";
                return_type.code = Type::TypeCode::Int;
            } else if (op->type.is_uint()) {
                rhs << "ui";
                return_type.code = Type::TypeCode::UInt;
            } else {
                internal_error << "Unexpected type for read_image\n";
            }
            rhs << "(" << print_name(string_imm->value) << ", sampler, ";
            if (is_2d_array) {
                rhs << "(int4)(" << x << ", " << y << ", " << z << ", 0)";
            } else {
                rhs << "(int2)(" << x << ", " << y << ")";
            }
            rhs << ")";
            print_assignment(return_type, rhs.str());
            // Get the first value (because it returns a vector)
            print_assignment(return_type.element_of(), id + ".x");
            results.push_back(id);
        }
        // Convert to the correct type if necessary
        if (return_type != op->type) {
            string operand = id;
            if (op->type.is_vector()) {
                internal_assert(op->type.width == (int)results.size());
                ostringstream operand_vector;
                operand_vector << "("
                               << print_type(return_type.vector_of(op->type.width))
                               << ")(";
                for (int i = 0; i < op->type.width; i++) {
                    operand_vector << results[i];
                    if (i < op->type.width - 1) {
                        operand_vector << ", ";
                    }
                }
                operand_vector << ")";
                id = operand_vector.str();
            }
            print_assignment(op->type, "convert_" + print_type(op->type) + "(" + id + ")");
        }
#endif
    } else if (op->name == Call::image_store) {
        // image_store(<image name>, <buffer>, <x>, <y>, <c>, <value>)
        internal_assert(op->args.size() == 5 || op->args.size() == 6); // 2D and 3D or is it always normalized to 3D?
        // string_imm is the name of the image being written to
        const StringImm *string_imm = op->args[0].as<StringImm>();
        if (!string_imm) {
            internal_assert(op->args[0].as<Broadcast>());
            string_imm = op->args[0].as<Broadcast>()->value.as<StringImm>();
        }
        internal_assert(string_imm);
#if 0
        bool is_2d_array = op->args.size() == 5;
        string arg1 = print_expr(op->args[1]);
        string arg2 = print_expr(op->args[2]);
        string arg3 = print_expr(op->args[3]);
        string arg4 = is_2d_array ? print_expr(op->args[4]) : arg3;
        Type arg_type = op->args[1].type();
        Type value_type = op->args.back().type();
        internal_assert(arg_type.width == value_type.width);
        // If doing a write_image with a vector, flatten into a
        // sequence of write_image calls
        for (int i = 0; i < arg_type.width; i++) {
            string x = arg1;
            string y = arg2;
            string z = arg3;
            string value = arg4;
            if (arg_type.is_vector()) {
                ostringstream x_i;
                x_i << arg1 << ".s" << i;
                x = print_assignment(arg_type.element_of(), x_i.str());
                ostringstream y_i;
                y_i << arg2 << ".s" << i;
                y = print_assignment(arg_type.element_of(), y_i.str());
                if (is_2d_array) {
                    ostringstream z_i;
                    z_i << arg3 << ".s" << i;
                    z = print_assignment(arg_type.element_of(), z_i.str());
                    ostringstream value_i;
                    value_i << arg4 << ".s" << i;
                    value = print_assignment(value_type.element_of(), value_i.str());
                    if (value_type.bits != 32) {
                        Type converted_value_type = value_type;
                        converted_value_type.bits = 32;
                        value = print_assignment(converted_value_type,
                                                 "convert_"
                                                 + print_type(converted_value_type)
                                                 + "(" + value + ")");
                    }
                } else {
                    ostringstream value_i;
                    value_i << arg3 << ".s" << i;
                    value = print_assignment(value_type.element_of(), value_i.str());
                }
            } else if (value_type.bits != 32) {
                Type converted_value_type = value_type;
                converted_value_type.bits = 32;
                value = print_assignment(converted_value_type,
                                         "convert_" + print_type(converted_value_type)
                                         + "(" + value + ")");
            }
            // Codegen the write_image call
            do_indent();
            stream << "write_image";
            Type color_type(Type::TypeCode::UInt, 32, 4);
            if (value_type.is_float()) {
                stream << "f";
                color_type.code = Type::TypeCode::Float;
            } else if (value_type.is_int()) {
                stream << "i";
                color_type.code = Type::TypeCode::Int;
            } else if (value_type.is_uint()) {
                stream << "ui";
                color_type.code = Type::TypeCode::UInt;
            } else {
                internal_error << "Unexpected type for write_image\n";
            }
            stream << "(" << print_name(string_imm->value) << ", ";
            if (is_2d_array) {
                stream << "(int4)(" << x << ", " << y << ", " << z << ", 0)";
            } else {
                stream << "(int2)(" << x << ", " << y << ")";
            }
            stream << ", (" << print_type(color_type) << ")(" << value << ", 0, 0, 0));\n";
        }
#endif
    } else {
        CodeGen_C::visit(op);
    }
}

void CodeGen_OpenCL_Dev::CodeGen_OpenCL_C::visit(const Load *op) {
    // If we're loading a contiguous ramp into a vector, use vload instead.
    Expr ramp_base = is_ramp1(op->index);
    if (ramp_base.defined()) {
        internal_assert(op->type.is_vector());
        string id_ramp_base = print_expr(ramp_base);

        ostringstream rhs;
        rhs << "vload" << op->type.width
            << "(0, (" << get_memory_space(op->name) << " "
            << print_type(op->type.element_of()) << "*)"
            << print_name(op->name) << " + " << id_ramp_base << ")";

        print_assignment(op->type, rhs.str());
        return;
    }

    string id_index = print_expr(op->index);

    // Get the rhs just for the cache.
    bool type_cast_needed = !(allocations.contains(op->name) &&
                              allocations.get(op->name).type == op->type);
    ostringstream rhs;
    if (type_cast_needed) {
        rhs << "((" << get_memory_space(op->name) << " "
            << print_type(op->type) << " *)"
            << print_name(op->name)
            << ")";
    } else {
        rhs << print_name(op->name);
    }
    rhs << "[" << id_index << "]";

    std::map<string, string>::iterator cached = cache.find(rhs.str());
    if (cached != cache.end()) {
        id = cached->second;
        return;
    }

    if (op->index.type().is_vector()) {
        // If index is a vector, gather vector elements.
        internal_assert(op->type.is_vector());

        id = "_" + unique_name('V');
        cache[rhs.str()] = id;

        do_indent();
        stream << print_type(op->type)
               << " " << id << ";\n";

        for (int i = 0; i < op->type.width; ++i) {
            do_indent();
            stream
                << id << ".s" << vector_elements[i]
                << " = ((" << get_memory_space(op->name) << " "
                << print_type(op->type.element_of()) << "*)"
                << print_name(op->name) << ")"
                << "[" << id_index << ".s" << vector_elements[i] << "];\n";
        }
    } else {
        print_assignment(op->type, rhs.str());
    }
}

void CodeGen_OpenCL_Dev::CodeGen_OpenCL_C::visit(const Store *op) {
    string id_value = print_expr(op->value);
    Type t = op->value.type();

    // If we're writing a contiguous ramp, use vstore instead.
    Expr ramp_base = is_ramp1(op->index);
    if (ramp_base.defined()) {
        internal_assert(op->value.type().is_vector());
        string id_ramp_base = print_expr(ramp_base);

        do_indent();
        stream << "vstore" << t.width << "("
               << id_value << ","
               << 0 << ", (" << get_memory_space(op->name) << " "
               << print_type(t.element_of()) << "*)"
               << print_name(op->name) << " + " << id_ramp_base
               << ");\n";

    } else if (op->index.type().is_vector()) {
        // If index is a vector, scatter vector elements.
        internal_assert(t.is_vector());

        string id_index = print_expr(op->index);

        for (int i = 0; i < t.width; ++i) {
            do_indent();
            stream << "((" << get_memory_space(op->name) << " "
                   << print_type(t.element_of()) << " *)"
                   << print_name(op->name)
                   << ")["
                   << id_index << ".s" << vector_elements[i] << "] = "
                   << id_value << ".s" << vector_elements[i] << ";\n";
        }
    } else {
        bool type_cast_needed = !(allocations.contains(op->name) &&
                                  allocations.get(op->name).type == t);

        string id_index = print_expr(op->index);
        string id_value = print_expr(op->value);
        do_indent();

        if (type_cast_needed) {
            stream << "(("
                   << get_memory_space(op->name) << " "
                   << print_type(t)
                   << " *)"
                   << print_name(op->name)
                   << ")";
        } else {
            stream << print_name(op->name);
        }
        stream << "[" << id_index << "] = "
               << id_value << ";\n";
    }

    cache.clear();
}

namespace {
// OpenCL doesn't support vectors of bool, so we re-write them to use
// signed integers. Binary operators produce a signed integer of the
// same width as the two input types. This function generates the "bool"
// vector type, given an operand type.
Type vec_bool_to_int(Type result_type, Type input_type) {
    if (result_type.is_vector() && result_type.bits == 1) {
        result_type.code = Type::Int;
        result_type.bits = input_type.bits;
    }
    return result_type;
}
}

void CodeGen_OpenCL_Dev::CodeGen_OpenCL_C::visit(const EQ *op) {
    visit_binop(vec_bool_to_int(op->type, op->a.type()), op->a, op->b, "==");
}

void CodeGen_OpenCL_Dev::CodeGen_OpenCL_C::visit(const NE *op) {
    visit_binop(vec_bool_to_int(op->type, op->a.type()), op->a, op->b, "!=");
}

void CodeGen_OpenCL_Dev::CodeGen_OpenCL_C::visit(const LT *op) {
    visit_binop(vec_bool_to_int(op->type, op->a.type()), op->a, op->b, "<");
}

void CodeGen_OpenCL_Dev::CodeGen_OpenCL_C::visit(const LE *op) {
    visit_binop(vec_bool_to_int(op->type, op->a.type()), op->a, op->b, "<=");
}

void CodeGen_OpenCL_Dev::CodeGen_OpenCL_C::visit(const GT *op) {
    visit_binop(vec_bool_to_int(op->type, op->a.type()), op->a, op->b, ">");
}

void CodeGen_OpenCL_Dev::CodeGen_OpenCL_C::visit(const GE *op) {
    visit_binop(vec_bool_to_int(op->type, op->a.type()), op->a, op->b, ">=");
}

void CodeGen_OpenCL_Dev::CodeGen_OpenCL_C::visit(const Cast *op) {
    if (op->type.is_vector()) {
        print_assignment(op->type, "convert_" + print_type(op->type) + "(" + print_expr(op->value) + ")");
    } else {
        CodeGen_C::visit(op);
    }
}

void CodeGen_OpenCL_Dev::CodeGen_OpenCL_C::visit(const Select *op) {
    if (op->condition.type().is_vector()) {
        string true_val = print_expr(op->true_value);
        string false_val = print_expr(op->false_value);
        string cond = print_expr(op->condition);

        // Yes, you read this right. OpenCL's select function is declared
        // 'select(false_case, true_case, condition)'.
        ostringstream rhs;
        rhs << "select(" << false_val << ", " << true_val << ", " << cond << ")";
        print_assignment(op->type, rhs.str());
    } else {
        CodeGen_C::visit(op);
    }
}

void CodeGen_OpenCL_Dev::CodeGen_OpenCL_C::visit(const Allocate *op) {
    user_assert(!op->new_expr.defined()) << "Allocate node inside OpenCL kernel has custom new expression.\n" <<
        "(Memoization is not supported inside GPU kernels at present.)\n";

    if (op->name == "__shared") {
        // Already handled
        op->body.accept(this);
    } else {
        open_scope();

        debug(2) << "Allocate " << op->name << " on device\n";

        debug(3) << "Pushing allocation called " << op->name << " onto the symbol table\n";

        // Allocation is not a shared memory allocation, just make a local declaration.
        // It must have a constant size.
        int32_t size;
        bool is_constant = constant_allocation_size(op->extents, op->name, size);
        user_assert(is_constant)
            << "Allocation " << op->name << " has a dynamic size. "
            << "Only fixed-size allocations are supported on the gpu. "
            << "Try storing into shared memory instead.";

        do_indent();
        stream << print_type(op->type) << ' '
               << print_name(op->name) << "[" << size << "];\n";
        do_indent();
        stream << "#define " << get_memory_space(op->name) << " __private\n";

        Allocation alloc;
        alloc.type = op->type;
        allocations.push(op->name, alloc);

        op->body.accept(this);

        // Should have been freed internally
        internal_assert(!allocations.contains(op->name));

        close_scope("alloc " + print_name(op->name));
    }
}

void CodeGen_OpenCL_Dev::CodeGen_OpenCL_C::visit(const Free *op) {
    if (op->name == "__shared") {
        return;
    } else {
        // Should have been freed internally
        internal_assert(allocations.contains(op->name));
        allocations.pop(op->name);
        do_indent();
        stream << "#undef " << get_memory_space(op->name) << "\n";
    }
}


void CodeGen_OpenCL_Dev::add_kernel(Stmt s,
                                    const string &name,
                                    const vector<GPU_Argument> &args) {
    debug(2) << "CodeGen_OpenCL_Dev::compile " << name << "\n";

    // TODO: do we have to uniquify these names, or can we trust that they are safe?
    cur_kernel_name = name;
    clc.add_kernel(s, name, args);
}

namespace {
struct BufferSize {
    string name;
    size_t size;

    BufferSize() : size(0) {}
    BufferSize(string name, size_t size) : name(name), size(size) {}

    bool operator < (const BufferSize &r) const {
        return size < r.size;
    }
};
}

void CodeGen_OpenCL_Dev::CodeGen_OpenCL_C::add_kernel(Stmt s,
                                                      const string &name,
                                                      const vector<GPU_Argument> &args) {

    debug(2) << "Adding OpenCL kernel " << name << "\n";

    debug(2) << "Eliminating bool vectors\n";
    s = eliminate_bool_vectors(s);
    debug(2) << "After eliminating bool vectors:\n" << s << "\n";

    // Figure out which arguments should be passed in __constant.
    // Such arguments should be:
    // - not written to,
    // - loads are block-uniform,
    // - constant size,
    // - and all allocations together should be less than the max constant
    //   buffer size given by CL_DEVICE_MAX_CONSTANT_BUFFER_SIZE.
    // The last condition is handled via the preprocessor in the kernel
    // declaration.
    vector<BufferSize> constants;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i].is_buffer &&
            CodeGen_GPU_Dev::is_buffer_constant(s, args[i].name) &&
            args[i].size > 0) {
            constants.push_back(BufferSize(args[i].name, args[i].size));
        }
    }

    // Sort the constant candidates from smallest to largest. This will put
    // as many of the constant allocations in __constant as possible.
    // Ideally, we would prioritize constant buffers by how frequently they
    // are accessed.
    sort(constants.begin(), constants.end());

    // Compute the cumulative sum of the constants.
    for (size_t i = 1; i < constants.size(); i++) {
        constants[i].size += constants[i - 1].size;
    }

    // Create preprocessor replacements for the address spaces of all our buffers.
    stream << "// Address spaces for " << name << "\n";
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i].is_buffer) {
            vector<BufferSize>::iterator constant = constants.begin();
            while (constant != constants.end() &&
                   constant->name != args[i].name) {
                constant++;
            }

            if (constant != constants.end()) {
                stream << "#if " << constant->size << " < MAX_CONSTANT_BUFFER_SIZE && "
                       << constant - constants.begin() << " < MAX_CONSTANT_ARGS\n";
                stream << "#define " << get_memory_space(args[i].name) << " __constant\n";
                stream << "#else\n";
                stream << "#define " << get_memory_space(args[i].name) << " __global\n";
                stream << "#endif\n";
            } else {
                stream << "#define " << get_memory_space(args[i].name) << " __global\n";
            }
        }
    }

    // Emit the function prototype
    stream << "__kernel void " << name << "(\n";
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i].is_buffer) {
            stream << " " << get_memory_space(args[i].name) << " ";
            if (!args[i].write) stream << "const ";
            stream << print_type(args[i].type) << " *"
                   << print_name(args[i].name);
            Allocation alloc;
            alloc.type = args[i].type;
            allocations.push(args[i].name, alloc);
        } else {
            stream << " const "
                   << print_type(args[i].type)
                   << " "
                   << print_name(args[i].name);
        }

        if (i < args.size()-1) stream << ",\n";
    }
    stream << ",\n" << " __address_space___shared int16* __shared";

    stream << ")\n";

    open_scope();
    print(s);
    close_scope("kernel " + name);

    for (size_t i = 0; i < args.size(); i++) {
        // Remove buffer arguments from allocation scope
        if (args[i].is_buffer) {
            allocations.pop(args[i].name);
        }
    }

    // Undef all the buffer address spaces, in case they're different in another kernel.
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i].is_buffer) {
            stream << "#undef " << get_memory_space(args[i].name) << "\n";
        }
    }
}

void CodeGen_OpenCL_Dev::init_module() {
    debug(2) << "OpenCL device codegen init_module\n";

    // wipe the internal kernel source
    src_stream.str("");
    src_stream.clear();

    // This identifies the program as OpenCL C (as opposed to SPIR).
    src_stream << "/*OpenCL C*/\n";

    src_stream << "#pragma OPENCL FP_CONTRACT ON\n";

    // Write out the Halide math functions.
    src_stream << "float float_from_bits(unsigned int x) {return as_float(x);}\n"
               << "float nan_f32() { return NAN; }\n"
               << "float neg_inf_f32() { return -INFINITY; }\n"
               << "float inf_f32() { return INFINITY; }\n"
               << "#define sqrt_f32 sqrt \n"
               << "#define sin_f32 sin \n"
               << "#define cos_f32 cos \n"
               << "#define exp_f32 exp \n"
               << "#define log_f32 log \n"
               << "#define abs_f32 fabs \n"
               << "#define floor_f32 floor \n"
               << "#define ceil_f32 ceil \n"
               << "#define round_f32 round \n"
               << "#define trunc_f32 trunc \n"
               << "#define pow_f32 pow\n"
               << "#define asin_f32 asin \n"
               << "#define acos_f32 acos \n"
               << "#define tan_f32 tan \n"
               << "#define atan_f32 atan \n"
               << "#define atan2_f32 atan2\n"
               << "#define sinh_f32 sinh \n"
               << "#define asinh_f32 asinh \n"
               << "#define cosh_f32 cosh \n"
               << "#define acosh_f32 acosh \n"
               << "#define tanh_f32 tanh \n"
               << "#define atanh_f32 atanh \n"
               << "#define fast_inverse_f32 native_recip \n"
               << "#define fast_inverse_sqrt_f32 native_rsqrt \n"
               << "int halide_gpu_thread_barrier() {\n"
               << "  barrier(CLK_LOCAL_MEM_FENCE);\n" // Halide only ever needs local memory fences.
               << "  return 0;\n"
               << "}\n";

    // __shared always has address space __local.
    src_stream << "#define __address_space___shared __local\n";

    if (target.has_feature(Target::CLDoubles)) {
        src_stream << "#pragma OPENCL EXTENSION cl_khr_fp64 : enable\n"
                   << "bool is_nan_f64(double x) {return x != x; }\n"
                   << "#define sqrt_f64 sqrt\n"
                   << "#define sin_f64 sin\n"
                   << "#define cos_f64 cos\n"
                   << "#define exp_f64 exp\n"
                   << "#define log_f64 log\n"
                   << "#define abs_f64 fabs\n"
                   << "#define floor_f64 floor\n"
                   << "#define ceil_f64 ceil\n"
                   << "#define round_f64 round\n"
                   << "#define trunc_f64 trunc\n"
                   << "#define pow_f64 pow\n"
                   << "#define asin_f64 asin\n"
                   << "#define acos_f64 acos\n"
                   << "#define tan_f64 tan\n"
                   << "#define atan_f64 atan\n"
                   << "#define atan2_f64 atan2\n"
                   << "#define sinh_f64 sinh\n"
                   << "#define asinh_f64 asinh\n"
                   << "#define cosh_f64 cosh\n"
                   << "#define acosh_f64 acosh\n"
                   << "#define tanh_f64 tanh\n"
                   << "#define atanh_f64 atanh\n";
    }

    src_stream << '\n';

    // Add at least one kernel to avoid errors on some implementations for functions
    // without any GPU schedules.
    src_stream << "__kernel void _at_least_one_kernel(int x) { }\n";

    cur_kernel_name = "";
}

vector<char> CodeGen_OpenCL_Dev::compile_to_src() {
    string str = src_stream.str();
    debug(1) << "OpenCL kernel:\n" << str << "\n";
    vector<char> buffer(str.begin(), str.end());
    buffer.push_back(0);
    return buffer;
}

string CodeGen_OpenCL_Dev::get_current_kernel_name() {
    return cur_kernel_name;
}

void CodeGen_OpenCL_Dev::dump() {
    std::cerr << src_stream.str() << std::endl;
}

std::string CodeGen_OpenCL_Dev::print_gpu_name(const std::string &name) {
    return name;
}

}}
