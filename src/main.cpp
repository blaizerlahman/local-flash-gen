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

std::string SYSTEM_PROMPT = R"(You are a flashcard generator. You receive one or more text/Markdown notes and output Anki-compatible flashcards.

INPUT: text/Markdown note content.

OUTPUT: A JSON array of objects, each with a "question" and "answer" field. Output only the JSON array and nothing else. No commentary, no markdown code fences, no explanation.

Example output:
[{"question":"What is mitosis?","answer":"A type of cell division that produces two genetically identical daughter cells."},{"question":"How many phases does mitosis have?","answer":"Four: prophase, metaphase, anaphase, and telophase."}]

RULES:
- Extract key facts, definitions, and relationships from the notes.
- Write short, specific questions with concise answers. Target a single self-contained fact per card.
- Phrase questions so only one answer is correct and unambiguous.
- Do not produce cards for trivial information such as headings, formatting artifacts, or TODOs.
- Output ONLY the JSON array. No other text.


)";

struct Config {
  std::vector<fs::path> input_files;
  fs::path output_file{"anki_cards.txt"};
  std::string prompt_append;
  int chunk_size{1};
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
    std::ostringstream content;
    content << f.rdbuf();
    return content.str();
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
          cfg.chunk_size = std::stoi(chunk_val); 
        }
        catch (const std::exception&) {
          std::cerr << "error: -c requires an integer argument\n";
          return std::nullopt;
        }

        if (cfg.chunk_size < 1) {
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
    std::cerr << "ERROR in getting system prompt tokens: " << httplib::to_string(err) << std::endl;
    return 1;
  }

  return 0;
}

// returns the content and token count of the note at filepath, or nullopt on error
std::pair<std::optional<std::string>, std::optional<int>> read_and_get_tokens(httplib::Client& client, const fs::path& filepath) {

  std::ifstream file(filepath);
  if (!file) {
    std::cerr << "ERROR: cannot read file: " << filepath << std::endl;
    return {std::nullopt, std::nullopt};
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
        return {std::nullopt, std::nullopt};
      }

      return {content.str(), static_cast<int>(res_body["tokens"].size())};
    } else {
      std::cerr << "ERROR: /tokenize returned with non-OK status\nSTATUS CODE: " << res->status << std::endl;
      return {std::nullopt, std::nullopt};
    }
  } else {
    auto err = res.error();
    std::cerr << "ERROR: " << httplib::to_string(err) << std::endl;
    return {std::nullopt, std::nullopt};
  }
}

// build note request body and 
std::optional<nlohmann::json> send_content(httplib::Client& client, std::ostringstream& buffer_content, int output_tokens, Config& cfg) {
  
  nlohmann::json req_body = {
    {"messages", nlohmann::json::array({
      {{"role", "system"}, {"content", SYSTEM_PROMPT}},
      {{"role", "user"},   {"content", buffer_content.str()}}
    })},
    {"max_tokens", output_tokens},
    {"temperature", 0.3}, // TODO test to find ideal temperature
    {"response_format", {{"type", "json_object"}}}
  };

  if (auto res = client.Post("/v1/chat/completions", req_body.dump(), "application/json")) {
    
    // return body of response if successful
    if (res->status == httplib::StatusCode::OK_200) {
      nlohmann::json res_body;
      try {
        res_body = nlohmann::json::parse(res->body);
      } catch (nlohmann::json::parse_error& e) {
        std::cerr << "ERROR: JSON parsing of flashcard response failed: " << e.what() << std::endl;
        return std::nullopt;
      }

      return res_body;
    } else {
      std::cerr << "ERROR: Flashcard response returned with non-OK status\nSTATUS CODE: " << res->status << std::endl;
      return std::nullopt;
    }
  } else {
    auto err = res.error();
    std::cerr << "ERROR in sending notes to model: " << httplib::to_string(err) << std::endl; 
    return 1;
  }
}

