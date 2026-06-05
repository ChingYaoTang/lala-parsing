// Copyright 2025 Yi-Nung Tsao

/**
 * SMT-LIB parser variant used by parser-only statistics tools.
 *
 * The parser logic is shared between normal parsing and stats collection. The
 * collect_stats switch only controls whether semantic actions update counters.
 */

#ifndef LALA_PARSING_SMT_PARSER_STAT_HPP
#define LALA_PARSING_SMT_PARSER_STAT_HPP

#include "peglib.h"
#include <any>
#include <cassert>
// #include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "battery/shared_ptr.hpp"
#include "lala/logic/ast.hpp"
#include "flatzinc_parser.hpp"

namespace lala {

struct SMTParseStats {
  std::string theory;
  std::string expected_status;

  size_t var_total = 0;
  size_t var_bool = 0;
  size_t var_int = 0;
  size_t var_real = 0;

  size_t cmd_assert = 0;
  size_t cmd_declare_fun = 0;
  size_t cmd_declare_const = 0;
  size_t cmd_define_fun = 0;

  size_t op_le = 0;
  size_t op_ge = 0;
  size_t op_eq = 0;
  size_t op_gt = 0;
  size_t op_lt = 0;
  size_t op_and = 0;
  size_t op_or = 0;
  size_t op_not = 0;
  size_t op_imply = 0;
  size_t op_xor = 0;
  size_t op_add = 0;
  size_t op_sub = 0;
  size_t op_mul = 0;
  size_t op_div = 0;
  size_t op_ite = 0;
  size_t op_let = 0;
  size_t op_distinct = 0;
  size_t op_fun_application = 0;
};

template <class Allocator>
struct SMTParseResult {
  TFormula<Allocator> formula;
  bool success = false;
  std::string diagnostic;
  SMTParseStats stats;
};

namespace impl {

template <class Allocator>
class SMTParser {
  using allocator_type = Allocator;
  using F = TFormula<allocator_type>;
  using SV = peg::SemanticValues;
  using So = Sort<allocator_type>;
  using FSeq = typename F::Sequence;

  struct DefinedFunction {
    std::vector<std::pair<std::string, So>> params;
    So return_sort;
    F body;
  };

  std::map<std::string, So> declared_symbols; // Symbol name -> sort.
  std::map<std::string, DefinedFunction> defined_functions; // Function name -> signature and body.
  std::map<std::string, So> active_function_parameters; // Parameters currently in scope while parsing a function body.
  bool is_nnv;
  bool error;   // If an error was found during parsing.
  bool silent;  // If we do not want to output error messages.
  bool collect_stats;
  std::string first_diagnostic;
  SMTParseStats stats;

  SolverOutput<Allocator>& output;

 public:
  SMTParser(SolverOutput<Allocator>& output, bool is_nnv)
    : is_nnv(is_nnv), error(false), silent(false), collect_stats(true), output(output) {}

  F parse(const std::string& input) {
    return parse_with_status(input, false).formula;
  }

