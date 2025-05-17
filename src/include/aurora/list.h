/**
 * @file list.h
 * @brief Double linked list library from the Linux Kernel
 *
 * Author: Linux Kernel Developers
 * Derived from: github.com/torvalds/linux/blob/master/include/linux/list.h
 */

/* SPDX-License-Identifier: GPL-2.0 */
#pragma once

#include "pico/stdlib.h"
#include "pico/types.h"

#include "container_of.h"

/*
 * Architectures might want to move the poison pointer offset
 * into some well-recognized area such as 0xdead000000000000,
 * that is also not mappable by user-space exploits:
 */
#ifdef CONFIG_ILLEGAL_POINTER_VALUE
# define POISON_POINTER_DELTA _AC(CONFIG_ILLEGAL_POINTER_VALUE, UL)
#else
# define POISON_POINTER_DELTA 0
#endif

#define LIST_POISON1  ((void *) 0x100 + POISON_POINTER_DELTA)
#define LIST_POISON2  ((void *) 0x122 + POISON_POINTER_DELTA)

/*
 * Use __READ_ONCE() instead of READ_ONCE() if you do not require any
 * atomicity. Note that this may result in tears!
 */
#ifndef __READ_ONCE
#define __READ_ONCE(x)  (*(const volatile __unqual_scalar_typeof(x) *)&(x))
#endif

#define READ_ONCE(x)                                                    \
({                                                                      \
        __READ_ONCE(x);                                                 \
})

#define __WRITE_ONCE(x, val)                                            \
do {                                                                    \
        *(volatile typeof(x) *)&(x) = (val);                            \
} while (0)

#define WRITE_ONCE(x, val)                                              \
do {                                                                    \
        __WRITE_ONCE(x, val);                                           \
} while (0)

struct list_head {
    struct list_head *next, *prev;
};

struct hlist_head {
    struct hlist_node *first;
};

struct hlist_node {
    struct hlist_node *next, **pprev;
};

/*
 * Circular doubly linked list implementation.
 *
 * Some of the internal functions ("__xxx") are useful when
 * manipulating whole lists rather than single entries, as
 * sometimes we already know the next/prev entries and we can
 * generate better code by using them directly rather than
 * using the generic single-entry routines.
 */

#define LIST_HEAD_INIT(name) (struct list_head) { &(name), &(name) }

#define LIST_HEAD(name) \
	struct list_head name = LIST_HEAD_INIT(name)

/**
 * @brief Initialize a list_head structure
 *
 * @param list: list_head structure to be initialized.
 *
 * Initializes the list_head to point to itself.  If it is a list header,
 * the result is an empty list.
 */
static inline void INIT_LIST_HEAD(struct list_head *list)
{
	WRITE_ONCE(list->next, list);
	WRITE_ONCE(list->prev, list);
}

static inline bool __list_add_valid(struct list_head *new,
				struct list_head *prev,
				struct list_head *next)
{
	return true;
}
static inline bool __list_del_entry_valid(struct list_head *entry)
{
	return true;
}

/**
 * @brief Insert a new entry between two known consecutive entries.
 *
 * @param new: new entry to be added
 * @param prev: the entry before the new entry
 * @param next: the entry after the new entry
 *
 * This is only for internal list manipulation where we know
 * the prev/next entries already!
 */
static inline void __list_add(struct list_head *new,
			      struct list_head *prev,
			      struct list_head *next)
{
	if (!__list_add_valid(new, prev, next))
		return;

	next->prev = new;
	new->next = next;
	new->prev = prev;
	WRITE_ONCE(prev->next, new);
}

/**
 * @brief list_add - add a new entry
 *
 * @param new: new entry to be added
 * @param head: list head to add it after
 *
 * Insert a new entry after the specified head.
 * This is good for implementing stacks.
 */
static inline void list_add(struct list_head *new, struct list_head *head)
{
	__list_add(new, head, head->next);
}

/**
 * @brief list_add_tail - add a new entry
 *
 * @param new: new entry to be added
 * @param head: list head to add it before
 *
 * Insert a new entry before the specified head.
 * This is useful for implementing queues.
 */
static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
	__list_add(new, head->prev, head);
}

/**
 * @brief Delete a list entry by making the prev/next entries
 * point to each other.
 *
 * @param prev: the entry before the entry to be deleted
 * @param next: the entry after the entry to be deleted
 *
 * This is only for internal list manipulation where we know
 * the prev/next entries already!
 */
