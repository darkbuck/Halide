#include "JavaGlueGen.h"

#include "Module.h"
#include "Util.h"

namespace Halide {
namespace Internal {
    
namespace {

std::string lowered_arg_to_java_arg(const Argument &arg) {
    std::string result;

    if (arg.kind == Argument::InputScalar) {
        // TODO: Some sort of translation?
        user_assert(!arg.type.is_uint()) << "Unsigned argument types are not supported in Java.\n";
        // TODO: Likey need some way to pass through arguments to callbacks, user context, etc.
        user_assert(!arg.type.is_handle()) << "Handle argument types are not supported in Java.\n";

        if (arg.type.is_int()) {
            switch (arg.type.bits()) {
            case 1:
                result = "boolean";
                break;
            case 8:
                result = "byte";
                break;
            case 16:
                result = "short";
                break;
            case 32:
                result = "int";
                break;
            case 64:
                result = "long";
                break;
            }
        } else {
            internal_assert(arg.type.is_float()) << "Expected only remaining type to be floating-point here.\n";
            switch (arg.type.bits()) {
            case 32:
                result = "float";
                break;
            case 64:
                result = "double";
                break;
            }
        }
    } else {
        result = "Buffer ";
    }

    result += arg.name;
    return result;
}

std::string lowered_arg_to_jni_arg(const Argument &arg) {
    std::string result;

    if (arg.kind == Argument::InputScalar) {
        // TODO: Some sort of translation?
        user_assert(!arg.type.is_uint()) << "Unsigned argument types are not supported in Java.\n";
        // TODO: Likey need some way to pass through arguments to callbacks, user context, etc.
        user_assert(!arg.type.is_handle()) << "Handle argument types are not supported in Java.\n";

        if (arg.type.is_int()) {
            switch (arg.type.bits()) {
            case 1:
                result = "jboolean";
                break;
            case 8:
                result = "jbyte";
                break;
            case 16:
                result = "jshort";
                break;
            case 32:
                result = "jint";
                break;
            case 64:
                result = "jlong";
                break;
            }
        } else {
            internal_assert(arg.type.is_float()) << "Expected only remaining type to be floating-point here.\n";
            switch (arg.type.bits()) {
            case 32:
                result = "jfloat";
                break;
            case 64:
                result = "jdouble";
                break;
            }
           
        }
    } else {
        result = "jobject ";
    }

    result += arg.name;
    return result;
}

}

JNIGlueGen::JNIGlueGen(std::ostream &dest, const std::string &header_name) : dest(dest), header_name(header_name) {
}

void JNIGlueGen::compile(const Module &module) {
    dest << "#include <jni.h>\n\n";
    dest << "#include \"" << header_name << "\"\n\n";
    dest << "extern \"C\" {\n";
    for (const auto &f : module.functions()) {
        if (f.linkage == LoweredFunc::Internal) {
            continue;
        }

        std::vector<std::string> namespaces;
        std::string base_fn_name = extract_namespaces(f.name, namespaces);
        
        std::string jni_name = "Java_com_";
        for (const auto &n : namespaces) {
            jni_name += n + "_";
        }
        jni_name += base_fn_name;

        size_t buffer_args = 0;
        // TODO: Decide it we want to throw an exception instead of returning the int32_t result code
        dest << "JNIEXPORT int JNICALL " << jni_name << "(";
        const char *prefix = "";
        for (const auto &arg : f.args) {
            dest << prefix << lowered_arg_to_jni_arg(arg);
            prefix = ", ";
            if (arg.kind == Argument::InputBuffer || arg.kind == Argument::OutputBuffer) {
                buffer_args += 1;
            }
        }
        dest << ") {\n";

        size_t buf_index = 0;
        if (buffer_args != 0) {
            dest << "    Halide::Runtime::Buffer<> _buf_args[" << buffer_args << "]\n\n;";

            for (const auto &arg : f.args) {
              if (arg.kind == Argument::InputBuffer || arg.kind == Argument::OutputBuffer) {
                    dest << "    _buf_args[" << buf_index << "] = java_buffer_to_halide_buffer_t(" << arg.name << ");\n";
                    buf_index += 1;
                }
            }
            dest << "\n";
        }

        buf_index = 0;
        prefix = "";
        dest << "int _result = " << f.name << "(";
        for (const auto &arg : f.args) {
            if (arg.kind == Argument::InputScalar) {
                dest << prefix << arg.name;
            } else {
                dest << prefix << "_buf_args[" << buf_index << "].raw_buffer()";
                buf_index += 1;
            }
            prefix = ", ";
        }
        dest << ");\n\n";

        dest << "    return _result;\n";

        dest << "}\n";
    }
    dest << "}\n";
}

JavaGlueGen::JavaGlueGen(std::ostream &dest) : dest(dest) {
}

void JavaGlueGen::compile(const Module &module) {
    std::vector<std::string> namespaces;
    std::string base_fn_name;
    std::string first_name;

    for (const auto &f : module.functions()) {
        if (f.linkage == LoweredFunc::Internal) {
            continue;
        }
        first_name = f.name;
        base_fn_name = extract_namespaces(f.name, namespaces);
        break;
    }
    user_assert(!base_fn_name.empty()) << "Generating Java glue requires module to have at least one exported Func.\n";
    user_assert(!namespaces.empty()) << "Generating Java glue requires at least one namespace for the class name on all exported Funcs.\n";

    std::string class_name = namespaces.back();
    namespaces.pop_back();

    // TODO: Should this be allowed? I believe Java allows top level classes but it us a bad idea to use them.
    if (!namespaces.empty()) {
        dest << "package ";
        const char *prefix = "";
        for (const auto n : namespaces) {
            dest << prefix << n;
            prefix = ".";
        }
        dest << ";\n\n";
    }

    // TODO: Is this the right name for this?
    // TODO: Do we need any other names?
    dest << "import com.halide.runtime.Buffer;\n";

    std::map<std::string, std::vector<LoweredFunc>> class_contents;

    for (const auto &f : module.functions()) {
        if (f.linkage == LoweredFunc::Internal) {
            continue;
        }

        std::vector<std::string> this_fn_namespaces;
        std::string this_fn_name = extract_namespaces(f.name, this_fn_namespaces);

        user_assert(!this_fn_namespaces.empty()) << "Generating Java glue requires at least one namespace for the class name on all exported Funcs.\n";

        std::string this_class = this_fn_namespaces.back();
        this_fn_namespaces.pop_back();

        if (this_fn_namespaces != namespaces) {
            user_error << "Generating Java glue requires all exported functions be in the same package:\n"
                       << "    " << first_name << "\n"
                       << "is not in the same package as:\n"
                       << "    " << f.name << "\n";
        }

        class_contents[this_class].push_back(f);
    }

    for (const auto &class_pair : class_contents) {
        dest << "\npublic class " << class_pair.first << " {\n";

        for (const auto &f : class_pair.second) {
            std::vector<std::string> this_fn_namespaces;
            std::string this_fn_name = extract_namespaces(f.name, this_fn_namespaces);

            // TODO: This returns the error code, but the wrapper should probably throw instead.
            dest << "    public native int " << this_fn_name << "(";
            const char *prefix = "";
            for (const auto &arg : f.args) {
                dest << prefix << lowered_arg_to_java_arg(arg);
                prefix = ", ";
            }
            dest << ");\n";
        }

        dest << "};\n";
    }
}

}
}
