#define FUSE_USE_VERSION  26
#define _FILE_OFFSET_BITS 64

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

//----------------------------------------------------------------------------------------------------------------->
//Size of a disk block
#define BLOCK_SIZE 512

#define MAX_FILENAME 8
#define MAX_EXTENSION 3

//How many files can there be in one directory?
#define MAX_FILES_IN_DIR (BLOCK_SIZE - (MAX_FILENAME + 1) - sizeof(int)) / \
    ((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))

//How much data can one block hold?
#define MAX_DATA_IN_BLOCK (BLOCK_SIZE)

struct mkfs_directory_entry {
    char dname[MAX_FILENAME + 1]; //The directory name (plus space for a nul)
    int nFiles; //How many files are in this directory

    struct mkfs_file_directory {
        char fname[MAX_FILENAME + 1]; //Filename (plus space for nul)
        char fext[MAX_EXTENSION + 1]; //Extension (plus space for nul)
        size_t fsize; //File size
        long nStartBlock; //Where the first block is on disk
    } files[MAX_FILES_IN_DIR]; //There is an array of these
};

int last_allocation_start = 0;
typedef struct mkfs_directory_entry mkfs_directory_entry;
typedef struct mkfs_file_directory mkfs_file_directory;

struct mkfs_disk_block {
    char data[MAX_DATA_IN_BLOCK]; //Data storage
};

typedef struct mkfs_disk_block mkfs_disk_block;
//----------------------------------------------------------------------------------------------------------------->

static void *_init(struct fuse_conn_info * conn);
static void _destroy(void *a);
static int _getattr(const char *path, struct stat *stbuf);
static int _readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi);
static int _mkdir(const char *path, mode_t mode);
static int _rmdir(const char *path);
static int _mknod(const char *path, mode_t mode, dev_t dev);
static int _unlink(const char *path);
static int _read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info * fi);
static int _write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
static int _open(const char *path, struct fuse_file_info *fi);
static int _flush (const char *path , struct fuse_file_info *fi);
static int _truncate(const char *path, off_t size);

static struct fuse_operations oper = {
    .destroy = _destroy,
    .init = _init,
    .getattr = _getattr,
    .readdir = _readdir,
    .mkdir = _mkdir,
    .rmdir = _rmdir,
    .mknod = _mknod,
    .unlink = _unlink,
    .read = _read,
    .write = _write,
    .open = _open,
    .flush = _flush,
    .truncate = _truncate
};

int main(int argc, char *argv[]) {
    return fuse_main(argc, argv, &oper, NULL);
}

static void *_init(struct fuse_conn_info * conn) {
    printf("--------------------------------------------------------------------->MAX_FILES_IN_DIR = %d\n", MAX_FILES_IN_DIR);
    printf("--------------------------------------------------------------------->Filesystem has been initialized!\n");
    return NULL;
}

static void _destroy(void *a) {
    printf("--------------------------------------------------------------------->Filesystem has been destroyed!\n");
}