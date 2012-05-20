
/* Dictionary object implementation using a hash table */

/* The distribution includes a separate file, Objects/dictnotes.txt,
   describing explorations into dictionary design and optimization.
   It covers typical dictionary use patterns, the parameters for
   tuning dictionaries, and several ideas for possible optimizations.
*/

#include "Python.h"

long dub_hash(long x);
/* Set a key error with the specified argument, wrapping it in a
 * tuple automatically so that tuple keys are not unpacked as the
 * exception arguments. */
static void
set_key_error(PyObject *arg)
{
    PyObject *tup;
    tup = PyTuple_Pack(1, arg);
    if (!tup)
        return; /* caller will expect error to be set anyway */
    PyErr_SetObject(PyExc_KeyError, tup);
    Py_DECREF(tup);
}

/* Define this out if you don't want conversion statistics on exit. */
#undef SHOW_CONVERSION_COUNTS

/* See large comment block below.  This must be >= 1. */
#define PERTURB_SHIFT 5

/*
Major subtleties ahead:  Most hash schemes depend on having a "good" hash
function, in the sense of simulating randomness.  Python doesn't:  its most
important hash functions (for strings and ints) are very regular in common
cases:

>>> map(hash, (0, 1, 2, 3))
[0, 1, 2, 3]
>>> map(hash, ("namea", "nameb", "namec", "named"))
[-1658398457, -1658398460, -1658398459, -1658398462]
>>>

This isn't necessarily bad!  To the contrary, in a table of size 2**i, taking
the low-order i bits as the initial table index is extremely fast, and there
are no collisions at all for dicts indexed by a contiguous range of ints.
The same is approximately true when keys are "consecutive" strings.  So this
gives better-than-random behavior in common cases, and that's very desirable.

OTOH, when collisions occur, the tendency to fill contiguous slices of the
hash table makes a good collision resolution strategy crucial.  Taking only
the last i bits of the hash code is also vulnerable:  for example, consider
[i << 16 for i in range(20000)] as a set of keys.  Since ints are their own
hash codes, and this fits in a dict of size 2**15, the last 15 bits of every
hash code are all 0:  they *all* map to the same table index.

But catering to unusual cases should not slow the usual ones, so we just take
the last i bits anyway.  It's up to collision resolution to do the rest.  If
we *usually* find the key we're looking for on the first try (and, it turns
out, we usually do -- the table load factor is kept under 2/3, so the odds
are solidly in our favor), then it makes best sense to keep the initial index
computation dirt cheap.

The first half of collision resolution is to visit table indices via this
recurrence:

    j = ((5*j) + 1) mod 2**i

For any initial j in range(2**i), repeating that 2**i times generates each
int in range(2**i) exactly once (see any text on random-number generation for
proof).  By itself, this doesn't help much:  like linear probing (setting
j += 1, or j -= 1, on each loop trip), it scans the table entries in a fixed
order.  This would be bad, except that's not the only thing we do, and it's
actually *good* in the common cases where hash keys are consecutive.  In an
example that's really too small to make this entirely clear, for a table of
size 2**3 the order of indices is:

    0 -> 1 -> 6 -> 7 -> 4 -> 5 -> 2 -> 3 -> 0 [and here it's repeating]

If two things come in at index 5, the first place we look after is index 2,
not 6, so if another comes in at index 6 the collision at 5 didn't hurt it.
Linear probing is deadly in this case because there the fixed probe order
is the *same* as the order consecutive keys are likely to arrive.  But it's
extremely unlikely hash codes will follow a 5*j+1 recurrence by accident,
and certain that consecutive hash codes do not.

The other half of the strategy is to get the other bits of the hash code
into play.  This is done by initializing a (unsigned) vrbl "perturb" to the
full hash code, and changing the recurrence to:

    j = (5*j) + 1 + perturb;
    perturb >>= PERTURB_SHIFT;
    use j % 2**i as the next table index;

Now the probe sequence depends (eventually) on every bit in the hash code,
and the pseudo-scrambling property of recurring on 5*j+1 is more valuable,
because it quickly magnifies small differences in the bits that didn't affect
the initial index.  Note that because perturb is unsigned, if the recurrence
is executed often enough perturb eventually becomes and remains 0.  At that
point (very rarely reached) the recurrence is on (just) 5*j+1 again, and
that's certain to find an empty slot eventually (since it generates every int
in range(2**i), and we make sure there's always at least one empty slot).

Selecting a good value for PERTURB_SHIFT is a balancing act.  You want it
small so that the high bits of the hash code continue to affect the probe
sequence across iterations; but you want it large so that in really bad cases
the high-order hash bits have an effect on early iterations.  5 was "the
best" in minimizing total collisions across experiments Tim Peters ran (on
both normal and pathological cases), but 4 and 6 weren't significantly worse.

Historical:  Reimer Behrends contributed the idea of using a polynomial-based
approach, using repeated multiplication by x in GF(2**n) where an irreducible
polynomial for each table size was chosen such that x was a primitive root.
Christian Tismer later extended that to use division by x instead, as an
efficient way to get the high bits of the hash code into play.  This scheme
also gave excellent collision statistics, but was more expensive:  two
if-tests were required inside the loop; computing "the next" index took about
the same number of operations but without as much potential parallelism
(e.g., computing 5*j can go on at the same time as computing 1+perturb in the
above, and then shifting perturb can be done while the table index is being
masked); and the PyDictObject struct required a member to hold the table's
polynomial.  In Tim's experiments the current scheme ran faster, produced
equally good collision statistics, needed less code & used less memory.

Theoretical Python 2.5 headache:  hash codes are only C "long", but
sizeof(Py_ssize_t) > sizeof(long) may be possible.  In that case, and if a
dict is genuinely huge, then only the slots directly reachable via indexing
by a C long can be the first slot in a probe sequence.  The probe sequence
will still eventually reach every slot in the table, but the collision rate
on initial probes may be much higher than this scheme was designed for.
Getting a hash code as fat as Py_ssize_t is the only real cure.  But in
practice, this probably won't make a lick of difference for many years (at
which point everyone will have terabytes of RAM on 64-bit boxes).
*/

/* Object used as dummy key to fill deleted entries */
static PyObject *dummy = NULL; /* Initialized by first call to newPyDictObject() */

#ifdef Py_REF_DEBUG
PyObject *
_PyDict_Dummy(void)
{
    return dummy;
}
#endif

/* forward declarations */
static PyDictEntry *
lookdict_string(PyDictObject *mp, PyObject *key, long hash);


#ifdef SHOW_CONVERSION_COUNTS
static long created = 0L;
static long converted = 0L;

static void
show_counts(void)
{
    fprintf(stderr, "created %ld string dicts\n", created);
    fprintf(stderr, "converted %ld to normal dicts\n", converted);
    fprintf(stderr, "%.2f%% conversion rate\n", (100.0*converted)/created);
}
#endif

/* Debug statistic to compare allocations with reuse through the free list */
#undef SHOW_ALLOC_COUNT
#ifdef SHOW_ALLOC_COUNT
static size_t count_alloc = 0;
static size_t count_reuse = 0;

static void
show_alloc(void)
{
    fprintf(stderr, "Dict allocations: %" PY_FORMAT_SIZE_T "d\n",
        count_alloc);
    fprintf(stderr, "Dict reuse through freelist: %" PY_FORMAT_SIZE_T
        "d\n", count_reuse);
    fprintf(stderr, "%.2f%% reuse rate\n\n",
        (100.0*count_reuse/(count_alloc+count_reuse)));
}
#endif

/* Debug statistic to count GC tracking of dicts */
#ifdef SHOW_TRACK_COUNT
static Py_ssize_t count_untracked = 0;
static Py_ssize_t count_tracked = 0;

static void
show_track(void)
{
    fprintf(stderr, "Dicts created: %" PY_FORMAT_SIZE_T "d\n",
        count_tracked + count_untracked);
    fprintf(stderr, "Dicts tracked by the GC: %" PY_FORMAT_SIZE_T
        "d\n", count_tracked);
    fprintf(stderr, "%.2f%% dict tracking rate\n\n",
        (100.0*count_tracked/(count_untracked+count_tracked)));
}
#endif


/* Initialization macros.
   There are two ways to create a dict:  PyDict_New() is the main C API
   function, and the tp_new slot maps to dict_new().  In the latter case we
   can save a little time over what PyDict_New does because it's guaranteed
   that the PyDictObject struct is already zeroed out.
   Everyone except dict_new() should use EMPTY_TO_MINSIZE (unless they have
   an excellent reason not to).
*/

#define INIT_NONZERO_DICT_SLOTS(mp) do {                                \
    (mp)->ma_table = (mp)->ma_smalltable;                               \
    (mp)->ma_mask = PyDict_MINSIZE - 1;                                 \
    } while(0)

#define EMPTY_TO_MINSIZE(mp) do {                                       \
    memset((mp)->ma_smalltable, 0, sizeof((mp)->ma_smalltable));        \
    (mp)->ma_used = (mp)->ma_fill = 0;                                  \
    INIT_NONZERO_DICT_SLOTS(mp);                                        \
    } while(0)

/* Dictionary reuse scheme to save calls to malloc, free, and memset */
#ifndef PyDict_MAXFREELIST
#define PyDict_MAXFREELIST 80
#endif
static PyDictObject *free_list[PyDict_MAXFREELIST];
static int numfree = 0;

void
PyDict_Fini(void)
{
    PyDictObject *op;

    while (numfree) {
        op = free_list[--numfree];
        assert(PyDict_CheckExact(op));
        PyObject_GC_Del(op);
    }
}

PyObject *
PyDict_New(void)
{
    register PyDictObject *mp;
    if (dummy == NULL) { /* Auto-initialize dummy */
        dummy = PyString_FromString("<dummy key>");
        if (dummy == NULL)
            return NULL;
#ifdef SHOW_CONVERSION_COUNTS
        Py_AtExit(show_counts);
#endif
#ifdef SHOW_ALLOC_COUNT
        Py_AtExit(show_alloc);
#endif
#ifdef SHOW_TRACK_COUNT
        Py_AtExit(show_track);
#endif
    }
    if (numfree) {
        mp = free_list[--numfree];
        assert (mp != NULL);
        assert (Py_TYPE(mp) == &PyDict_Type);
        _Py_NewReference((PyObject *)mp);
        if (mp->ma_fill) {
            EMPTY_TO_MINSIZE(mp);
        } else {
            /* At least set ma_table and ma_mask; these are wrong
               if an empty but presized dict is added to freelist */
            INIT_NONZERO_DICT_SLOTS(mp);
        }
        assert (mp->ma_used == 0);
        assert (mp->ma_table == mp->ma_smalltable);
        assert (mp->ma_mask == PyDict_MINSIZE - 1);
#ifdef SHOW_ALLOC_COUNT
        count_reuse++;
#endif
    } else {
        mp = PyObject_GC_New(PyDictObject, &PyDict_Type);
        if (mp == NULL)
            return NULL;
        EMPTY_TO_MINSIZE(mp);
#ifdef SHOW_ALLOC_COUNT
        count_alloc++;
#endif
    }
    mp->ma_lookup = lookdict_string;
#ifdef SHOW_TRACK_COUNT
    count_untracked++;
#endif
#ifdef SHOW_CONVERSION_COUNTS
    ++created;
#endif
    return (PyObject *)mp;
}

