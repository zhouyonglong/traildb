#define _DEFAULT_SOURCE /* ftruncate() */
#define _GNU_SOURCE /* mkostemp() */

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#undef JUDYERROR
#define JUDYERROR(CallerFile, CallerLine, JudyFunc, JudyErrno, JudyErrID) \
{                                                                         \
   if ((JudyErrno) != JU_ERRNO_NOMEM)                                     \
       goto out_of_memory;                                                \
}
#include <Judy.h>

#include "judy_str_map.h"
#include "tdb_internal.h"
#include "tdb_error.h"
#include "tdb_io.h"
#include "arena.h"

#ifndef EVENTS_ARENA_INCREMENT
#define EVENTS_ARENA_INCREMENT 1000000
#endif

struct jm_fold_state{
    FILE *out;
    uint64_t offset;
    int ret;
    uint64_t width;
};

static void *lexicon_store_fun(uint64_t id,
                               const char *value,
                               uint64_t len,
                               void *state)
{
    struct jm_fold_state *s = (struct jm_fold_state*)state;
    int ret = 0;

    if (s->ret)
        return state;

    /* NOTE: vals start at 1, otherwise we would need to +1 */
    TDB_SEEK(s->out, id * s->width);
    TDB_WRITE(s->out, &s->offset, s->width);

    TDB_SEEK(s->out, s->offset);
    TDB_WRITE(s->out, value, len);

done:
    s->ret = ret;
    s->offset += len;
    return state;
}

static int lexicon_store(const struct judy_str_map *lexicon, const char *path)
{
    /*
    Lexicon format:
    [ number of values N ] 4 or 8 bytes
    [ value offsets ...  ] N * (4 or 8 bytes)
    [ last value offset  ] 4 or 8 bytes
    [ values ...         ] X bytes
    */

    struct jm_fold_state state;
    uint64_t count = jsm_num_keys(lexicon);
    uint64_t size = (count + 2) * 4 + jsm_values_size(lexicon);
    int ret = 0;

    state.offset = (count + 2) * 4;
    state.width = 4;

    if (size > UINT32_MAX){
        size = (count + 2) * 8 + jsm_values_size(lexicon);
        state.offset = (count + 2) * 8;
        state.width = 8;
    }

    if (size > TDB_MAX_LEXICON_SIZE)
        return TDB_ERR_LEXICON_TOO_LARGE;

    state.out = NULL;
    state.ret = 0;

    TDB_OPEN(state.out, path, "w");
    TDB_TRUNCATE(state.out, (off_t)size);
    TDB_WRITE(state.out, &count, state.width);

    jsm_fold(lexicon, lexicon_store_fun, &state);
    if ((ret = state.ret))
        goto done;

    TDB_SEEK(state.out, (count + 1) * state.width);
    TDB_WRITE(state.out, &state.offset, state.width);

done:
    TDB_CLOSE_FINAL(state.out);
    return ret;
}

static int store_lexicons(tdb_cons *cons)
{
    tdb_field i;
    FILE *out = NULL;
    char path[TDB_MAX_PATH_SIZE];
    int ret = 0;

    TDB_PATH(path, "%s/fields", cons->root);
    TDB_OPEN(out, path, "w");

    for (i = 0; i < cons->num_ofields; i++){
        TDB_PATH(path, "%s/lexicon.%s", cons->root, cons->ofield_names[i]);
        if ((ret = lexicon_store(&cons->lexicons[i], path)))
            goto done;
        TDB_FPRINTF(out, "%s\n", cons->ofield_names[i]);
    }
done:
    TDB_CLOSE_FINAL(out);
    return ret;
}

static int store_version(tdb_cons *cons)
{
    FILE *out = NULL;
    char path[TDB_MAX_PATH_SIZE];
    int ret = 0;

    TDB_PATH(path, "%s/version", cons->root);
    TDB_OPEN(out, path, "w");
    TDB_FPRINTF(out, "%llu", TDB_VERSION_LATEST);
done:
    TDB_CLOSE_FINAL(out);
    return ret;
}

static void *store_uuids_fun(__uint128_t key,
                             Word_t *value __attribute__((unused)),
                             void *state)
{
    struct jm_fold_state *s = (struct jm_fold_state*)state;
    int ret = 0;
    TDB_WRITE(s->out, &key, 16);
done:
    s->ret = ret;
    return s;
}

