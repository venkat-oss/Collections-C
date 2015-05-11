/*
 * Collections-C
 * Copyright (C) 2013-2015 Srđan Panić <srdja.panic@gmail.com>
 *
 * This file is part of Collections-C.
 *
 * Collections-C is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Collections-C is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Collections-C.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "hashtable.h"

#define DEFAULT_CAPACITY 16
#define DEFAULT_LOAD_FACTOR 0.75f

struct hashtable_s {
    size_t       capacity;
    size_t       size;
    size_t       threshold;
    uint32_t     hash_seed;
    int          key_len;
    float        load_factor;
    TableEntry **buckets;

    size_t  (*hash)       (const void *key, int l, uint32_t seed);
    bool    (*key_cmp)    (void *k1, void *k2);
    void   *(*mem_alloc)  (size_t size);
    void   *(*mem_calloc) (size_t blocks, size_t size);
    void    (*mem_free)   (void *block);
};

static size_t  get_table_index  (HashTable *table, void *key);
static bool    resize           (HashTable *t, size_t new_capacity);
static size_t  round_pow_two    (size_t n);
static void   *get_null_key     (HashTable *table);
static bool    add_null_key     (HashTable *table, void *val);
static void   *remove_null_key  (HashTable *table);
static void    move_entries     (TableEntry **src_bucket, TableEntry **dest_bucket,
                                 size_t src_size, size_t dest_size);

/**
 * Allocates a new HashTable object using the standard allocators. The newly
 * created HashTable will work with string keys. NULL may be returned if the
 * underlying memory allocators fail.
 *
 * @return a new HashTable or NULL if the memory allocation fails.
 */
HashTable *hashtable_new()
{
    HashTableConf htc;
    hashtable_conf_init(&htc);
    return hashtable_new_conf(&htc);
}

/**
 * Allocates a new configured HashTable based on the provided HashTableConf object.
 * The table is allocated using the memory allocators specified in the HashTableConf
 * object. In case the allocation of the table structure fails, NULL is returned.
 * The HashTableConf object is not modified by this function and can therefore
 * be used for other tables.
 *
 * @param conf the HashTableConf object used to configure this new HashTable
 *
 * @return a new HashTable or NULL if the memory allocation fails.
 */
HashTable *hashtable_new_conf(HashTableConf *conf)
{
    HashTable *table   = conf->mem_calloc(1, sizeof(HashTable));

    if (table == NULL)
        return NULL;

    table->hash        = conf->hash;
    table->key_cmp     = conf->key_compare;
    table->load_factor = conf->load_factor;
    table->capacity    = round_pow_two(conf->initial_capacity);
    table->hash_seed   = conf->hash_seed;
    table->key_len     = conf->key_length;
    table->size        = 0;
    table->mem_alloc   = conf->mem_alloc;
    table->mem_calloc  = conf->mem_calloc;
    table->mem_free    = conf->mem_free;
    table->buckets     = conf->mem_calloc(table->capacity, sizeof(TableEntry));
    table->threshold   = table->capacity * table->load_factor;

    return table;
}

/**
 * Initializes the HashTableConf structs fields to default values.
 *
 * @param[in] conf the struct that is being initialized
 */
void hashtable_conf_init(HashTableConf *conf)
{
    conf->hash             = STRING_HASH;
    conf->key_compare      = CMP_STRING;
    conf->initial_capacity = DEFAULT_CAPACITY;
    conf->load_factor      = DEFAULT_LOAD_FACTOR;
    conf->key_length       = KEY_LENGTH_VARIABLE;
    conf->hash_seed        = 0;
    conf->mem_alloc        = malloc;
    conf->mem_calloc       = calloc;
    conf->mem_free         = free;
}

/**
 * Destroys the specified HashTable structure without destroying the the data
 * contained within it. In other words the keys and the values are not freed,
 * but only the table structure.
 *
 * @param[in] table HashTable to be destroyed.
 */
void hashtable_destroy(HashTable *table)
{
    int i;
    for (i = 0; i < table->capacity; i++) {
        TableEntry *next = table->buckets[i];

        while (next) {
            TableEntry *tmp = next->next;
            table->mem_free(next);
            next = tmp;
        }
    }
    table->mem_free(table->buckets);
    table->mem_free(table);
}