/*
The basic lookup function used by all operations.
This is based on Algorithm D from Knuth Vol. 3, Sec. 6.4.
Open addressing is preferred over chaining since the link overhead for
chaining would be substantial (100% with typical malloc overhead).

The initial probe index is computed as hash mod the table size. Subsequent
probe indices are computed as explained earlier.

All arithmetic on hash should ignore overflow.

(The details in this version are due to Tim Peters, building on many past
contributions by Reimer Behrends, Jyrki Alakuijala, Vladimir Marangozov and
Christian Tismer).

lookdict() is general-purpose, and may return NULL if (and only if) a
comparison raises an exception (this was new in Python 2.5).
lookdict_string() below is specialized to string keys, comparison of which can
never raise an exception; that function can never return NULL.  For both, when
the key isn't found a PyDictEntry* is returned for which the me_value field is
NULL; this is the slot in the dict at which the key would have been found, and
the caller can (if it wishes) add the <key, value> pair to the returned
PyDictEntry*.
*/
#ifdef INSTRUMENT_DICT
//instrumentation
static int nlookupcount = 0;
static int nprobecount = 0;
static int ncollisioncount = 0;
#endif
static PyDictEntry *
lookdict(PyDictObject *mp, PyObject *key, register long hash)
{
    register size_t i;
    register size_t perturb;
    register PyDictEntry *freeslot;
    register size_t mask = (size_t)mp->ma_mask;
    PyDictEntry *ep0 = mp->ma_table;
    register PyDictEntry *ep;
    register int cmp;
    PyObject *startkey;
#ifdef INSTRUMENT_DICT
    nlookupcount++;
    nprobecount++;
#endif

    #ifdef DOUBLE_HASH
    hash = dub_hash(hash);
    #endif
    i = (size_t)hash & mask;
    ep = &ep0[i];
    if (ep->me_key == NULL || ep->me_key == key)
        return ep;

    if (ep->me_key == dummy)
        freeslot = ep;
    else {
        if (ep->me_hash == hash) {
            startkey = ep->me_key;
            Py_INCREF(startkey);
            cmp = PyObject_RichCompareBool(startkey, key, Py_EQ);
            Py_DECREF(startkey);
            if (cmp < 0)
                return NULL;
            if (ep0 == mp->ma_table && ep->me_key == startkey) {
                if (cmp > 0)
                    return ep;
            }
            else {
                /* The compare did major nasty stuff to the
                 * dict:  start over.
                 * XXX A clever adversary could prevent this
                 * XXX from terminating.
                 */
                return lookdict(mp, key, hash);
            }
        }
        freeslot = NULL;
    }

    /* In the loop, me_key == dummy is by far (factor of 100s) the
       least likely outcome, so test for that last. */
#ifdef LINEAR_PROBING
    while (1) { //should finish if there is an empty slot which due to size constraints we guarantee
        i = i + 1;
#else
    for (perturb = hash; ; perturb >>= PERTURB_SHIFT) {
        i = (i << 2) + i + perturb + 1;
#endif
#ifdef INSTRUMENT_DICT
        nprobecount++;
        ncollisioncount++;
#endif
        ep = &ep0[i & mask];
        if (ep->me_key == NULL)
            return freeslot == NULL ? ep : freeslot;
        if (ep->me_key == key)
            return ep;
        if (ep->me_hash == hash && ep->me_key != dummy) {
            startkey = ep->me_key;
            Py_INCREF(startkey);
            cmp = PyObject_RichCompareBool(startkey, key, Py_EQ);
            Py_DECREF(startkey);
            if (cmp < 0)
                return NULL;
            if (ep0 == mp->ma_table && ep->me_key == startkey) {
                if (cmp > 0)
                    return ep;
            }
            else {
                /* The compare did major nasty stuff to the
                 * dict:  start over.
                 * XXX A clever adversary could prevent this
                 * XXX from terminating.
                 */
                return lookdict(mp, key, hash);
            }
        }
        else if (ep->me_key == dummy && freeslot == NULL)
            freeslot = ep;
    }
    assert(0);          /* NOT REACHED */
    return 0;
}

/*
 * Hacked up version of lookdict which can assume keys are always strings;
 * this assumption allows testing for errors during PyObject_RichCompareBool()
 * to be dropped; string-string comparisons never raise exceptions.  This also
 * means we don't need to go through PyObject_RichCompareBool(); we can always
 * use _PyString_Eq() directly.
 *
 * This is valuable because dicts with only string keys are very common.
 */
#ifdef INSTRUMENT_DICT
//instrumentation
static int slookupcount = 0;
static int sprobecount = 0;
static int scollisioncount = 0;
#endif
static PyDictEntry *
lookdict_string(PyDictObject *mp, PyObject *key, register long hash)
{
    register size_t i;
    register size_t perturb;
    register PyDictEntry *freeslot;
    register size_t mask = (size_t)mp->ma_mask;
    PyDictEntry *ep0 = mp->ma_table;
    register PyDictEntry *ep;

#ifdef DOUBLE_HASH
    hash = dub_hash(hash);
#endif
    /* Make sure this function doesn't have to handle non-string keys,
       including subclasses of str; e.g., one reason to subclass
       strings is to override __eq__, and for speed we don't cater to
       that here. */
    if (!PyString_CheckExact(key)) {
#ifdef SHOW_CONVERSION_COUNTS
        ++converted;
#endif
        mp->ma_lookup = lookdict;
        return lookdict(mp, key, hash);
    }
#ifdef INSTRUMENT_DICT
    slookupcount++;
    sprobecount++;
#endif

    i = hash & mask;
    ep = &ep0[i];
    if (ep->me_key == NULL || ep->me_key == key)
        return ep;
    if (ep->me_key == dummy)
        freeslot = ep;
    else {
        if (ep->me_hash == hash && _PyString_Eq(ep->me_key, key))
            return ep;
        freeslot = NULL;
    }

    /* In the loop, me_key == dummy is by far (factor of 100s) the
       least likely outcome, so test for that last. */
#ifdef LINEAR_PROBING
    while (1) { //should finish if there is an empty slot which due to size constraints we guarantee
        i = i + 1;
#else
    for (perturb = hash; ; perturb >>= PERTURB_SHIFT) {
        i = (i << 2) + i + perturb + 1;
#endif
#ifdef INSTRUMENT_DICT
        sprobecount++;
        scollisioncount++;
#endif
        ep = &ep0[i & mask];
        if (ep->me_key == NULL)
            return freeslot == NULL ? ep : freeslot;
        if (ep->me_key == key
            || (ep->me_hash == hash
            && ep->me_key != dummy
            && _PyString_Eq(ep->me_key, key)))
            return ep;
        if (ep->me_key == dummy && freeslot == NULL)
            freeslot = ep;
    }
    assert(0);          /* NOT REACHED */
    return 0;
}

#ifdef SHOW_TRACK_COUNT
#define INCREASE_TRACK_COUNT \
    (count_tracked++, count_untracked--);
#define DECREASE_TRACK_COUNT \
    (count_tracked--, count_untracked++);
#else
#define INCREASE_TRACK_COUNT
#define DECREASE_TRACK_COUNT
#endif

#define MAINTAIN_TRACKING(mp, key, value) \
    do { \
        if (!_PyObject_GC_IS_TRACKED(mp)) { \
            if (_PyObject_GC_MAY_BE_TRACKED(key) || \
                _PyObject_GC_MAY_BE_TRACKED(value)) { \
                _PyObject_GC_TRACK(mp); \
                INCREASE_TRACK_COUNT \
            } \
        } \
    } while(0)

void
_PyDict_MaybeUntrack(PyObject *op)
{
    PyDictObject *mp;
    PyObject *value;
    Py_ssize_t mask, i;
    PyDictEntry *ep;

    if (!PyDict_CheckExact(op) || !_PyObject_GC_IS_TRACKED(op))
        return;

    mp = (PyDictObject *) op;
    ep = mp->ma_table;
    mask = mp->ma_mask;
    for (i = 0; i <= mask; i++) {
        if ((value = ep[i].me_value) == NULL)
            continue;
        if (_PyObject_GC_MAY_BE_TRACKED(value) ||
            _PyObject_GC_MAY_BE_TRACKED(ep[i].me_key))
            return;
    }
    DECREASE_TRACK_COUNT
    _PyObject_GC_UNTRACK(op);
}


/*
Internal routine to insert a new item into the table.
Used both by the internal resize routine and by the public insert routine.
Eats a reference to key and one to value.
Returns -1 if an error occurred, or 0 on success.
*/
static int
insertdict(register PyDictObject *mp, PyObject *key, long hash, PyObject *value)
{
    PyObject *old_value;
    register PyDictEntry *ep;
    typedef PyDictEntry *(*lookupfunc)(PyDictObject *, PyObject *, long);

    assert(mp->ma_lookup != NULL);
    ep = mp->ma_lookup(mp, key, hash);
    if (ep == NULL) {
        Py_DECREF(key);
        Py_DECREF(value);
        return -1;
    }
    MAINTAIN_TRACKING(mp, key, value);
    if (ep->me_value != NULL) {
        old_value = ep->me_value;
        ep->me_value = value;
        Py_DECREF(old_value); /* which **CAN** re-enter */
        Py_DECREF(key);
    }
    else {
        if (ep->me_key == NULL)
            mp->ma_fill++;
        else {
            assert(ep->me_key == dummy);
            Py_DECREF(dummy);
        }
        ep->me_key = key;
        ep->me_hash = (Py_ssize_t)hash;
        ep->me_value = value;
        mp->ma_used++;
    }
    return 0;
}

/*
Internal routine used by dictresize() to insert an item which is
known to be absent from the dict.  This routine also assumes that
the dict contains no deleted entries.  Besides the performance benefit,
using insertdict() in dictresize() is dangerous (SF bug #1456209).
Note that no refcounts are changed by this routine; if needed, the caller
is responsible for incref'ing `key` and `value`.
*/
static void
insertdict_clean(register PyDictObject *mp, PyObject *key, long hash,
                 PyObject *value)
{
    register size_t i;
    register size_t perturb;
    register size_t mask = (size_t)mp->ma_mask;
    PyDictEntry *ep0 = mp->ma_table;
    register PyDictEntry *ep;

    MAINTAIN_TRACKING(mp, key, value);
#ifdef DOUBLE_HASHING
    hash = dub_hash(hash);
#endif


    i = hash & mask;
    ep = &ep0[i];

#ifdef LINEAR_PROBING
    while (ep->me_key != NULL) { //should finish if there is an empty slot which due to size constraints we guarantee
        i = i + 1;
#else
    for (perturb = hash; ep->me_key != NULL; perturb >>= PERTURB_SHIFT) {
        i = (i << 2) + i + perturb + 1;
#endif
        ep = &ep0[i & mask];
    }
    assert(ep->me_value == NULL);
    mp->ma_fill++;
    ep->me_key = key;
    ep->me_hash = (Py_ssize_t)hash;
    ep->me_value = value;
    mp->ma_used++;
}

/*
Restructure the table by allocating a new table and reinserting all
items again.  When entries have been deleted, the new table may
actually be smaller than the old one.
*/
static int
dictresize(PyDictObject *mp, Py_ssize_t minused)
{
    Py_ssize_t newsize;
    PyDictEntry *oldtable, *newtable, *ep;
    Py_ssize_t i;
    int is_oldtable_malloced;
    PyDictEntry small_copy[PyDict_MINSIZE];

    assert(minused >= 0);

    /* Find the smallest table size > minused. */
    for (newsize = PyDict_MINSIZE;
         newsize <= minused && newsize > 0;
         newsize <<= 1)
        ;
    if (newsize <= 0) {
        PyErr_NoMemory();
        return -1;
    }

    /* Get space for a new table. */
    oldtable = mp->ma_table;
    assert(oldtable != NULL);
    is_oldtable_malloced = oldtable != mp->ma_smalltable;

    if (newsize == PyDict_MINSIZE) {
        /* A large table is shrinking, or we can't get any smaller. */
        newtable = mp->ma_smalltable;
        if (newtable == oldtable) {
            if (mp->ma_fill == mp->ma_used) {
                /* No dummies, so no point doing anything. */
                return 0;
            }
            /* We're not going to resize it, but rebuild the
               table anyway to purge old dummy entries.
               Subtle:  This is *necessary* if fill==size,
               as lookdict needs at least one virgin slot to
               terminate failing searches.  If fill < size, it's
               merely desirable, as dummies slow searches. */
            assert(mp->ma_fill > mp->ma_used);
            memcpy(small_copy, oldtable, sizeof(small_copy));
            oldtable = small_copy;
        }
    }
    else {
        newtable = PyMem_NEW(PyDictEntry, newsize);
        if (newtable == NULL) {
            PyErr_NoMemory();
            return -1;
        }
    }

    /* Make the dict empty, using the new table. */
    assert(newtable != oldtable);
    mp->ma_table = newtable;
    mp->ma_mask = newsize - 1;
    memset(newtable, 0, sizeof(PyDictEntry) * newsize);
    mp->ma_used = 0;
    i = mp->ma_fill;
    mp->ma_fill = 0;

    /* Copy the data over; this is refcount-neutral for active entries;
       dummy entries aren't copied over, of course */
    for (ep = oldtable; i > 0; ep++) {
        if (ep->me_value != NULL) {             /* active entry */
            --i;
            insertdict_clean(mp, ep->me_key, (long)ep->me_hash,
                             ep->me_value);
        }
        else if (ep->me_key != NULL) {          /* dummy entry */
            --i;
            assert(ep->me_key == dummy);
            Py_DECREF(ep->me_key);
        }
        /* else key == value == NULL:  nothing to do */
    }

    if (is_oldtable_malloced)
        PyMem_DEL(oldtable);
    return 0;
}

/* Create a new dictionary pre-sized to hold an estimated number of elements.
   Underestimates are okay because the dictionary will resize as necessary.
   Overestimates just mean the dictionary will be more sparse than usual.
*/

PyObject *
_PyDict_NewPresized(Py_ssize_t minused)
{
    PyObject *op = PyDict_New();

    if (minused>5 && op != NULL && dictresize((PyDictObject *)op, minused) == -1) {
        Py_DECREF(op);
        return NULL;
    }
    return op;
}

/* Note that, for historical reasons, PyDict_GetItem() suppresses all errors
 * that may occur (originally dicts supported only string keys, and exceptions
 * weren't possible).  So, while the original intent was that a NULL return
 * meant the key wasn't present, in reality it can mean that, or that an error
 * (suppressed) occurred while computing the key's hash, or that some error
 * (suppressed) occurred when comparing keys in the dict's internal probe
 * sequence.  A nasty example of the latter is when a Python-coded comparison
 * function hits a stack-depth error, which can cause this to return NULL
 * even if the key is present.
 */
PyObject *
PyDict_GetItem(PyObject *op, PyObject *key)
{
    long hash;
    PyDictObject *mp = (PyDictObject *)op;
    PyDictEntry *ep;
    PyThreadState *tstate;
    if (!PyDict_Check(op))
        return NULL;
    if (!PyString_CheckExact(key) ||
        (hash = ((PyStringObject *) key)->ob_shash) == -1)
    {
        hash = PyObject_Hash(key);
        if (hash == -1) {
            PyErr_Clear();
            return NULL;
        }
    }

    /* We can arrive here with a NULL tstate during initialization: try
       running "python -Wi" for an example related to string interning.
       Let's just hope that no exception occurs then...  This must be
       _PyThreadState_Current and not PyThreadState_GET() because in debug
       mode, the latter complains if tstate is NULL. */
    tstate = _PyThreadState_Current;
    if (tstate != NULL && tstate->curexc_type != NULL) {
        /* preserve the existing exception */
        PyObject *err_type, *err_value, *err_tb;
        PyErr_Fetch(&err_type, &err_value, &err_tb);
        ep = (mp->ma_lookup)(mp, key, hash);
        /* ignore errors */
        PyErr_Restore(err_type, err_value, err_tb);
        if (ep == NULL)
            return NULL;
    }
    else {
        ep = (mp->ma_lookup)(mp, key, hash);
        if (ep == NULL) {
            PyErr_Clear();
            return NULL;
        }
    }
    return ep->me_value;
}

/* CAUTION: PyDict_SetItem() must guarantee that it won't resize the
 * dictionary if it's merely replacing the value for an existing key.
 * This means that it's safe to loop over a dictionary with PyDict_Next()
 * and occasionally replace a value -- but you can't insert new keys or
 * remove them.
 */
int
PyDict_SetItem(register PyObject *op, PyObject *key, PyObject *value)
{
    register PyDictObject *mp;
    register long hash;
    register Py_ssize_t n_used;

    if (!PyDict_Check(op)) {
        PyErr_BadInternalCall();
        return -1;
    }
    assert(key);
    assert(value);
    mp = (PyDictObject *)op;
    if (PyString_CheckExact(key)) {
        hash = ((PyStringObject *)key)->ob_shash;
        if (hash == -1)
            hash = PyObject_Hash(key);
    }
    else {
        hash = PyObject_Hash(key);
        if (hash == -1)
            return -1;
    }
    assert(mp->ma_fill <= mp->ma_mask);  /* at least one empty slot */
    n_used = mp->ma_used;
    Py_INCREF(value);
    Py_INCREF(key);
    if (insertdict(mp, key, hash, value) != 0)
        return -1;
    /* If we added a key, we can safely resize.  Otherwise just return!
     * If fill >= 2/3 size, adjust size.  Normally, this doubles or
     * quaduples the size, but it's also possible for the dict to shrink
     * (if ma_fill is much larger than ma_used, meaning a lot of dict
     * keys have been * deleted).
     *
     * Quadrupling the size improves average dictionary sparseness
     * (reducing collisions) at the cost of some memory and iteration
     * speed (which loops over every possible entry).  It also halves
     * the number of expensive resize operations in a growing dictionary.
     *
     * Very large dictionaries (over 50K items) use doubling instead.
     * This may help applications with severe memory constraints.
     */
    if (!(mp->ma_used > n_used && mp->ma_fill*3 >= (mp->ma_mask+1)*2))
        return 0;
    return dictresize(mp, (mp->ma_used > 50000 ? 2 : 4) * mp->ma_used);
}

int
PyDict_DelItem(PyObject *op, PyObject *key)
{
    register PyDictObject *mp;
    register long hash;
    register PyDictEntry *ep;
    PyObject *old_value, *old_key;

    if (!PyDict_Check(op)) {
        PyErr_BadInternalCall();
        return -1;
    }
    assert(key);
    if (!PyString_CheckExact(key) ||
        (hash = ((PyStringObject *) key)->ob_shash) == -1) {
        hash = PyObject_Hash(key);
        if (hash == -1)
            return -1;
    }
    mp = (PyDictObject *)op;
    ep = (mp->ma_lookup)(mp, key, hash);
    if (ep == NULL)
        return -1;
    if (ep->me_value == NULL) {
        set_key_error(key);
        return -1;
    }
    old_key = ep->me_key;
    Py_INCREF(dummy);
    ep->me_key = dummy;
    old_value = ep->me_value;
    ep->me_value = NULL;
    mp->ma_used--;
    Py_DECREF(old_value);
    Py_DECREF(old_key);
    return 0;
}

void
PyDict_Clear(PyObject *op)
{
    PyDictObject *mp;
    PyDictEntry *ep, *table;
    int table_is_malloced;
    Py_ssize_t fill;
    PyDictEntry small_copy[PyDict_MINSIZE];
#ifdef Py_DEBUG
    Py_ssize_t i, n;
#endif

    if (!PyDict_Check(op))
        return;
    mp = (PyDictObject *)op;
#ifdef Py_DEBUG
    n = mp->ma_mask + 1;
    i = 0;
#endif

    table = mp->ma_table;
    assert(table != NULL);
    table_is_malloced = table != mp->ma_smalltable;

    /* This is delicate.  During the process of clearing the dict,
     * decrefs can cause the dict to mutate.  To avoid fatal confusion
     * (voice of experience), we have to make the dict empty before
     * clearing the slots, and never refer to anything via mp->xxx while
     * clearing.
     */
    fill = mp->ma_fill;
    if (table_is_malloced)
        EMPTY_TO_MINSIZE(mp);

    else if (fill > 0) {
        /* It's a small table with something that needs to be cleared.
         * Afraid the only safe way is to copy the dict entries into
         * another small table first.
         */
        memcpy(small_copy, table, sizeof(small_copy));
        table = small_copy;
        EMPTY_TO_MINSIZE(mp);
    }
    /* else it's a small table that's already empty */

    /* Now we can finally clear things.  If C had refcounts, we could
     * assert that the refcount on table is 1 now, i.e. that this function
     * has unique access to it, so decref side-effects can't alter it.
     */
    for (ep = table; fill > 0; ++ep) {
#ifdef Py_DEBUG
        assert(i < n);
        ++i;
#endif
        if (ep->me_key) {
            --fill;
            Py_DECREF(ep->me_key);
            Py_XDECREF(ep->me_value);
        }
#ifdef Py_DEBUG
        else
            assert(ep->me_value == NULL);
#endif
    }

    if (table_is_malloced)
        PyMem_DEL(table);
}

/*
 * Iterate over a dict.  Use like so:
 *
 *     Py_ssize_t i;
 *     PyObject *key, *value;
 *     i = 0;   # important!  i should not otherwise be changed by you
 *     while (PyDict_Next(yourdict, &i, &key, &value)) {
 *              Refer to borrowed references in key and value.
 *     }
 *
 * CAUTION:  In general, it isn't safe to use PyDict_Next in a loop that
 * mutates the dict.  One exception:  it is safe if the loop merely changes
 * the values associated with the keys (but doesn't insert new keys or
 * delete keys), via PyDict_SetItem().
 */
int
PyDict_Next(PyObject *op, Py_ssize_t *ppos, PyObject **pkey, PyObject **pvalue)
{
    register Py_ssize_t i;
    register Py_ssize_t mask;
    register PyDictEntry *ep;

    if (!PyDict_Check(op))
        return 0;
    i = *ppos;
    if (i < 0)
        return 0;
    ep = ((PyDictObject *)op)->ma_table;
    mask = ((PyDictObject *)op)->ma_mask;
    while (i <= mask && ep[i].me_value == NULL)
        i++;
    *ppos = i+1;
    if (i > mask)
        return 0;
    if (pkey)
        *pkey = ep[i].me_key;
    if (pvalue)
        *pvalue = ep[i].me_value;
    return 1;
}

/* Internal version of PyDict_Next that returns a hash value in addition to the key and value.*/
int
_PyDict_Next(PyObject *op, Py_ssize_t *ppos, PyObject **pkey, PyObject **pvalue, long *phash)
{
    register Py_ssize_t i;
    register Py_ssize_t mask;
    register PyDictEntry *ep;

    if (!PyDict_Check(op))
        return 0;
    i = *ppos;
    if (i < 0)
        return 0;
    ep = ((PyDictObject *)op)->ma_table;
    mask = ((PyDictObject *)op)->ma_mask;
    while (i <= mask && ep[i].me_value == NULL)
        i++;
    *ppos = i+1;
    if (i > mask)
        return 0;
    *phash = (long)(ep[i].me_hash);
    if (pkey)
        *pkey = ep[i].me_key;
    if (pvalue)
        *pvalue = ep[i].me_value;
    return 1;
}

/* Methods */

static void
dict_dealloc(register PyDictObject *mp)
{
    register PyDictEntry *ep;
    Py_ssize_t fill = mp->ma_fill;
    PyObject_GC_UnTrack(mp);
    Py_TRASHCAN_SAFE_BEGIN(mp)
    for (ep = mp->ma_table; fill > 0; ep++) {
        if (ep->me_key) {
            --fill;
            Py_DECREF(ep->me_key);
            Py_XDECREF(ep->me_value);
        }
    }
    if (mp->ma_table != mp->ma_smalltable)
        PyMem_DEL(mp->ma_table);
    if (numfree < PyDict_MAXFREELIST && Py_TYPE(mp) == &PyDict_Type)
        free_list[numfree++] = mp;
    else
        Py_TYPE(mp)->tp_free((PyObject *)mp);
    Py_TRASHCAN_SAFE_END(mp)
}

static int
dict_print(register PyDictObject *mp, register FILE *fp, register int flags)
{
    register Py_ssize_t i;
    register Py_ssize_t any;
    int status;

    status = Py_ReprEnter((PyObject*)mp);
    if (status != 0) {
        if (status < 0)
            return status;
        Py_BEGIN_ALLOW_THREADS
        fprintf(fp, "{...}");
        Py_END_ALLOW_THREADS
        return 0;
    }

    Py_BEGIN_ALLOW_THREADS
    fprintf(fp, "{");
    Py_END_ALLOW_THREADS
    any = 0;
    for (i = 0; i <= mp->ma_mask; i++) {
        PyDictEntry *ep = mp->ma_table + i;
        PyObject *pvalue = ep->me_value;
        if (pvalue != NULL) {
            /* Prevent PyObject_Repr from deleting value during
               key format */
            Py_INCREF(pvalue);
            if (any++ > 0) {
                Py_BEGIN_ALLOW_THREADS
                fprintf(fp, ", ");
                Py_END_ALLOW_THREADS
            }
            if (PyObject_Print((PyObject *)ep->me_key, fp, 0)!=0) {
                Py_DECREF(pvalue);
                Py_ReprLeave((PyObject*)mp);
                return -1;
            }
            Py_BEGIN_ALLOW_THREADS
            fprintf(fp, ": ");
            Py_END_ALLOW_THREADS
            if (PyObject_Print(pvalue, fp, 0) != 0) {
                Py_DECREF(pvalue);
                Py_ReprLeave((PyObject*)mp);
                return -1;
            }
            Py_DECREF(pvalue);
        }
    }
    Py_BEGIN_ALLOW_THREADS
    fprintf(fp, "}");
    Py_END_ALLOW_THREADS
    Py_ReprLeave((PyObject*)mp);
    return 0;
}

static PyObject *
dict_repr(PyDictObject *mp)
{
    Py_ssize_t i;
    PyObject *s, *temp, *colon = NULL;
    PyObject *pieces = NULL, *result = NULL;
    PyObject *key, *value;

    i = Py_ReprEnter((PyObject *)mp);
    if (i != 0) {
        return i > 0 ? PyString_FromString("{...}") : NULL;
    }

    if (mp->ma_used == 0) {
        result = PyString_FromString("{}");
        goto Done;
    }

    pieces = PyList_New(0);
    if (pieces == NULL)
        goto Done;

    colon = PyString_FromString(": ");
    if (colon == NULL)
        goto Done;

    /* Do repr() on each key+value pair, and insert ": " between them.
       Note that repr may mutate the dict. */
    i = 0;
    while (PyDict_Next((PyObject *)mp, &i, &key, &value)) {
        int status;
        /* Prevent repr from deleting value during key format. */
        Py_INCREF(value);
        s = PyObject_Repr(key);
        PyString_Concat(&s, colon);
        PyString_ConcatAndDel(&s, PyObject_Repr(value));
        Py_DECREF(value);
        if (s == NULL)
            goto Done;
        status = PyList_Append(pieces, s);
        Py_DECREF(s);  /* append created a new ref */
        if (status < 0)
            goto Done;
    }

    /* Add "{}" decorations to the first and last items. */
    assert(PyList_GET_SIZE(pieces) > 0);
    s = PyString_FromString("{");
    if (s == NULL)
        goto Done;
    temp = PyList_GET_ITEM(pieces, 0);
    PyString_ConcatAndDel(&s, temp);
    PyList_SET_ITEM(pieces, 0, s);
    if (s == NULL)
        goto Done;

    s = PyString_FromString("}");
    if (s == NULL)
        goto Done;
    temp = PyList_GET_ITEM(pieces, PyList_GET_SIZE(pieces) - 1);
    PyString_ConcatAndDel(&temp, s);
    PyList_SET_ITEM(pieces, PyList_GET_SIZE(pieces) - 1, temp);
    if (temp == NULL)
        goto Done;

    /* Paste them all together with ", " between. */
    s = PyString_FromString(", ");
    if (s == NULL)
        goto Done;
    result = _PyString_Join(s, pieces);
    Py_DECREF(s);

Done:
    Py_XDECREF(pieces);
    Py_XDECREF(colon);
    Py_ReprLeave((PyObject *)mp);
    return result;
}

static Py_ssize_t
dict_length(PyDictObject *mp)
{
    return mp->ma_used;
}

static PyObject *
dict_subscript(PyDictObject *mp, register PyObject *key)
{
    PyObject *v;
    long hash;
    PyDictEntry *ep;
    assert(mp->ma_table != NULL);
    if (!PyString_CheckExact(key) ||
        (hash = ((PyStringObject *) key)->ob_shash) == -1) {
        hash = PyObject_Hash(key);
        if (hash == -1)
            return NULL;
    }
    ep = (mp->ma_lookup)(mp, key, hash);
    if (ep == NULL)
        return NULL;
    v = ep->me_value;
    if (v == NULL) {
        if (!PyDict_CheckExact(mp)) {
            /* Look up __missing__ method if we're a subclass. */
            PyObject *missing, *res;
            static PyObject *missing_str = NULL;
            missing = _PyObject_LookupSpecial((PyObject *)mp,
                                              "__missing__",
                                              &missing_str);
            if (missing != NULL) {
                res = PyObject_CallFunctionObjArgs(missing,
                                                   key, NULL);
                Py_DECREF(missing);
                return res;
            }
            else if (PyErr_Occurred())
                return NULL;
        }
        set_key_error(key);
        return NULL;
    }
    else
        Py_INCREF(v);
    return v;
}

static int
dict_ass_sub(PyDictObject *mp, PyObject *v, PyObject *w)
{
    if (w == NULL)
        return PyDict_DelItem((PyObject *)mp, v);
    else
        return PyDict_SetItem((PyObject *)mp, v, w);
}

static PyMappingMethods dict_as_mapping = {
    (lenfunc)dict_length, /*mp_length*/
    (binaryfunc)dict_subscript, /*mp_subscript*/
    (objobjargproc)dict_ass_sub, /*mp_ass_subscript*/
};

static PyObject *
dict_keys(register PyDictObject *mp)
{
    register PyObject *v;
    register Py_ssize_t i, j;
    PyDictEntry *ep;
    Py_ssize_t mask, n;

  again:
    n = mp->ma_used;
    v = PyList_New(n);
    if (v == NULL)
        return NULL;
    if (n != mp->ma_used) {
        /* Durnit.  The allocations caused the dict to resize.
         * Just start over, this shouldn't normally happen.
         */
        Py_DECREF(v);
        goto again;
    }
    ep = mp->ma_table;
    mask = mp->ma_mask;
    for (i = 0, j = 0; i <= mask; i++) {
        if (ep[i].me_value != NULL) {
            PyObject *key = ep[i].me_key;
            Py_INCREF(key);
            PyList_SET_ITEM(v, j, key);
            j++;
        }
    }
    assert(j == n);
    return v;
}

static PyObject *
dict_values(register PyDictObject *mp)
{
    register PyObject *v;
    register Py_ssize_t i, j;
    PyDictEntry *ep;
    Py_ssize_t mask, n;

  again:
    n = mp->ma_used;
    v = PyList_New(n);
    if (v == NULL)
        return NULL;
    if (n != mp->ma_used) {
        /* Durnit.  The allocations caused the dict to resize.
         * Just start over, this shouldn't normally happen.
         */
        Py_DECREF(v);
        goto again;
    }
    ep = mp->ma_table;
    mask = mp->ma_mask;
    for (i = 0, j = 0; i <= mask; i++) {
        if (ep[i].me_value != NULL) {
            PyObject *value = ep[i].me_value;
            Py_INCREF(value);
            PyList_SET_ITEM(v, j, value);
            j++;
        }
    }
    assert(j == n);
    return v;
}

static PyObject *
dict_items(register PyDictObject *mp)
{
    register PyObject *v;
    register Py_ssize_t i, j, n;
    Py_ssize_t mask;
    PyObject *item, *key, *value;
    PyDictEntry *ep;

    /* Preallocate the list of tuples, to avoid allocations during
     * the loop over the items, which could trigger GC, which
     * could resize the dict. :-(
     */
  again:
    n = mp->ma_used;
    v = PyList_New(n);
    if (v == NULL)
        return NULL;
    for (i = 0; i < n; i++) {
        item = PyTuple_New(2);
        if (item == NULL) {
            Py_DECREF(v);
            return NULL;
        }
        PyList_SET_ITEM(v, i, item);
    }
    if (n != mp->ma_used) {
        /* Durnit.  The allocations caused the dict to resize.
         * Just start over, this shouldn't normally happen.
         */
        Py_DECREF(v);
        goto again;
    }
    /* Nothing we do below makes any function calls. */
    ep = mp->ma_table;
    mask = mp->ma_mask;
    for (i = 0, j = 0; i <= mask; i++) {
        if ((value=ep[i].me_value) != NULL) {
            key = ep[i].me_key;
            item = PyList_GET_ITEM(v, j);
            Py_INCREF(key);
            PyTuple_SET_ITEM(item, 0, key);
            Py_INCREF(value);
            PyTuple_SET_ITEM(item, 1, value);
            j++;
        }
    }
    assert(j == n);
    return v;
}

static PyObject *
dict_fromkeys(PyObject *cls, PyObject *args)
{
    PyObject *seq;
    PyObject *value = Py_None;
    PyObject *it;       /* iter(seq) */
    PyObject *key;
    PyObject *d;
    int status;

    if (!PyArg_UnpackTuple(args, "fromkeys", 1, 2, &seq, &value))
        return NULL;

    d = PyObject_CallObject(cls, NULL);
    if (d == NULL)
        return NULL;

    if (PyDict_CheckExact(d) && PyDict_CheckExact(seq)) {
        PyDictObject *mp = (PyDictObject *)d;
        PyObject *oldvalue;
        Py_ssize_t pos = 0;
        PyObject *key;
        long hash;

        if (dictresize(mp, Py_SIZE(seq))) {
            Py_DECREF(d);
            return NULL;
        }

        while (_PyDict_Next(seq, &pos, &key, &oldvalue, &hash)) {
            Py_INCREF(key);
            Py_INCREF(value);
            if (insertdict(mp, key, hash, value)) {
                Py_DECREF(d);
                return NULL;
            }
        }
        return d;
    }

    if (PyDict_CheckExact(d) && PyAnySet_CheckExact(seq)) {
        PyDictObject *mp = (PyDictObject *)d;
        Py_ssize_t pos = 0;
        PyObject *key;
        long hash;

        if (dictresize(mp, PySet_GET_SIZE(seq))) {
            Py_DECREF(d);
            return NULL;
        }

        while (_PySet_NextEntry(seq, &pos, &key, &hash)) {
            Py_INCREF(key);
            Py_INCREF(value);
            if (insertdict(mp, key, hash, value)) {
                Py_DECREF(d);
                return NULL;
            }
        }
        return d;
    }

    it = PyObject_GetIter(seq);
    if (it == NULL){
        Py_DECREF(d);
        return NULL;
    }

    if (PyDict_CheckExact(d)) {
        while ((key = PyIter_Next(it)) != NULL) {
            status = PyDict_SetItem(d, key, value);
            Py_DECREF(key);
            if (status < 0)
                goto Fail;
        }
    } else {
        while ((key = PyIter_Next(it)) != NULL) {
            status = PyObject_SetItem(d, key, value);
            Py_DECREF(key);
            if (status < 0)
                goto Fail;
        }
    }

    if (PyErr_Occurred())
        goto Fail;
    Py_DECREF(it);
    return d;

Fail:
    Py_DECREF(it);
    Py_DECREF(d);
    return NULL;
}

static int
dict_update_common(PyObject *self, PyObject *args, PyObject *kwds, char *methname)
{
    PyObject *arg = NULL;
    int result = 0;

    if (!PyArg_UnpackTuple(args, methname, 0, 1, &arg))
        result = -1;

    else if (arg != NULL) {
        if (PyObject_HasAttrString(arg, "keys"))
            result = PyDict_Merge(self, arg, 1);
        else
            result = PyDict_MergeFromSeq2(self, arg, 1);
    }
    if (result == 0 && kwds != NULL)
        result = PyDict_Merge(self, kwds, 1);
    return result;
}

static PyObject *
dict_update(PyObject *self, PyObject *args, PyObject *kwds)
{
    if (dict_update_common(self, args, kwds, "update") != -1)
        Py_RETURN_NONE;
    return NULL;
}

/* Update unconditionally replaces existing items.
   Merge has a 3rd argument 'override'; if set, it acts like Update,
   otherwise it leaves existing items unchanged.

   PyDict_{Update,Merge} update/merge from a mapping object.

   PyDict_MergeFromSeq2 updates/merges from any iterable object
   producing iterable objects of length 2.
*/

int
PyDict_MergeFromSeq2(PyObject *d, PyObject *seq2, int override)
{
    PyObject *it;       /* iter(seq2) */
    Py_ssize_t i;       /* index into seq2 of current element */
    PyObject *item;     /* seq2[i] */
    PyObject *fast;     /* item as a 2-tuple or 2-list */

    assert(d != NULL);
    assert(PyDict_Check(d));
    assert(seq2 != NULL);

    it = PyObject_GetIter(seq2);
    if (it == NULL)
        return -1;

    for (i = 0; ; ++i) {
        PyObject *key, *value;
        Py_ssize_t n;

        fast = NULL;
        item = PyIter_Next(it);
        if (item == NULL) {
            if (PyErr_Occurred())
                goto Fail;
            break;
        }

        /* Convert item to sequence, and verify length 2. */
        fast = PySequence_Fast(item, "");
        if (fast == NULL) {
            if (PyErr_ExceptionMatches(PyExc_TypeError))
                PyErr_Format(PyExc_TypeError,
                    "cannot convert dictionary update "
                    "sequence element #%zd to a sequence",
                    i);
            goto Fail;
        }
        n = PySequence_Fast_GET_SIZE(fast);
        if (n != 2) {
            PyErr_Format(PyExc_ValueError,
                         "dictionary update sequence element #%zd "
                         "has length %zd; 2 is required",
                         i, n);
            goto Fail;
        }

        /* Update/merge with this (key, value) pair. */
        key = PySequence_Fast_GET_ITEM(fast, 0);
        value = PySequence_Fast_GET_ITEM(fast, 1);
        if (override || PyDict_GetItem(d, key) == NULL) {
            int status = PyDict_SetItem(d, key, value);
            if (status < 0)
                goto Fail;
        }
        Py_DECREF(fast);
        Py_DECREF(item);
    }

    i = 0;
    goto Return;
Fail:
    Py_XDECREF(item);
    Py_XDECREF(fast);
    i = -1;
Return:
    Py_DECREF(it);
    return Py_SAFE_DOWNCAST(i, Py_ssize_t, int);
}

int
PyDict_Update(PyObject *a, PyObject *b)
{
    return PyDict_Merge(a, b, 1);
}

int
PyDict_Merge(PyObject *a, PyObject *b, int override)
{
    register PyDictObject *mp, *other;
    register Py_ssize_t i;
    PyDictEntry *entry;

    /* We accept for the argument either a concrete dictionary object,
     * or an abstract "mapping" object.  For the former, we can do
     * things quite efficiently.  For the latter, we only require that
     * PyMapping_Keys() and PyObject_GetItem() be supported.
     */
    if (a == NULL || !PyDict_Check(a) || b == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }
    mp = (PyDictObject*)a;
    if (PyDict_Check(b)) {
        other = (PyDictObject*)b;
        if (other == mp || other->ma_used == 0)
            /* a.update(a) or a.update({}); nothing to do */
            return 0;
        if (mp->ma_used == 0)
            /* Since the target dict is empty, PyDict_GetItem()
             * always returns NULL.  Setting override to 1
             * skips the unnecessary test.
             */
            override = 1;
        /* Do one big resize at the start, rather than
         * incrementally resizing as we insert new items.  Expect
         * that there will be no (or few) overlapping keys.
         */
        if ((mp->ma_fill + other->ma_used)*3 >= (mp->ma_mask+1)*2) {
           if (dictresize(mp, (mp->ma_used + other->ma_used)*2) != 0)
               return -1;
        }
        for (i = 0; i <= other->ma_mask; i++) {
            entry = &other->ma_table[i];
            if (entry->me_value != NULL &&
                (override ||
                 PyDict_GetItem(a, entry->me_key) == NULL)) {
                Py_INCREF(entry->me_key);
                Py_INCREF(entry->me_value);
                if (insertdict(mp, entry->me_key,
                               (long)entry->me_hash,
                               entry->me_value) != 0)
                    return -1;
            }
        }
    }
    else {
        /* Do it the generic, slower way */
        PyObject *keys = PyMapping_Keys(b);
        PyObject *iter;
        PyObject *key, *value;
        int status;

        if (keys == NULL)
            /* Docstring says this is equivalent to E.keys() so
             * if E doesn't have a .keys() method we want
             * AttributeError to percolate up.  Might as well
             * do the same for any other error.
             */
            return -1;

        iter = PyObject_GetIter(keys);
        Py_DECREF(keys);
        if (iter == NULL)
            return -1;

        for (key = PyIter_Next(iter); key; key = PyIter_Next(iter)) {
            if (!override && PyDict_GetItem(a, key) != NULL) {
                Py_DECREF(key);
                continue;
            }
            value = PyObject_GetItem(b, key);
            if (value == NULL) {
                Py_DECREF(iter);
                Py_DECREF(key);
                return -1;
            }
            status = PyDict_SetItem(a, key, value);
            Py_DECREF(key);
            Py_DECREF(value);
            if (status < 0) {
                Py_DECREF(iter);
                return -1;
            }
        }
        Py_DECREF(iter);
        if (PyErr_Occurred())
            /* Iterator completed, via error */
            return -1;
    }
    return 0;
}

static PyObject *
dict_copy(register PyDictObject *mp)
{
    return PyDict_Copy((PyObject*)mp);
}

PyObject *
PyDict_Copy(PyObject *o)
{
    PyObject *copy;

    if (o == NULL || !PyDict_Check(o)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    copy = PyDict_New();
    if (copy == NULL)
        return NULL;
    if (PyDict_Merge(copy, o, 1) == 0)
        return copy;
    Py_DECREF(copy);
    return NULL;
}

Py_ssize_t
PyDict_Size(PyObject *mp)
{
    if (mp == NULL || !PyDict_Check(mp)) {
        PyErr_BadInternalCall();
        return -1;
    }
    return ((PyDictObject *)mp)->ma_used;
}

PyObject *
PyDict_Keys(PyObject *mp)
{
    if (mp == NULL || !PyDict_Check(mp)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return dict_keys((PyDictObject *)mp);
}

PyObject *
PyDict_Values(PyObject *mp)
{
    if (mp == NULL || !PyDict_Check(mp)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return dict_values((PyDictObject *)mp);
}

PyObject *
PyDict_Items(PyObject *mp)
{
    if (mp == NULL || !PyDict_Check(mp)) {
        PyErr_BadInternalCall();
        return NULL;
    }
    return dict_items((PyDictObject *)mp);
}

/* Subroutine which returns the smallest key in a for which b's value
   is different or absent.  The value is returned too, through the
   pval argument.  Both are NULL if no key in a is found for which b's status
   differs.  The refcounts on (and only on) non-NULL *pval and function return
   values must be decremented by the caller (characterize() increments them
   to ensure that mutating comparison and PyDict_GetItem calls can't delete
   them before the caller is done looking at them). */

static PyObject *
characterize(PyDictObject *a, PyDictObject *b, PyObject **pval)
{
    PyObject *akey = NULL; /* smallest key in a s.t. a[akey] != b[akey] */
    PyObject *aval = NULL; /* a[akey] */
    Py_ssize_t i;
    int cmp;

    for (i = 0; i <= a->ma_mask; i++) {
        PyObject *thiskey, *thisaval, *thisbval;
        if (a->ma_table[i].me_value == NULL)
            continue;
        thiskey = a->ma_table[i].me_key;
        Py_INCREF(thiskey);  /* keep alive across compares */
        if (akey != NULL) {
            cmp = PyObject_RichCompareBool(akey, thiskey, Py_LT);
            if (cmp < 0) {
                Py_DECREF(thiskey);
                goto Fail;
            }
            if (cmp > 0 ||
                i > a->ma_mask ||
                a->ma_table[i].me_value == NULL)
            {
                /* Not the *smallest* a key; or maybe it is
                 * but the compare shrunk the dict so we can't
                 * find its associated value anymore; or
                 * maybe it is but the compare deleted the
                 * a[thiskey] entry.
                 */
                Py_DECREF(thiskey);
                continue;
            }
        }

        /* Compare a[thiskey] to b[thiskey]; cmp <- true iff equal. */
        thisaval = a->ma_table[i].me_value;
        assert(thisaval);
        Py_INCREF(thisaval);   /* keep alive */
        thisbval = PyDict_GetItem((PyObject *)b, thiskey);
        if (thisbval == NULL)
            cmp = 0;
        else {
            /* both dicts have thiskey:  same values? */
            cmp = PyObject_RichCompareBool(
                                    thisaval, thisbval, Py_EQ);
            if (cmp < 0) {
                Py_DECREF(thiskey);
                Py_DECREF(thisaval);
                goto Fail;
            }
        }
        if (cmp == 0) {
            /* New winner. */
            Py_XDECREF(akey);
            Py_XDECREF(aval);
            akey = thiskey;
            aval = thisaval;
        }
        else {
            Py_DECREF(thiskey);
            Py_DECREF(thisaval);
        }
    }
    *pval = aval;
    return akey;

Fail:
    Py_XDECREF(akey);
    Py_XDECREF(aval);
    *pval = NULL;
    return NULL;
}

static int
dict_compare(PyDictObject *a, PyDictObject *b)
{
    PyObject *adiff, *bdiff, *aval, *bval;
    int res;

    /* Compare lengths first */
    if (a->ma_used < b->ma_used)
        return -1;              /* a is shorter */
    else if (a->ma_used > b->ma_used)
        return 1;               /* b is shorter */

    /* Same length -- check all keys */
    bdiff = bval = NULL;
    adiff = characterize(a, b, &aval);
    if (adiff == NULL) {
        assert(!aval);
        /* Either an error, or a is a subset with the same length so
         * must be equal.
         */
        res = PyErr_Occurred() ? -1 : 0;
        goto Finished;
    }
    bdiff = characterize(b, a, &bval);
    if (bdiff == NULL && PyErr_Occurred()) {
        assert(!bval);
        res = -1;
        goto Finished;
    }
    res = 0;
    if (bdiff) {
        /* bdiff == NULL "should be" impossible now, but perhaps
         * the last comparison done by the characterize() on a had
         * the side effect of making the dicts equal!
         */
        res = PyObject_Compare(adiff, bdiff);
    }
    if (res == 0 && bval != NULL)
        res = PyObject_Compare(aval, bval);

Finished:
    Py_XDECREF(adiff);
    Py_XDECREF(bdiff);
    Py_XDECREF(aval);
    Py_XDECREF(bval);
    return res;
}

/* Return 1 if dicts equal, 0 if not, -1 if error.
 * Gets out as soon as any difference is detected.
 * Uses only Py_EQ comparison.
 */
static int
dict_equal(PyDictObject *a, PyDictObject *b)
{
    Py_ssize_t i;

    if (a->ma_used != b->ma_used)
        /* can't be equal if # of entries differ */
        return 0;

    /* Same # of entries -- check all of 'em.  Exit early on any diff. */
    for (i = 0; i <= a->ma_mask; i++) {
        PyObject *aval = a->ma_table[i].me_value;
        if (aval != NULL) {
            int cmp;
            PyObject *bval;
            PyObject *key = a->ma_table[i].me_key;
            /* temporarily bump aval's refcount to ensure it stays
               alive until we're done with it */
            Py_INCREF(aval);
            /* ditto for key */
            Py_INCREF(key);
            bval = PyDict_GetItem((PyObject *)b, key);
            Py_DECREF(key);
            if (bval == NULL) {
                Py_DECREF(aval);
                return 0;
            }
            cmp = PyObject_RichCompareBool(aval, bval, Py_EQ);
            Py_DECREF(aval);
            if (cmp <= 0)  /* error or not equal */
                return cmp;
        }
    }
    return 1;
 }

static PyObject *
dict_richcompare(PyObject *v, PyObject *w, int op)
{
    int cmp;
    PyObject *res;

    if (!PyDict_Check(v) || !PyDict_Check(w)) {
        res = Py_NotImplemented;
    }
    else if (op == Py_EQ || op == Py_NE) {
        cmp = dict_equal((PyDictObject *)v, (PyDictObject *)w);
        if (cmp < 0)
            return NULL;
        res = (cmp == (op == Py_EQ)) ? Py_True : Py_False;
    }
    else {
        /* Py3K warning if comparison isn't == or !=  */
        if (PyErr_WarnPy3k("dict inequality comparisons not supported "
                           "in 3.x", 1) < 0) {
            return NULL;
        }
        res = Py_NotImplemented;
    }
    Py_INCREF(res);
    return res;
 }

static PyObject *
dict_contains(register PyDictObject *mp, PyObject *key)
{
    long hash;
    PyDictEntry *ep;

    if (!PyString_CheckExact(key) ||
        (hash = ((PyStringObject *) key)->ob_shash) == -1) {
        hash = PyObject_Hash(key);
        if (hash == -1)
            return NULL;
    }
    ep = (mp->ma_lookup)(mp, key, hash);
    if (ep == NULL)
        return NULL;
    return PyBool_FromLong(ep->me_value != NULL);
}

static PyObject *
dict_has_key(register PyDictObject *mp, PyObject *key)
{
    if (PyErr_WarnPy3k("dict.has_key() not supported in 3.x; "
                       "use the in operator", 1) < 0)
        return NULL;
    return dict_contains(mp, key);
}

static PyObject *
dict_get(register PyDictObject *mp, PyObject *args)
{
    PyObject *key;
    PyObject *failobj = Py_None;
    PyObject *val = NULL;
    long hash;
    PyDictEntry *ep;

    if (!PyArg_UnpackTuple(args, "get", 1, 2, &key, &failobj))
        return NULL;

    if (!PyString_CheckExact(key) ||
        (hash = ((PyStringObject *) key)->ob_shash) == -1) {
        hash = PyObject_Hash(key);
        if (hash == -1)
            return NULL;
    }
    ep = (mp->ma_lookup)(mp, key, hash);
    if (ep == NULL)
        return NULL;
    val = ep->me_value;
    if (val == NULL)
        val = failobj;
    Py_INCREF(val);
    return val;
}


static PyObject *
dict_setdefault(register PyDictObject *mp, PyObject *args)
{
    PyObject *key;
    PyObject *failobj = Py_None;
    PyObject *val = NULL;
    long hash;
    PyDictEntry *ep;

    if (!PyArg_UnpackTuple(args, "setdefault", 1, 2, &key, &failobj))
        return NULL;

    if (!PyString_CheckExact(key) ||
        (hash = ((PyStringObject *) key)->ob_shash) == -1) {
        hash = PyObject_Hash(key);
        if (hash == -1)
            return NULL;
    }
    ep = (mp->ma_lookup)(mp, key, hash);
    if (ep == NULL)
        return NULL;
    val = ep->me_value;
    if (val == NULL) {
        val = failobj;
        if (PyDict_SetItem((PyObject*)mp, key, failobj))
            val = NULL;
    }
    Py_XINCREF(val);
    return val;
}


static PyObject *
dict_clear(register PyDictObject *mp)
{
    PyDict_Clear((PyObject *)mp);
    Py_RETURN_NONE;
}

static PyObject *
dict_pop(PyDictObject *mp, PyObject *args)
{
    long hash;
    PyDictEntry *ep;
    PyObject *old_value, *old_key;
    PyObject *key, *deflt = NULL;

    if(!PyArg_UnpackTuple(args, "pop", 1, 2, &key, &deflt))
        return NULL;
    if (mp->ma_used == 0) {
        if (deflt) {
            Py_INCREF(deflt);
            return deflt;
        }
        set_key_error(key);
        return NULL;
    }
    if (!PyString_CheckExact(key) ||
        (hash = ((PyStringObject *) key)->ob_shash) == -1) {
        hash = PyObject_Hash(key);
        if (hash == -1)
            return NULL;
    }
    ep = (mp->ma_lookup)(mp, key, hash);
    if (ep == NULL)
        return NULL;
    if (ep->me_value == NULL) {
        if (deflt) {
            Py_INCREF(deflt);
            return deflt;
        }
        set_key_error(key);
        return NULL;
    }
    old_key = ep->me_key;
    Py_INCREF(dummy);
    ep->me_key = dummy;
    old_value = ep->me_value;
    ep->me_value = NULL;
    mp->ma_used--;
    Py_DECREF(old_key);
    return old_value;
}

static PyObject *
dict_popitem(PyDictObject *mp)
{
    Py_ssize_t i = 0;
    PyDictEntry *ep;
    PyObject *res;

    /* Allocate the result tuple before checking the size.  Believe it
     * or not, this allocation could trigger a garbage collection which
     * could empty the dict, so if we checked the size first and that
     * happened, the result would be an infinite loop (searching for an
     * entry that no longer exists).  Note that the usual popitem()
     * idiom is "while d: k, v = d.popitem()". so needing to throw the
     * tuple away if the dict *is* empty isn't a significant
     * inefficiency -- possible, but unlikely in practice.
     */
    res = PyTuple_New(2);
    if (res == NULL)
        return NULL;
    if (mp->ma_used == 0) {
        Py_DECREF(res);
        PyErr_SetString(PyExc_KeyError,
                        "popitem(): dictionary is empty");
        return NULL;
    }
    /* Set ep to "the first" dict entry with a value.  We abuse the hash
     * field of slot 0 to hold a search finger:
     * If slot 0 has a value, use slot 0.
     * Else slot 0 is being used to hold a search finger,
     * and we use its hash value as the first index to look.
     */
    ep = &mp->ma_table[0];
    if (ep->me_value == NULL) {
        i = ep->me_hash;
        /* The hash field may be a real hash value, or it may be a
         * legit search finger, or it may be a once-legit search
         * finger that's out of bounds now because it wrapped around
         * or the table shrunk -- simply make sure it's in bounds now.
         */
        if (i > mp->ma_mask || i < 1)
            i = 1;              /* skip slot 0 */
        while ((ep = &mp->ma_table[i])->me_value == NULL) {
            i++;
            if (i > mp->ma_mask)
                i = 1;
        }
    }
    PyTuple_SET_ITEM(res, 0, ep->me_key);
    PyTuple_SET_ITEM(res, 1, ep->me_value);
    Py_INCREF(dummy);
    ep->me_key = dummy;
    ep->me_value = NULL;
    mp->ma_used--;
    assert(mp->ma_table[0].me_value == NULL);
    mp->ma_table[0].me_hash = i + 1;  /* next place to start */
    return res;
}

static int
dict_traverse(PyObject *op, visitproc visit, void *arg)
{
    Py_ssize_t i = 0;

    PyObject *pk;
    PyObject *pv;

    while (PyDict_Next(op, &i, &pk, &pv)) {
        Py_VISIT(pk);
        Py_VISIT(pv);
    }
    return 0;
}

static int
dict_tp_clear(PyObject *op)
{
    PyDict_Clear(op);
    return 0;
}


extern PyTypeObject PyDictIterKey_Type; /* Forward */
extern PyTypeObject PyDictIterValue_Type; /* Forward */
extern PyTypeObject PyDictIterItem_Type; /* Forward */
static PyObject *dictiter_new(PyDictObject *, PyTypeObject *);

static PyObject *
dict_iterkeys(PyDictObject *dict)
{
    return dictiter_new(dict, &PyDictIterKey_Type);
}

static PyObject *
dict_itervalues(PyDictObject *dict)
{
    return dictiter_new(dict, &PyDictIterValue_Type);
}

static PyObject *
dict_iteritems(PyDictObject *dict)
{
    return dictiter_new(dict, &PyDictIterItem_Type);
}

static PyObject *
dict_sizeof(PyDictObject *mp)
{
    Py_ssize_t res;

    res = sizeof(PyDictObject);
    if (mp->ma_table != mp->ma_smalltable)
        res = res + (mp->ma_mask + 1) * sizeof(PyDictEntry);
    return PyInt_FromSsize_t(res);
}

PyDoc_STRVAR(has_key__doc__,
"D.has_key(k) -> True if D has a key k, else False");

PyDoc_STRVAR(contains__doc__,
"D.__contains__(k) -> True if D has a key k, else False");

PyDoc_STRVAR(getitem__doc__, "x.__getitem__(y) <==> x[y]");

PyDoc_STRVAR(sizeof__doc__,
"D.__sizeof__() -> size of D in memory, in bytes");

PyDoc_STRVAR(get__doc__,
"D.get(k[,d]) -> D[k] if k in D, else d.  d defaults to None.");

PyDoc_STRVAR(setdefault_doc__,
"D.setdefault(k[,d]) -> D.get(k,d), also set D[k]=d if k not in D");

PyDoc_STRVAR(pop__doc__,
"D.pop(k[,d]) -> v, remove specified key and return the corresponding value.\n\
If key is not found, d is returned if given, otherwise KeyError is raised");

PyDoc_STRVAR(popitem__doc__,
"D.popitem() -> (k, v), remove and return some (key, value) pair as a\n\
2-tuple; but raise KeyError if D is empty.");

PyDoc_STRVAR(keys__doc__,
"D.keys() -> list of D's keys");

PyDoc_STRVAR(items__doc__,
"D.items() -> list of D's (key, value) pairs, as 2-tuples");

PyDoc_STRVAR(values__doc__,
"D.values() -> list of D's values");

PyDoc_STRVAR(update__doc__,
"D.update([E, ]**F) -> None.  Update D from dict/iterable E and F.\n"
"If E present and has a .keys() method, does:     for k in E: D[k] = E[k]\n\
If E present and lacks .keys() method, does:     for (k, v) in E: D[k] = v\n\
In either case, this is followed by: for k in F: D[k] = F[k]");

PyDoc_STRVAR(fromkeys__doc__,
"dict.fromkeys(S[,v]) -> New dict with keys from S and values equal to v.\n\
v defaults to None.");

PyDoc_STRVAR(clear__doc__,
"D.clear() -> None.  Remove all items from D.");

PyDoc_STRVAR(copy__doc__,
"D.copy() -> a shallow copy of D");

PyDoc_STRVAR(iterkeys__doc__,
"D.iterkeys() -> an iterator over the keys of D");

PyDoc_STRVAR(itervalues__doc__,
"D.itervalues() -> an iterator over the values of D");

PyDoc_STRVAR(iteritems__doc__,
"D.iteritems() -> an iterator over the (key, value) items of D");

/* Forward */
static PyObject *dictkeys_new(PyObject *);
static PyObject *dictitems_new(PyObject *);
static PyObject *dictvalues_new(PyObject *);

PyDoc_STRVAR(viewkeys__doc__,
             "D.viewkeys() -> a set-like object providing a view on D's keys");
PyDoc_STRVAR(viewitems__doc__,
             "D.viewitems() -> a set-like object providing a view on D's items");
PyDoc_STRVAR(viewvalues__doc__,
             "D.viewvalues() -> an object providing a view on D's values");

static PyMethodDef mapp_methods[] = {
    {"__contains__",(PyCFunction)dict_contains,         METH_O | METH_COEXIST,
     contains__doc__},
    {"__getitem__", (PyCFunction)dict_subscript,        METH_O | METH_COEXIST,
     getitem__doc__},
    {"__sizeof__",      (PyCFunction)dict_sizeof,       METH_NOARGS,
     sizeof__doc__},
    {"has_key",         (PyCFunction)dict_has_key,      METH_O,
     has_key__doc__},
    {"get",         (PyCFunction)dict_get,          METH_VARARGS,
     get__doc__},
    {"setdefault",  (PyCFunction)dict_setdefault,   METH_VARARGS,
     setdefault_doc__},
    {"pop",         (PyCFunction)dict_pop,          METH_VARARGS,
     pop__doc__},
    {"popitem",         (PyCFunction)dict_popitem,      METH_NOARGS,
     popitem__doc__},
    {"keys",            (PyCFunction)dict_keys,         METH_NOARGS,
    keys__doc__},
    {"items",           (PyCFunction)dict_items,        METH_NOARGS,
     items__doc__},
    {"values",          (PyCFunction)dict_values,       METH_NOARGS,
     values__doc__},
    {"viewkeys",        (PyCFunction)dictkeys_new,      METH_NOARGS,
     viewkeys__doc__},
    {"viewitems",       (PyCFunction)dictitems_new,     METH_NOARGS,
     viewitems__doc__},
    {"viewvalues",      (PyCFunction)dictvalues_new,    METH_NOARGS,
     viewvalues__doc__},
    {"update",          (PyCFunction)dict_update,       METH_VARARGS | METH_KEYWORDS,
     update__doc__},
    {"fromkeys",        (PyCFunction)dict_fromkeys,     METH_VARARGS | METH_CLASS,
     fromkeys__doc__},
    {"clear",           (PyCFunction)dict_clear,        METH_NOARGS,
     clear__doc__},
    {"copy",            (PyCFunction)dict_copy,         METH_NOARGS,
     copy__doc__},
    {"iterkeys",        (PyCFunction)dict_iterkeys,     METH_NOARGS,
     iterkeys__doc__},
    {"itervalues",      (PyCFunction)dict_itervalues,   METH_NOARGS,
     itervalues__doc__},
    {"iteritems",       (PyCFunction)dict_iteritems,    METH_NOARGS,
     iteritems__doc__},
    {NULL,              NULL}   /* sentinel */
};

/* Return 1 if `key` is in dict `op`, 0 if not, and -1 on error. */
int
PyDict_Contains(PyObject *op, PyObject *key)
{
    long hash;
    PyDictObject *mp = (PyDictObject *)op;
    PyDictEntry *ep;

    if (!PyString_CheckExact(key) ||
        (hash = ((PyStringObject *) key)->ob_shash) == -1) {
        hash = PyObject_Hash(key);
        if (hash == -1)
            return -1;
    }
    ep = (mp->ma_lookup)(mp, key, hash);
    return ep == NULL ? -1 : (ep->me_value != NULL);
}

/* Internal version of PyDict_Contains used when the hash value is already known */
int
_PyDict_Contains(PyObject *op, PyObject *key, long hash)
{
    PyDictObject *mp = (PyDictObject *)op;
    PyDictEntry *ep;

    ep = (mp->ma_lookup)(mp, key, hash);
    return ep == NULL ? -1 : (ep->me_value != NULL);
}

/* Hack to implement "key in dict" */
static PySequenceMethods dict_as_sequence = {
    0,                          /* sq_length */
    0,                          /* sq_concat */
    0,                          /* sq_repeat */
    0,                          /* sq_item */
    0,                          /* sq_slice */
    0,                          /* sq_ass_item */
    0,                          /* sq_ass_slice */
    PyDict_Contains,            /* sq_contains */
    0,                          /* sq_inplace_concat */
    0,                          /* sq_inplace_repeat */
};

static PyObject *
dict_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *self;

    assert(type != NULL && type->tp_alloc != NULL);
    self = type->tp_alloc(type, 0);
    if (self != NULL) {
        PyDictObject *d = (PyDictObject *)self;
        /* It's guaranteed that tp->alloc zeroed out the struct. */
        assert(d->ma_table == NULL && d->ma_fill == 0 && d->ma_used == 0);
        INIT_NONZERO_DICT_SLOTS(d);
        d->ma_lookup = lookdict_string;
        /* The object has been implicitly tracked by tp_alloc */
        if (type == &PyDict_Type)
            _PyObject_GC_UNTRACK(d);
#ifdef SHOW_CONVERSION_COUNTS
        ++created;
#endif
#ifdef SHOW_TRACK_COUNT
        if (_PyObject_GC_IS_TRACKED(d))
            count_tracked++;
        else
            count_untracked++;
#endif
    }
    return self;
}

static int
dict_init(PyObject *self, PyObject *args, PyObject *kwds)
{
    return dict_update_common(self, args, kwds, "dict");
}

static PyObject *
dict_iter(PyDictObject *dict)
{
    return dictiter_new(dict, &PyDictIterKey_Type);
}

PyDoc_STRVAR(dictionary_doc,
"dict() -> new empty dictionary\n"
"dict(mapping) -> new dictionary initialized from a mapping object's\n"
"    (key, value) pairs\n"
"dict(iterable) -> new dictionary initialized as if via:\n"
"    d = {}\n"
"    for k, v in iterable:\n"
"        d[k] = v\n"
"dict(**kwargs) -> new dictionary initialized with the name=value pairs\n"
"    in the keyword argument list.  For example:  dict(one=1, two=2)");

PyTypeObject PyDict_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "dict",
    sizeof(PyDictObject),
    0,
    (destructor)dict_dealloc,                   /* tp_dealloc */
    (printfunc)dict_print,                      /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    (cmpfunc)dict_compare,                      /* tp_compare */
    (reprfunc)dict_repr,                        /* tp_repr */
    0,                                          /* tp_as_number */
    &dict_as_sequence,                          /* tp_as_sequence */
    &dict_as_mapping,                           /* tp_as_mapping */
    (hashfunc)PyObject_HashNotImplemented,      /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Py_TPFLAGS_BASETYPE | Py_TPFLAGS_DICT_SUBCLASS,         /* tp_flags */
    dictionary_doc,                             /* tp_doc */
    dict_traverse,                              /* tp_traverse */
    dict_tp_clear,                              /* tp_clear */
    dict_richcompare,                           /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    (getiterfunc)dict_iter,                     /* tp_iter */
    0,                                          /* tp_iternext */
    mapp_methods,                               /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    dict_init,                                  /* tp_init */
    PyType_GenericAlloc,                        /* tp_alloc */
    dict_new,                                   /* tp_new */
    PyObject_GC_Del,                            /* tp_free */
};

/* For backward compatibility with old dictionary interface */

PyObject *
PyDict_GetItemString(PyObject *v, const char *key)
{
    PyObject *kv, *rv;
    kv = PyString_FromString(key);
    if (kv == NULL)
        return NULL;
    rv = PyDict_GetItem(v, kv);
    Py_DECREF(kv);
    return rv;
}

int
PyDict_SetItemString(PyObject *v, const char *key, PyObject *item)
{
    PyObject *kv;
    int err;
    kv = PyString_FromString(key);
    if (kv == NULL)
        return -1;
    PyString_InternInPlace(&kv); /* XXX Should we really? */
    err = PyDict_SetItem(v, kv, item);
    Py_DECREF(kv);
    return err;
}

int
PyDict_DelItemString(PyObject *v, const char *key)
{
    PyObject *kv;
    int err;
    kv = PyString_FromString(key);
    if (kv == NULL)
        return -1;
    err = PyDict_DelItem(v, kv);
    Py_DECREF(kv);
    return err;
}

/* Dictionary iterator types */

typedef struct {
    PyObject_HEAD
    PyDictObject *di_dict; /* Set to NULL when iterator is exhausted */
    Py_ssize_t di_used;
    Py_ssize_t di_pos;
    PyObject* di_result; /* reusable result tuple for iteritems */
    Py_ssize_t len;
} dictiterobject;

static PyObject *
dictiter_new(PyDictObject *dict, PyTypeObject *itertype)
{
    dictiterobject *di;
    di = PyObject_GC_New(dictiterobject, itertype);
    if (di == NULL)
        return NULL;
    Py_INCREF(dict);
    di->di_dict = dict;
    di->di_used = dict->ma_used;
    di->di_pos = 0;
    di->len = dict->ma_used;
    if (itertype == &PyDictIterItem_Type) {
        di->di_result = PyTuple_Pack(2, Py_None, Py_None);
        if (di->di_result == NULL) {
            Py_DECREF(di);
            return NULL;
        }
    }
    else
        di->di_result = NULL;
    _PyObject_GC_TRACK(di);
    return (PyObject *)di;
}

static void
dictiter_dealloc(dictiterobject *di)
{
    Py_XDECREF(di->di_dict);
    Py_XDECREF(di->di_result);
    PyObject_GC_Del(di);
}

static int
dictiter_traverse(dictiterobject *di, visitproc visit, void *arg)
{
    Py_VISIT(di->di_dict);
    Py_VISIT(di->di_result);
    return 0;
}

static PyObject *
dictiter_len(dictiterobject *di)
{
    Py_ssize_t len = 0;
    if (di->di_dict != NULL && di->di_used == di->di_dict->ma_used)
        len = di->len;
    return PyInt_FromSize_t(len);
}

PyDoc_STRVAR(length_hint_doc, "Private method returning an estimate of len(list(it)).");

static PyMethodDef dictiter_methods[] = {
    {"__length_hint__", (PyCFunction)dictiter_len, METH_NOARGS, length_hint_doc},
    {NULL,              NULL}           /* sentinel */
};

static PyObject *dictiter_iternextkey(dictiterobject *di)
{
    PyObject *key;
    register Py_ssize_t i, mask;
    register PyDictEntry *ep;
    PyDictObject *d = di->di_dict;

    if (d == NULL)
        return NULL;
    assert (PyDict_Check(d));

    if (di->di_used != d->ma_used) {
        PyErr_SetString(PyExc_RuntimeError,
                        "dictionary changed size during iteration");
        di->di_used = -1; /* Make this state sticky */
        return NULL;
    }

    i = di->di_pos;
    if (i < 0)
        goto fail;
    ep = d->ma_table;
    mask = d->ma_mask;
    while (i <= mask && ep[i].me_value == NULL)
        i++;
    di->di_pos = i+1;
    if (i > mask)
        goto fail;
    di->len--;
    key = ep[i].me_key;
    Py_INCREF(key);
    return key;

fail:
    Py_DECREF(d);
    di->di_dict = NULL;
    return NULL;
}

PyTypeObject PyDictIterKey_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "dictionary-keyiterator",                   /* tp_name */
    sizeof(dictiterobject),                     /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictiter_dealloc,               /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictiter_traverse,            /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    (iternextfunc)dictiter_iternextkey,         /* tp_iternext */
    dictiter_methods,                           /* tp_methods */
    0,
};

static PyObject *dictiter_iternextvalue(dictiterobject *di)
{
    PyObject *value;
    register Py_ssize_t i, mask;
    register PyDictEntry *ep;
    PyDictObject *d = di->di_dict;

    if (d == NULL)
        return NULL;
    assert (PyDict_Check(d));

    if (di->di_used != d->ma_used) {
        PyErr_SetString(PyExc_RuntimeError,
                        "dictionary changed size during iteration");
        di->di_used = -1; /* Make this state sticky */
        return NULL;
    }

    i = di->di_pos;
    mask = d->ma_mask;
    if (i < 0 || i > mask)
        goto fail;
    ep = d->ma_table;
    while ((value=ep[i].me_value) == NULL) {
        i++;
        if (i > mask)
            goto fail;
    }
    di->di_pos = i+1;
    di->len--;
    Py_INCREF(value);
    return value;

fail:
    Py_DECREF(d);
    di->di_dict = NULL;
    return NULL;
}

PyTypeObject PyDictIterValue_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "dictionary-valueiterator",                 /* tp_name */
    sizeof(dictiterobject),                     /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictiter_dealloc,               /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictiter_traverse,            /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    (iternextfunc)dictiter_iternextvalue,       /* tp_iternext */
    dictiter_methods,                           /* tp_methods */
    0,
};

static PyObject *dictiter_iternextitem(dictiterobject *di)
{
    PyObject *key, *value, *result = di->di_result;
    register Py_ssize_t i, mask;
    register PyDictEntry *ep;
    PyDictObject *d = di->di_dict;

    if (d == NULL)
        return NULL;
    assert (PyDict_Check(d));

    if (di->di_used != d->ma_used) {
        PyErr_SetString(PyExc_RuntimeError,
                        "dictionary changed size during iteration");
        di->di_used = -1; /* Make this state sticky */
        return NULL;
    }

    i = di->di_pos;
    if (i < 0)
        goto fail;
    ep = d->ma_table;
    mask = d->ma_mask;
    while (i <= mask && ep[i].me_value == NULL)
        i++;
    di->di_pos = i+1;
    if (i > mask)
        goto fail;

    if (result->ob_refcnt == 1) {
        Py_INCREF(result);
        Py_DECREF(PyTuple_GET_ITEM(result, 0));
        Py_DECREF(PyTuple_GET_ITEM(result, 1));
    } else {
        result = PyTuple_New(2);
        if (result == NULL)
            return NULL;
    }
    di->len--;
    key = ep[i].me_key;
    value = ep[i].me_value;
    Py_INCREF(key);
    Py_INCREF(value);
    PyTuple_SET_ITEM(result, 0, key);
    PyTuple_SET_ITEM(result, 1, value);
    return result;

fail:
    Py_DECREF(d);
    di->di_dict = NULL;
    return NULL;
}

PyTypeObject PyDictIterItem_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "dictionary-itemiterator",                  /* tp_name */
    sizeof(dictiterobject),                     /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictiter_dealloc,               /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    0,                                          /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictiter_traverse,            /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    PyObject_SelfIter,                          /* tp_iter */
    (iternextfunc)dictiter_iternextitem,        /* tp_iternext */
    dictiter_methods,                           /* tp_methods */
    0,
};

/***********************************************/
/* View objects for keys(), items(), values(). */
/***********************************************/

/* The instance lay-out is the same for all three; but the type differs. */

typedef struct {
    PyObject_HEAD
    PyDictObject *dv_dict;
} dictviewobject;


static void
dictview_dealloc(dictviewobject *dv)
{
    Py_XDECREF(dv->dv_dict);
    PyObject_GC_Del(dv);
}

static int
dictview_traverse(dictviewobject *dv, visitproc visit, void *arg)
{
    Py_VISIT(dv->dv_dict);
    return 0;
}

static Py_ssize_t
dictview_len(dictviewobject *dv)
{
    Py_ssize_t len = 0;
    if (dv->dv_dict != NULL)
        len = dv->dv_dict->ma_used;
    return len;
}

static PyObject *
dictview_new(PyObject *dict, PyTypeObject *type)
{
    dictviewobject *dv;
    if (dict == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }
    if (!PyDict_Check(dict)) {
        /* XXX Get rid of this restriction later */
        PyErr_Format(PyExc_TypeError,
                     "%s() requires a dict argument, not '%s'",
                     type->tp_name, dict->ob_type->tp_name);
        return NULL;
    }
    dv = PyObject_GC_New(dictviewobject, type);
    if (dv == NULL)
        return NULL;
    Py_INCREF(dict);
    dv->dv_dict = (PyDictObject *)dict;
    _PyObject_GC_TRACK(dv);
    return (PyObject *)dv;
}

/* TODO(guido): The views objects are not complete:

 * support more set operations
 * support arbitrary mappings?
   - either these should be static or exported in dictobject.h
   - if public then they should probably be in builtins
*/

/* Return 1 if self is a subset of other, iterating over self;
   0 if not; -1 if an error occurred. */
static int
all_contained_in(PyObject *self, PyObject *other)
{
    PyObject *iter = PyObject_GetIter(self);
    int ok = 1;

    if (iter == NULL)
        return -1;
    for (;;) {
        PyObject *next = PyIter_Next(iter);
        if (next == NULL) {
            if (PyErr_Occurred())
                ok = -1;
            break;
        }
        ok = PySequence_Contains(other, next);
        Py_DECREF(next);
        if (ok <= 0)
            break;
    }
    Py_DECREF(iter);
    return ok;
}

static PyObject *
dictview_richcompare(PyObject *self, PyObject *other, int op)
{
    Py_ssize_t len_self, len_other;
    int ok;
    PyObject *result;

    assert(self != NULL);
    assert(PyDictViewSet_Check(self));
    assert(other != NULL);

    if (!PyAnySet_Check(other) && !PyDictViewSet_Check(other)) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }

    len_self = PyObject_Size(self);
    if (len_self < 0)
        return NULL;
    len_other = PyObject_Size(other);
    if (len_other < 0)
        return NULL;

    ok = 0;
    switch(op) {

    case Py_NE:
    case Py_EQ:
        if (len_self == len_other)
            ok = all_contained_in(self, other);
        if (op == Py_NE && ok >= 0)
            ok = !ok;
        break;

    case Py_LT:
        if (len_self < len_other)
            ok = all_contained_in(self, other);
        break;

      case Py_LE:
          if (len_self <= len_other)
              ok = all_contained_in(self, other);
          break;

    case Py_GT:
        if (len_self > len_other)
            ok = all_contained_in(other, self);
        break;

    case Py_GE:
        if (len_self >= len_other)
            ok = all_contained_in(other, self);
        break;

    }
    if (ok < 0)
        return NULL;
    result = ok ? Py_True : Py_False;
    Py_INCREF(result);
    return result;
}

