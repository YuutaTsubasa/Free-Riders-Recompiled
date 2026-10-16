#pragma once

// Included by each generated ppc_recomp.*.cpp, before the generated code.
// XenonRecomp names every function twice: __imp__X holds the code and X is
// an alias of it, weak so that a hook can replace X. Mach-O has no symbol
// aliases, so on Apple platforms each alias is only a weak declaration, and
// the generated aliases.cpp defines X as a call to __imp__X.
#if defined(__APPLE__)
#define alias(target) weak
#endif