static inline void __list_del(struct list_head * prev, struct list_head * next)
{
	next->prev = prev;
	WRITE_ONCE(prev->next, next);
}

/**
 * @brief Delete a list entry and clear the 'prev' pointer.
 *
 * @param entry: the element to delete the previous pointer from
 *
 * This is a special-purpose list clearing method used in the networking code
 * for lists allocated as per-cpu, where we don't want to incur the extra
 * WRITE_ONCE() overhead of a regular list_del_init(). The code that uses this
 * needs to check the node 'prev' pointer instead of calling list_empty().
 */
static inline void __list_del_clearprev(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
	entry->prev = NULL;
}

static inline void __list_del_entry(struct list_head *entry)
{
	if (!__list_del_entry_valid(entry))
		return;

	__list_del(entry->prev, entry->next);
}

/**
 * @brief list_del - deletes entry from list.
 *
 * @param entry: the element to delete from the list.
 *
 * @note list_empty() on entry does not return true after this, the entry is
 * in an undefined state.
 */
static inline void list_del(struct list_head *entry)
{
	__list_del_entry(entry);
	entry->next = LIST_POISON1;
	entry->prev = LIST_POISON2;
}

/**
 * @brief list_replace - replace old entry by new one
 *
 * @param old : the element to be replaced
 * @param new : the new element to insert
 *
 * If @old was empty, it will be overwritten.
 */
static inline void list_replace(struct list_head *old,
				struct list_head *new)
{
	new->next = old->next;
	new->next->prev = new;
	new->prev = old->prev;
	new->prev->next = new;
}

/**
 * @brief list_replace_init - replace old entry by new one and initialize the old one
 *
 * @param old : the element to be replaced
 * @param new : the new element to insert
 *
 * If @old was empty, it will be overwritten.
 */
static inline void list_replace_init(struct list_head *old,
				     struct list_head *new)
{
	list_replace(old, new);
	INIT_LIST_HEAD(old);
}

/**
 * @brief list_swap - replace entry1 with entry2 and re-add entry1 at entry2's position
 *
 * @param entry1: the location to place entry2
 * @param entry2: the location to place entry1
 */
static inline void list_swap(struct list_head *entry1,
			     struct list_head *entry2)
{
	struct list_head *pos = entry2->prev;

	list_del(entry2);
	list_replace(entry1, entry2);
	if (pos == entry1)
		pos = entry2;
	list_add(entry1, pos);
}

/**
 * @brief list_del_init - deletes entry from list and reinitialize it.
 *
 * @param entry: the element to delete from the list.
 */
static inline void list_del_init(struct list_head *entry)
{
	__list_del_entry(entry);
	INIT_LIST_HEAD(entry);
}

/**
 * @brief list_move - delete from one list and add as another's head
 *
 * @param list: the entry to move
 *
 * @param head: the head that will precede our entry
 */
static inline void list_move(struct list_head *list, struct list_head *head)
{
	__list_del_entry(list);
	list_add(list, head);
}

/**
 * @brief list_move_tail - delete from one list and add as another's tail
 *
 * @param list: the entry to move
 * @param head: the head that will follow our entry
 */
static inline void list_move_tail(struct list_head *list,
				  struct list_head *head)
{
	__list_del_entry(list);
	list_add_tail(list, head);
}

/**
 * @brief list_bulk_move_tail - move a subsection of a list to its tail
 *
 * @param head: the head that will follow our entry
 * @param first: first entry to move
 * @param last: last entry to move, can be the same as first
 *
 * Move all entries between @first and including @last before @head.
 * All three entries must belong to the same linked list.
 */
static inline void list_bulk_move_tail(struct list_head *head,
				       struct list_head *first,
				       struct list_head *last)
{
	first->prev->next = last->next;
	last->next->prev = first->prev;

	head->prev->next = first;
	first->prev = head->prev;

	last->next = head;
	head->prev = last;
}

/**
 * @brief list_is_first -- tests whether @list is the first entry in list @head
 *
 * @param list: the entry to test
 * @param head: the head of the list
 */
static inline int list_is_first(const struct list_head *list, const struct list_head *head)
{
	return list->prev == head;
}

/**
 * @brief list_is_last - tests whether @list is the last entry in list @head
 *
 * @param list: the entry to test
 * @param head: the head of the list
 */
static inline int list_is_last(const struct list_head *list, const struct list_head *head)
{
	return list->next == head;
}

/**
 * @brief list_is_head - tests whether @list is the list @head
 *
 * @param list: the entry to test
 * @param head: the head of the list
 */