static PyObject *
dictview_repr(dictviewobject *dv)
{
    PyObject *seq;
    PyObject *seq_str;
    PyObject *result;

    seq = PySequence_List((PyObject *)dv);
    if (seq == NULL)
        return NULL;

    seq_str = PyObject_Repr(seq);
    result = PyString_FromFormat("%s(%s)", Py_TYPE(dv)->tp_name,
                                 PyString_AS_STRING(seq_str));
    Py_DECREF(seq_str);
    Py_DECREF(seq);
    return result;
}

/*** dict_keys ***/

static PyObject *
dictkeys_iter(dictviewobject *dv)
{
    if (dv->dv_dict == NULL) {
        Py_RETURN_NONE;
    }
    return dictiter_new(dv->dv_dict, &PyDictIterKey_Type);
}

static int
dictkeys_contains(dictviewobject *dv, PyObject *obj)
{
    if (dv->dv_dict == NULL)
        return 0;
    return PyDict_Contains((PyObject *)dv->dv_dict, obj);
}

static PySequenceMethods dictkeys_as_sequence = {
    (lenfunc)dictview_len,              /* sq_length */
    0,                                  /* sq_concat */
    0,                                  /* sq_repeat */
    0,                                  /* sq_item */
    0,                                  /* sq_slice */
    0,                                  /* sq_ass_item */
    0,                                  /* sq_ass_slice */
    (objobjproc)dictkeys_contains,      /* sq_contains */
};

