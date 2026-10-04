# Compiler settings shared by every target of bayan-desktop: strict warnings (errors by default), hardening, and optional sanitizers.
# Targets opt in by linking bayan::compile_options. Third-party headers (Qt) are included as system headers, so their warnings are not ours to fix.

include_guard(GLOBAL)

option(BAYAN_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
set(BAYAN_SANITIZERS "" CACHE STRING "Sanitizers to build with (GCC and Clang only), as a semicolon-separated list such as address;undefined")

add_library(bayan_compile_options INTERFACE)
add_library(bayan::compile_options ALIAS bayan_compile_options)
target_compile_features(bayan_compile_options INTERFACE cxx_std_20)

# Qt settings that turn common mistakes into compile errors: no implicit conversions between QString and 8-bit strings (which hide encoding bugs), no deprecated Qt API, no narrowing conversions in signal connections.
target_compile_definitions(bayan_compile_options INTERFACE
  QT_NO_CAST_FROM_ASCII
  QT_NO_CAST_TO_ASCII
  QT_NO_CAST_FROM_BYTEARRAY
  QT_NO_URL_CAST_FROM_STRING
  QT_NO_NARROWING_CONVERSIONS_IN_CONNECT
  QT_NO_FOREACH
  QT_USE_QSTRINGBUILDER
  QT_DISABLE_DEPRECATED_UP_TO=0x060C00
)

if(MSVC)
  target_compile_options(bayan_compile_options INTERFACE
    /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /external:anglebrackets /external:W0
    # Hardening: extra security checks and Control Flow Guard.
    /sdl /guard:cf
    $<$<BOOL:${BAYAN_WARNINGS_AS_ERRORS}>:/WX>
  )
  # The linker does not get /WX: with Ninja, CMake embeds manifests itself and passes /MANIFEST:NO together with /MANIFESTUAC:NO, which
  # always makes the linker warn (LNK4075).
  target_link_options(bayan_compile_options INTERFACE
    /guard:cf /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /CETCOMPAT
  )
  if(BAYAN_SANITIZERS)
    message(FATAL_ERROR "BAYAN_SANITIZERS is supported with GCC and Clang only")
  endif()
else()
  target_compile_options(bayan_compile_options INTERFACE
    -Wall -Wextra -Wpedantic
    -Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Woverloaded-virtual
    -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
    $<$<CXX_COMPILER_ID:GNU>:-Wduplicated-cond -Wduplicated-branches -Wlogical-op>
    $<$<BOOL:${BAYAN_WARNINGS_AS_ERRORS}>:-Werror>
    # Hardening: stack protection, and bounds checks in the standard library containers.
    -fstack-protector-strong
    $<$<PLATFORM_ID:Linux>:-fstack-clash-protection>
  )
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    target_compile_options(bayan_compile_options INTERFACE -fcf-protection=full)
  endif()
  if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_compile_definitions(bayan_compile_options INTERFACE _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST)
  else()
    target_compile_definitions(bayan_compile_options INTERFACE _GLIBCXX_ASSERTIONS)
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(bayan_compile_options INTERFACE LINKER:-z,relro LINKER:-z,now LINKER:-z,noexecstack)
  endif()

  if(BAYAN_SANITIZERS)
    list(JOIN BAYAN_SANITIZERS "," sanitizer_list)
    target_compile_options(bayan_compile_options INTERFACE
      -fsanitize=${sanitizer_list} -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(bayan_compile_options INTERFACE -fsanitize=${sanitizer_list})
  else()
    # Checked versions of C library functions in optimized builds (the sanitizers do their own checking). Level 3 needs glibc; Apple's C library supports level 2.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      set(fortify_level 3)
    else()
      set(fortify_level 2)
    endif()
    target_compile_options(bayan_compile_options INTERFACE
      $<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=${fortify_level}>)
  endif()
endif()
