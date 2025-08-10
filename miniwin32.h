// vidir: minimal Win32 API definitions  
// https://github.com/peter0x44/vidir-c-portable
// This is free and unencumbered software released into the public domain.

// Platform-specific types (basic types come from vidir.c)
typedef ptrdiff_t       iptr;
typedef size_t          uptr;
typedef unsigned short  char16_t;
typedef char16_t        c16;

enum {
    CP_UTF8 = 65001,

    CREATE_ALWAYS = 2,

    FILE_ATTRIBUTE_DIRECTORY = 0x10,
    FILE_ATTRIBUTE_NORMAL = 0x80,
    FILE_ATTRIBUTE_TEMPORARY = 0x100,
    FILE_SHARE_READ = 1,
    FILE_SHARE_ALL = 7,

    GENERIC_READ = (i32)0x80000000,
    GENERIC_WRITE = 0x40000000,

    INFINITE = -1,
    INVALID_HANDLE_VALUE = -1,

    MEM_COMMIT  = 0x1000,
    MEM_RESERVE = 0x2000,

    OPEN_EXISTING = 3,

    PAGE_READWRITE = 4,

    STD_INPUT_HANDLE  = -10,
    STD_OUTPUT_HANDLE = -11,
    STD_ERROR_HANDLE  = -12,
};

typedef struct {
    i32 attr;
    u32 create[2], access[2], write[2];
    u32 size[2];
    u32 reserved1[2];
    c16 name[260];
    c16 altname[14];
    u32 reserved2[2];
} finddata;

typedef struct {
    i32 cb;
    c16 *reserved;
    c16 *desktop;
    c16 *title;
    i32 x, y, cx, cy;
    i32 xcountchars, ycountchars;
    i32 fillattribute;
    i32 flags;
    i32 showwindow;
    i32 reserved2;
    iptr reserved3;
    iptr stdin_handle;
    iptr stdout_handle;
    iptr stderr_handle;
} startupinfo;

typedef struct {
    iptr hProcess;
    iptr hThread;
    i32 dwProcessId;
    i32 dwThreadId;
} processinfo;

#define W32(r) __declspec(dllimport) r __stdcall
W32(b32)    CloseHandle(iptr);
W32(c16 **) CommandLineToArgvW(c16 *, i32 *);
W32(b32)    CreateDirectoryW(c16 *, uptr);
W32(i32)    CreateFileW(c16 *, i32, i32, uptr, i32, i32, i32);
W32(b32)    CreateProcessW(c16 *, c16 *, uptr, uptr, b32, i32, uptr, c16 *, startupinfo *, processinfo *);
W32(b32)    DeleteFileW(c16 *);
W32(void)   ExitProcess(i32);
W32(b32)    FindClose(iptr);
W32(iptr)   FindFirstFileW(c16 *, finddata *);
W32(b32)    FindNextFileW(iptr, finddata *);
W32(c16 *)  GetCommandLineW(void);
W32(b32)    GetConsoleMode(iptr, i32 *);
W32(i32)    GetEnvironmentVariableW(c16 *, c16 *, i32);
W32(b32)    GetExitCodeProcess(iptr, i32 *);
W32(i32)    GetFileAttributesW(c16 *);
W32(i32)    GetModuleFileNameW(iptr, c16 *, i32);
W32(iptr)   GetStdHandle(i32);
W32(i32)    GetTempFileNameW(c16 *, c16 *, i32, c16 *);
W32(i32)    GetTempPathW(i32, c16 *);
W32(b32)    MoveFileW(c16 *, c16 *);
W32(b32)    ReadFile(iptr, u8 *, i32, i32 *, uptr);
W32(b32)    RemoveDirectoryW(c16 *);
W32(b32)    SetStdHandle(i32, iptr);
W32(byte *) VirtualAlloc(uptr, iz, i32, i32);
W32(i32)    WaitForSingleObject(iptr, i32);
W32(b32)    WriteConsoleW(iptr, c16 *, i32, i32 *, uptr);
W32(b32)    WriteFile(iptr, u8 *, i32, i32 *, uptr);
