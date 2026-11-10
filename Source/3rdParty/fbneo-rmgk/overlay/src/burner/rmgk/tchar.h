#include <stdint.h>
// Always available so non-_UNICODE code that still handles wide buffers
// (e.g. burn.cpp zip-name parsing) can call wcslen/wcscpy. Mirrors the
// headless frontend's tchar.h.
#include <wchar.h>
#ifdef _UNICODE
//TODO: bah
#ifndef __TEXT
#define __TEXT(q)    L ## q
#endif

typedef wchar_t   TCHAR;
typedef wchar_t   _TCHAR;

#else

#ifndef __TEXT
#define __TEXT(q)    q
#endif

#ifndef RC_INVOKED
typedef char   TCHAR;
typedef char   _TCHAR;
#endif

#define _tcslen        strlen
#define _tcscpy        strcpy
#define _tcsncpy       strncpy

#define _tprintf       printf
#define _vstprintf     vsprintf
#define _vsntprintf    vsnprintf
#define _stprintf      sprintf
#define _sntprintf     snprintf
#define _ftprintf      fprintf
#define _tsprintf      sprintf

#define _tcscmp        strcmp
#define _tcsncmp       strncmp
#define _tcsicmp       strcasecmp
#define _tcsnicmp      strncasecmp
#define _tcstol        strtol
#define _tcsrchr       strrchr
#define _tcsstr        strstr

#define _totlower(c)   tolower((unsigned char)(c))

#define _fgetts        fgets
#define _fputts        fputs
#define _fputtc        fputc

#define _istspace      isspace

#define _tfopen        fopen

#define _stricmp       strcasecmp
#define stricmp        strcasecmp
#define _strnicmp      strncmp

// FBA function, change this!
#define dprintf        printf

#endif

#define _TEXT(x)    __TEXT(x)
#define _T(x)       __TEXT(x)
