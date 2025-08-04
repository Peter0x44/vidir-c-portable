// vidir: C implementation of vidir (moreutils)
// This is free and unencumbered software released into the public domain.

#include <stddef.h>

typedef unsigned char    u8;
typedef   signed int     b32;
typedef   signed int     i32;
typedef unsigned int     u32;
typedef ptrdiff_t        iz;
typedef          char    byte;

#define assert(c)     while (!(c)) __builtin_trap()
#define countof(a)    (iz)(sizeof(a) / sizeof(*(a)))
#define new(a, t, n)  (t *)alloc(a, sizeof(t), n)
#define s8(s)         {(u8 *)s, countof(s)-1}
#define S(s)          (s8)s8(s)

typedef struct os os;

typedef struct {
    u8 *s;
    iz  len;
} s8;

typedef struct s8node s8node;
struct s8node {
    s8node *next;
    s8      str;
};

typedef struct {
    byte *beg;
    byte *end;
    os   *ctx;
} arena;

typedef struct {
    arena perm;
    u8  **args;         // command line arguments (minus argv[0])
    i32   nargs;        // number of arguments
} config;


static byte *alloc(arena *a, iz size, iz count)
{
    iz pad = -(ptrdiff_t)a->beg & (sizeof(void *) - 1);
    iz total = size * count;
    if (count > 0 && total/count != size) {
        // Integer overflow
        assert(0);
    }
    if (total > (a->end - a->beg) - pad) {
        // Out of memory  
        assert(0);
    }
    void *p = a->beg + pad;
    a->beg += pad + total;
    return p;
}

static s8 s8fromcstr(u8 *z)
{
    s8 s = {0};
    if (z) {
        s.s = z;
        for (; s.s[s.len]; s.len++) {}
    }
    return s;
}

static b32 s8equals(s8 a, s8 b)
{
    if (a.len != b.len) return 0;
    for (iz i = 0; i < a.len; i++) {
        if (a.s[i] != b.s[i]) return 0;
    }
    return 1;
}

static s8 takehead(s8 s, iz len)
{
    assert(len >= 0);
    assert(len <= s.len);
    s.len = len;
    return s;
}

static b32 startswith(s8 s, s8 prefix)
{
    return s.len >= prefix.len && s8equals(takehead(s, prefix.len), prefix);
}

// String comparison for sorting
static i32 u8compare(u8 *a, u8 *b, iz n)
{
    for (; n; n--) {
        i32 d = *a++ - *b++;
        if (d) return d;
    }
    return 0;
}

static i32 s8compare_(s8 a, s8 b)
{
    iz len = a.len<b.len ? a.len : b.len;
    iz r   = u8compare(a.s, b.s, len);
    return r ? r : a.len-b.len;
}

// Merge sort for s8node linked list
static s8node *s8sort_(s8node *head)
{
    if (!head || !head->next) {
        return head;
    }

    iz      len  = 0;
    s8node *tail = head;
    s8node *last = head;
    for (s8node *n = head; n; n = n->next, len++) {
        if (len & 1) {
            last = tail;
            tail = tail->next;
        }
    }

    last->next = 0;
    head = s8sort_(head);
    tail = s8sort_(tail);

    s8node  *rhead = 0;
    s8node **rtail = &rhead;
    while (head && tail) {
        if (s8compare_(head->str, tail->str) < 1) {
            *rtail = head;
            head = head->next;
        } else {
            *rtail = tail;
            tail = tail->next;
        }
        rtail = &(*rtail)->next;
    }
    *rtail = head ? head : tail;
    return rhead;
}

static void os_write(os *, i32 fd, s8);
static i32  os_read(os *, i32 fd, u8 *, i32);
static b32  os_path_is_dir(arena scratch, s8 path);
static s8node *os_list_dir(arena *, s8 path);
static s8    os_get_temp_file(arena *);
static b32  os_open_file_for_write(os *, s8 path);

typedef struct {
    arena *perm;
    os    *ctx;
    u8    *buf;
    iz     len;
    iz     cap;
    i32    fd;
} u8buf;

