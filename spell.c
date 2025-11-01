#define _POSIX_C_SOURCE 200809L
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include "spell.h"-

//using memcy() to implement strdup()
static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *r = malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n + 1);
    return r;
}

//to lowercase
static void lower_inplace(char *s) {
    for (; *s; ++s) *s = (char)tolower((unsigned char)*s);
}

//compare function for dir search
static int dictcmp(const void *a, const void *b) {
    const DictEntry *da = (const DictEntry*)a;
    const DictEntry *db = (const DictEntry*)b;
    return strcmp(da->lower, db->lower);
}

//read file descriptor into a malloced buffer
//return length in outlen
static char *read_all_fd(int fd, ssize_t *outlen) {
    const size_t CHUNK = 8192;
    size_t cap = CHUNK;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    size_t len = 0;
    while (1) {
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0) {
            free(buf);
            return NULL;
        }
        if (r == 0) break;
        len += (size_t)r;
        if (len == cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
    }
    char *nb = realloc(buf, len + 1);
    if (nb) buf = nb;
    buf[len] = '\0';
    if (outlen) *outlen = (ssize_t)len;
    return buf;
}

//load dictionary from path
//Returns array in *outEntries and count in *outCount
//free entries and strings
static int load_dictionary(const char *path, DictEntry **outEntries, size_t *outCount) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Could not open dictionary '%s': %s\n", path, strerror(errno));
        return -1;
    }
    ssize_t len = 0;
    char *content = read_all_fd(fd, &len);
    close(fd);
    if (!content) {
        fprintf(stderr, "Failed to read dictionary '%s'\n", path);
        return -1;
    }

    //count line
    size_t count = 0;
    for (ssize_t i = 0; i < len; ++i) if (content[i] == '\n') ++count;
    //if file doesn't end with newline
    if (len > 0 && content[len-1] != '\n') ++count;

    DictEntry *entries = calloc(count, sizeof(DictEntry));
    if (!entries) { free(content); return -1; }

    //parsing
    size_t idx = 0;
    char *start = content;
    for (ssize_t i = 0; i <= len; ++i) {
        if (i == len || content[i] == '\n') {
            content[i] = '\0';
            if (start[0] != '\0') {
                entries[idx].word = xstrdup(start);
                entries[idx].lower = xstrdup(start);
                if (!entries[idx].word || !entries[idx].lower) {
                    for (size_t j = 0; j < idx; ++j) {
                        free(entries[j].word);
                        free(entries[j].lower);
                    }
                    free(entries);
                    free(content);
                    return -1;
                }
                lower_inplace(entries[idx].lower);
                ++idx;
            }
            start = content + i + 1;
        }
    }
    
    if (idx != count) {
        DictEntry *tmp = realloc(entries, idx * sizeof(DictEntry));
        if (tmp) entries = tmp;
        count = idx;
    }
    //sort by lower-case
    qsort(entries, count, sizeof(DictEntry), dictcmp);

    *outEntries = entries;
    *outCount = count;
    free(content);
    return 0;
}

//find range of entries matching lowerkey
//return start index and count via out parameters.
//Uses binary search to find one match then expands.
 
static int find_lower_range(DictEntry *entries, size_t n, const char *lowerkey, size_t *out_start, size_t *out_len) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int cmp = strcmp(entries[mid].lower, lowerkey);
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= n || strcmp(entries[lo].lower, lowerkey) != 0) {
        *out_start = 0; *out_len = 0;
        return 0;
    }
    size_t start = lo;
    size_t end = lo + 1;
    while (start > 0 && strcmp(entries[start - 1].lower, lowerkey) == 0) --start;
    while (end < n && strcmp(entries[end].lower, lowerkey) == 0) ++end;
    *out_start = start;
    *out_len = end - start;
    return 0;
}

//Check capitalization constraint:
static int capitalization_matches(const char *dict, const char *word) {
    size_t ld = strlen(dict);
    size_t lw = strlen(word);
    if (ld != lw) return 0;
    for (size_t i = 0; i < ld; ++i) {
        unsigned char dc = (unsigned char)dict[i];
        unsigned char wc = (unsigned char)word[i];
        if (isalpha(dc)) {
            if (isupper(dc)) {
                //if dict[i] is uppercase letter then word[i] must equal dict[i]
                if (wc != dc) return 0;
            } else {
                //if dict[i] is lowercase letter then word[i] may be lower or upper of that letter
                if (tolower(wc) != dc) return 0;
            }
        } else {
            if (wc != dc) return 0;
        }
    }
    return 1;
}