static inline int list_is_head(const struct list_head *list, const struct list_head *head)
{
	return list == head;
}

/**
 * @brief list_empty - tests whether a list is empty
 *
 * @param head: the list to test.
 */
static inline int list_empty(const struct list_head *head)
{
	return READ_ONCE(head->next) == head;
}

/**
 * @brief list_rotate_left - rotate the list to the left
 *
 * @param head: the head of the list
 */
static inline void list_rotate_left(struct list_head *head)
{
	struct list_head *first;

	if (!list_empty(head)) {
		first = head->next;
		list_move_tail(first, head);
	}
}

/**
 * @brief list_rotate_to_front() - Rotate list to specific item.
 *
 * @param list: The desired new front of the list.
 * @param head: The head of the list.
 *
 * Rotates list so that @list becomes the new front of the list.
 */
static inline void list_rotate_to_front(struct list_head *list,
					struct list_head *head)
{
	/*
	 * Deletes the list head from the list denoted by @head and
	 * places it as the tail of @list, this effectively rotates the
	 * list so that @list is at the front.
	 */
	list_move_tail(head, list);
}

/**
 * @brief list_is_singular - tests whether a list has just one entry.
 *
 * @param head: the list to test.
 */
static inline int list_is_singular(const struct list_head *head)
{
	return !list_empty(head) && (head->next == head->prev);
}

static inline void __list_cut_position(struct list_head *list,
		struct list_head *head, struct list_head *entry)
{
	struct list_head *new_first = entry->next;
	list->next = head->next;
	list->next->prev = list;
	list->prev = entry;
	entry->next = list;
	head->next = new_first;
	new_first->prev = head;
}

/**
 * @brief list_cut_position - cut a list into two
 *
 * @param list: a new list to add all removed entries
 * @param head: a list with entries
 * @param entry: an entry within head, could be the head itself
 *	and if so we won't cut the list
 *
 * This helper moves the initial part of @head, up to and
 * including @entry, from @head to @list. You should
 * pass on @entry an element you know is on @head. @list
 * should be an empty list or a list you do not care about
 * losing its data.
 *
 */
static inline void list_cut_position(struct list_head *list,
		struct list_head *head, struct list_head *entry)
{
	if (list_empty(head))
		return;
	if (list_is_singular(head) && !list_is_head(entry, head) && (entry != head->next))
		return;
	if (list_is_head(entry, head))
		INIT_LIST_HEAD(list);
	else
		__list_cut_position(list, head, entry);
}

/**
 * @brief list_cut_before - cut a list into two, before given entry
 *
 * @param list: a new list to add all removed entries
 * @param head: a list with entries
 * @param entry: an entry within head, could be the head itself
 *
 * This helper moves the initial part of @head, up to but
 * excluding @entry, from @head to @list.  You should pass
 * in @entry an element you know is on @head.  @list should
 * be an empty list or a list you do not care about losing
 * its data.
 * If @entry == @head, all entries on @head are moved to
 * @list.
 */
static inline void list_cut_before(struct list_head *list,
				   struct list_head *head,
				   struct list_head *entry)
{
	if (head->next == entry) {
		INIT_LIST_HEAD(list);
		return;
	}
	list->next = head->next;
	list->next->prev = list;
	list->prev = entry->prev;
	list->prev->next = list;
	head->next = entry;
	entry->prev = head;
}

static inline void __list_splice(const struct list_head *list,
				 struct list_head *prev,
				 struct list_head *next)
{
	struct list_head *first = list->next;
	struct list_head *last = list->prev;

	first->prev = prev;
	prev->next = first;

	last->next = next;
	next->prev = last;
}

/**
 * @brief list_splice - join two lists, this is designed for stacks
 *
 * @param list: the new list to add.
 * @param head: the place to add it in the first list.
 */
static inline void list_splice(const struct list_head *list,
				struct list_head *head)
{
	if (!list_empty(list))
		__list_splice(list, head, head->next);
}

/**
 * @brief list_splice_tail - join two lists, each list being a queue
 *
 * @param list: the new list to add.
 * @param head: the place to add it in the first list.
 */
static inline void list_splice_tail(struct list_head *list,
				struct list_head *head)
{
	if (!list_empty(list))
		__list_splice(list, head->prev, head);
}

/**
 * @brief list_splice_init - join two lists and reinitialise the emptied list.
 * @param list: the new list to add.
 * @param head: the place to add it in the first list.
 *
 * The list at @list is reinitialised
 */
