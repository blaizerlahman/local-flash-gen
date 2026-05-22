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

std::string SYSTEM_PROMPT{R"(You are a flashcard generator. You receive one or more text/Markdown notes and output Anki-compatible flashcards.

  **INPUT:** text/Markdown note content.
  **OUTPUT:** Plain text, one card per line, front and back separated by a tab character:
  Question\tAnswer

  **RULES:**
  - Extract key facts, definitions, and relationships from the notes.
  - Write short, specific questions with concise answers. Target a single self-contained fact per card.
  - Phrase questions so only one answer is correct and unambiguous.
  - Do not produce cards for trivial information (headings, formatting artifacts, TODOs, etc.).
  - Do not add commentary, headers, or formatting; output only tab-separated lines.

  )"};

struct Config {
  std::vector<fs::path> input_files;
  fs::path output_file{"anki_cards.txt"};
  std::string prompt_append;
  int chunk_size_notes{1};
  int server_port{8080};
  int context_window;
  int system_prompt_tokens;
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
      std::cerr << "error: cannot read -a file: " << value << std::endl;
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
      std::cerr << "error: unknown flag: " << flag << std::endl;
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
  for (const auto& file : cfg.input_files) {

    std::error_code ec;
    if (!fs::exists(file, ec) || !fs::is_regular_file(file, ec)) {
      std::cerr << "error: input file not found or not a regular file: " << file << std::endl;
      return std::nullopt;
    }
    if (std::ifstream f(file); !f) {
      std::cerr << "error: input file is not readable: " << file << std::endl;
      return std::nullopt;
    }
  }

  {
    fs::path out_dir = cfg.output_file.parent_path();
    if (!out_dir.empty()) {
      std::error_code ec;
      if (!fs::is_directory(out_dir, ec)) {
        std::cerr << "error: output directory does not exist: " << out_dir << std::endl;
        return std::nullopt;
      }
    }
  }

  return cfg;
}

// fetch model context length and token count of system prompt
int get_props(httplib::Client& client, Config& cfg) {

  // get context window length of model
  if (auto res = client.Get("/props")) {
    if (res->status == httplib::StatusCode::OK_200) {
      auto metadata = nlohmann::json::parse(res->body);
      int context = metadata["default_generation_settings"].value("n_ctx", -1);
      if (context < 0) {
        std::cerr << "ERROR: context window not found or is set to -1" << std::endl;
        return 1;
      }

      cfg.context_window = context;

    } else {
      std::cerr << "STATUS CODE: " << res->status << std::endl;
      return 1;
    }
  } else {
    auto err = res.error();
    std::cerr << "ERROR: " << httplib::to_string(err) << std::endl;
    return 1;
  }

  nlohmann::json req_body = { {"content", SYSTEM_PROMPT} };

  // get token count of system prompt
  if (auto res = client.Post("/tokenize", req_body.dump(), "application/json")) {

    if (res->status == httplib::StatusCode::OK_200) {
      
      nlohmann::json res_body;
      try {
        res_body = nlohmann::json::parse(res->body);
      } catch (nlohmann::json::parse_error& e) {
        std::cerr << "ERROR: JSON parsing of /tokenize response failed" << e.what() << std::endl;
        return 1;
      }
      
      int tokens = res_body["tokens"].size();
      if (tokens == 0) {
        std::cerr << "ERROR: No system prompt or error with /tokenize" << std::endl;
        return 1;
      }

      cfg.system_prompt_tokens = tokens;
    } else {
      std::cerr << "STATUS CODE: " << res->status << std::endl;
      return 1;
    }
  } else {
    auto err = res.error();
    std::cerr << "ERROR: " << httplib::to_string(err) << std::endl;
    return 1;
  }

  return 0;
}

// returns the token count of the note at filepath, or nullopt on error
std::optional<int> get_note_token_count(httplib::Client& client, const fs::path& filepath) {

  std::ifstream file(filepath);
  if (!file) {
    std::cerr << "error: cannot read file: " << filepath << std::endl;
    return std::nullopt;
  }

  // read in whole file content
  std::ostringstream content;
  content << file.rdbuf();

  nlohmann::json req_body = { {"content", content.str()} };

  // get file token count
  if (auto res = client.Post("/tokenize", req_body.dump(), "application/json")) {

    if (res->status == httplib::StatusCode::OK_200) {
      nlohmann::json res_body;
      try {
        res_body = nlohmann::json::parse(res->body);
      } catch (nlohmann::json::parse_error& e) {
        std::cerr << "ERROR: JSON parsing of /tokenize response failed: " << e.what() << std::endl;
        return std::nullopt;
      }

      return static_cast<int>(res_body["tokens"].size());
    } else {
      std::cerr << "STATUS CODE: " << res->status << std::endl;
      return std::nullopt;
    }
  } else {
    auto err = res.error();
    std::cerr << "ERROR: " << httplib::to_string(err) << std::endl;
    return std::nullopt;
  }
}

int main(int argc, char* argv[]) {

  if (argc == 1) {
    print_usage(argv[0]);
    return 0;
  }

  auto temp_cfg = parse_args(argc, argv);
  if (!temp_cfg) return 1;

  Config& cfg = *temp_cfg;

  std::cout << "Input files:\n";
  for (const auto& p : cfg.input_files)
    std::cout << "  " << p << std::endl;

  std::cout << "Output file: " << cfg.output_file << std::endl;
  std::cout << "Chunk size: " << cfg.chunk_size_notes << " notes\n";
  std::cout << "Local server port: " << cfg.server_port << std::endl;

  httplib::Client client("localhost", cfg.server_port);

  int err = get_props(client, cfg);
  if (err) return 1;

  std::cout << "Model context window: " << cfg.context_window << std::endl;
  std::cout << "System prompt token count: " << cfg.system_prompt_tokens << std::endl;



  return 0;
}

