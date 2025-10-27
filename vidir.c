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
#define new(a, t, n)  (t *)alloc(a, sizeof(t), n, _Alignof(t))
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


static byte *alloc(arena *a, iz size, iz count, iz align)
{
    iz pad = -(ptrdiff_t)a->beg & (align - 1);
    iz total = size * count;
    if (count > 0 && total/count != size) {
        // Integer overflow
        assert(0);
    }
    if (total > (a->end - a->beg) - pad) {
        // Out of memory  
        assert(0);
    }
    byte *p = a->beg + pad;
    a->beg += pad + total;
    for (iz i = 0; i < total; i++) {
        p[i] = 0;
    }
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

// Operation types for file rename plan
typedef enum { 
    OP_DELETE,  // delete src (dst unused)
    OP_RENAME,  // clobbering move src to dst
    OP_STASH,   // rename src to the temp name (dst unused)
    OP_UNSTASH, // rename the temp name to dst (src unused)
} Op;

typedef struct {
    Op op;
    s8 src;
    s8 dst;
} Action;

typedef struct {
    Action *actions;
    iz      len;
    iz      cap;
} Plan;

enum { 
    NEXT_OUTSIDE = -1,  // For owner[] mapping: destination is outside original set
    NO_DEPENDENCY = -1, // For deps[]/rdeps[]: no dependency relationship
    NOT_FOUND = -1      // For pathmap.value: key not found in map
};

static u32 s8hash(s8 s)
{
    u32 h = 0x811c9dc5;  // FNV-1a 32-bit offset basis
    for (iz i = 0; i < s.len; i++) {
        h ^= s.s[i];
        h *= 0x01000193;  // FNV-1a 32-bit prime
    }
    return h;
}

static b32 s8equals(s8 a, s8 b)
{
    if (a.len != b.len) return 0;
    for (iz i = 0; i < a.len; i++) {
        if (a.s[i] != b.s[i]) return 0;
    }
    return 1;
}
typedef struct pathmap pathmap;
struct pathmap {
    pathmap *child[4];   // 4-way trie
    s8       key;        // the path string
    iz       value;      // 0-based array index (-1 = not found)
};

// Insert or lookup a path in the map (path -> array index)
static iz *pathmap_insert(pathmap **m, s8 key, arena *perm)
{
    for (u32 h = s8hash(key); *m; h <<= 2) {
        if (s8equals((*m)->key, key)) {
            return &(*m)->value;
        }
        m = &(*m)->child[h>>30];  // Use top 2 bits
    }
    if (!perm) {
        return 0;  // Not found, don't create
    }
    *m = new(perm, pathmap, 1);
    (*m)->key = key;
    (*m)->value = NOT_FOUND;
    return &(*m)->value;
}

// Lookup a path in the reverse map
static iz *pathmap_lookup(pathmap **m, s8 key)
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
static iz u8compare(u8 *a, u8 *b, iz n)
{
    for (; n; n--) {
        iz d = *a++ - *b++;
        if (d) return d;
    }
    return 0;
}

static iz s8compare_(s8 a, s8 b)
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

static b32  os_path_exists(arena scratch, s8 path);

// File system state tracker to cache OS queries
typedef struct {
    pathmap *existing_files;  // Maps path -> 1 if file exists
} fsstate;

static fsstate *new_fsstate(arena *perm)
{
    fsstate *fs = new(perm, fsstate, 1);
    fs->existing_files = 0;
    return fs;
}

static void fsstate_mark_exists(fsstate *fs, s8 path, arena *perm)
{
    iz *exists = pathmap_insert(&fs->existing_files, path, perm);
    *exists = 1;
}

static void fsstate_mark_deleted(fsstate *fs, s8 path, arena *perm)
{
    iz *exists = pathmap_insert(&fs->existing_files, path, perm);
    *exists = 0;
}

static b32 fsstate_exists(fsstate *fs, s8 path, arena *perm)
{
    iz *exists = pathmap_lookup(&fs->existing_files, path);
    if (exists) {
        return *exists != 0;
    }
    
    // First time seeing this path - query OS once and cache result
    arena scratch = *perm;
    b32 file_exists = os_path_exists(scratch, path);
    
    // Cache the result
    iz *cached = pathmap_insert(&fs->existing_files, path, perm);
    *cached = file_exists ? 1 : 0;
    
    return file_exists;
}

// Generate a unique non-conflicting name for a file
static s8 fsstate_unique_name(fsstate *fs, s8 base_path, arena *perm)
{
    if (!fsstate_exists(fs, base_path, perm)) {
        return base_path;  // No conflict
    }
    
    // Try base_path~, base_path~1, base_path~2, etc.
    iz max_len = base_path.len + 20;  // Room for ~999999...
    u8 *candidate = new(perm, u8, max_len);
    
    // Copy base path
    for (iz i = 0; i < base_path.len; i++) {
        candidate[i] = base_path.s[i];
    }
    candidate[base_path.len] = '~';
    
    s8 candidate_path = {candidate, base_path.len + 1};
    if (!fsstate_exists(fs, candidate_path, perm)) {
        return candidate_path;
    }
    
    // Try with numbers
    for (i32 counter = 1; ; counter++) {
        iz pos = base_path.len + 1;
        i32 temp_counter = counter;
        
        // Convert counter to digits
        u8 digits[16];
        iz digit_count = 0;
        do {
            digits[digit_count++] = '0' + (temp_counter % 10);
            temp_counter /= 10;
        } while (temp_counter > 0);
        
        // Copy digits in correct order
        for (iz i = 0; i < digit_count; i++) {
            candidate[pos + i] = digits[digit_count - 1 - i];
        }
        
        candidate_path.len = pos + digit_count;
        if (!fsstate_exists(fs, candidate_path, perm)) {
            return candidate_path;
        }
        
        // Prevent overflow
        if (counter == 0x7fffffff) {  // INT32_MAX
            return base_path;  // Just give up...
        }
    }
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
static void os_exit(i32 code);

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

// Add a leading "./" to a relative path for display and stable matching
static s8 prepend_dot_slash(arena *perm, s8 path)
{
    if (path.len >= 2 && path.s[0] == '.' && (path.s[1] == '/' || path.s[1] == '\\')) {
        return path; // already has ./ or .\\ form
    }
    // Absolute paths (starts with '/' or a Windows drive like 'C:') stay unchanged
    if ((path.len >= 1 && (path.s[0] == '/' || path.s[0] == '\\')) ||
        (path.len >= 2 && ((path.s[1] == ':' && ((path.s[0] >= 'A' && path.s[0] <= 'Z') || (path.s[0] >= 'a' && path.s[0] <= 'z')))))) {
        return path;
    }
    u8 *p = new(perm, u8, path.len + 2);
    p[0] = '.';
    p[1] = '/';
    for (iz i = 0; i < path.len; i++) p[2 + i] = path.s[i];
    return (s8){p, path.len + 2};
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

// Parse the temporary file into an array of names. Returns an array of exactly
// original_name_count items. Missing items (to be deleted) are null.
static s8 *parse_temp_file(arena *perm, u8input *input, iz original_name_count, u8buf *err);

// Produce a sequence of operations necessary to achieve the new name set.
static Plan compute_plan(arena *perm, s8 *oldnames, s8 *newnames, iz num_names);

// Execute the plan 
static b32 execute_plan(Plan plan, arena scratch, os *ctx, u8buf *out, u8buf *err, b32 verbose);

// Parse a line from temp file: "number\tpath"
static b32 parse_temp_line(s8 *line, i32 *line_number);

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
    return (s8){0};
}

// Parse the temporary file into an array of names. Returns an array of exactly
// original_name_count items. Missing items (to be deleted) are null.
static s8 *parse_temp_file(arena *perm, u8input *input, iz original_name_count, u8buf *err)
{
    s8 *names = new(perm, s8, original_name_count);
    // Track duplicate item numbers
    u8 *seen = new(perm, u8, original_name_count);
    for (iz i = 0; i < original_name_count; i++) seen[i] = 0;
    
    for (;;) {
        s8 line = nextline(input);
        if (line.len == 0 && line.s == 0) break;  // EOF
        
        // Skip empty lines
        if (line.len == 0) continue;
        
        i32 parsed_line_num;
        s8 line_copy = line;
        if (!parse_temp_line(&line_copy, &parsed_line_num)) {
            prints8(err, S("vidir: unable to parse line, aborting\n"));
            flush(err);
            os_exit(1);
        }
        
        // Check if line number is in valid range [1, original_name_count]
        if (parsed_line_num < 1 || parsed_line_num > original_name_count) {
            prints8(err, S("vidir: unknown item number\n"));
            flush(err);
            os_exit(1);
        }
        
        // Copy the path to permanent memory
        u8 *path_copy = new(perm, u8, line_copy.len + 1);
        for (iz i = 0; i < line_copy.len; i++) {
            path_copy[i] = line_copy.s[i];
        }
        path_copy[line_copy.len] = 0;  // null terminate
        
        // Store in the array (convert to 0-based index), normalizing the path
        iz idx = (iz)(parsed_line_num - 1);
        if (seen[idx]) {
            prints8(err, S("vidir: duplicate item number in temp file\n"));
            flush(err);
            os_exit(1);
        }
        seen[idx] = 1;
        s8 parsed_path = {path_copy, line_copy.len};
        names[idx] = prepend_dot_slash(perm, parsed_path);
    }
    
    return names;
}

// Produce a sequence of operations necessary to achieve the new name set.
// Helper: append an action to a plan (arena grows, simple copy-on-append)
static void plan_append(arena *perm, Plan *p, Op op, s8 src, s8 dst)
{
    // Amortized growth
    if (p->len >= p->cap) {
        iz new_cap = p->cap ? p->cap * 2 : 8;
        Action *arr = new(perm, Action, new_cap);
        for (iz i = 0; i < p->len; i++) arr[i] = p->actions[i];
        p->actions = arr;
        p->cap = new_cap;
    }
    p->actions[p->len++] = (Action){op, src, dst};
}

static Plan compute_plan(arena *perm, s8 *oldnames, s8 *newnames, iz num_names)
{
/* 
 * For each file, construct a dependency graph:
 *   - deps[i]   : index of the file currently blocking file i's target (NO_DEPENDENCY if free)
 *   - rdeps[i]  : index of the file waiting for file i to move (NO_DEPENDENCY if none)
 *
 * Traversal:
 *   - Follow each file's dependency chain to the last file in the chain 
 *     (file with no further dependencies).
 *   - Detect cycles when the chain loops back to the starting file.
 *   - Break cycles by temporarily stashing the starting file.
 *   - Resolve the chain backwards via rdeps[] to emit operations in correct order.
 *
 */
    
    Plan plan = (Plan){0};

    if (num_names <= 0) return plan;

    // Build lookup map from old names to indices
    pathmap *oldmap = 0;
    for (iz i = 0; i < num_names; i++) {
        if (oldnames[i].s && oldnames[i].len > 0) {
            iz *slot = pathmap_insert(&oldmap, oldnames[i], perm);
            *slot = i;
        }
    }

    // Build dependency graph
    // deps[i] = index of file that must move before file i can move (NO_DEPENDENCY if none)
    // rdeps[i] = index of file that's waiting for file i to move (NO_DEPENDENCY if none)
    iz *deps = new(perm, iz, num_names);
    iz *rdeps = new(perm, iz, num_names);
    for (iz i = 0; i < num_names; i++) {
        deps[i] = rdeps[i] = NO_DEPENDENCY;
    }

    // Handle duplicate targets: last one wins, earlier ones get ~ suffixes
    s8 *final_dest = new(perm, s8, num_names);
    {
        pathmap *target_last_idx = 0;  // Maps target -> last index wanting it
        pathmap *dup_count_map = 0;    // Maps target -> count of duplicates seen
        
        // Find last occurrence of each target
        for (iz i = 0; i < num_names; i++) {
            final_dest[i] = newnames[i];  // Default: use original target
            s8 n = newnames[i];
            if (n.s && n.len && !s8equals(oldnames[i], n)) {
                iz *last = pathmap_insert(&target_last_idx, n, perm);
                *last = i;
            }
        }
        
        // Assign ~ suffixes to duplicates (all but last occurrence)
        for (iz i = 0; i < num_names; i++) {
            s8 target = newnames[i];
            // Skip non-moves
            if (s8equals(oldnames[i], target) || !target.s || !target.len) continue;
            
            iz *last = pathmap_lookup(&target_last_idx, target);
            if (!last || *last == i) continue;  // Not found or last occurrence
            
            // This is an earlier duplicate - add ~ suffix
            iz *count = pathmap_insert(&dup_count_map, target, perm);
            if (*count == NOT_FOUND) *count = 0;  // Initialize counter
            iz suffix_num = (*count)++;
            
            // Build final path: target~ or target~N
            iz suffix_len = 1;  // For '~'
            if (suffix_num > 0) {
                // Count digits in suffix_num
                for (iz n = suffix_num; n > 0; n /= 10) suffix_len++;
            }
            
            u8 *path = new(perm, u8, target.len + suffix_len);
            for (iz j = 0; j < target.len; j++) path[j] = target.s[j];
            path[target.len] = '~';
            
            if (suffix_num > 0) {
                // Write digits in reverse
                iz pos = target.len + suffix_len - 1;
                for (iz n = suffix_num; n > 0; n /= 10) {
                    path[pos--] = '0' + (n % 10);
                }
            }
            
            final_dest[i] = (s8){path, target.len + suffix_len};
        }
    }

    // Build dependency relationships
    for (iz i = 0; i < num_names; i++) {
        s8 dest = final_dest[i];
        if (!dest.s || dest.len == 0 || s8equals(oldnames[i], dest)) {
            continue;  // Skip deletes and non-moves
        }

        // Check if destination is currently occupied by another file
        iz *blocker = pathmap_lookup(&oldmap, dest);
        if (blocker && *blocker != i) {
            // File i depends on file *blocker moving first
            deps[i] = *blocker;
            rdeps[*blocker] = i;
        }
    }

    u8 *processed = new(perm, u8, num_names);
    for (iz i = 0; i < num_names; i++) {
        if (processed[i]) continue;  // Already handled this file

        // Handle deletes first
        if (!final_dest[i].s || final_dest[i].len == 0) {
            plan_append(perm, &plan, OP_DELETE, oldnames[i], (s8){0});
            processed[i] = 1;
            continue;
        }

        // Handle non-moves
        if (s8equals(oldnames[i], final_dest[i])) {
            processed[i] = 1;
            continue;
        }

        // Handle files with no dependencies
        if (deps[i] == NO_DEPENDENCY || processed[deps[i]]) {
            plan_append(perm, &plan, OP_RENAME, oldnames[i], final_dest[i]);
            processed[i] = 1;
            continue;
        }

        // Follow dependency chain to find the end
        iz last = deps[i];
        while (deps[last] != NO_DEPENDENCY && deps[last] != i && !processed[deps[last]]) {
            last = deps[last];
        }

        b32 cycle_detected = (deps[last] == i);
        
        if (cycle_detected) {
            // Break the cycle by stashing the starting file
            plan_append(perm, &plan, OP_STASH, oldnames[i], (s8){0});
            processed[i] = 1;
        }

        // Process dependency chain in execution order
        // Start from the end of the chain and work backwards to the beginning
        while (last != i) {
            plan_append(perm, &plan, OP_RENAME, oldnames[last], final_dest[last]);
            processed[last] = 1;
            last = rdeps[last];  // Move to the file waiting for this one
            if (last == NO_DEPENDENCY) break;  // Chain is broken
        }

        if (cycle_detected) {
            // Complete the cycle by unstashing to final destination
            plan_append(perm, &plan, OP_UNSTASH, (s8){0}, final_dest[i]);
        } else {
            // No cycle - just rename the starting file
            plan_append(perm, &plan, OP_RENAME, oldnames[i], final_dest[i]);
            processed[i] = 1;
        }
    }

    return plan;
}

// Execute the plan 

static b32 execute_plan(Plan plan, arena scratch, os *ctx, u8buf *out, u8buf *err, b32 verbose)
{
    // Track filesystem state for existence queries
    fsstate *fs = new_fsstate(&scratch);

    // Reserve all destination paths first to avoid temp name collisions
    for (iz i = 0; i < plan.len; i++) {
        Action a = plan.actions[i];
        if ((a.op == OP_RENAME || a.op == OP_UNSTASH) && a.dst.s && a.dst.len) {
            fsstate_mark_exists(fs, a.dst, &scratch);
        }
    }

    s8 temp_name = {0};  // Will be generated on first STASH operation

    // Execute each action
    for (iz i = 0; i < plan.len; i++) {
        Action a = plan.actions[i];
        switch (a.op) {
        case OP_STASH: {
            // Generate temporary name on first use
            if (!temp_name.s) {
                temp_name = fsstate_unique_name(fs, S(".vidir_temp"), &scratch);
            }
            
            // Move file to temporary location
            if (!os_rename_file(ctx, scratch, a.src, temp_name)) {
                prints8(err, S("vidir: failed to stash: "));
                prints8(err, a.src);
                prints8(err, S(" -> "));
                prints8(err, temp_name);
                prints8(err, S("\n"));
                flush(err);
                return 0;
            }
            
            fsstate_mark_deleted(fs, a.src, &scratch);
            fsstate_mark_exists(fs, temp_name, &scratch);
            
            if (verbose) {
                prints8(out, S("stash "));
                prints8(out, a.src);
                prints8(out, S(" -> "));
                prints8(out, temp_name);
                prints8(out, S("\n"));
            }
        } break;
        case OP_RENAME: {
            // Ensure destination directory exists
            s8 dir = dirname_s8(a.dst);
            if (!os_create_dir(ctx, scratch, dir)) {
                prints8(err, S("vidir: failed to create directory for: "));
                prints8(err, a.dst);
                prints8(err, S("\n"));
                flush(err);
                return 0;
            }

            // Try rename directly
            if (!os_rename_file(ctx, scratch, a.src, a.dst)) {
                prints8(err, S("vidir: failed to rename: "));
                prints8(err, a.src);
                prints8(err, S(" -> "));
                prints8(err, a.dst);
                prints8(err, S("\n"));
                flush(err);
                return 0;
            }
            fsstate_mark_deleted(fs, a.src, &scratch);
            fsstate_mark_exists(fs, a.dst, &scratch);
            if (verbose) {
                prints8(out, S("rename "));
                prints8(out, a.src);
                prints8(out, S(" -> "));
                prints8(out, a.dst);
                prints8(out, S("\n"));
            }
        } break;
        case OP_UNSTASH: {
            // Move from temporary location to final destination
            // temp_name should have been set by a previous STASH operation
            if (!temp_name.s) {
                prints8(err, S("vidir: unstash without prior stash\n"));
                flush(err);
                return 0;
            }

            // Ensure destination directory exists
            s8 dir = dirname_s8(a.dst);
            if (!os_create_dir(ctx, scratch, dir)) {
                prints8(err, S("vidir: failed to create directory for: "));
                prints8(err, a.dst);
                prints8(err, S("\n"));
                flush(err);
                return 0;
            }

            if (!os_rename_file(ctx, scratch, temp_name, a.dst)) {
                prints8(err, S("vidir: failed to unstash: "));
                prints8(err, temp_name);
                prints8(err, S(" -> "));
                prints8(err, a.dst);
                prints8(err, S("\n"));
                flush(err);
                return 0;
            }
            
            fsstate_mark_deleted(fs, temp_name, &scratch);
            fsstate_mark_exists(fs, a.dst, &scratch);
            
            if (verbose) {
                prints8(out, S("unstash "));
                prints8(out, temp_name);
                prints8(out, S(" -> "));
                prints8(out, a.dst);
                prints8(out, S("\n"));
            }
        } break;
        case OP_DELETE: {
            if (!os_delete_path(ctx, scratch, a.src)) {
                // If already gone, ignore; else report
                if (fsstate_exists(fs, a.src, &scratch)) {
                    prints8(err, S("vidir: failed to delete: "));
                    prints8(err, a.src);
                    prints8(err, S("\n"));
                    flush(err);
                    return 0;
                }
            } else {
                fsstate_mark_deleted(fs, a.src, &scratch);
            }
            if (verbose) {
                prints8(out, S("delete "));
                prints8(out, a.src);
                prints8(out, S("\n"));
            }
        } break;
        }
    }

    return 1;
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
            i32 digit = line->s[i] - '0';
            if (num > (0x7fffffff - digit) / 10) {
                // Would overflow
                return 0;
            }
            
            num = num * 10 + digit;
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
    
    // Set up buffered output
    u8buf *out = newfdbuf(perm, 1, 4096);  // stdout
    u8buf *err = newfdbuf(perm, 2, 4096);  // stderr
    u8buf *tmp = newfdbuf(perm, 3, 4096);  // temp file
    u8input *input = newinput(perm, 3, 4096);  // reading back from temp file
    u8input *stdin_input = newinput(perm, 0, 4096); // stdin reading
    
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
                    os_exit(1);
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

    // Filter out . and .. entries and write to temporary file
    s8 *original_names = new(perm, s8, paths_count);
    i32 original_name_count = 0;
    
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
        
        s8 display = prepend_dot_slash(perm, path);
        original_names[original_name_count] = display;
        original_name_count++;
        
        printi64(tmp, original_name_count);
        prints8(tmp, S("\t"));
        prints8(tmp, display);
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
    
    // Parse the temp file into the new names array
    s8 *new_names = parse_temp_file(perm, input, original_name_count, err);
    
    // Compute the plan
    Plan plan = compute_plan(perm, original_names, new_names, original_name_count);
    
    // Execute the plan
    scratch = *perm;
    scratch.beg = perm->beg;  // Start scratch from current position, don't overlap permanent data
    b32 success = execute_plan(plan, scratch, perm->ctx, out, err, verbose);
    
    flush(out);
    flush(err);
    
    if (!success) {
        // TODO: Could set exit code here
    }
}
