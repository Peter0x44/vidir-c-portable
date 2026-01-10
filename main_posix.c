// POSIX platform layer for vidir
// This is free and unencumbered software released into the public domain.

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "vidir.c"

struct os {
    i32 temp_fd;      // File descriptor for temporary file
    u8 *temp_path;    // Path to temporary file
    i32 temp_path_len;
};

static s8 cuthead(s8 s, iz off) {
    assert(off >= 0);
    assert(off <= s.len);
    s.s += off;
    s.len -= off;
    return s;
}

static u8 *tocstr(arena *a, s8 s)
{
    u8 *z = new(a, u8, s.len + 1);
    memcpy(z, s.s, (size_t)s.len);
    z[s.len] = 0;
    return z;
}

static arena newarena_(os *ctx, iz cap)
{
    arena a = {0};
    a.beg = malloc((size_t)cap);
    if (!a.beg) {
        os_write(ctx, 2, S("vidir: failed to allocate memory\n"));
        os_exit(ctx, 1);
    }
    a.end = a.beg + cap;
    a.ctx = ctx;
    return a;
}

static config *newconfig_(os *ctx, i32 argc, u8 **argv)
{
    arena perm = newarena_(ctx, 1<<26);
    config *conf = new(&perm, config, 1);
    
    if (argc > 1) {
        // Skip argv[0], only pass actual arguments
        conf->nargs = argc - 1;
        conf->args = new(&perm, u8*, conf->nargs);
        
        for (i32 i = 1; i < argc; i++) {
            conf->args[i-1] = argv[i];
        }
    } else {
        conf->nargs = 0;
        conf->args = 0;
    }
    
    conf->perm = perm;
    
    return conf;
}

static void os_write(os *ctx, i32 fd, s8 s)
{
    assert(ctx);
    assert(fd > 0 && fd <= 3);
    
    // Map fd 3 to our temp file descriptor
    i32 actual_fd = (fd == 3) ? ctx->temp_fd : fd;
    
    while (s.len) {
        iz written = write(actual_fd, s.s, (size_t)s.len);
        if (written < 0) {
            return;  // Write error, silently fail
        }
        s = cuthead(s, written);
    }
}

static i32 os_read(os *ctx, i32 fd, u8 *buf, i32 len)
{
    assert(ctx);
    assert(buf);
    assert(len >= 0);
    assert(fd >= 0 && fd <= 3);
    
    // Map fd 3 to our temp file descriptor
    i32 actual_fd = (fd == 3) ? ctx->temp_fd : fd;
    
    iz nread = read(actual_fd, buf, (size_t)len);
    if (nread < 0) {
        return -1;
    }
    return (i32)nread;
}

static b32 os_path_is_dir(os *ctx, arena scratch, s8 path)
{
    assert(ctx);
    assert(path.s);
    (void)ctx;
    
    u8 *cstr = tocstr(&scratch, path);
    struct stat st;
    if (stat((char *)cstr, &st) != 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode);
}

static b32 os_path_exists(os *ctx, arena scratch, s8 path)
{
    assert(ctx);
    assert(path.s);
    (void)ctx;
    
    u8 *cstr = tocstr(&scratch, path);
    struct stat st;
    return stat((char *)cstr, &st) == 0;
}

