
#include "xmalloc.h"
#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include "hmalloc.h"
#include <pthread.h>

/*
  typedef struct hm_stats {
  long pages_mapped;
  long pages_unmapped;
  long chunks_allocated;
  long chunks_freed;
  long free_length;
  } hm_stats;
*/

const size_t PAGE_SIZE = 4096;
static hm_stats stats; // This initializes the stats to 0.
static pthread_mutex_t thread_safe_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct node_t {
	int size;
	struct node_t * next;
} node;

node* free_list = NULL;

long
free_list_length()
{
    int size = 0;
    node* curr = free_list;
    while(curr != NULL) {
        size++;
        curr = curr->next;
    }
    return size;
}

hm_stats*
hgetstats()
{
    stats.free_length = free_list_length();
    return &stats;
}

void
hprintstats()
{
    stats.free_length = free_list_length();
    fprintf(stderr, "\n== husky malloc stats ==\n");
    fprintf(stderr, "Mapped:   %ld\n", stats.pages_mapped);
    fprintf(stderr, "Unmapped: %ld\n", stats.pages_unmapped);
    fprintf(stderr, "Allocs:   %ld\n", stats.chunks_allocated);
    fprintf(stderr, "Frees:    %ld\n", stats.chunks_freed);
    fprintf(stderr, "Freelen:  %ld\n", stats.free_length);
}

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

void
print_free_list(node* head) {
    node* curr = head;
    int ii = 0;
    printf("Free List:\n");
    while (curr != NULL) {
        printf("Node %d: %p\n Size: %d\n", ii, curr, curr->size);
        ii++;
        curr = curr->next;
    }
    printf("-------------------\n");
}

/* Coalesce a free-list. */
node*
hcoalesce(node* head) {
    if (head != NULL) {
        if (head->next == NULL) {
            return head;
        } else {
            /* If consecutive nodes of free memory */
            if ((node*)((char*)head + sizeof(node) + head->size) == head->next) {
                head->size = head->size + sizeof(node) + head->next->size;
                head->next = head->next->next;
                return hcoalesce(head);
            } else {
                /* Not consecutive nodes. */
                return hcoalesce(head->next);
            }
        }
    } else {
        return head;
    }
}

