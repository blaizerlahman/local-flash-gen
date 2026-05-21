#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

struct Config {
  std::vector<fs::path> input_files;
  fs::path output_file{"anki_cards.txt"};
  std::string prompt_append;
  int chunk_size_notes{1};
  int server_port{8080};
};

static void print_usage(const char* prog) {
  std::cerr <<
    "Usage: " << prog << " -i <input.md> [-i <input2.md> ...] [-o <output.txt>] [-a <string|file>] [-c <chunk size>] [-p <port>]\n"
    "\n"
    "  -i <path>   Markdown file to convert (required, repeatable)\n"
    "  -o <path>   Output file for Anki cards (default: anki_cards.txt)\n"
    "  -a <value>  Text appended to the system prompt; may be a string or a path\n"
    "              to a .txt file whose contents will be read\n"
    "  -c <int>    Number of notes per LLM chunk (default: 1)\n"
    "  -p <int>    Port number of the llama.cpp server (default: 8080)\n";
}

// if value is a path to an existing regular file, returns its contents; otherwise returns value as-is.
static std::string resolve_append(const std::string& value) {

  std::error_code ec;

  // get file contents as string
  if (fs::is_regular_file(value, ec)) {
    std::ifstream f(value);
    if (!f) {
      std::cerr << "error: cannot read -a file: " << value << '\n';
      return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
  }

  return value;
}

static std::optional<Config> parse_args(int argc, char* argv[]) {

  Config cfg;

  for (int i = 1; i < argc; i++) {

    std::string flag = argv[i];

    if (flag == "-i" || flag == "-o" || flag == "-a" || flag == "-s" || flag == "-c" || flag == "-p") {

      if (i + 1 >= argc) {
        std::cerr << "error: flag " << flag << " requires an argument\n";
        print_usage(argv[0]);
        return std::nullopt;
      }

      if (flag == "-i") {

        // iterate through all provided files until another flag is reached
        while (++i < argc && argv[i][0] != '-') {
          cfg.input_files.push_back((std::string)argv[i]);
        }
        --i; // back up so the outer for loop's i++ lands on the next flag

      } else if (flag == "-o") {
        cfg.output_file = argv[++i];
      } else if (flag == "-c") {

        std::string chunk_val = argv[++i];
        try { 
          cfg.chunk_size_notes = std::stoi(chunk_val); 
        }
        catch (const std::exception&) {
          std::cerr << "error: -c requires an integer argument\n";
          return std::nullopt;
        }

        if (cfg.chunk_size_notes < 1) {
          std::cerr << "error: -c must be >= 1\n";
          return std::nullopt;
        }
      } else if (flag == "-p") {

        std::string port = argv[++i];
        try { 
          cfg.server_port = std::stoi(port); 
        }
        catch (const std::exception&) {
          std::cerr << "error: -p requires an integer argument\n";
          return std::nullopt;
        }

        if (cfg.server_port < 1 || cfg.server_port > 65535) {
          std::cerr << "error: -p must be a valid port number\n";
          return std::nullopt;
        }
      } else {

        std::string append_val = argv[++i];

        cfg.prompt_append = resolve_append(append_val);
        if (cfg.prompt_append.empty() && fs::is_regular_file(append_val)) {
          return std::nullopt; // resolve_append already printed the error
        }
      }
    } else {
      std::cerr << "error: unknown flag: " << flag << '\n';
      print_usage(argv[0]);
      return std::nullopt;
    }
  }

  // check if input files were given
  if (cfg.input_files.empty()) {
    std::cerr << "error: at least one -i input file is required\n";
    print_usage(argv[0]);
    return std::nullopt;
  }

  // verify that all input files are valid 
  for (const auto& p : cfg.input_files) {

    std::error_code ec;
    if (!fs::exists(p, ec) || !fs::is_regular_file(p, ec)) {
      std::cerr << "error: input file not found or not a regular file: " << p << '\n';
      return std::nullopt;
    }
    if (std::ifstream f(p); !f) {
      std::cerr << "error: input file is not readable: " << p << '\n';
      return std::nullopt;
    }
  }

  {
    fs::path out_dir = cfg.output_file.parent_path();
    if (!out_dir.empty()) {
      std::error_code ec;
      if (!fs::is_directory(out_dir, ec)) {
        std::cerr << "error: output directory does not exist: " << out_dir << '\n';
        return std::nullopt;
      }
    }
  }

  return cfg;
}

int main(int argc, char* argv[]) {

  if (argc == 1) {
    print_usage(argv[0]);
    return 0;
  }

  auto cfg = parse_args(argc, argv);
  if (!cfg) return 1;

  std::cout << "Input files:\n";
  for (const auto& p : cfg->input_files)
    std::cout << "  " << p << '\n';

  std::cout << "Output file: " << cfg->output_file << '\n';
  std::cout << "Chunk size: " << cfg->chunk_size_notes << " notes\n";
  std::cout << "Local server port: " << cfg->server_port << '\n';


  return 0;
}