static s8node *os_list_dir(os *ctx, arena *perm, s8 path)
{
    assert(ctx);
    assert(perm);
    assert(path.s);
    (void)ctx;
    
    arena scratch = *perm;
    u8 *cstr = tocstr(&scratch, path);
    DIR *dir = opendir((char *)cstr);
    if (!dir) {
        return 0;
    }
    
    s8node *head = 0;
    s8node **tail = &head;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != 0) {
        s8 name = s8fromcstr((u8 *)entry->d_name);
        
        // Skip "." and ".."
        if (name.len == 1 && name.s[0] == '.') continue;
        if (name.len == 2 && name.s[0] == '.' && name.s[1] == '.') continue;
        
        // Build full path: path + "/" + name
        iz separator_needed = (path.len > 0 && path.s[path.len-1] != '/') ? 1 : 0;
        iz full_len = path.len + separator_needed + name.len;
        
        u8 *full_path = new(perm, u8, full_len);
        iz pos = 0;
        
        // Copy directory path
        for (iz i = 0; i < path.len; i++) {
            full_path[pos++] = path.s[i];
        }
        
        // Add separator if needed
        if (separator_needed) {
            full_path[pos++] = '/';
        }
        
        // Copy filename
        for (iz i = 0; i < name.len; i++) {
            full_path[pos++] = name.s[i];
        }
        
        // Create node with full path
        s8node *node = new(perm, s8node, 1);
        node->str = (s8){full_path, full_len};
        node->next = 0;
        
        *tail = node;
        tail = &node->next;
    }
    
    closedir(dir);
    return head;
}

static void os_create_temp_file(os *ctx, arena *perm)
{
    // Get temp directory from environment, default to /tmp
    u8 *tmpdir = (u8 *)getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0]) {
        tmpdir = (u8 *)"/tmp";
    }
    
    // Build template: tmpdir + "/vidirXXXXXX"
    s8 dir = s8fromcstr(tmpdir);
    s8 suf = S("/vidirXXXXXX");
    
    ctx->temp_path_len = dir.len + suf.len;
    ctx->temp_path = new(perm, u8, ctx->temp_path_len + 1);
    
    for (iz i = 0; i < dir.len; i++) {
        ctx->temp_path[i] = dir.s[i];
    }
    for (iz i = 0; i < suf.len; i++) {
        ctx->temp_path[dir.len + i] = suf.s[i];
    }
    ctx->temp_path[ctx->temp_path_len] = 0;
    
    ctx->temp_fd = mkstemp((char *)ctx->temp_path);
    if (ctx->temp_fd < 0) {
        os_write(ctx, 2, S("vidir: failed to create temporary file\n"));
        os_exit(ctx, 1);
    }
}

static void os_close_temp_file(os *ctx)
{
    if (ctx->temp_fd >= 0) {
        close(ctx->temp_fd);
        ctx->temp_fd = -1;
    }
}

static void os_open_temp_file(os *ctx)
{
    if (ctx->temp_fd >= 0) {
        close(ctx->temp_fd);
    }
    
    ctx->temp_fd = open((char *)ctx->temp_path, O_RDONLY);
    if (ctx->temp_fd < 0) {
        os_write(ctx, 2, S("vidir: failed to open temporary file\n"));
        os_exit(ctx, 1);
    }
}

static void os_remove_temp_file(os *ctx)
{
    if (ctx->temp_fd >= 0) {
        close(ctx->temp_fd);
        ctx->temp_fd = -1;
    }
    
    if (ctx->temp_path_len > 0) {
        unlink((char *)ctx->temp_path);
        ctx->temp_path_len = 0;
    }
}

#ifndef DEFAULT_EDITOR
#define DEFAULT_EDITOR "vi"
#endif

