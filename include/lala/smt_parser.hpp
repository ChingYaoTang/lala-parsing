// Copyright 2025 Yi-Nung Tsao

#ifndef LALA_PARSING_SMT_PARSER_HPP
#define LALA_PARSING_SMT_PARSER_HPP

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
#include <vector>

#include "battery/shared_ptr.hpp"
#include "lala/logic/ast.hpp"
#include "flatzinc_parser.hpp"

namespace lala {

namespace impl {

template <class Allocator>
class SMTParser {
  using allocator_type = Allocator;
  using F = TFormula<allocator_type>;
  using SV = peg::SemanticValues;
  using So = Sort<allocator_type>;
  using FSeq = typename F::Sequence;

  struct FormalParam {
    std::string name;
    So sort;
  };

  struct DefinedFunction {
    std::vector<FormalParam> params;
    So return_sort;
    F body;
  };

  std::map<std::string, So> declared_symbols; // Symbol name -> sort.
  std::map<std::string, DefinedFunction> defined_functions; // Function name -> signature and body.
  std::map<std::string, So> active_function_parameters; // Parameters currently in scope while parsing a function body.
  bool error;   // If an error was found during parsing.
  bool silent;  // If we do not want to output error messages.

 public:
  SMTParser() : error(false), silent(false) {}

