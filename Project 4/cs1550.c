/*
	FUSE: Filesystem in Userspace
	Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

	This program can be distributed under the terms of the GNU GPL.
	See the file COPYING.
*/

#define	FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>

//size of a disk block
#define	BLOCK_SIZE 512

//we'll use 8.3 filenames
#define	MAX_FILENAME 8
#define	MAX_EXTENSION 3

//How many files can there be in one directory?
#define MAX_FILES_IN_DIR (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))

#define DISK_SIZE  5242880 // (5 * (2^20)) bytes
#define MAX_BLOCK  10239 // DISK_SIZE / BLOCK_SIZE - 1 to account for index starting at 0
#define NUM_BLOCKS 10240 // DISK_SIZE / BLOCK_SIZE /// = number of bits needed to track allocated blocks 
#define BITMAP_BLOCK 10237 // bitmap will need 3 blocks of memory /// NUM_BLOCKS / (sizeof(byte)*BLOCK_SIZE) = 2.5 blocks needed for bitmap, aka will need blocks 10237, 10238, 10239 

#define BLOCK_COUNT (DISK_SIZE / BLOCK_SIZE - ((DISK_SIZE - 1) / (8 * BLOCK_SIZE * BLOCK_SIZE) + 1)) //+

//The attribute packed means to not align these things
struct cs1550_directory_entry
{
	int nFiles;	//How many files are in this directory.
				//Needs to be less than MAX_FILES_IN_DIR

	struct cs1550_file_directory
	{
		char fname[MAX_FILENAME + 1];	//filename (plus space for nul)
		char fext[MAX_EXTENSION + 1];	//extension (plus space for nul)
		size_t fsize;					//file size
		long nIndexBlock;				//where the index block is on disk
	} __attribute__((packed)) files[MAX_FILES_IN_DIR];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.
	char padding[BLOCK_SIZE - MAX_FILES_IN_DIR * sizeof(struct cs1550_file_directory) - sizeof(int)];
} ;

typedef struct cs1550_root_directory cs1550_root_directory;

#define MAX_DIRS_IN_ROOT (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + sizeof(long))

struct cs1550_root_directory
{
	int nDirectories;	//How many subdirectories are in the root
						//Needs to be less than MAX_DIRS_IN_ROOT
	struct cs1550_directory
	{
		char dname[MAX_FILENAME + 1];	//directory name (plus space for nul)
		long nStartBlock;				//where the directory block is on disk
	} __attribute__((packed)) directories[MAX_DIRS_IN_ROOT];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.
	char padding[BLOCK_SIZE - MAX_DIRS_IN_ROOT * sizeof(struct cs1550_directory) - sizeof(int)];
};


typedef struct cs1550_directory_entry cs1550_directory_entry;

//How many entries can one index block hold?
#define	MAX_ENTRIES_IN_INDEX_BLOCK (BLOCK_SIZE/sizeof(long))

struct cs1550_index_block
{
    //All the space in the index block can be used for index entries.
	// Each index entry is a data block number.
	long entries[MAX_ENTRIES_IN_INDEX_BLOCK];
};

typedef struct cs1550_index_block cs1550_index_block;

//How much data can one block hold?
#define	MAX_DATA_IN_BLOCK (BLOCK_SIZE)

struct cs1550_disk_block
{
	//All of the space in the block can be used for actual data
	//storage.
	char data[MAX_DATA_IN_BLOCK];
};

typedef struct cs1550_disk_block cs1550_disk_block;

void * cs1550_init(struct fuse_conn_info* fi);
static FILE * get_disk(void);
static int set_bitmap(FILE * f, long block_idx, char val);
static long next_free_block(FILE * f);
static int save_block(FILE * f, long block_idx, void *block);
static cs1550_root_directory * load_root_directory(FILE * f);
static void * load_block(FILE * f, long block_idx);
static cs1550_directory_entry* load_subdirectory(FILE *f, long block_idx);
static cs1550_disk_block* load_file_block(FILE *f, long block_idx);
static int throw_error(int e, int l);
static int subdirectory_exists(cs1550_root_directory * root, char * directory);
static int file_exists(cs1550_directory_entry * dir, char * file, char * ext);

/*
 * Called whenever the system wants to know the file attributes, including
 * simply whether the file exists or not.
 *
 * man -s 2 stat will show the fields of a stat structure
 */