/**
 * Creates a new key-value mapping in the specified HashTable. If the unique key
 * is already mapped to a value in this table, that value is replaced with the
 * new value. This operation may fail if the space allocation for the new entry
 * fails.
 *
 * @param[in] table the table to which this new key-value mapping is being added
 * @param[in] key a hash table key used to access the specified value
 * @param[in] val a value that is being stored in the table
 *
 * @return true if the operation was successful
 */
bool hashtable_add(HashTable *table, void *key, void *val)
{
    if (table->size >= table->threshold)
        resize(table, table->capacity << 1);

    if (!key)
        return add_null_key(table, val);

    const size_t hash = table->hash(key, table->key_len, table->hash_seed);
    const size_t i    = hash & (table->capacity - 1);

    TableEntry *replace = table->buckets[i];

    while (replace) {
        if (table->key_cmp(replace->key, key)) {
            replace->value = val;
            return true;
        }
        replace = replace->next;
    }

    TableEntry *new_entry = table->mem_alloc(sizeof(TableEntry));

    if (!new_entry)
        return false;

    new_entry->key   = key;
    new_entry->value = val;
    new_entry->hash  = hash;
    new_entry->next  = table->buckets[i];

    table->buckets[i] = new_entry;
    table->size++;

    return true;
}

/**
 * Creates a new key-value mapping for the NULL key. This operation may fail if
 * the space allocation for the new entry fails.
 *
 * @param[in] table the table into which this key value-mapping is being add in
 * @param[in] val the value that is being mapped to the NULL key
 *
 * @return true if the operation was successful
 */
static bool add_null_key(HashTable *table, void *val)
{
    TableEntry *replace = table->buckets[0];

    while (replace) {
        if (!replace->key) {
            replace->value = val;
            return true;
        }
        replace = replace->next;
    }

    TableEntry *new_entry = table->mem_alloc(sizeof(TableEntry));

    if (!new_entry)
        return false;

    new_entry->key   = NULL;
    new_entry->value = val;
    new_entry->hash  = 0;
    new_entry->next  = table->buckets[0];

    table->buckets[0] = new_entry;
    table->size++;

    return true;
}

/**
 * Returns a value associated with the specified key. If there is no value
 * associated with this key, NULL is returned. In the case where the provided
 * key explicitly maps to a NULL value, calling <code>hashtable_contains_key()
 * </code> before this function can resolve the ambiguity.
 *
 * @param[in] table the table from which the mapping is being returned
 * @param[in] key   the key that is being looked up
 *
 * @return the value mapped to the specified key, or null if the mapping doesn't
 *         exit
 */
void *hashtable_get(HashTable *table, void *key)
{
    if (!key)
        return get_null_key(table);

    size_t      index  = get_table_index(table, key);
    TableEntry *bucket = table->buckets[index];

    while (bucket) {
        if (table->key_cmp(bucket->key, key))
            return bucket->value;
        bucket = bucket->next;
    }
    return NULL;
}

/**
 * Returns a value associated with the NULL key. If there is not value mapped to
 * the NULL key NULL is returned. NULL may also be returned if the NULL key
 * mapps to a NULL value.
 *
 * @param[in] table the table from which the value mapped to this key is being
 *                  returned
 *
 * @return the value mapped to the NULL key, or NULL
 */
static void *get_null_key(HashTable *table)
{
    TableEntry *bucket = table->buckets[0];

    while (bucket) {
        if (bucket->key == NULL)
            return bucket->value;
        bucket = bucket->next;
    }
    return NULL;
}

/**
 * Removes a key-value mapping from the specified hash table and returns the
 * value that was mapped to the specified key. In case the key doesn't exist
 * NULL is returned. NULL might also be returned if the key maps to a null value.
 * Calling <code>hashtable_contains_key()</code> before this functin can resolve
 * the ambiguity.
 *
 * @param[in] table the table from which the key-value pair is being removed
 * @param[in] key the key of the value being returned
 *
 * @return the value associated with the removed key, or NULL if the key doesn't
 *         exist
 */
