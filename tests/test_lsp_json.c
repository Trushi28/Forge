/* Parser regression test: huge completion reply (arena must not move) and \u escapes.
   Includes lsp.c directly to reach its static helpers. */
#include "../core/lsp.c"
#include <assert.h>

int main(void) {
    LSPClient c; memset(&c, 0, sizeof c); c.running = true;

    /* 1. ~200KB completion reply: used to realloc the arena under live pointers */
    size_t cap = 1 << 20; char *js = malloc(cap); size_t n = 0;
    n += snprintf(js + n, cap - n, "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"items\":[");
    for (int i = 0; i < 3000; i++)
        n += snprintf(js + n, cap - n, "%s{\"label\":\"sym_%d\",\"kind\":3,\"detail\":\"int sym_%d(void)\"}", i ? "," : "", i, i);
    n += snprintf(js + n, cap - n, "]}}");
    c.pending[0].id = 7; c.pending[0].type = LSP_RESP_COMPLETION; c.pending_count = 1;
    handle_message(&c, js, (int)n);
    assert(c.completion_ready && c.completion_count == LSP_MAX_COMPLETIONS);
    assert(strcmp(c.completions[127].label, "sym_127") == 0 && c.completions[5].kind == 3);
    free(js);

    /* 2. \u escapes + surrogate pair in hover text */
    const char *h = "{\"id\":9,\"result\":{\"contents\":{\"kind\":\"plaintext\",\"value\":\"a\\u003cb\\n\\ud83d\\ude00 \\u00e9\"}}}";
    c.pending[0].id = 9; c.pending[0].type = LSP_RESP_HOVER; c.pending_count = 1;
    handle_message(&c, h, (int)strlen(h));
    assert(c.hover.valid && strcmp(c.hover.contents, "a<b\n\xF0\x9F\x98\x80 \xC3\xA9") == 0);

    /* 3. control chars are escaped on the way out */
    JsonWriter w; jw_init(&w); jw_string(&w, "x\x1b\b"); w.buf[w.len] = 0;
    assert(strcmp(w.buf, "\"x\\u001b\\u0008\",") == 0); jw_free(&w);
    puts("lsp json ok"); return 0;
}