static int cs1550_getattr(const char *path, struct stat *stbuf) {

	memset(stbuf, 0, sizeof(struct stat));

	if (strcmp(path, "/") == 0) { // is path the root dir?

		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;

	} else {

		char directory[MAX_FILENAME + 1], filename[MAX_FILENAME + 1], extension[MAX_EXTENSION + 1];
		int count = sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);
		if (count < 1) { return throw_error(-ENOENT, __LINE__); }

		FILE *disk = get_disk(); // Open disk file
		if (!disk) { return -ENXIO; }

		cs1550_root_directory* root = load_root_directory(disk); // Load root directory block
		if (!root) { return -EIO; }
		
		size_t dir_idx = subdirectory_exists(root, directory); // Search for subdirectory
		
		if (dir_idx == -1) { return throw_error(-ENOENT, __LINE__); }

		if (count == 1) { // path was to directory

			stbuf->st_mode = S_IFDIR | 0755;
			stbuf->st_nlink = 2;

 		} else { // path was to file

 			cs1550_directory_entry * dir = load_subdirectory(disk, root->directories[dir_idx].nStartBlock); // Load subdirectory
 			if (!dir) { return -EIO; } 

			size_t file_idx = file_exists(dir, filename, extension);  // Search for file

			stbuf->st_mode  = S_IFREG | 0666;
            stbuf->st_nlink = 1; // file links
            stbuf->st_size  = dir->files[file_idx].fsize; // file size

            free(dir);

	    }

		free(root);
		fclose(disk);
	}

	return 0;
}

/*
 * Called whenever the contents of a directory are desired. Could be from an 'ls'
 * or could even be when a user hits TAB to do autocompletion
 */
static int cs1550_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {

	(void) offset;
	(void) fi;

	char directory[MAX_FILENAME + 1], filename[MAX_FILENAME + 1], extension[MAX_EXTENSION + 1];
	int count = sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);
	if (count > 1) { return throw_error(-ENOENT, __LINE__); }
	// the filler function allows us to add entries to the listing
	// read the fuse.h file for a description (in the ../include dir)
	filler(buf, ".",  NULL, 0);
	filler(buf, "..", NULL, 0);

	FILE *disk = get_disk(); // Open disk file
	if (!disk) { return -ENXIO; }
	cs1550_root_directory* root = load_root_directory(disk); // Load root directory block
	if (!root) { return -EIO; } 
	
	size_t dir_idx;
	if (!strcmp(path, "/")) { // Add subdirectories
		for (dir_idx = 0; dir_idx < root->nDirectories; ++dir_idx) { filler(buf, root->directories[dir_idx].dname, NULL, 0); }
	} else if (count == 1) {

		size_t dir_idx = subdirectory_exists(root, directory); // Search for subdirectory

		if (dir_idx != -1) { // Load subdirectory

			cs1550_directory_entry* dir = load_subdirectory(disk, root->directories[dir_idx].nStartBlock);
			if (!dir) { return -EIO; }

			size_t file_idx; // Add files
			for (file_idx = 0; file_idx < dir->nFiles; file_idx++) { // Get full file name (fname.ext)
				char filename[MAX_FILENAME + MAX_EXTENSION + 1];
				strcpy(filename, dir->files[file_idx].fname);
				strcat(filename, ".");
				strcat(filename, dir->files[file_idx].fext);
				filler(buf, filename, NULL, 0);
			}
			free(dir);
		}
	}
	free(root);
	
	fclose(disk);
	return 0;

}

static int clean_and_return(FILE * f, cs1550_root_directory * r, int e) {
	fclose(f);
	free(r);
	return e;
}

/*
 * Creates a directory. We can ignore mode since we're not dealing with
 * permissions, as long as getattr returns appropriate ones for us.
 */