  SMTParseResult<Allocator> parse_with_status(const std::string& input, bool should_collect_stats = true) {
    error = false;
    collect_stats = should_collect_stats;
    first_diagnostic.clear();
    stats = SMTParseStats{};
    // const auto parse_start = std::chrono::steady_clock::now();
    // const auto report_elapsed = [&parse_start]() {
    //   const auto elapsed = std::chrono::duration<double>(
    //      std::chrono::steady_clock::now() - parse_start);
    //   std::cerr << "SMTParser::parse() took " << elapsed.count() << " s" << std::endl;
    // };
    peg::parser parser(R"(
        Statements     <- (StatusInfo / SetLogic / DeclareConst / DeclareFun / DefineFun / Assertion / Comment)+

        Integer        <- < [+-]?[0-9]+ >
        Real           <- < ('inf' / '-inf' /
                            [+-]?[0-9]+ (('.' (&'..' / !'.') [0-9]*) /
                            ([Ee][+-]?[0-9]+)) ) >
        Boolean        <- < 'true' / 'false' >
        Literal        <- Real / Boolean / Integer

        SimpleSymbol   <- < [a-zA-Z_?~!$%^&*+=<>/-][a-zA-Z0-9_?~!$%^&*+=<>/-@.]* >
        QuotedSymbol   <- < '|' (!'|' .)* '|' >
        Symbol         <- QuotedSymbol / SimpleSymbol
        Identifier     <- Symbol
        
        Sort           <- < 'Real' / 'Bool' / 'Int' >
        SortedVars     <- '(' ( '(' Symbol Sort ')' )* ')'

        Status         <- < 'sat' / 'unsat' / 'unknown' >
        LogicName      <- < [a-zA-Z0-9_+-]+ >
        StatusInfo     <- '(' 'set-info' ':status' Status ')'
        SetLogic       <- '(' 'set-logic' LogicName ')'

        DeclareConst   <- '(' 'declare-const' Symbol Sort ')'
        DeclareFun     <- '(' 'declare-fun' Symbol '(' ')' Sort ')'
        DefineFun      <- '(' 'define-fun' Symbol SortedVars Sort Term ')'

        BinaryOp       <- < '<=' / '>=' / '=' / '>' / '<' >
        LogicOp        <- < 'and' / 'or' / 'not' / '=>' / 'xor' >
        ArithOp        <- < '+' / '-' / '*' / '/' >

        Arith          <- '(' ArithOp Term+ ')'
        Ite            <- '(' 'ite' Term Term Term ')'
        FunApplication <- '(' Symbol Term+ ')' 
        
        Let            <- '(' 'let' '(' ('(' Symbol Term ')')+ ')' Term ')'
        Distinct       <- '(' 'distinct' Term Term+ ')'
        Bound          <- '(' BinaryOp Term Term ')'
        Constraint     <- '(' LogicOp Term+ ')'
        Term           <- Let / Distinct / Constraint / Bound / Ite / Arith / Literal / FunApplication / Identifier
        Assertion      <- '(' 'assert' Term ')'
        
        IgnoredAtom    <- < [^() \n\r\t]+ >
        IgnoredQuoted  <- '"' ( '""' / !'"' . )* '"'
        IgnoredBar     <- '|' (!'|' .)* '|'
        IgnoredSExpr   <- IgnoredQuoted / IgnoredBar / IgnoredAtom / '(' IgnoredSExpr* ')'
        IgnoredCmd     <- '(' ('set-info' / 'set-logic' / 'check-sat' / 'exit') IgnoredSExpr* ')'
        
        ~Comment       <- ';' [^\n\r]* [ \n\r\t]* / IgnoredCmd
        %whitespace    <- [ \n\r\t]*
      )");
    assert(static_cast<bool>(parser) == true);

    parser["Statements"] = [this](const SV& sv) { return make_statements(sv); };
    // parser["Integer"] = [](const SV& sv) { return F::make_z(sv.token_to_number<logic_int>()); };
    parser["Integer"] = [](const SV& sv) { return F::make_real(impl::string_to_real(sv.token_to_string())); };
    parser["Real"] = [](const SV& sv) { return F::make_real(impl::string_to_real(sv.token_to_string())); };
    parser["Boolean"] = [](const SV& sv) { return sv.token_to_string() == "true" ? F::make_true() : F::make_false(); };
    parser["SimpleSymbol"] = [](const SV& sv) { return sv.token_to_string(); };
    parser["QuotedSymbol"] = [](const SV& sv) { return sv.token_to_string(); };
    parser["Identifier"] = [this](const SV& sv) { return make_identifier(sv); };
    parser["BinaryOp"] = [](const SV& sv) { return sv.token_to_string(); };
    parser["LogicOp"] = [](const SV& sv) { return sv.token_to_string(); };
    parser["ArithOp"] = [](const SV& sv) { return sv.token_to_string(); };
    parser["Sort"] = [](const SV& sv) { return sv.token_to_string(); };
    parser["Status"] = [](const SV& sv) { return sv.token_to_string(); };
    parser["LogicName"] = [](const SV& sv) { return sv.token_to_string(); };
    parser["StatusInfo"] = [this](const SV& sv) { return make_status_info(sv); };
    parser["SetLogic"] = [this](const SV& sv) { return make_set_logic(sv); };
    parser["DeclareConst"] = [this](const SV& sv) { return make_declare_const(sv); };
    parser["DeclareFun"] = [this](const SV& sv) { return make_declare_fun(sv); };
    parser["SortedVars"] = [this](const SV& sv) { return make_sorted_vars(sv); };
    parser["DefineFun"] = [this](const SV& sv) { return make_define_fun(sv); };
    parser["Let"] = [this](const SV& sv) { return make_let(sv); };
    parser["Distinct"] = [this](const SV& sv) { return make_distinct(sv); };
    parser["Ite"] = [this](const SV& sv) { return make_ite(sv); };
    parser["Arith"] = [this](const SV& sv) { return make_arith(sv); };
    parser["FunApplication"] = [this](const SV& sv) { return make_fun_application(sv); };
    parser["Bound"] = [this](const SV& sv) { return make_bound(sv); };
    parser["Constraint"] = [this](const SV& sv) { return make_constraint(sv); };
    parser["Assertion"] = [this](const SV& sv) { return make_assertion(sv); };

    F smt_formulas;
    SMTParseResult<Allocator> result;
    if (parser.parse(input.c_str(), smt_formulas) && !error) {
      // report_elapsed();
      result.formula = std::move(smt_formulas);
      result.success = true;
    }
    else {
      // report_elapsed();
      if (first_diagnostic.empty()) {
        first_diagnostic = "SMT parsing is failed.";
      }
      std::cerr << "SMT parsing is failed." << std::endl;
      result.formula = F::make_false();
      result.success = false;
      result.diagnostic = first_diagnostic;
    }
    result.stats = stats;
    return result;
  }

