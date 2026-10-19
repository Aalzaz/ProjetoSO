#ifndef CUSTOMSTRUCTS_H
#define CUSTOMSTRUCTS_H

#include <stddef.h>

typedef struct {
    char *filepath;
    char *filename;
} ConfFile;

typedef struct {
    ConfFile *files;
    size_t count;
    size_t capacity;
} ConfFileList;

ConfFileList* conf_list_create(size_t initial_capacity);
int conf_list_add(ConfFileList *list, const char *dir_path, const char *filename);
int conf_file_compare(const void *a, const void *b);
void conf_list_destroy(ConfFileList *list);

#endif