static inline void list_splice_init(struct list_head *list,
				    struct list_head *head)
{
	if (!list_empty(list)) {
		__list_splice(list, head, head->next);
		INIT_LIST_HEAD(list);
	}
}

/**
 * @brief list_splice_tail_init - join two lists and reinitialise the emptied list
 *
 * @param list: the new list to add.
 * @param head: the place to add it in the first list.
 *
 * Each of the lists is a queue.
 * The list at @list is reinitialised
 */
static inline void list_splice_tail_init(struct list_head *list,
					 struct list_head *head)
{
	if (!list_empty(list)) {
		__list_splice(list, head->prev, head);
		INIT_LIST_HEAD(list);
	}
}

/**
 * @brief list_entry - get the struct for this entry
 *
 * @param ptr:	the &struct list_head pointer.
 * @param type:	the type of the struct this is embedded in.
 * @param member:	the name of the list_head within the struct.
 */
#define list_entry(ptr, type, member) \
	container_of(ptr, type, member)

/**
 * @brief list_first_entry - get the first element from a list
 *
 * @param ptr:	the list head to take the element from.
 * @param type:	the type of the struct this is embedded in.
 * @param member:	the name of the list_head within the struct.
 *
 * Note, that list is expected to be not empty.
 */
#define list_first_entry(ptr, type, member) \
	list_entry((ptr)->next, type, member)

/**
 * @brief list_last_entry - get the last element from a list
 *
 * @param ptr:	the list head to take the element from.
 * @param type:	the type of the struct this is embedded in.
 * @param member:	the name of the list_head within the struct.
 *
 * Note, that list is expected to be not empty.
 */
#define list_last_entry(ptr, type, member) \
	list_entry((ptr)->prev, type, member)

/**
 * @brief list_first_entry_or_null - get the first element from a list
 *
 * @param ptr:	the list head to take the element from.
 * @param type:	the type of the struct this is embedded in.
 * @param member:	the name of the list_head within the struct.
 *
 * Note that if the list is empty, it returns NULL.
 */
#define list_first_entry_or_null(ptr, type, member) ({ \
	struct list_head *head__ = (ptr); \
	struct list_head *pos__ = READ_ONCE(head__->next); \
	pos__ != head__ ? list_entry(pos__, type, member) : NULL; \
})

/**
 * @brief list_next_entry - get the next element in list
 *
 * @param pos:	the type * to cursor
 * @param member:	the name of the list_head within the struct.
 */
#define list_next_entry(pos, member) \
	list_entry((pos)->member.next, typeof(*(pos)), member)

/**
 * @brief list_next_entry_circular - get the next element in list
 *
 * @param pos:	the type * to cursor.
 * @param head:	the list head to take the element from.
 * @param member:	the name of the list_head within the struct.
 *
 * Wraparound if pos is the last element (return the first element).
 * Note, that list is expected to be not empty.
 */
#define list_next_entry_circular(pos, head, member) \
	(list_is_last(&(pos)->member, head) ? \
	list_first_entry(head, typeof(*(pos)), member) : list_next_entry(pos, member))

/**
 * @brief list_prev_entry - get the prev element in list
 *
 * @param pos:	the type * to cursor
 * @param member:	the name of the list_head within the struct.
 */
#define list_prev_entry(pos, member) \
	list_entry((pos)->member.prev, typeof(*(pos)), member)

/**
 * @brief list_prev_entry_circular - get the prev element in list
 *
 * @param pos:	the type * to cursor.
 * @param head:	the list head to take the element from.
 * @param member:	the name of the list_head within the struct.
 *
 * Wraparound if pos is the first element (return the last element).
 * Note, that list is expected to be not empty.
 */
#define list_prev_entry_circular(pos, head, member) \
	(list_is_first(&(pos)->member, head) ? \
	list_last_entry(head, typeof(*(pos)), member) : list_prev_entry(pos, member))

/**
 * @brief list_for_each	-	iterate over a list
 *
 * @param pos:	the &struct list_head to use as a loop cursor.
 * @param head:	the head for your list.
 */
#define list_for_each(pos, head) \
	for (pos = (head)->next; !list_is_head(pos, (head)); pos = pos->next)

/**
 * @brief list_for_each_rcu - Iterate over a list in an RCU-safe fashion
 *
 * @param pos:	the &struct list_head to use as a loop cursor.
 * @param head:	the head for your list.
 */
#define list_for_each_rcu(pos, head)		  \
	for (pos = rcu_dereference((head)->next); \
	     !list_is_head(pos, (head)); \
	     pos = rcu_dereference(pos->next))

