#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#include "cache.h"
#include "main.h"

/* cache configuration parameters */
static int cache_usize = DEFAULT_CACHE_SIZE;
static int cache_block_size = DEFAULT_CACHE_BLOCK_SIZE;
static int words_per_block = DEFAULT_CACHE_BLOCK_SIZE / WORD_SIZE;
static int cache_assoc = DEFAULT_CACHE_ASSOC;
static int cache_writeback = DEFAULT_CACHE_WRITEBACK;
static int cache_writealloc = DEFAULT_CACHE_WRITEALLOC;
static int num_core = DEFAULT_NUM_CORE;

/* cache model data structures */
/* max of 8 cores */
static cache mesi_cache[8];
static cache_stat mesi_cache_stat[8];

/************************************************************/
void set_cache_param(param, value)
  int param;
  int value;
{
  switch (param) {
  case NUM_CORE:
	num_core = value;
	break;
  case CACHE_PARAM_BLOCK_SIZE:
	cache_block_size = value;
	words_per_block = value / WORD_SIZE;
	break;
  case CACHE_PARAM_USIZE:
	cache_usize = value;
	break;
  case CACHE_PARAM_ASSOC:
	cache_assoc = value;
	break;
  default:
	printf("error set_cache_param: bad parameter value\n");
	exit(-1);
  }
}
/************************************************************/

/************************************************************/
void init_cache()
{
  /* initialize the cache, and cache statistics data structures */

	int i;
	//unified cache for each core
	for(i = 0; i < num_core; i++) {
		mesi_cache[i].id = i;
		mesi_cache[i].size = cache_usize / WORD_SIZE;    
		mesi_cache[i].associativity = cache_assoc;
		mesi_cache[i].n_sets = cache_usize/cache_block_size/cache_assoc;
		mesi_cache[i].index_mask = (mesi_cache[i].n_sets-1) << LOG2(cache_block_size);
		mesi_cache[i].index_mask_offset = LOG2(cache_block_size);

		mesi_cache_stat[i].accesses = 0;
		mesi_cache_stat[i].misses = 0;
		mesi_cache_stat[i].replacements = 0;
		mesi_cache_stat[i].demand_fetches = 0;
		mesi_cache_stat[i].copies_back = 0;
		mesi_cache_stat[i].broadcasts = 0;
		
		//allocate the array of cache line pointers
		mesi_cache[i].LRU_head = (Pcache_line *)malloc(sizeof(Pcache_line)*mesi_cache[i].n_sets);
		mesi_cache[i].LRU_tail = (Pcache_line *)malloc(sizeof(Pcache_line)*mesi_cache[i].n_sets);
		mesi_cache[i].set_contents = (int *)malloc(sizeof(int)*mesi_cache[i].n_sets);
		int j;
		//allocate each cache line in the pointer array
		for(j = 0; j < mesi_cache[i].n_sets; j++) {
			mesi_cache[i].LRU_head[j] = NULL;
			mesi_cache[i].LRU_tail[j] = NULL;
			mesi_cache[i].set_contents[j] = 0;
		}
	}
}
/************************************************************/

/************************************************************/
void perform_access(addr, access_type, pid)
	unsigned addr, access_type, pid;
{
  /* handle accesses to the mesi caches */
	access_cache(mesi_cache[pid], addr, access_type);
}
/************************************************************/