 private:
  static F f(const std::any& any) { return std::any_cast<F>(any); }

  F make_error(const SV& sv, const std::string& msg) {
    std::string diagnostic = std::to_string(sv.line_info().first) + ":" +
      std::to_string(sv.line_info().second) + ":" + msg;
    if (!silent) {
      std::cerr << diagnostic << std::endl;
    }
    if (first_diagnostic.empty()) {
      first_diagnostic = diagnostic;
    }
    error = true;

    return F::make_false();
  }

  So get_sort_type(const std::string& sort_name) const {
    if (sort_name == "Int") {
      return So(So::Int);
    }
    if (sort_name == "Real") {
      return So(So::Real);
    }
    assert(sort_name == "Bool");
    return So(So::Bool);
  }

  void count_stat(size_t& counter) {
    if (collect_stats) {
      counter++;
    }
  }

  F make_status_info(const SV& sv) {
    if (collect_stats) {
      stats.expected_status = std::any_cast<std::string>(sv[0]);
    }
    return F::make_true();
  }

  F make_set_logic(const SV& sv) {
    if (collect_stats) {
      stats.theory = std::any_cast<std::string>(sv[0]);
    }
    return F::make_true();
  }

  F make_declare_const(const SV& sv) {
    count_stat(stats.cmd_declare_const);
    return make_variable_decl(sv);
  }

  F make_declare_fun(const SV& sv) {
    count_stat(stats.cmd_declare_fun);
    return make_variable_decl(sv);
  }

  void count_declared_variable(const So& sort) {
    if (!collect_stats) {
      return;
    }
    stats.var_total++;
    if (sort.is_bool()) {
      stats.var_bool++;
    }
    else if (sort.is_int()) {
      stats.var_int++;
    }
    else if (sort.is_real()) {
      stats.var_real++;
    }
  }

