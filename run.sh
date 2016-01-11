rm ./mkfs
mkdir ./mkfs_root
gcc mkfs.c -o mkfs -lfuse
./mkfs -d mkfs_root/