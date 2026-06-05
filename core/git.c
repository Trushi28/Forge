#include "git.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAS_LIBGIT2
/* ══════════════════════════════════════════════════════════════
   Full libgit2 implementation
   ══════════════════════════════════════════════════════════════ */

#include <git2.h>

/* ── Init / Free ───────────────────────────────────────────── */

bool git_state_init(GitState *gs, const char *workdir) {
    memset(gs, 0, sizeof(*gs));
    git_libgit2_init();

    git_repository *repo = NULL;
    int err = git_repository_open_ext(&repo, workdir, 0, NULL);
    if (err != 0) {
        gs->repo_open = false;
        return false;
    }

    gs->repo_handle = repo;
    gs->repo_open = true;
    gs->timeline_selected = 0;
    gs->timeline_visible = false;
    gs->timeline_viewing = false;
    gs->blame_visible = false;

    git_refresh_branch(gs);
    return true;
}

void git_state_free(GitState *gs) {
    if (gs->repo_handle)
        git_repository_free((git_repository *)gs->repo_handle);
    gs->repo_handle = NULL;
    gs->repo_open = false;
    git_libgit2_shutdown();
}

/* ── Branch ────────────────────────────────────────────────── */

void git_refresh_branch(GitState *gs) {
    gs->branch[0] = '\0';
    if (!gs->repo_open) return;

    git_repository *repo = (git_repository *)gs->repo_handle;
    git_reference *head = NULL;

    if (git_repository_head(&head, repo) == 0) {
        const char *name = git_reference_shorthand(head);
        if (name)
            snprintf(gs->branch, sizeof(gs->branch), "%s", name);
        git_reference_free(head);
    } else {
        snprintf(gs->branch, sizeof(gs->branch), "(no branch)");
    }
}

/* ── Diff — working tree vs HEAD ───────────────────────────── */

typedef struct {
    GitState   *gs;
    const char *target_path;
} DiffCtx;

static int diff_file_cb(const git_diff_delta *delta, float progress, void *payload) {
    (void)delta; (void)progress; (void)payload;
    return 0;
}

static int diff_hunk_cb(const git_diff_delta *delta, const git_diff_hunk *hunk,
                        void *payload) {
    (void)delta; (void)hunk; (void)payload;
    return 0;
}

static int diff_line_cb(const git_diff_delta *delta, const git_diff_hunk *hunk,
                        const git_diff_line *line, void *payload) {
    (void)hunk;
    DiffCtx *ctx = (DiffCtx *)payload;

    const char *path = delta->new_file.path ? delta->new_file.path
                                             : delta->old_file.path;
    if (!path || !ctx->target_path) return 0;

    const char *target_base = strrchr(ctx->target_path, '/');
    target_base = target_base ? target_base + 1 : ctx->target_path;
    const char *delta_base = strrchr(path, '/');
    delta_base = delta_base ? delta_base + 1 : path;

    if (strcmp(target_base, delta_base) != 0 &&
        strcmp(ctx->target_path, path) != 0)
        return 0;

    if (ctx->gs->diff_count >= GIT_MAX_DIFF_LINES) return 0;

    GitDiffLine *dl = &ctx->gs->diff[ctx->gs->diff_count];

    if (line->origin == GIT_DIFF_LINE_ADDITION) {
        dl->line   = line->new_lineno - 1;
        dl->status = GIT_DIFF_ADDED;
        ctx->gs->diff_count++;
    } else if (line->origin == GIT_DIFF_LINE_DELETION) {
        dl->line   = line->old_lineno - 1;
        dl->status = GIT_DIFF_DELETED;
        ctx->gs->diff_count++;
    }

    return 0;
}