  F make_variable_decl(const SV& sv) {
    if(is_nnv) {
      return F::make_true();
    }
    else {
      // Refer to make_parameter_decl(), make_existential(), and make_variable_decl() in flatzinc_parser.hpp
      // for the implementation of variable declaration.

      // Expected semantic values: [Symbol, Sort].
      std::string name = std::any_cast<std::string>(sv[0]);
      // Check if the variable name is already used by a declared symbol or a defined function.
      if (declared_symbols.contains(name) || defined_functions.contains(name)) {
        return make_error(sv, "Symbol `" + name + "` already declared.");
      }

      std::string sort_name = std::any_cast<std::string>(sv[1]);
      // Get the corresponding sort type object
      So var_sort = get_sort_type(sort_name);
      
      declared_symbols.emplace(name, var_sort);
      output.add_var(name);
      count_declared_variable(var_sort);
      return F::make_exists(UNTYPED, LVar<allocator_type>(name), std::move(var_sort));
    }
  }

  std::vector<std::pair<std::string, So>> make_sorted_vars(const SV& sv) {
    // Expected semantic values: [Symbol, Sort, Symbol, Sort, ...].
    // Each adjacent pair describes one function parameter from the SMT sorted-var list.
    std::vector<std::pair<std::string, So>> params;
    active_function_parameters.clear();

    for (size_t i = 0; i < sv.size(); i += 2) {
      auto var_name = std::any_cast<std::string>(sv[i]);
      auto sort_name = std::any_cast<std::string>(sv[i + 1]);

      if (active_function_parameters.contains(var_name)) {
        // SMT-LIB does not explicitly require define-fun parameters to be distinct.
        // This parser rejects duplicates because its substitution map is keyed by
        // parameter name and cannot represent shadowed parameters correctly.
        make_error(sv, "Duplicate function parameter `" + var_name + "`.");
        return {};
      }

      So var_sort = get_sort_type(sort_name);
      active_function_parameters.emplace(var_name, var_sort);
      params.emplace_back(std::move(var_name), std::move(var_sort));
    }
    return params;
  }

  F make_define_fun(const SV& sv) {
    count_stat(stats.cmd_define_fun);

    // Expected semantic values:
    // [Symbol(function name), SortedVarList(params), Sort(return sort), Term(body)].
    // The grammar has already parsed the parameter list before the body, so
    // active_function_parameters was available while the body was being built.
    auto fun_name = std::any_cast<std::string>(sv[0]);
    auto params = std::any_cast<std::vector<std::pair<std::string, So>>>(sv[1]);
    auto return_sort = get_sort_type(std::any_cast<std::string>(sv[2]));
    F body = f(sv[3]);

    // The parameter scope is only needed while parsing the body.
    active_function_parameters.clear();

    if (declared_symbols.contains(fun_name) || defined_functions.contains(fun_name)) {
      return make_error(sv, "Symbol `" + fun_name + "` already declared.");
    }

    // A define-fun command does not add an assertion directly in this parser.
    // It is stored as a named macro-like definition and expand applications in make_apply_fun().
    defined_functions.emplace(fun_name, DefinedFunction{
      std::move(params),
      std::move(return_sort),
      std::move(body)
    });
    return F::make_true();
  }

  F make_identifier(const SV& sv) {
    auto name = std::any_cast<std::string>(sv[0]);

    // Function parameters are scoped only while parsing the function body.
    if (active_function_parameters.contains(name)) {
      return F::make_lvar(UNTYPED, LVar<allocator_type>(name.data()));
    }

    auto fun_it = defined_functions.find(name);
    // A bare symbol that is not a defined function is treated as a logical variable.
    if (fun_it == defined_functions.end()) {
      return F::make_lvar(UNTYPED, LVar<allocator_type>(name.data()));
    }
    // Identifier only handles bare symbols. A defined function with parameters
    // must be parsed through FunApplication, where its arguments are available.
    if (!fun_it->second.params.empty()) {
      return make_error(sv, "Function `" + name + "` expects arguments.");
    }
    return fun_it->second.body;
  }

