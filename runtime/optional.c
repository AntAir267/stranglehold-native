/* System parts the program can do without, each failing the way it would where the part is
   missing or empty: DBGHELP (crash reports: symbols and stack walks find nothing, no minidump),
   AVIFIL32 (movie capture: no AVI file is made) and MSACM32 (audio codecs: none). */
#include "runtime.h"

/* ------------------------------------------------------------------ DBGHELP */
/* (functions returning a DWORD64 clear edx too) */
API(DBGHELP, SymInitialize, 3) { return 1; }
API(DBGHELP, SymGetOptions, 0) { return 0; }
API(DBGHELP, SymSetOptions, 1) { return ARG(0); }
API(DBGHELP, SymLoadModule64, 7)
{
    c->edx = ARG(5);                     /* BaseOfDll (a DWORD64): "loaded" there */
    return ARG(4);
}
API(DBGHELP, SymGetModuleBase64, 3) { c->edx = 0; return 0; }
API(DBGHELP, SymFunctionTableAccess64, 3) { return 0; }
API(DBGHELP, SymGetModuleInfo64, 4) { set_last_error(126); return 0; }
API(DBGHELP, SymGetSymFromAddr64, 5) { set_last_error(487); return 0; }   /* ERROR_INVALID_ADDRESS */
API(DBGHELP, SymGetLineFromAddr64, 5) { set_last_error(487); return 0; }
API(DBGHELP, StackWalk64, 9) { return 0; }                                /* (the end of the stack) */
API(DBGHELP, MiniDumpWriteDump, 7)
{
    LOG(LOG_ALWAYS, "MiniDumpWriteDump: no minidump written");
    set_last_error(50);                  /* ERROR_NOT_SUPPORTED */
    return 0;
}

/* ------------------------------------------------------------------ AVIFIL32 */
#define AVIERR_UNSUPPORTED 0x80044065u   /* MAKE_AVIERROR(101) */
#define AVIERR_FILEOPEN 0x8004406du      /* MAKE_AVIERROR(109) */

API(AVIFIL32, AVIFileInit, 0) { return 0; }
API(AVIFIL32, AVIFileExit, 0) { return 0; }
API(AVIFIL32, AVIFileOpenW, 4)
{
    char b[512];
    LOG(LOG_ALWAYS, "AVIFileOpenW(\"%s\"): movie capture is not supported", gwstr(ARG(1), b, sizeof b));
    if (ARG(0)) W32(ARG(0), 0);
    return AVIERR_FILEOPEN;
}
API(AVIFIL32, AVIFileCreateStreamW, 3) { if (ARG(1)) W32(ARG(1), 0); return AVIERR_UNSUPPORTED; }
API(AVIFIL32, AVIMakeCompressedStream, 4) { if (ARG(0)) W32(ARG(0), 0); return AVIERR_UNSUPPORTED; }
API(AVIFIL32, AVIStreamSetFormat, 4) { return AVIERR_UNSUPPORTED; }
API(AVIFIL32, AVIStreamWrite, 8) { return AVIERR_UNSUPPORTED; }
API(AVIFIL32, AVIStreamRelease, 1) { return 0; }
API(AVIFIL32, AVIFileRelease, 1) { return 0; }
API(AVIFIL32, AVISaveOptions, 5) { return 0; }                 /* (as if the user cancelled) */
API(AVIFIL32, AVISaveOptionsFree, 2) { return 0; }

/* ------------------------------------------------------------------ MSACM32 */
#define MMSYSERR_INVALHANDLE 5
#define ACMERR_NOTPOSSIBLE 512

API(MSACM32, acmFormatSuggest, 5) { return ACMERR_NOTPOSSIBLE; }
API(MSACM32, acmStreamOpen, 8)
{
    LOG(LOG_SOUND, "acmStreamOpen: no audio codecs");
    if (ARG(0)) W32(ARG(0), 0);
    return ACMERR_NOTPOSSIBLE;
}
API(MSACM32, acmStreamSize, 4) { return MMSYSERR_INVALHANDLE; }
API(MSACM32, acmStreamPrepareHeader, 3) { return MMSYSERR_INVALHANDLE; }
API(MSACM32, acmStreamUnprepareHeader, 3) { return MMSYSERR_INVALHANDLE; }
API(MSACM32, acmStreamConvert, 3) { return MMSYSERR_INVALHANDLE; }