/* Determine if a char is considered an opening punctuation to strip from start */
static int is_opening_punc(char c) {
    return c == '(' || c == '[' || c == '{' || c == '\'' || c == '"';
}

/* Determine if character is letter or digit */
static int is_letter_or_digit(char c) {
    return isalpha((unsigned char)c) || isdigit((unsigned char)c);
}

/* Process buffer content (text file), reporting misspellings.
 * filename: path string or "<stdin>"
 * entries, n: dictionary
 * returns 0 if file processed successfully (regardless of misspellings), -1 on I/O error
 * The function prints misspellings to stdout in the format file:line:col word
 */
static int process_buffer(const char *filename, const char *buf, ssize_t buflen, DictEntry *entries, size_t n, int *out_found_misspell) {
    ssize_t i = 0;
    ssize_t line = 1;
    ssize_t col = 1;
    int any_bad = 0;

    while (i < buflen) {
        /* skip whitespace but update line/col */
        while (i < buflen && isspace((unsigned char)buf[i])) {
            if (buf[i] == '\n') { ++line; col = 1; ++i; }
            else { ++col; ++i; }
        }
        if (i >= buflen) break;

        ssize_t wstart = i;
        ssize_t wcol = col;

        while (i < buflen && !isspace((unsigned char)buf[i])) {
            ++col; ++i;
        }
        ssize_t wend = i;
        ssize_t wlen = wend - wstart;
        if (wlen <= 0) continue;

        //copy the word into a small buffer word
        char *word = malloc(wlen + 1);
        if (!word) return -1;
        memcpy(word, buf + wstart, wlen);
        word[wlen] = '\0';

        //punctuation
        ssize_t trim_start = 0;
        while (trim_start < wlen && is_opening_punc(word[trim_start])) ++trim_start;

        ssize_t trim_end = wlen; 
        while (trim_end > trim_start) {
            char c = word[trim_end - 1];
            if (!is_letter_or_digit(c)) --trim_end;
            else break;
        }

        if (trim_start >= trim_end) {
            free(word);
            continue;
        }

        //colon
        ssize_t trimmed_col = wcol + trim_start;

        //trimmed word string
        size_t tlen = (size_t)(trim_end - trim_start);
        char *tword = malloc(tlen + 1);
        if (!tword) { free(word); return -1; }
        memcpy(tword, word + trim_start, tlen);
        tword[tlen] = '\0';

        int has_letter = 0;
        int has_digit = 0;

        for (size_t k = 0; k < tlen; ++k) {
            if (isalpha((unsigned char)tword[k])) has_letter = 1;
            if (isdigit((unsigned char)tword[k])) has_digit = 1;
        }
        if (!has_letter) {
            free(word);
            free(tword);
            continue;
        }

        char *lowerkey = xstrdup(tword);
        if (!lowerkey) { free(word); free(tword); return -1; }
        lower_inplace(lowerkey);

        size_t start_idx = 0, match_count = 0;
        find_lower_range(entries, n, lowerkey, &start_idx, &match_count);
        int matched = 0;
        if (match_count > 0) {
            for (size_t k = 0; k < match_count; ++k) {
                DictEntry *de = &entries[start_idx + k];
                if (capitalization_matches(de->word, tword)) {
                    matched = 1;
                    break;
                }
            }
        }
        if (!matched) {
            //report misspelling: filename:line:col word
            //If filename is NULL, use stdin
            if (filename) printf("%s:%zd:%zd %s\n", filename, line, trimmed_col, tword);
            else printf("%s:%zd:%zd %s\n", "<stdin>", line, trimmed_col, tword);
            any_bad = 1;
        }

        free(word);
        free(tword);
        free(lowerkey);
    }

    if (out_found_misspell) *out_found_misspell = any_bad;
    return 0;
}

//Process a single file path (open, read, process). 
//Returns 0 on opened successfully, -1 on open/read error.
//sets found_misspell flag (1 if any misspelling found).
static int process_file(const char *path, DictEntry *entries, size_t n, int *found_misspell) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Could not open file '%s': %s\n", path, strerror(errno));
        return -1;
    }

    ssize_t len = 0;
    char *buf = read_all_fd(fd, &len);
    close(fd);

    if (!buf) {
        fprintf(stderr, "Could not read file '%s'\n", path);
        return -1;
    }

    int local_bad = 0;
    int rc = process_buffer(path, buf, len, entries, n, &local_bad);

    if (rc == 0 && found_misspell) *found_misspell = local_bad;
    free(buf);
    return rc;
}