static int cs1550_mkdir(const char *path, mode_t mode) {

	(void) path;
	(void) mode;

	char directory[MAX_FILENAME + 1], filename[MAX_FILENAME + 1], extension[MAX_EXTENSION + 1];
	
	if (sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension) > 1) { return -EPERM; } // check if in the form of a filename
	if (strlen(directory) > MAX_FILENAME) { return -ENAMETOOLONG; } // Check if the directory name is overlength

	FILE * disk = get_disk(); // Open disk file and load root directory block
	cs1550_root_directory * root = load_root_directory(disk);

	if (!root || !disk)                         { return clean_and_return(disk, root, -ENXIO); }
	if (subdirectory_exists(root, directory) != -1)   { return clean_and_return(disk, root, -EEXIST); } // Search for subdirectory
	if (root->nDirectories >= MAX_DIRS_IN_ROOT) { return clean_and_return(disk, root, -ENOSPC); } // When the directory is full

	long new_block_idx = next_free_block(disk); // Subdirectory does not exist, create new
	if (new_block_idx > 0) {

		fprintf(stderr, "Made new directory at block #%ld\n", new_block_idx);
		strcpy(root->directories[root->nDirectories].dname, directory);
		root->directories[root->nDirectories].nStartBlock = new_block_idx;
		root->nDirectories++;

		if (save_block(disk, 0, root) || set_bitmap(disk, new_block_idx, 1)) { return clean_and_return(disk, root, -EIO); }
		else { return clean_and_return(disk, root, 0); }

	} 
	else if (new_block_idx == -1) { return clean_and_return(disk, root, -ENOSPC); } 
	else 						  { return clean_and_return(disk, root, -EIO);    }
}

/*
 * Removes a directory.
 */
static int cs1550_rmdir(const char *path)
{
	(void) path;
	return 0;
}

/*
 * Does the actual creation of a file. Mode and dev can be ignored.
 *
 */
static int cs1550_mknod(const char * path, mode_t mode, dev_t dev) {

	(void) mode;
	(void) dev;
	(void) path;

	char directory[MAX_FILENAME + 1], filename[MAX_FILENAME + 1], extension[MAX_EXTENSION + 1];
	if (sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension) < 3)  { return -EPERM; }
	if (strlen(directory) > MAX_FILENAME) { return -ENAMETOOLONG; } // Check if the directory name is overlength
	if (strlen(filename) > MAX_FILENAME || strlen(extension) > MAX_EXTENSION) { return -ENAMETOOLONG; } // Check if the file name or directory name is overlength

	FILE * disk = get_disk(); // Open disk file
	if (!disk) { return -ENXIO; }
	cs1550_root_directory * root = load_root_directory(disk); // Load root directory block
	if (!root) { return -EIO; } 

	size_t root_dirs_i = subdirectory_exists(root, directory);  // Search for subdirectory
	if (root_dirs_i == -1) { return throw_error(-ENOENT, __LINE__); } 
	long dir_block = root->directories[root_dirs_i].nStartBlock;
	cs1550_directory_entry * dir = load_subdirectory(disk, dir_block); // Load subdirectory
	
	if (!dir) { return -EIO; } 
	fprintf(stderr, "Max files possible: %u \nCurrently contains before adding one: %d \n", MAX_FILES_IN_DIR, dir->nFiles);
	if (dir->nFiles >= MAX_FILES_IN_DIR) { return -ENOSPC; } // When the directory is full

	size_t file_idx = file_exists(dir, filename, extension); // Search for file
	if (file_idx != -1) { return -EEXIST; } // if file found, return "file already exists" error

	// Create new file //
	long new_i = next_free_block(disk); // find place for new index block
	set_bitmap(disk, new_i, 1); 		// set new index block to allocated
	long new_d = next_free_block(disk); // find place for new data block
	set_bitmap(disk, new_d, 1); 		// set new data block as allocated

	strcpy(dir->files[dir->nFiles].fname, filename); // update directory/file info
	strcpy(dir->files[dir->nFiles].fext, extension);
	dir->files[dir->nFiles].nIndexBlock = new_i;
	dir->nFiles = dir->nFiles + 1; // this doesnt work??? mknod only gets a 17/18 on autograder because i cant successfully update this field after the first time

	cs1550_index_block * i_block = (cs1550_index_block*) load_block(disk, new_i); // laod index block
	i_block->entries[0] = new_d; // set first entry to equal data block location

	if (save_block(disk, dir_block, dir)) { return -EIO; } // save updated directory block back to disk
	if (save_block(disk, new_i, i_block)) { return -EIO; } // save index block to disk, dont need to save data bc its all zeroes
	/////////////////////

	free(i_block);
	free(dir);
	free(root);
	fclose(disk);
	return 0;

}

/*
 * Deletes a file
 */
static int cs1550_unlink(const char *path)
{
	(void) path;
	return 0;
}

/*
 * Read size bytes from file into buf starting from offset
 *
 */
