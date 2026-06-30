cmake_minimum_required(VERSION 3.24)

project(pagecore_embed_example LANGUAGES CXX)

find_package(PageCore CONFIG REQUIRED)

add_executable(pagecore_embed_minimal embed_minimal.cpp)
target_link_libraries(pagecore_embed_minimal PRIVATE PageCore::pagecore)
