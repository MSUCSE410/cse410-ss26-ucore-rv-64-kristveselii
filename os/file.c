#include "file.h"
#include "defs.h"
#include "fcntl.h"
#include "fs.h"
#include "proc.h"

#define STAT_MODE_DIR  0x00400000
#define STAT_MODE_FILE 0x00100000

//This is a system-level open file table that holds open files of all process.
struct file filepool[FILEPOOLSIZE];

//Abstract the stdio into a file.
struct file *stdio_init(int fd)
{
	struct file *f = filealloc();
	f->type = FD_STDIO;
	f->ref = 1;
	f->readable = (fd == STDIN || fd == STDERR);
	f->writable = (fd == STDOUT || fd == STDERR);
	return f;
}

//The operation performed on the system-level open file table entry after some process closes a file.
void fileclose(struct file *f)
{
	if (f->ref < 1)
		panic("fileclose");
	if (--f->ref > 0) {
		return;
	}
	switch (f->type) {
	case FD_STDIO:
		// Do nothing
		break;
	case FD_INODE:
		iput(f->ip);
		break;
	default:
		panic("unknown file type %d\n", f->type);
	}

	f->off = 0;
	f->readable = 0;
	f->writable = 0;
	f->ref = 0;
	f->type = FD_NONE;
}

//Add a new system-level table entry for the open file table
struct file *filealloc()
{
	for (int i = 0; i < FILEPOOLSIZE; ++i) {
		if (filepool[i].ref == 0) {
			filepool[i].ref = 1;
			return &filepool[i];
		}
	}
	return 0;
}

//Show names of all files in the root_dir.
int show_all_files()
{
	return dirls(root_dir());
}

//Create a new empty file based on path and type and return its inode;
//if the file under the path exists, return its inode;
//returns 0 if the type of file to be created is not T_file
static struct inode *create(char *path, short type)
{
	struct inode *ip, *dp;
	dp = root_dir(); //Remember that the root_inode is open in this step,so it needs closing then.
	ivalid(dp);
	if ((ip = dirlookup(dp, path, 0)) != 0) {
		warnf("create a exist file\n");
		iput(dp); //Close the root_inode
		ivalid(ip);
		if (type == T_FILE && ip->type == T_FILE)
			return ip;
		iput(ip);
		return 0;
	}
	if ((ip = ialloc(dp->dev, type)) == 0)
		panic("create: ialloc");

	tracef("create dinode and inode type = %d\n", type);

	ivalid(ip);
	iupdate(ip);
	if (dirlink(dp, path, ip->inum) < 0)
		panic("create: dirlink");

	iput(dp);
	return ip;
}

//A process creates or opens a file according to its path, returning the file descriptor of the created or opened file.
//If omode is O_CREATE, create a new file
//if omode if the others,open a created file.
int fileopen(char *path, uint64 omode)
{
	int fd;
	struct file *f;
	struct inode *ip;
	if (omode & O_CREATE) {
		ip = create(path, T_FILE);
		if (ip == 0) {
			return -1;
		}
	} else {
		if ((ip = namei(path)) == 0) {
			return -1;
		}
		ivalid(ip);
	}
	if (ip->type != T_FILE)
		panic("unsupported file inode type\n");
	if ((f = filealloc()) == 0 ||
	    (fd = fdalloc(f)) <
		    0) { //Assign a system-level table entry to a newly created or opened file
		//and then create a file descriptor that points to it
		if (f)
			fileclose(f);
		iput(ip);
		return -1;
	}
	// only support FD_INODE
	f->type = FD_INODE;
	f->off = 0;
	f->ip = ip;
	f->readable = !(omode & O_WRONLY);
	f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
	if ((omode & O_TRUNC) && ip->type == T_FILE) {
		itrunc(ip);
	}
	return fd;
}

// Write data to inode.
uint64 inodewrite(struct file *f, uint64 va, uint64 len)
{
	int r;
	ivalid(f->ip);
	if ((r = writei(f->ip, 1, va, f->off, len)) > 0)
		f->off += r;
	return r;
}

//Read data from inode.
uint64 inoderead(struct file *f, uint64 va, uint64 len)
{
	int r;
	ivalid(f->ip);
	if ((r = readi(f->ip, 1, va, f->off, len)) > 0)
		f->off += r;
	return r;
}