static u8buf *newfdbuf(arena *perm, i32 fd, iz cap)
{
    u8buf *b = new(perm, u8buf, 1);
    b->cap = cap;
    b->buf = new(perm, u8, cap);
    b->fd  = fd;
    b->ctx = perm->ctx;
    return b;
}

static s8 gets8(u8buf *b)
{
    s8 s = {0};
    s.s = b->buf;
    s.len = b->len;
    return s;
}

static void flush(u8buf *b)
{
    if (b->fd >= 1 && b->len) {
        os_write(b->ctx, b->fd, gets8(b));
    }
    b->len = 0;
}

static void prints8(u8buf *b, s8 s)
{
    for (iz off = 0; off < s.len;) {
        iz avail = b->cap - b->len;
        iz count = avail<s.len-off ? avail : s.len-off;
        for (iz i = 0; i < count; i++) {
            b->buf[b->len + i] = s.s[off + i];
        }
        b->len += count;
        off += count;
        if (b->len == b->cap) {
            flush(b);
        }
    }
}

// Input buffer for reading from stdin
typedef struct {
    arena *perm;
    os    *ctx;
    u8    *buf;
    iz     len;
    iz     cap;
    iz     pos;
    i32    fd;
    b32    eof;
} u8input;

static u8input *newinput(arena *perm, i32 fd, iz cap)
{
    u8input *b = new(perm, u8input, 1);
    b->perm = perm;
    b->cap = cap;
    b->buf = new(perm, u8, cap);
    b->fd  = fd;
    b->ctx = perm->ctx;
    return b;
}

static void refill(u8input *b)
{
    if (b->eof) return;
    
    // Move remaining data to beginning of buffer
    if (b->pos < b->len) {
        iz remaining = b->len - b->pos;
        for (iz i = 0; i < remaining; i++) {
            b->buf[i] = b->buf[b->pos + i];
        }
        b->len = remaining;
    } else {
        b->len = 0;
    }
    b->pos = 0;
    
    // Read more data
    iz space = b->cap - b->len;
    if (space > 0) {
        i32 n = os_read(b->ctx, b->fd, b->buf + b->len, (i32)space);
        if (n <= 0) {
            b->eof = 1;
        } else {
            b->len += n;
        }
    }
}

// Read next line from input, returns empty string on EOF
static s8 nextline(u8input *b)
{
    while (!b->eof) {
        // Look for newline in current buffer
        for (iz i = b->pos; i < b->len; i++) {
            if (b->buf[i] == '\n') {
                iz line_len = i - b->pos;
                s8 line = {b->buf + b->pos, line_len};
                b->pos = i + 1;  // Skip the newline
                return line;
            }
        }
        
        // No newline found, need more data
        if (b->pos > 0) {
            refill(b);
        } else if (b->len == b->cap) {
            // Buffer full but no newline - allocate larger buffer
            iz new_cap = b->cap * 2;
            u8 *new_buf = new(b->perm, u8, new_cap);
            for (iz i = 0; i < b->len; i++) {
                new_buf[i] = b->buf[i];
            }
            b->buf = new_buf;
            b->cap = new_cap;
            refill(b);
        } else {
            refill(b);
        }
    }
    
    // EOF reached, return any remaining data as final line
    if (b->pos < b->len) {
        s8 line = {b->buf + b->pos, b->len - b->pos};
        b->pos = b->len;
        return line;
    }
    
    // No more data
    s8 empty = {0};
    return empty;
}

static void vidir(config *);