// parse flashcard response and write to desired output file
int parse_and_write(nlohmann::json json_body, Config& cfg) {

  // extract model's text output from chat completion response
  std::string content;
  try {
    content = json_body["choices"][0]["message"]["content"].get<std::string>();
  } catch (const nlohmann::json::exception& e) {
    std::cerr << "ERROR: Failed to extract content from model response: " << e.what() << std::endl;
    return 1;
  }

  // strip markdown code fences if present (e.g. ```json\n...\n```)
  if (content.size() >= 3 && content.substr(0, 3) == "```") {
    auto first_newline = content.find('\n');
    if (first_newline != std::string::npos)
      content = content.substr(first_newline + 1);
    auto closing_fence = content.rfind("```");
    if (closing_fence != std::string::npos)
      content = content.substr(0, closing_fence);
  }

  // parse content as JSON array of flashcards
  nlohmann::json cards;
  try {
    cards = nlohmann::json::parse(content);
  } catch (const nlohmann::json::parse_error& e) {
    std::cerr << "ERROR: Failed to parse flashcard JSON from model output: " << e.what() << std::endl;
    return 1;
  }

  if (!cards.is_array()) {
    std::cerr << "ERROR: Expected model output to be a JSON array" << std::endl;
    return 1;
  }

  std::ofstream output(cfg.output_file, std::ios::app);
  if (!output) {
    std::cerr << "ERROR: Cannot open output file: " << cfg.output_file << std::endl;
    return 1;
  }

  // write Anki separator header only when creating a new file
  if (output.tellp() == 0) {
    output << "#separator:semicolon" << std::endl;
  }

  // append flashcards to output file
  for (const auto& card : cards) {
    output << card["question"].get<std::string>() << ";" << card["answer"].get<std::string>() << std::endl;
  }

  return 0;
}

// run application logic, send notes in chunks while parsing/streaming response
// into output file
int run(httplib::Client& client, Config& cfg) {

  int output_tokens = 1000; // placeholder until we implement dynamic output buffer sizing

  int total_token_space = cfg.context_window - cfg.system_prompt_tokens - output_tokens;

  int buffer_tokens = 0; // how many tokens are in the send buffer
  int curr_chunk_count = 0; // how many files are in the current chunk

  std::ostringstream buffer_content;

  // iterate through notes, sending them and writing corresponding flashcards
  for (const auto& file : cfg.input_files) {
    
    auto [file_content, file_tokens] = read_and_get_tokens(client, file);

    if (!file_content || !file_tokens) return 1; // error printed in read_and_get_tokens

    std::string note_content = *file_content;
    int note_tokens = *file_tokens;

    // TODO possibly have interaction with user here about how to approach this
    // (split into further chunks, skip file, stop running, reduce system prompt, etc.)
    if (note_tokens > total_token_space) {
      std::cerr << "ERROR: Token count of " << file << " exceeds maximum context window" << std::endl;
      return 1;
    }

    bool token_overflow = buffer_tokens + note_tokens > total_token_space;
    bool chunk_overflow = curr_chunk_count >= cfg.chunk_size;

    // if the current file needs its own chunk, send current chunk before beginning new one
    if (!buffer_content.str().empty() && (token_overflow || chunk_overflow)) {

      auto res_content = send_content(client, buffer_content, output_tokens, cfg);
      if (!res_content) return 1; // error printed in send_content

      std::cout << res_content->dump(4) << std::endl; // DEBUG

      int err = parse_and_write(*res_content, cfg);
      if (err) return 1;

      // clear buffer
      buffer_content.str("");
      buffer_content.clear();
      buffer_tokens = 0;
      curr_chunk_count = 0;
    }

    // populate buffer
    buffer_content << "NEW NOTE:\n";
    buffer_content << note_content;

    buffer_tokens += note_tokens;
    curr_chunk_count++;
  }

  // send any remaining notes in buffer
  if (!buffer_content.str().empty()) {
    auto res_content = send_content(client, buffer_content, output_tokens, cfg);
    if (!res_content) return 1; // error printed in send_content

    std::cout << res_content->dump(4) << std::endl; // DEBUG

    int err = parse_and_write(*res_content, cfg);
    if (err) return 1;
  }

  return 0;
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
  std::cout << "Chunk size: " << cfg.chunk_size << " notes\n";
  std::cout << "Local server port: " << cfg.server_port << std::endl;

  httplib::Client client("localhost", cfg.server_port);

  int err;

  // get model context window and system prompt token count
  err = get_props(client, cfg);
  if (err) return 1;

  std::cout << "Model context window: " << cfg.context_window << std::endl;
  std::cout << "System prompt token count: " << cfg.system_prompt_tokens << std::endl;

  // send notes and write flashcards to output file
  err = run(client, cfg);
  if (err) return 1;

  return 0;
}

// TODO once the flashcard output functionality is made, test the relationships
// between input notes word count/line count/token count with outputted flashcard tokens
// so that we can set the output buffer (max_tokens for the model) to then be able to restrict
// how many notes we can use as input