// PROJECT 4: filestat — fill the user-space Stat structure for sys_fstat.
// The Stat layout is defined by the user-side header (user/lib/stat.h):
//   dev (u64), ino (u64), mode (u32), nlink (u32), pad[7] (u64)
// We build it in kernel space then copyout to the user virtual address.
int filestat(struct file *f, uint64 addr)
{
    if (f == 0 || f->type != FD_INODE || f->ip == 0) { // Validate the file pointer and ensure it's an inode file
        return -1;
    }

    struct proc *p = curr_proc(); // Get the current process to access its page table for copying data back to user space

	// PROJECT 4: anonymous struct matching the user-side Stat layout exactly.
	// Field sizes and ordering must match or the user program reads garbage.
    struct {
        uint64 dev;
        uint64 ino;
        uint32 mode;
        uint32 nlink;
        uint64 pad[7];
    } st;

	// PROJECT 4: fill each Stat field from the inode.
    st.dev = 0; // Single-device kernel — device number is always 0.
    st.ino = f->ip->inum; // Inode number uniquely identifies the file on disk.
	// PROJECT 4: encode file type in the mode field using the constants
	// the user program expects: 0x040000 = directory, 0x100000 = regular file.
    st.mode = (f->ip->type == T_DIR) ? 0x00400000 : 0x00100000;
	// PROJECT 4: hard link count — tells the user how many names point to this inode.
    st.nlink = f->ip->nlink;

	// Pad the remaining fields with zeros
    for (int i = 0; i < 7; i++) {
        st.pad[i] = 0;
    }

	// PROJECT 4: copy the completed Stat struct from kernel space into the
	// user virtual address provided by the syscall argument.
    if (copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0) {
        return -1;
    }

    return 0;
}

// PROJECT 4: filelink — create a hard link from 'old' to 'new'.
// A hard link is simply a second directory entry that maps a new name to the
// same inode number. Both names refer to identical file data; neither is the
// "original". The inode's nlink field tracks how many such entries exist.
int filelink(char *old, char *new)
{
    struct inode *dp = root_dir(); // Get the root directory inode to perform lookups and modifications
    struct inode *ip = dirlookup(dp, old, 0); // Look up the inode for the existing file specified by 'old' path
    if (ip == 0) { // If the existing file does not exist, return an error
        iput(dp); // Release the reference to the root directory inode before returning
        return -1; // Return -1 to indicate failure in creating the link
    }

    ivalid(ip); // Ensure the inode for the existing file is valid and its data is loaded into memory

	// PROJECT 4: write a new dirent {new, ip->inum} into the root directory.
	// dirlink checks that 'new' doesn't already exist (returns -1 if it does),
	// then finds a free dirent slot (inum==0) and writes the new entry there.
	// The inode number is shared with 'old' — that's what makes it a hard link.
    if (dirlink(dp, new, ip->inum) < 0) {
        iput(ip);
        iput(dp);
        return -1;
    }

    ip->nlink++; // PROJECT 4: one more directory entry now points to this inode.
    iupdate(ip); // PROJECT 4: flush the new nlink value to disk so it survives reboot.

    iput(ip); // Release the reference to the existing file's inode
    iput(dp); // Release the reference to the root directory inode
    return 0;
}

// PROJECT 4: fileunlink — remove one hard link (directory entry) for 'path'.
// If this was the last link (nlink reaches 0), the final iput() call will
// free the inode and all data blocks via itrunc(). If other links remain,
// the file data is preserved and accessible through those other names.
int fileunlink(char *path)
{
    struct inode *dp = root_dir(); // Get the root directory inode to perform lookups and modifications
    struct inode *ip = dirlookup(dp, path, 0); // Look up the inode for the file specified by 'path' to be unlinked
    if (ip == 0) {
        iput(dp); // Release the reference to the root directory inode before returning if the file does not exist
        return -1; // Return -1 to indicate failure in unlinking the file since it does not exist
    }

    ivalid(ip); // Ensure the inode for the file to be unlinked is valid and its data is loaded into memory

	// PROJECT 4: remove the directory entry — zeroes the dirent slot in the
	// root directory so the name no longer maps to any inode. This does NOT
	// touch the inode itself; the inode still exists until nlink hits 0.
    if (dirunlink(dp, path) < 0) {
        iput(ip);
        iput(dp);
        return -1;
    }

    ip->nlink--; // PROJECT 4: one fewer directory entry points to this inode.
    iupdate(ip); // PROJECT 4: flush the decremented nlink to disk immediately.

	// PROJECT 4: iput drops our lookup reference. If nlink==0 AND ref drops
	// to 0 inside iput, it calls itrunc() to free data blocks and zeroes the
	// dinode on disk — the file is completely deleted at that point.
    iput(ip);
    iput(dp);

    return 0;
}