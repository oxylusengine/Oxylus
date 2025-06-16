#pragma once

#ifdef OX_PLATFORM_WINDOWS
  #ifdef OX_BUILD_DLL
    #define OX_SHARED __declspec(dllexport)
  #else
    #define OX_SHARED __declspec(dllimport)
  #endif
#else
  #define OX_SHARED
#endif