static b32 os_invoke_editor(os *ctx, arena scratch)
{
    // Close temp file before invoking editor
    if (ctx->temp_fd >= 0) {
        close(ctx->temp_fd);
        ctx->temp_fd = -1;
    }
    
    // Get editor from environment, default to DEFAULT_EDITOR
    u8 *editor = (u8 *)getenv("VISUAL");
    if (!editor || !editor[0]) {
        editor = (u8 *)getenv("EDITOR");
    }
    if (!editor || !editor[0]) {
        editor = (u8 *)DEFAULT_EDITOR;
    }
    
    // Build the full command before forking
    s8 ed = s8fromcstr(editor);
    s8 space = S(" ");
    s8 temp = s8fromcstr(ctx->temp_path);
    
    // Allocate buffer for full command from scratch arena
    iz total = ed.len + space.len + temp.len + 1;
    char *full_cmd = (char *)new(&scratch, u8, total);
    
    iz pos = 0;
    for (iz i = 0; i < ed.len; i++) full_cmd[pos++] = (char)ed.s[i];
    for (iz i = 0; i < space.len; i++) full_cmd[pos++] = (char)space.s[i];
    for (iz i = 0; i < temp.len; i++) full_cmd[pos++] = (char)temp.s[i];
    full_cmd[pos] = 0;
    
    i32 pid = fork();
    if (pid < 0) {
        return 0;
    }
    
    if (pid == 0) {
        char *cmd[4];
        cmd[0] = "sh";
        cmd[1] = "-c";
        cmd[2] = full_cmd;
        cmd[3] = 0;
        
        execvp("sh", cmd);
        
        // If execvp returns, it failed - try to print a helpful message
        os_write(ctx, 2, S("vidir: cannot execute editor: "));
        os_write(ctx, 2, ed);
        os_write(ctx, 2, S("\n"));
        _exit(127);
    }
    
    // Parent process: wait for editor to finish
    i32 status;
    if (waitpid(pid, &status, 0) < 0) {
        return 0;
    }
    
    // Check if editor exited normally and with success
    if (!WIFEXITED(status)) {
        return 0;  // Editor was killed by signal
    }
    
    return WEXITSTATUS(status) == 0;
}

static b32 os_delete_path(os *ctx, arena scratch, s8 path)
{
    assert(ctx);
    assert(path.s);
    (void)ctx;
    
    u8 *cstr = tocstr(&scratch, path);
    
    // Try to remove as file first, then as directory
    if (unlink((char *)cstr) == 0) {
        return 1;
    }
    if (rmdir((char *)cstr) == 0) {
        return 1;
    }
    return 0;
}

static b32 os_create_dir(os *ctx, arena scratch, s8 path)
{
    assert(ctx);
    assert(path.s);
    (void)ctx;
    
    u8 *cstr = tocstr(&scratch, path);
    
    // Check if directory already exists
    struct stat st;
    if (stat((char *)cstr, &st) == 0 && S_ISDIR(st.st_mode)) {
        return 1;  // Already exists and is a directory
    }
    
    // Create directories iteratively by null-terminating at each slash
    for (iz i = 0; i < path.len; i++) {
        if (cstr[i] == '/') {
            u8 saved = cstr[i];
            cstr[i] = 0;
            
            // Skip empty path components
            if (i > 0) {
                if (stat((char *)cstr, &st) != 0) {
                    // Directory doesn't exist, create it
                    if (mkdir((char *)cstr, 0777) != 0) {
                        return 0;  // Failed to create
                    }
                } else if (!S_ISDIR(st.st_mode)) {
                    return 0;  // Exists but is not a directory
                }
            }
            
            cstr[i] = saved;  // Restore the slash
        }
    }
    
    // Create final directory if it doesn't exist
    if (stat((char *)cstr, &st) != 0) {
        return mkdir((char *)cstr, 0777) == 0;
    }
    
    return S_ISDIR(st.st_mode);
}

static b32 os_rename_file(os *ctx, arena scratch, s8 src, s8 dst)
{
    assert(ctx);
    assert(src.s);
    assert(dst.s);
    (void)ctx;
    
    u8 *src_cstr = tocstr(&scratch, src);
    u8 *dst_cstr = tocstr(&scratch, dst);
    
    return rename((char *)src_cstr, (char *)dst_cstr) == 0;
}

static void os_exit(os *ctx, i32 code)
{
    (void)ctx;
    exit(code);
}

int main(int argc, char **argv)
{
    os ctx[1] = {0};
    ctx->temp_fd = -1;
    
    config *conf = newconfig_(ctx, argc, (u8 **)argv);
    
    os_create_temp_file(ctx, &conf->perm);
    
    vidir(conf);
    
    return 0;
}