void *hashtable_remove(HashTable *table, void *key)
{
    if (!key)
        return remove_null_key(table);

    const size_t i = get_table_index(table, key);

    TableEntry *e    = table->buckets[i];
    TableEntry *prev = NULL;
    TableEntry *next = NULL;

    while (e) {
        next = e->next;

        if (table->key_cmp(key, e->key)) {
            void *value = e->value;

            if (!prev)
                table->buckets[i] = next;
            else
                prev->next = next;

            table->mem_free(e);
            table->size--;
            return value;
        }
        prev = e;
        e = next;
    }
    return NULL;
}

/**
 * Removes a NULL key mapping from the specified hash table and returns the
 * value that was mapped to the NULL key. In case the NULL key doesn't exist
 * NULL is returned. NULL might also be returned if a NULL key is mapped to a
 * NULL value.
 *
 * @param[in] table the table from which the NULL key mapping is being removed
 *
 * @return the value associated with the NULL key, or NULL if the NULL key was
 * not mapped
 */
void *remove_null_key(HashTable *table)
{
    TableEntry *e = table->buckets[0];

    TableEntry *prev = NULL;
    TableEntry *next = NULL;

    while (e) {
        next = e->next;

        if (e->key == NULL) {
            void *value = e->value;

            if (!prev)
                table->buckets[0] = next;
            else
                prev->next = next;

            table->mem_free(e);
            table->size--;
            return value;
        }
        prev = e;
        e = next;
    }
    return NULL;
}

/**
 * Removes all key-value mappings from the specified table.
 *
 * @param[in] table the table from which all mappings are being removed
 */
void hashtable_remove_all(HashTable *table)
{
    int i;
    for (i = 0; i < table->capacity; i++) {
        TableEntry *entry = table->buckets[i];
        while (entry) {
            TableEntry *next = entry->next;
            table->mem_free(entry);
            table->size--;
            entry = next;
        }
        table->buckets[i] = NULL;
    }
}

/**
 * Resizes the table to match the provided capacity. The new capacity must be a
 * power of two. This function returns true if the resize was successfull or false
 * if not.
 *
 * @param[in] table the table that is being resized.
 * @param[in] new_capacity the new capacity to which the table should be resized
 *
 * @return true if the resize was successfull
 */
static bool resize(HashTable *t, size_t new_capacity)
{
    if (t->capacity == MAX_POW_TWO)
        return false;

    TableEntry **new_buckets = t->mem_calloc(new_capacity, sizeof(TableEntry));
    TableEntry **old_buckets = t->buckets;

    if (!new_buckets)
        return false;

    move_entries(old_buckets, new_buckets, t->capacity, new_capacity);

    t->buckets   = new_buckets;
    t->capacity  = new_capacity;
    t->threshold = t->load_factor * new_capacity;

    t->mem_free(old_buckets);

    return true;
}

/**
 * Rounds the integer to the nearest upper power of two.
 *
 * @param[in] the unsigned integer that is being rounded
 *
 * @return the nearest upper power of two
 */
static INLINE size_t round_pow_two(size_t n)
{
    if (n >= MAX_POW_TWO)
        return MAX_POW_TWO;

    if (n == 0)
        return 2;
    /**
     * taken from:
     * http://graphics.stanford.edu/~seander/
     * bithacks.html#RoundUpPowerOf2Float
     */
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n++;

    return n;
}

/**
 * Moves all entries from one bucket array to another.
 *
 * @param[in] src_bucket  the source bucket from which the entries are moved
 * @param[in] dest_bucket the destination bucket to which the entries are being
 *                        moved
 * @param[in] src_size    size of the source bucket
 * @param[in] dest_size   size of the destination bucket
 */
static INLINE void
move_entries(TableEntry **src_bucket, TableEntry **dest_bucket,
             size_t       src_size,   size_t       dest_size)
{
    int i;
    for (i = 0; i < src_size; i++) {
        TableEntry *entry = src_bucket[i];

        while (entry) {
            TableEntry *next  = entry->next;
            size_t      index = entry->hash & (dest_size - 1);

            entry->next = dest_bucket[index];
            dest_bucket[index] = entry;

            entry = next;
        }
    }
}

/**
 * Returns the size of the specified HashTable. Size of a HashTable represents
 * the number of key-value mappings within the table.
 *
 * @param[in] table the table whose size is being returned
 * @return the size of the table
 */
