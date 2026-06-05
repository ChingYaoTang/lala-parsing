// Copyright 2026 Yi-Nung Tsao

/**
 * Parser-only SMT-LIB probe.
 *
 * This executable parses exactly one `.smt2` file through `lala/smt_parser_stat.hpp`
 * and prints one machine-readable result line. It does not enter Turbo's solver
 * pipeline, so the reported status and timing are limited to parser work.
 */

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "battery/allocator.hpp"
#include "lala/smt_parser_stat.hpp"
#include "lala/solver_output.hpp"

namespace {

constexpr const char* kResultPrefix = "SMT_PARSER_RESULT";

std::string json_string(const std::string& value) {
  std::ostringstream out;
  out << '"';
  for (unsigned char ch : value) {
    switch (ch) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20) {
          out << "\\u"
              << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<int>(ch)
              << std::dec << std::setfill(' ');
        }
        else {
          out << static_cast<char>(ch);
        }
    }
  }
  out << '"';
  return out.str();
}

void append_json_field(std::ostringstream& out, const char* name, const std::string& value, bool& first) {
  if (!first) {
    out << ',';
  }
  first = false;
  out << json_string(name) << ':' << json_string(value);
}

void append_json_field(std::ostringstream& out, const char* name, size_t value, bool& first) {
  if (!first) {
    out << ',';
  }
  first = false;
  out << json_string(name) << ':' << value;
}

void append_json_field(std::ostringstream& out, const char* name, bool value, bool& first) {
  if (!first) {
    out << ',';
  }
  first = false;
  out << json_string(name) << ':' << (value ? "true" : "false");
}

void append_json_field(std::ostringstream& out, const char* name, double value, bool& first) {
  if (!first) {
    out << ',';
  }
  first = false;
  out << json_string(name) << ':' << std::setprecision(9) << value;
}

std::string make_result_json(
    const lala::SMTParseResult<battery::standard_allocator>& result,
    double parse_seconds,
    bool stats_collected) {
  std::ostringstream out;
  bool first = true;
  out << '{';
  append_json_field(out, "parse_success", result.success, first);
  append_json_field(out, "diagnostic", result.diagnostic, first);
  append_json_field(out, "parse_seconds", parse_seconds, first);
  append_json_field(out, "stats_collected", stats_collected, first);
  if (!stats_collected) {
    out << '}';
    return out.str();
  }

  const auto& stats = result.stats;
  append_json_field(out, "theory", stats.theory, first);
  append_json_field(out, "expected_status", stats.expected_status, first);
  append_json_field(out, "var_total", stats.var_total, first);
  append_json_field(out, "var_bool", stats.var_bool, first);
  append_json_field(out, "var_int", stats.var_int, first);
  append_json_field(out, "var_real", stats.var_real, first);
  append_json_field(out, "cmd_assert", stats.cmd_assert, first);
  append_json_field(out, "cmd_declare_fun", stats.cmd_declare_fun, first);
  append_json_field(out, "cmd_declare_const", stats.cmd_declare_const, first);
  append_json_field(out, "cmd_define_fun", stats.cmd_define_fun, first);
  append_json_field(out, "op_le", stats.op_le, first);
  append_json_field(out, "op_ge", stats.op_ge, first);
  append_json_field(out, "op_eq", stats.op_eq, first);
  append_json_field(out, "op_gt", stats.op_gt, first);
  append_json_field(out, "op_lt", stats.op_lt, first);
  append_json_field(out, "op_and", stats.op_and, first);
  append_json_field(out, "op_or", stats.op_or, first);
  append_json_field(out, "op_not", stats.op_not, first);
  append_json_field(out, "op_imply", stats.op_imply, first);
  append_json_field(out, "op_xor", stats.op_xor, first);
  append_json_field(out, "op_add", stats.op_add, first);
  append_json_field(out, "op_sub", stats.op_sub, first);
  append_json_field(out, "op_mul", stats.op_mul, first);
  append_json_field(out, "op_div", stats.op_div, first);
  append_json_field(out, "op_ite", stats.op_ite, first);
  append_json_field(out, "op_let", stats.op_let, first);
  append_json_field(out, "op_distinct", stats.op_distinct, first);
  append_json_field(out, "op_fun_application", stats.op_fun_application, first);
  out << '}';
  return out.str();
}

int run_probe(const std::string& filename, bool print_ast, bool timing_only) {
  battery::standard_allocator allocator;
  lala::SolverOutput<battery::standard_allocator> output(allocator, lala::OutputType::SMT2);
  const bool collect_stats = !timing_only;

  const auto start = std::chrono::steady_clock::now();
  auto result = lala::parse_smt_with_status<battery::standard_allocator>(
    filename,
    output,
    false,
    collect_stats);
  const auto stop = std::chrono::steady_clock::now();
  const double parse_seconds = std::chrono::duration<double>(stop - start).count();

  if (print_ast && result.success) {
    result.formula.print();
    std::cout << '\n';
  }

  std::cout << kResultPrefix << '\t' << make_result_json(result, parse_seconds, collect_stats) << '\n';
  std::cout.flush();
  std::cerr.flush();

  // Keep parser-only runs from reporting recursive AST destruction as parser failure.
  std::_Exit(0);
}

}  // namespace

int main(int argc, char** argv) {
  bool print_ast = false;
  bool timing_only = false;
  std::string filename;

  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--ast") {
      print_ast = true;
    }
    else if (arg == "--timing-only") {
      timing_only = true;
    }
    else if (filename.empty()) {
      filename = arg;
    }
    else {
      std::cerr << "Usage: " << argv[0] << " [--ast] [--timing-only] <file.smt2>" << std::endl;
      return 1;
    }
  }

  if (filename.empty()) {
    std::cerr << "Usage: " << argv[0] << " [--ast] [--timing-only] <file.smt2>" << std::endl;
    return 1;
  }

  try {
    return run_probe(filename, print_ast, timing_only);
  }
  catch (const std::exception& exception) {
    std::cerr << "Unexpected exception: " << exception.what() << std::endl;
    return 2;
  }
  catch (...) {
    std::cerr << "Unexpected non-standard exception." << std::endl;
    return 3;
  }
}
