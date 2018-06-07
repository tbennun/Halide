#include "CopyElision.h"
#include "IREquality.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "Substitute.h"
#include "Func.h"
#include "FindCalls.h"

namespace Halide {
namespace Internal {

using std::map;
using std::set;
using std::string;
using std::vector;

typedef map<FunctionPtr, FunctionPtr> SubstitutionMap;

namespace {

string print_function(const Function &f) {
    std::ostringstream stream;
    stream << f.name() << "(";
    for (int i = 0; i < f.dimensions(); ++i) {
        stream << f.args()[i];
        if (i != f.dimensions()-1) {
            stream << ", ";
        }
    }
    stream << ") = ";
    if (f.values().size() > 1) {
        stream << "{";
    }
    for (int i = 0; i < (int)f.values().size(); ++i) {
        stream << f.values()[i];
        if (i != (int)f.values().size()-1) {
            stream << ", ";
        }
    }
    if (f.values().size() > 1) {
        stream << "}";
    }
    return stream.str();
}

struct CopyPair {
    string prod; // Copy from
    string cons; // Store into
};

/** If function 'f' operation only involves pointwise copy from another
  * function, return the name of the function from which it copies from.
  * If the function being copied from is a tuple, we have to ensure that 'f'
  * copies the whole tuple and not only some of the tuple values; otherwise,
  * treat it as non pointwise copies. For non pointwise copy or if 'f' has
  * update definitions or is an extern function, return an empty string.
  */
string get_pointwise_copy_producer(const Function &f) {
    if (f.has_update_definition() || f.has_extern_definition()) {
        return "";
    }

    string prod;
    for (int i = 0; i < (int)f.values().size(); ++i) {
        const Expr &val = f.values()[i];
        if (const Call *call = val.as<Call>()) {
            if (call->call_type == Call::Halide) {
                // Check if it is a pointwise copy. For tuple, check if 'f'
                // copies the whole tuple values.
                if (!prod.empty() && (prod != call->name)) {
                    debug(0) << "...Function \"" << f.name() << "\" calls multiple "
                             << "functions: \"" << call->name << "\" and \""
                             << prod << "\"\n";
                    return "";
                }
                prod = call->name;

                Function prod_f = Function(call->func);
                if (f.dimensions() != prod_f.dimensions()) {
                    debug(0) << "...Function \"" << f.name() << "\" does not call "
                             << "the whole tuple values of function \""
                             << prod_f.name() << "\"\n";
                    return "";
                }

                if (i != call->value_index) {
                    debug(0) << "...Function \"" << f.name() << "\" calls "
                             << prod_f.name() << "[" << call->value_index
                             << "] at value index " << i << "\n";
                    return "";
                }

                for (int j = 0; j < f.dimensions(); ++j) {
                    // Check if the call args are equivalent for both the
                    // RHS ('f') and LHS ('prod_f').
                    // TODO(psuriana): Handle case for copy with some index shifting
                    if (!equal(f.args()[j], prod_f.args()[j])) {
                        debug(0) << "At arg " << j << ", " << f.name() << "("
                                 << f.args()[i] << ") != " << prod_f.name()
                                 << "[" << call->value_index << "]("
                                 << prod_f.args()[j] << ")\n";
                    return "";
                    }
                }
            }
        } else if (!prod.empty()) {
            debug(0) << "...Function \"" << f.name() << "\" does not call "
                     << "the whole tuple values of function \""
                     << prod << "\" or is not a simple copy\n";
            return "";
        }
    }

    if (!prod.empty()) {
        debug(0) << "...Find pointwise copy -> " << print_function(f) << "\n";
    }
    return prod;
}

/** Return all pairs of functions which operation only involves pointwise copy
  * of another function and the function from which it copies from. Ignore
  * functions that have updates or are extern functions. */
vector<CopyPair> get_pointwise_copies(const map<string, Function> &env) {
    vector<CopyPair> pointwise_copies;
    for (const auto &iter : env) {
        string copied_from = get_pointwise_copy_producer(iter.second);
        if (!copied_from.empty()) {
            pointwise_copies.push_back({copied_from, iter.first});
        }
    }
    return pointwise_copies;
}

class FindCallers : public IRVisitor {
public:
    FindCallers(const string &f) : func(f) {}
    set<string> callers;
private:
    const string &func;

    using IRVisitor::visit;

    void visit(const Call *op) {
        if (op->name == func) {
        }
        IRVisitor::visit(op);
    }
};

} // anonymous namespace

Stmt copy_elision(Stmt s,
                  const vector<string> &order,
                  const map<string, Function> &env) {

    vector<CopyPair> copy_pairs = get_pointwise_copies(env);
    return s;
}

map<string, Function> elide_copy_calls(const map<string, Function> &env) {
    map<string, Function> elided_env;
    for (const auto &iter : env) {
        elided_env.emplace(iter.first, iter.second);
    }

    vector<CopyPair> copy_pairs = get_pointwise_copies(env);

    // TODO(psuriana): Check no duplicates in copy pairs

    for (const auto &cp : copy_pairs) {
        // TODO(psuriana): Need to ensure that the copy elision is actually a
        // valid thing to do
        Function f_prod = env.at(cp.prod);
        Function f_cons = env.at(cp.cons);
        debug(0) << ".....RENAMING producer store: " << cp.prod << " -> " << cp.cons << "\n";
        //f_prod.set_name(cp.cons);
    }

    return elided_env;
}

void copy_elision_test() {
    Func tile("tile"), output("output"), f("f"), g("g"), h("h"), in("in");
    Var x("x"), y("y");

    f(x, y) = x + y;
    g(x, y) = x - y;
    h(x, y) = g(x, y);
    in(x, y) = h(x, y);
    tile(x, y) = {f(x, y), g(x, y)};
    output(x, y) = tile(y, x);

    map<string, Function> env;
    env.emplace(tile.name(), tile.function());
    env.emplace(output.name(), output.function());
    env.emplace(f.name(), f.function());
    env.emplace(g.name(), g.function());
    env.emplace(h.name(), h.function());
    env.emplace(in.name(), in.function());

    vector<CopyPair> result = get_pointwise_copies(env);
    debug(0) << "\nPointwise copies:\n";
    for (const auto &p : result) {
        debug(0) << "\t" << "prod: " << print_function(env.at(p.prod)) << "\t\tcons: " << print_function(env.at(p.cons)) << "\n";
    }
    debug(0) << "\n";

    map<string, Function> new_env = elide_copy_calls(env);
    debug(0) << "\n\nNEW ENV:\n";
    for (const auto &iter : new_env) {
        debug(0) << "\t" << print_function(iter.second) << "\n";
    }
    debug(0) << "\n";

    std::cout << "Copy elision test passed" << std::endl;
}

}  // namespace Internal
}  // namespace Halide