size_t hashtable_size(HashTable *table)
{
    return table->size;
}

/**
 * Returns the current capacity of the table. The capacity is is the number of
 * buckets or the number of random access for table entries.
 *
 * @param[in] table the table whos current capacity is being returned
 *
 * @return the current capacity of the specified table
 */
size_t hashtable_capacity(HashTable *table)
{
    return table->capacity;
}

/**
 * Checks whether or not the HashTable contains the specified key.
 *
 * @param[in] table the table on which the search is being performed
 * @param[in] key the key that is being searched for
 * @return true if the table contains the key.
 */
bool hashtable_contains_key(HashTable *table, void *key)
{
    TableEntry *entry = table->buckets[get_table_index(table, key)];

    while (entry) {
        if (table->key_cmp(key, entry->key))
            return true;

        entry = entry->next;
    }
    return false;
}

/**
 * Returns a Array of hashtable values. The returned Array is allocated
 * using the same memory allocators used by the HashTable. NULL may be
 * returned if the memory allocation of the Array structure fails.
 *
 * @param[in] table the table whose values are being returned
 *
 * @return a Array of values or NULL
 */
Array *hashtable_get_values(HashTable *table)
{
    ArrayConf vc;
    array_conf_init(&vc);

    vc.capacity   = table->size;
    vc.mem_alloc  = table->mem_alloc;
    vc.mem_calloc = table->mem_calloc;
    vc.mem_free   = table->mem_free;

    Array *v = array_new_conf(&vc);

    if (!v)
        return NULL;

    int i;
    for (i = 0; i <table->capacity; i++) {
        if (!table->buckets[i])
            continue;

        TableEntry *entry = table->buckets[i];

        while (entry) {
            array_add(v, entry->value);
            entry = entry->next;
        }
    }
    return v;
}

/**
 * Returns a Array of hashtable keys. The returned Array is allocated
 * using the same memory allocators used by the HashTable. NULL may be
 * returned if the memory allocation of the Array structure fails.
 *
 * @param[in] table the table whos keys are being returned
 *
 * @return a Array of keys or NULL
 */
Array *hashtable_get_keys(HashTable *table)
{
    ArrayConf vc;
    array_conf_init(&vc);

    vc.capacity   = table->size;
    vc.mem_alloc  = table->mem_alloc;
    vc.mem_calloc = table->mem_calloc;
    vc.mem_free   = table->mem_free;

    Array *keys = array_new_conf(&vc);

    if (!keys)
        return NULL;

    int i;
    for (i = 0; i < table->capacity; i++) {
        if (!table->buckets[i])
            continue;

        TableEntry *entry = table->buckets[i];

        while (entry) {
            array_add(keys, entry->key);
            entry = entry->next;
        }
    }
    return keys;
}

/**
 * Returns the bucket index that maps to the specified key.
 */
static INLINE size_t get_table_index(HashTable *table, void *key)
{
    size_t hash = table->hash(key, table->key_len, table->hash_seed);
    return hash & (table->capacity - 1);
}

/**
 * String key comparator function.
 *
 * @param[in] key1 first key
 * @param[in] key2 second key
 *
 * @return true if the keys are identical and false if otherwise
 */
bool hashtable_string_key_cmp(void *key1, void *key2)
{
    return strcmp((char*)key1, (char*)key2) == 0;
}

/**
 * Double key comparator function.
 *
 * @param[in] key1 first key
 * @param[in] key2 second key
 *
 * @return true if the keys are identical
 */
bool hashtable_double_key_cmp(void *key1, void *key2)
{
    return *(double*) key1 == *(double*) key2;
}

/**
 * Double key comparator function.
 *
 * @param[in] key1 first key
 * @param[in] key2 second key
 *
 * @return true if the keys are identical
 */
bool hashtable_float_key_cmp(void *key1, void *key2)
{
    return *(float*) key1 == *(float*) key2;
}

/**
 * Char key comparator function.
 *
 * @param[in] key1 first key
 * @param[in] key2 second key
 *
 * @return true if the keys are identical
 */
bool hashtable_char_key_cmp(void *key1, void *key2)
{
    return *(char*) key1 == *(char*) key2;
}