void git_refresh_diff(GitState *gs, const char *filepath) {
    gs->diff_count = 0;
    if (!gs->repo_open || !filepath) return;

    git_repository *repo = (git_repository *)gs->repo_handle;
    git_diff *diff = NULL;

    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    opts.context_lines = 0;

    if (git_diff_index_to_workdir(&diff, repo, NULL, &opts) != 0) {
        git_object *head_obj = NULL;
        git_tree *head_tree = NULL;

        if (git_revparse_single(&head_obj, repo, "HEAD") == 0 &&
            git_commit_tree(&head_tree, (git_commit *)head_obj) == 0) {
            git_diff_tree_to_workdir_with_index(&diff, repo, head_tree, &opts);
            git_tree_free(head_tree);
        }
        if (head_obj) git_object_free(head_obj);
    }

    if (!diff) return;

    DiffCtx ctx = { .gs = gs, .target_path = filepath };
    git_diff_foreach(diff, diff_file_cb, NULL, diff_hunk_cb, diff_line_cb, &ctx);
    git_diff_free(diff);
}

/* ── Log — commit history ──────────────────────────────────── */

void git_refresh_log(GitState *gs, const char *filepath) {
    gs->commit_count = 0;
    if (!gs->repo_open) return;
    (void)filepath;

    git_repository *repo = (git_repository *)gs->repo_handle;
    git_revwalk *walker = NULL;

    if (git_revwalk_new(&walker, repo) != 0) return;

    git_revwalk_sorting(walker, GIT_SORT_TIME);
    git_revwalk_push_head(walker);

    git_oid oid;
    while (git_revwalk_next(&oid, walker) == 0 &&
           gs->commit_count < GIT_MAX_COMMITS) {
        git_commit *commit = NULL;
        if (git_commit_lookup(&commit, repo, &oid) != 0) continue;

        GitCommit *gc = &gs->commits[gs->commit_count];

        git_oid_tostr(gc->sha, sizeof(gc->sha), &oid);
        memcpy(gc->short_sha, gc->sha, 7);
        gc->short_sha[7] = '\0';

        const char *msg = git_commit_message(commit);
        if (msg) {
            const char *nl = strchr(msg, '\n');
            int mlen = nl ? (int)(nl - msg) : (int)strlen(msg);
            if (mlen >= (int)sizeof(gc->message))
                mlen = (int)sizeof(gc->message) - 1;
            memcpy(gc->message, msg, mlen);
            gc->message[mlen] = '\0';
        }

        const git_signature *sig = git_commit_author(commit);
        if (sig && sig->name)
            snprintf(gc->author, sizeof(gc->author), "%s", sig->name);

        gc->date = (sig) ? sig->when.time : 0;

        gs->commit_count++;
        git_commit_free(commit);
    }

    git_revwalk_free(walker);
}

/* ── File at commit ────────────────────────────────────────── */

char *git_file_at_commit(GitState *gs, const char *filepath,
                         const char *sha) {
    if (!gs->repo_open || !filepath || !sha) return NULL;

    git_repository *repo = (git_repository *)gs->repo_handle;
    git_oid oid;
    if (git_oid_fromstr(&oid, sha) != 0) return NULL;

    git_commit *commit = NULL;
    if (git_commit_lookup(&commit, repo, &oid) != 0) return NULL;

    git_tree *tree = NULL;
    if (git_commit_tree(&tree, commit) != 0) {
        git_commit_free(commit);
        return NULL;
    }

    git_tree_entry *entry = NULL;
    const char *basename = strrchr(filepath, '/');
    basename = basename ? basename + 1 : filepath;

    if (git_tree_entry_bypath(&entry, tree, filepath) != 0) {
        if (git_tree_entry_bypath(&entry, tree, basename) != 0) {
            git_tree_free(tree);
            git_commit_free(commit);
            return NULL;
        }
    }

    git_blob *blob = NULL;
    if (git_blob_lookup(&blob, repo, git_tree_entry_id(entry)) != 0) {
        git_tree_entry_free(entry);
        git_tree_free(tree);
        git_commit_free(commit);
        return NULL;
    }

    size_t content_size = git_blob_rawsize(blob);
    const char *content = (const char *)git_blob_rawcontent(blob);

    char *result = malloc(content_size + 1);
    memcpy(result, content, content_size);
    result[content_size] = '\0';

    git_blob_free(blob);
    git_tree_entry_free(entry);
    git_tree_free(tree);
    git_commit_free(commit);

    return result;
}

