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
} Plan;

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
    i32      value;      // 1-based array index (0 = not found)
};

// Insert or lookup a path in the map (path -> array index)
static i32 *pathmap_insert(pathmap **m, s8 key, arena *perm)
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
    (*m)->value = 0;
    return &(*m)->value;
}

// Lookup a path in the reverse map
static i32 *pathmap_lookup(pathmap **m, s8 key)
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

static b32  os_path_exists(arena scratch, s8 path);

// File system state tracker to cache OS queries
typedef struct {
    pathmap *existing_files;  // Maps path -> 1 if file exists
    arena *perm;
} fsstate;

static fsstate *new_fsstate(arena *perm)
{
    fsstate *fs = new(perm, fsstate, 1);
    fs->existing_files = 0;
    fs->perm = perm;
    return fs;
}

static void fsstate_mark_exists(fsstate *fs, s8 path)
{
    i32 *exists = pathmap_insert(&fs->existing_files, path, fs->perm);
    *exists = 1;
}

static void fsstate_mark_deleted(fsstate *fs, s8 path)
{
    i32 *exists = pathmap_insert(&fs->existing_files, path, fs->perm);
    *exists = 0;
}

static b32 fsstate_exists(fsstate *fs, s8 path)
{
    i32 *exists = pathmap_lookup(&fs->existing_files, path);
    if (exists) {
        return *exists != 0;
    }
    
    // First time seeing this path - query OS once and cache result
    arena scratch = *fs->perm;
    b32 file_exists = os_path_exists(scratch, path);
    
    // Cache the result
    i32 *cached = pathmap_insert(&fs->existing_files, path, fs->perm);
    *cached = file_exists ? 1 : 0;
    
    return file_exists;
}

