function(enable_strict_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Werror
        -Wno-unused-parameter
        -Wno-unused-variable
        -Wno-unused-but-set-variable
    )
endfunction()

function(disable_strict_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
    )
endfunction()