static PyObject*
dictviews_sub(PyObject* self, PyObject *other)
{
    PyObject *result = PySet_New(self);
    PyObject *tmp;
    if (result == NULL)
        return NULL;

    tmp = PyObject_CallMethod(result, "difference_update", "O", other);
    if (tmp == NULL) {
        Py_DECREF(result);
        return NULL;
    }

    Py_DECREF(tmp);
    return result;
}

static PyObject*
dictviews_and(PyObject* self, PyObject *other)
{
    PyObject *result = PySet_New(self);
    PyObject *tmp;
    if (result == NULL)
        return NULL;

    tmp = PyObject_CallMethod(result, "intersection_update", "O", other);
    if (tmp == NULL) {
        Py_DECREF(result);
        return NULL;
    }

    Py_DECREF(tmp);
    return result;
}

static PyObject*
dictviews_or(PyObject* self, PyObject *other)
{
    PyObject *result = PySet_New(self);
    PyObject *tmp;
    if (result == NULL)
        return NULL;

    tmp = PyObject_CallMethod(result, "update", "O", other);
    if (tmp == NULL) {
        Py_DECREF(result);
        return NULL;
    }

    Py_DECREF(tmp);
    return result;
}

static PyObject*
dictviews_xor(PyObject* self, PyObject *other)
{
    PyObject *result = PySet_New(self);
    PyObject *tmp;
    if (result == NULL)
        return NULL;

    tmp = PyObject_CallMethod(result, "symmetric_difference_update", "O",
                              other);
    if (tmp == NULL) {
        Py_DECREF(result);
        return NULL;
    }

    Py_DECREF(tmp);
    return result;
}

