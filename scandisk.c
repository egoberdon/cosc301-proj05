#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <ctype.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"

uint16_t free_clust(int* clust_ref, uint16_t cluster, int num_correct, struct bpb33 *bpb, uint8_t *image_buf){
	u_int16_t extra_cluster = 0;
	u_int16_t temp_cluster = 0;
	for(int i = 0; i < num_correct; i++){
		cluster = get_fat_entry(cluster, image_buf, bpb);
	}
	extra_cluster = get_fat_entry(cluster, image_buf, bpb);
	set_fat_entry(cluster, FAT12_MASK & CLUST_EOFS, image_buf, bpb);
	int count_freed = 0;
	while (is_valid_cluster(extra_cluster, bpb))
		{
			temp_cluster = extra_cluster;
			set_fat_entry(extra_cluster, FAT12_MASK & CLUST_FREE, image_buf, bpb);
			extra_cluster = get_fat_entry(temp_cluster, image_buf, bpb);
			count_freed++;
		}
	set_fat_entry(extra_cluster, FAT12_MASK & CLUST_FREE, image_buf, bpb); //clean up final cluster
	return count_freed;
}

//Divides, rounds up, used in sz_chck
int div_round_up(int dividend, int divisor){
	return (dividend + (divisor - 1)) / divisor;
}

//compares size in metadata to size in clusters
void sz_check(int meta_size, int clust_count, struct bpb33* bpb, int start_cluster, uint8_t *image_buf, int* clust_ref, struct direntry *dirent){
	int clust_size = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
	int max_size = clust_count * clust_size;  //largest possible size according to cluster chain
	int min_size = max_size - clust_size;   //smallest size according to cluster chain
	if (meta_size > max_size){
		printf("Big YIKES! -> Metadata is bigger than allocated clusters, has size %d, according to clusters size should be between %d and %d\n",
				meta_size, min_size, max_size);
				//u_int32_t new_file_size = 0;
				putulong(dirent->deFileSize, max_size);

    }
  	if (meta_size < min_size){
		  int no_bad_clust = div_round_up(min_size - meta_size, clust_size);
			printf("Small YIKES! -> Metadata is smaller than allocated clusters, need to free %d cluster(s).\n", no_bad_clust);
			printf("Freeing %d clusters\n", no_bad_clust);
			int freed = free_clust(clust_ref, start_cluster, (meta_size/clust_size), bpb, image_buf);
			printf("Freed %d clusters\n", freed);
	}
}

int checker(uint8_t *image_buf, struct bpb33 *bpb, int* clust_ref, uint16_t cluster)
{
	int clust_count = 0;
	uint16_t previous_cluster = 0;
  while (is_valid_cluster(cluster, bpb))
    {
		    clust_count ++;
        if ((cluster & FAT12_MASK) == (CLUST_BAD & FAT12_MASK)){
          printf("Found bad cluster\n");
					set_fat_entry(previous_cluster, FAT12_MASK & CLUST_EOFS, image_buf, bpb); //set previous to EOF
					set_fat_entry(cluster, FAT12_MASK & CLUST_FREE, image_buf, bpb); //free current
					cluster = previous_cluster;
					break;
        }
        clust_ref[cluster] = 1; //we reached this cluster
				previous_cluster = cluster;
        cluster = get_fat_entry(cluster, image_buf, bpb);
    }
    if (is_end_of_file(cluster)){
      clust_ref[cluster] = 1;
    }
    else{
			//this is super bad
			printf("\n");
			printf("Found super bad thing!!\n");
			printf("\n");
			set_fat_entry(previous_cluster, FAT12_MASK & CLUST_EOFS, image_buf, bpb); //set previous to EOF
			set_fat_entry(cluster, FAT12_MASK & CLUST_FREE, image_buf, bpb); //free current
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
	sz_check(size, clust_num, bpb, start_cluster, image_buf, clust_ref, dirent);
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

/* write the values into a directory entry */
void write_dirent(struct direntry *dirent, char *filename,
		  uint16_t start_cluster, uint32_t size)
{
    char *p, *p2;
    char *uppername;
    int len, i;

    /* clean out anything old that used to be here */
    memset(dirent, 0, sizeof(struct direntry));

    /* extract just the filename part */
    uppername = strdup(filename);
    p2 = uppername;
    for (i = 0; i < strlen(filename); i++)
    {
	if (p2[i] == '/' || p2[i] == '\\')
	{
	    uppername = p2+i+1;
	}
    }

    /* convert filename to upper case */
    for (i = 0; i < strlen(uppername); i++)
    {
	uppername[i] = toupper(uppername[i]);
    }

    /* set the file name and extension */
    memset(dirent->deName, ' ', 8);
    p = strchr(uppername, '.');
    memcpy(dirent->deExtension, "___", 3);
    if (p == NULL)
    {
	fprintf(stderr, "No filename extension given - defaulting to .___\n");
    }
    else
    {
	*p = '\0';
	p++;
	len = strlen(p);
	if (len > 3) len = 3;
	memcpy(dirent->deExtension, p, len);
    }

    if (strlen(uppername)>8)
    {
	uppername[8]='\0';
    }
    memcpy(dirent->deName, uppername, strlen(uppername));
    free(p2);

    /* set the attributes and file size */
    dirent->deAttributes = ATTR_NORMAL;
    putushort(dirent->deStartCluster, start_cluster);
    putulong(dirent->deFileSize, size);

    /* could also set time and date here if we really
       cared... */
}

void orphan_alloc(uint8_t *image_buf, struct bpb33* bpb, u_int16_t cluster, int orph_num){ //adds a new root directory address for orphan block
	uint16_t start_cluster = cluster;
	int j = 0;
	while (is_valid_cluster(cluster, bpb)){
		cluster = get_fat_entry(cluster, image_buf, bpb);
		j++;
	}
	set_fat_entry(cluster, FAT12_MASK & CLUST_EOFS, image_buf, bpb);
	int clust_size = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
	int size = j * clust_size;
	char buffer[64];
	snprintf(buffer, 64, "found%d.dat", orph_num);
	struct direntry *dirent = (struct direntry*)cluster_to_addr(0, image_buf, bpb); //root dirent
	write_dirent(dirent, buffer, start_cluster, size);
}
void fix_FAT(uint8_t *image_buf, struct bpb33* bpb, int* clust_ref){
  uint16_t cluster = 0;
	uint16_t fatent  = 0;
	int foundFree = 0;
	int k = 0;
  for(int i = 0; i < 2880; i++){
		fatent = get_fat_entry(cluster, image_buf, bpb);
		if ((fatent & FAT12_MASK) == (FAT12_MASK & CLUST_FREE)){
				foundFree = 1;
		}
		if (foundFree && (clust_ref[i] == 0) && ((fatent & FAT12_MASK) != (FAT12_MASK & CLUST_FREE))){
			k++;
			printf("i is: %d, fatent is %d\n", i, fatent);
			printf("Orphan found, get a parent!\n");
			orphan_alloc(image_buf, bpb, cluster, k);
    }
    cluster++;
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
		fix_FAT(image_buf, bpb, clust_ref);

    unmmap_file(image_buf, &fd);
    return 0;
}
