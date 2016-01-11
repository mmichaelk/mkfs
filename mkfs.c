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

static int _getattr(const char *path, struct stat * stbuf) {
    printf("--------------------------------------------------------------------->GETATTR: %s\n", path);

    int res = 0;
    memset(stbuf, 0, sizeof(struct stat));

    touch(".dir"); //just in case it wasn't precreated

    FILE* f = fopen(".dir", "r");

    char dir_target[9];
    char file_target[9];
    char ext_target[4];

    dir_target[0] = 0;
    file_target[0] = 0;
    ext_target[0] = 0;
    parse_path(path, dir_target, file_target, ext_target);

    struct mkfs_directory_entry cur_dir;
    fread(&cur_dir, sizeof(cur_dir), 1, f);
    
    while (strcmp(cur_dir.dname, dir_target) != 0 && !feof(f)) {
        int read = fread(&cur_dir, sizeof(cur_dir), 1, f);
        if (read == 0) break;
    }
    fclose(f);

    if (strcmp(cur_dir.dname, dir_target) == 0 || strcmp(path, "/") == 0) {
        //if we are looking for directory attributes
        if (strlen(file_target) == 0) {
            stbuf->st_nlink = 2;
            stbuf->st_mode = S_IFDIR | 0755;
        } else { //if we are looking for a file which is there
            int file_index = find_file(&cur_dir, file_target, ext_target);

            if (file_index == -1) {
                res = -ENOENT;
            } else { //if we found file
                stbuf->st_mode = S_IFREG | 0666;
                stbuf->st_nlink = 1;
                stbuf->st_size = cur_dir.files[file_index].fsize;
                stbuf->st_blksize = 512;
                stbuf->st_blocks = cur_dir.files[file_index].fsize / 512;
                if (cur_dir.files[file_index].fsize % 512 != 0) {
                    stbuf->st_blocks++;
                }
            }
        }
    } else {
        res = -ENOENT;
    }
    return res;
}

static int _readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info * fi) {
    printf("--------------------------------------------------------------------->READDIR: %s\n", path);
    
    (void) offset;
    (void) fi;

    touch(".dir"); //just in case it wasn't precreated

    FILE* f = fopen(".dir", "r");
    struct mkfs_directory_entry cur_dir;

    if (strcmp(path, "/") == 0) {
        filler(buf, ".", NULL, 0);
        filler(buf, "..", NULL, 0);
        while (fread(&cur_dir, sizeof(cur_dir), 1, f) > 0 && !feof(f)) {
            filler(buf, cur_dir.dname, NULL, 0);
        }
    } else {
        int found = 0;
        while (fread(&cur_dir, sizeof(cur_dir), 1, f) > 0 && !feof(f)) {
            if (strcmp(cur_dir.dname, path + 1) == 0) {
                int i;
                for (i = 0; i < cur_dir.nFiles; i++) {
                    char full_name[13];
                    full_name[0] = 0;
                    strcat(full_name, cur_dir.files[i].fname);
                    if (strlen(cur_dir.files[i].fext) > 0) {
                        strcat(full_name, ".");
                    }
                    strcat(full_name, cur_dir.files[i].fext);
                    filler(buf, full_name, NULL, 0);
                }
                found = 1;
            }
        }
        if (!found) {
            fclose(f);
            return -ENOENT;
        }
    }
    fclose(f);
    return 0;
}

static int _mkdir(const char *path, mode_t mode) {
    printf("--------------------------------------------------------------------->MKDIR: %s\n", path);

    char dir_target[9];
    char file_target[9];
    char ext_target[4];

    file_target[0] = 0;

    parse_path(path, dir_target, file_target, ext_target);

    if (strlen(dir_target) > 9) return -ENAMETOOLONG;
    if (strlen(file_target) > 0) return -EPERM;
    
    touch(".dir"); //just in case it wasn't precreated

    FILE* g = fopen(".dir", "r");
    struct mkfs_directory_entry cur_dir;
    
    fread(&cur_dir, sizeof(cur_dir), 1, g);
    while (strcmp(cur_dir.dname, path + 1) != 0 && fread(&cur_dir, sizeof(cur_dir), 1, g) > 0);
    fclose(g);

    if (strcmp(cur_dir.dname, path + 1) == 0) return -EEXIST;
    
    FILE* f = fopen(".dir", "a");
    struct mkfs_directory_entry next_dir;
    strcpy(next_dir.dname, path + 1);
    next_dir.nFiles = 0;

    fwrite(&next_dir, sizeof(next_dir),  1, f);
    fclose(f);
    return 0;
}

