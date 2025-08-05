#include "vidir.c"
#include "miniwin32.h"

// Additional types for UTF-8/UTF-16 handling
typedef i32 c32;  // Use i32 directly instead of char32_t for compatibility

enum {
    REPLACEMENT_CHARACTER = 0xfffd
};

typedef struct {
    s8  tail;
    c32 rune;
} utf8;

typedef struct {
    c16 *s;
    iz   len;
} s16;

typedef struct {
    s16 tail;
    c32 rune;
} utf16;

static s8 cuthead(s8 s, iz off) {
    assert(off <= s.len);
    s.s += off;
    s.len -= off;
    return s;
}

static s16 s16cuthead_(s16 s, iz off) {
    assert(off >= 0);
    assert(off <= s.len);
    s.s += off;
    s.len -= off;
    return s;
}

static utf8 utf8decode_(s8 s) {
    assert(s.len);
    utf8 r = {0};
    switch (s.s[0]&0xf0) {
    default  : r.rune = s.s[0];
               if (r.rune > 0x7f) break;
               r.tail = cuthead(s, 1);
               return r;
    case 0xc0:
    case 0xd0: if (s.len < 2) break;
               if ((s.s[1]&0xc0) != 0x80) break;
               r.rune = (c32)(s.s[0]&0x1f) << 6 |
                        (c32)(s.s[1]&0x3f) << 0;
               if (r.rune < 0x80) break;
               r.tail = cuthead(s, 2);
               return r;
    case 0xe0: if (s.len < 3) break;
               if ((s.s[1]&0xc0) != 0x80) break;
               if ((s.s[2]&0xc0) != 0x80) break;
               r.rune = (c32)(s.s[0]&0x0f) << 12 |
                        (c32)(s.s[1]&0x3f) <<  6 |
                        (c32)(s.s[2]&0x3f) <<  0;
               if (r.rune < 0x800) break;
               if (r.rune>=0xd800 && r.rune<=0xdfff) break;
               r.tail = cuthead(s, 3);
               return r;
    case 0xf0: if (s.len < 4) break;
               if ((s.s[1]&0xc0) != 0x80) break;
               if ((s.s[2]&0xc0) != 0x80) break;
               if ((s.s[3]&0xc0) != 0x80) break;
               r.rune = (c32)(s.s[0]&0x0f) << 18 |
                        (c32)(s.s[1]&0x3f) << 12 |
                        (c32)(s.s[2]&0x3f) <<  6 |
                        (c32)(s.s[3]&0x3f) <<  0;
               if (r.rune < 0x10000) break;
               if (r.rune > 0x10ffff) break;
               r.tail = cuthead(s, 4);
               return r;
    }
    r.rune = REPLACEMENT_CHARACTER;
    r.tail = cuthead(s, 1);
    return r;
}

static i32 utf16encode_(c16 *dst, c32 rune)
{
    if (rune<0 || (rune>=0xd800 && rune<=0xdfff) || rune>0x10ffff) {
        rune = REPLACEMENT_CHARACTER;
    }
    if (rune >= 0x10000) {
        rune -= 0x10000;
        dst[0] = (c16)((rune >> 10) + 0xd800);
        dst[1] = (c16)((rune&0x3ff) + 0xdc00);
        return 2;
    }
    dst[0] = (c16)rune;
    return 1;
}

// Encode code point returning the output length (1-4).
static i32 utf8encode_(u8 *s, c32 rune)
{
    if (rune<0 || (rune>=0xd800 && rune<=0xdfff) || rune>0x10ffff) {
        rune = REPLACEMENT_CHARACTER;
    }
    switch ((rune >= 0x80) + (rune >= 0x800) + (rune >= 0x10000)) {
    case 0: s[0] = (u8)(0x00 | ((rune >>  0)     )); return 1;
    case 1: s[0] = (u8)(0xc0 | ((rune >>  6)     ));
            s[1] = (u8)(0x80 | ((rune >>  0) & 63)); return 2;
    case 2: s[0] = (u8)(0xe0 | ((rune >> 12)     ));
            s[1] = (u8)(0x80 | ((rune >>  6) & 63));
            s[2] = (u8)(0x80 | ((rune >>  0) & 63)); return 3;
    case 3: s[0] = (u8)(0xf0 | ((rune >> 18)     ));
            s[1] = (u8)(0x80 | ((rune >> 12) & 63));
            s[2] = (u8)(0x80 | ((rune >>  6) & 63));
            s[3] = (u8)(0x80 | ((rune >>  0) & 63)); return 4;
    }
    assert(0);
}

