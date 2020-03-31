
#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include "xmalloc.h"

#define NUM_BINS 7

const size_t PAGE_SIZE = 4096;
const int BIN_INDEX_OFFSET = 5;

typedef struct node_t {
	int size;
	struct node_t * next;
} node;

__thread node* bins[NUM_BINS];

int BIN_SIZES[] = {32, 64, 128, 256, 512, 1024, 2048};;

static
size_t
div_up(size_t xx, size_t yy)
{
    // This is useful to calculate # of pages
    // for large allocations.
    size_t zz = xx / yy;

    if (zz * yy == xx) {
        return zz;
    }
    else {
        return zz + 1;
    }
}


/* Decide what size bin it goes in */
size_t
assign_bin(size_t size)
{
    if (size <= BIN_SIZES[0]) {
        return BIN_SIZES[0];
    }

    for (int ii = 1; ii < NUM_BINS; ii++) {

        if (size <= BIN_SIZES[ii] && size > BIN_SIZES[ii - 1]) {
            return BIN_SIZES[ii];
        }

    }

    /* Was greater than last bin size */
    /* Try not to get here lol */
    return 0;
}

int
lookup(size_t bin_size)
{
    /* TODO: Get index of array that bin lives (using log)
    Aim for O(1) */

    /* log_2(x) = log_y(x) / log_y(2) */
    int index = log(bin_size) / log(2);
    index -= BIN_INDEX_OFFSET;
    return index;
}

/* Return a free_list of page_size bin_size cells */
node*
init_bin(size_t bin_size)
{
    int num_cells = PAGE_SIZE / bin_size;
    node* head = (node*)mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
    head->size = bin_size;
    node* prev = head;
    node* curr = NULL;

    for (int idx = 0; idx < num_cells - 1; idx++) {
        /* Create node */
        curr = (node*)((char*)prev + bin_size);

        /* Set node size field */
        curr->size = bin_size;

        /* Set size header */
        *(size_t*)((char*)curr + sizeof(node)) = bin_size - sizeof(size_t);

        /* Set previous's next */
        prev->next = curr;

        /* Update prev */
        prev = curr;
    }
    return head;
}

void*
allot_cell(size_t bin_size)
{
    /* Lookup the index to place bin */
    int ii = lookup(bin_size);

    /* If no bin yet */
    if (bins[ii] == NULL) {
        bins[ii] = init_bin(bin_size);
    }

    /* Get first cell of free list */
    node* fl = bins[ii];

    /* Set size header of free cell */
    *(size_t*)fl = bin_size - sizeof(size_t);

    /* Get ptr to start of free memory */
    void* ptr = (void*)((char*)fl + sizeof(size_t));


    /* Update head of free list */
    bins[ii] = fl->next;

    return ptr;
}

/* Return chunk at ptr to the free list */
void
release_cell(void* ptr, size_t bin_size)
{
    /* Place new cell */
    node* new_cell = (node*)((char*)ptr - sizeof(size_t));
    new_cell->size = bin_size;

    /* Get index of bin */
    int ii = lookup(bin_size);

    /* Add new_cell to the front of the free list */
    node* head = bins[ii];
    bins[ii] = new_cell;
    new_cell->next = head;
}

void
clean_up_bin(size_t bin_size)
{
    /*
        TODO: munmap whole free_list if it's all free
    */
}

void*
xmalloc(size_t size)
{
    /* Size cannot be smaller than the size of a node */
    if (size < sizeof(node)) {
        size = sizeof(node);
    }

    size += sizeof(size_t);

    // stats.chunks_allocated += 1;

    /* If size >= PAGE_SIZE */
    if (size >= BIN_SIZES[NUM_BINS - 1]) {

        int num_pages = div_up(size, PAGE_SIZE);
        size_t block_size = num_pages * PAGE_SIZE;
        size_t* block = mmap(NULL, block_size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
        *block = block_size;

        // stats.pages_mapped += num_pages;

        return (char*)block + sizeof(size_t); // Return the start of the chunk of memory after the size header.

    } else {
        /* Assign a bin size to the given size */
        size_t bin_size = assign_bin(size);

        /* Lookup the index to place bin */
        int ii = lookup(bin_size);

        /* If no bin yet */
        if (bins[ii] == NULL) {
            bins[ii] = init_bin(bin_size);
        }
        /* Allot a cell of memory in a bin */
        void* ptr = allot_cell(bin_size);
        return ptr;
    }
}

void
xfree(void* ptr)
{
    /* Get size of memory */
    size_t* block_head = (size_t*)((char*) ptr - sizeof(size_t));
    size_t block_size = *block_head;

    /* If size >= PAGE_SIZE */
    if (block_size >= BIN_SIZES[NUM_BINS - 1]) {
        /* Unmap memory */
        munmap(block_head, block_size);

        // stats.chunks_freed += 1;
        // stats.pages_unmapped += num_pages;

    } else {
        /* Release cell to bin's free list */
        release_cell(ptr, block_size + sizeof(size_t));

        /* Get rid of bin if it's all free */
        // clean_up_bin(lookup(bin_size));
    }
}

void*
xrealloc(void* prev, size_t bytes)
{
	void* ptr = xmalloc(bytes);
        size_t mem = *(size_t*)((char*) prev - sizeof(size_t));
	memcpy(ptr, prev, mem);
        xfree(prev);
	return ptr;
}
