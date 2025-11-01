#ifndef SPELL_H
#define SPELL_H

#include <stddef.h>

#define DEFAULT_SUFFIX ".txt"

typedef struct {
    char *word; 
    char *lower; 
} DictEntry;

#endif
