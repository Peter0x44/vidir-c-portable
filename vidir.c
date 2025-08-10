// vidir: C implementation of vidir (moreutils)
// This is free and unencumbered software released into the public domain.

#include <stddef.h>

typedef unsigned char    u8;
typedef   signed int     b32;
typedef   signed int     i32;
typedef unsigned int     u32;
typedef   signed long long i64;
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
// Hash with the identity function
static u32 i32hash(i32 x)
{
    return (u32)x;
}

// Hash trie mapping line numbers to paths
typedef struct pathmap pathmap;
struct pathmap {
    pathmap *child[4];   // 4-way trie using bottom 2 bits of the key
    i32      key;
    s8       path;
};

// Insert or lookup a path by line number
static s8 *pathmap_insert(pathmap **m, i32 key, arena *perm)
{
    u32 h = i32hash(key);
    while (*m) {
        if ((*m)->key == key) {
            return &(*m)->path;
        }
        m = &(*m)->child[h & 0x3];  // Use bottom 2 bits
        h >>= 2;
    }
    if (!perm) {
        return 0;  // Not found, don't create
    }
    *m = new(perm, pathmap, 1);
    (*m)->child[0] = 0;
    (*m)->child[1] = 0;
    (*m)->child[2] = 0;
    (*m)->child[3] = 0;
    (*m)->key = key;
    (*m)->path = (s8){0}; // Initialize to empty
    return &(*m)->path;
}

// Lookup a path by line number
static s8 *pathmap_lookup(pathmap **m, i32 key)
{
    return pathmap_insert(m, key, 0);
}