/* ── Blame ─────────────────────────────────────────────────── */

void git_refresh_blame(GitState *gs, const char *filepath) {
    gs->blame_count = 0;
    if (!gs->repo_open || !filepath) return;

    git_repository *repo = (git_repository *)gs->repo_handle;
    git_blame *blame = NULL;

    git_blame_options opts = GIT_BLAME_OPTIONS_INIT;

    if (git_blame_file(&blame, repo, filepath, &opts) != 0) {
        const char *basename = strrchr(filepath, '/');
        basename = basename ? basename + 1 : filepath;
        if (git_blame_file(&blame, repo, basename, &opts) != 0)
            return;
    }

    uint32_t hunk_count = git_blame_get_hunk_count(blame);

    for (uint32_t i = 0; i < hunk_count && gs->blame_count < GIT_MAX_BLAME_LINES; i++) {
        const git_blame_hunk *hunk = git_blame_get_hunk_byindex(blame, i);
        if (!hunk) continue;

        for (size_t line = hunk->final_start_line_number;
             line < hunk->final_start_line_number + hunk->lines_in_hunk &&
             gs->blame_count < GIT_MAX_BLAME_LINES;
             line++) {

            GitBlameLine *bl = &gs->blame[gs->blame_count];

            git_oid_tostr(bl->sha, sizeof(bl->sha), &hunk->final_commit_id);

            if (hunk->final_signature && hunk->final_signature->name)
                snprintf(bl->author, sizeof(bl->author), "%s",
                         hunk->final_signature->name);
            else
                snprintf(bl->author, sizeof(bl->author), "unknown");

            bl->date = hunk->final_signature ? hunk->final_signature->when.time : 0;

            git_commit *commit = NULL;
            if (git_commit_lookup(&commit, repo, &hunk->final_commit_id) == 0) {
                const char *msg = git_commit_message(commit);
                if (msg) {
                    const char *nl = strchr(msg, '\n');
                    int mlen = nl ? (int)(nl - msg) : (int)strlen(msg);
                    if (mlen >= (int)sizeof(bl->summary))
                        mlen = (int)sizeof(bl->summary) - 1;
                    memcpy(bl->summary, msg, mlen);
                    bl->summary[mlen] = '\0';
                }
                git_commit_free(commit);
            }

            gs->blame_count++;
        }
    }

    git_blame_free(blame);
}

#else /* !HAS_LIBGIT2 */
/* ══════════════════════════════════════════════════════════════
   No-op stubs when libgit2 is not available.
   The editor works fine without git features — no diff gutter,
   no blame, no timeline, no branch display.
   ══════════════════════════════════════════════════════════════ */

bool git_state_init(GitState *gs, const char *workdir) {
    (void)workdir;
    memset(gs, 0, sizeof(*gs));
    gs->repo_open = false;
    return false;
}

void git_state_free(GitState *gs) {
    gs->repo_open = false;
}

void git_refresh_branch(GitState *gs) {
    gs->branch[0] = '\0';
}

void git_refresh_diff(GitState *gs, const char *filepath) {
    (void)filepath;
    gs->diff_count = 0;
}

void git_refresh_log(GitState *gs, const char *filepath) {
    (void)filepath;
    gs->commit_count = 0;
}

char *git_file_at_commit(GitState *gs, const char *filepath,
                         const char *sha) {
    (void)gs; (void)filepath; (void)sha;
    return NULL;
}

void git_refresh_blame(GitState *gs, const char *filepath) {
    (void)filepath;
    gs->blame_count = 0;
}

#endif /* HAS_LIBGIT2 */

/* ══════════════════════════════════════════════════════════════
   Query — works regardless of libgit2 presence
   ══════════════════════════════════════════════════════════════ */

GitDiffStatus git_line_diff_status(GitState *gs, int line) {
    for (int i = 0; i < gs->diff_count; i++) {
        if (gs->diff[i].line == line)
            return gs->diff[i].status;
    }
    return GIT_DIFF_NONE;
}