static int cs1550_read(const char * path, char * buf, size_t size, off_t offset, struct fuse_file_info * fi) {

	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;

	if (!(size > 0)) { return -EPERM; } // Check that size is > 0
	char directory[MAX_FILENAME + 1], filename[MAX_FILENAME + 1], extension[MAX_EXTENSION + 1];
	int count = sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);
	if (count == EOF) { return -EISDIR; }
	if (strlen(directory) > MAX_FILENAME || strlen(filename) > MAX_FILENAME || strlen(extension) > MAX_EXTENSION) { return -ENAMETOOLONG; } // Check if the directory name, file name, or extension name is overlength

	FILE * disk = get_disk(); // Open disk file
	if (!disk) { return -ENXIO; }

	cs1550_root_directory * root = load_root_directory(disk); // Load root directory block
	if (!root) { return -EIO; }
	
	size_t dir_idx = subdirectory_exists(root, directory); // Search for subdirectory
	if (dir_idx == -1)   { return(throw_error(-ENOENT, __LINE__)); } // Subdirectory exists

	if      (count == 1) { return -EISDIR; } // Attempting to read existing directory
	else if (count == 2) { return throw_error(-ENOENT, __LINE__); } // No extension name provided

	long dir_block_idx = root->directories[dir_idx].nStartBlock;
	cs1550_directory_entry* dir = load_subdirectory(disk, dir_block_idx); // Load subdirectory

	if (!dir) { return -EIO; }
	
	size_t file_idx = file_exists(dir, filename, extension); // Search for file
	if (file_idx == -1) { return(throw_error(-ENOENT, __LINE__)); }
		
	if (offset > dir->files[file_idx].fsize) { return -EFBIG; } 
	if (offset + size > dir->files[file_idx].fsize) { size = dir->files[file_idx].fsize - offset; } // Set read size bound

	long block_idx = dir->files[file_idx].nIndexBlock;
	cs1550_index_block * i_block = (cs1550_index_block *) (block_idx * BLOCK_SIZE);
	cs1550_disk_block  * d_block = NULL;

	int i = 0;
	size_t remaining = size;
	while (i_block->entries[i] != 0 && i < MAX_ENTRIES_IN_INDEX_BLOCK && remaining > 0) {

		if (offset > MAX_DATA_IN_BLOCK) { // traverse offset bytes/blocks first

			offset -= MAX_DATA_IN_BLOCK;

		} else { // then start reading after offset

			d_block = (cs1550_disk_block*) load_block(disk, i_block->entries[i]); // get data block 
			
			int read_size = (remaining < (MAX_DATA_IN_BLOCK - offset) ? remaining : MAX_DATA_IN_BLOCK - offset); // read after offset
			memcpy(buf, d_block->data + offset, read_size); // copy data into buffer

			remaining -= read_size; // update how many bytes we have left to read
			buf += read_size; // update buffer start point

			offset = 0; // only use offset the first block we read

		}

		i++; // increment entry index
		
	}
	
	free(d_block);
	free(i_block);	
	free(dir);
	free(root);
	fclose(disk);
	return size;
}

/*
 * Write size bytes from buf into file starting from offset
 *
 */