  F parse(const std::string& input) {
    // const auto parse_start = std::chrono::steady_clock::now();
    // const auto report_elapsed = [&parse_start]() {
    //   const auto elapsed = std::chrono::duration<double>(
    //     std::chrono::steady_clock::now() - parse_start);
    //   std::cerr << "SMTParser::parse() took " << elapsed.count() << " s" << std::endl;
    // };
			peg::parser parser(R"(
				Statements    <- (DeclareVar / DeclareFun / DefineFun / Assertion / Comment)+

        Literal       <- Real / Boolean / Integer
				Integer       <- < [+-]?[0-9]+ >
				Real          <- < ('inf' / '-inf' /
														[+-]?[0-9]+ (('.' (&'..' / !'.') [0-9]*) /
														([Ee][+-]?[0-9]+)) ) >
	      Boolean       <- < 'true' / 'false' >

				Identifier    <- QuotedIdentifier / SimpleIdentifier
				SimpleIdentifier <- < [a-zA-Z_?~!@$%^&*+=<>./#-][a-zA-Z0-9_?~!@$%^&*+=<>./#-]* >
				QuotedIdentifier <- < '|' (!'|' .)* '|' >

        BinaryOp      <- < '<=' / '>=' / '=' / '>' / '<' >
				LogicOp       <- < 'and' / 'or' / 'not' / '=>' / 'xor' >
				ArithOp       <- < '+' / '-' / '*' / '/' >

				VarType       <- < 'Real' / 'Bool' / 'Int' >
        SortedVarList <- '(' ( '(' Identifier VarType ')' )* ')'

				Term          <- Ite / Arith / Literal / ApplyFun / Identifier
				Arith         <- '(' ArithOp Term+ ')'
				Ite           <- '(' 'ite' Formula Formula Formula ')'
        ApplyFun      <- '(' Identifier Formula+ ')'

				DeclareVar    <- '(' 'declare-const' Identifier VarType ')'
				DeclareFun    <- '(' 'declare-fun' Identifier '(' ')' VarType ')'
        DefineFun     <- '(' 'define-fun' Identifier SortedVarList VarType Formula ')'
        
        Let           <- '(' 'let' '(' ('(' Identifier Formula ')')+ ')' Formula ')'
        Formula       <- Let / Constraint / Bound / Term
        Bound         <- '(' BinaryOp Formula Formula ')'
        Constraint    <- '(' LogicOp Formula+ ')'
        Assertion     <- '(' 'assert' Formula ')'

				IgnoredAtom   <- < [^() \n\r\t]+ >
				IgnoredQuoted <- '"' ( '""' / !'"' . )* '"'
				IgnoredBar    <- '|' (!'|' .)* '|'
				IgnoredSExpr  <- IgnoredQuoted / IgnoredBar / IgnoredAtom / '(' IgnoredSExpr* ')'
				IgnoredCmd    <- '(' ('set-info' / 'set-logic' / 'check-sat' / 'exit') IgnoredSExpr* ')'

				~Comment      <- ';' [^\n\r]* [ \n\r\t]* / IgnoredCmd
				%whitespace   <- [ \n\r\t]*
			)");
    assert(static_cast<bool>(parser) == true);

    parser["Statements"] = [this](const SV& sv) { return make_statements(sv); };
    parser["Literal"] = [](const SV& sv) { return f(sv[0]); };
    parser["Integer"] = [](const SV& sv) { return F::make_z(sv.token_to_number<logic_int>()); };
    parser["Real"] = [](const SV& sv) { return F::make_real(impl::string_to_real(sv.token_to_string())); };
    parser["Boolean"] = [](const SV& sv) { return sv.token_to_string() == "true" ? F::make_true() : F::make_false(); };
    parser["SimpleIdentifier"] = [](const SV& sv) { return sv.token_to_string(); };
    parser["QuotedIdentifier"] = [](const SV& sv) { return sv.token_to_string(); };
    parser["Identifier"] = [](const SV& sv) { return std::any_cast<std::string>(sv[0]); };
    parser["BinaryOp"] = [](const SV& sv) { return sv.token_to_string(); };
    parser["LogicOp"] = [](const SV& sv) { return sv.token_to_string(); };
    parser["ArithOp"] = [](const SV& sv) { return sv.token_to_string(); };
    parser["VarType"] = [](const SV& sv) { return sv.token_to_string(); };
	  parser["DeclareVar"] = [this](const SV& sv) { return make_variable_decl(sv); };
	  parser["DeclareFun"] = [this](const SV& sv) { return make_variable_decl(sv); };
    parser["SortedVarList"] = [this](const SV& sv) { return make_sorted_var_list(sv); };
    parser["DefineFun"] = [this](const SV& sv) { return make_define_fun(sv); };
    parser["Term"] = [this](const SV& sv) { return make_term(sv); };
    parser["Let"] = [this](const SV& sv) { return make_let(sv); };
    parser["Ite"] = [this](const SV& sv) { return make_ite(sv); };
    parser["Arith"] = [this](const SV& sv) { return make_arith(sv); };
    parser["ApplyFun"] = [this](const SV& sv) { return make_apply_fun(sv); };
    parser["Bound"] = [this](const SV& sv) { return make_bound(sv); };
    parser["Formula"] = [this](const SV& sv) { return f(sv[0]); };
    parser["Constraint"] = [this](const SV& sv) { return make_constraint(sv); };
    parser["Assertion"] = [this](const SV& sv) { return make_assertion(sv); };

    F smt_formulas;
    if (parser.parse(input.c_str(), smt_formulas) && !error) {
      // report_elapsed();
      return smt_formulas; 
    } 
    else {
      // report_elapsed();
      std::cerr << "SMT parsing is failed." << std::endl;
      return F::make_false();
    }
  }

 private:
  static F f(const std::any& any) { return std::any_cast<F>(any); }

  F make_error(const SV& sv, const std::string& msg) {
    if (!silent) {
      std::cerr << sv.line_info().first << ":" << sv.line_info().second << ":"
                << msg << std::endl;
    }
    error = true;

    return F::make_false();
  }

  So get_sort_type(const std::string& type_name) const {
    if (type_name == "Int") {
      return So(So::Int);
    }
    if (type_name == "Real") {
      return So(So::Real);
    }
    assert(type_name == "Bool");
    return So(So::Bool);
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

  F make_variable_decl(const SV& sv) { 
    // Refer to make_parameter_decl(), make_existential(), and make_variable_decl() in flatzinc_parser.hpp 
    // for the implementation of variable declaration.

    // Expected semantic values: [Identifier, VarType].
    auto name = std::any_cast<std::string>(sv[0]);
    // Check if the variable name is already used by a declared symbol or a defined function.
    if (declared_symbols.contains(name) || defined_functions.contains(name)) {
      return make_error(sv, "Symbol `" + name + "` already declared.");
    }

    auto type_name = std::any_cast<std::string>(sv[1]);
    // Get the corresponding sort type object
    So var_type = get_sort_type(type_name);
    
    declared_symbols.emplace(name, var_type);
    return F::make_exists(UNTYPED, LVar<allocator_type>(name.data()), std::move(var_type));
  }

  std::vector<FormalParam> make_sorted_var_list(const SV& sv) {
    std::vector<FormalParam> params;
    active_function_parameters.clear();

    for (size_t i = 0; i < sv.size(); i += 2) {
      auto name = std::any_cast<std::string>(sv[i]);
      auto type_name = std::any_cast<std::string>(sv[i + 1]);

      if (active_function_parameters.contains(name)) {
        make_error(sv, "Duplicate function parameter `" + name + "`.");
        return {};
      }

      So sort_type = get_sort_type(type_name);
      active_function_parameters.emplace(name, sort_type);
      params.push_back(FormalParam{
        std::move(name),
        std::move(sort_type)
      });
    }
    return params;
  }

  F make_define_fun(const SV& sv) {
    auto name = std::any_cast<std::string>(sv[0]);
    auto params = std::any_cast<std::vector<FormalParam>>(sv[1]);
    auto return_sort = get_sort_type(std::any_cast<std::string>(sv[2]));
    F body = f(sv[3]);

    // The parameter scope is only needed while parsing the body.
    active_function_parameters.clear();

    if (declared_symbols.contains(name) || defined_functions.contains(name)) {
      return make_error(sv, "Symbol `" + name + "` already declared.");
    }

    defined_functions.emplace(name, DefinedFunction{
      std::move(params),
      std::move(return_sort),
      std::move(body)
    });
    return F::make_true();
  }

  F make_term(const SV& sv) {
    try {
      return f(sv[0]);
    } catch (const std::bad_any_cast&) {
      // For current implementation, if the term is not an F, 
      // it should be an identifier, which is treated as a logical variable.
      auto name = std::any_cast<std::string>(sv[0]);

      // For function parameters
      if (active_function_parameters.contains(name)) {
        return F::make_lvar(UNTYPED, LVar<allocator_type>(name.data()));
      }

      auto fun_it = defined_functions.find(name);
      // For simple identifier term
      if (fun_it == defined_functions.end()) {
        return F::make_lvar(UNTYPED, LVar<allocator_type>(name.data()));
      }
      if (!fun_it->second.params.empty()) {
        return make_error(sv, "Function `" + name + "` expects arguments.");
      }
      return fun_it->second.body;
    }
  }

  // Substitute a map of symbolic bindings inside a formula body.
  F substitute_bindings(F body, const std::map<std::string, F>& bindings) {
    if (bindings.empty()) {
      return body;
    }
    // Modify body in-place: body is owned by this function (taken by value),
    // so no full copy is needed. inplace_map visits only leaf nodes.
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

  F make_apply_fun(const SV& sv) {
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
      bindings.emplace(fun.params[i].name, f(sv[i + 1]));
    }
    return substitute_bindings(fun.body, bindings);
  }

  F make_let(const SV& sv) {
    // Expected semantic values:
    // [Identifier, Formula, Identifier, Formula, ..., Formula(body)].
    // One binding is (Identifier, Formula pair).
    // Therefore, `sv` size must be odd and at least size 3 (one binding + one body).
    if (sv.size() < 3 || (sv.size() % 2) == 0) {
      return make_error(sv, "Incorrect `let` expression.");
    }

    std::map<std::string, F> used_bindings;
    for (size_t i = 0; i < sv.size() - 1; i += 2) {
      std::string name;
      // Expected sv[i] is an identifier, which is the name of the binding.
      try {
        name = std::any_cast<std::string>(sv[i]);
      } catch (const std::bad_any_cast&) { // If it is not, report an error.
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
    FSeq seq;
    seq.push_back(f(sv[0]));
    seq.push_back(f(sv[1]));
    seq.push_back(f(sv[2]));
    return F::make_nary(ITE, std::move(seq));
  }

  F make_arith(const SV& sv) {
    auto arith_operator = std::any_cast<std::string>(sv[0]);

    Sig sig;
    if (arith_operator == "+") {
      sig = ADD;
    } else if (arith_operator == "-") {
      // Negative is represented as a unary operator in AST.
      if (sv.size() == 2) {
        return F::make_unary(NEG, f(sv[1]));
      }
      // Subtraction is parsed with make_nary instead of explicitly handled in left-associative way with make_binary.
      // It will be ternarized in left-fold way in ternarize.hpp so that the left-associative property is preserved.
      sig = SUB;
    } else if (arith_operator == "*") {
      sig = MUL;
    } else if (arith_operator == "/") {
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
    if (binary_operator == "=") sig = EQ;
    else if (binary_operator == "<=") sig = LEQ;
    else if (binary_operator == ">=") sig = GEQ;
    else if (binary_operator == ">") sig = GT;
    else if (binary_operator == "<") sig = LT;

    return F::make_binary(f(sv[1]), sig, f(sv[2]));
  }

  F make_constraint(const SV& sv) {
    auto logic_operator = std::any_cast<std::string>(sv[0]);

    Sig sig;
    if (logic_operator == "not") {
      if (sv.size() != 2) {
        return make_error(sv, "`not` expects exactly one argument.");
      }
      return F::make_unary(NOT, f(sv[1]));
    } else if (logic_operator == "and") {
      sig = AND;
    } else if (logic_operator == "or") {
      sig = OR;
    } else if (logic_operator == "xor") {
      sig = XOR;
    } else if (logic_operator == "=>") {
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
    if (sv.size() == 1) {
      return f(sv[0]);
    } 
    else {
      FSeq disjuncts;
      auto logic_operator = std::any_cast<std::string>(sv[0]); // OR
      for (int i = 1; i < sv.size(); ++i) {
        disjuncts.push_back(f(sv[i]));
      }

      return F::make_nary(OR, std::move(disjuncts));
    }
  }
};
}  // namespace impl

template <class Allocator>
TFormula<Allocator> parse_smt_str(const std::string& input) {
  impl::SMTParser<Allocator> parser;
  return parser.parse(input);
}

template <class Allocator>
TFormula<Allocator> parse_smt(const std::string& filename) {
  std::ifstream t(filename);
  if (t.is_open()) {
    std::string input((std::istreambuf_iterator<char>(t)),
                      std::istreambuf_iterator<char>());
    return parse_smt_str<Allocator>(input);
  } else {
    std::cerr << "File `" << filename << "` does not exists." << std::endl;
  }
  return TFormula<Allocator>::make_false();
}

}  // namespace lala

#endif