void* hmalloc(size_t size) 
{
    size += sizeof(size_t);
    // printf("Size: %ld\n", size);
    if (size < sizeof(node)) {
        size = sizeof(node);
    }

    stats.chunks_allocated += 1;

    if (size >= PAGE_SIZE) {
        int num_pages = div_up(size, PAGE_SIZE);
        size_t block_size = num_pages * PAGE_SIZE;
        size_t* block = mmap(NULL, block_size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
        *block = block_size;
        stats.pages_mapped += num_pages; // Update pages_mapped
        return (char*)block + sizeof(size_t); // Return the start of the chunk of memory after the size header.
    } else {
        if (free_list == NULL) {
            free_list = (node*) mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
            free_list->size = 4096; //- sizeof(node);
            free_list->next = NULL;
            stats.pages_mapped += 1;
        }
        /* Look for a node that can fit my desired memory */
        node* prev = NULL;
        node* curr = free_list;
        while (curr != NULL) {
            if (curr->size >= size) {
                size_t leftover_size = curr->size - size;
                if (leftover_size > sizeof(node)) {
                    // Add a new node for the leftover memory
                    node* new = (node*)((char*)curr + size);
                    new->size = leftover_size;
                    new->next = curr->next;
                    if (prev == NULL) {
                        free_list = new;
                    } else {
                        prev->next = new;
                    }
                } else {
                    if (prev == NULL && curr->next == NULL) {
                    /* Leftover was not big enough to store a node */
                    /* This was the only node on the list, so set free_list to
                    NULL since there will be no more free cells on this list*/
                        free_list = NULL;
                    } else if (prev != NULL && curr->next == NULL) {
                        prev->next = NULL;
                    } else if (prev == NULL  && curr->next != NULL) {
                        free_list = curr->next;
                    } else {
                        prev->next = curr->next;
                    }

                    *(size_t*)curr = curr->size - sizeof(size_t); // size - sizeof(size_t);
                    return (void*)((char*)curr + sizeof(size_t)); // Return the pointer to the allocated memory
                }

                *(size_t*)curr = size - sizeof(size_t);
                return (void*)((char*)curr + sizeof(size_t)); // Return the pointer to the allocated memory

            }
            prev = curr;
            curr = curr->next; // Keep looping
        }

        // Could't find a block big enough
        size_t* block = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
        stats.pages_mapped += 1;
        size_t leftover_size = PAGE_SIZE - size;
        if (leftover_size > sizeof(node)) {
            *block = size - sizeof(size_t);
            // Add a new node for the leftover memory
            node* new = (node*)((char*)block + size);
            new->size = leftover_size;
            new->next = NULL;
            if (prev == NULL) {
                free_list = new;
            } else {
                prev->next = new;
            }
        } else {
            *block = PAGE_SIZE;
            return (void*)((char*) block + sizeof(size_t));
        }
        return (void*)((char*) block + sizeof(size_t));
    }
}

void*
xmalloc(size_t size)
{
	pthread_mutex_lock(&thread_safe_lock);
    void* ptr = hmalloc(size);
	pthread_mutex_unlock(&thread_safe_lock);    
    return ptr;
}

void
hfree(void* item)
{
    size_t* block_head = (size_t*)((char*) item - sizeof(size_t));
    size_t block_size = *block_head;

    if (block_size >= PAGE_SIZE) {
        munmap(item, block_size);
        stats.chunks_freed += 1;
        int num_pages = block_size / PAGE_SIZE;
        stats.pages_unmapped += num_pages; // Update pages_unmapped
    } else {
        node* curr = free_list;

        node* prev = NULL;
        // Go through nodes in free list
        while(curr != NULL) {
            // See if block of memory goes before current node
            if ((node*)block_head < curr) {
                node* new = (node*)block_head;
                new->next = curr;
                new->size = block_size + sizeof(size_t);
                if (prev != NULL) {
                    prev->next = new;
                }
                stats.chunks_freed += 1;
                break;
            } else if (curr->next == NULL) {
                /* If the freed memory belongs after the last node */
                node* new = (node*)block_head;
                new->next = NULL;
                new->size = block_size + sizeof(size_t);
                if (prev != NULL) {
                    prev->next = new;
                }
                stats.chunks_freed += 1;
            }
            // Update curr
            curr = curr->next;
        }
        // Coalescing
        free_list = hcoalesce(free_list);
    }
}

void
xfree(void* item)
{
    pthread_mutex_lock(&thread_safe_lock);
    hfree(item);
	pthread_mutex_unlock(&thread_safe_lock);
}


/* Reallocate memory */
void* xrealloc(void* ptr, size_t size) {
	//Null case
	if (ptr == NULL) { return xmalloc(size); }

	void* begin = (char*)ptr - sizeof(size_t);
	size_t* curr_size = (size_t*) begin;

	if(*curr_size == size) {
		//if equal
		return ptr;
	} else if (size > *curr_size) {
		//if given bytes are greater then memcpy
		void* memory = xmalloc(size);
		memcpy(memory, ptr, *curr_size - sizeof(size_t));
		xfree(ptr);
		return memory;
	} else {
		//third case where *curr_size > size
		int remainder = *curr_size - size;
		int node_size = sizeof(node);
		if (remainder >= node_size) {
			//thread safe it
			pthread_mutex_lock(&thread_safe_lock);

			// Add a new node for the leftover memory
			node* new = (node*)((char*)begin + *curr_size);
			new->size = remainder;
			// new->next = NULL;

			if (free_list == NULL) {
					free_list = new;
			} else {
                //placement at end
                node* curr = free_list;
                node* prev = NULL;

                while(curr != NULL) {
                    // See if block of memory goes before current node
                    if (new < curr) {
                        new->next = curr->next;
                        if (prev != NULL) {
                            prev->next = new;
                        }
                        break;
                    } else if (curr->next == NULL) {
                        /* If the free memory belongs after the last node */
                        new->next = NULL;
                        if (prev != NULL) {
                            prev->next = new;
                        }
                    }
                    // Update curr
                    curr = curr->next;
                }
			}
			//unlock thread safe
			pthread_mutex_unlock(&thread_safe_lock);
			//mutate old/current size
			*curr_size = size;
			return ptr;
		} else {
            return ptr;
		}
	}
}