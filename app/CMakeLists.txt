# The command processor is a library so the test suite can exercise it.
add_library(mx_cli STATIC CommandProcessor.cpp)
add_library(mx::cli ALIAS mx_cli)
target_include_directories(mx_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mx_cli PUBLIC mx::exchange)
mx_set_warnings(mx_cli)

if(MX_BUILD_APP)
  add_executable(miniexchange main.cpp)
  target_link_libraries(miniexchange PRIVATE mx::cli)
  mx_set_warnings(miniexchange)
endif()
