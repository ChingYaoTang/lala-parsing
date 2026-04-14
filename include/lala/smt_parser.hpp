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

  std::map<std::string, So> declared_symbols; // Symbol name -> sort.
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
				Statements    <- (DeclareVar / DeclareFun / Assertion / Comment)+

        Literal       <- Real / Boolean / Integer
				Integer       <- < [+-]?[0-9]+ >
				Real          <- < ('inf' / '-inf' /
														[+-]?[0-9]+ (('.' (&'..' / !'.') [0-9]*) /
														([Ee][+-]?[0-9]+)) ) >
	      Boolean       <- < 'true' / 'false' >
				Identifier    <- QuotedIdentifier / SimpleIdentifier
				SimpleIdentifier <- < [a-zA-Z_?~!@$%^&*+=<>./#-][a-zA-Z0-9_?~!@$%^&*+=<>./#-]* >
				QuotedIdentifier <- < '|' (!'|' .)* '|' >

        BinaryOp      <- < '<=' / '>=' / ('=' !'>') / '>' / '<' >
				LogicOp       <- < 'and' / 'or' / 'not' / '=>' / 'xor' >
				ArithOp       <- < '+' / '-' / '*' / '/' >
				VarType       <- < 'Real' / 'Bool' / 'Int' >

				Term          <- Ite / Arith / Literal / Identifier
				Arith         <- '(' ArithOp Term+ ')'
				Ite           <- '(' 'ite' Formula Formula Formula ')'

				DeclareVar    <- '(' 'declare-const' Identifier VarType ')'
				DeclareFun    <- '(' 'declare-fun' Identifier '(' ')' VarType ')'
        
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
    parser["Identifier"] = [](const SV& sv) { return sv.token_to_string(); };
    parser["BinaryOp"] = [](const SV& sv) { return sv.token_to_string(); };
    parser["LogicOp"] = [](const SV& sv) { return sv.token_to_string(); };
    parser["ArithOp"] = [](const SV& sv) { return sv.token_to_string(); };
    parser["VarType"] = [](const SV& sv) { return sv.token_to_string(); };
	  parser["DeclareVar"] = [this](const SV& sv) { return make_variable_decl(sv); };
	  parser["DeclareFun"] = [this](const SV& sv) { return make_variable_decl(sv); };
    parser["Term"] = [this](const SV& sv) { return make_term(sv[0]); };
    parser["Let"] = [this](const SV& sv) { return make_let(sv); };
    parser["Ite"] = [this](const SV& sv) { return make_ite(sv); };
    parser["Arith"] = [this](const SV& sv) { return make_arith(sv); };
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
    auto name = std::any_cast<std::string>(sv[0]);
    if (declared_symbols.contains(name)) {
      return make_error(sv, "Variable `" + name + "` already declared.");
    }

    auto type_name = std::any_cast<std::string>(sv[1]);
    So var_type(So::Real);
    if (type_name == "Int") var_type = So::Int;
    else if (type_name == "Real") var_type = So::Real;
    else if (type_name == "Bool") var_type = So::Bool;
    else {
      return make_error(sv, "Unsupported variable type: `" + type_name + "`.");
    }
    
    declared_symbols.emplace(name, var_type);
    return F::make_exists(UNTYPED, LVar<allocator_type>(name.data()), std::move(var_type));
  }

  F make_term(const std::any& any) {
    try {
      return f(any);
    } catch (const std::bad_any_cast&) {
      // For current implementation, if the term is not an F, 
      // it should be an identifier, which is treated as a logical variable.
      auto name = std::any_cast<std::string>(any);
      return F::make_lvar(UNTYPED, LVar<allocator_type>(name.data()));
    }
  }

  // Substitute the let bindings in the body of the let expression with the corresponding formulas.
  F substitute_let_bindings(F body, const std::map<std::string, F>& let_bindings) {
    if (let_bindings.empty()) {
      return body;
    }
    // body.map() recursively substitutes the let bindings in the body of the let expression.
    return body.map(
      [&let_bindings](const F& leaf, const F&) -> F {
        // Check if a leaf in the body is a LVar
        if (leaf.is(F::LV)) {
          // If it is, further check if it is in the let bindings
          auto it = let_bindings.find(std::string(leaf.lv().data()));
          if (it != let_bindings.end()) {
            // If it is, substitute it with the corresponding formula.
            return it->second;
          }
        }
        // Otherwise, return the original leaf, which means no substitution is needed for this leaf.
        return leaf;
      }
    );
  }

  F make_let(const SV& sv) {
    // Expected semantic values:
    // [Identifier, Formula, Identifier, Formula, ..., Formula(body)].
    // Therefore, `sv` size must be odd and at least 3 (one binding + one body).
    if (sv.size() < 3 || (sv.size() % 2) == 0) {
      return make_error(sv, "Malformed `let` expression.");
    }

    std::map<std::string, F> used_bindings;
    for (size_t i = 0; i < sv.size() - 1; i += 2) {
      std::string name;
      // Expected sv[i] is an identifier, which is the name of the let binding.
      try {
        name = std::any_cast<std::string>(sv[i]);
      } catch (const std::bad_any_cast&) { // If it is not, report an error.
        return make_error(sv, "Malformed `let` binding name.");
      }
      // Check for duplicate bindings.
      if (used_bindings.contains(name)) {
        return make_error(sv, "Duplicate `let` binding `" + name + "`.");
      }
      used_bindings.emplace(std::move(name), f(sv[i + 1]));
    }
    // The last element of `sv` is the body of the let expression, where the let bindings should be substituted.
    // substitute_let_bindings() performs the substitution and returns the resulting formula.
    return substitute_let_bindings(f(sv[sv.size() - 1]), used_bindings);
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
      sig = SUB;
    } else if (arith_operator == "*") {
      sig = MUL;
    } else if (arith_operator == "/") {
      if (sv.size() != 3) {
        return make_error(sv, "`/` expects exactly two operands.");
      }
      return F::make_binary(f(sv[1]), DIV, f(sv[2]));
    } else {
      return make_error(sv, "Unsupported arithmetic operator: `" + arith_operator + "`.");
    }

    if (sv.size() < 3) {
      return make_error(sv, "`" + arith_operator + "` expects at least two operands.");
    }

    FSeq seq;
    for (size_t i = 1; i < sv.size(); ++i) {
      seq.push_back(f(sv[i]));
    }

    // if (sig == SUB) {
    //   // Subtraction is left-associative and SMT allows n-ary syntax for left-associative op
    //   // (- a b c) == ((a - b) - c)
    //   F subtraction = std::move(seq[0]);
    //   for (size_t i = 1; i < seq.size(); ++i) {
    //     subtraction = F::make_binary(std::move(subtraction), sig, std::move(seq[i]));
    //   }
    //   return subtraction;
    // }
    return F::make_nary(sig, std::move(seq));
  }

  F make_bound(const SV& sv) {
    auto binary_operator = std::any_cast<std::string>(sv[0]);
    Sig sig;
    if (binary_operator == "=") sig = EQ;
    else if (binary_operator == "<=") sig = LEQ;
    else if (binary_operator == ">=") sig = GEQ;
    else if (binary_operator == ">") sig = GT;
    else {
      assert(binary_operator == "<");
      sig = LT;
    }

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
      sig = IMPLY;
    } else {
      return make_error(sv, "Unsupported logical operator `" + logic_operator + "`.");
    }

    if (sv.size() < 3) {
      return make_error(sv, "`" + logic_operator + "` expects at least two operands.");
    }

    FSeq seq;
    for (size_t i = 1; i < sv.size(); ++i) {
      seq.push_back(f(sv[i]));
    }

    // if (sig == XOR) {
    //   // XOR is left-associative in SMT-LIB.
    //   // (xor a b c) == (xor (xor a b) c)
    //   F xor_formula = std::move(seq[0]);
    //   for (size_t i = 1; i < seq.size(); ++i) {
    //     xor_formula = F::make_binary(std::move(xor_formula), XOR, std::move(seq[i]));
    //   }
    //   return xor_formula;
    // }
    if (sig == IMPLY) {
      // Implication is right-associative and SMT allows n-ary syntax for right-associative op
      // (=> a b c) == (=> a (=> b c))
      F implication = std::move(seq[seq.size() - 1]);
      for (int i = static_cast<int>(seq.size()) - 2; i >= 0; --i) {
        implication = F::make_binary(std::move(seq[i]), IMPLY, std::move(implication));
      }
      return implication;
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