/************************************************************/
void access_cache(c, addr, access_type)
  cache c;
  unsigned addr, access_type;
{
	/*Function to access cache*/

	unsigned index = (addr & c.index_mask) >> c.index_mask_offset;
	unsigned addr_tag = addr >> (c.index_mask_offset + LOG2(c.n_sets));

	int i;
	int flag1 = FALSE;
	int flag2 = FALSE;
	int flag3 = FALSE;
	int hit_flag = FALSE;
	Pcache_line temp;

	//Cache access stat
	mesi_cache_stat[c.id].accesses += 1; 

	//Compulsory miss
	if (c.LRU_head[index] == NULL){
		mesi_cache_stat[c.id].misses += 1;
		mesi_cache_stat[c.id].broadcasts += 1;
		mesi_cache_stat[c.id].demand_fetches += words_per_block;
		temp = (Pcache_line *)malloc(sizeof(cache_line));
        temp->tag = addr_tag;
        temp->LRU_prev = NULL;
        temp->LRU_next = NULL;
		//Remote read miss
		if (access_type == TRACE_LOAD) {
			//Change modified/exclusive to shared
			flag1 = update_state(c.id, MODIFIED, SHARED, addr_tag, index);
			flag2 = update_state(c.id, EXCLUSIVE, SHARED, addr_tag, index);
			flag3 = update_state(c.id, SHARED, SHARED, addr_tag, index);
			//data from cache
			if (flag1 == TRUE || flag2 == TRUE || flag3 == TRUE) {
				temp->state = SHARED;
			}
			//data from memory
			else{
				temp->state = EXCLUSIVE;
			}

		}
		//Remote write miss
		else if (access_type == TRACE_STORE) {
			//Change shared/modified/exclusive to invalid
			update_state(c.id, MODIFIED, INVALID, addr_tag, index);
			update_state(c.id, SHARED, INVALID, addr_tag, index);
			update_state(c.id, EXCLUSIVE, INVALID, addr_tag, index);
			temp->state = MODIFIED;
		}
		c.LRU_head[index] = temp;
        c.LRU_tail[index] = temp;
        c.set_contents[index] += 1;
	}

	//Else, check cache set
	else{
		temp = c.LRU_head[index];
		for (i = 0; i < c.set_contents[index]; i++) {
			if (temp->tag == addr_tag) {
				//Read hit
				if (access_type == TRACE_LOAD && temp->state != INVALID) {
					hit_flag = TRUE;
					//Insert cache line at head
					if (c.set_contents[index] > 1){
						delete(&c.LRU_head[index], &c.LRU_tail[index], temp);
						insert(&c.LRU_head[index], &c.LRU_tail[index], temp);
					}
					break;
				}
				//Write hit
				else if (access_type == TRACE_STORE && temp->state != INVALID) {
					hit_flag = TRUE;
					//State transition to invalid
					if (temp->state == SHARED) {
						update_state(c.id, SHARED, INVALID, addr_tag, index);
						mesi_cache_stat[c.id].broadcasts += 1;
					}
					//Otherwise this core has the only copy
					temp->state = MODIFIED;
					if (c.set_contents[index] > 1){
						delete(&c.LRU_head[index], &c.LRU_tail[index], temp);
						insert(&c.LRU_head[index], &c.LRU_tail[index], temp);
					}
					break;
				}
			}
			//We've reached the tail
			if (temp->LRU_next == NULL) {
				break;
			}
			temp = temp->LRU_next;
		}

		//Cache miss
		if (hit_flag == FALSE) {
			mesi_cache_stat[c.id].misses += 1;
			mesi_cache_stat[c.id].broadcasts += 1;
			mesi_cache_stat[c.id].demand_fetches += words_per_block;
			//Insert cache line if one is free
			if (c.set_contents[index] < c.associativity) {
				temp = (Pcache_line *)malloc(sizeof(cache_line));
                temp->tag = addr_tag;
                //Remote read miss
                if (access_type == TRACE_LOAD) {
		             //Change modified/exclusive to shared
					flag1 = update_state(c.id, MODIFIED, SHARED, addr_tag, index);
					flag2 = update_state(c.id, EXCLUSIVE, SHARED, addr_tag, index);
					flag3 = update_state(c.id, SHARED, SHARED, addr_tag, index);
					//data from cache
					if (flag1 == TRUE || flag2 == TRUE || flag3 == TRUE) {
						temp->state = SHARED;
					}
					//data from memory
					else{
						temp->state = EXCLUSIVE;
					}
                }
                //Remote write miss
				else if (access_type == TRACE_STORE) {
					//Change shared/modified/exclusive to invalid
					update_state(c.id, MODIFIED, INVALID, addr_tag, index);
					update_state(c.id, SHARED, INVALID, addr_tag, index);
					update_state(c.id, EXCLUSIVE, INVALID, addr_tag, index);
					temp->state = MODIFIED;
				}
				insert(&c.LRU_head[index], &c.LRU_tail[index], temp);
				c.set_contents[index] += 1;
			}
			//Cache eviction
			else if (c.set_contents[index] == c.associativity) {
				mesi_cache_stat[c.id].replacements += 1;
				if (temp->state == MODIFIED) {
					mesi_cache_stat[c.id].copies_back += 1;
				}
				delete(&c.LRU_head[index], &c.LRU_tail[index], temp);
				temp = (Pcache_line *)malloc(sizeof(cache_line));
                temp->tag = addr_tag;
                //Remote read miss
                if (access_type == TRACE_LOAD) {
		             //Change modified/exclusive to shared
					flag1 = update_state(c.id, MODIFIED, SHARED, addr_tag, index);
					flag2 = update_state(c.id, EXCLUSIVE, SHARED, addr_tag, index);
					flag3 = update_state(c.id, SHARED, SHARED, addr_tag, index);
					//data from cache
					if (flag1 == TRUE || flag2 == TRUE || flag3 == TRUE) {
						temp->state = SHARED;
					}
					//data from memory
					else{
						temp->state = EXCLUSIVE;
					}
                }
                //Remote write miss
				else if (access_type == TRACE_STORE) {
					//Change shared/modified/exclusive to invalid
					update_state(c.id, MODIFIED, INVALID, addr_tag, index);
					update_state(c.id, SHARED, INVALID, addr_tag, index);
					update_state(c.id, EXCLUSIVE, INVALID, addr_tag, index);
					temp->state = MODIFIED;
				}
				insert(&c.LRU_head[index], &c.LRU_tail[index], temp);
			}
		}
	}
}
/************************************************************/