/**
 * Short key comparator function.
 *
 * @param[in] key1 first key
 * @param[in] key2 second key
 *
 * @return true if the keys are identical
 */
bool hashtable_short_key_cmp(void *key1, void *key2)
{
    return *(short*) key1 == *(short*) key2;
}

/**
 * Int key comparator function.
 *
 * @param[in] key1 first key
 * @param[in] key2 second key
 *
 * @return true if the keys are identical
 */
bool hashtable_int_key_cmp(void *key1, void *key2)
{
    return *(int*) key1 == *(int*) key2;
}

/**
 * Int key comparator function.
 *
 * @param[in] key1 first key
 * @param[in] key2 second key
 *
 * @return true if the keys are identical
 */
bool hashtable_long_key_cmp(void *key1, void *key2)
{
    const long *l1 = key1;
    const long *l2 = key2;

    return *l1 == *l2;
}

/**
 * Pointer key comparator function.
 *
 * @param[in] key1 first key
 * @param[in] key2 second key
 *
 * @return true if the keys are identical
 */
bool hashtable_pointer_key_cmp(void *key1, void *key2)
{
    return key1 == key2;
}

/**
 * A 'foreach loop' function that invokes the specified function on every key in
 * the table. The operation function should not modify the key. Any modification
 * of the key will invalidate the HashTable.
 *
 * @param[in] table the table on which this operation is being perfomed
 * @param[in] op the operation function that is invoked on each key of the table
 */
void hashtable_foreach_key(HashTable *table, void (*op) (const void *key))
{
    int i;
    for (i = 0; i <table->capacity; i++) {
        if (!table->buckets[i])
            continue;

        TableEntry *entry = table->buckets[i];

        while (entry) {
            op(entry->key);
            entry = entry->next;
        }
    }
}

/**
 * A 'foreach loop' function that invokes the specified function on every value
 * in the table.
 *
 * @param[in] table the table on which this operation is being performed
 * @param[in] op the operation function that is invoked on each value of the
 *               table
 */
void hashtable_foreach_value(HashTable *table, void (*op) (void *val))
{
    int i;
    for (i = 0; i <table->capacity; i++) {
        if (!table->buckets[i])
            continue;

        TableEntry *entry = table->buckets[i];

        while (entry) {
            op(entry->value);
            entry = entry->next;
        }
    }
}

/**
 * Initializes the HashTableIter structure.
 *
 * @note The order at which the entries are returned is unspecified.
 *
 * @param[in] iter the iterator that is being initialized
 * @param[in] table the table over whose entries the iterator is going to iterate
 */
void hashtable_iter_init(HashTableIter *iter, HashTable *table)
{
    iter->table = table;

    int i;
    for (i = 0; i < table->capacity; i++) {
        TableEntry *e = table->buckets[i];
        if (e) {
            iter->bucket_index = i;
            iter->next_entry   = e;
            iter->prev_entry   = NULL;
            break;
        }
    }
}

/**
 * Checks whether or not the iterator has a next entry iterate over.
 *
 * @return true if the next entry exists or false if the iterator has reached
 * the end of the table.
 */
bool hashtable_iter_has_next(HashTableIter *iter)
{
    return iter->next_entry != NULL ? true : false;
}

/**
 * Advances the iterator and returns a table entry.
 *
 * @param[in] iter the iterator that is being advanced
 */
TableEntry *hashtable_iter_next(HashTableIter *iter)
{
    iter->prev_entry = iter->next_entry;
    iter->next_entry = iter->next_entry->next;

    if (iter->next_entry)
        return iter->prev_entry;

    int i;
    for (i = iter->bucket_index + 1; i < iter->table->capacity; i++) {
        iter->next_entry = iter->table->buckets[i];

        if (iter->next_entry) {
            iter->bucket_index = i;
            return iter->prev_entry;
        }
    }
}

/**
 * Removes the last returned table entry
 *
 * @param[in] The iterator on which this operation is performed
 */
void hashtable_iter_remove(HashTableIter *iter)
{
    hashtable_remove(iter->table, iter->prev_entry->key);
}


/*******************************************************************************
 *
 *
 *  djb2 string hash
 *
 *
 ******************************************************************************/

