// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

// Keeps the C++ runtime's error reporting out of the installer, the same way
// app/main.cpp does for the calculator.
//
// Setup is built without exceptions, but libstdc++ still reports container and
// allocation failures through the helpers below, and std::function reports an
// empty call the same way. Their library versions construct std::logic_error
// and friends, which pull in the narrow std::string instantiation, and the
// default terminate handler prints a demangled type name, which pulls in the
// whole symbol demangler: together around 75KB for diagnostics a windowed
// installer has nowhere to show. Every one of these paths is a bug or an
// out-of-memory condition, and ending the process is what the library versions
// would do here anyway, once the exception found no handler.

#include <windows.h>

#include <cstdlib>
#include <new>

namespace
{
    [[noreturn]] void Fail()
    {
        ExitProcess(3);
    }
}

namespace std
{
    void __throw_bad_alloc() { Fail(); }
    void __throw_bad_array_new_length() { Fail(); }
    void __throw_bad_function_call() { Fail(); }
    void __throw_logic_error(const char*) { Fail(); }
    void __throw_length_error(const char*) { Fail(); }
    void __throw_out_of_range(const char*) { Fail(); }
    void __throw_out_of_range_fmt(const char*, ...) { Fail(); }
    void __throw_invalid_argument(const char*) { Fail(); }
}

namespace __gnu_cxx
{
    void __verbose_terminate_handler()
    {
        Fail();
    }
}

// Setup has its own entry point (SetupEntry, in SetupUi.cpp) rather than the
// C runtime's, and atexit belongs to that runtime. The compiler still calls it
// to register destructors for objects with static storage, and __main calls it
// for its own table of them. Setup always leaves through ExitProcess, which
// releases everything those destructors would, so registering them is all the
// work there is to skip.
extern "C" int atexit(void (*)(void))
{
    return 0;
}

// The library's operator new throws std::bad_alloc, and a reference to that
// type is enough to bring the error machinery back in.
void* operator new(std::size_t size)
{
    void* memory = std::malloc(size != 0 ? size : 1);
    if (memory == nullptr)
    {
        Fail();
    }
    return memory;
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    return std::malloc(size != 0 ? size : 1);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    return std::malloc(size != 0 ? size : 1);
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}
