/*
 * rs_test.cpp: host tests for rSharp's interpreter (apps/rsharp/engine).
 *
 *   tests/run-tests.sh                    build (with ASan/UBSan) and run cases.txt
 *   build/rs_test tests/cases.txt [name]  run the cases (whose name contains name)
 *   build/rs_test -e 'code'               run one snippet, print its output
 *
 * cases.txt: "### name [views]" starts a case, then its code, a line "---",
 * then the expected output. [views] turns on the hex/binary result lines.
 */
#include "../engine/rs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

static std::vector<rs_char> ToUtf16(const std::string &s)
{
    std::vector<rs_char> out;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = s[i];
        unsigned cp;
        if (c < 0x80) { cp = c; i++; }
        else if ((c & 0xE0) == 0xC0) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); i += 2; }
        else if ((c & 0xF0) == 0xE0) { cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); i += 3; }
        else { cp = '?'; i += 4; }
        out.push_back((rs_char)cp);
    }
    return out;
}

static std::string ToUtf8(const rs_char *c, int n)
{
    std::string s;
    for (int i = 0; i < n; i++) {
        unsigned cp = c[i];
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
        else { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    }
    return s;
}

static std::string Run(const std::string &aCode, bool aViews, int *aStatus)
{
    std::vector<rs_char> src = ToUtf16(aCode);
    rs_options opt;
    memset(&opt, 0, sizeof opt);
    opt.int_views = aViews;
    opt.stack_limit = 512 * 1024;
    const rs_char *out;
    int len;
    *aStatus = rs_run(src.data(), (int)src.size(), &opt, &out, &len);
    std::string r = ToUtf8(out, len);
    rs_free_output();
    return r;
}

static std::string TrimEnd(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\r')) s.pop_back();
    return s;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "-e")) {
        int st;
        std::string out = Run(argv[2], argc >= 4, &st);
        fputs(out.c_str(), stdout);
        return st;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: rs_test cases.txt [filter] | rs_test -e 'code' [views]\n");
        return 2;
    }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 2; }
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) all.append(buf, n);
    fclose(fp);

    struct Case { std::string name, code, expect; bool views; };
    std::vector<Case> cases;
    size_t pos = 0;
    Case *cur = NULL;
    bool inExpect = false;
    while (pos < all.size()) {
        size_t nl = all.find('\n', pos);
        std::string line = all.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? all.size() : nl + 1;
        if (line.compare(0, 4, "### ") == 0) {
            cases.push_back(Case());
            cur = &cases.back();
            cur->name = line.substr(4);
            cur->views = cur->name.find("[views]") != std::string::npos;
            inExpect = false;
            continue;
        }
        if (!cur) continue;
        if (line == "---" && !inExpect) { inExpect = true; continue; }
        (inExpect ? cur->expect : cur->code) += line + "\n";
    }
    int pass = 0, fail = 0;
    for (size_t i = 0; i < cases.size(); i++) {
        Case &c = cases[i];
        if (argc >= 3 && c.name.find(argv[2]) == std::string::npos) continue;
        int st;
        std::string got = TrimEnd(Run(c.code, c.views, &st));
        std::string want = TrimEnd(c.expect);
        if (got == want) {
            pass++;
        } else {
            fail++;
            printf("FAIL: %s\n--- code:\n%s--- expected:\n%s\n--- got:\n%s\n\n", c.name.c_str(), c.code.c_str(), want.c_str(), got.c_str());
        }
    }
    printf("%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
