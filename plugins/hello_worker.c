/* Build with: ./forge plugin build plugins/hello_worker.c
   This worker talks JSON-lines protocol v2 over stdin/stdout. */
#include <stdio.h>
#include <string.h>

int main(void) {
    char event[4096];
    while (fgets(event, sizeof(event), stdin)) {
        if (strstr(event, "\"event\":\"save\"")) {
            puts("{\"protocol\":2,\"action\":\"notify\",\"message\":\"C worker saw a save\"}");
            fflush(stdout);
        }
    }
    return 0;
}
