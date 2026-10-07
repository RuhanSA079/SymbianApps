#!/bin/sh
# After tools/cc-check.sh: list symbols referenced by the objects in build/obj
# but defined by none of them and not by libc (i.e. what the platform layer
# still has to provide). Runs INSIDE the container, from apps/rssh.
. /opt/symbian/gnupoc/gnupoc-common.sh
export LC_ALL=C
arm-none-symbianelf-nm -g --defined-only build/obj/*.o | awk 'NF==3{print $3}' | sort -u > build/defined.txt
arm-none-symbianelf-nm -u build/obj/*.o | awk 'NF>=2{print $2}' | sort -u > build/undef.txt
comm -23 build/undef.txt build/defined.txt \
  | grep -vE '^(__aeabi_.*|__assert|__errno|abort|abs|atoi|fclose|ferror|fflush|fgetc|fgets|fprintf|fputc|fputs|fread|free|fwrite|gmtime|isdigit|isspace|iswprint|isxdigit|localtime|malloc|mbrtowc|memchr|memcmp|memcpy|memmove|memset|qsort|realloc|setlocale|sprintf|sscanf|strcasecmp|strcat|strchr|strcmp|strcpy|strcspn|strerror|strftime|strlen|strncasecmp|strncmp|strrchr|strspn|strstr|strtol|strtoul|time|tolower|vsnprintf|wcrtomb|wcslen|wcsncmp|mbstowcs|wcstombs|mbtowc|wctomb|mblen|stat|open|close|unlink|rename|mkdir|getenv|isalpha|isalnum|isupper|islower|toupper|isprint|iscntrl|ispunct|wcwidth|towlower|towupper|strdup|strtoull|strtoll|snprintf|vsprintf|calloc|exit|fopen|fdopen|fseek|ftell|rewind|getpid|gettimeofday|isthreaded|__isthreaded|__sF|__stdin|__stdout|__stderr|__locale_mb_cur_max|___mb_cur_max|nl_langinfo|wcscpy|wcscmp|memrchr|bsearch|strtod)$' \
  > build/platform-missing.txt
wc -l < build/platform-missing.txt