  // Substitute a map of symbolic bindings inside a formula body.
  F substitute_bindings(F body, const std::map<std::string, F>& bindings) {
    if (bindings.empty()) {
      return body;
    }
    // Modify body in-place: body is owned by this function (taken by value),
    // inplace_map visits only leaf nodes and replaces them directly
    body.inplace_map(
      [&bindings](F& leaf, const F&) {
        if (leaf.is(F::LV)) {
          auto it = bindings.find(std::string(leaf.lv().data()));
          if (it != bindings.end()) {
            leaf = it->second;
          }
        }
      }
    );
    return body;
  }

  F make_fun_application(const SV& sv) {
    count_stat(stats.op_fun_application);

    // Expected semantic values: [Symbol(function name), Term(arg1), Term(arg2), ...].
    auto callee = std::any_cast<std::string>(sv[0]);
    auto fun_it = defined_functions.find(callee);
    if (fun_it == defined_functions.end()) {
      return make_error(sv, "Undefined function `" + callee + "`.");
    }

    const DefinedFunction& fun = fun_it->second;
    const size_t actual_arity = sv.size() - 1;
    if (actual_arity != fun.params.size()) {
      return make_error(
        sv,
        "Function `" + callee + "` expects " + std::to_string(fun.params.size()) +
        " arguments but got " + std::to_string(actual_arity) + "."
      );
    }

    std::map<std::string, F> bindings;
    for (size_t i = 0; i < fun.params.size(); ++i) {
      // fun.params[i].first is the param name, and sv[i + 1] is the corresponding argument formula.
      bindings.emplace(fun.params[i].first, f(sv[i + 1]));
    }
    return substitute_bindings(fun.body, bindings);
  }

  F make_let(const SV& sv) {
    count_stat(stats.op_let);

    // Expected semantic values:
    // [Symbol, Term, Symbol, Term, ..., Term(body)].
    // One binding is (Symbol, Term pair).
    // Therefore, `sv` size must be odd and at least size 3 (one binding + one body).
    if (sv.size() < 3 || (sv.size() % 2) == 0) {
      return make_error(sv, "Incorrect `let` expression.");
    }

    std::map<std::string, F> used_bindings;
    for (size_t i = 0; i < sv.size() - 1; i += 2) {
      std::string name;
      // Expected sv[i] is a symbol, which is the name of the binding.
      try {
        name = std::any_cast<std::string>(sv[i]);
      }
      catch (const std::bad_any_cast&) { // If it is not, report an error.
        return make_error(sv, "Incorrect `let` binding name.");
      }
      // Check for duplicate bindings.
      if (used_bindings.contains(name)) {
        return make_error(sv, "Duplicate `let` binding `" + name + "`.");
      }
      // Put the binding into the map
      used_bindings.emplace(std::move(name), f(sv[i + 1]));
    }
    // The last element of `sv` is the body of the let expression, where the let bindings should be substituted.
    // substitute_bindings() performs the substitution and returns the resulting formula.
    return substitute_bindings(f(sv[sv.size() - 1]), used_bindings);
  }

  F make_ite(const SV& sv) {
    count_stat(stats.op_ite);

    FSeq seq;
    seq.push_back(f(sv[0]));
    seq.push_back(f(sv[1]));
    seq.push_back(f(sv[2]));
    return F::make_nary(ITE, std::move(seq));
  }