static utf16 utf16decode_(s16 s)
{
    assert(s.len);
    utf16 r = {0};
    r.rune = s.s[0];
    if (r.rune>=0xdc00 && r.rune<=0xdfff) {
        goto reject;  // unpaired low surrogate
    } else if (r.rune>=0xd800 && r.rune<=0xdbff) {
        if (s.len < 2) {
            goto reject;  // missing low surrogate
        }
        i32 hi = r.rune;
        i32 lo = s.s[1];
        if (lo<0xdc00 || lo>0xdfff) {
            goto reject;  // expected low surrogate
        }
        r.rune = 0x10000 + ((hi - 0xd800)<<10) + (lo - 0xdc00);
        r.tail = s16cuthead_(s, 2);
        return r;
    }
    r.tail = s16cuthead_(s, 1);
    return r;

    reject:
    r.rune = REPLACEMENT_CHARACTER;
    r.tail = s16cuthead_(s, 1);
    return r;
}

static s8 fromwide_(arena *perm, s16 w)
{
    iz len = 0;
    utf16 state = {0};
    state.tail = w;
    while (state.tail.len) {
        state = utf16decode_(state.tail);
        u8 tmp[4];
        len += utf8encode_(tmp, state.rune);
    }

    s8 s = {0};
    s.s = new(perm, u8, len);
    state.tail = w;
    while (state.tail.len) {
        state = utf16decode_(state.tail);
        s.len += utf8encode_(s.s+s.len, state.rune);
    }
    return s;
}

// For communication with os_write()
struct os {
    // 4 handles: stdin stdout stderr tempfile
    struct {
        iptr h;
        b32  isconsole;
        b32  err;
    } handles[4];
    
    c16 *temp_file_path_w;  // UTF-16 path to temp file
};

static arena newarena_(iz cap)
{
    arena arena = {0};
    arena.beg = VirtualAlloc(0, cap, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!arena.beg) {
        arena.beg = (byte *)16;  // aligned, non-null, zero-size arena
        //TODO make this bail. WTF is this? casting 16 to a pointer is not right.
        cap = 0;
    }
    arena.end = arena.beg + cap;
    return arena;
}



// Get environment variable as UTF-16
static s16 fromenv_w(arena *perm, c16 *name)
{
    i32 wlen = GetEnvironmentVariableW(name, 0, 0);
    if (!wlen) {
        s16 r = {0};
        return r;
    }
    
    c16 *wbuf = new(perm, c16, wlen);
    GetEnvironmentVariableW(name, wbuf, wlen);
    
    return (s16){wbuf, wlen - 1};  // Exclude null terminator
}

static config *newconfig_(os *ctx)
{
    arena perm = newarena_(1<<23);
    perm.ctx = ctx;
    config *conf = new(&perm, config, 1);
    conf->perm = perm;
        
    c16 *cmdline = GetCommandLineW();
    i32 argc = 0;
    c16 **wargv = CommandLineToArgvW(cmdline, &argc);
    
    if (argc > 1) {
        // Skip argv[0], only pass actual arguments
        conf->nargs = argc - 1;
        conf->args = new(&perm, u8*, conf->nargs);

        for (i32 i = 1; i < argc; i++) {
            i32 wlen = 0;
            while (wargv[i][wlen]) wlen++;
            
            s16 wide = {wargv[i], wlen};
            s8 utf8_arg = fromwide_(&perm, wide);
            
            // Extend the allocation for null terminator
            u8 *null_term = new(&perm, u8, 1);
            *null_term = 0;
            
            conf->args[i-1] = utf8_arg.s;
        }
    } else {
        conf->nargs = 0;
        conf->args = 0;
    }

    

    conf->perm = perm;
    
    return conf;
}

// Buffer for converted UTF-16 output.
typedef struct {
    c16  buf[256];
    iptr handle;
    i32  len;
    b32  err;
} u16buf;

static void flushconsole_(u16buf *b)
{
    if (!b->err && b->len) {
        i32 written;
        if (!WriteConsoleW(b->handle, b->buf, b->len, &written, 0) || 
            written != b->len) {
            b->err = 1;
        }
    }
    b->len = 0;
}

