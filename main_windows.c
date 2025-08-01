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

static s8 cuthead(s8 s, iz off) {
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

// For communication with os_write()
struct os {
    // 4 handles: stdin stdout stderr
    struct {
        iptr h;
        b32  isconsole;
        b32  err;
    } handles[4];
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

// Get environment variable as s8
static s8 fromenv_(arena *perm, c16 *name)
{
    i32 wlen = GetEnvironmentVariableW(name, 0, 0);
    if (!wlen) {
        s8 r = {0};
        return r;
    }
    
    c16 *wbuf = new(perm, c16, wlen);
    GetEnvironmentVariableW(name, wbuf, wlen);
    
    i32 len = WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen - 1, 0, 0, 0, 0);
    if (!len) {
        s8 r = {0};
        return r;
    }
    
    u8 *buf = new(perm, u8, len);
    WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen - 1, buf, len, 0, 0);
    
    s8 r = {buf, len};
    return r;
}

static config *newconfig_(os *ctx)
{
    arena perm = newarena_(1<<20);
    perm.ctx = ctx;
    config *conf = new(&perm, config, 1);
    conf->perm = perm;
    
    conf->editor = fromenv_(&perm, L"EDITOR");
    
    c16 *cmdline = GetCommandLineW();
    i32 argc = 0;
    c16 **wargv = CommandLineToArgvW(cmdline, &argc);
    
    if (argc > 1) {
        // Skip argv[0], only pass actual arguments
        conf->nargs = argc - 1;
        conf->args = new(&perm, u8*, conf->nargs);

        for (i32 i = 1; i < argc; i++) {
            i32 len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, 0, 0, 0, 0);
            conf->args[i-1] = new(&perm, u8, len);
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, conf->args[i-1], len, 0, 0);
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

static b32 os_path_is_dir(os *ctx, s8 path)
{
    c16 wpath[32767];  // Maximum Windows path length
    
    utf8 state = {0};
    state.tail = path;
    i32 wlen = 0;
    
    while (state.tail.len && wlen < countof(wpath) - 1) {
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

static s8node *os_list_dir(os *ctx, arena *perm, s8 path)
{
    // Create search pattern: path\*
    c16 wpath[32767];
    utf8 state = {0};
    state.tail = path;
    i32 wlen = 0;
    
    // Convert path to UTF-16
    while (state.tail.len && wlen < countof(wpath) - 4) {  // Reserve space for \* and null
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
        // Get length of wide filename
        i32 name_len_w = 0;
        while (fd.name[name_len_w]) name_len_w++;
        
        // Skip . and .. entries
        if ((name_len_w == 1 && fd.name[0] == '.') ||
            (name_len_w == 2 && fd.name[0] == '.' && fd.name[1] == '.')) {
            continue;
        }
        
        // Convert filename from UTF-16 to UTF-8
        i32 name_utf8_len = WideCharToMultiByte(CP_UTF8, 0, fd.name, name_len_w, 0, 0, 0, 0);
        if (!name_utf8_len) continue;  // Skip if conversion fails
        
        // Create full path: original_path/filename
        iz full_path_len = path.len;
        if (path.len > 0 && path.s[path.len-1] != '\\' && path.s[path.len-1] != '/') {
            full_path_len += 1;  // Add separator
        }
        full_path_len += name_utf8_len;
        
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
        
        // Convert filename to UTF-8
        WideCharToMultiByte(CP_UTF8, 0, fd.name, name_len_w, (u8*)(full_path + pos), name_utf8_len, 0, 0);
        
        // Create node
        s8node *node = new(perm, s8node, 1);
        node->str = (s8){full_path, full_path_len};
        node->next = 0;
        
        *tail = node;
        tail = &node->next;
        
    } while (FindNextFileW(handle, &fd));
    
    FindClose(handle);
    return head;
}

static s8 os_get_temp_file(os *ctx, arena *perm)
{
    c16 temp_file[32767];
    
    i32 result = GetTempFileNameW(0, L"vidir", 0, temp_file);
    if (result == 0) {
        
        return (s8){0,0};
    }
    
    // Get length of temp file path
    i32 wlen = 0;
    while (temp_file[wlen]) wlen++;
    
    i32 utf8_len = WideCharToMultiByte(CP_UTF8, 0, temp_file, wlen, 0, 0, 0, 0);
    if (utf8_len <= 0) {
        return (s8){0, 0};
    }
    
    u8 *utf8_path = new(perm, u8, utf8_len);
    WideCharToMultiByte(CP_UTF8, 0, temp_file, wlen, utf8_path, utf8_len, 0, 0);
    
    s8 temp_path = {utf8_path, utf8_len};
    
    return temp_path;
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
    
    vidir(conf);
    
    ExitProcess(ctx->handles[1].err || ctx->handles[2].err);
}
#endif