  // SMT-LIB n-ary `distinct` means every pair of operands is different.
  // Turbo's `NEQ` is interpreted as a binary disequality, so n-ary `distinct` must be expanded into an AND of pairwise binary NEQ nodes.
  F make_distinct(const SV& sv) {
    count_stat(stats.op_distinct);

    // Expected semantic values: [Term1, Term2, ...].
    if (sv.size() == 2) {
      return F::make_binary(f(sv[0]), NEQ, f(sv[1]));
    }
    
    // To prevent repeatedly calling f(sv[i]) when building pairwise NEQ nodes, a list of terms for all operands is built first.
    FSeq seq;
    for (size_t i = 0; i < sv.size(); ++i) {
      seq.push_back(f(sv[i]));
    }

    FSeq pairwise;
    for (size_t i = 0; i < seq.size(); ++i) {
      for (size_t j = i + 1; j < seq.size(); ++j) {
        pairwise.push_back(F::make_binary(seq[i], NEQ, seq[j]));
      }
    }

    return F::make_nary(AND, std::move(pairwise));
  }

  F make_arith(const SV& sv) {
    auto arith_operator = std::any_cast<std::string>(sv[0]);

    Sig sig;
    if (arith_operator == "+") {
      count_stat(stats.op_add);
      sig = ADD;
    }
    else if (arith_operator == "-") {
      count_stat(stats.op_sub);
      // Negative is represented as a unary operator in AST.
      if (sv.size() == 2) {
        return F::make_unary(NEG, f(sv[1]));
      }
      // Subtraction is parsed with make_nary instead of explicitly handled in left-associative way with make_binary.
      // It will be ternarized in left-fold way in ternarize.hpp so that the left-associative property is preserved.
      sig = SUB;
    }
    else if (arith_operator == "*") {
      count_stat(stats.op_mul);
      sig = MUL;
    }
    else if (arith_operator == "/") {
      count_stat(stats.op_div);
      if (sv.size() != 3) {
        return make_error(sv, "`/` expects exactly two operands.");
      }
      return F::make_binary(f(sv[1]), DIV, f(sv[2]));
    }

    if (sv.size() < 3) {
      return make_error(sv, "`" + arith_operator + "` expects at least two operands.");
    }

    FSeq seq;
    for (size_t i = 1; i < sv.size(); ++i) {
      seq.push_back(f(sv[i]));
    }

    return F::make_nary(sig, std::move(seq));
  }

  F make_bound(const SV& sv) {
    auto binary_operator = std::any_cast<std::string>(sv[0]);
    Sig sig;
    if (binary_operator == "=") {
      count_stat(stats.op_eq);
      sig = EQ;
    }
    else if (binary_operator == "<=") {
      count_stat(stats.op_le);
      sig = LEQ;
    }
    else if (binary_operator == ">=") {
      count_stat(stats.op_ge);
      sig = GEQ;
    }
    else if (binary_operator == ">") {
      count_stat(stats.op_gt);
      sig = GT;
    }
    else if (binary_operator == "<") {
      count_stat(stats.op_lt);
      sig = LT;
    }

    return F::make_binary(f(sv[1]), sig, f(sv[2]));
  }

  F make_constraint(const SV& sv) {
    auto logic_operator = std::any_cast<std::string>(sv[0]);

    Sig sig;
    if (logic_operator == "not") {
      count_stat(stats.op_not);
      if (sv.size() != 2) {
        return make_error(sv, "`not` expects exactly one argument.");
      }
      return F::make_unary(NOT, f(sv[1]));
    }
    else if (logic_operator == "and") {
      count_stat(stats.op_and);
      sig = AND;
    }
    else if (logic_operator == "or") {
      count_stat(stats.op_or);
      sig = OR;
    }
    else if (logic_operator == "xor") {
      count_stat(stats.op_xor);
      sig = XOR;
    }
    else if (logic_operator == "=>") {
      count_stat(stats.op_imply);
      // Implication is right-associative and SMT allows n-ary syntax for right-associative op
      // (=> a b c) == (=> a (=> b c))
      F implication = f(sv[sv.size() - 1]);
      for (size_t i = sv.size() - 2; i >= 1; --i) {
        implication = F::make_binary(f(sv[i]), IMPLY, std::move(implication));
      }
      return implication;
    }

    if (sv.size() < 3) {
      return make_error(sv, "`" + logic_operator + "` expects at least two operands.");
    }

    FSeq seq;
    for (size_t i = 1; i < sv.size(); ++i) {
      seq.push_back(f(sv[i]));
    }

    return F::make_nary(sig, std::move(seq));
  }

