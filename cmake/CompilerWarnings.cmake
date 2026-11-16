# Applies a strict warning set to a target.
function(mx_set_warnings target)
  if(MSVC)
    set(warnings /W4 /permissive-)
    if(MX_WARNINGS_AS_ERRORS)
      list(APPEND warnings /WX)
    endif()
  else()
    set(warnings
      -Wall
      -Wextra
      -Wpedantic
      -Wshadow
      -Wnon-virtual-dtor
      -Wold-style-cast
      -Wcast-align
      -Woverloaded-virtual
      -Wunused
      -Wimplicit-fallthrough)
    if(MX_WARNINGS_AS_ERRORS)
      list(APPEND warnings -Werror)
    endif()
  endif()
  target_compile_options(${target} PRIVATE ${warnings})
endfunction()