size_t hashtable_hash_string(const void *key, int len, uint32_t seed)
{
    const    char   *str  = key;
    register size_t  hash = 5381;

    while (*str++)
        hash = ((hash << 5) + hash) ^ *str;

    return hash;
}

/*******************************************************************************
 *
 *
 *  MurmurHash3 by Austin Appleby, adapted for hashtable use.
 *
 *
 ******************************************************************************/

#ifdef _MSC_VER

#define ROTL32(x,y) _rotl(x,y)
#define ROTL64(x,y)	_rotl64(x,y)
#define BIG_CONSTANT(x) (x)

#else

inline uint32_t rotl32(uint32_t x, int8_t r)
{
    return (x << r) | (x >> (32 - r));
}

inline uint64_t rotl64(uint64_t x, int8_t r)
{
    return (x << r) | (x >> (64 - r));
}

#define ROTL32(x,y) rotl32(x,y)
#define ROTL64(x,y)	rotl64(x,y)
#define BIG_CONSTANT(x) (x##LLU)

#endif


/*****************************************************************************
 *
 *                            -- 64bit --
 *
 ****************************************************************************/
#ifdef ARCH_64


FORCE_INLINE uint64_t fmix64(uint64_t k)
{
    k ^= k >> 33;
    k *= BIG_CONSTANT(0xff51afd7ed558ccd);
    k ^= k >> 33;
    k *= BIG_CONSTANT(0xc4ceb9fe1a85ec53);
    k ^= k >> 33;

    return k;
}

uint64_t hashtable_hash(const void *key, int len, uint32_t seed)
{
    const uint8_t  *data    = (const uint8_t*) key;
    const int       nblocks = len / 16;

    uint64_t        h1      = seed;
    uint64_t        h2      = seed;

    const uint64_t  c1      = BIG_CONSTANT(0x87c37b91114253d5);
    const uint64_t  c2      = BIG_CONSTANT(0x4cf5ad432745937f);

    const uint64_t *blocks  = (const uint64_t*)(data);

    int i;
    for(i = 0; i < nblocks; i++) {
        uint64_t k1 = blocks[i*2+0];
        uint64_t k2 = blocks[i*2+1];

        k1 *= c1;
        k1  = ROTL64(k1,31);
        k1 *= c2;
        h1 ^= k1;
        h1  = ROTL64(h1,27);
        h1 += h2;
        h1  = h1 * 5 + 0x52dce729;

        k2 *= c2;
        k2  = ROTL64(k2,33);
        k2 *= c1;
        h2 ^= k2;
        h2  = ROTL64(h2,31);
        h2 += h1;
        h2  = h2 * 5 + 0x38495ab5;
    }

    const uint8_t *tail = (const uint8_t*)(data + nblocks*16);

    uint64_t k1 = 0;
    uint64_t k2 = 0;

    switch(len & 15) {
    case 15: k2 ^= ((uint64_t)tail[14]) << 48;
    case 14: k2 ^= ((uint64_t)tail[13]) << 40;
    case 13: k2 ^= ((uint64_t)tail[12]) << 32;
    case 12: k2 ^= ((uint64_t)tail[11]) << 24;
    case 11: k2 ^= ((uint64_t)tail[10]) << 16;
    case 10: k2 ^= ((uint64_t)tail[ 9]) << 8;
    case  9: k2 ^= ((uint64_t)tail[ 8]) << 0;
             k2 *= c2;
             k2  = ROTL64(k2,33);
             k2 *= c1;
             h2 ^= k2;

    case  8: k1 ^= ((uint64_t)tail[ 7]) << 56;
    case  7: k1 ^= ((uint64_t)tail[ 6]) << 48;
    case  6: k1 ^= ((uint64_t)tail[ 5]) << 40;
    case  5: k1 ^= ((uint64_t)tail[ 4]) << 32;
    case  4: k1 ^= ((uint64_t)tail[ 3]) << 24;
    case  3: k1 ^= ((uint64_t)tail[ 2]) << 16;
    case  2: k1 ^= ((uint64_t)tail[ 1]) << 8;
    case  1: k1 ^= ((uint64_t)tail[ 0]) << 0;
             k1 *= c1;
             k1  = ROTL64(k1,31);
             k1 *= c2;
             h1 ^= k1;
    };

    h1 ^= len; h2 ^= len;

    h1 += h2;
    h2 += h1;

    h1 = fmix64(h1);
    h2 = fmix64(h2);

    h1 += h2;
    h2 += h1;

    return h1;
}