static int _rmdir(const char *path) {
    printf("--------------------------------------------------------------------->RMDIR: %s\n", path);

    char dir_target[9];
    char file_target[9];
    char ext_target[4];

    file_target[0] = 0;

    parse_path(path, dir_target, file_target, ext_target);
    
    if (strlen(file_target) > 0) return -ENOTDIR;

    touch(".dir"); //just in case it wasn't precreated

    FILE* f = fopen(".dir", "r+b");

    fseek(f, -sizeof(mkfs_directory_entry), SEEK_END);
    int length = ftell(f);
    struct mkfs_directory_entry temp_entry;
    fread(&temp_entry, sizeof(temp_entry), 1, f);

    //find the one to delete
    fseek(f, 0, SEEK_SET); //go to begin
    struct mkfs_directory_entry cur_dir;
    fread(&cur_dir, sizeof(cur_dir), 1, f);
   
    //look for deletion target
    while (strcmp(cur_dir.dname, path + 1) != 0 && !feof(f)) {
        fread(&cur_dir, sizeof(cur_dir), 1, f);
    }

    //if we found deletion target, overwrite it with last element
    if (strcmp(cur_dir.dname, path + 1) == 0) {
        fseek(f, -sizeof(cur_dir), SEEK_CUR);
        fwrite(&temp_entry, sizeof(cur_dir), 1, f);
        fseek(f, 0, SEEK_SET);
    } else {
        fclose(f);
        return -ENOENT;
    }
    fclose(f);
    return 0;
}

static int _mknod(const char *path, mode_t mode, dev_t dev) {
    printf("--------------------------------------------------------------------->MKNOD: %s\n", path);

    (void) mode;
    (void) dev;

    char dir_targ[9];
    char file_targ[9];
    char ext_targ[4];

    ext_targ[0] = 0;
    file_targ[0] = 0;
    dir_targ[0] = 0;
    parse_path(path, dir_targ, file_targ, ext_targ);

    check_bitmap();

    if (strlen(file_targ) > 8 || strlen(ext_targ) > 3 ) return -ENAMETOOLONG;
    if (strlen(file_targ) == 0) return -EPERM; //if we are trying to create a file in root, parse path returns null for file and ext strings


    mkfs_directory_entry cur_dir;
    
    //find directory, make sure it exists
    int dir_idx = find_dir(&cur_dir, dir_targ);
    if (dir_idx == -1) return -ENOENT;
    
    //make sure file does not exist
    int file_idx = find_file(&cur_dir, file_targ, ext_targ);
    if (file_idx != -1) return -EEXIST;

    //make the file
    int new_file_idx = cur_dir.nFiles;
    if (new_file_idx > MAX_FILES_IN_DIR) return -EPERM; //if the directory is full return a permission error

    strcpy(cur_dir.files[new_file_idx].fname, file_targ);
    strcpy(cur_dir.files[new_file_idx].fext, ext_targ);
    cur_dir.files[new_file_idx].fsize = 0;

    int new_file_block = -1;
    cur_dir.files[new_file_idx].nStartBlock = new_file_block;

    cur_dir.nFiles++;

    FILE* f = fopen(".dir", "r+b");
    fseek(f, dir_idx, SEEK_SET);
    fwrite(&cur_dir, sizeof(cur_dir), 1, f);

    fclose(f);
    return 0;
}

static int _unlink(const char *path) {
    printf("--------------------------------------------------------------------->UNLINK: %s\n", path);

    char dir_target[9];
    char file_target[9];
    char ext_target[4];

    dir_target[0] = 0;
    file_target[0] = 0;
    ext_target[0] = 0;
    parse_path(path, dir_target, file_target, ext_target);

    if (!(strlen(file_target) > 0)) { //it is a directory
        return -EISDIR;
    }

    struct stat stbuf;
    int attr = _getattr(path, &stbuf);
    if (attr != 0) {
        return attr;
    }
    
    mkfs_directory_entry dir_struct;
    int dir_idx = find_dir(&dir_struct, dir_target);

    if (dir_idx == -1) {
        return -ENOENT;
    }
    
    int file_index = find_file(&dir_struct, file_target, ext_target);
    mkfs_file_directory the_file = dir_struct.files[file_index];
    
    if (the_file.fsize > 0) {
        printf("--------------------------------------------------------------------->UNLINK: Deleting a file (%s) of size %d\n", the_file.fname, the_file.fsize);
        int num_blocks = the_file.fsize / 512;
        if (the_file.fsize % 512 != 0) {
            num_blocks++;
        }
        unallocate(the_file.nStartBlock, num_blocks); //free the blocks it used
    }

    //removing all references to it in the .dir file
    dir_struct.files[file_index] = dir_struct.files[dir_struct.nFiles - 1];
    dir_struct.nFiles--;

    FILE* f = fopen(".dir", "r+b");
    fwrite(&dir_struct, sizeof(dir_struct), 1, f);
    fclose(f);
    return 0;
}