/**
 * @brief list_for_each_continue - continue iteration over a list
 *
 * @param pos:	the &struct list_head to use as a loop cursor.
 * @param head:	the head for your list.
 *
 * Continue to iterate over a list, continuing after the current position.
 */
#define list_for_each_continue(pos, head) \
	for (pos = pos->next; !list_is_head(pos, (head)); pos = pos->next)

/**
 * @brief list_for_each_prev	-	iterate over a list backwards
 *
 * @param pos:	the &struct list_head to use as a loop cursor.
 * @param head:	the head for your list.
 */
#define list_for_each_prev(pos, head) \
	for (pos = (head)->prev; !list_is_head(pos, (head)); pos = pos->prev)

/**
 * @brief list_for_each_safe - iterate over a list safe against removal of list entry
 *
 * @param pos:	the &struct list_head to use as a loop cursor.
 * @param n:		another &struct list_head to use as temporary storage
 * @param head:	the head for your list.
 */
#define list_for_each_safe(pos, n, head) \
	for (pos = (head)->next, n = pos->next; \
	     !list_is_head(pos, (head)); \
	     pos = n, n = pos->next)

/**
 * @brief list_for_each_prev_safe - iterate over a list backwards safe against removal of list entry
 *
 * @param pos:	the &struct list_head to use as a loop cursor.
 * @param n:		another &struct list_head to use as temporary storage
 * @param head:	the head for your list.
 */
#define list_for_each_prev_safe(pos, n, head) \
	for (pos = (head)->prev, n = pos->prev; \
	     !list_is_head(pos, (head)); \
	     pos = n, n = pos->prev)

/**
 * @brief list_count_nodes - count nodes in the list
 *
 * @param head:	the head for your list.
 */
static inline size_t list_count_nodes(struct list_head *head)
{
	struct list_head *pos;
	size_t count = 0;

	list_for_each(pos, head)
		count++;

	return count;
}

/**
 * @brief list_entry_is_head - test if the entry points to the head of the list
 *
 * @param pos:	the type * to cursor
 * @param head:	the head for your list.
 * @param member:	the name of the list_head within the struct.
 */
#define list_entry_is_head(pos, head, member)				\
	list_is_head(&pos->member, (head))

/**
 * @brief list_for_each_entry	-	iterate over list of given type
 *
 * @param pos:	the type * to use as a loop cursor.
 * @param head:	the head for your list.
 * @param member:	the name of the list_head within the struct.
 */
#define list_for_each_entry(pos, head, member)				\
	for (pos = list_first_entry(head, typeof(*pos), member);	\
	     !list_entry_is_head(pos, head, member);			\
	     pos = list_next_entry(pos, member))

/**
 * @brief list_for_each_entry_reverse - iterate backwards over list of given type.
 *
 * @param pos:	the type * to use as a loop cursor.
 * @param head:	the head for your list.
 * @param member:	the name of the list_head within the struct.
 */
#define list_for_each_entry_reverse(pos, head, member)			\
	for (pos = list_last_entry(head, typeof(*pos), member);		\
	     !list_entry_is_head(pos, head, member); 			\
	     pos = list_prev_entry(pos, member))

/**
 * list_prepare_entry - prepare a pos entry for use in list_for_each_entry_continue()
 * @param pos:	the type * to use as a start point
 * @param head:	the head of the list
 * @param member:	the name of the list_head within the struct.
 *
 * Prepares a pos entry for use as a start point in list_for_each_entry_continue().
 */
#define list_prepare_entry(pos, head, member) \
	((pos) ? : list_entry(head, typeof(*pos), member))

/**
 * @brief list_for_each_entry_continue - continue iteration over list of given type
 *
 * @param pos:	the type * to use as a loop cursor.
 * @param head:	the head for your list.
 * @param member:	the name of the list_head within the struct.
 *
 * Continue to iterate over list of given type, continuing after
 * the current position.
 */
#define list_for_each_entry_continue(pos, head, member) 		\
	for (pos = list_next_entry(pos, member);			\
	     !list_entry_is_head(pos, head, member);			\
	     pos = list_next_entry(pos, member))

/**
 * @brief list_for_each_entry_continue_reverse - iterate backwards from the given point
 *
 * @param pos:	the type * to use as a loop cursor.
 * @param head:	the head for your list.
 * @param member:	the name of the list_head within the struct.
 *
 * Start to iterate over list of given type backwards, continuing after
 * the current position.
 */