//check whether a filename should be considered (during directory traversal)
//if name ends with suffix, accept; else skip
static int name_accepted_by_suffix(const char *name, const char *suffix) {
    if (!name || !suffix) return 0;
    if (name[0] == '.') return 0;
    size_t nl = strlen(name);
    size_t sl = strlen(suffix);
    if (nl < sl) return 0;
    
    int result = strcmp(name + nl - sl, suffix) == 0;
    //printf("DEBUG: name_accepted_by_suffix: name='%s', suffix='%s', result=%d\n", name, suffix, result);  //debug
    
    return result;
}

static int traverse_dir_and_process(const char *path, const char *suffix, DictEntry *entries, size_t n, int *any_bad, int *any_open_error) {
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "Could not open directory '%s': %s\n", path, strerror(errno));
        if (any_open_error) *any_open_error = 1;
        return -1;
    }
    
    //printf("DEBUG: Scanning directory: %s\n", path);  // debug
    //printf("DEBUG: Looking for suffix: %s\n", suffix);  // debug
    
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        
        /* FIX: Skip ".", "..", and all hidden files */
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
            continue; // skip "." and ".."
        }
        if (name[0] == '.') continue; //skip other hidden files/dirs 
        
        //printf("DEBUG: Found file/dir: %s\n", name);  // debug

        //full path
        size_t len = strlen(path) + 1 + strlen(name) + 1;
        char *full = malloc(len);

        if (!full) { closedir(d); return -1; }
        snprintf(full, len, "%s/%s", path, name);
        
        struct stat st;
        if (lstat(full, &st) != 0) {
            fprintf(stderr, "Could not stat '%s': %s\n", full, strerror(errno));
            free(full);
            if (any_open_error) *any_open_error = 1;
            continue;
        }
        
        if (S_ISDIR(st.st_mode)) {
            traverse_dir_and_process(full, suffix, entries, n, any_bad, any_open_error);
        } else if (S_ISREG(st.st_mode)) {
            if (name_accepted_by_suffix(name, suffix)) {
                int file_bad = 0;

                if (process_file(full, entries, n, &file_bad) < 0) {
                    if (any_open_error) *any_open_error = 1;
                } else if (file_bad) {
                    if (any_bad) *any_bad = 1;
                }
            }
        }
        free(full);
    }
    closedir(d);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s [-s suffix] dictionary [file_or_dir ...]\n", argv[0]);
        return EXIT_FAILURE;
    }

    //parse -s
    const char *suffix = DEFAULT_SUFFIX;
    int argi = 1;
    if (argc >= 3 && strcmp(argv[1], "-s") == 0) {
        if (argc < 4) {
            fprintf(stderr, "-s requires an argument\n");
            return EXIT_FAILURE;
        }
        suffix = argv[2];
        argi = 3;
    }

    if (argi >= argc) {
        fprintf(stderr, "Missing dictionary argument\n");
        return EXIT_FAILURE;
    }

    const char *dictpath = argv[argi++];
    DictEntry *entries = NULL;
    size_t nentries = 0;
    if (load_dictionary(dictpath, &entries, &nentries) != 0) {
        return EXIT_FAILURE;
    }

    int overall_bad = 0;
    int any_open_error = 0;

    if (argi == argc) {
        //no files specified: read from stdin, read stdin
        ssize_t len = 0;
        char *buf = read_all_fd(0, &len);
        if (!buf) {
            fprintf(stderr, "Error reading stdin\n");

            for (size_t i = 0; i < nentries; ++i) { 
                free(entries[i].word); free(entries[i].lower); 
            }
            free(entries);
            return EXIT_FAILURE;
        }
        int local_bad = 0;
        process_buffer(NULL, buf, len, entries, nentries, &local_bad);
        overall_bad |= local_bad;
        free(buf);
    } else {
        //one or more paths case
        for (int i = argi; i < argc; ++i) {
            const char *path = argv[i];
            struct stat st;
            if (stat(path, &st) != 0) {
                fprintf(stderr, "Could not stat '%s': %s\n", path, strerror(errno));
                any_open_error = 1;
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                //traverse
                traverse_dir_and_process(path, suffix, entries, nentries, &overall_bad, &any_open_error);
            } else if (S_ISREG(st.st_mode)) {
                int file_bad = 0;
                if (process_file(path, entries, nentries, &file_bad) < 0) {
                    any_open_error = 1;
                } else if (file_bad) overall_bad = 1;
            } else {
                continue;
            }
        }
    }

    //clean up
    for (size_t i = 0; i < nentries; ++i) {
        free(entries[i].word);
        free(entries[i].lower);
    }
    free(entries);

    if (any_open_error) return EXIT_FAILURE;
    if (overall_bad) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
