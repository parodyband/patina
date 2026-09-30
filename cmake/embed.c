/* Build-time helper: writes a file as a C array initializer (decimal bytes plus a terminating 0) for
 * `static const unsigned char x[] = {
 * #include "x.inc"
 * };`
 * No size limits (unlike string literals on MSVC) and bytes are kept exactly. usage: embed <in> <out> */
#include <stdio.h>

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: embed <in> <out>\n");
    return 2;
  }
  FILE* in = fopen(argv[1], "rb");
  if (!in) { perror(argv[1]); return 1; }
  FILE* out = fopen(argv[2], "wb");
  if (!out) { perror(argv[2]); fclose(in); return 1; }
  static unsigned char buf[1 << 16];
  size_t n, col = 0;
  while ((n = fread(buf, 1, sizeof buf, in)) > 0)
    for (size_t i = 0; i < n; i++) {
      fprintf(out, "%u,", buf[i]);
      if (++col % 40 == 0) fputc('\n', out);
    }
  fputs("0\n", out);
  fclose(in);
  return fclose(out) == 0 ? 0 : 1;
}
