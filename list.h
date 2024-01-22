/*****************************************************************************
 * list.h
 *****************************************************************************
 * Copyright (C) 2022 EasyTools
 *
 * Authors: Arnaud de Turckheim <adeturckheim@easytools.tv>
 *
 * This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://sam.zoy.org/wtfpl/COPYING for more details.
 *****************************************************************************/

#ifndef _LIST_H_
#define _LIST_H_

/*
 * link for double linked list
 */
typedef struct link_t
{
    /* pointer to the previous link in the list */
    struct link_t *prev;
    /* pointer to the next link in the list */
    struct link_t *next;
} link_t;

/*
 * initialize a link
 */
#define LINK_INIT(Link) { .prev = &(Link), .next = &(Link) }

static inline void link_init(link_t *link)
{
    if (link)
        link->next = link->prev = link;
}

/*
 * check if the link is linked to another
 */
static inline bool link_is_linked(const link_t *link)
{
    return link && link->next != link;
}

/*
 * remove the link from the list
 */
static inline link_t *link_del(link_t *link)
{
    if (link) {
        link->prev->next = link->next;
        link->next->prev = link->prev;
        link_init(link);
    }
    return link;
}

/*
 * add link l2 after link l1 in the list
 */
static inline void link_add_after(link_t *l1, link_t *l2)
{
    if (l1 && l2) {
        link_del(l2);
        l2->prev = l1;
        l2->next = l1->next;
        l2->next->prev = l2;
        l1->next = l2;
    }
}

/*
 * add link l2 before link l1 in the list
 */
static inline void link_add_before(link_t *l1, link_t *l2)
{
    if (l1 && l2) {
        link_del(l2);
        l2->prev = l1->prev;
        l2->next = l1;
        l2->prev->next = l2;
        l1->prev = l2;
    }
}

/*
 * list
 */
typedef struct list_t
{
    /* link on the first element of the list */
    struct link_t link;
} list_t;

/*
 * initialize the list
 */
#define LIST_INIT(List) { .link = LINK_INIT(List.link) }

static inline void list_init(list_t *list)
{
    link_init(list ? &list->link : NULL);
}

/*
 * check if the list is empty
 */
static inline bool list_is_empty(const list_t *list)
{
    return !list || !link_is_linked(&list->link);
}

/*
 * check if e if the first element in the list
 */
static inline bool list_is_first(const list_t *list, link_t *e)
{
    return list && e && list->link.next == e;
}

/*
 * check if e if the last element in the list
 */
static inline bool list_is_last(const list_t *list, link_t *e)
{
    return list && e && list->link.prev == e;
}

/*
 * add element e as the last element of the list
 */
static inline void list_add(list_t *list, link_t *e)
{
    if (list && e)
        link_add_before(&list->link, e);
}

/*
 * add element e as the first element of the list
 */
static inline void list_unshift(list_t *list, link_t *e)
{
    if (list && e)
        link_add_after(&list->link, e);
}

/*
 * return the first element of the list
 */
static inline link_t *list_first(const list_t *list)
{
    return list && !list_is_empty(list) ? (link_t *)list->link.next : NULL;
}

/*
 * return the last element of the list
 */
static inline link_t *list_last(const list_t *list)
{
    return list && !list_is_empty(list) ? (link_t *)list->link.prev : NULL;
}

/*
 * remove and return the last element of the list
 */
static inline link_t *list_pop(list_t *list)
{
    return link_del(list_last(list));
}

/*
 * remove and return the first element of the list
 */
static inline link_t *list_shift(list_t *list)
{
    return link_del(list_first(list));
}

/*
 * remove e from its list (if any) and add it as the last element of dst
 */
static inline void link_move(link_t *e, list_t *dst)
{
    if (e && dst)
    {
        link_del(e);
        list_add(dst, e);
    }
}

/*
 * return the element after prev in the list or the first element if prev is
 * NULL
 */
static inline link_t *list_next(const list_t *list, const link_t *prev)
{
    if (prev == NULL)
        return list_first(list);
    return prev->next == &list->link ? NULL : (link_t *)prev->next;
}

/*
 * macros to iterate over a list
 */
#define LIST_EACH(Next, List, I)                                    \
    for (typeof (Next(List, NULL)) I = Next(List, NULL); I; I = Next(List, I))

#define list_each(...)  LIST_EACH(list_next, __VA_ARGS__)

/*
 * macros to pop all elements of a list
 */
#define LIST_FLUSH(Remove, List, I)                                 \
    for (typeof (Remove(List)) I = Remove(List); I; I = Remove(List))

#define list_flush(...) LIST_FLUSH(list_pop, __VA_ARGS__)

/*
 * macro to pop and call Free on each element of a list
 */
#define LIST_CLEAN(Remove, Free, List)                               \
    LIST_FLUSH(Remove, List, I) Free(I)

/*
 * return the element at a position
 */
static inline link_t *list_at(const list_t *list, unsigned at)
{
    list_each(list, e)
        if (!at--)
            return e;
    return NULL;
}

/*
 * move all elements of list from to list to
 */
