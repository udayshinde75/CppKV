# Runtime sanitizers (GCC and Clang/AppleClang).
#
#   address    — ASan: out-of-bounds, use-after-free, double free (and leaks on Linux)
#   undefined  — UBSan: signed overflow, misaligned access, invalid casts, ...
#   thread     — TSan: data races. Cannot be combined with address.
#
# Sanitizers are applied to the whole build directory (including GoogleTest).
# TSan in particular needs all code that touches shared memory instrumented,
# otherwise it reports false positives or misses races.

function(cppkv_enable_sanitizers sanitizers)
    if(NOT sanitizers)
        return()
    endif()

    if(MSVC)
        message(FATAL_ERROR "CppKV: sanitizer presets support GCC and Clang only")
    endif()

    set(allowed address undefined thread)
    foreach(s IN LISTS sanitizers)
        if(NOT s IN_LIST allowed)
            message(FATAL_ERROR "CppKV: unknown sanitizer '${s}' (allowed: ${allowed})")
        endif()
    endforeach()

    if("address" IN_LIST sanitizers AND "thread" IN_LIST sanitizers)
        message(FATAL_ERROR "CppKV: AddressSanitizer and ThreadSanitizer cannot be combined")
    endif()

    list(JOIN sanitizers "," joined)
    set(flags
        -fsanitize=${joined}
        -fno-omit-frame-pointer   # readable stack traces
    )
    if("undefined" IN_LIST sanitizers)
        # By default UBSan prints a message and continues; make it abort so a
        # test actually fails when undefined behaviour occurs.
        list(APPEND flags -fno-sanitize-recover=undefined)
    endif()

    # Directory scope: affects every target defined after this call.
    add_compile_options(${flags})
    add_link_options(-fsanitize=${joined})

    message(STATUS "CppKV: sanitizers enabled: ${joined}")
endfunction()
