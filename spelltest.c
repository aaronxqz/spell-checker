/* spelltest.c - small test harness for spell
 *
 * The harness expects certain test files exist (see README/test plan). It runs
 * spell with those files and prints PASS/FAIL.
 *
 * Build: gcc -std=c11 -Wall -Wextra -O2 -o spelltest spelltest.c
 */
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run_and_capture(const char *cmd, const char *expected_output) {
    /* Use popen to capture output */
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        perror("popen");
        return -1;
    }
    char buf[8192];
    size_t pos = 0;
    buf[0] = '\0';
    while (fgets(buf + pos, sizeof(buf) - pos, fp) != NULL) {
        pos = strlen(buf);
        if (pos >= sizeof(buf) - 1) break;
    }
    int rc = pclose(fp);
    /* normalize expected and actual by trimming trailing newlines for simple compare */
    char *act = buf;
    while (strlen(act) > 0 && (act[strlen(act)-1] == '\n' || act[strlen(act)-1] == '\r'))
        act[strlen(act)-1] = '\0';
    char *exp = (char*)expected_output;
    while (strlen(exp) > 0 && (exp[strlen(exp)-1] == '\n' || exp[strlen(exp)-1] == '\r'))
        ((char*)exp)[strlen(exp)-1] = '\0';

    if (strcmp(act, exp) == 0) {
        return 0; /* PASS */
    } else {
        printf("COMMAND: %s\n", cmd);
        printf("EXPECTED:\n%s\n", expected_output);
        printf("GOT:\n%s\n", act);
        return 1; /* FAIL */
    }
}

int main(void) {
    int failures = 0;

    /* Example test 1: simple dictionary and a file with one bad word */
    failures += run_and_capture("./spell test_dict1.txt test1.txt", "test1.txt:1:1 foom");

    /* Example test 2: punctuation trimming and parentheses */
    failures += run_and_capture("./spell test_dict1.txt test2.txt", "test2.txt:1:6 Badd");

    /* Example test 3: directory traversal with suffix .tst */
    failures += run_and_capture("./spell -s .tst test_dict1.txt testdir", "testdir/sub.tst:1:1 almost-correkt");

    /* Summary */
    if (failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    } else {
        printf("%d TEST(S) FAILED\n", failures);
        return 2;
    }
}