static void vidir(config *conf)
{
    arena *perm = &conf->perm;
    b32 verbose = 0;
    b32 read_from_stdin = 0;
    
    // Set up buffered output (like u-config)
    u8buf *out = newfdbuf(perm, 1, 4096);  // stdout
    u8buf *err = newfdbuf(perm, 2, 4096);  // stderr
    
    // Simple growing array of file paths
    s8 *paths = 0;
    i32 paths_count = 0;

    // First pass: collect all path arguments (don't expand directories yet)
    if (conf->nargs > 0) {
        for (i32 i = 0; i < conf->nargs; i++) {
            s8 arg = s8fromcstr(conf->args[i]);
            if (s8equals(arg, S("-"))) {
                read_from_stdin = 1;
            } else if (startswith(arg, S("--"))) {
                arg.len-=2;
                arg.s+=2;
                if (s8equals(arg, S("verbose"))) {
                    verbose = 1;
                } else {
                    prints8(err, S("vidir: unknown option: --"));
                    prints8(err, arg);
                    prints8(err, S("\n"));
                    flush(err);
                    assert(0 && "Unknown option: TODO exit");
                }
            } else {
                // Add path as-is, we'll expand directories later
                s8 *new_paths = new(perm, s8, paths_count + 1);
                for (i32 j = 0; j < paths_count; j++) {
                    new_paths[j] = paths[j];
                }
                new_paths[paths_count] = arg;
                paths = new_paths;
                paths_count++;
            }
        }
    }
    
    // No paths provided, default to .
    if (paths_count == 0) {
        s8 current_dir = S(".");
        s8 *new_paths = new(perm, s8, paths_count + 1);
        new_paths[0] = current_dir;
        paths = new_paths;
        paths_count = 1;
    }

    // Read from stdin if requested
    if (read_from_stdin) {
        u8input *input = newinput(perm, 0, 4096);  // stdin
        
        for (;;) {
            s8 line = nextline(input);
            if (line.len == 0 && line.s == 0) break;  // EOF (null pointer)
            
            // Trim trailing whitespace (including \r if present)
            while (line.len > 0 && (line.s[line.len-1] == ' ' || 
                                   line.s[line.len-1] == '\t' ||
                                   line.s[line.len-1] == '\r')) {
                line.len--;
            }
            
            if (line.len == 0) continue;  // Skip empty lines
            
            // Make a copy of the line in permanent memory
            u8 *line_copy = new(perm, u8, line.len);
            for (iz j = 0; j < line.len; j++) {
                line_copy[j] = line.s[j];
            }
            s8 path = {line_copy, line.len};
            
            // Reallocate paths array with one more slot
            s8 *new_paths = new(perm, s8, paths_count + 1);
            for (i32 j = 0; j < paths_count; j++) {
                new_paths[j] = paths[j];
            }
            new_paths[paths_count] = path;
            paths = new_paths;
            paths_count++;
        }
    }

    // Now expand any directories in the collected paths
    s8 *final_paths = 0;
    i32 final_count = 0;
    
    for (i32 i = 0; i < paths_count; i++) {
        if (os_path_is_dir(*perm, paths[i])) {
            // Expand directory contents
            s8node *entries = os_list_dir(perm, paths[i]);
            entries = s8sort_(entries);  // Sort directory listings
            while (entries) {
                // Reallocate final_paths array with one more slot
                s8 *new_final = new(perm, s8, final_count + 1);
                for (i32 j = 0; j < final_count; j++) {
                    new_final[j] = final_paths[j];
                }
                new_final[final_count] = entries->str;
                final_paths = new_final;
                final_count++;
                entries = entries->next;
            }
        } else {
            // Regular file, add as-is
            s8 *new_final = new(perm, s8, final_count + 1);
            for (i32 j = 0; j < final_count; j++) {
                new_final[j] = final_paths[j];
            }
            new_final[final_count] = paths[i];
            final_paths = new_final;
            final_count++;
        }
    }
    
    // Use the expanded paths
    paths = final_paths;
    paths_count = final_count;

    // Print debug output showing what we collected
    prints8(out, S("vidir: collected "));
    // TODO: Add a number printing function
    if (paths_count < 10) {
        prints8(out, (s8){(u8*)"0123456789" + paths_count, 1});
    } else {
        prints8(out, S("many"));
    }
    prints8(out, S(" paths\n"));
    
    if (verbose) {
        for (i32 i = 0; i < paths_count; i++) {
            prints8(out, S("  "));
            prints8(out, paths[i]);
            if (os_path_is_dir(*perm, paths[i])) {
                prints8(out, S(" (directory)"));
            }
            prints8(out, S("\n"));
        }
    }
    
    flush(out);
    flush(err);
}


