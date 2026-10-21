#ifndef _DIRENT_H
#define _DIRENT_H
typedef struct _DIR DIR;
struct dirent {
    char d_name[256];
};
DIR           *opendir(const char *path);
struct dirent *readdir(DIR *d);
int            closedir(DIR *d);
#endif