static inline void list_move(list_t *from, list_t *to)
{
    if (!to)
        return;

    list_init(to);
    if (!list_is_empty(from))
    {
        to->link.next = from->link.next;
        to->link.prev = from->link.prev;
        from->link.next->prev = &to->link;
        from->link.prev->next = &to->link;
        list_init(from);
    }
}

/*
 * simple sort of a list
 */
typedef int (*list_sort_data_func)(void *data, const link_t *, const link_t *);

static inline void list_sort_data(list_t *list,
                                  list_sort_data_func sort, void *priv)
{
    if (!list_is_empty(list) && sort)
    {
        list_t tmp;
        list_move(list, &tmp);
        list_flush(&tmp, e)
        {
            link_t *i = list->link.next;
            while (i != &list->link && sort(priv, e, i) > 0)
                i = i->next;
            link_add_before(i, e);
        }
    }
}

typedef int (*list_sort_func)(const link_t *, const link_t *);

static inline int list_sort_wrap(void *priv, const link_t *t1, const link_t *t2)
{
    list_sort_func sort = priv;
    return sort(t1, t2);
}

static inline void list_sort(list_t *list, list_sort_func sort)
{
    if (sort)
        list_sort_data(list, list_sort_wrap, sort);
}

/*
 * Define link helpers.
 */

#define LINK_OF(Name, Link)                                                 \
static inline struct Name *Name##_from_##Link(const link_t *link)           \
{                                                                           \
    return link ? container_of(link, struct Name, Link) : NULL;             \
}                                                                           \
                                                                            \
static inline link_t *Name##_to_##Link(const struct Name *c)                \
{                                                                           \
    return c ? (link_t *)&c->Link : NULL;                                   \
}                                                                           \
                                                                            \
static inline void Name##_init_##Link(struct Name *c)                       \
{                                                                           \
    link_init(Name##_to_##Link(c));                                         \
}                                                                           \
                                                                            \
static inline struct Name *Name##_del_##Link(struct Name *c)                \
{                                                                           \
    return Name##_from_##Link(link_del(Name##_to_##Link(c)));               \
}                                                                           \
                                                                            \
static inline void Name##_add_##Link##_before(struct Name *c1,              \
                                              struct Name *c2)              \
{                                                                           \
    link_add_before(Name##_to_##Link(c1), Name##_to_##Link(c2));            \
}                                                                           \
                                                                            \
static inline void Name##_add_##Link##_after(struct Name *c1,               \
                                             struct Name *c2)               \
{                                                                           \
    link_add_after(Name##_to_##Link(c1), Name##_to_##Link(c2));             \
}                                                                           \
                                                                            \
static inline bool Name##_is_##Link##_linked(struct Name *c)                \
{                                                                           \
    return link_is_linked(Name##_to_##Link(c));                             \
}

#define LIST_OF(Type, List, Name, Link)                                     \
                                                                            \
static inline list_t *Type##_to_##List(const struct Type *t)                \
{                                                                           \
    const list_t *list = t ? &t->List : NULL;                               \
    return (list_t *)list;                                                  \
}                                                                           \
                                                                            \
static inline void Type##_init(struct Type *t)                              \
{                                                                           \
    list_init(Type##_to_##List(t));                                         \
}                                                                           \
                                                                            \
static inline bool Type##_is_empty(const struct Type *t)                    \
{                                                                           \
    return list_is_empty(Type##_to_##List(t));                              \
}                                                                           \
                                                                            \
static inline void Type##_move(struct Type *from, struct Type *to)          \
{                                                                           \
    list_move(Type##_to_##List(from), Type##_to_##List(to));                \
}                                                                           \
                                                                            \
static inline struct Name *Type##_first(const struct Type *list)            \
{                                                                           \
    return Name##_from_##Link(list_first(Type##_to_##List(list)));          \
}                                                                           \
                                                                            \
static inline void Type##_add(struct Type *list, struct Name *c)            \
{                                                                           \
    list_add(Type##_to_##List(list), Name##_to_##Link(c));                  \
}                                                                           \
                                                                            \
static inline struct Name *Type##_pop(struct Type *list)                    \
{                                                                           \
    return Name##_from_##Link(list_pop(Type##_to_##List(list)));            \
}                                                                           \
                                                                            \
static inline struct Name *                                                 \
Type##_next(const struct Type *list, const struct Name *c)                  \
{                                                                           \
    return Name##_from_##Link(list_next(Type##_to_##List(list),             \
                                        Name##_to_##Link(c)));              \
}                                                                           \
                                                                            \
static inline struct Name *                                                 \
Type##_at(const struct Type *list, unsigned int at)                         \
{                                                                           \
    return Name##_from_##Link(list_at(Type##_to_##List(list), at));         \
}                                                                           \
                                                                            \
static inline int                                                           \
Type##_sort_wrap(void *priv, const link_t *l1,  const link_t *l2)           \
{                                                                           \
    int (*sort)(const struct Name *, const struct Name *) = priv;           \
    return sort(Name##_from_##Link(l1), Name##_from_##Link(l2));            \
}                                                                           \
                                                                            \
static inline void                                                          \
Type##_sort(struct Type *list, int (*sort)(const struct Name *,             \
                                           const struct Name *))            \
{                                                                           \
    return list_sort_data(Type##_to_##List(list), Type##_sort_wrap, sort);  \
}

#endif
