#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "customstructs.h"

ConfFileList* conf_list_create(size_t initial_capacity) {
    ConfFileList *list = malloc(sizeof(ConfFileList));
    if (!list) return NULL;
    
    list->files = malloc(initial_capacity * sizeof(ConfFile));
    if (!list->files) {
        free(list);
        return NULL;
    }
    
    list->count = 0;
    list->capacity = initial_capacity;
    return list;
}

int conf_list_add(ConfFileList *list, const char *dir_path, const char *filename) {
    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity * 2;
        ConfFile *new_files = realloc(list->files, new_capacity * sizeof(ConfFile));
        if (!new_files) return -1;
        list->files = new_files;
        list->capacity = new_capacity;
    }
    
    list->files[list->count].filename = strdup(filename);
    
    size_t path_len = strlen(dir_path) + strlen(filename) + 2;
    list->files[list->count].filepath = malloc(path_len);
    snprintf(list->files[list->count].filepath, path_len, "%s/%s", dir_path, filename);
    
    list->count++;
    return 0;
}
int conf_file_compare(const void *a, const void *b) {
    const ConfFile *file_a = (const ConfFile *)a;
    const ConfFile *file_b = (const ConfFile *)b;
    return strcmp(file_a->filename, file_b->filename);
}

void conf_list_destroy(ConfFileList *list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) {
        free(list->files[i].filename);
        free(list->files[i].filepath);
    }
    free(list->files);
    free(list);
}