#include "FactorRVars.h"

namespace Halide{
namespace Internal {

using std::map;
using std::string;
using std::vector;

namespace {

// Replace the definition args/values/rdom with the last definition
// in its rfactors.
void replace_with_rfactor(Definition &def) {
    /*const vector<Definition> &rfactors = def.rfactors();
    if (!rfactors.empty()) {
        const Definition &replacement = rfactors.back();
        def.args() = replacement.args();
        def.values() = replacement.values();
        def.set_domain(replacement.domain());
        // Change reference to the reduction domain in the schedule to the new
        // reduction domain
        def.schedule().set_reduction_domain(replacement.domain());
    }

    for (Specialization &s : def.specializations()) {
        replace_with_rfactor(s.definition);
    }*/
}


} // anonymous namespace

void factor_rvars(map<string, Function> &env) {
    for (auto &iter : env) {
        debug(0) << "Replacing Func " <<  iter.first << " with its rfactors\n";
        replace_with_rfactor(iter.second.definition());
        /*for (Definition &update : iter.second.updates()) {
            replace_with_rfactor(update);
        }*/
    }
}

}
}