  F make_assertion(const SV& sv) {
    count_stat(stats.cmd_assert);
    return f(sv[0]);
  }

  F make_statements(const SV& sv) {
    if (sv.size() == 1) {
      return f(sv[0]);
    }
    else {
      FSeq children;
      for (size_t i = 0; i < sv.size(); ++i) {
        F formula = f(sv[i]);
        if (!formula.is_true()) {
          children.push_back(formula);
        }
      }
      return F::make_nary(AND, std::move(children));
    }
  }

  // Since ( assert ⟨term⟩ ) is the only way to add assertions in SMT-lib, the else branch will never be used.
  // we can directly return the assertion formula without checking the size of `sv`.
  // F make_assertion(const SV& sv) {
  //   if (sv.size() == 1) {
  //     return f(sv[0]);
  //   }
  //   else {
  //     FSeq disjuncts;
  //     auto logic_operator = std::any_cast<std::string>(sv[0]); // OR
  //     for (int i = 1; i < sv.size(); ++i) {
  //       disjuncts.push_back(f(sv[i]));
  //     }

  //     return F::make_nary(OR, std::move(disjuncts));
  //   }
  // }
};
}  // namespace impl

// template <class Allocator>
// TFormula<Allocator> parse_smt_str(const std::string& input) {
//   impl::SMTParser<Allocator> parser;
//   return parser.parse(input);
// }

template <class Allocator>
TFormula<Allocator> parse_smt_str(const std::string& input, SolverOutput<Allocator>& output, bool is_nnv) {
  impl::SMTParser<Allocator> parser(output, is_nnv);
  return parser.parse(input);
}

template <class Allocator>
SMTParseResult<Allocator> parse_smt_str_with_status(
    const std::string& input,
    SolverOutput<Allocator>& output,
    bool is_nnv,
    bool collect_stats = true) {
  impl::SMTParser<Allocator> parser(output, is_nnv);
  return parser.parse_with_status(input, collect_stats);
}

// template <class Allocator>
// TFormula<Allocator> parse_smt(const std::string& filename) {
//   std::ifstream t(filename);
//   if (t.is_open()) {
//     std::string input((std::istreambuf_iterator<char>(t)),
//                       std::istreambuf_iterator<char>());
//     return parse_smt_str<Allocator>(input);
//   }
//   else {
//     std::cerr << "File `" << filename << "` does not exists." << std::endl;
//   }
//   return TFormula<Allocator>::make_false();
// }

template <class Allocator>
TFormula<Allocator> parse_smt(const std::string& filename, SolverOutput<Allocator>& output, bool is_nnv) {
  std::ifstream t(filename);
  if (t.is_open()) {
    std::string input((std::istreambuf_iterator<char>(t)),
                      std::istreambuf_iterator<char>());
    return parse_smt_str<Allocator>(input, output, is_nnv);
  }
  else {
    std::cerr << "File `" << filename << "` does not exists." << std::endl;
  }
  return TFormula<Allocator>::make_false();
}

template <class Allocator>
SMTParseResult<Allocator> parse_smt_with_status(
    const std::string& filename,
    SolverOutput<Allocator>& output,
    bool is_nnv,
    bool collect_stats = true) {
  std::ifstream t(filename);
  if (t.is_open()) {
    std::string input((std::istreambuf_iterator<char>(t)),
                      std::istreambuf_iterator<char>());
    return parse_smt_str_with_status<Allocator>(input, output, is_nnv, collect_stats);
  }
  SMTParseResult<Allocator> result;
  result.formula = TFormula<Allocator>::make_false();
  result.success = false;
  result.diagnostic = "File `" + filename + "` does not exists.";
  std::cerr << result.diagnostic << std::endl;
  return result;
}

}  // namespace lala

#endif
