// SPDX-License-Identifier: GPL-3.0-or-later
/* RMG-Core's Library.cpp for a program without dlopen (the Switch, and its Linux self-test): the core
 * is linked in, and its "library handle" is the table switch/dynlib_static.c looks names up in.
 * RMG-K's core wrappers (m64p/CoreApi.cpp, Hook) find the core's functions through these. */

#include "Library.hpp"

extern "C" void* osal_dynlib_getproc(void* lib, const char* name);

CoreLibraryHandle CoreOpenLibrary(std::filesystem::path path)
{
    (void)path;
    return nullptr;   /* nothing is opened by path: the handles come from znx_static_lib() */
}

CoreLibrarySymbol CoreGetLibrarySymbol(CoreLibraryHandle handle, const char* symbol)
{
    return osal_dynlib_getproc(handle, symbol);
}

void CoreCloseLibrary(CoreLibraryHandle handle)
{
    (void)handle;
}

std::string CoreGetLibraryError(void)
{
    return "the function is not in the linked library's table";
}
