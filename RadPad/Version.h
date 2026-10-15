#pragma once

// RadPad's version. Change it here only: RadPad.rc (the exe's version resource), the About box and the
// installer (RadPad.vcxproj reads these lines for installer\RadPad.iss) all use it.
#define RADPAD_VERSION_MAJOR 1
#define RADPAD_VERSION_MINOR 0
#define RADPAD_VERSION_PATCH 0

#define RADPAD_STRINGIZE2(x) #x
#define RADPAD_STRINGIZE(x) RADPAD_STRINGIZE2(x)
#define RADPAD_VERSION_STRING RADPAD_STRINGIZE(RADPAD_VERSION_MAJOR) "." RADPAD_STRINGIZE(RADPAD_VERSION_MINOR) "." RADPAD_STRINGIZE(RADPAD_VERSION_PATCH)