static int cs1550_write(const char * path, const char * buf, size_t size, off_t offset, struct fuse_file_info * fi) {
	
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;

	if (!(size > 0)) { return -EPERM; } // Check that size is > 0
	size_t total_size = size + offset;
	char directory[MAX_FILENAME + 1], filename[MAX_FILENAME + 1], extension[MAX_EXTENSION + 1];
	int count = sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);
	if (count < 3) { return throw_error(-ENOENT, __LINE__); }
	if (strlen(directory) > MAX_FILENAME || strlen(filename) > MAX_FILENAME || strlen(extension) > MAX_EXTENSION) { return throw_error(-ENOENT, __LINE__); } // Check if the directory name, file name, or extension name is overlength
	FILE *disk = get_disk(); // Open disk file
	if (!disk) { return -ENXIO; }
	cs1550_root_directory* root = load_root_directory(disk); // Load root directory block

	if (!root) { return -EIO; }
	
	size_t dir_idx = subdirectory_exists(root, directory); // Search for subdirectory
	if (dir_idx == -1) { return -ENOENT; } // Subdirectory exists

	long dir_block_idx = root->directories[dir_idx].nStartBlock;
	cs1550_directory_entry * dir = load_subdirectory(disk, dir_block_idx); // Load subdirectory

	if (!dir) { return -EIO; }

	size_t file_idx = file_exists(dir, filename, extension);
	if (file_idx == -1) { return -ENOENT; }
	if (offset > dir->files[file_idx].fsize) { return -EFBIG; }
	
	long block_idx = dir->files[file_idx].nIndexBlock;
	fprintf(stderr, "Loading index block #%ld\n", block_idx);
	cs1550_index_block * i_block = (cs1550_index_block*) load_block(disk, block_idx);

	int entry_jump = offset / BLOCK_SIZE;
	if (entry_jump > (sizeof(i_block->entries) / sizeof(long))) { return -EIO; } // if offset goes past filesize, throw error
	block_idx = i_block->entries[entry_jump];
	fprintf(stderr, "Loading data block #%ld\n", block_idx);
	cs1550_disk_block * d_block = load_file_block(disk, block_idx); // jump to block where offset gets us

	offset = offset % BLOCK_SIZE; // we've jumped across n blocks, just need to jump across part of block now
	size_t remaining = size;
	size_t write_size = 0;
	int i = 0;
	while (i_block->entries[i] != 0 && i < MAX_ENTRIES_IN_INDEX_BLOCK && remaining > 0) {

		if (offset > MAX_DATA_IN_BLOCK) { // traverse offset bytes/blocks first

			offset -= MAX_DATA_IN_BLOCK;

		} else { // then start reading after offset

			if (i_block->entries[i] == 0) { i_block->entries[i] = next_free_block(disk); } // if need new block, find free one
			d_block = (cs1550_disk_block*) load_block(disk, i_block->entries[i]); // get data block 
			set_bitmap(disk, i_block->entries[i], 1); // update bitmap
			
			int read_size = (remaining < (MAX_DATA_IN_BLOCK - offset) ? remaining : MAX_DATA_IN_BLOCK - offset); // read after offset
			memcpy(d_block->data + offset, buf, write_size); // Copy data to current block

			remaining -= read_size; // update how many bytes we have left to read
			buf += read_size; // update buffer start point

			offset = 0; // only use offset the first block we read

		}

		i++; // increment entry index
		
	}

	dir->files[file_idx].fsize = total_size;
	free(i_block);
	free(d_block);	
	free(dir);
	free(root);
	fclose(disk);
	return size;
}

/*
 * truncate is called when a new file is created (with a 0 size) or when an
 * existing file is made shorter. We're not handling deleting files or
 * truncating existing ones, so all we need to do here is to initialize
 * the appropriate directory entry.
 *
 */
static int cs1550_truncate(const char *path, off_t size)
{
	(void) path;
	(void) size;

	return 0;
}

/*
 * Called when we open a file
 *
 */
static int cs1550_open(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;
    /*
        //if we can't find the desired file, return an error
        return throw_error(-ENOENT, __LINE__);
    */

    //It's not really necessary for this project to anything in open

    /* We're not going to worry about permissions for this project, but
	   if we were and we don't have them to the file we should return an error

        return -EACCES;
    */

    return 0; //success!
}

/*
 * Called when close is called on a file descriptor, but because it might
 * have been dup'ed, this isn't a guarantee we won't ever need the file
 * again. For us, return success simply to avoid the unimplemented error
 * in the debug log.
 */
static int cs1550_flush (const char *path , struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;

	return 0; //success!
}

/* Thanks to Mohammad Hasanzadeh Mofrad (@moh18) for these
   two functions */
void * cs1550_init(struct fuse_conn_info* fi)
{
	(void) fi;
	printf("We're all gonna live from here ....\n");
	return NULL;
}

static void cs1550_destroy(void* args)
{
	(void) args;
	printf("... and die like a boss here\n");
}

//register our new functions as the implementations of the syscalls
static struct fuse_operations hello_oper = {
	.getattr  = cs1550_getattr,
	.readdir  = cs1550_readdir,
	.mkdir	  = cs1550_mkdir,
	.rmdir    = cs1550_rmdir,
	.read	  = cs1550_read,
	.write	  = cs1550_write,
	.mknod	  = cs1550_mknod,
	.unlink   = cs1550_unlink,
	.truncate = cs1550_truncate,
	.flush    = cs1550_flush,
	.open	  = cs1550_open,
	.init     = cs1550_init,
	.destroy  = cs1550_destroy,
};

////////////////////////////////////////

static FILE * get_disk(void) {

	FILE * f = fopen(".disk", "r+b");

	if (!f) {
		fprintf(stderr, "disk not found\n");
		return NULL;
	}

	// Size of disk should be 5 MB (5*2^20 bytes)
	if (fseek(f, 0, SEEK_END) || ftell(f) != 5242880) {
		fclose(f);
		fprintf(stderr, "disk file size not valid\n");
		return NULL;
	}

	return f;
}