static PyNumberMethods dictviews_as_number = {
    0,                                  /*nb_add*/
    (binaryfunc)dictviews_sub,          /*nb_subtract*/
    0,                                  /*nb_multiply*/
    0,                                  /*nb_divide*/
    0,                                  /*nb_remainder*/
    0,                                  /*nb_divmod*/
    0,                                  /*nb_power*/
    0,                                  /*nb_negative*/
    0,                                  /*nb_positive*/
    0,                                  /*nb_absolute*/
    0,                                  /*nb_nonzero*/
    0,                                  /*nb_invert*/
    0,                                  /*nb_lshift*/
    0,                                  /*nb_rshift*/
    (binaryfunc)dictviews_and,          /*nb_and*/
    (binaryfunc)dictviews_xor,          /*nb_xor*/
    (binaryfunc)dictviews_or,           /*nb_or*/
};

static PyMethodDef dictkeys_methods[] = {
    {NULL,              NULL}           /* sentinel */
};

PyTypeObject PyDictKeys_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "dict_keys",                                /* tp_name */
    sizeof(dictviewobject),                     /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictview_dealloc,               /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_reserved */
    (reprfunc)dictview_repr,                    /* tp_repr */
    &dictviews_as_number,                       /* tp_as_number */
    &dictkeys_as_sequence,                      /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Py_TPFLAGS_CHECKTYPES,                  /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictview_traverse,            /* tp_traverse */
    0,                                          /* tp_clear */
    dictview_richcompare,                       /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    (getiterfunc)dictkeys_iter,                 /* tp_iter */
    0,                                          /* tp_iternext */
    dictkeys_methods,                           /* tp_methods */
    0,
};

