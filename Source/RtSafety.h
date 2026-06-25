#pragma once

// Real-time-safety attribute macro.
//
// Under the RealtimeSanitizer build (-DPREDROP_RT_SANITIZE=ON) the CMake target
// defines PREDROP_RT_NONBLOCKING as [[clang::nonblocking]], which makes clang's
// RTSan verify that the annotated function performs no allocation, lock, or
// syscall on the audio thread. In every other build (MSVC, normal release,
// SonarCloud analysis) it expands to nothing, so the code is unaffected.
#ifndef PREDROP_RT_NONBLOCKING
#define PREDROP_RT_NONBLOCKING
#endif
