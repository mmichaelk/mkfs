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

//Main functions-------------------------------------------------------------start->
void parse_path(const char* path, char* directory, char* filename, char* extension);
void touch(char* path);

int find_file(mkfs_directory_entry* dir, char* file_target, char* ext_target);
int find_dir(mkfs_directory_entry* dir_struct, char* dir_name);

int last_bitmap_index();
int get_state(int block_idx);
int get_bitmap_size();

void change_bit(int i, int sign);
void set(int i);
void unset(int i);

void allocate(int start_block, int num_blocks);
void unallocate(int start_block, int num_blocks);

void print_bitmap();
void check_bitmap();

int find_free_space(int num_blocks);
//Main functions---------------------------------------------------------------end->

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

//Implementation main functions------------------------------------------------------------------------------start->
void parse_path(const char* path, char* directory, char* filename, char* extension) {
    sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);
}

//returns the index of a file in a directory
int find_file(mkfs_directory_entry* dir, char* file_target, char* ext_target) {
    int i;
    for (i = 0; i < dir->nFiles; i++) {
        mkfs_file_directory cur_file = dir->files[i];
        if (strcmp(file_target, cur_file.fname) == 0 && strcmp(ext_target, cur_file.fext) == 0) {
            return i;
        }
    }
    return -1;
}

//returns index of directory entry in .dir
int find_dir(mkfs_directory_entry* dir_struct, char* dir_name) {
    FILE* f = fopen(".dir", "rb");
    fread(dir_struct, sizeof(*dir_struct), 1, f);
    
    while (strcmp(dir_struct->dname, dir_name) != 0 && !feof(f)) {
        int read = fread(dir_struct, sizeof(*dir_struct), 1, f);
        if (read == 0) break;
    }
    
    if (strcmp(dir_struct->dname, dir_name) == 0) {
        int result = ftell(f) - sizeof(*dir_struct);
        fclose(f);
        return result;
    } else {
        fclose(f);
        return -1;
    }
}

void touch(char* path) {
    FILE* f = fopen(path, "a");
    fclose(f);
}

//return last index of block from bitmap
int last_bitmap_index() {
    touch(".disk"); //just in case it wasn't precreated
    FILE* f = fopen(".disk", "r+b");
    fseek(f, 0, SEEK_END); //set position of stream to end
    int bytes_on_disk = ftell(f);
    int blocks_on_disk = bytes_on_disk / 512;
    fclose(f);
    return blocks_on_disk - 1;
}

//returns 1 if the block is allocated, otherwise 0
int get_state(int block_idx) {
	FILE* f = fopen(".disk", "r+b");
	int byte_idx = block_idx / 8;
	int bit_idx = block_idx % 8;
	fseek(f, byte_idx, SEEK_SET);
	unsigned char target_byte;
	fread(&target_byte, sizeof(char), 1, f);
	fclose(f);
	unsigned char operand = 1 << bit_idx;
	unsigned char result = target_byte & operand;
	result = result >> bit_idx;
	return result;
}

//calculates size (in blocks) of bitmap
int get_bitmap_size() {
    touch(".disk"); //just in case it wasn't precreated
    FILE* f = fopen(".disk", "r+b");
    fseek(f, 0, SEEK_END);
    int bytes_on_disk = ftell(f);
    int blocks_on_disk = bytes_on_disk / 512; //keep as is to round down so you don't have a half sized block at end
    int bitmap_bytes_needed = blocks_on_disk / 8 + 1;
    int bitmap_blocks_needed = bitmap_bytes_needed / 512 + 1;
    fclose(f);
    return bitmap_blocks_needed;
}

void change_bit(int i, int sign) {
    touch(".disk"); //just in case it wasn't precreated
    FILE* f = fopen(".disk", "r+b");
    
    int byte_to_set = i / 8;
    i = i % 8;
    
    fseek(f, byte_to_set, SEEK_SET);
    
    unsigned char curr_byte = 0;
    fread(&curr_byte, sizeof(char), 1, f);
    unsigned char operand = 1 << i;
    if (sign == -1) { //if we are unsetting the bit
        operand = ~operand;
        curr_byte = curr_byte & operand;
    } else { //if we are setting the bit
        curr_byte = curr_byte | operand;
    }
    fseek(f, -1, SEEK_CUR);
    fwrite(&curr_byte, sizeof(char), 1, f);
    fclose(f);
    return;
}

//wrappers for the change_bit
void set(int i) { //set the bit to 1
    change_bit(i, 1);
    return;
}

void unset(int i) { //set the bit to 0
    change_bit(i, -1);
    return;
}

void allocate(int start_block, int num_blocks) {
    int i;
    for (i = start_block; i < start_block + num_blocks; i++) {
        set(i);
    }
}

void unallocate(int start_block, int num_blocks) {
    int i;
    printf("--------------------------------------------------------------------->Unallocating %d starting at %d\n", num_blocks, start_block);
    for (i = start_block; i < start_block + num_blocks; i++) {
        unset(i);
    }
}

//just for test
void print_bitmap() {
    printf("---------------------------------------------------------------------start->print_bitmap\n");
    int i = 0;
    touch(".disk"); //just in case it wasn't precreated
    int size = last_bitmap_index(); //get the number of blocks in bitmap
    for (i = 0; i <= size; i++) { //for each byte in each block
        int this_bit = get_state(i);
        printf("%u", this_bit); 
        if (i % 20 == 19) {
            printf("\n");
        }
        if (i % 512 == 511) {
            printf("\n");
        }
    }
    printf("-----------------------------------------------------------------------end->print_bitmap\n");
    return;
}

//makes the bitmap if it doesnt exist
void check_bitmap() {
    touch(".disk"); //just in case it wasn't precreated
    FILE* f = fopen(".disk", "r+b");
    char first_char = 0;
    fseek(f, 0, SEEK_SET);
    fread(&first_char, sizeof(char), 1, f);
    if (first_char == 0) { //then the beginning of the bitmap is zero and thus the bitmap does not exist
        int bitmap_size = get_bitmap_size();
        int i = 0;
        printf("--------------------------------------------------------------------->Creating new bitmap of size %d\n", bitmap_size);
        for (i = 0; i < bitmap_size; i++) {
            set(i);
        }
        // print_bitmap();
    }
    fclose(f);
    return;
}

//finds contiguous free blocks
int find_free_space(int num_blocks) {
    int run_start = -1;
    int run_length = 0;
    int cur_block = 0;

    while (cur_block <= last_bitmap_index()) {
        int this_bit = get_state(cur_block);
        if (this_bit == 0) {
            if (run_start == -1) {
                run_start = cur_block;
            }
            run_length++;
            if (run_length >= num_blocks) {
                last_allocation_start = run_start;
                return run_start;
            }
        } else if (this_bit == 1) {
            run_length = 0;
            run_start = -1;
        }
        cur_block++;
    }
    return -1;
}
//Implementation main functions--------------------------------------------------------------------------------end->

static void *_init(struct fuse_conn_info * conn) {
    printf("--------------------------------------------------------------------->MAX_FILES_IN_DIR = %d\n", MAX_FILES_IN_DIR);
    printf("--------------------------------------------------------------------->Filesystem has been initialized!\n");
    return NULL;
}

static void _destroy(void *a) {
    printf("--------------------------------------------------------------------->Filesystem has been destroyed!\n");
}