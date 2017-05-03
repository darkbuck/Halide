#ifndef HALIDE_JAVA_GLUE_GEN_H_
#define HALIDE_JAVA_GLUE_GEN_H_

#include <iostream>
#include <string>

namespace Halide {

class Module;
  
namespace Internal {

class JNIGlueGen {
    std::ostream &dest;
    std::string header_name;

public:
    JNIGlueGen(std::ostream &dest, const std::string &header_name);

    void compile(const Module &module);
};

class JavaGlueGen {
    std::ostream &dest;

 public:
    JavaGlueGen(std::ostream &dest);

    void compile(const Module &module);
};

}
}

#endif // HALIDE_JAVA_GLUE_GEN_H_
