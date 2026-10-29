# Warning flags for CppKV's own targets.
#
# Applied per target (not globally) so that third-party code such as
# GoogleTest is not compiled with our strict flags.

function(cppkv_set_warnings target)
    if(MSVC)
        set(warnings /W4 /permissive-)
        set(werror /WX)
    else()
        set(warnings
            -Wall
            -Wextra
            -Wpedantic                # non-standard extensions
            -Wshadow                  # a local hides an outer variable
            -Wconversion              # implicit narrowing, e.g. size_t -> int
            -Wsign-conversion         # implicit signed <-> unsigned
            -Wnon-virtual-dtor        # polymorphic class without virtual dtor
            -Wold-style-cast          # (int)x instead of static_cast<int>(x)
            -Woverloaded-virtual      # a function hides a base-class overload
            -Wdouble-promotion
            -Wformat=2
            -Wimplicit-fallthrough
            -Wcast-align
        )
        set(werror -Werror)
    endif()

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        list(APPEND warnings
            -Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
            -Wuseless-cast
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")   # Clang and AppleClang
        # Clang only: GCC's -Wnull-dereference runs after inlining and reports
        # false positives inside Boost.Asio headers in optimised builds
        # (observed with GCC 13 + Boost 1.83 at -O3), which -isystem does not
        # suppress.
        list(APPEND warnings -Wnull-dereference)
    endif()

    if(CPPKV_WARNINGS_AS_ERRORS)
        list(APPEND warnings ${werror})
    endif()

    target_compile_options(${target} PRIVATE ${warnings})
endfunction()