/*
 * MurmurHash3 the 64bit variant that hashes the pointer itself
 */
uint64_t hashtable_hash_ptr(const void *key, int len, uint32_t seed)
{
    const int nblocks = len / 4;

    uint64_t h1 = seed;
    uint64_t h2 = seed;

    const uint64_t c1 = BIG_CONSTANT(0x87c37b91114253d5);
    const uint64_t c2 = BIG_CONSTANT(0x4cf5ad432745937f);

    int i;
    for (i = 0; i < nblocks; i++) {
        uint64_t k1 = ((uintptr_t) key >> (2 * i)) & 0xff;
        uint64_t k2 = ROTL64(k1, 13);

        k1 *= c1;
        k1  = ROTL64(k1,31);
        k1 *= c2;
        h1 ^= k1;
        h1  = ROTL64(h1,27);
        h1 += h2;
        h1  = h1 * 5 + 0x52dce729;

        k2 *= c2;
        k2  = ROTL64(k2,33);
        k2 *= c1;
        h2 ^= k2;
        h2  = ROTL64(h2,31);
        h2 += h1;
        h2  = h2 * 5 + 0x38495ab5;
    }

    /* Since the pointers are power of two length
     * we don't need a tail mix */

    h1 ^= len; h2 ^= len;

    h1 += h2;
    h2 += h1;

    h1 = fmix64(h1);
    h2 = fmix64(h2);

    h1 += h2;
    h2 += h1;

    return h1;
}


/*****************************************************************************
 *
 *                            -- 32bit --
 *
 ****************************************************************************/
#else

FORCE_INLINE uint32_t fmix32(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;

    return h;
}

/**
 * MurmurHash3 the 32bit variant.
 */
size_t hashtable_hash(const void *key, int len, uint32_t seed)
{
    const uint8_t *data    = (const uint8_t*)key;
    const int      nblocks = len / 4;

    uint32_t h1 = seed;

    const uint32_t c1 = 0xcc9e2d51;
    const uint32_t c2 = 0x1b873593;

    const uint32_t *blocks = (const uint32_t *)(data + nblocks*4);

    int i;
    for (i = -nblocks; i; i++) {
        uint32_t k1 = blocks[i];

        k1 *= c1;
        k1 = ROTL32(k1,15);
        k1 *= c2;

        h1 ^= k1;
        h1 = ROTL32(h1,13);
        h1 = h1*5+0xe6546b64;
    }

    const uint8_t * tail = (const uint8_t*)(data + nblocks*4);

    uint32_t k1 = 0;

    switch(len & 3) {
    case 3: k1 ^= tail[2] << 16;
    case 2: k1 ^= tail[1] << 8;
    case 1: k1 ^= tail[0];
            k1 *= c1;
            k1  = ROTL32(k1,15);
            k1 *= c2;
            h1 ^= k1;
    };

    h1 ^= len;
    h1  = fmix32(h1);

    return (size_t) h1;
}

/*
 * MurmurHash3 the 32bit variant that hashes the pointer itself
 */
size_t hashtable_hash_ptr(const void *key, int len, uint32_t seed)
{
    const int nblocks = len / 4;

    uint32_t h1 = seed;

    const uint32_t c1 = 0xcc9e2d51;
    const uint32_t c2 = 0x1b873593;

    int i;
    for (i = 0; i < nblocks; i++) {
        uint32_t k1 = ((uintptr_t) key >> (2*i)) & 0xff;

        k1 *= c1;
        k1 = ROTL32(k1,15);
        k1 *= c2;

        h1 ^= k1;
        h1 = ROTL32(h1,13);
        h1 = h1*5+0xe6546b64;
    }

    /* Since the pointers are power of two length
     * we don't need a tail mix */

    h1 ^= len;
    h1  = fmix32(h1);

    return (size_t) h1;
}

#endif /* ARCH_64 */