#define list_for_each_entry_continue_reverse(pos, head, member)		\
	for (pos = list_prev_entry(pos, member);			\
	     !list_entry_is_head(pos, head, member);			\
	     pos = list_prev_entry(pos, member))

/**
 * @brief list_for_each_entry_from - iterate over list of given type from the current point
 *
 * @param pos:	the type * to use as a loop cursor.
 * @param head:	the head for your list.
 * @param member:	the name of the list_head within the struct.
 *
 * Iterate over list of given type, continuing from current position.
 */
#define list_for_each_entry_from(pos, head, member) 			\
	for (; !list_entry_is_head(pos, head, member);			\
	     pos = list_next_entry(pos, member))

/**
 * @brief list_for_each_entry_from_reverse - iterate backwards over list of given type
 *                                    from the current point
 *
 * @param pos:	the type * to use as a loop cursor.
 * @param head:	the head for your list.
 * @param member:	the name of the list_head within the struct.
 *
 * Iterate backwards over list of given type, continuing from current position.
 */
#define list_for_each_entry_from_reverse(pos, head, member)		\
	for (; !list_entry_is_head(pos, head, member);			\
	     pos = list_prev_entry(pos, member))

/**
 * @brief list_for_each_entry_safe - iterate over list of given type safe against removal of list entry
 *
 * @param pos:	the type * to use as a loop cursor.
 * @param n:		another type * to use as temporary storage
 * @param head:	the head for your list.
 * @param member:	the name of the list_head within the struct.
 */
#define list_for_each_entry_safe(pos, n, head, member)			\
	for (pos = list_first_entry(head, typeof(*pos), member),	\
		n = list_next_entry(pos, member);			\
	     !list_entry_is_head(pos, head, member); 			\
	     pos = n, n = list_next_entry(n, member))

/**
 * @brief list_for_each_entry_safe_continue - continue list iteration safe against removal
 *
 * @param pos:	the type * to use as a loop cursor.
 * @param n:		another type * to use as temporary storage
 * @param head:	the head for your list.
 * @param member:	the name of the list_head within the struct.
 *
 * Iterate over list of given type, continuing after current point,
 * safe against removal of list entry.
 */
#define list_for_each_entry_safe_continue(pos, n, head, member) 		\
	for (pos = list_next_entry(pos, member), 				\
		n = list_next_entry(pos, member);				\
	     !list_entry_is_head(pos, head, member);				\
	     pos = n, n = list_next_entry(n, member))

/**
 * @brief list_for_each_entry_safe_from - iterate over list from current point safe against removal
 *
 * @param pos:	the type * to use as a loop cursor.
 * @param n:		another type * to use as temporary storage
 * @param head:	the head for your list.
 * @param member:	the name of the list_head within the struct.
 *
 * Iterate over list of given type from current point, safe against
 * removal of list entry.
 */
#define list_for_each_entry_safe_from(pos, n, head, member) 			\
	for (n = list_next_entry(pos, member);					\
	     !list_entry_is_head(pos, head, member);				\
	     pos = n, n = list_next_entry(n, member))

/**
 * @brief list_for_each_entry_safe_reverse - iterate backwards over list safe against removal
 *
 * @param pos:	the type * to use as a loop cursor.
 * @param n:		another type * to use as temporary storage
 * @param head:	the head for your list.
 * @param member:	the name of the list_head within the struct.
 *
 * Iterate backwards over list of given type, safe against removal
 * of list entry.
 */
#define list_for_each_entry_safe_reverse(pos, n, head, member)		\
	for (pos = list_last_entry(head, typeof(*pos), member),		\
		n = list_prev_entry(pos, member);			\
	     !list_entry_is_head(pos, head, member); 			\
	     pos = n, n = list_prev_entry(n, member))

/**
 * @brief list_safe_reset_next - reset a stale list_for_each_entry_safe loop
 *
 * @param pos:	the loop cursor used in the list_for_each_entry_safe loop
 * @param n:		temporary storage used in list_for_each_entry_safe
 * @param member:	the name of the list_head within the struct.
 *
 * list_safe_reset_next is not safe to use in general if the list may be
 * modified concurrently (eg. the lock is dropped in the loop body). An
 * exception to this is if the cursor element (pos) is pinned in the list,
 * and list_safe_reset_next is called after re-taking the lock and before
 * completing the current iteration of the loop body.
 */
#define list_safe_reset_next(pos, n, member)				\
	n = list_next_entry(pos, member)