static int store_uuids(tdb_cons *cons)
{
    char path[TDB_MAX_PATH_SIZE];
    struct jm_fold_state state = {.ret = 0};
    uint64_t num_trails = j128m_num_keys(&cons->trails);
    int ret = 0;

    /* this is why num_trails < TDB_MAX)NUM_TRAILS < 2^59:
       (2^59 - 1) * 16 < LONG_MAX (off_t) */
    if (num_trails > TDB_MAX_NUM_TRAILS)
        return TDB_ERR_TOO_MANY_TRAILS;

    TDB_PATH(path, "%s/uuids", cons->root);
    TDB_OPEN(state.out, path, "w");
    TDB_TRUNCATE(state.out, ((off_t)(num_trails * 16)));

    j128m_fold(&cons->trails, store_uuids_fun, &state);
    ret = state.ret;

done:
    TDB_CLOSE_FINAL(state.out);
    return ret;
}

int is_fieldname_invalid(const char* field)
{
    uint64_t i;

    if (!strcmp(field, "time"))
        return 1;

    for (i = 0; i < TDB_MAX_FIELDNAME_LENGTH && field[i]; i++)
        if (!index(TDB_FIELDNAME_CHARS, field[i]))
            return 1;

    if (i == 0 || i == TDB_MAX_FIELDNAME_LENGTH)
        return 1;

    return 0;
}

static int find_duplicate_fieldnames(const char **ofield_names,
                                     uint64_t num_ofields)
{
    Pvoid_t check = NULL;
    tdb_field i;
    Word_t tmp;

    for (i = 0; i < num_ofields; i++){
        Word_t *ptr;
        JSLI(ptr, check, (const uint8_t*)ofield_names[i]);
        if (*ptr){
            JSLFA(tmp, check);
            return TDB_ERR_DUPLICATE_FIELDS;

        }
        *ptr = 1;
    }
    JSLFA(tmp, check);
    return 0;

out_of_memory:
    return TDB_ERR_NOMEM;
}

tdb_cons *tdb_cons_init(void)
{
    return calloc(1, sizeof(tdb_cons));
}

int tdb_cons_open(tdb_cons *cons,
                  const char *root,
                  const char **ofield_names,
                  uint64_t num_ofields)
{
    tdb_field i;
    int fd;
    int ret = 0;

    /*
    by handling the "cons == NULL" case here gracefully, we allow the return
    value of tdb_init() to be used unchecked like here:

    int err;
    tdb_cons *cons = tdb_cons_init();
    if ((err = tdb_cons_open(cons, path, fields, num_fields)))
        printf("Opening cons failed: %s", tdb_error(err));
    */
    if (!cons)
        return TDB_ERR_HANDLE_IS_NULL;

    if (cons->events.item_size)
        return TDB_ERR_HANDLE_ALREADY_OPENED;

    if (num_ofields > TDB_MAX_NUM_FIELDS)
        return TDB_ERR_TOO_MANY_FIELDS;

    if ((ret = find_duplicate_fieldnames(ofield_names, num_ofields)))
        goto done;

    if (!(cons->ofield_names = calloc(num_ofields, sizeof(char*))))
        return TDB_ERR_NOMEM;

    for (i = 0; i < num_ofields; i++){
        if (is_fieldname_invalid(ofield_names[i])){
            ret = TDB_ERR_INVALID_FIELDNAME;
            goto done;
        }
        if (!(cons->ofield_names[i] = strdup(ofield_names[i]))){
            ret = TDB_ERR_NOMEM;
            goto done;
        }
    }

    j128m_init(&cons->trails);

    if (!(cons->root = strdup(root))){
        ret = TDB_ERR_NOMEM;
        goto done;
    }

    cons->min_timestamp = UINT64_MAX;
    cons->num_ofields = num_ofields;
    cons->events.arena_increment = EVENTS_ARENA_INCREMENT;
    cons->events.item_size = sizeof(tdb_cons_event);
    cons->items.item_size = sizeof(tdb_item);

    /* Opportunistically try to create the output directory.
       We don't care if it fails, e.g. because it already exists */
    mkdir(root, 0755);
    TDB_PATH(cons->tempfile, "%s/tmp.items.XXXXXX", root);
    if ((fd = mkostemp(cons->tempfile, 0)) == -1){
        ret = TDB_ERR_IO_OPEN;
        goto done;
    }

    if (!(cons->items.fd = fdopen(fd, "w"))){
        ret = TDB_ERR_IO_OPEN;
        goto done;
    }

    if (cons->num_ofields > 0)
        if (!(cons->lexicons = calloc(cons->num_ofields,
                                      sizeof(struct judy_str_map)))){
            ret = TDB_ERR_NOMEM;
            goto done;
        }

    for (i = 0; i < cons->num_ofields; i++)
        if (jsm_init(&cons->lexicons[i])){
            ret = TDB_ERR_NOMEM;
            goto done;
        }

done:
    return ret;
}