static PyObject *
dictkeys_new(PyObject *dict)
{
    return dictview_new(dict, &PyDictKeys_Type);
}

/*** dict_items ***/

static PyObject *
dictitems_iter(dictviewobject *dv)
{
    if (dv->dv_dict == NULL) {
        Py_RETURN_NONE;
    }
    return dictiter_new(dv->dv_dict, &PyDictIterItem_Type);
}

static int
dictitems_contains(dictviewobject *dv, PyObject *obj)
{
    PyObject *key, *value, *found;
    if (dv->dv_dict == NULL)
        return 0;
    if (!PyTuple_Check(obj) || PyTuple_GET_SIZE(obj) != 2)
        return 0;
    key = PyTuple_GET_ITEM(obj, 0);
    value = PyTuple_GET_ITEM(obj, 1);
    found = PyDict_GetItem((PyObject *)dv->dv_dict, key);
    if (found == NULL) {
        if (PyErr_Occurred())
            return -1;
        return 0;
    }
    return PyObject_RichCompareBool(value, found, Py_EQ);
}

static PySequenceMethods dictitems_as_sequence = {
    (lenfunc)dictview_len,              /* sq_length */
    0,                                  /* sq_concat */
    0,                                  /* sq_repeat */
    0,                                  /* sq_item */
    0,                                  /* sq_slice */
    0,                                  /* sq_ass_item */
    0,                                  /* sq_ass_slice */
    (objobjproc)dictitems_contains,     /* sq_contains */
};

static PyMethodDef dictitems_methods[] = {
    {NULL,              NULL}           /* sentinel */
};

PyTypeObject PyDictItems_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "dict_items",                               /* tp_name */
    sizeof(dictviewobject),                     /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictview_dealloc,               /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_reserved */
    (reprfunc)dictview_repr,                    /* tp_repr */
    &dictviews_as_number,                       /* tp_as_number */
    &dictitems_as_sequence,                     /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Py_TPFLAGS_CHECKTYPES,                  /* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictview_traverse,            /* tp_traverse */
    0,                                          /* tp_clear */
    dictview_richcompare,                       /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    (getiterfunc)dictitems_iter,                /* tp_iter */
    0,                                          /* tp_iternext */
    dictitems_methods,                          /* tp_methods */
    0,
};

static PyObject *
dictitems_new(PyObject *dict)
{
    return dictview_new(dict, &PyDictItems_Type);
}

/*** dict_values ***/

static PyObject *
dictvalues_iter(dictviewobject *dv)
{
    if (dv->dv_dict == NULL) {
        Py_RETURN_NONE;
    }
    return dictiter_new(dv->dv_dict, &PyDictIterValue_Type);
}

static PySequenceMethods dictvalues_as_sequence = {
    (lenfunc)dictview_len,              /* sq_length */
    0,                                  /* sq_concat */
    0,                                  /* sq_repeat */
    0,                                  /* sq_item */
    0,                                  /* sq_slice */
    0,                                  /* sq_ass_item */
    0,                                  /* sq_ass_slice */
    (objobjproc)0,                      /* sq_contains */
};

static PyMethodDef dictvalues_methods[] = {
    {NULL,              NULL}           /* sentinel */
};

PyTypeObject PyDictValues_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "dict_values",                              /* tp_name */
    sizeof(dictviewobject),                     /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)dictview_dealloc,               /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_reserved */
    (reprfunc)dictview_repr,                    /* tp_repr */
    0,                                          /* tp_as_number */
    &dictvalues_as_sequence,                    /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    0,                                          /* tp_call */
    0,                                          /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
    0,                                          /* tp_doc */
    (traverseproc)dictview_traverse,            /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    (getiterfunc)dictvalues_iter,               /* tp_iter */
    0,                                          /* tp_iternext */
    dictvalues_methods,                         /* tp_methods */
    0,
};

static PyObject *
dictvalues_new(PyObject *dict)
{
    return dictview_new(dict, &PyDictValues_Type);
}

#ifdef INSTRUMENT_DICT
void printInstrumentDictStats() {
    fprintf(stderr, "nlookupcount: %d\n", nlookupcount);
    fprintf(stderr, "nprobecount: %d\n", nprobecount);
    fprintf(stderr, "ncollisioncount: %d\n", ncollisioncount);
    fprintf(stderr, "slookupcount: %d\n", slookupcount);
    fprintf(stderr, "sprobecount: %d\n", sprobecount);
    fprintf(stderr, "scollisioncount: %d\n", scollisioncount);
    fprintf(stderr, "chain-length: %f\n", sprobecount / (float)slookupcount);
}

void printInstrumentDictJsonStats() {
    fprintf(stdout, "{");
    fprintf(stdout, "\"nlookupcount\": %d, ", nlookupcount);
    fprintf(stdout, "\"nprobecount\": %d,", nprobecount);
    fprintf(stdout, "\"ncollisioncount\": %d,", ncollisioncount);
    fprintf(stdout, "\"slookupcount\": %d,", slookupcount);
    fprintf(stdout, "\"sprobecount\": %d,", sprobecount);
    fprintf(stdout, "\"scollisioncount\": %d,", scollisioncount);
    fprintf(stdout, "\"chain-length\": %f", sprobecount / (float)slookupcount);
    fprintf(stdout, "}\n");
}

void PyDict_outputDistribution(PyObject *op)
{
    register Py_ssize_t i;
    register Py_ssize_t mask;
    register PyDictEntry *ep;
    FILE* out = fopen("/tmp/dictdistr","w");
    ep = ((PyDictObject *)op)->ma_table;
    mask = ((PyDictObject *)op)->ma_mask;
    i = 0;
    while (i < mask) {
        if (ep[i].me_value == NULL) {
            fprintf(out, "0");
        } else {
            fprintf(out, "1");
        }
        i++;
    }
    fprintf(out, "\n");
    fclose(out);
}
#endif

#ifdef DOUBLE_HASH

