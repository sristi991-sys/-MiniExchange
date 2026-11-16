# Global sanitizer switches. They must apply to every target (including
# GoogleTest), so they are added as directory-wide options before any target.
if(MX_ENABLE_ASAN AND MX_ENABLE_TSAN)
  message(FATAL_ERROR "MX_ENABLE_ASAN and MX_ENABLE_TSAN cannot be combined")
endif()

if(MX_ENABLE_ASAN)
  if(MSVC)
    add_compile_options(/fsanitize=address)
  else()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
  endif()
  message(STATUS "AddressSanitizer + UBSan enabled")
endif()

if(MX_ENABLE_TSAN)
  if(MSVC)
    message(FATAL_ERROR "ThreadSanitizer is not supported by MSVC")
  endif()
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
  message(STATUS "ThreadSanitizer enabled")
endif()