// Generate a unique non-conflicting name for a file
static s8 fsstate_unique_name(fsstate *fs, s8 base_path)
{
    if (!fsstate_exists(fs, base_path)) {
        return base_path;  // No conflict
    }
    
    // Try base_path~, base_path~1, base_path~2, etc.
    iz max_len = base_path.len + 20;  // Room for ~999999...
    u8 *candidate = new(fs->perm, u8, max_len);
    
    // Copy base path
    for (iz i = 0; i < base_path.len; i++) {
        candidate[i] = base_path.s[i];
    }
    candidate[base_path.len] = '~';
    
    s8 candidate_path = {candidate, base_path.len + 1};
    if (!fsstate_exists(fs, candidate_path)) {
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
        if (!fsstate_exists(fs, candidate_path)) {
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

// Parse the temporary file into an array of names. Returns an array of exactly
// original_name_count items. Missing items (to be deleted) are null.
static s8 *parse_temp_file(arena *perm, u8input *input, iz original_name_count);

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
static s8 *parse_temp_file(arena *perm, u8input *input, iz original_name_count)
{
    s8 *names = new(perm, s8, original_name_count);
    
    for (;;) {
        s8 line = nextline(input);
        if (line.len == 0 && line.s == 0) break;  // EOF
        
        // Skip empty lines
        if (line.len == 0) continue;
        
        i32 parsed_line_num;
        s8 line_copy = line;
        if (!parse_temp_line(&line_copy, &parsed_line_num)) {
            // Unable to parse line - this is fatal in strict mode (like Perl vidir)
            assert(0 && "vidir: unable to parse line, aborting");
        }
        
        // Check if line number is in valid range [1, original_name_count]
        if (parsed_line_num < 1 || parsed_line_num > original_name_count) {
            // Invalid line number - this is fatal in strict mode (like Perl vidir)
            assert(0 && "vidir: unknown item number");
        }
        
        // Copy the path to permanent memory
        u8 *path_copy = new(perm, u8, line_copy.len + 1);
        for (iz i = 0; i < line_copy.len; i++) {
            path_copy[i] = line_copy.s[i];
        }
        path_copy[line_copy.len] = 0;  // null terminate
        
        // Store in the array (convert to 0-based index)
        names[parsed_line_num - 1] = (s8){path_copy, line_copy.len};
    }
    
    return names;
}

// Produce a sequence of operations necessary to achieve the new name set.
static Plan compute_plan(arena *perm, s8 *oldnames, s8 *newnames, iz num_names)
{
    Plan plan = {0};
    
    // Create filesystem state tracker
    fsstate *fs = new_fsstate(perm);
    
    // Initialize with all original files
    for (iz i = 0; i < num_names; i++) {
        fsstate_mark_exists(fs, oldnames[i]);
    }

    // Create reverse map: newname -> old index (for detecting cycles)
    pathmap *newname_map = 0;
    for (iz i = 0; i < num_names; i++) {
        if (newnames[i].s && newnames[i].len > 0) {
            i32 *old_idx = pathmap_insert(&newname_map, newnames[i], perm);
            *old_idx = (i32)(i + 1);  // Store 1-based index (0 means not found)
        }
    }
    
    // Resolve conflicts deterministically by generating unique names upfront
    s8 *resolved_names = new(perm, s8, num_names);
    for (iz i = 0; i < num_names; i++) {
        if (!newnames[i].s || newnames[i].len == 0) {
            resolved_names[i] = (s8){0}; // Mark for deletion
        } else if (s8equals(oldnames[i], newnames[i])) {
            // No change - use original name as-is
            resolved_names[i] = newnames[i];
        } else {
            // This is a rename - check if destination conflicts with existing files
            // But not with other files in our rename set (those are cycles, not conflicts)
            
            // Check if destination exists in filesystem AND is not one of our source files
            b32 is_filesystem_conflict = fsstate_exists(fs, newnames[i]);
            b32 is_our_source_file = 0;
            for (iz j = 0; j < num_names; j++) {
                if (s8equals(newnames[i], oldnames[j])) {
                    is_our_source_file = 1;
                    break;
                }
            }
            
            if (is_filesystem_conflict && !is_our_source_file) {
                // Real conflict with existing filesystem entry - generate unique name
                resolved_names[i] = fsstate_unique_name(fs, newnames[i]);
                fsstate_mark_exists(fs, resolved_names[i]);
            } else {
                // No real conflict (either destination is free, or it's a cycle) - use original destination
                resolved_names[i] = newnames[i];
                fsstate_mark_exists(fs, resolved_names[i]);
            }
        }
    }
    
    // Count operations needed
    iz action_count = 0;
    for (iz i = 0; i < num_names; i++) {
        if (!resolved_names[i].s || resolved_names[i].len == 0) {
            // This item should be deleted
            action_count++;
        } else if (!s8equals(oldnames[i], resolved_names[i])) {
            // This item is being renamed
            i32 *conflict_idx = pathmap_lookup(&newname_map, oldnames[i]);
            if (conflict_idx && *conflict_idx > 0 && (*conflict_idx - 1) != i) {
                // There's a cycle - we need stash/unstash
                action_count += 3;  // STASH, RENAME, UNSTASH
            } else {
                action_count++;  // Simple RENAME
            }
        }
    }
    
    plan.actions = new(perm, Action, action_count);
    plan.len = 0;
    
    // Track which items have been processed to handle cycles
    b32 *processed = new(perm, b32, num_names);
    for (iz i = 0; i < num_names; i++) {
        processed[i] = 0;
    }
    
    // First pass: handle simple renames and start cycles
    for (iz i = 0; i < num_names; i++) {
        if (processed[i]) continue;
        
        if (!resolved_names[i].s || resolved_names[i].len == 0) {
            // Delete item
            plan.actions[plan.len++] = (Action){OP_DELETE, oldnames[i], {0}};
            processed[i] = 1;
        } else if (!s8equals(oldnames[i], resolved_names[i])) {
            // Rename item (resolved_names[i] is valid and different from oldnames[i])
            // Check if this creates a cycle by following the chain
            i32 *conflict_idx = pathmap_lookup(&newname_map, oldnames[i]);
            if (conflict_idx && *conflict_idx > 0 && (*conflict_idx - 1) != i) {
                // Potential cycle detected - follow the chain to find all participants
                iz cycle_start = i;
                iz cycle_length = 0;
                iz *cycle_nodes = new(perm, iz, num_names);  // Allocate enough for worst case
                iz current = i;
                
                // Follow the cycle chain in the forward direction (where each file wants to go)
                do {
                    if (cycle_length >= num_names) {
                        // Cycle too long (shouldn't happen with valid input) - fall back to 2-element handling
                        cycle_length = 0;
                        break;
                    }
                    cycle_nodes[cycle_length++] = current;
                    
                    // Find where current file wants to go (forward direction)
                    s8 current_destination = newnames[current];
                    if (!current_destination.s || current_destination.len == 0) {
                        // This file is being deleted, not part of rename cycle
                        cycle_length = 0;
                        break;
                    }
                    
                    // Find which file currently occupies that destination
                    iz next_file = -1;
                    for (iz j = 0; j < num_names; j++) {
                        if (s8equals(oldnames[j], current_destination)) {
                            next_file = j;
                            break;
                        }
                    }
                    
                    if (next_file == -1) {
                        // Destination is not one of our files - not a cycle
                        cycle_length = 0;
                        break;
                    }
                    
                    current = next_file;
                } while (current != cycle_start && cycle_length < num_names);
                
                if (cycle_length > 1 && current == cycle_start) {
                    // Complete cycle found! Handle it properly
                    // For cycle A→B→C→A, we need: stash A, move C→A, move B→C, unstash A→B
                    
                    // Stash the first file to break the cycle
                    iz stash_file = cycle_nodes[0];
                    plan.actions[plan.len++] = (Action){OP_STASH, oldnames[stash_file], {0}};
                    processed[stash_file] = 1;
                    
                    // Perform renames in reverse order (last file in cycle moves first)
                    for (iz j = cycle_length - 1; j >= 1; j--) {
                        iz from_file = cycle_nodes[j];
                        iz to_file = cycle_nodes[j - 1];  
                        plan.actions[plan.len++] = (Action){OP_RENAME, oldnames[from_file], oldnames[to_file]};
                        processed[from_file] = 1;
                    }
                    
                    // Unstash the first file to complete the cycle (to where the last file was)
                    iz final_dest_file = cycle_nodes[cycle_length - 1];
                    plan.actions[plan.len++] = (Action){OP_UNSTASH, {0}, oldnames[final_dest_file]};
                    
                } else {
                    // No complete cycle detected - handle as simple rename
                    plan.actions[plan.len++] = (Action){OP_RENAME, oldnames[i], resolved_names[i]};
                    processed[i] = 1;
                }
            } else {
                // Simple rename
                plan.actions[plan.len++] = (Action){OP_RENAME, oldnames[i], resolved_names[i]};
                processed[i] = 1;
            }
        } else {
            // No change needed
            processed[i] = 1;
        }
    }
    
    return plan;
}

// Execute the plan 
static b32 execute_plan(Plan plan, arena scratch, os *ctx, u8buf *out, u8buf *err, b32 verbose)
{
    b32 has_errors = 0;
    s8 stash_name = {0};  // Track stashed file name
#if 0
    // Debug: print all actions
    for (iz i = 0; i < plan.len; i++) {
        Action *action = &plan.actions[i];
        prints8(err, S("DEBUG ACTION["));
        printi64(err, i);
        prints8(err, S("]: op="));
        printi64(err, action->op);
        prints8(err, S(" src='"));
        prints8(err, action->src);
        prints8(err, S("' dst_ptr=0x"));
        printi64(err, (i64)action->dst.s);
        prints8(err, S(" dst_len="));
        printi64(err, action->dst.len);
        prints8(err, S(" dst='"));
        if (action->dst.s && action->dst.len > 0) {
            prints8(err, action->dst);
        } else {
            prints8(err, S("(null/empty)"));
        }
        prints8(err, S("'\n"));
    }
    flush(err);
#endif

    for (iz i = 0; i < plan.len; i++) {
        Action *action = &plan.actions[i];
        
        switch (action->op) {
        case OP_DELETE:
            if (!os_delete_path(ctx, scratch, action->src)) {
                prints8(err, S("vidir: failed to remove "));
                prints8(err, action->src);
                prints8(err, S("\n"));
                has_errors = 1;
            } else if (verbose) {
                prints8(out, S("removed '"));
                prints8(out, action->src);
                prints8(out, S("'\n"));
            }
            break;
            
        case OP_RENAME:
            // Debug: check if dst is empty
            if (!action->dst.s || action->dst.len == 0) {
                prints8(err, S("vidir: DEBUG - empty destination path for src: "));
                prints8(err, action->src);
                prints8(err, S("\n"));
                has_errors = 1;
                break;
            }
            
            // Create destination directory if needed
            {
                s8 dst_dir = dirname_s8(action->dst);
                if (!s8equals(dst_dir, S(".")) && !os_path_is_dir(scratch, dst_dir)) {
                    if (!os_create_dir(ctx, scratch, dst_dir)) {
                        prints8(err, S("vidir: failed to create directory tree '"));
                        prints8(err, dst_dir);
                        prints8(err, S("' for destination '"));
                        prints8(err, action->dst);
                        prints8(err, S("'\n"));
                        has_errors = 1;
                        break;
                    }
                }
            }
            
            // Perform the rename (conflicts already resolved during planning)
            if (!os_rename_file(ctx, scratch, action->src, action->dst)) {
                prints8(err, S("vidir: failed to rename "));
                prints8(err, action->src);
                prints8(err, S(" to "));
                prints8(err, action->dst);
                prints8(err, S("\n"));
                has_errors = 1;
            } else if (verbose) {
                prints8(out, S("'"));
                prints8(out, action->src);
                prints8(out, S("' => '"));
                prints8(out, action->dst);
                prints8(out, S("'\n"));
            }
            break;
            
        case OP_STASH:
            // Generate unique stash name
            {
                iz stash_len = action->src.len + 20;  // Room for .vidir_stash_123456
                u8 *stash_buf = new(&scratch, u8, stash_len);
                iz pos = 0;
                for (iz j = 0; j < action->src.len; j++) {
                    stash_buf[pos++] = action->src.s[j];
                }
                s8 suffix = S(".vidir_stash");
                for (iz j = 0; j < suffix.len; j++) {
                    stash_buf[pos++] = suffix.s[j];
                }
                
                i32 counter = 0;
                stash_name = (s8){stash_buf, pos};
                
                while (os_path_exists(scratch, stash_name)) {
                    counter++;
                    pos = action->src.len + suffix.len;
                    i32 temp_counter = counter;
                    u8 digits[16];
                    iz digit_count = 0;
                    
                    do {
                        digits[digit_count++] = '0' + (temp_counter % 10);
                        temp_counter /= 10;
                    } while (temp_counter > 0);
                    
                    for (iz j = 0; j < digit_count; j++) {
                        stash_buf[pos + j] = digits[digit_count - 1 - j];
                    }
                    stash_name.len = pos + digit_count;
                }
            }
            
            if (!os_rename_file(ctx, scratch, action->src, stash_name)) {
                prints8(err, S("vidir: failed to stash "));
                prints8(err, action->src);
                prints8(err, S(" to "));
                prints8(err, stash_name);
                prints8(err, S("\n"));
                has_errors = 1;
            } else if (verbose) {
                prints8(out, S("stashed '"));
                prints8(out, action->src);
                prints8(out, S("' -> '"));
                prints8(out, stash_name);
                prints8(out, S("'\n"));
            }
            break;
            
        case OP_UNSTASH:
            if (!stash_name.s) {
                prints8(err, S("vidir: internal error - no stash to unstash\n"));
                has_errors = 1;
                break;
            }
            
            // Create destination directory if needed
            {
                s8 dst_dir = dirname_s8(action->dst);
                if (!s8equals(dst_dir, S(".")) && !os_path_is_dir(scratch, dst_dir)) {
                    if (!os_create_dir(ctx, scratch, dst_dir)) {
                        prints8(err, S("vidir: failed to create directory tree "));
                        prints8(err, dst_dir);
                        prints8(err, S("\n"));
                        has_errors = 1;
                        break;
                    }
                }
            }
            
            if (!os_rename_file(ctx, scratch, stash_name, action->dst)) {
                prints8(err, S("vidir: failed to unstash "));
                prints8(err, stash_name);
                prints8(err, S(" to "));
                prints8(err, action->dst);
                prints8(err, S("\n"));
                has_errors = 1;
            } else if (verbose) {
                prints8(out, S("unstashed '"));
                prints8(out, stash_name);
                prints8(out, S("' => '"));
                prints8(out, action->dst);
                prints8(out, S("'\n"));
            }
            stash_name = (s8){0};  // Clear stash name
            break;
        }
    }
    
    return !has_errors;
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
        
        original_names[original_name_count] = path;
        original_name_count++;
        
        printi64(tmp, original_name_count);
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
    
    // Parse the temp file into the new names array
    s8 *new_names = parse_temp_file(perm, input, original_name_count);
    
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
