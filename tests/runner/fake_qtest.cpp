// stand-in for a QtTest program, used only by test_run_tests.py to check run_tests.bat.
// it takes -o file,txt and function names like QtTest, knows the functions alpha, beta
// (two data rows) and gamma, and prints QtTest-style result lines.  FAKE_MODE picks the
// behavior: pass, fail, noresults, crash, ignoresel, skip, xfail or blacklist.
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *known[] = {"alpha", "beta", "gamma"};

int main(int argc, char **argv)
{
    const char *self = argv[0];
    const char *cls = strstr(self, "test_core") ? "CoreTests" : strstr(self, "test_rules") ? "RulesTests"
                      : strstr(self, "test_ui") ? "UiTests" : "Unknown";
    char nscls[64];
    if (getenv("FAKE_NS")) {
        snprintf(nscls, sizeof nscls, "app::%s", cls);
        cls = nscls;
    }
    const char *mode = getenv("FAKE_MODE");
    if (!mode)
        mode = "pass";
    const char *log = getenv("FAKE_LOG");
    char out[1024] = "";
    const char *sel[16];
    int nsel = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            strncpy(out, argv[++i], sizeof out - 1);
            char *c = strrchr(out, ',');
            if (c)
                *c = 0;
        } else if (nsel < 16) {
            sel[nsel++] = argv[i];
        }
    }
    // every start is logged, so the harness can tell which programs ran and with what
    if (log) {
        FILE *l = fopen(log, "a");
        if (l) {
            fprintf(l, "%s", cls);
            for (int i = 0; i < nsel; ++i)
                fprintf(l, " %s", sel[i]);
            fprintf(l, " | data=%s\n", getenv("TCG_DATA_DIR") ? getenv("TCG_DATA_DIR") : "(none)");
            fclose(l);
        }
    }
    if (!strcmp(mode, "crash"))
        return 3;
    if (!strcmp(mode, "noresults"))
        return 0;
    FILE *f = out[0] ? fopen(out, "w") : stdout;
    if (!f)
        return 4;
    int pass = 0, fail = 0, skip = 0, rc = 0;
    fprintf(f, "********* Start testing of %s *********\n", cls);
    for (int i = 0; i < nsel; ++i) {
        int ok = 0;
        for (int k = 0; k < 3; ++k)
            ok |= !strcmp(sel[i], known[k]);
        if (!ok) {
            fprintf(stderr, "Unknown test function: '%s'.\n", sel[i]);
            fprintf(f, "FAIL!  : %s::%s() Function not found: %s\n", cls, sel[i], sel[i]);
            ++fail;
            rc = 1;
        }
    }
    fprintf(f, "PASS   : %s::initTestCase()\n", cls);
    ++pass;
    for (int k = 0; k < 3; ++k) {
        int run = nsel == 0;
        for (int i = 0; i < nsel; ++i)
            run |= !strcmp(sel[i], known[k]);
        if (!strcmp(mode, "ignoresel") || !run)
            continue;
        if (!strcmp(mode, "skip")) {
            // as QtTest 6.11 prints a skipped function: no pass line follows
            fprintf(f, "SKIP   : %s::%s() not available here\n", cls, known[k]);
            ++skip;
        } else if (!strcmp(mode, "xfail")) {
            fprintf(f, "XFAIL  : %s::%s() known problem\n", cls, known[k]);
            fprintf(f, "PASS   : %s::%s()\n", cls, known[k]);
            ++pass;
        } else if (!strcmp(mode, "blacklist")) {
            fprintf(f, "BPASS  : %s::%s()\n", cls, known[k]);
            ++pass;
        } else if (!strcmp(known[k], "beta")) {
            fprintf(f, "PASS   : %s::beta(x)\n", cls);
            ++pass;
            if (!strcmp(mode, "fail")) {
                fprintf(f, "FAIL!  : %s::beta(y) Compared values are not the same\n", cls);
                ++fail;
                rc = 1;
            } else {
                fprintf(f, "PASS   : %s::beta(y)\n", cls);
                ++pass;
            }
        } else {
            fprintf(f, "QDEBUG : %s::%s() some debug output\n", cls, known[k]);
            fprintf(f, "PASS   : %s::%s()\n", cls, known[k]);
            ++pass;
        }
    }
    fprintf(f, "PASS   : %s::cleanupTestCase()\n", cls);
    ++pass;
    fprintf(f, "Totals: %d passed, %d failed, %d skipped, 0 blacklisted, 1ms\n", pass, fail, skip);
    fprintf(f, "********* Finished testing of %s *********\n", cls);
    if (f != stdout)
        fclose(f);
    return rc;
}