static int _read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info * fi) {
    printf("--------------------------------------------------------------------->READ: %s\n", path);
    
    (void) buf;
    (void) offset;
    (void) fi;
    (void) path;

    char dir_targ[9];
    char file_targ[9];
    char ext_targ[4];

    dir_targ[0] = 0;
    file_targ[0] = 0;
    ext_targ[0] = 0;
    parse_path(path, dir_targ, file_targ, ext_targ);

    touch(".disk");

    //check to make sure path exists
    mkfs_directory_entry cur_dir;
    int dir_exists = find_dir(&cur_dir, dir_targ);
    if (dir_exists == -1) return -ENOENT;

    int file_index = find_file(&cur_dir, file_targ, ext_targ);
    if (file_index == -1) return -ENOENT;

    mkfs_file_directory cur_file = cur_dir.files[file_index];
    
    if (size <= 0) return 0; //Why not?
    
    if (offset > cur_file.fsize) return 0; //nothing left
    
    //figure out where to read from
    int read_index = cur_file.nStartBlock * 512 + offset;
    int max_read = cur_file.fsize - offset;
    if (max_read < size) size = max_read;
    
    //read in data
    FILE* f = fopen(".disk", "rb");
    fseek(f, read_index, SEEK_SET);
    int bytes_read = fread(buf, 1, size, f);
    printf("--------------------------------------------------------------------->READ: DBUG Read %d bytes\n", bytes_read);
    fclose(f);
    return bytes_read;
}

static int _write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    printf("--------------------------------------------------------------------->WRITE: %s\n", path);
    
    char dir_targ[9];
    char file_targ[9];
    char ext_targ[4];

    dir_targ[0] = 0;
    file_targ[0] = 0;
    ext_targ[0] = 0;
    parse_path(path, dir_targ, file_targ, ext_targ);

    touch(".disk");

    //check to make sure path exists
    mkfs_directory_entry cur_dir;
    int dir_index = find_dir(&cur_dir, dir_targ);
    if (dir_index == -1) return -ENOENT;

    int file_index = find_file(&cur_dir, file_targ, ext_targ);
    if (file_index == -1) return -ENOENT;

    mkfs_file_directory cur_file = cur_dir.files[file_index];

    if (size <= 0) return 0; //Why not?

    if (offset > cur_file.fsize) return 0; //nothing left
    
    printf("--------------------------------------------------------------------->WRITE: Size of cur_file = %d\n", cur_file.fsize);
    //calculates number of bytes the file
    int new_bytes = (offset + size) - cur_file.fsize;
    int new_blocks = 0;
    
    printf("--------------------------------------------------------------------->WRITE: New bytes needed: %d\n", new_bytes);
    
    int blocks_used = 0;
    while (blocks_used * BLOCK_SIZE < cur_file.fsize) {
        blocks_used++;
    }

    int bytes_left = (BLOCK_SIZE * blocks_used) - cur_file.fsize;
    printf("--------------------------------------------------------------------->WRITE: Bytes availible in current block: %d\n", bytes_left);
    
    int i = new_bytes;
    while (i > bytes_left) {
        new_blocks++;
        i -= BLOCK_SIZE;
    }
    
    printf("--------------------------------------------------------------------->WRITE: New blocks needed: %d\n", new_blocks);
    if (new_blocks > 0) {
        int cur_blocks = cur_file.fsize / BLOCK_SIZE;
        if (cur_file.fsize % BLOCK_SIZE != 0) {
            cur_blocks++;
        }
        printf("--------------------------------------------------------------------->WRITE: Current blocks: %d\n", cur_blocks);
        
        //only unallocate space for files that have been allocated space!
        printf("--------------------------------------------------------------------->WRITE: Start Block of current file: %d\n", cur_file.nStartBlock);
        if (cur_file.nStartBlock != -1) {
            unallocate(cur_file.nStartBlock, cur_blocks);
        }

        int new_start_loc = find_free_space(new_blocks + cur_blocks);
        printf("--------------------------------------------------------------------->WRITE: Attempting to put file at %d\n", new_start_loc);
        
        if (new_start_loc == -1) { // then the space request is unsatisfiable via contiguous allocation
            allocate(cur_file.nStartBlock, cur_blocks);
            return -ENOSPC;
        }
        
        allocate(new_start_loc, new_blocks + cur_blocks);
        cur_dir.files[file_index].nStartBlock = new_start_loc;
    }


    //write the data
    int write_index = cur_dir.files[file_index].nStartBlock * BLOCK_SIZE + offset;
    printf("--------------------------------------------------------------------->WRITE: Bitmap ends at %d.  Writing at %d.\n", get_bitmap_size(), write_index);
    
    FILE* f = fopen(".disk", "r+b");
    fseek(f, write_index, SEEK_SET);
    fwrite(buf, size, 1, f);
    fclose(f);

    if (new_bytes > 0) {
        cur_dir.files[file_index].fsize += new_bytes;
    }

    FILE* g = fopen(".dir", "r+b");
    fseek(g, dir_index, SEEK_SET);
    fwrite(&cur_dir, sizeof(cur_dir), 1, g);
    fclose(g);

    // print_bitmap();
    return size;
}

static int _open(const char *path, struct fuse_file_info * fi) {
    printf("--------------------------------------------------------------------->OPEN: %s\n", path);

    (void) path;
    (void) fi;
 
    return 0;
}