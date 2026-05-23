#ifndef FORGE_GIT_H
#define FORGE_GIT_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* ── Diff line info ─────────────────────────────────────────── */

#define GIT_MAX_DIFF_LINES   8192
#define GIT_MAX_COMMITS      512
#define GIT_MAX_BLAME_LINES  8192

typedef enum {
    GIT_DIFF_NONE,       /* unchanged */
    GIT_DIFF_ADDED,      /* new line */
    GIT_DIFF_DELETED,    /* deleted (shown in gutter only) */
    GIT_DIFF_MODIFIED    /* changed line */
} GitDiffStatus;

typedef struct {
    int            line;     /* 0-indexed line in current file */
    GitDiffStatus  status;
} GitDiffLine;

/* ── Commit info ────────────────────────────────────────────── */

typedef struct {
    char   sha[41];          /* hex SHA-1 */
    char   short_sha[8];     /* first 7 chars */
    char   message[256];     /* first line of commit message */
    char   author[128];
    time_t date;
} GitCommit;

/* ── Blame line info ────────────────────────────────────────── */

typedef struct {
    char   author[128];
    time_t date;
    char   sha[41];
    char   summary[256];
} GitBlameLine;

/* ── Git state ──────────────────────────────────────────────── */

typedef struct {
    bool  repo_open;            /* is a git repo available? */
    char  branch[128];          /* current branch name */

    /* Working-tree diff vs HEAD */
    GitDiffLine  diff[GIT_MAX_DIFF_LINES];
    int          diff_count;

    /* Commit history for current file */
    GitCommit    commits[GIT_MAX_COMMITS];
    int          commit_count;

    /* Timeline state */
    bool  timeline_visible;
    int   timeline_selected;    /* index into commits[] */
    bool  timeline_viewing;     /* viewing file at a past commit? */
    char  timeline_file_content[1]; /* placeholder; actually allocated */

    /* Blame data for current file */
    GitBlameLine blame[GIT_MAX_BLAME_LINES];
    int          blame_count;
    bool         blame_visible;

    /* Internal: libgit2 repo handle (opaque here to avoid header dep) */
    void *repo_handle;
} GitState;

/* ── Public API ─────────────────────────────────────────────── */

/* Open the repo at the given working directory. Returns true on success. */
bool git_state_init(GitState *gs, const char *workdir);

/* Close the repo handle and free resources */
void git_state_free(GitState *gs);

/* Refresh the diff for a given file (relative to repo root).
   Populates gs->diff[] and gs->diff_count. */
void git_refresh_diff(GitState *gs, const char *filepath);

/* Get the commit log for a given file.
   Populates gs->commits[] and gs->commit_count. */
void git_refresh_log(GitState *gs, const char *filepath);

/* Get file contents at a specific commit SHA.
   Returns malloc'd string (caller frees), or NULL on failure. */
char *git_file_at_commit(GitState *gs, const char *filepath,
                         const char *sha);

/* Get blame data for a file.
   Populates gs->blame[] and gs->blame_count. */
void git_refresh_blame(GitState *gs, const char *filepath);

/* Get current branch name. Populates gs->branch. */
void git_refresh_branch(GitState *gs);

/* Get diff status for a specific line (0-indexed). */
GitDiffStatus git_line_diff_status(GitState *gs, int line);

#endif