static int set_bitmap(FILE * f, long block_idx, char val) { // returns 0 if successful

	if (block_idx >= BITMAP_BLOCK) { fprintf(stderr, "block %ld does not exist\n", block_idx); return -1; }

	if (fseek(f, BITMAP_BLOCK * BLOCK_SIZE ,SEEK_SET)) { return 1; } // seek to bitmap
	long bytes_away = (block_idx / 8);
	if (fseek(f, bytes_away, SEEK_CUR)) { return 1; }

	char byte;
	if (!fread(&byte, 1, 1, f))  { return 1; }

	int bits_away = block_idx % 8;
	char mask = 1 << bits_away;

	if (val) { byte |=   mask; } 
	else     { byte &= (~mask); }

	if (fseek(f, -1, SEEK_CUR))  { return 1; }
	if (fwrite(&byte, 1, 1, f) != 1) { return 1; }
	
	return 0;
}

static int save_block(FILE *f, long block_idx, void * block) {

	if (block_idx >= BLOCK_COUNT) {
		fprintf(stderr, "requested block %ld to save does not exist\n", block_idx);
		return -1;
	}

	if (fseek(f, block_idx * BLOCK_SIZE, SEEK_SET)) {
		fprintf(stderr, "failed to seek to block %ld\n", block_idx);
		return -1;
	}

	if (fwrite(block, BLOCK_SIZE, 1, f) != 1) {
		fprintf(stderr, "failed to write to block %ld\n", block_idx);
		return -1;
	}

	return 0;
}

static cs1550_root_directory* load_root_directory(FILE * f) { 
	return (cs1550_root_directory*) load_block(f, 0);
}

static cs1550_directory_entry* load_subdirectory(FILE * f, long block_idx) {
	return (cs1550_directory_entry*) load_block(f, block_idx);
}

static cs1550_disk_block* load_file_block(FILE * f, long block_idx) {
	return (cs1550_disk_block*) load_block(f, block_idx);
}

static int subdirectory_exists(cs1550_root_directory * root, char * directory) {
	int i;
	for (i = 0; i < root->nDirectories; ++i) {
		fprintf(stderr, "%s\n", root->directories[i].dname);
		if (strcmp(root->directories[i].dname, directory) == 0) { return i; }
	} return -1;
}

static int file_exists(cs1550_directory_entry * dir, char * file, char * ext) {
	int i;
	for (i = 0; i < dir->nFiles; i++) {
		fprintf(stderr, "%s.%s\n", dir->files[i].fname, dir->files[i].fext);
		if (strcmp(dir->files[i].fname, file) == 0 && strcmp(dir->files[i].fext, ext) == 0) { return i; }
	} return -1; 
}

static long next_free_block(FILE * f) {

	fseek(f, BITMAP_BLOCK * BLOCK_SIZE, SEEK_SET); // seek to bitmap

	int B = 0, b = 0;

	while (((B*8) + b) < BITMAP_BLOCK) {

		char byte;
		fread(&byte, 1, 1, f);

		for (b = 0; b < 8; b++) {

			if (B == 0 && b == 0) { continue; } // skip root
			if (((B*8) + b) >= BITMAP_BLOCK) { break; } // hit bitmap blocks

			char mask = 1 << (8 - b - 1);
			if (!(byte & mask)) { return ((B*8) + b); }

		}

		B++;
	}

	return -1;
}

static void * load_block(FILE *f, long block_idx) {

	if (block_idx >= BLOCK_COUNT) {
		fprintf(stderr, "requested block %ld to load does not exist\n", block_idx);
		return NULL;
	}

	if (fseek(f, block_idx * BLOCK_SIZE, SEEK_SET)) {
		fprintf(stderr, "failed to seek to block %ld\n", block_idx);
		return NULL;
	}

	void * block = malloc(BLOCK_SIZE);
	if (fread(block, BLOCK_SIZE, 1, f) != 1) {
		fprintf(stderr, "failed to load block %ld\n", block_idx);
		free(block);
		return NULL;
	}
	return block;
}

static int throw_error(int e, int l) {
	fprintf(stderr, "Error %d thrown at line %d\n", e, l);
	return e;
}

//Don't change this.
int main(int argc, char *argv[])
{
	return fuse_main(argc, argv, &hello_oper, NULL);
}