void tdb_cons_close(tdb_cons *cons)
{
    uint64_t i;
    for (i = 0; i < cons->num_ofields; i++){
        if (cons->ofield_names)
            free(cons->ofield_names[i]);
        if (cons->lexicons)
            jsm_free(&cons->lexicons[i]);
    }
    free(cons->lexicons);
    if (cons->items.fd)
        fclose(cons->items.fd);
    if (strcmp(cons->tempfile, ""))
        unlink(cons->tempfile);
    if (cons->events.data)
        free(cons->events.data);
    if (cons->items.data)
        free(cons->items.data);

    j128m_free(&cons->trails);
    free(cons->ofield_names);
    free(cons->root);
    free(cons);
}

/*
Append an event in this cons.
*/
int tdb_cons_add(tdb_cons *cons,
                 const uint8_t uuid[16],
                 const uint64_t timestamp,
                 const char **values,
                 const uint64_t *value_lengths)
{
    tdb_field i;
    tdb_cons_event *event;
    Word_t *uuid_ptr;
    __uint128_t uuid_key;

    for (i = 0; i < cons->num_ofields; i++)
        if (value_lengths[i] > TDB_MAX_VALUE_SIZE)
            return TDB_ERR_VALUE_TOO_LONG;

    memcpy(&uuid_key, uuid, 16);
    uuid_ptr = j128m_insert(&cons->trails, uuid_key);

    if (!(event = (tdb_cons_event*)arena_add_item(&cons->events)))
        return TDB_ERR_NOMEM;

    event->item_zero = cons->items.next;
    event->num_items = 0;
    event->timestamp = timestamp;
    event->prev_event_idx = *uuid_ptr;
    *uuid_ptr = cons->events.next;

    if (timestamp < cons->min_timestamp)
        cons->min_timestamp = timestamp;

    for (i = 0; i < cons->num_ofields; i++){
        tdb_field field = (tdb_field)(i + 1);
        tdb_val val = 0;
        tdb_item item;
        void *dst;

        /* TODO add a test for sparse trails */
        if (value_lengths[i]){
            if (!(val = (tdb_val)jsm_insert(&cons->lexicons[i],
                                            values[i],
                                            value_lengths[i])))
                return TDB_ERR_NOMEM;

        }
        item = tdb_make_item(field, val);
        if (!(dst = arena_add_item(&cons->items)))
            return TDB_ERR_NOMEM;
        memcpy(dst, &item, sizeof(tdb_item));
        ++event->num_items;
    }
    return 0;
}

/*
Append a single event to this cons. The event is a usual srequence
of (remapped) items. Used by tdb_cons_append().
*/
static void append_event(tdb_cons *cons,
                         const tdb_item *items,
                         Word_t *uuid_ptr)
{
    tdb_cons_event *event = (tdb_cons_event*)arena_add_item(&cons->events);

    event->item_zero = cons->items.next;
    event->num_items = 0;
    event->timestamp = items[0];
    event->prev_event_idx = *uuid_ptr;
    *uuid_ptr = cons->events.next;

    tdb_field field;
    for (field = 1; field < cons->num_ofields + 1; field++){
        if (items[field]){
            memcpy(arena_add_item(&cons->items),
                   &items[field],
                   sizeof(tdb_item));
            ++event->num_items;
        }
    }
}

/*
Append the lexicons of an existing TrailDB, db, to this cons. Used by
tdb_cons_append().
*/
static uint64_t **append_lexicons(tdb_cons *cons, const tdb *db)
{
    tdb_val **lexicon_maps;
    tdb_val i;
    tdb_field field;

    if (!(lexicon_maps = calloc(cons->num_ofields, sizeof(tdb_val*))))
        return NULL;

    for (field = 0; field < cons->num_ofields; field++){
        struct tdb_lexicon lex;
        uint64_t *map;

        tdb_lexicon_read(db, field + 1, &lex);

        if (!(map = lexicon_maps[field] = malloc(lex.size * sizeof(tdb_val))))
            goto error;

        for (i = 0; i < lex.size; i++){
            uint64_t value_length;
            const char *value = tdb_lexicon_get(&lex, i, &value_length);
            tdb_val val;
            if ((val = (tdb_val)jsm_insert(&cons->lexicons[field],
                                            value,
                                            value_length)))
                map[i] = val;
            else
                goto error;
        }
    }
    return lexicon_maps;
error:
    for (i = 0; i < cons->num_ofields; i++)
        free(lexicon_maps[i]);
    free(lexicon_maps);
    return NULL;
}

