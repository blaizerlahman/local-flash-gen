cmake_minimum_required(VERSION 3.14) # stable FetchContent

project(local-flashcard-generator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

include(FetchContent)

FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.40.0
)

FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)

FetchContent_MakeAvailable(httplib json)

add_executable(local-flashcard-generator src/main.cpp)
target_link_libraries(local-flashcard-generator PRIVATE httplib::httplib nlohmann_json::nlohmann_json)