static void os_write(os *ctx, i32 fd, s8 s)
{
    assert(fd > 0 && fd <= 3);
    if (ctx->handles[fd].err) return;
    
    if (ctx->handles[fd].isconsole) {
        u16buf b = {0};
        b.handle = ctx->handles[fd].h;
        utf8 state = {0};
        state.tail = s;
        while (state.tail.len) {
            state = utf8decode_(state.tail);
            if (b.len > countof(b.buf)-2) {
                flushconsole_(&b);
            }
            b.len += utf16encode_(b.buf+b.len, state.rune);
        }
        flushconsole_(&b);
        ctx->handles[fd].err = b.err;
    } else {
        i32 dummy;
        ctx->handles[fd].err = !WriteFile(ctx->handles[fd].h, s.s, s.len, &dummy, 0);
    }
}

static i32 os_read(os *ctx, i32 fd, u8 *buf, i32 len)
{
    assert(fd >= 0 && fd <= 3);
    if (ctx->handles[fd].err) return -1;
    
    i32 bytesRead = 0;
    if (!ReadFile(ctx->handles[fd].h, buf, len, &bytesRead, 0)) {
        ctx->handles[fd].err = 1;
        return -1;
    }
    return bytesRead;
}

static b32 os_path_is_dir(arena scratch, s8 path)
{
    utf8 state = {0};
    state.tail = path;
    
    // First pass: count required UTF-16 code units dynamically
    utf8 count_state = state;
    i32 required_len = 0;
    while (count_state.tail.len) {
        count_state = utf8decode_(count_state.tail);
        if (count_state.rune >= 0x10000) {
            required_len += 2;  // Surrogate pair
        } else {
            required_len += 1;
        }
    }
    
    // Allocate exactly the required size from scratch arena (+1 for null terminator)
    c16 *wpath = new(&scratch, c16, required_len + 1);
    
    // Convert UTF-8 to UTF-16 dynamically
    i32 wlen = 0;
    while (state.tail.len && wlen < required_len) {
        state = utf8decode_(state.tail);
        wlen += utf16encode_(wpath + wlen, state.rune);
    }
    wpath[wlen] = 0;  // Null terminate

    i32 attr = GetFileAttributesW(wpath);
    
    if (attr == -1) {
        return 0;  // Path doesn't exist or access denied
    }
    
    // Check if it's a directory
    return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static s8node *os_list_dir(arena *perm, s8 path)
{
    arena scratch = *perm;
    
    // Calculate required UTF-16 buffer size for search pattern dynamically
    utf8 state = {0};
    state.tail = path;
    i32 required_len = 0;
    
    while (state.tail.len) {
        state = utf8decode_(state.tail);
        if (state.rune >= 0x10000) {
            required_len += 2;  // Surrogate pair
        } else {
            required_len += 1;
        }
    }
    
    // Add space for /* pattern and null terminator
    required_len += 3;
    
    c16 *wpath = new(&scratch, c16, required_len);
    
    // Convert path to UTF-16
    state.tail = path;
    i32 wlen = 0;
    while (state.tail.len && wlen < required_len - 3) {  // Reserve space for \* and null
        state = utf8decode_(state.tail);
        wlen += utf16encode_(wpath + wlen, state.rune);
    }
    
    // Add /* pattern (or just * if path already ends with /)
    if (wlen > 0 && wpath[wlen-1] != '\\' && wpath[wlen-1] != '/') {
        wpath[wlen++] = '/';
    }
    wpath[wlen++] = '*';
    wpath[wlen] = 0;  // Null terminate
    
    // Start directory search
    finddata fd;
    iptr handle = FindFirstFileW(wpath, &fd);
    if (handle == INVALID_HANDLE_VALUE) {
        return 0;  // Directory doesn't exist or can't be read
    }
    
    s8node *head = 0;
    s8node **tail = &head;
    
    do {
        i32 name_len_w = 0;
        while (fd.name[name_len_w]) name_len_w++;
        
        // Skip . and .. entries
        if ((name_len_w == 1 && fd.name[0] == '.') ||
            (name_len_w == 2 && fd.name[0] == '.' && fd.name[1] == '.')) {
            continue;
        }
        
        // Calculate UTF-8 length for filename first
        s16 wide_name = {fd.name, name_len_w};
        utf16 state = {0};
        state.tail = wide_name;
        iz utf8_filename_len = 0;
        while (state.tail.len) {
            state = utf16decode_(state.tail);
            u8 tmp[4];
            utf8_filename_len += utf8encode_(tmp, state.rune);
        }
        
        if (!utf8_filename_len) continue;  // Skip if conversion fails
        
        // Calculate full path length
        iz full_path_len = path.len;
        if (path.len > 0 && path.s[path.len-1] != '\\' && path.s[path.len-1] != '/') {
            full_path_len += 1;  // Add separator
        }
        full_path_len += utf8_filename_len;
        
        // Allocate full path buffer from permanent arena
        u8 *full_path = new(perm, u8, full_path_len);
        
        // Copy original path
        for (iz i = 0; i < path.len; i++) {
            full_path[i] = path.s[i];
        }
        iz pos = path.len;
        
        // Add separator if needed
        if (path.len > 0 && path.s[path.len-1] != '\\' && path.s[path.len-1] != '/') {
            full_path[pos++] = '/';
        }
        
        // Convert filename directly into the full path buffer
        state.tail = wide_name;
        while (state.tail.len) {
            state = utf16decode_(state.tail);
            pos += utf8encode_(full_path + pos, state.rune);
        }
        
        // Create node and allocate from input arena
        s8node *node = new(perm, s8node, 1);
        node->str = (s8){full_path, full_path_len};
        node->next = 0;
        
        *tail = node;
        tail = &node->next;
        
    } while (FindNextFileW(handle, &fd));
    
    FindClose(handle);
    return head;
}

// Create a temp file, and open "file descriptor 3" with it
// NOTE: MAX_PATH hardcoded buffer uses here are okay.
// GetTempPathW documents:
// The maximum possible return value is MAX_PATH+1 (261).
// GetTempFileNameW documents:
// The string cannot be longer than MAX_PATH–14 characters or GetTempFileName will fail. 

static void os_create_temp_file(os *ctx, arena *perm)
{
    c16 temp_dir[261];
    i32 temp_dir_len = GetTempPathW(260, temp_dir);
    if (temp_dir_len == 0) {
        assert(0 && "TODO: Exit if this happens");
        ctx->handles[3].err = 1;
        return;
    }
    
    c16 temp_file[261];
    if (!GetTempFileNameW(temp_dir, L"vdr", 0, temp_file)) {
        ctx->handles[3].err = 1;
        assert(0 && "TODO: Exit if this happens");
        return;
    }
    
    i32 path_len = 0;
    while (temp_file[path_len]) path_len++;
    
    ctx->temp_file_path_w = new(perm, c16, path_len + 1);
    for (i32 i = 0; i <= path_len; i++) {  // Include null terminator
        ctx->temp_file_path_w[i] = temp_file[i];
    }
    
    // Open the temp file
    ctx->handles[3].h = CreateFileW(
        temp_file,
        GENERIC_WRITE,
        0,  // No sharing
        0,  // Default security
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY,
        0
    );
    
    if (ctx->handles[3].h == INVALID_HANDLE_VALUE) {
        ctx->handles[3].err = 1;
        assert(0 && "TODO: Exit if this happens");
    }
    
    ctx->handles[3].isconsole = 0;
    ctx->handles[3].err = 0;
}

// Close temp file so editor can open it
static void os_close_temp_file(os *ctx)
{
    if (ctx->handles[3].h && ctx->handles[3].h != INVALID_HANDLE_VALUE) {
        CloseHandle(ctx->handles[3].h);
        ctx->handles[3].h = INVALID_HANDLE_VALUE;
    }
}

// Open for reading
static void os_open_temp_file(os *ctx)
{
    if (!ctx->temp_file_path_w) {
        ctx->handles[3].err = 1;
        return;
    }
    
    ctx->handles[3].h = CreateFileW(
        ctx->temp_file_path_w,
        GENERIC_READ,
        0,
        0,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_TEMPORARY,
        0
    );
    
    if (ctx->handles[3].h == INVALID_HANDLE_VALUE) {
        ctx->handles[3].err = 1;
        assert(0 && "TODO: exit when this happens");
        return;
    }
    
    ctx->handles[3].isconsole = 0;
    ctx->handles[3].err = 0;
}

// Remove temp file from filesystem
static void os_remove_temp_file(os *ctx)
{
    // Close the file first if it's still open
    if (ctx->handles[3].h && ctx->handles[3].h != INVALID_HANDLE_VALUE) {
        CloseHandle(ctx->handles[3].h);
        ctx->handles[3].h = INVALID_HANDLE_VALUE;
    }
    
    if (ctx->temp_file_path_w) {
        DeleteFileW(ctx->temp_file_path_w);
    }
}


#ifndef DEFAULT_EDTIOR
#define DEFAULT_EDITOR L"notepad"
#endif
// Invoke editor on temp file using busybox sh -c
// This is okay for w64devkit, but might need adjustment in other environments.

static b32 os_invoke_editor(os *ctx, arena *scratch)
{
    if (!ctx->temp_file_path_w) {
        assert(0 && "Implement proper error");  // No temp file to edit
    }
    
    s16 editor = fromenv_w(scratch, L"EDITOR");
    if (!editor.len) {
        editor = (s16){ DEFAULT_EDITOR, countof(DEFAULT_EDITOR) - 1};
    }
    
    // Build command line: busybox sh -c "editor \"tempfile\""
    i32 tempfile_len = 0;
    while (ctx->temp_file_path_w[tempfile_len]) tempfile_len++;
    
    i32 total_len = countof(L"busybox sh ") - 1 +  // "busybox sh " (exclude null)
                    countof(L"-c ") - 1 +          // "-c " (exclude null)
                    1 +                            // opening quote
                    editor.len +                   // editor name
                    1 +                            // space between editor and file
                    1 +                            // opening quote for tempfile
                    tempfile_len +                 // temp file path
                    1 +                            // closing quote for tempfile
                    1 +                            // closing quote for -c
                    1;                             // null terminator
    
    c16 *cmdline = new(scratch, c16, total_len);
    i32 pos = 0;
    
    // Copy "busybox sh "
    c16 busybox[] = L"busybox sh ";
    for (i32 i = 0; i < countof(busybox) - 1; i++) {  // Exclude null terminator
        cmdline[pos++] = busybox[i];
    }
    
    // Copy "-c "
    cmdline[pos++] = L'-';
    cmdline[pos++] = L'c';
    cmdline[pos++] = L' ';
    
    // Opening quote
    cmdline[pos++] = L'"';
    
    // Copy editor
    for (i32 i = 0; i < editor.len; i++) {
        cmdline[pos++] = editor.s[i];
    }
    
    // Space
    cmdline[pos++] = L' ';
    
    // Opening quote for temp file path
    cmdline[pos++] = L'"';
    
    // Copy temp file path, converting \ to /
    for (i32 i = 0; i < tempfile_len; i++) {
        if (ctx->temp_file_path_w[i] == L'\\') {
            cmdline[pos++] = L'/';
        } else {
            cmdline[pos++] = ctx->temp_file_path_w[i];
        }
    }
    
    // Closing quote for temp file path
    cmdline[pos++] = L'"';
    
    // Closing quote for -c argument
    cmdline[pos++] = L'"';
    
    // Null terminator
    cmdline[pos] = 0;
    
    startupinfo si = {0};
    si.cb = sizeof(startupinfo);
    processinfo pi = {0};
    
    // Create the process
    b32 success = CreateProcessW(
        0,           // Application name (use command line)
        cmdline,     // Command line
        0,           // Process security attributes
        0,           // Thread security attributes
        0,           // Inherit handles
        0,           // Creation flags
        0,           // Environment
        0,           // Current directory
        &si,         // Startup info
        &pi          // Process info
    );
    
    if (!success) {
        return 0;  // Failed to start process
    }
    
    // Wait for the process to complete
    WaitForSingleObject(pi.hProcess, INFINITE);
    
    // Get exit code
    i32 exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    
    // Clean up handles
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    
    return exit_code == 0;  // Return success if editor exited cleanly
}



#if 1
__attribute((force_align_arg_pointer))
void mainCRTStartup(void) {
    os ctx[1] = {0};
    i32 dummy;
    ctx->handles[0].h         = GetStdHandle(STD_INPUT_HANDLE);
    ctx->handles[0].isconsole = GetConsoleMode(ctx->handles[0].h, &dummy);
    ctx->handles[1].h         = GetStdHandle(STD_OUTPUT_HANDLE);
    ctx->handles[1].isconsole = GetConsoleMode(ctx->handles[1].h, &dummy);
    ctx->handles[2].h         = GetStdHandle(STD_ERROR_HANDLE);
    ctx->handles[2].isconsole = GetConsoleMode(ctx->handles[2].h, &dummy);

    config *conf = newconfig_(ctx);
    
    // Initialize temp file for fd 3
    os_create_temp_file(ctx, &conf->perm);
    
    vidir(conf);
    
    ExitProcess(ctx->handles[1].err || ctx->handles[2].err);
}
#endif