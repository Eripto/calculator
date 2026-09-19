// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Force-included ahead of everything else.
//
// libstdc++ ships std::wstring as an explicit instantiation and declares it
// `extern template` in <string>, so every use of it links the whole of
// wstring-inst.o -- about 24KB, most of it members this app never calls.
// c++config.h defines _GLIBCXX_EXTERN_TEMPLATE unconditionally, so it cannot be
// set from the command line; including that header first and then redefining it
// works because of its own include guard. The value -1 is libstdc++'s documented
// special case: it disables the extern template declarations for basic_string
// only, leaving every other explicit instantiation alone. The members actually
// used are then emitted locally, where -ffunction-sections and --gc-sections can
// drop the rest.

#pragma once

#include <bits/c++config.h>

#undef _GLIBCXX_EXTERN_TEMPLATE
#define _GLIBCXX_EXTERN_TEMPLATE -1