/*
This is a variation of tdb_cons_add(): Instead of accepting fields as
strings, it reads them as integer items from an existing TrailDB and
remaps them to match with this cons.
*/
int tdb_cons_append(tdb_cons *cons, const tdb *db)
{
    tdb_val **lexicon_maps = NULL;
    uint64_t trail_id;
    tdb_item *items = NULL;
    uint64_t i, e, n, items_len = 0;
    tdb_field field;
    int ret = 0;
    /* event_width includes the event delimiter, 0 byte */
    const uint64_t event_width = db->num_fields + 1;

    /* NOTE we could be much more permissive with what can be joined:
    we could support "full outer join" and replace all missing fields
    with NULLs automatically.
    */
    if (cons->num_ofields != db->num_fields - 1)
        return TDB_ERR_APPEND_FIELDS_MISMATCH;

    for (field = 0; field < cons->num_ofields; field++)
        if (strcmp(cons->ofield_names[field], tdb_get_field_name(db, field + 1)))
            return TDB_ERR_APPEND_FIELDS_MISMATCH;

    if (db->min_timestamp < cons->min_timestamp)
        cons->min_timestamp = db->min_timestamp;

    if (!(lexicon_maps = append_lexicons(cons, db))){
        ret = TDB_ERR_NOMEM;
        goto done;
    }

    for (trail_id = 0; trail_id < tdb_num_trails(db); trail_id++){
        __uint128_t uuid_key;
        Word_t *uuid_ptr;

        memcpy(&uuid_key, tdb_get_uuid(db, trail_id), 16);
        uuid_ptr = j128m_insert(&cons->trails, uuid_key);

        /* TODO this could use an iterator of the trail */
        if (tdb_get_trail(db, trail_id, &items, &items_len, &n, 0)){
            ret = TDB_ERR_NOMEM;
            goto done;
        }
        for (e = 0; e < n; e += event_width){
            for (field = 1; field < cons->num_ofields + 1; field++){
                uint64_t idx = e + field;
                tdb_val val = tdb_item_val(items[idx]);
                if (val)
                    /* translate non-NULL vals */
                    val = lexicon_maps[field - 1][val - 1];
                items[idx] = tdb_make_item(field, val);
            }
            append_event(cons, &items[e], uuid_ptr);
        }
    }
done:
    if (lexicon_maps){
        for (i = 0; i < cons->num_ofields; i++)
            free(lexicon_maps[i]);
        free(lexicon_maps);
    }
    free(items);
    return ret;
}

int tdb_cons_finalize(tdb_cons *cons, uint64_t flags __attribute__((unused)))
{
    struct tdb_file items_mmapped;
    uint64_t num_events = cons->events.next;
    int ret = 0;

    memset(&items_mmapped, 0, sizeof(struct tdb_file));

    /* finalize event items */
    if ((ret = arena_flush(&cons->items)))
        goto error;

    if (cons->items.fd && fclose(cons->items.fd)) {
        cons->items.fd = NULL;
        return TDB_ERR_IO_CLOSE;
    }
    cons->items.fd = NULL;

    if (num_events && cons->num_ofields) {
        if (tdb_mmap(cons->tempfile, &items_mmapped))
            return TDB_ERR_IO_READ;
    }

    TDB_TIMER_DEF

    TDB_TIMER_START
    if ((ret = store_lexicons(cons)))
        goto error;
    TDB_TIMER_END("encoder/store_lexicons")

    TDB_TIMER_START
    if ((ret = store_uuids(cons)))
        goto error;
    TDB_TIMER_END("encoder/store_uuids")

    TDB_TIMER_START
    if ((ret = store_version(cons)))
        goto error;
    TDB_TIMER_END("encoder/store_version")

    TDB_TIMER_START
    if ((ret = tdb_encode(cons, (tdb_item*)items_mmapped.data)))
        goto error;
    TDB_TIMER_END("encoder/encode")

error:
    if (items_mmapped.data)
        munmap(items_mmapped.data, items_mmapped.size);
    return ret;
}
