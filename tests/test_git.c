/* libgit2 integration test: needs -DHAS_LIBGIT2 -lgit2 */
#include "../core/git.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sh(const char *cmd) { int rc = system(cmd); assert(rc == 0); }

int main(void) {
#ifndef HAS_LIBGIT2
    puts("git test skipped (built without libgit2)"); return 0;
#else
    char dir[] = "/tmp/forge_git_XXXXXX"; assert(mkdtemp(dir));
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
        "cd %s && git init -q -b main . && git config user.email t@t && git config user.name T && "
        "printf 'one\\ntwo\\n' > a.c && git add a.c && git commit -qm first && "
        "printf 'one\\ntwo\\nthree\\n' > a.c && git commit -qam second", dir);
    sh(cmd);

    GitState gs;
    memset(&gs, 0, sizeof gs);
    git_state_free(&gs);                       /* zeroed free must not unbalance libgit2 */
    assert(!git_state_init(&gs, "/tmp"));      /* not a repo: false, balanced */
    git_state_free(&gs);

    assert(git_state_init(&gs, dir) && gs.repo_open);
    assert(strcmp(gs.branch, "main") == 0);

    char path[512]; snprintf(path, sizeof path, "%s/a.c", dir);
    git_refresh_log(&gs, path);
    assert(gs.commit_count == 2 && strcmp(gs.commits[0].message, "second") == 0);

    char *old = git_file_at_commit(&gs, path, gs.commits[1].sha);
    assert(old && strcmp(old, "one\ntwo\n") == 0); free(old);

    git_refresh_blame(&gs, path);
    assert(gs.blame_count == 3);

    snprintf(cmd, sizeof cmd, "printf 'one\\nTWO\\nthree\\nfour\\n' > %s", path); sh(cmd);
    git_refresh_diff(&gs, path);
    assert(gs.diff_count > 0);
    assert(git_line_diff_status(&gs, 3) == GIT_DIFF_ADDED);

    git_state_free(&gs);
    assert(git_state_init(&gs, dir));          /* re-open after free works */
    git_state_free(&gs);
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir); sh(cmd);
    puts("git ok"); return 0;
#endif
}