// Extract directory path from a file path
static s8 dirname_s8(s8 path)
{
    iz last_slash = -1;
    for (iz i = path.len - 1; i >= 0; i--) {
        if (path.s[i] == '/' || path.s[i] == '\\') {
            last_slash = i;
            break;
        }
    }
    
    if (last_slash == -1) {
        return S(".");  // No slash found, use current directory
    }
    
    if (last_slash == 0) {
        return S("/");  // Root directory
    }
    
    // Return slice of original string up to last slash
    return (s8){path.s, last_slash};
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
static b32  os_path_exists(arena scratch, s8 path);
static s8node *os_list_dir(arena *, s8 path);
static b32  os_invoke_editor(os *ctx, arena scratch);
static void os_close_temp_file(os *ctx);
static void os_open_temp_file(os *ctx);
static b32  os_rename_file(os *ctx, arena scratch, s8 src, s8 dst);
static b32  os_delete_path(os *ctx, arena scratch, s8 path);
static b32  os_create_dir(os *ctx, arena scratch, s8 path);

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

static void printi64(u8buf *b, i64 x)
{
    u8  tmp[32];
    u8 *end = tmp + countof(tmp);
    u8 *beg = end;
    i64 t   = x<0 ? x : -x;
    do {
        *--beg = '0' - (u8)(t%10);
    } while (t /= 10);
    if (x < 0) {
        *--beg = '-';
    }
    s8 numstr = {beg, end - beg};
    prints8(b, numstr);
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

// Parse a line from temp file: "number\tpath"
// Modifies line to contain only the path part, returns the line number via pointer
// Returns true if line was parsed successfully, false if invalid
static b32 parse_temp_line(s8 *line, i32 *line_number)
{
    // Scan for tab
    iz tab_pos = -1;
    for (iz i = 0; i < line->len; i++) {
        if (line->s[i] == '\t') {
            tab_pos = i;
            break;
        }
    }
    
    if (tab_pos == -1) {
        // No tab found - invalid line
        return 0;
    }
    
    // Parse number part
    i32 num = 0;
    for (iz i = 0; i < tab_pos; i++) {
        if (line->s[i] >= '0' && line->s[i] <= '9') {
            num = num * 10 + (line->s[i] - '0');
        } else {
            // Invalid character in number
            return 0;
        }
    }
    
    // Modify line to contain only the path part
    line->s += tab_pos + 1;
    line->len -= tab_pos + 1;
    
    // Trim trailing whitespace from path
    while (line->len > 0 && 
           (line->s[line->len-1] == ' ' || 
            line->s[line->len-1] == '\t' ||
            line->s[line->len-1] == '\r')) {
        line->len--;
    }
    
    *line_number = num;
    return 1;
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
    u8buf *tmp = newfdbuf(perm, 3, 4096);  // temp file
    u8input *input = newinput(perm, 3, 4096);  // reading back from temp file
    u8input *stdin_input = newinput(perm, 0, 4096); // stdin reading
    
    // Hash trie to store original paths indexed by line number
    pathmap *original_paths = 0;
    
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
    
    // No paths provided and not reading from stdin, default to .
    if (paths_count == 0 && !read_from_stdin) {
        s8 current_dir = S(".");
        s8 *new_paths = new(perm, s8, paths_count + 1);
        new_paths[0] = current_dir;
        paths = new_paths;
        paths_count = 1;
    }

    // Read from stdin if requested
    if (read_from_stdin) {
        
        for (;;) {
            s8 line = nextline(stdin_input);
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

    // Write paths to temporary file in vidir format
    // <number>\t<name>\n
    i32 item_count = 0;
    for (i32 i = 0; i < paths_count; i++) {
        s8 path = paths[i];
        s8 basename = path;
        
        // Find the last slash to get basename
        for (iz j = path.len - 1; j >= 0; j--) {
            if (path.s[j] == '/') {
                basename.s = path.s + j + 1;
                basename.len = path.len - j - 1;
                break;
            }
        }
        
        if (s8equals(basename, S(".")) || s8equals(basename, S(".."))) {
            continue;
        }
        
        item_count++;
        
        // Store the original path using pathmap (line numbers start at 1)
        s8 *stored_path = pathmap_insert(&original_paths, item_count, perm);
        *stored_path = path;
        
        printi64(tmp, item_count);
        prints8(tmp, S("\t"));
        prints8(tmp, path);
        prints8(tmp, S("\n"));
    }
    
    flush(tmp);
    
    // Close temp file so editor can open it
    os_close_temp_file(perm->ctx);
    
    arena scratch = *perm;
    b32 editor_success = os_invoke_editor(perm->ctx, scratch);
    if (!editor_success) {
        prints8(err, S("vidir: failed to invoke editor\n"));
        flush(err);
        return;
    }
    
    // Reopen temp file for reading
    os_open_temp_file(perm->ctx);
    
    b32 has_errors = 0;
        
    // Parse all lines from temp file and process operations
    for (;;) {
        s8 line = nextline(input);
        if (line.len == 0 && line.s == 0) break;  // EOF
        
        // Skip empty lines
        if (line.len == 0) continue;
        
        i32 parsed_line_num;
        s8 line_copy = line;
        if (!parse_temp_line(&line_copy, &parsed_line_num)) {
            // Unable to parse line - this is an error
            prints8(err, S("vidir: unable to parse line \""));
            // Safely print the line - replace any control characters
            for (iz i = 0; i < line.len; i++) {
                if (line.s[i] >= 32 && line.s[i] <= 126) {
                    u8 c = line.s[i];
                    prints8(err, (s8){&c, 1});
                } else {
                    prints8(err, S("?"));
                }
            }
            prints8(err, S("\", aborting\n"));
            has_errors = 1;
            flush(err);
            return;
        }
        
        // Look up the original path for this line number
        s8 *original_path = pathmap_lookup(&original_paths, parsed_line_num);
        if (!original_path) {
            prints8(err, S("vidir: unknown item number "));
            printi64(err, parsed_line_num);
            prints8(err, S("\n"));
            has_errors = 1;
            flush(err);
            return;
        }
        
        s8 src = *original_path;
        s8 dst = line_copy;  // After parse_temp_line, this contains just the path
        
        // Check if the source file still exists
        if (!s8equals(src, dst)) {  // Only check if we're doing an operation
            if (dst.len == 0) {
                // This is deletion, don't check existence
            } else if (!os_path_exists(scratch, src)) {
                prints8(err, S("vidir: "));
                prints8(err, src);
                prints8(err, S(" does not exist\n"));
                // Mark this item as processed by clearing it
                *original_path = (s8){0};
                continue;
            }
        }
        
        if (!s8equals(src, dst)) {
            if (dst.len == 0) {
                // Skip deletion for now - we'll handle it at the end
            } else {
                // Handle swaps: if destination exists and matches one of our items
                if (os_path_exists(scratch, dst)) {
                    // Generate a unique temporary name
                    u8 *temp_name = new(perm, u8, dst.len + 10);
                    for (iz i = 0; i < dst.len; i++) {
                        temp_name[i] = dst.s[i];
                    }
                    temp_name[dst.len] = '~';
                    s8 temp_path = {temp_name, dst.len + 1};
                    
                    i32 counter = 0;
                    while (os_path_exists(scratch, temp_path)) {
                        counter++;
                        // Rebuild temp path with counter
                        iz pos = dst.len + 1;
                        i32 temp_counter = counter;
                        u8 digits[16];
                        iz digit_count = 0;
                        
                        do {
                            digits[digit_count++] = '0' + (temp_counter % 10);
                            temp_counter /= 10;
                        } while (temp_counter > 0);
                        
                        // Reverse digits and copy
                        for (iz i = 0; i < digit_count; i++) {
                            temp_name[pos + i] = digits[digit_count - 1 - i];
                        }
                        temp_path.len = pos + digit_count;
                    }
                    
                    if (!os_rename_file(perm->ctx, scratch, dst, temp_path)) {
                        prints8(err, S("vidir: failed to rename "));
                        prints8(err, dst);
                        prints8(err, S(" to "));
                        prints8(err, temp_path);
                        prints8(err, S("\n"));
                        has_errors = 1;
                    } else {
                        if (verbose) {
                            prints8(out, S("'"));
                            prints8(out, dst);
                            prints8(out, S("' -> '"));
                            prints8(out, temp_path);
                            prints8(out, S("'\n"));
                        }
                        
                        // Update any items that pointed to the old destination
                        for (i32 j = 1; j <= item_count; j++) {
                            s8 *check_path = pathmap_lookup(&original_paths, j);
                            if (check_path && s8equals(*check_path, dst)) {
                                // Copy temp_path to permanent memory
                                u8 *perm_path = new(perm, u8, temp_path.len);
                                for (iz k = 0; k < temp_path.len; k++) {
                                    perm_path[k] = temp_path.s[k];
                                }
                                *check_path = (s8){perm_path, temp_path.len};
                            }
                        }
                    }
                }
                
                // Create destination directory if needed
                s8 dst_dir = dirname_s8(dst);
                if (!s8equals(dst_dir, S(".")) && !os_path_is_dir(scratch, dst_dir)) {
                    if (!os_create_dir(perm->ctx, scratch, dst_dir)) {
                        prints8(err, S("vidir: failed to create directory tree "));
                        prints8(err, dst_dir);
                        prints8(err, S("\n"));
                        has_errors = 1;
                    }
                }
                
                // Perform the rename
                if (!os_rename_file(perm->ctx, scratch, src, dst)) {
                    prints8(err, S("vidir: failed to rename "));
                    prints8(err, src);
                    prints8(err, S(" to "));
                    prints8(err, dst);
                    prints8(err, S("\n"));
                    has_errors = 1;
                } else {
                    if (verbose) {
                        prints8(out, S("'"));
                        prints8(out, src);
                        prints8(out, S("' => '"));
                        prints8(out, dst);
                        prints8(out, S("'\n"));
                    }
                    
                    // If we renamed a directory, update all items that were inside it
                    if (os_path_is_dir(scratch, dst)) {
                        for (i32 j = 1; j <= item_count; j++) {
                            s8 *check_path = pathmap_lookup(&original_paths, j);
                            if (check_path && check_path->len > src.len) {
                                // Check if this path starts with src/ 
                                b32 is_subpath = 1;
                                for (iz k = 0; k < src.len && is_subpath; k++) {
                                    if (check_path->s[k] != src.s[k]) {
                                        is_subpath = 0;
                                    }
                                }
                                if (is_subpath && (check_path->s[src.len] == '/' || check_path->s[src.len] == '\\')) {
                                    // This item is inside the moved directory
                                    iz suffix_len = check_path->len - src.len;
                                    u8 *new_path = new(perm, u8, dst.len + suffix_len);
                                    for (iz k = 0; k < dst.len; k++) {
                                        new_path[k] = dst.s[k];
                                    }
                                    for (iz k = 0; k < suffix_len; k++) {
                                        new_path[dst.len + k] = check_path->s[src.len + k];
                                    }
                                    *check_path = (s8){new_path, dst.len + suffix_len};
                                }
                            }
                        }
                    }
                }
            }
        }
        
        // Mark this item as processed by clearing its pathmap entry
        *original_path = (s8){0, 0};
    }
    
    // Now handle remaining items (these should be deleted)
    for (i32 i = 1; i <= item_count; i++) {
        
        s8 *path = pathmap_lookup(&original_paths, i);
        if (path && path->len > 0 && path->s) {  // Item was not processed, so it should be deleted
            if (!os_delete_path(perm->ctx, scratch, *path)) {
                prints8(err, S("vidir: failed to remove "));
                prints8(err, *path);
                prints8(err, S("\n"));
                has_errors = 1;
            } else {
                if (verbose) {
                    prints8(out, S("removed '"));
                    prints8(out, *path);
                    prints8(out, S("'\n"));
                }
            }
        }
    }
    
    flush(out);
    flush(err);
}