long 
dub_hash(long x) {
    long table0[256] = {
3823599747470803317, 6049665225936901233, 351709241968265740, -3567193261191146132, -2329246068513330833, -7881953978804000231, -5134155326318512237, 6977199918936832171, \
3878699803414024627, 6653163622710268196, 3204566177338596744, -1470711584827076289, 117487758559889599, 4438002078960907239, -8649532664808606985, 3961248710215013788, \
2757123492204357504, 574407754960250196, -6501306440310702141, -4510097668149834244, -2575526590200388415, -7361511802113759183, -2322608073872608665, 194155791575295602, \
-2288486394389341650, -8133757518791266775, -2298015529664186262, 3232145515556952429, 400744677788046308, -1687342448932369124, -4331766055813010854, 8851441406838731884, \
-7325849701687374650, -2425041771848882682, -3037724927721330485, 6626023002520638476, -6711283026153489055, 852388368875775000, -2979248586639516488, 3857807401557377008, \
8718606972609098740, -6829090877370060631, -3583920417620074790, 254059545051343475, -686536787422719233, 4032380955250148835, -3990778865629872884, 1711256517929123431, \
156340403005155067, -3665862282139002860, 5873774968511658565, 8091736197025565010, -410816346217722919, 4716048958649107413, 8383048535043026177, 686140283088652783, \
2731582647533119361, 3212659455936304781, 5221888375944341652, -4702725489219318519, -4983666174764706923, -2747382136344304958, 3314668547721091681, 5582291702751648778, \
-940014190823171282, -7801612559083075502, 8033261866686872982, -6056190462606470467, 4008011538824781130, 974962620650778073, -2255552795693304621, 4195652201914080928, \
781514889530297078, 6280461301395635219, -5292807593359894832, 5603308539278569319, -4898363640418086628, -4024764785510648649, 6885694195974204527, -3232084626700843404, \
-3063350158683801071, 2546540173480498208, 5038574827895228009, 5261872311948874625, 3852456208346356843, 4434575939837453837, 4233299108678665750, 6003377979757498692, \
-4868884280726519755, -6087721578971526867, -5564948339593012096, 433884805341811510, -733496510069854914, 4632354041342286703, -6656455309142324796, 1875910704266656961, \
-3038209229774302982, 5422763816838443443, 5378034280521168353, 8718630710454889576, 5850927924825422616, 2376693192129394536, -1113935635113968456, -3776044516959337704, \
-3021878593144547877, 6147760323513194830, 5848202506338957021, -4490835702674877894, 6347638730180771688, 1358826462773067290, 3140786713976763235, 9154276580716552571, \
-8774810171167566042, 1287744241693888898, -5355877533772003299, -8220138972185253732, 8842666939316767300, -7310368176964323383, 8694678388826309353, 5505830564521939585, \
3872315288340475954, 7101234704540354520, -2492273698713334671, -1493122298033173275, -5274899745819463938, -3690588929911546489, 6503790797391098878, 7569737523837997913, \
-2499042819877341517, 8885705421221296414, 4345051331416241559, 8370439513829447259, -1390090356785034153, 374112010240799699, 6349055026038274201, 3694931163033928008, \
1200957931716197889, -7913808324425406737, -3889683945371643231, 149441417387939193, -6911615812136114536, -4065213746164219640, 4756105051632574133, -4596048812708497702, \
-320483432920729187, -9160290250699324444, -671826912047853505, 2410273406739952847, 7988774768726382032, 5350426242391203604, -661040674123495797, -6935173326558459805, \
-7075348019720816809, 1934919368562081267, -8062504157320511563, -4808153572457178715, 4506897326840905871, 9137364303110940371, 4408505596846776173, 1324546992044642524, \
5446630186586632066, -530327846480091672, -6217929345850081963, -6714136574568816241, -943348777415129678, -8821499668012323223, 5264138893610119204, 6691572640040533863, \
-3087881958748784919, 7840878109192272930, 8509451521221460302, 3752499679542382425, 5791502386981022622, 1707269401703073017, -3788531507481489860, 5944082651657965129, \
-3860036106621708730, 4511562374640006809, -7670408726718689638, 3783796703606284823, 7100410815595546877, -7898474989732106245, 3822551703801456254, -4835896675420659501, \
2363516583754407597, 8327991053099405923, -8321297725151354868, 7578313334241810011, -4621939690248170303, 6109917451212670572, -274744947101249772, -2397298040121494519, \
4947806894014661733, -2622149160336848946, 24171865747616089, 5956594110031054268, 6646088250487351531, -6200161941652170556, -844683006724017704, 2156617698079422378, \
548554183542883636, -8245250930218668742, -2405780165060844121, 9106321363720264230, -4888005783312891067, -1642182790075967596, 2968670927456589899, -2128338068513283085, \
-3802845951344827067, -651218734995145842, 6127419860754424136, -8195249145756279933, -1630693413192855437, 3765519718075077106, -4297265766458717215, 6329546143396625651, \
-5884775183161381736, 378608319664886272, -5449834737994045628, 6115318324525170274, -303799677099841241, 3346924664412510346, -3229289429586666103, 1427560111058470260, \
5648043942522307761, 7029174610462868523, -3898497903720503521, 6468263754159305281, -7303102064239536963, -2189844567084237300, 4657788295483806979, 6178172547461874787, \
5590082643037261743, 2395431817330092428, -712887944697564387, 2305785652465068192, 4688012863294634282, -873603852657155483, -6948080958834620756, 3181206258464103033, \
-6383687519493312566, -7976231343598033149, 3107145795224037850, 3795924510782413246, 1921507511891144513, -8107873474538462039, -2248914717869755884, 8435541229237799831, \
-4778792496826026018, -104123334275530739, 6159310136305697171, -839827485672536873, 5939858665527177410, 7431618345246258531, -5364180426734326520, -6878391904301271694, \
};
long table1[256] = {
-4917478622833795045, -225453312933991424, -4428313229687735864, 288892660201016131, 2088301924481798494, -1490102494721481932, -9155031917243255308, -5557032552092233316, \
-2180287231320238231, -7156633420032239774, 247986972181304136, 2357791079018785647, -439390044784298997, 3953035089085845578, -4861323199186031237, -8421032089636407433, \
-5529657287601672594, 4160590640842490114, 4642579138953369851, -4633367748212847500, -3596311505219366310, -5412672950566279738, 727098926129939735, -2774235387005792366, \
-2196003079342734446, -4555969857860887270, -6805453011638049315, -5459606579581196612, 3339412136129991443, 2592911131091485711, -9214755354086440079, -51471218041962851, \
9014656732120567910, 3763513305439870230, -3567621024284806800, -3755296379649958917, -78742163246251445, 9181305354573386753, 5952826857441212994, -1552985477701879271, \
1717510423477381485, 4476659690823695074, 4625482082627072668, 8319340354982067953, 497287774419527829, -7301163774534074131, -1780687038935644941, -7123433662497136571, \
-6705025717600536699, -7977160067847596566, -2900358328017213272, -336968726842937163, 747386411546680932, -2879123135729738364, -7399286865664739493, 4296236061837556565, \
195573401479533543, -7338126105980034662, -4086096023480144425, 2261018762942244547, -5189965341103187118, 1956199802020834124, -2335148531809559478, 1950623231710061564, \
-4759035140688524526, 5653257947734969603, -662817669452837559, 9125538417299298046, 9139489648477076949, -960167760228667689, -2004938265197554834, 810879405820675253, \
7485759192927087432, 5149402600650072654, -3967258933677102423, 7526905839896708336, 6409205765303465095, -208887959051767819, -2895763609032354675, 450326703238843045, \
8457472149347895979, -3214773891947134446, 7579807155867515531, -248142588335469292, 1032293652506178012, -5084891730412104254, -8443517770632219958, 8123552554816936466, \
-1042631582656947839, -6573056277124512865, -7156306943876905300, -5199971253638998349, -8150176348770439844, -4023469949605387240, 2956038617226413564, 5507490485561661962, \
-3905930119505079524, -1761267645610244574, 3519742914032939701, -6747448295546048598, 5605637009716822560, -6414608551243550957, -8364192749539979983, -6001925921908686619, \
-1977804591575770849, 6105903588711634221, -8244463465436942245, 591390929628773913, 8839381757602593190, -2573484213268630118, 7157714302062086124, 8240801372985546660, \
-3772437402143975294, -4699060913370732502, 3625229099290804267, -7509943166163423962, 1614667253382360493, 7927167176340562617, 5475200548438583951, 4552074204632908911, \
-2516350285054386033, 4570076479620704275, 6016387482406944501, -4666509797216127717, -148244079849456923, 5088622465347717590, -5342206713514080200, 5470556859165666804, \
5339294400983057593, -638223115210242706, -6458444688156893830, -6445060285185945072, 4857208712479825630, 905602615167170820, 7507240982092477255, 2083419888139922001, \
6809979941356810483, -7804422405629426996, -5518866663343026197, 289580413561082753, -4756851352595617434, -8136419174859022388, -2479538453450392326, -8235408631522120702, \
2007161401420677917, -1354838866610872079, 4546247792367281064, 7349486844268203061, 265660714014900282, 6508701497070805872, 3200108423665774844, 4589228217100749711, \
-5056243967550628415, 6688686656469544512, 4320737449733595215, -2135357039932517242, 1777317379404128201, 1317474305259262148, 2836755624444389222, -133465844400612105, \
5015761783009090401, -6976351040955673412, 2809357955984115384, -3534793133352277972, 6722870586251723654, 7780358373917171124, 3456058689067282126, 1883753374581962475, \
3245000451677596835, 5748467045110767483, -5295247012566297804, -3211746392197307972, -3583729028504029300, 7279211551767647108, 8289104262117370323, -1886244548197456822, \
1045734581001503877, 1804288238494082950, -6017856968615052810, 6192055266703860841, -1698803555985071233, 5378580420875182730, 3226801347520675978, 2367166528359161092, \
6278357031616746029, -3734972410063797729, 4635862598254552495, 5626833505009898045, 7077161090544153443, -2104826607186969335, 8199200284452357753, -2279575443423151683, \
6968541338275044592, -8659361190223143250, -42514682070599066, 6281743122432795015, 7475028339506144202, 6848424024047653761, 8861241329259875540, 2970877078668821121, \
-2291705992833672562, 6322434936118542859, -8670043286301081316, -4117543250914960479, -3205236220722139797, -7320518654217490433, 7609319510684998093, 8194809008979680416, \
7954320442020609872, 1380502393702475035, 8527185082345703443, 1964223284597187339, -2742094224618387186, -1157858525315913501, -3773350466584685530, -5434693274418752009, \
-7793053107463078759, 4006186997790637832, -289384358865875283, -5995713291684477268, 905611242932197444, 3716749654190411066, 9201441221455568185, -7833802581248623887, \
-7112078362480783299, 4204813126110534439, 2181196406151510515, 6144147660046858888, -5416336085238574221, 6201543656738684001, 2854039423695958883, -7171242386797714121, \
4970724453730237502, -4702133085700049174, -7737070974443385197, -6996785951419779940, -6807109890387204521, 6116215037515448577, 8008794729945998530, 5638520356246988113, \
-2972013474585434163, 6251413874231230301, -7437273317504996376, 2209843176357381246, -1964912101917904864, 2671665196474681532, -2690256485086161407, 5844905168646431630, \
5686329807785003057, -5409031241947251162, 217183183645647677, -2006009822802342220, 7093140622889945338, 3800887754315865451, -6383488102434610160, 3668413993558381749, \
};
long table2[256] = {
8265502224032878610, 2971722201847552437, -2506257286188195530, 6862223079152072469, -7010822093538034864, -6228031731033701067, -6289954584449451383, 93162630067576320, \
-133453458018696777, -4855830406103851011, -8167071214803794452, 5800796176583697464, 3471882142860995859, -3730305612220325748, -5070028333783072693, -54011168787658012, \
-8871813674746809450, -2714123123297887793, 1232355686846900440, 6858922965543806665, -3323347373818181942, 8594048365427567563, -7278324740127661822, 7539851856909364646, \
6677394968260009005, 3681946490674805502, 2496523933501080226, -1760080898425284423, -8707183658082428662, 2914041481794005713, -5673197838934844844, 8561628206281225387, \
4458754607730955028, -5432538524420386493, 4783914999214078290, -714213997015512181, -7797736799650769425, 4644827405982884452, -7885453968865666861, 4491330956999174470, \
-9124101314179293819, -6340209159602242051, 3911940175651672971, -3657304500531317623, 1253646400881227519, 8220003921799699453, 8579510425324148444, -7860200908556531829, \
-212063667714533450, 4589442757430662329, -5353966599083900416, 5596867439154033362, -3636806053453158449, -2767515560642947134, -2606656855066343511, 5227960139673321480, \
-6094923679647261243, -6738615130854872067, 192018112676622224, 904311584632156689, 3525414022898210357, -2253763657960423491, -4468127090737349635, -605850599922309660, \
-8949348014977529819, 8368820153183779013, -2705053486165781444, -586868395727583997, -172678482720626171, 5234139901433886550, -2912642479412031251, 4137596857308271170, \
-5497465910544477837, -2283477781729358329, 2531736270988433557, 3893723247871609919, -6814187475666321975, -7380638282184896404, -6371173354849013421, 8016193707626186976, \
2143494744570918801, -4072545655166146435, -5724217368459108816, 3841717406750416933, -2969010948569054427, 2238916394037688930, -3645554393963840857, -5647362556145628282, \
2688583210844753357, 5631380251725337298, -2999757325653319600, -8162632765302893100, -7873946154752643809, 4914361077346554481, -5106651341550445574, -7209558657673205094, \
-4671202336024935935, -40647638885135652, 6799263099564015276, -3127484976545482793, 337740002155189418, -2214206630932952806, -5222736152747654763, -3799695766916659856, \
1893081046378711680, 6723879665563059702, 1435194647831683759, -6670220922678318114, 599125677594787986, 4779243568176375413, -2140981677992859030, -5932787706002941674, \
-8392569782668501456, -8118064064044612101, -4666000788103960874, 2860658029277560255, 4027340303207484517, 6905517264058092427, 8112137168635245356, -5558018911252916996, \
8455363021059948393, -6187234394573791755, 7683704807969919425, -6368503934022448853, -5589827446414550008, -3678050164942373927, -5683555195241317394, 2916883847950131289, \
6615434210752200092, -6631513187500512054, -4543796764689360686, 1427475286242829138, 6541409706120186455, -2532761753224121284, -7214529500536104097, 573376326478776691, \
-3019872712172629561, 266117843691235799, -4555145106704791883, 6237830927484723374, -8447903995111230226, 8699588560608894307, -1056893647760701575, 2035945632973709661, \
-7382260741996598562, 5549596252024961974, -8384996803334776376, 2521503569928668727, -3558951686640522741, 5139209970150954655, 899666432474671586, -7344087323146425455, \
277272268005550209, 2496783039516224807, -8667342967299714826, 5365061323479875785, -6497238267272714431, -8248875087263340347, -5772998125391057577, 6609920193840580221, \
6391137399678951552, -4516794669276849703, 7068229576607925950, -2267954463647874550, -550459623886018814, -8041556604906320136, -2127668280835040378, -390130948834065892, \
5805539236587548756, -5122748849360336427, 11056758938732897, -5854076526411574319, 2933349390602171205, -4351099500063763346, -8831724848679166893, -370682571907436226, \
8011200962899169435, 3592690538498277097, 7394380924856025313, -7502000587033526934, -4532337068723532824, -7305365265677508836, -6263588157534609034, 1332877630374808937, \
-3301255165238055964, -517855534467218391, -368651092814110285, 9097869582833132399, -4021137899024016569, 8096419485101372761, 8473833596282430170, -5004902954834480296, \
-2494829399624433556, -106210701809082012, 4760568543395239303, -8812923485524260266, 4648702989811342560, 4034469860525284423, -3058769415599882169, 5680318919676611841, \
-3651674564238297917, -6979554740083442625, 279730998349184471, -7560155485946456538, -4582341489278244895, -2699955093221509915, -3627535962978354674, -7577585309153943181, \
-2693591297484397226, 3749128884831315626, 1859140918325485042, -7769319358194044531, -8875251174771445120, -2150346815836413573, -106374508931066, -4547924361652525371, \
7901651276470980622, -8276806473897447505, 1580591055378718473, 7672841066358697669, -4477098697629274808, 3804613202413240432, 2955049654026285956, 6692141147181747084, \
-4863670057763913543, -4928257707900021121, -231199883813880679, -6766136164304464008, 1662511827110051639, -934893973382285093, -2847388119018827185, 8032752231427532527, \
-2154713968258646591, 8548815748732926898, -1470715085129982172, 1756766020329591389, -1061467982110526201, -6652206203408706796, 4752272602515811630, -2955111328310209624, \
-4731739864575234211, 926756959796504466, -2294754665985995837, 3931440883130698784, 8755235609307038531, 7933953456222926636, -4620898923474432617, -7003901818379980595, \
4284377185404356886, -6978007734050766627, 5575721296473242399, 1170831397516933781, -218483028344231058, 9055546746902513759, 2884771651647320837, 4867061614138024779, \
};
long table3[256] = {
-8816748716858216437, -5235779177520853463, -7257935343898024184, -2646325719849116921, 6435384565929154625, 1867408838629783707, 9076032087555113854, 5908308212396727219, \
3256352855300845526, -6611964285434526545, 6034587192014391952, 6802368186234870167, -7874503911089455786, 2834076306807265211, -4428011790714841971, 8365156636964481776, \
7947151508523096500, -2856693604346397841, -5674426893855455320, 4890510726201130852, -1977082924221869321, 4306161293477257441, -7569381587212119510, -2464721383990083340, \
5457683293986846115, -5150629762150811964, -2176943001186102283, 448292208713684172, -6561189947086546174, -4470882463872600945, 2042286168799895930, -7959650112342326504, \
-2971990035489041843, -6843274677426385257, -5105649262634536400, -2002056922260803883, 8922912408779870422, -3865026675972028889, -6359250385902755099, -8255389817416972539, \
2287353125512480490, 3271277363047528760, -1094630640787503325, -2819094131619796579, 2738304038553692295, -3621969201886983283, -4316748889189444161, -5975366171297744988, \
-7283528573955637259, 8255089650153973859, 875655304582147713, 3068790749371956063, 1192974567328858000, 8728614271078349690, 6094484626446053913, 147352471432974470, \
-2355745441834098765, 4996938331970948581, -574875343080898098, -4098305532556396109, 661262089559996074, -5640933306374524866, -4200953262670345586, -3493877937135748315, \
-7860000674581695192, -4417404348976958666, 6746704098561540851, 731722900915061488, -2007683731290963454, -6197819509808911241, 123865655545491763, 3468647168123206192, \
3484086336311284822, -8132966624926434949, -3539312498663501891, -5062145577482137682, 2628727122577012488, 4241924575377635820, -1536462794179220819, -8602991642199988300, \
2710506354767724028, -5004302957896314185, 1867960849900613403, 4904626401468355465, 1013046011978704585, 8763803920706474478, 2308699899920276024, 8334355918491830878, \
1919422968373903181, 6069484861382889543, -8222718552802230593, -2900384617364929221, 4782915159084353903, 7626288954829933488, 4467831807535196044, -5106213369547229537, \
-3238463775744095881, 1698735528870456734, -3601384381744366882, 2028482221316121393, 7181397642691712221, -5605536513093374457, -7565535134780478180, 3181077142948990420, \
-7192608568771178922, -114316007038574525, -7200499516636796132, -7225214925692460332, 1171442462953137332, -3314024402420235824, -6743604903189448433, -5407739615267022917, \
1114299289552433866, -2270168945443953855, 2186256766813646497, 3334752857528880110, -6539744722366898260, 882989748518144714, -3680374568166352471, -3365826989841310263, \
2502438797086209182, 453462929925803196, -2440728512102157092, 1486531633930479312, -427473349358557430, 1973841527198512647, 8139245057202729539, -3266330562191837234, \
874876182199455090, -1014875907859623082, -7668870433777412654, -4978421545814291951, 461305623785491799, 456231569162401872, -6644278575927587983, 1011104754336204554, \
-141009736334733438, 782474452698233660, 1675809723675370181, 1771061835921107503, -3517026929165526072, 5560952665806933817, -3541529406439684771, -1391495848969836498, \
-9146023735220783427, -111632640664086670, -8086018911335065446, 1108996285453129074, -5319358379251644929, 2096775616940959696, 1865827062740247432, 7476640847683415109, \
-4268364988096732996, 2819836583260129987, 7367597204576666446, 5617570057040685062, 2940428779098587731, 5456062077013319297, -5489832422664600283, -4458405869537527263, \
-2213107837335434123, -6348669141361412469, 4815348258722486187, -5264163127615073371, -5794603413370911248, 6741652444324486421, -5383660279003257174, -8874163656525746327, \
-4581104391639714621, -3155680910384389146, 6633954828911704319, 5483863457698044593, -162548211320663066, 2238413552889619651, -4005448740204134011, -3572132794819262392, \
-5950770803358687632, -297803428956115063, 8281744386850611041, -4154348836732390447, -2260483988226352036, -9077955308010021642, -4161916463690473232, -31349809834533882, \
1490965349629809679, -7999606083203260615, -5292122585904959530, -7719793703707727533, -301531011329292738, 4753831554546235537, 6952330032118599175, 7627562696934858968, \
-224383131864153454, 6439542820405891342, 742702770273236807, 631314816752836705, -4148249224350074612, 4967081259805522198, 6282446699112664074, 4101804652496960348, \
6133527994266312955, -4311005337896814792, -7865217051849267704, 1442469846634984848, 6744650623565713086, 1393358247180050226, 6644705881069959420, 8077996600590685570, \
-7628143089042390140, -115950580295055711, 6178941237530145565, 6108089621156443433, -4921629793820605601, 1032071968327271785, 8895720935586016931, -3209126640584950910, \
8707538094717251441, -4476075786980844755, -9167343804606614423, -8498889214021154191, -3540516778017966484, -149610272952132793, -5483007922529137476, 8611137194856620132, \
-6960810029165797507, 7017387105014111117, -7020074355197797447, 165087752249991178, 1542057663135021043, -8670508243753136737, -1229446970747486025, 4966273190286894870, \
3081454081129761311, -3320209815462713716, 877476565299019868, -3542289627124610717, -8401906623844010549, -1685139022195990802, -1447658574760515780, 6995939808523134648, \
-2543051045417111245, 6293079949765739786, 6711617394001824016, 7846644801333066602, -3790074288625830259, 5085816640178104938, 5976984907895862645, -5510016109083405341, \
2930572856565120344, -1763634667066995816, 895740305602784366, 4698711336872353613, 995762865172049986, 7921034255324187580, -8644124160288489624, 5299060422232828642, \
};
long table4[256] = {
-4516920530639746819, -3475564227948834662, 4543512722469299562, -7715984661561665861, 5814772133053214706, -5525462923732213592, 3030352139752800722, -6621142040904600911, \
6998786199229534165, 5756299232434775790, -1519599796431595511, -750069270548003738, -594030824799154700, 1466989231233163353, -9035148889650806316, -7119445349640839333, \
-237753189240280168, 5585493598615142587, 8027084166487486974, -317388320413525548, 3884785751987890446, -5526483829168487894, 3055256101125333000, -362002929088586481, \
192186483850727097, 2502393437558876365, 4174160510074562288, -111120157775273576, 4036649785358643703, -4149769136740437400, -212474751549788482, 7933641833387659885, \
5342752968485774261, -2288300067127947236, 7711479536456475750, 6885126486453778855, -6249059296188037942, 4352924397625936545, 3926432405358427399, 1335626666024510342, \
-5923769184854105632, -4794759563737903556, -1764296363283833359, -377663185183182425, -180616953337275796, -3172948770763697626, -5504847989468421008, 3886955088670695774, \
7007096829486983362, 1373012166418324140, 1729421656631214232, -3246950746122982852, 9200697254075281948, 575677930532321070, -6961660387389105463, 5192985746251715798, \
-2590902955180028359, -2988768921939144022, -5582881659029858067, 6194670689022560243, 2180693726347141558, -7925291216347559130, -7523826879022912043, 130409368556005419, \
-4193571634361190885, -6336965151720345492, 2229658547118615848, 7689644360919284563, -3029701794828062904, -2019172287860698863, 8250585309955517499, -6998179700513249464, \
-5653932978874540390, 4065155388142138277, -8221265664944266083, -655009002336301701, 2341126436162723459, 7121924483262956653, 1919410802267661170, 391729556885980729, \
2359653791533687395, -4003670262574150606, 3264109457743453787, 144512207860661184, -5455938439133122792, 4972979028040283317, 2402066435521804207, -1962898829125081702, \
-7759874351736519848, -1568434882785115268, -6044228619379122785, 9093898962579577758, -366706578994730705, 7114290735632296679, -4524831060260134583, -3222747517680236088, \
146939669916493939, 3042054427924234220, 94819663743393735, -8798409793347667907, 3337603041472910690, 1702283064986164219, 8481924208488179573, 6914865344188047325, \
-3478189783087729380, -5576432558834894779, -5028788486668124971, -3869595379563410620, -4099957717661229622, -4418685758467840777, -5065984898005346341, 2621927850911863686, \
-7990260393005836729, -2340830789788186717, -5136899253609720583, 4161213659762565741, -4490687349112055392, -3638769011025179673, -219633020961852721, 7440066408279107994, \
-5569212020903687678, 7119019633414692050, -1602595257102934209, -3096905670766152624, -3241354490904366618, 7037625168131905160, 889604851123908300, 2029286837145133491, \
-352744336744414765, 2654480623552272081, 4058039964517865359, -7891282458095561666, -8093179214171100567, -6787554789173176341, -6565376830342246345, -1581926085324166672, \
-1760173107051373170, -7588039491849374130, -3266983074130871221, -2123904809760683799, -4255687494988632310, -7430859650768466430, 6139148684098709088, 6564236262087763925, \
-5602224325127392526, 2157692306932151290, 8442057728469667176, -5313207200652052737, 3314290553312781482, -8974190895236554055, 6300582485417797577, 15770085010182796, \
6336504585084563138, -8201085607674787300, 5902137199531061090, -5718335993755845576, -6734569274994205823, -6216517364149739512, -4917139881754313284, 5810317596291204522, \
1013914060853054903, 5713831955955527404, 6705423041959208322, 7097954391635837085, 8402784539179180480, -4723948809642399196, -4806561412171493424, -4376336758865661772, \
-7977299613242659233, -7284255085011176176, 6491933089872579374, -978460140833386503, 6421399029972752678, -224878231841907035, 3415816830849720572, 5593385970994672397, \
6732779919673923376, -464493274009484678, -7740977004570743436, 774983771340132291, 2410251548382898902, 1741222847185588597, -3689290693583429711, 5646798507465495222, \
352966609783762796, 4712035827136231889, -5643152266135710301, -8052375041259425828, 4367485198367918009, 4679688943830874500, 6030671650020742925, 3749241144833427979, \
-5839787812878615104, -7516003256259790642, 7019721653862047701, -691143784280682674, -3364179878514081683, -1684577650980316317, -19423021660874663, -605101979327436576, \
681654370723926324, -6745907081612144410, 8028456725233218079, -8484106471189505337, -3930024936052434470, -4326880517042994612, -5577531232623117964, 8213555270636870543, \
1680313839291706869, 2466787467863387570, 8135041864615978989, -4323527218348909948, 3678814159186298398, 5140513281562192961, 7694816878080834496, 8830258000001532439, \
-4908750100644079519, -8602886636000786249, 2955703956369646494, -6202139057332808255, 7949834425398098130, -4779226914383438010, -7349137592040220278, -910010833432392603, \
-2454670201838062188, -1936001551513417758, 809997381319576794, -1267120497960759136, -111504525847572103, -1084264938249570730, -2604251774418742386, -1721100657966871881, \
3842913548561538444, 4379692455818604208, 6162003192688415012, -5106213198697093899, -5495852038374785219, 4264702376918927614, 7068781281242791224, -7544678257454352017, \
1022498684016952951, -644280930179833147, -6950604427577305031, -2390014815943048690, -7757639601321277286, -2342121189618657807, 6422033089968034128, 3794497400295918103, \
7233562110044079769, 7768430338356152876, 2123684585693525636, -1023167262255417594, -3536587420900838654, 8853842108474094007, -3828123305645408065, 441088799675378885, \
};
long table5[256] = {
5127983853058919457, -8144458736169113537, -5913333389916053027, 6723678311281683728, 4171969019039586076, -5652994333110133640, -9010053123430991465, -5843319896280027409, \
-8930331000201206225, 1161474200969163052, -2847970377570091204, 8517135374117821654, -4758895234591792760, 402970344850083023, -3697053423928888808, 3743100402973630622, \
3018630075658962919, -8460063981461793357, -3793110977735302729, -8311666178184469070, -8995481963571976214, 4370821816680091145, 6559333303611273426, 3829962648703953497, \
-8223490514255026619, 421845493347062233, -4213589693678967256, 1738018732832406139, -7454944988843881154, 2175902180221515053, -7266021167813238278, -2937095130936336877, \
1434262750741051360, 1462744124203392241, 4887056656149519788, -6837429224475961902, -5249006822028811310, 3486822843058916600, 2178293599229946252, -8247328605839452019, \
7775264780589851524, -4626644623544847482, 8638848241821956962, 2378281029681343662, -2752251965395236300, -2343517933003981344, -3893804846126043344, 8614230085254692515, \
-2503534695139312361, -6467245390948491805, 2911812665366677029, -6035671309500620217, -5375789212322058258, 4762489482585029669, -5666934492888836092, 1811337625810896206, \
3715641116269232044, -1145336081945049573, -1814892488461642177, -5964370625579626726, 6827561046782664689, 3784393551975238005, -6802073288325024638, -1398228392320410369, \
-7582104166390775163, -6872336201171007571, 5361410724854938730, 3791933071120825682, -153594507318902867, 4390699018252538345, -5324976329445439868, 8519684551780917797, \
-4461115899938810226, 8869343658670433217, 8777774416100306326, -2994780974334071340, 3302210278830227211, -7287530058942996358, 8369372557460796942, -425700924581934465, \
-447078213967708491, -8676579386024391059, -3456996810774499411, -6024560342999222401, -2173268965510102119, 5865374325885518983, 6096334252430967191, 2538228061934751043, \
-3309539742808785668, 4513584978172624448, 7769688335618854975, 9023626870930304534, -7816769308801538796, 6721506119477015924, 3615740127237831994, 8227414714573026922, \
-529398510044941676, -3417334602595372586, 6616303843362780334, -3331696213288912727, 4563055865296764612, -6726532802506683218, -7997608047398175365, 8708965806118225391, \
-4526519781571625615, -1157967041697646838, -7867701957560429541, -2650382701008084126, -6533040066002992351, 3216093222054267402, 3217074004518470950, -7655826176388946871, \
4428800104260155229, -5375069354061548268, -2789850427507367628, 7059502257793736344, -4904503578166367134, -6427330257549826497, 5802569692222780575, -3423432236724370247, \
-923750601486647822, -7468366467949089024, 7606372358292475409, 7585953111580235492, -144621195953162086, -8247240592609657793, -7057100855902287506, -5007368227868870184, \
-2825003344866580542, 3706706784537090062, 3309348109077413366, 1247394445643378414, -6408428673575810781, 2388892978841571649, -5438709212004695644, 7887551076866580512, \
6948695019133582582, 4451701144686267716, 4246870612534287001, 7480566157367930811, -8850573008120590345, 1878731693445970173, 7874196993471318645, -7054719707170113759, \
3007265358076887558, 5480023746719667756, -3915135213535876371, -2409392160732802814, 3969812173145126309, -5794713662134730402, 9209400460702252188, 1586488313114597057, \
-8055764538536033349, 2709323932478230037, 9207018571765264986, -2722360469313992030, -8684125583162722415, 5759265096677257487, 6919468166522775717, -113583957088594272, \
2920135978048583859, 6577484901200171493, 105374431812757633, 7977763023988107146, 5793243706581142380, -5971497617908599857, -5288010257390323118, -495908509559557265, \
7941917798770941283, 6283911430760001832, -5567601003475669771, 7289187691561712238, -3083929072750483900, 8194999675631673661, 6310129460847107073, -5267231081989820297, \
-2190692192314997376, 533412647679503322, -6737144936863472109, -6738706923300083270, -5226720731185428751, 2180287858274135473, -2468760469520285267, 891909043931665905, \
2582480878688810982, -6531871092362401314, -7270950349966937951, 3462697706520164330, 8790129481802995225, -7005350921278641076, -3731113211192771683, 3175533341379212028, \
-1230905941463651136, 5300851108063252332, -7238266868934632824, -1016513525175182168, -2289571573984827447, -6869170403477577259, -1881935967904830446, 2830924838742443131, \
-1401058306582153709, -8175140354146255489, 6829208100063141710, -9005591831391783519, -4465587381106679992, -701752505368650398, -152768327284091277, -3191488575195234673, \
4934669954098119579, 7534389823728688782, -1051620481643236008, 4739814198523531479, -5513232481323135118, -4842946202410042260, 212237936399302502, -833717942624190422, \
5279792923164559642, -2055748346667311355, -5016772338106691559, 2378314574790358649, 5461582051787099774, -6424899221992172129, 347351970101542100, 4196536272177328998, \
-5449592746822051401, -859450861230406090, 7835032345268678939, 8155774815277755175, -4469743753080849257, -3751251220674276722, -4112978596241369623, -2672125585981144346, \
-7634034175659044729, 4970052893166556772, -2408673246599122394, 1226940016323915332, 5139044190282360720, 5741300598356704411, 7785425900869418437, -6330218104668449709, \
8265142230118690091, 8470308631983820009, 563987554213961675, 8578732738941275328, 4324068440702412163, -1408292042363813022, -4275696561415656121, 1792907639311418937, \
1233953791316256553, -7856446183866712909, 4552379983898262691, -4659892112815728948, 3148462176979912416, -8054146616602260296, 340027668835310225, -7636080999858532469, \
};
long table6[256] = {
-8126498922419378240, -4182394371676443540, 3688203099773961276, -4840983442283562053, 192656810425634413, -3873258944755831440, 8018735749818908510, -6399656280098385115, \
2105980179486709247, 6214235960199447356, 2765961598746644630, -1712089648486311730, 6841163054379985371, -6407854193835691080, 718326358247875224, -5057469644928018425, \
1008373492756906311, -1027147599621416569, -6781960790611762915, 3141921218422358399, 5253424659847999933, 4826532739678444239, -5220081645090021985, 2799463641699031166, \
179202341935535080, -1057579998297189098, -3070541883194854377, 6154263601414182029, -2242281798023088389, -3400749243215052545, 3621021386961987120, 9061461158389897076, \
1130287689599608010, 8580954729956026183, -1045858263226464292, -191555915082434898, -5349710395845727035, -7193661811382078231, -4362432321978568489, -5191483013981435253, \
-6261598201976818884, -9154242237360403120, -48100431086047871, 143377596240352953, -2477368150968552057, 2464267036572689441, -1331000356630943174, -4589221466641840610, \
-2254312960369690747, -8584416181258297653, 1697502822103829039, -3297032690766025258, -5974356776852497215, 5870324651530614709, 5232507978915722801, 3748792288165286792, \
9045463599606611623, -3179120483382766247, -1289788944730505612, -7563491450050630928, 1497764469051493600, 655324748452615584, 4920937689793895377, -8235437754062108883, \
697435828788476158, 6115293609998833460, -662041446396592053, -6151591762183127773, 6744206777067152463, -5279178224345322531, 5730743674880403369, -7221777263651184855, \
-8518711072054238093, -5547473308324977404, 5318901438612363932, -514271147901061093, 3997582027568692245, 1226903832949122982, -4920621363679052375, 6123981217001863414, \
3228937600991565938, -1999036715040273018, -601678217396058448, 2764192110906699773, 2462010752563847094, 5072451633197654977, 8817388558509130243, -3791337002730992040, \
838075817586602566, -584964325355437198, 1044935209419833936, 5394275115852272553, 8843002062846025677, 3442110227915251784, -243357833479713418, -2759588734118509201, \
-7056611158874269205, -8082399260914590322, -7470003044267469417, -7025979118955738176, -8225206738335087507, 842311247827926841, 8775487384742755196, -3406935214232897554, \
-4166891709186766651, 3114119225793902756, -7354729589213555375, -8371487153729604060, -4784122602509158386, 5068921529285381177, 1753256552005391734, -3274250064066125030, \
-8721979645536865837, 793005377219310389, 8146090013447553661, -6657027853894244539, 2190295381508947211, 2878829309741484398, -3048764129156762941, -7154857931105554644, \
2142947135805188159, -4753217060859319475, 3880182519272487160, 8404212467742270878, -2308336676856293626, -8559854851793146951, 2614044702512731426, 9163580017465473109, \
-8044196359010578288, -2186290591730799131, -4889168437715668048, 9069070848753584728, -3230020893429452125, 4678019485329768226, -7929263279854350843, 6490411616377994983, \
-4032750073852127255, -858126043850247713, -3656610398113977011, 7232388633722067153, 1565616660401315736, 4463436465583337724, -4790539773940417902, 5825532622384172372, \
5116412926370947086, 7735267206979645408, -7478652011653868016, -7795086224049375889, 2185246470504804604, -6394263423104921635, -3746264625957797592, -2430867895317902477, \
1520296701882532048, -6419206398808042991, 3825487648287461624, -5864497799829731892, -4382571114590867900, 5266916420328053307, 93221431162558168, -6398172785381489852, \
1579254187839861741, -6794364399636051321, -11467686157620857, 47264994435927317, -5993237265385670371, -5111585066768747333, 6848999971117153585, -6659240119205271734, \
5920396077587195529, -7403324233802423606, -1035036298290150457, -1059604446136335186, 6254214100746864173, 5081560313339354533, 9066194210748897651, 8969821853170856403, \
2943028141602476569, 8447497347047349160, 3332265970148788816, -5376571419492233690, -4843913642361941584, 3122354834629908665, 9048595995189696042, 8265112509589341361, \
-2749869163166436020, -5095403031048074028, -5585196188790786408, 6799712209319969215, 7058467893640047466, -2687631488121754077, -6846619839177477910, -1497135263943220680, \
-7010800293835600675, 6810000249649815836, -5888189433781126630, 321745540586563845, 5976227604482674584, -2150513542392570838, -8125631594740729400, 3141619695235756921, \
-3086283693125489418, -3646793230088986241, -2239084624623675519, -3217009411900205954, 5649061961260512170, -4916237886612282531, 1928661946281760622, -8746888435422108409, \
-5149908200844057799, 3661136665678371998, -2980479408562969522, 1499968561462409175, 4811483078394440032, 4337049569765570341, 5258303758107329317, -5933925833427774543, \
-9073108254093615549, -3876367103614384154, -4709958773975710977, 8595558470703171561, -2927589966476865296, 2636967623026351074, 5750152503387593873, -258627734767036962, \
-5455330501833880424, 4743347371381759415, -3720686886590253488, -5077166518516721662, 8872253999247401379, 2857334193033264905, 3319634685098984407, -8661351348640190348, \
-8749746432343364497, 156190603975069581, -3183111074632111102, 7218174272782029330, -8895668157366238358, -4857984398172584310, 1362557203501998507, -4600490735537509832, \
-7652529925251406083, -3242359135577964672, 8672778929468945120, 4686775384640451638, -1441236990708465790, 5518381805927907072, -364287482349324922, 3794099643084490158, \
-910769861526128336, 5530160865590908953, -5705978629845326450, -792207137901951589, 7965536591785753043, -433965872801839939, -2487183933115916806, -8450310589767848299, \
};
long table7[256] = {
-4443348174438570025, 5307528052562411927, 204543315308219109, 2008286402823586271, 3116055094448130982, 3002320905062191040, -8204757987936275814, 1538818922167732934, \
-7340603222706907714, 4816419350012723446, 7760679411852283763, -3746482097228960841, -8493505263089808086, 1846865668728135233, -7689678529965472395, -1833549796494005041, \
5947949298156896490, -7045698652630173112, -7422566006073943476, 7033893169966751280, -5505823250387661086, -2026579193781233600, 8776628306768285754, 1062708001035016586, \
3855068177031991755, 4369431516322278286, -5511652095235418581, -4584036070237065121, -3550199304772735575, -726216462193692554, -6519208203712205833, 1825390088106689085, \
8579151254362298527, 208719728837396661, -2026678704839805424, 7445590091281029014, 540975062350704405, -4131365927920696946, -8732901540765094981, 3827021890605454090, \
2037331122972139937, 2527862228838790258, -954238870075255645, 260983875884225426, -4734130593511024643, 7413028360627535198, -4063558155254066155, -8617801272716755497, \
-3428614089323550571, -3200158302949732560, -8801658902873316844, 2492899199801429574, -1776656974006719918, 2088999107651826666, -8562381945879749009, 2677688896579108157, \
3831579672082844380, -5698668472367376978, -4254250039068506593, -5191886791282675570, 600936605695300291, 7419739932398244310, -4602187647991459185, 7067799335738518932, \
-7187195135538504465, -3204376362443322311, -4957407587456949518, 6920188972846763714, 6572172238873129892, -6603044796890075698, -7234665032724408380, 5059088855226335445, \
7374564493806551108, -5591596072858546068, 2920382618069851068, 7631418996519685266, 3068977308024431819, 7765935153079606112, 6024347245629789189, -325848131179675223, \
-7227113340283813596, -5592609733830569723, 1941959961230881720, 4819617984560244193, 4031341713572363585, -2234743085153064179, -8882289010253574050, -8897305876687646927, \
281848482898498928, -2346259851439026269, -3580782008290233759, -6685269285464322299, 1298417225859045547, 860546353667648283, 4317823408669233702, -3994585554325657464, \
661570950667965505, -574094549108055584, -8037468546141676768, -3441240092482715166, 7943092353608017036, -2135146734745702952, -3483113922281880868, -6533953715606286147, \
8313768872503091155, -1562830050281887321, -8575497766687089846, 7512507969569779052, -2786874858255724018, -1057827964388400575, 8740611785847503097, 5547410043971087421, \
7381975067586140926, 4513034765514392055, -901763825927069145, -6319660631340645987, 966904614604781999, 3244476414974935195, -3344632479930875180, 1851857031561417542, \
-9086079716485852126, 4063222377135687715, -7614378893820339532, -3582349178368011283, -4353516808737338397, 2190207527057293697, 6209923601883197137, -148552394119584527, \
-4840128903237597962, 5077132589703262510, 1877401798749928285, -1853411769019237296, -1048824918732994517, 6083389038309161055, 5600829917215699340, 3382574667029617464, \
-7625436321029132820, -4940274049161843519, 5240390360339776261, -3647837560252160958, 7521548491928581085, 4983745725259617670, 1330954943585955540, 6073748941618428803, \
-1092607009261638497, 2032569823067031676, 8285054336519347630, -1482223246550756428, 2801607527947906191, -3774189648078939055, 5741353698259448131, 8379967736373569532, \
7253293214646468433, 36615973155698936, -4467069923529126667, -1451046465537252851, -4329452794347504034, 7738346406259801725, -4123528528528680913, 3164490130845930868, \
6275740493074507904, 6759792629516312252, 1093719747028215570, -2549236514267684042, -4569672800371027133, 3270719013168563062, 7564041700424683960, -228037002017494632, \
-3781564943250885661, -6860034246119936097, -5740707378499564756, -4302188984862594230, 5200796796744995330, -3091046714146024955, -4000439231975148193, 3488087408308713468, \
5492407907449355385, -4532099162327066652, -8718275449576605326, -6115391450980028948, -7674399577374271029, -5071131535485732996, -7680866531493736111, -2247715029315579357, \
-6093243646749284222, 9138408704755387272, -8914592262751510878, -2520928059268337810, -4987651538679931469, 3761283334799203043, 8849957779864913309, -38647850336327843, \
1442182969069073720, 4117238386941670071, 721975992748827580, 7317077613483675503, 4734255156444744855, -3858162293541440079, 3561065886091369377, -5695710600953111899, \
9254602643229238, -7467869838593382128, 3012403948345567390, 3775965613559097278, -4989188117651667347, 8802140128917111338, 5342031309827028864, -6958814521301871372, \
-6819700544408249165, -8290802092221324387, 5015145418502939786, -4933775216565689318, 7715841322619919150, 5390249312020423619, -5676748265203720645, 1439853943447960305, \
-5393509438036471271, 8158711359456406049, -8054574585796867687, 3907384165387894377, 2692713946051877354, -7078112030610449048, 3626554664117024503, -5446183764474002567, \
5124817322072766677, 3469044738266617074, -7849281466282883081, -3157495144026266646, 5361600201588746230, -3264037187516159040, 880474098764912327, 1509333070377172063, \
-6876783395799578547, -344264099435085080, -5640596543264044543, 1362317447013829782, 8600754275947906914, -3941566460206209678, 253159915959408691, -7839164139681474085, \
-8777994705961834435, 277898823194561566, 655584306801073228, 1654209109690846452, 7206738870490995843, 4618828208651355311, -6815354048294096226, -8453732330529702602, \
-6511037824987535872, 8685721270243486781, -3650433162696948739, 347506646812413080, 4984637004997021226, 2669559898318687978, -8841415429917811166, 148072958550296305, \
};
    printf("w");
    printf("k=%d", x);
    x = - 1;
    unsigned short bytes[8];
    int i;
    for (i = 0; i < 8;i++)
    {
        printf("i=%d", i);
        bytes[i] = (unsigned long)(x&(256-1));
        x = x>>8;
    }
    printf("x");
    // for (i = 0; i<4;i++)
    // {
    //     printf("%d\n", bytes[i]);
    // }
    unsigned long xored_table = table0[bytes[0]] ^ table1[bytes[1]] ^ table2[bytes[2]] ^ table3[bytes[3]] ^ table4[bytes[4]] ^ table5[bytes[5]] ^ table6[bytes[6]] ^ table7[bytes[7]];
    printf("y");
    // printf("%d\n", xored_table);
    return xored_table;
}

#endif