/************************************************************/
int update_state(id, old_state, new_state, addr_tag, index)
  int id, old_state, new_state;
  unsigned addr_tag, index;
{
	/*Updates cache block states for other cores*/

	int i, j;
	int from_cache = FALSE;
	Pcache_line temp;
	for (i = 0; i < id; i ++) {
		if (mesi_cache[i].LRU_head[index] != NULL) {
			temp = mesi_cache[i].LRU_head[index];
			for (j = 0; j < mesi_cache[i].set_contents[index]; j++) {
				if (temp->tag == addr_tag) {
					if (temp->state == old_state) {
						temp->state = new_state;
						from_cache = TRUE;
						break;
					}
				}
				//we've reached the tail
	            if (temp->LRU_next == NULL) {
	                break;
	            }
	            temp = temp->LRU_next;
			}
		}
	}
	for (i = id + 1; i < num_core; i ++) {
		if (mesi_cache[i].LRU_head[index] != NULL) {
			temp = mesi_cache[i].LRU_head[index];
			for (j = 0; j < mesi_cache[i].set_contents[index]; j++) {
				if (temp->tag == addr_tag) {
					if (temp->state == old_state) {
						temp->state = new_state;
						from_cache = TRUE;
						break;
					}
				}
				//we've reached the tail
	            if (temp->LRU_next == NULL) {
	                break;
	            }
	            temp = temp->LRU_next;
			}
		}
	}
	return from_cache;
}
/************************************************************/

/************************************************************/
void flush()
{
  /* flush the mesi caches */
	int i, j;
	for (i = 0; i < num_core; i++) {
		for(j = 0; j < mesi_cache[i].n_sets; j++) {
			Pcache_line flush_line;
			if(mesi_cache[i].LRU_head[j] != NULL) {
				for(flush_line = mesi_cache[i].LRU_head[j]; flush_line != mesi_cache[i].LRU_tail[j]->LRU_next; flush_line = flush_line->LRU_next) {
					if(flush_line != NULL && flush_line->state == MODIFIED) {
						mesi_cache_stat[i].copies_back += words_per_block;
					}
				}
			}
		}
	}
}
/************************************************************/

/************************************************************/
void delete(head, tail, item)
  Pcache_line *head, *tail;
  Pcache_line item;
{
  if (item->LRU_prev) {
	item->LRU_prev->LRU_next = item->LRU_next;
  } else {
	/* item at head */
	*head = item->LRU_next;
  }

  if (item->LRU_next) {
	item->LRU_next->LRU_prev = item->LRU_prev;
  } else {
	/* item at tail */
	*tail = item->LRU_prev;
  }
}
/************************************************************/

/************************************************************/
/* inserts at the head of the list */
void insert(head, tail, item)
  Pcache_line *head, *tail;
  Pcache_line item;
{
  item->LRU_next = *head;
  item->LRU_prev = (Pcache_line)NULL;

  if (item->LRU_next)
	item->LRU_next->LRU_prev = item;
  else
	*tail = item;

  *head = item;
}
/************************************************************/

/************************************************************/
void dump_settings()
{
  printf("Cache Settings:\n");
  printf("\tSize: \t%d\n", cache_usize);
  printf("\tAssociativity: \t%d\n", cache_assoc);
  printf("\tBlock size: \t%d\n", cache_block_size);
}
/************************************************************/

/************************************************************/
void print_stats()
{
  int i;
  int demand_fetches = 0;
  int copies_back = 0;
  int broadcasts = 0;

  printf("*** CACHE STATISTICS ***\n");

  for (i = 0; i < num_core; i++) {
	printf("  CORE %d\n", i);
	printf("  accesses:  %d\n", mesi_cache_stat[i].accesses);
	printf("  misses:    %d\n", mesi_cache_stat[i].misses);
	printf("  miss rate: %f (%f)\n", 
	   (float)mesi_cache_stat[i].misses / (float)mesi_cache_stat[i].accesses,
	   1.0 - (float)mesi_cache_stat[i].misses / (float)mesi_cache_stat[i].accesses);
	printf("  replace:   %d\n", mesi_cache_stat[i].replacements);
  }

  printf("\n");
  printf("  TRAFFIC\n");
  for (i = 0; i < num_core; i++) {
	demand_fetches += mesi_cache_stat[i].demand_fetches;
	copies_back += mesi_cache_stat[i].copies_back;
	broadcasts += mesi_cache_stat[i].broadcasts;
  }
  printf("  demand fetch (words): %d\n", demand_fetches);
  /* number of broadcasts */
  printf("  broadcasts:           %d\n", broadcasts);
  printf("  copies back (words):  %d\n", copies_back);
}
/************************************************************/