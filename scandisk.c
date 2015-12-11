#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"

//Divides, rounds up, used in sz_chck
int div_round_up(int dividend, int divisor){
	return (dividend + (divisor - 1)) / divisor;
}

//compares size in metadata to size in clusters
void sz_check(int meta_size, int clust_count, struct bpb33* bpb){
	int clust_size = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
	int max_size = clust_count * clust_size;  //largest possible size according to cluster chain
	int min_size = max_size - clust_size;   //smallest size according to cluster chain
	if (meta_size > max_size){
		printf("Big YIKES! -> Metadata is bigger than allocated clusters, has size %d, according to clusters size should be between %d and %d\n",
				meta_size, min_size, max_size);
    }
  	if (meta_size < min_size){
		  int no_bad_clust = div_round_up(min_size - meta_size, clust_size);
			printf("Small YIKES! -> Metadata is smaller than allocated clusters, need to free %d cluster(s).\n", no_bad_clust);
	}
}

int checker(uint8_t *image_buf, struct bpb33 *bpb, int* clust_ref, uint16_t cluster)
{
	int clust_count = 0;
  while (is_valid_cluster(cluster, bpb))
    {
		    clust_count ++;
        if ((cluster & FAT12_MASK) >= CLUST_BAD){
          printf("Bad\n");
        }
        clust_ref[cluster] = 1; //good cluster
        cluster = get_fat_entry(cluster, image_buf, bpb);
    }
    if (is_end_of_file(cluster)){
      clust_ref[cluster] = 2; //end of file
    }
    else{
      clust_ref[cluster] = 3; //bad boy
    }
	return clust_count;
}

uint16_t print_dirent(struct direntry *dirent, int indent, int* clust_ref, uint8_t *image_buf, struct bpb33* bpb)
{
    uint16_t followclust = 0;

    int i;
    char name[9];
    char extension[4];
    uint32_t size;
    uint16_t file_cluster;
    name[8] = ' ';
    extension[3] = ' ';
    memcpy(name, &(dirent->deName[0]), 8);
    memcpy(extension, dirent->deExtension, 3);
    if (name[0] == SLOT_EMPTY)
    {
		return followclust;
    }

    /* skip over deleted entries */
    if (((uint8_t)name[0]) == SLOT_DELETED)
    {
		return followclust;
    }

    if (((uint8_t)name[0]) == 0x2E)
    {
	// dot entry ("." or "..")
	// skip it
        return followclust;
    }

    /* names are space padded - remove the spaces */
    for (i = 8; i > 0; i--)
    {
	if (name[i] == ' ')
	    name[i] = '\0';
	else
	    break;
    }

    /* remove the spaces from extensions */
    for (i = 3; i > 0; i--)
    {
	if (extension[i] == ' ')
	    extension[i] = '\0';
	else
	    break;
    }

    if ((dirent->deAttributes & ATTR_WIN95LFN) == ATTR_WIN95LFN)
    {
	// ignore any long file name extension entries
	//
	// printf("Win95 long-filename entry seq 0x%0x\n", dirent->deName[0]);
    }
    else if ((dirent->deAttributes & ATTR_VOLUME) != 0)
    {
	printf("Volume: %s\n", name);
    }
    else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0)
    {
        // don't deal with hidden directories; MacOS makes these
        // for trash directories and such; just ignore them.
	if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN)
        {
    	    printf("%s/ (directory)\n", name);
            file_cluster = getushort(dirent->deStartCluster);
            followclust = file_cluster;
        }
    }
    else
    {
        /*
         * a "regular" file entry
         * print attributes, size, starting cluster, etc.
         */
	int ro = (dirent->deAttributes & ATTR_READONLY) == ATTR_READONLY;
	int hidden = (dirent->deAttributes & ATTR_HIDDEN) == ATTR_HIDDEN;
	int sys = (dirent->deAttributes & ATTR_SYSTEM) == ATTR_SYSTEM;
	int arch = (dirent->deAttributes & ATTR_ARCHIVE) == ATTR_ARCHIVE;
  int start_cluster = getushort(dirent->deStartCluster);
	int clust_num = 0;
	size = getulong(dirent->deFileSize);
	clust_num = checker(image_buf, bpb, clust_ref, start_cluster);
	sz_check(size, clust_num, bpb);
	printf("%s.%s (%u bytes) (starting cluster %d) %c%c%c%c\n",
	       name, extension, size, start_cluster,
	       ro?'r':' ',
               hidden?'h':' ',
               sys?'s':' ',
               arch?'a':' ');
    }

    return followclust;
}


void follow_dir(uint16_t cluster, int indent,
		uint8_t *image_buf, struct bpb33* bpb, int* clust_ref)
{
    while (is_valid_cluster(cluster, bpb))
    {
        struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        int i = 0;
	for ( ; i < numDirEntries; i++)
	{

            uint16_t followclust = print_dirent(dirent, indent, clust_ref, image_buf, bpb);
            if (followclust)
                follow_dir(followclust, indent+1, image_buf, bpb, clust_ref);
            dirent++;
	}

	cluster = get_fat_entry(cluster, image_buf, bpb);
    }
}


void traverse_root(uint8_t *image_buf, struct bpb33* bpb, int* clust_ref)
{
    uint16_t cluster = 0;

    struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

    int i = 0;
    for ( ; i < bpb->bpbRootDirEnts; i++)
    {
        uint16_t followclust = print_dirent(dirent, 0, clust_ref, image_buf, bpb);
        if (is_valid_cluster(followclust, bpb))
            follow_dir(followclust, 1, image_buf, bpb, clust_ref);

        dirent++;
    }
}

void fix_FAT(uint8_t *image_buf, struct bpb33* bpb, int* clust_ref){
  uint16_t cluster = 0;
  uint16_t previous_cluster = 0;
  for(int i = 0; i < 2880; i++){
    if (clust_ref[i] == 3){
      set_fat_entry(cluster, CLUST_FREE, image_buf, bpb);
    }
    else if (clust_ref[i] == 0){
      if ((cluster & FAT12_MASK) != CLUST_FREE){//assuming this works

      }
    }
    previous_cluster = cluster;
    cluster = get_fat_entry(cluster, image_buf, bpb);
  }
}

void usage(char *progname) {
    fprintf(stderr, "usage: %s <imagename>\n", progname);
    exit(1);
}


int main(int argc, char** argv) {
    uint8_t *image_buf;
    int fd;
    struct bpb33* bpb;
    if (argc < 2) {
	usage(argv[0]);
    }

    image_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(image_buf);

    // your code should start here...
    int clust_ref[2880];
    for(int i = 0; i < 2880; i++){
      clust_ref[i] = 0;
    }
    traverse_root(image_buf, bpb, clust_ref);

    unmmap_file(image_buf, &fd);
    return 0;
}
