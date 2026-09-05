#pragma once

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#error "Onyrion Shell requires C23 or newer"
#endif

#if defined(__clang__)
    #define ONYRION_COMPILER_CLANG 1
#elif defined(__GNUC__)
    #define ONYRION_COMPILER_GCC 1
#else
    #warning "Onyrion Shell is being built with an untested C compiler"
#endif
