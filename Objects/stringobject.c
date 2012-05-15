/* String (str/bytes) object implementation */

#define PY_SSIZE_T_CLEAN

#include "Python.h"
#include <ctype.h>
#include <stddef.h>

#include <stdio.h>

#ifdef COUNT_ALLOCS
Py_ssize_t null_strings, one_strings;
#endif

static PyStringObject *characters[UCHAR_MAX + 1];
static PyStringObject *nullstring;

/* This dictionary holds all interned strings.  Note that references to
   strings in this dictionary are *not* counted in the string's ob_refcnt.
   When the interned string reaches a refcnt of 0 the string deallocation
   function will delete the reference from this dictionary.

   Another way to look at this is that to say that the actual reference
   count of a string is:  s->ob_refcnt + (s->ob_sstate?2:0)
*/
static PyObject *interned;

/* PyStringObject_SIZE gives the basic size of a string; any memory allocation
   for a string of length n should request PyStringObject_SIZE + n bytes.

   Using PyStringObject_SIZE instead of sizeof(PyStringObject) saves
   3 bytes per string allocation on a typical system.
*/
#define PyStringObject_SIZE (offsetof(PyStringObject, ob_sval) + 1)

/*
   For PyString_FromString(), the parameter `str' points to a null-terminated
   string containing exactly `size' bytes.

   For PyString_FromStringAndSize(), the parameter the parameter `str' is
   either NULL or else points to a string containing at least `size' bytes.
   For PyString_FromStringAndSize(), the string in the `str' parameter does
   not have to be null-terminated.  (Therefore it is safe to construct a
   substring by calling `PyString_FromStringAndSize(origstring, substrlen)'.)
   If `str' is NULL then PyString_FromStringAndSize() will allocate `size+1'
   bytes (setting the last byte to the null terminating character) and you can
   fill in the data yourself.  If `str' is non-NULL then the resulting
   PyString object must be treated as immutable and you must not fill in nor
   alter the data yourself, since the strings may be shared.

   The PyObject member `op->ob_size', which denotes the number of "extra
   items" in a variable-size object, will contain the number of bytes
   allocated for string data, not counting the null terminating character.
   It is therefore equal to the `size' parameter (for
   PyString_FromStringAndSize()) or the length of the string in the `str'
   parameter (for PyString_FromString()).
*/
PyObject *
PyString_FromStringAndSize(const char *str, Py_ssize_t size)
{
    register PyStringObject *op;
    if (size < 0) {
        PyErr_SetString(PyExc_SystemError,
            "Negative size passed to PyString_FromStringAndSize");
        return NULL;
    }
    if (size == 0 && (op = nullstring) != NULL) {
#ifdef COUNT_ALLOCS
        null_strings++;
#endif
        Py_INCREF(op);
        return (PyObject *)op;
    }
    if (size == 1 && str != NULL &&
        (op = characters[*str & UCHAR_MAX]) != NULL)
    {
#ifdef COUNT_ALLOCS
        one_strings++;
#endif
        Py_INCREF(op);
        return (PyObject *)op;
    }

    if (size > PY_SSIZE_T_MAX - PyStringObject_SIZE) {
        PyErr_SetString(PyExc_OverflowError, "string is too large");
        return NULL;
    }

    /* Inline PyObject_NewVar */
    op = (PyStringObject *)PyObject_MALLOC(PyStringObject_SIZE + size);
    if (op == NULL)
        return PyErr_NoMemory();
    PyObject_INIT_VAR(op, &PyString_Type, size);
    op->ob_shash = -1;
    op->ob_sstate = SSTATE_NOT_INTERNED;
    if (str != NULL)
        Py_MEMCPY(op->ob_sval, str, size);
    op->ob_sval[size] = '\0';
    /* share short strings */
    if (size == 0) {
        PyObject *t = (PyObject *)op;
        PyString_InternInPlace(&t);
        op = (PyStringObject *)t;
        nullstring = op;
        Py_INCREF(op);
    } else if (size == 1 && str != NULL) {
        PyObject *t = (PyObject *)op;
        PyString_InternInPlace(&t);
        op = (PyStringObject *)t;
        characters[*str & UCHAR_MAX] = op;
        Py_INCREF(op);
    }
    return (PyObject *) op;
}

PyObject *
PyString_FromString(const char *str)
{
    register size_t size;
    register PyStringObject *op;

    assert(str != NULL);
    size = strlen(str);
    if (size > PY_SSIZE_T_MAX - PyStringObject_SIZE) {
        PyErr_SetString(PyExc_OverflowError,
            "string is too long for a Python string");
        return NULL;
    }
    if (size == 0 && (op = nullstring) != NULL) {
#ifdef COUNT_ALLOCS
        null_strings++;
#endif
        Py_INCREF(op);
        return (PyObject *)op;
    }
    if (size == 1 && (op = characters[*str & UCHAR_MAX]) != NULL) {
#ifdef COUNT_ALLOCS
        one_strings++;
#endif
        Py_INCREF(op);
        return (PyObject *)op;
    }

    /* Inline PyObject_NewVar */
    op = (PyStringObject *)PyObject_MALLOC(PyStringObject_SIZE + size);
    if (op == NULL)
        return PyErr_NoMemory();
    PyObject_INIT_VAR(op, &PyString_Type, size);
    op->ob_shash = -1;
    op->ob_sstate = SSTATE_NOT_INTERNED;
    Py_MEMCPY(op->ob_sval, str, size+1);
    /* share short strings */
    if (size == 0) {
        PyObject *t = (PyObject *)op;
        PyString_InternInPlace(&t);
        op = (PyStringObject *)t;
        nullstring = op;
        Py_INCREF(op);
    } else if (size == 1) {
        PyObject *t = (PyObject *)op;
        PyString_InternInPlace(&t);
        op = (PyStringObject *)t;
        characters[*str & UCHAR_MAX] = op;
        Py_INCREF(op);
    }
    return (PyObject *) op;
}

PyObject *
PyString_FromFormatV(const char *format, va_list vargs)
{
    va_list count;
    Py_ssize_t n = 0;
    const char* f;
    char *s;
    PyObject* string;

#ifdef VA_LIST_IS_ARRAY
    Py_MEMCPY(count, vargs, sizeof(va_list));
#else
#ifdef  __va_copy
    __va_copy(count, vargs);
#else
    count = vargs;
#endif
#endif
    /* step 1: figure out how large a buffer we need */
    for (f = format; *f; f++) {
        if (*f == '%') {
#ifdef HAVE_LONG_LONG
            int longlongflag = 0;
#endif
            const char* p = f;
            while (*++f && *f != '%' && !isalpha(Py_CHARMASK(*f)))
                ;

            /* skip the 'l' or 'z' in {%ld, %zd, %lu, %zu} since
             * they don't affect the amount of space we reserve.
             */
            if (*f == 'l') {
                if (f[1] == 'd' || f[1] == 'u') {
                    ++f;
                }
#ifdef HAVE_LONG_LONG
                else if (f[1] == 'l' &&
                         (f[2] == 'd' || f[2] == 'u')) {
                    longlongflag = 1;
                    f += 2;
                }
#endif
            }
            else if (*f == 'z' && (f[1] == 'd' || f[1] == 'u')) {
                ++f;
            }

            switch (*f) {
            case 'c':
                (void)va_arg(count, int);
                /* fall through... */
            case '%':
                n++;
                break;
            case 'd': case 'u': case 'i': case 'x':
                (void) va_arg(count, int);
#ifdef HAVE_LONG_LONG
                /* Need at most
                   ceil(log10(256)*SIZEOF_LONG_LONG) digits,
                   plus 1 for the sign.  53/22 is an upper
                   bound for log10(256). */
                if (longlongflag)
                    n += 2 + (SIZEOF_LONG_LONG*53-1) / 22;
                else
#endif
                    /* 20 bytes is enough to hold a 64-bit
                       integer.  Decimal takes the most
                       space.  This isn't enough for
                       octal. */
                    n += 20;

                break;
            case 's':
                s = va_arg(count, char*);
                n += strlen(s);
                break;
            case 'p':
                (void) va_arg(count, int);
                /* maximum 64-bit pointer representation:
                 * 0xffffffffffffffff
                 * so 19 characters is enough.
                 * XXX I count 18 -- what's the extra for?
                 */
                n += 19;
                break;
            default:
                /* if we stumble upon an unknown
                   formatting code, copy the rest of
                   the format string to the output
                   string. (we cannot just skip the
                   code, since there's no way to know
                   what's in the argument list) */
                n += strlen(p);
                goto expand;
            }
        } else
            n++;
    }
 expand:
    /* step 2: fill the buffer */
    /* Since we've analyzed how much space we need for the worst case,
       use sprintf directly instead of the slower PyOS_snprintf. */
    string = PyString_FromStringAndSize(NULL, n);
    if (!string)
        return NULL;

    s = PyString_AsString(string);

    for (f = format; *f; f++) {
        if (*f == '%') {
            const char* p = f++;
            Py_ssize_t i;
            int longflag = 0;
#ifdef HAVE_LONG_LONG
            int longlongflag = 0;
#endif
            int size_tflag = 0;
            /* parse the width.precision part (we're only
               interested in the precision value, if any) */
            n = 0;
            while (isdigit(Py_CHARMASK(*f)))
                n = (n*10) + *f++ - '0';
            if (*f == '.') {
                f++;
                n = 0;
                while (isdigit(Py_CHARMASK(*f)))
                    n = (n*10) + *f++ - '0';
            }
            while (*f && *f != '%' && !isalpha(Py_CHARMASK(*f)))
                f++;
            /* Handle %ld, %lu, %lld and %llu. */
            if (*f == 'l') {
                if (f[1] == 'd' || f[1] == 'u') {
                    longflag = 1;
                    ++f;
                }
#ifdef HAVE_LONG_LONG
                else if (f[1] == 'l' &&
                         (f[2] == 'd' || f[2] == 'u')) {
                    longlongflag = 1;
                    f += 2;
                }
#endif
            }
            /* handle the size_t flag. */
            else if (*f == 'z' && (f[1] == 'd' || f[1] == 'u')) {
                size_tflag = 1;
                ++f;
            }

            switch (*f) {
            case 'c':
                *s++ = va_arg(vargs, int);
                break;
            case 'd':
                if (longflag)
                    sprintf(s, "%ld", va_arg(vargs, long));
#ifdef HAVE_LONG_LONG
                else if (longlongflag)
                    sprintf(s, "%" PY_FORMAT_LONG_LONG "d",
                        va_arg(vargs, PY_LONG_LONG));
#endif
                else if (size_tflag)
                    sprintf(s, "%" PY_FORMAT_SIZE_T "d",
                        va_arg(vargs, Py_ssize_t));
                else
                    sprintf(s, "%d", va_arg(vargs, int));
                s += strlen(s);
                break;
            case 'u':
                if (longflag)
                    sprintf(s, "%lu",
                        va_arg(vargs, unsigned long));
#ifdef HAVE_LONG_LONG
                else if (longlongflag)
                    sprintf(s, "%" PY_FORMAT_LONG_LONG "u",
                        va_arg(vargs, PY_LONG_LONG));
#endif
                else if (size_tflag)
                    sprintf(s, "%" PY_FORMAT_SIZE_T "u",
                        va_arg(vargs, size_t));
                else
                    sprintf(s, "%u",
                        va_arg(vargs, unsigned int));
                s += strlen(s);
                break;
            case 'i':
                sprintf(s, "%i", va_arg(vargs, int));
                s += strlen(s);
                break;
            case 'x':
                sprintf(s, "%x", va_arg(vargs, int));
                s += strlen(s);
                break;
            case 's':
                p = va_arg(vargs, char*);
                i = strlen(p);
                if (n > 0 && i > n)
                    i = n;
                Py_MEMCPY(s, p, i);
                s += i;
                break;
            case 'p':
                sprintf(s, "%p", va_arg(vargs, void*));
                /* %p is ill-defined:  ensure leading 0x. */
                if (s[1] == 'X')
                    s[1] = 'x';
                else if (s[1] != 'x') {
                    memmove(s+2, s, strlen(s)+1);
                    s[0] = '0';
                    s[1] = 'x';
                }
                s += strlen(s);
                break;
            case '%':
                *s++ = '%';
                break;
            default:
                strcpy(s, p);
                s += strlen(s);
                goto end;
            }
        } else
            *s++ = *f;
    }

 end:
    if (_PyString_Resize(&string, s - PyString_AS_STRING(string)))
        return NULL;
    return string;
}

PyObject *
PyString_FromFormat(const char *format, ...)
{
    PyObject* ret;
    va_list vargs;

#ifdef HAVE_STDARG_PROTOTYPES
    va_start(vargs, format);
#else
    va_start(vargs);
#endif
    ret = PyString_FromFormatV(format, vargs);
    va_end(vargs);
    return ret;
}


PyObject *PyString_Decode(const char *s,
                          Py_ssize_t size,
                          const char *encoding,
                          const char *errors)
{
    PyObject *v, *str;

    str = PyString_FromStringAndSize(s, size);
    if (str == NULL)
        return NULL;
    v = PyString_AsDecodedString(str, encoding, errors);
    Py_DECREF(str);
    return v;
}

PyObject *PyString_AsDecodedObject(PyObject *str,
                                   const char *encoding,
                                   const char *errors)
{
    PyObject *v;

    if (!PyString_Check(str)) {
        PyErr_BadArgument();
        goto onError;
    }

    if (encoding == NULL) {
#ifdef Py_USING_UNICODE
        encoding = PyUnicode_GetDefaultEncoding();
#else
        PyErr_SetString(PyExc_ValueError, "no encoding specified");
        goto onError;
#endif
    }

    /* Decode via the codec registry */
    v = PyCodec_Decode(str, encoding, errors);
    if (v == NULL)
        goto onError;

    return v;

 onError:
    return NULL;
}

PyObject *PyString_AsDecodedString(PyObject *str,
                                   const char *encoding,
                                   const char *errors)
{
    PyObject *v;

    v = PyString_AsDecodedObject(str, encoding, errors);
    if (v == NULL)
        goto onError;

#ifdef Py_USING_UNICODE
    /* Convert Unicode to a string using the default encoding */
    if (PyUnicode_Check(v)) {
        PyObject *temp = v;
        v = PyUnicode_AsEncodedString(v, NULL, NULL);
        Py_DECREF(temp);
        if (v == NULL)
            goto onError;
    }
#endif
    if (!PyString_Check(v)) {
        PyErr_Format(PyExc_TypeError,
                     "decoder did not return a string object (type=%.400s)",
                     Py_TYPE(v)->tp_name);
        Py_DECREF(v);
        goto onError;
    }

    return v;

 onError:
    return NULL;
}

PyObject *PyString_Encode(const char *s,
                          Py_ssize_t size,
                          const char *encoding,
                          const char *errors)
{
    PyObject *v, *str;

    str = PyString_FromStringAndSize(s, size);
    if (str == NULL)
        return NULL;
    v = PyString_AsEncodedString(str, encoding, errors);
    Py_DECREF(str);
    return v;
}

PyObject *PyString_AsEncodedObject(PyObject *str,
                                   const char *encoding,
                                   const char *errors)
{
    PyObject *v;

    if (!PyString_Check(str)) {
        PyErr_BadArgument();
        goto onError;
    }

    if (encoding == NULL) {
#ifdef Py_USING_UNICODE
        encoding = PyUnicode_GetDefaultEncoding();
#else
        PyErr_SetString(PyExc_ValueError, "no encoding specified");
        goto onError;
#endif
    }

    /* Encode via the codec registry */
    v = PyCodec_Encode(str, encoding, errors);
    if (v == NULL)
        goto onError;

    return v;

 onError:
    return NULL;
}

PyObject *PyString_AsEncodedString(PyObject *str,
                                   const char *encoding,
                                   const char *errors)
{
    PyObject *v;

    v = PyString_AsEncodedObject(str, encoding, errors);
    if (v == NULL)
        goto onError;

#ifdef Py_USING_UNICODE
    /* Convert Unicode to a string using the default encoding */
    if (PyUnicode_Check(v)) {
        PyObject *temp = v;
        v = PyUnicode_AsEncodedString(v, NULL, NULL);
        Py_DECREF(temp);
        if (v == NULL)
            goto onError;
    }
#endif
    if (!PyString_Check(v)) {
        PyErr_Format(PyExc_TypeError,
                     "encoder did not return a string object (type=%.400s)",
                     Py_TYPE(v)->tp_name);
        Py_DECREF(v);
        goto onError;
    }

    return v;

 onError:
    return NULL;
}

static void
string_dealloc(PyObject *op)
{
    switch (PyString_CHECK_INTERNED(op)) {
        case SSTATE_NOT_INTERNED:
            break;

        case SSTATE_INTERNED_MORTAL:
            /* revive dead object temporarily for DelItem */
            Py_REFCNT(op) = 3;
            if (PyDict_DelItem(interned, op) != 0)
                Py_FatalError(
                    "deletion of interned string failed");
            break;

        case SSTATE_INTERNED_IMMORTAL:
            Py_FatalError("Immortal interned string died.");

        default:
            Py_FatalError("Inconsistent interned string state.");
    }
    Py_TYPE(op)->tp_free(op);
}

/* Unescape a backslash-escaped string. If unicode is non-zero,
   the string is a u-literal. If recode_encoding is non-zero,
   the string is UTF-8 encoded and should be re-encoded in the
   specified encoding.  */

PyObject *PyString_DecodeEscape(const char *s,
                                Py_ssize_t len,
                                const char *errors,
                                Py_ssize_t unicode,
                                const char *recode_encoding)
{
    int c;
    char *p, *buf;
    const char *end;
    PyObject *v;
    Py_ssize_t newlen = recode_encoding ? 4*len:len;
    v = PyString_FromStringAndSize((char *)NULL, newlen);
    if (v == NULL)
        return NULL;
    p = buf = PyString_AsString(v);
    end = s + len;
    while (s < end) {
        if (*s != '\\') {
          non_esc:
#ifdef Py_USING_UNICODE
            if (recode_encoding && (*s & 0x80)) {
                PyObject *u, *w;
                char *r;
                const char* t;
                Py_ssize_t rn;
                t = s;
                /* Decode non-ASCII bytes as UTF-8. */
                while (t < end && (*t & 0x80)) t++;
                u = PyUnicode_DecodeUTF8(s, t - s, errors);
                if(!u) goto failed;

                /* Recode them in target encoding. */
                w = PyUnicode_AsEncodedString(
                    u, recode_encoding, errors);
                Py_DECREF(u);
                if (!w)                 goto failed;

                /* Append bytes to output buffer. */
                assert(PyString_Check(w));
                r = PyString_AS_STRING(w);
                rn = PyString_GET_SIZE(w);
                Py_MEMCPY(p, r, rn);
                p += rn;
                Py_DECREF(w);
                s = t;
            } else {
                *p++ = *s++;
            }
#else
            *p++ = *s++;
#endif
            continue;
        }
        s++;
        if (s==end) {
            PyErr_SetString(PyExc_ValueError,
                            "Trailing \\ in string");
            goto failed;
        }
        switch (*s++) {
        /* XXX This assumes ASCII! */
        case '\n': break;
        case '\\': *p++ = '\\'; break;
        case '\'': *p++ = '\''; break;
        case '\"': *p++ = '\"'; break;
        case 'b': *p++ = '\b'; break;
        case 'f': *p++ = '\014'; break; /* FF */
        case 't': *p++ = '\t'; break;
        case 'n': *p++ = '\n'; break;
        case 'r': *p++ = '\r'; break;
        case 'v': *p++ = '\013'; break; /* VT */
        case 'a': *p++ = '\007'; break; /* BEL, not classic C */
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7':
            c = s[-1] - '0';
            if (s < end && '0' <= *s && *s <= '7') {
                c = (c<<3) + *s++ - '0';
                if (s < end && '0' <= *s && *s <= '7')
                    c = (c<<3) + *s++ - '0';
            }
            *p++ = c;
            break;
        case 'x':
            if (s+1 < end &&
                isxdigit(Py_CHARMASK(s[0])) &&
                isxdigit(Py_CHARMASK(s[1])))
            {
                unsigned int x = 0;
                c = Py_CHARMASK(*s);
                s++;
                if (isdigit(c))
                    x = c - '0';
                else if (islower(c))
                    x = 10 + c - 'a';
                else
                    x = 10 + c - 'A';
                x = x << 4;
                c = Py_CHARMASK(*s);
                s++;
                if (isdigit(c))
                    x += c - '0';
                else if (islower(c))
                    x += 10 + c - 'a';
                else
                    x += 10 + c - 'A';
                *p++ = x;
                break;
            }
            if (!errors || strcmp(errors, "strict") == 0) {
                PyErr_SetString(PyExc_ValueError,
                                "invalid \\x escape");
                goto failed;
            }
            if (strcmp(errors, "replace") == 0) {
                *p++ = '?';
            } else if (strcmp(errors, "ignore") == 0)
                /* do nothing */;
            else {
                PyErr_Format(PyExc_ValueError,
                             "decoding error; "
                             "unknown error handling code: %.400s",
                             errors);
                goto failed;
            }
#ifndef Py_USING_UNICODE
        case 'u':
        case 'U':
        case 'N':
            if (unicode) {
                PyErr_SetString(PyExc_ValueError,
                          "Unicode escapes not legal "
                          "when Unicode disabled");
                goto failed;
            }
#endif
        default:
            *p++ = '\\';
            s--;
            goto non_esc; /* an arbitrary number of unescaped
                             UTF-8 bytes may follow. */
        }
    }
    if (p-buf < newlen && _PyString_Resize(&v, p - buf))
        goto failed;
    return v;
  failed:
    Py_DECREF(v);
    return NULL;
}

/* -------------------------------------------------------------------- */
/* object api */

static Py_ssize_t
string_getsize(register PyObject *op)
{
    char *s;
    Py_ssize_t len;
    if (PyString_AsStringAndSize(op, &s, &len))
        return -1;
    return len;
}

static /*const*/ char *
string_getbuffer(register PyObject *op)
{
    char *s;
    Py_ssize_t len;
    if (PyString_AsStringAndSize(op, &s, &len))
        return NULL;
    return s;
}

Py_ssize_t
PyString_Size(register PyObject *op)
{
    if (!PyString_Check(op))
        return string_getsize(op);
    return Py_SIZE(op);
}

/*const*/ char *
PyString_AsString(register PyObject *op)
{
    if (!PyString_Check(op))
        return string_getbuffer(op);
    return ((PyStringObject *)op) -> ob_sval;
}

int
PyString_AsStringAndSize(register PyObject *obj,
                         register char **s,
                         register Py_ssize_t *len)
{
    if (s == NULL) {
        PyErr_BadInternalCall();
        return -1;
    }

    if (!PyString_Check(obj)) {
#ifdef Py_USING_UNICODE
        if (PyUnicode_Check(obj)) {
            obj = _PyUnicode_AsDefaultEncodedString(obj, NULL);
            if (obj == NULL)
                return -1;
        }
        else
#endif
        {
            PyErr_Format(PyExc_TypeError,
                         "expected string or Unicode object, "
                         "%.200s found", Py_TYPE(obj)->tp_name);
            return -1;
        }
    }

    *s = PyString_AS_STRING(obj);
    if (len != NULL)
        *len = PyString_GET_SIZE(obj);
    else if (strlen(*s) != (size_t)PyString_GET_SIZE(obj)) {
        PyErr_SetString(PyExc_TypeError,
                        "expected string without null bytes");
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------- */
/* Methods */

#include "stringlib/stringdefs.h"
#include "stringlib/fastsearch.h"

#include "stringlib/count.h"
#include "stringlib/find.h"
#include "stringlib/partition.h"
#include "stringlib/split.h"

#define _Py_InsertThousandsGrouping _PyString_InsertThousandsGrouping
#include "stringlib/localeutil.h"



static int
string_print(PyStringObject *op, FILE *fp, int flags)
{
    Py_ssize_t i, str_len;
    char c;
    int quote;

    /* XXX Ought to check for interrupts when writing long strings */
    if (! PyString_CheckExact(op)) {
        int ret;
        /* A str subclass may have its own __str__ method. */
        op = (PyStringObject *) PyObject_Str((PyObject *)op);
        if (op == NULL)
            return -1;
        ret = string_print(op, fp, flags);
        Py_DECREF(op);
        return ret;
    }
    if (flags & Py_PRINT_RAW) {
        char *data = op->ob_sval;
        Py_ssize_t size = Py_SIZE(op);
        Py_BEGIN_ALLOW_THREADS
        while (size > INT_MAX) {
            /* Very long strings cannot be written atomically.
             * But don't write exactly INT_MAX bytes at a time
             * to avoid memory aligment issues.
             */
            const int chunk_size = INT_MAX & ~0x3FFF;
            fwrite(data, 1, chunk_size, fp);
            data += chunk_size;
            size -= chunk_size;
        }
#ifdef __VMS
        if (size) fwrite(data, (int)size, 1, fp);
#else
        fwrite(data, 1, (int)size, fp);
#endif
        Py_END_ALLOW_THREADS
        return 0;
    }

    /* figure out which quote to use; single is preferred */
    quote = '\'';
    if (memchr(op->ob_sval, '\'', Py_SIZE(op)) &&
        !memchr(op->ob_sval, '"', Py_SIZE(op)))
        quote = '"';

    str_len = Py_SIZE(op);
    Py_BEGIN_ALLOW_THREADS
    fputc(quote, fp);
    for (i = 0; i < str_len; i++) {
        /* Since strings are immutable and the caller should have a
        reference, accessing the interal buffer should not be an issue
        with the GIL released. */
        c = op->ob_sval[i];
        if (c == quote || c == '\\')
            fprintf(fp, "\\%c", c);
        else if (c == '\t')
            fprintf(fp, "\\t");
        else if (c == '\n')
            fprintf(fp, "\\n");
        else if (c == '\r')
            fprintf(fp, "\\r");
        else if (c < ' ' || c >= 0x7f)
            fprintf(fp, "\\x%02x", c & 0xff);
        else
            fputc(c, fp);
    }
    fputc(quote, fp);
    Py_END_ALLOW_THREADS
    return 0;
}

PyObject *
PyString_Repr(PyObject *obj, int smartquotes)
{
    register PyStringObject* op = (PyStringObject*) obj;
    size_t newsize = 2 + 4 * Py_SIZE(op);
    PyObject *v;
    if (newsize > PY_SSIZE_T_MAX || newsize / 4 != Py_SIZE(op)) {
        PyErr_SetString(PyExc_OverflowError,
            "string is too large to make repr");
        return NULL;
    }
    v = PyString_FromStringAndSize((char *)NULL, newsize);
    if (v == NULL) {
        return NULL;
    }
    else {
        register Py_ssize_t i;
        register char c;
        register char *p;
        int quote;

        /* figure out which quote to use; single is preferred */
        quote = '\'';
        if (smartquotes &&
            memchr(op->ob_sval, '\'', Py_SIZE(op)) &&
            !memchr(op->ob_sval, '"', Py_SIZE(op)))
            quote = '"';

        p = PyString_AS_STRING(v);
        *p++ = quote;
        for (i = 0; i < Py_SIZE(op); i++) {
            /* There's at least enough room for a hex escape
               and a closing quote. */
            assert(newsize - (p - PyString_AS_STRING(v)) >= 5);
            c = op->ob_sval[i];
            if (c == quote || c == '\\')
                *p++ = '\\', *p++ = c;
            else if (c == '\t')
                *p++ = '\\', *p++ = 't';
            else if (c == '\n')
                *p++ = '\\', *p++ = 'n';
            else if (c == '\r')
                *p++ = '\\', *p++ = 'r';
            else if (c < ' ' || c >= 0x7f) {
                /* For performance, we don't want to call
                   PyOS_snprintf here (extra layers of
                   function call). */
                sprintf(p, "\\x%02x", c & 0xff);
                p += 4;
            }
            else
                *p++ = c;
        }
        assert(newsize - (p - PyString_AS_STRING(v)) >= 1);
        *p++ = quote;
        *p = '\0';
        if (_PyString_Resize(&v, (p - PyString_AS_STRING(v))))
            return NULL;
        return v;
    }
}

static PyObject *
string_repr(PyObject *op)
{
    return PyString_Repr(op, 1);
}

static PyObject *
string_str(PyObject *s)
{
    assert(PyString_Check(s));
    if (PyString_CheckExact(s)) {
        Py_INCREF(s);
        return s;
    }
    else {
        /* Subtype -- return genuine string with the same value. */
        PyStringObject *t = (PyStringObject *) s;
        return PyString_FromStringAndSize(t->ob_sval, Py_SIZE(t));
    }
}

static Py_ssize_t
string_length(PyStringObject *a)
{
    return Py_SIZE(a);
}

static PyObject *
string_concat(register PyStringObject *a, register PyObject *bb)
{
    register Py_ssize_t size;
    register PyStringObject *op;
    if (!PyString_Check(bb)) {
#ifdef Py_USING_UNICODE
        if (PyUnicode_Check(bb))
            return PyUnicode_Concat((PyObject *)a, bb);
#endif
        if (PyByteArray_Check(bb))
            return PyByteArray_Concat((PyObject *)a, bb);
        PyErr_Format(PyExc_TypeError,
                     "cannot concatenate 'str' and '%.200s' objects",
                     Py_TYPE(bb)->tp_name);
        return NULL;
    }
#define b ((PyStringObject *)bb)
    /* Optimize cases with empty left or right operand */
    if ((Py_SIZE(a) == 0 || Py_SIZE(b) == 0) &&
        PyString_CheckExact(a) && PyString_CheckExact(b)) {
        if (Py_SIZE(a) == 0) {
            Py_INCREF(bb);
            return bb;
        }
        Py_INCREF(a);
        return (PyObject *)a;
    }
    size = Py_SIZE(a) + Py_SIZE(b);
    /* Check that string sizes are not negative, to prevent an
       overflow in cases where we are passed incorrectly-created
       strings with negative lengths (due to a bug in other code).
    */
    if (Py_SIZE(a) < 0 || Py_SIZE(b) < 0 ||
        Py_SIZE(a) > PY_SSIZE_T_MAX - Py_SIZE(b)) {
        PyErr_SetString(PyExc_OverflowError,
                        "strings are too large to concat");
        return NULL;
    }

    /* Inline PyObject_NewVar */
    if (size > PY_SSIZE_T_MAX - PyStringObject_SIZE) {
        PyErr_SetString(PyExc_OverflowError,
                        "strings are too large to concat");
        return NULL;
    }
    op = (PyStringObject *)PyObject_MALLOC(PyStringObject_SIZE + size);
    if (op == NULL)
        return PyErr_NoMemory();
    PyObject_INIT_VAR(op, &PyString_Type, size);
    op->ob_shash = -1;
    op->ob_sstate = SSTATE_NOT_INTERNED;
    Py_MEMCPY(op->ob_sval, a->ob_sval, Py_SIZE(a));
    Py_MEMCPY(op->ob_sval + Py_SIZE(a), b->ob_sval, Py_SIZE(b));
    op->ob_sval[size] = '\0';
    return (PyObject *) op;
#undef b
}

static PyObject *
string_repeat(register PyStringObject *a, register Py_ssize_t n)
{
    register Py_ssize_t i;
    register Py_ssize_t j;
    register Py_ssize_t size;
    register PyStringObject *op;
    size_t nbytes;
    if (n < 0)
        n = 0;
    /* watch out for overflows:  the size can overflow int,
     * and the # of bytes needed can overflow size_t
     */
    size = Py_SIZE(a) * n;
    if (n && size / n != Py_SIZE(a)) {
        PyErr_SetString(PyExc_OverflowError,
            "repeated string is too long");
        return NULL;
    }
    if (size == Py_SIZE(a) && PyString_CheckExact(a)) {
        Py_INCREF(a);
        return (PyObject *)a;
    }
    nbytes = (size_t)size;
    if (nbytes + PyStringObject_SIZE <= nbytes) {
        PyErr_SetString(PyExc_OverflowError,
            "repeated string is too long");
        return NULL;
    }
    op = (PyStringObject *)PyObject_MALLOC(PyStringObject_SIZE + nbytes);
    if (op == NULL)
        return PyErr_NoMemory();
    PyObject_INIT_VAR(op, &PyString_Type, size);
    op->ob_shash = -1;
    op->ob_sstate = SSTATE_NOT_INTERNED;
    op->ob_sval[size] = '\0';
    if (Py_SIZE(a) == 1 && n > 0) {
        memset(op->ob_sval, a->ob_sval[0] , n);
        return (PyObject *) op;
    }
    i = 0;
    if (i < size) {
        Py_MEMCPY(op->ob_sval, a->ob_sval, Py_SIZE(a));
        i = Py_SIZE(a);
    }
    while (i < size) {
        j = (i <= size-i)  ?  i  :  size-i;
        Py_MEMCPY(op->ob_sval+i, op->ob_sval, j);
        i += j;
    }
    return (PyObject *) op;
}

/* String slice a[i:j] consists of characters a[i] ... a[j-1] */

static PyObject *
string_slice(register PyStringObject *a, register Py_ssize_t i,
             register Py_ssize_t j)
     /* j -- may be negative! */
{
    if (i < 0)
        i = 0;
    if (j < 0)
        j = 0; /* Avoid signed/unsigned bug in next line */
    if (j > Py_SIZE(a))
        j = Py_SIZE(a);
    if (i == 0 && j == Py_SIZE(a) && PyString_CheckExact(a)) {
        /* It's the same as a */
        Py_INCREF(a);
        return (PyObject *)a;
    }
    if (j < i)
        j = i;
    return PyString_FromStringAndSize(a->ob_sval + i, j-i);
}

static int
string_contains(PyObject *str_obj, PyObject *sub_obj)
{
    if (!PyString_CheckExact(sub_obj)) {
#ifdef Py_USING_UNICODE
        if (PyUnicode_Check(sub_obj))
            return PyUnicode_Contains(str_obj, sub_obj);
#endif
        if (!PyString_Check(sub_obj)) {
            PyErr_Format(PyExc_TypeError,
                "'in <string>' requires string as left operand, "
                "not %.200s", Py_TYPE(sub_obj)->tp_name);
            return -1;
        }
    }

    return stringlib_contains_obj(str_obj, sub_obj);
}

static PyObject *
string_item(PyStringObject *a, register Py_ssize_t i)
{
    char pchar;
    PyObject *v;
    if (i < 0 || i >= Py_SIZE(a)) {
        PyErr_SetString(PyExc_IndexError, "string index out of range");
        return NULL;
    }
    pchar = a->ob_sval[i];
    v = (PyObject *)characters[pchar & UCHAR_MAX];
    if (v == NULL)
        v = PyString_FromStringAndSize(&pchar, 1);
    else {
#ifdef COUNT_ALLOCS
        one_strings++;
#endif
        Py_INCREF(v);
    }
    return v;
}

static PyObject*
string_richcompare(PyStringObject *a, PyStringObject *b, int op)
{
    int c;
    Py_ssize_t len_a, len_b;
    Py_ssize_t min_len;
    PyObject *result;

    /* Make sure both arguments are strings. */
    if (!(PyString_Check(a) && PyString_Check(b))) {
        result = Py_NotImplemented;
        goto out;
    }
    if (a == b) {
        switch (op) {
        case Py_EQ:case Py_LE:case Py_GE:
            result = Py_True;
            goto out;
        case Py_NE:case Py_LT:case Py_GT:
            result = Py_False;
            goto out;
        }
    }
    if (op == Py_EQ) {
        /* Supporting Py_NE here as well does not save
           much time, since Py_NE is rarely used.  */
        if (Py_SIZE(a) == Py_SIZE(b)
            && (a->ob_sval[0] == b->ob_sval[0]
            && memcmp(a->ob_sval, b->ob_sval, Py_SIZE(a)) == 0)) {
            result = Py_True;
        } else {
            result = Py_False;
        }
        goto out;
    }
    len_a = Py_SIZE(a); len_b = Py_SIZE(b);
    min_len = (len_a < len_b) ? len_a : len_b;
    if (min_len > 0) {
        c = Py_CHARMASK(*a->ob_sval) - Py_CHARMASK(*b->ob_sval);
        if (c==0)
            c = memcmp(a->ob_sval, b->ob_sval, min_len);
    } else
        c = 0;
    if (c == 0)
        c = (len_a < len_b) ? -1 : (len_a > len_b) ? 1 : 0;
    switch (op) {
    case Py_LT: c = c <  0; break;
    case Py_LE: c = c <= 0; break;
    case Py_EQ: assert(0);  break; /* unreachable */
    case Py_NE: c = c != 0; break;
    case Py_GT: c = c >  0; break;
    case Py_GE: c = c >= 0; break;
    default:
        result = Py_NotImplemented;
        goto out;
    }
    result = c ? Py_True : Py_False;
  out:
    Py_INCREF(result);
    return result;
}

int
_PyString_Eq(PyObject *o1, PyObject *o2)
{
    PyStringObject *a = (PyStringObject*) o1;
    PyStringObject *b = (PyStringObject*) o2;
    return Py_SIZE(a) == Py_SIZE(b)
      && *a->ob_sval == *b->ob_sval
      && memcmp(a->ob_sval, b->ob_sval, Py_SIZE(a)) == 0;
}

long
tabu_hash(long x)
{
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
    unsigned short bytes[8];
    int i;
    for (i = 0; i < 8;i++)
    {
        bytes[i] = (unsigned long)(x&(256-1));
        x = x>>8;
        //printf("EEE %d\n",bytes[i]);
    }
    // for (i = 0; i<4;i++)
    // {
    //     printf("%d\n", bytes[i]);
    // }
    long xored_table = table0[bytes[0]] ^ table1[bytes[1]] ^ table2[bytes[2]] ^ table3[bytes[3]] ^ table4[bytes[4]] ^ table5[bytes[5]] ^ table6[bytes[6]] ^ table7[bytes[7]];
    // printf("%d\n", xored_table);
    return xored_table;
}

static long
string_hash(PyStringObject *a)
{
    register Py_ssize_t len;
    register unsigned char *p;
    register long x;

#ifdef Py_DEBUG
    assert(_Py_HashSecret_Initialized);
#endif
    if (a->ob_shash != -1)
        return a->ob_shash;
    len = Py_SIZE(a);
    /*
      We make the hash of the empty string be 0, rather than using
      (prefix ^ suffix), since this slightly obfuscates the hash secret
    */
    if (len == 0) {
        a->ob_shash = 0;
        return 0;
    }
    p = (unsigned char *) a->ob_sval;
    x = _Py_HashSecret.prefix;
    x ^= *p << 7;
    while (--len >= 0)
        x = (1000003*x) ^ *p++;

#ifdef TABULATION
    //printf("x:%ld\n", x);
    x = tabu_hash(x);
    //printf("t:%ld\n", x);
#endif

    x ^= Py_SIZE(a);
    x ^= _Py_HashSecret.suffix;
    if (x == -1)
        x = -2;
    a->ob_shash = x;
    return x;
}

static PyObject*
string_subscript(PyStringObject* self, PyObject* item)
{
    if (PyIndex_Check(item)) {
        Py_ssize_t i = PyNumber_AsSsize_t(item, PyExc_IndexError);
        if (i == -1 && PyErr_Occurred())
            return NULL;
        if (i < 0)
            i += PyString_GET_SIZE(self);
        return string_item(self, i);
    }
    else if (PySlice_Check(item)) {
        Py_ssize_t start, stop, step, slicelength, cur, i;
        char* source_buf;
        char* result_buf;
        PyObject* result;

        if (PySlice_GetIndicesEx((PySliceObject*)item,
                         PyString_GET_SIZE(self),
                         &start, &stop, &step, &slicelength) < 0) {
            return NULL;
        }

        if (slicelength <= 0) {
            return PyString_FromStringAndSize("", 0);
        }
        else if (start == 0 && step == 1 &&
                 slicelength == PyString_GET_SIZE(self) &&
                 PyString_CheckExact(self)) {
            Py_INCREF(self);
            return (PyObject *)self;
        }
        else if (step == 1) {
            return PyString_FromStringAndSize(
                PyString_AS_STRING(self) + start,
                slicelength);
        }
        else {
            source_buf = PyString_AsString((PyObject*)self);
            result_buf = (char *)PyMem_Malloc(slicelength);
            if (result_buf == NULL)
                return PyErr_NoMemory();

            for (cur = start, i = 0; i < slicelength;
                 cur += step, i++) {
                result_buf[i] = source_buf[cur];
            }

            result = PyString_FromStringAndSize(result_buf,
                                                slicelength);
            PyMem_Free(result_buf);
            return result;
        }
    }
    else {
        PyErr_Format(PyExc_TypeError,
                     "string indices must be integers, not %.200s",
                     Py_TYPE(item)->tp_name);
        return NULL;
    }
}

static Py_ssize_t
string_buffer_getreadbuf(PyStringObject *self, Py_ssize_t index, const void **ptr)
{
    if ( index != 0 ) {
        PyErr_SetString(PyExc_SystemError,
                        "accessing non-existent string segment");
        return -1;
    }
    *ptr = (void *)self->ob_sval;
    return Py_SIZE(self);
}

static Py_ssize_t
string_buffer_getwritebuf(PyStringObject *self, Py_ssize_t index, const void **ptr)
{
    PyErr_SetString(PyExc_TypeError,
                    "Cannot use string as modifiable buffer");
    return -1;
}

static Py_ssize_t
string_buffer_getsegcount(PyStringObject *self, Py_ssize_t *lenp)
{
    if ( lenp )
        *lenp = Py_SIZE(self);
    return 1;
}

static Py_ssize_t
string_buffer_getcharbuf(PyStringObject *self, Py_ssize_t index, const char **ptr)
{
    if ( index != 0 ) {
        PyErr_SetString(PyExc_SystemError,
                        "accessing non-existent string segment");
        return -1;
    }
    *ptr = self->ob_sval;
    return Py_SIZE(self);
}

static int
string_buffer_getbuffer(PyStringObject *self, Py_buffer *view, int flags)
{
    return PyBuffer_FillInfo(view, (PyObject*)self,
                             (void *)self->ob_sval, Py_SIZE(self),
                             1, flags);
}

static PySequenceMethods string_as_sequence = {
    (lenfunc)string_length, /*sq_length*/
    (binaryfunc)string_concat, /*sq_concat*/
    (ssizeargfunc)string_repeat, /*sq_repeat*/
    (ssizeargfunc)string_item, /*sq_item*/
    (ssizessizeargfunc)string_slice, /*sq_slice*/
    0,                  /*sq_ass_item*/
    0,                  /*sq_ass_slice*/
    (objobjproc)string_contains /*sq_contains*/
};

static PyMappingMethods string_as_mapping = {
    (lenfunc)string_length,
    (binaryfunc)string_subscript,
    0,
};

static PyBufferProcs string_as_buffer = {
    (readbufferproc)string_buffer_getreadbuf,
    (writebufferproc)string_buffer_getwritebuf,
    (segcountproc)string_buffer_getsegcount,
    (charbufferproc)string_buffer_getcharbuf,
    (getbufferproc)string_buffer_getbuffer,
    0, /* XXX */
};



#define LEFTSTRIP 0
#define RIGHTSTRIP 1
#define BOTHSTRIP 2

/* Arrays indexed by above */
static const char *stripformat[] = {"|O:lstrip", "|O:rstrip", "|O:strip"};

#define STRIPNAME(i) (stripformat[i]+3)

PyDoc_STRVAR(split__doc__,
"S.split([sep [,maxsplit]]) -> list of strings\n\
\n\
Return a list of the words in the string S, using sep as the\n\
delimiter string.  If maxsplit is given, at most maxsplit\n\
splits are done. If sep is not specified or is None, any\n\
whitespace string is a separator and empty strings are removed\n\
from the result.");

static PyObject *
string_split(PyStringObject *self, PyObject *args)
{
    Py_ssize_t len = PyString_GET_SIZE(self), n;
    Py_ssize_t maxsplit = -1;
    const char *s = PyString_AS_STRING(self), *sub;
    PyObject *subobj = Py_None;

    if (!PyArg_ParseTuple(args, "|On:split", &subobj, &maxsplit))
        return NULL;
    if (maxsplit < 0)
        maxsplit = PY_SSIZE_T_MAX;
    if (subobj == Py_None)
        return stringlib_split_whitespace((PyObject*) self, s, len, maxsplit);
    if (PyString_Check(subobj)) {
        sub = PyString_AS_STRING(subobj);
        n = PyString_GET_SIZE(subobj);
    }
#ifdef Py_USING_UNICODE
    else if (PyUnicode_Check(subobj))
        return PyUnicode_Split((PyObject *)self, subobj, maxsplit);
#endif
    else if (PyObject_AsCharBuffer(subobj, &sub, &n))
        return NULL;

    return stringlib_split((PyObject*) self, s, len, sub, n, maxsplit);
}

PyDoc_STRVAR(partition__doc__,
"S.partition(sep) -> (head, sep, tail)\n\
\n\
Search for the separator sep in S, and return the part before it,\n\
the separator itself, and the part after it.  If the separator is not\n\
found, return S and two empty strings.");

static PyObject *
string_partition(PyStringObject *self, PyObject *sep_obj)
{
    const char *sep;
    Py_ssize_t sep_len;

    if (PyString_Check(sep_obj)) {
        sep = PyString_AS_STRING(sep_obj);
        sep_len = PyString_GET_SIZE(sep_obj);
    }
#ifdef Py_USING_UNICODE
    else if (PyUnicode_Check(sep_obj))
        return PyUnicode_Partition((PyObject *) self, sep_obj);
#endif
    else if (PyObject_AsCharBuffer(sep_obj, &sep, &sep_len))
        return NULL;

    return stringlib_partition(
        (PyObject*) self,
        PyString_AS_STRING(self), PyString_GET_SIZE(self),
        sep_obj, sep, sep_len
        );
}

PyDoc_STRVAR(rpartition__doc__,
"S.rpartition(sep) -> (head, sep, tail)\n\
\n\
Search for the separator sep in S, starting at the end of S, and return\n\
the part before it, the separator itself, and the part after it.  If the\n\
separator is not found, return two empty strings and S.");

static PyObject *
string_rpartition(PyStringObject *self, PyObject *sep_obj)
{
    const char *sep;
    Py_ssize_t sep_len;

    if (PyString_Check(sep_obj)) {
        sep = PyString_AS_STRING(sep_obj);
        sep_len = PyString_GET_SIZE(sep_obj);
    }
#ifdef Py_USING_UNICODE
    else if (PyUnicode_Check(sep_obj))
        return PyUnicode_RPartition((PyObject *) self, sep_obj);
#endif
    else if (PyObject_AsCharBuffer(sep_obj, &sep, &sep_len))
        return NULL;

    return stringlib_rpartition(
        (PyObject*) self,
        PyString_AS_STRING(self), PyString_GET_SIZE(self),
        sep_obj, sep, sep_len
        );
}

PyDoc_STRVAR(rsplit__doc__,
"S.rsplit([sep [,maxsplit]]) -> list of strings\n\
\n\
Return a list of the words in the string S, using sep as the\n\
delimiter string, starting at the end of the string and working\n\
to the front.  If maxsplit is given, at most maxsplit splits are\n\
done. If sep is not specified or is None, any whitespace string\n\
is a separator.");

static PyObject *
string_rsplit(PyStringObject *self, PyObject *args)
{
    Py_ssize_t len = PyString_GET_SIZE(self), n;
    Py_ssize_t maxsplit = -1;
    const char *s = PyString_AS_STRING(self), *sub;
    PyObject *subobj = Py_None;

    if (!PyArg_ParseTuple(args, "|On:rsplit", &subobj, &maxsplit))
        return NULL;
    if (maxsplit < 0)
        maxsplit = PY_SSIZE_T_MAX;
    if (subobj == Py_None)
        return stringlib_rsplit_whitespace((PyObject*) self, s, len, maxsplit);
    if (PyString_Check(subobj)) {
        sub = PyString_AS_STRING(subobj);
        n = PyString_GET_SIZE(subobj);
    }
#ifdef Py_USING_UNICODE
    else if (PyUnicode_Check(subobj))
        return PyUnicode_RSplit((PyObject *)self, subobj, maxsplit);
#endif
    else if (PyObject_AsCharBuffer(subobj, &sub, &n))
        return NULL;

    return stringlib_rsplit((PyObject*) self, s, len, sub, n, maxsplit);
}


PyDoc_STRVAR(join__doc__,
"S.join(iterable) -> string\n\
\n\
Return a string which is the concatenation of the strings in the\n\
iterable.  The separator between elements is S.");

static PyObject *
string_join(PyStringObject *self, PyObject *orig)
{
    char *sep = PyString_AS_STRING(self);
    const Py_ssize_t seplen = PyString_GET_SIZE(self);
    PyObject *res = NULL;
    char *p;
    Py_ssize_t seqlen = 0;
    size_t sz = 0;
    Py_ssize_t i;
    PyObject *seq, *item;

    seq = PySequence_Fast(orig, "");
    if (seq == NULL) {
        return NULL;
    }

    seqlen = PySequence_Size(seq);
    if (seqlen == 0) {
        Py_DECREF(seq);
        return PyString_FromString("");
    }
    if (seqlen == 1) {
        item = PySequence_Fast_GET_ITEM(seq, 0);
        if (PyString_CheckExact(item) || PyUnicode_CheckExact(item)) {
            Py_INCREF(item);
            Py_DECREF(seq);
            return item;
        }
    }

    /* There are at least two things to join, or else we have a subclass
     * of the builtin types in the sequence.
     * Do a pre-pass to figure out the total amount of space we'll
     * need (sz), see whether any argument is absurd, and defer to
     * the Unicode join if appropriate.
     */
    for (i = 0; i < seqlen; i++) {
        const size_t old_sz = sz;
        item = PySequence_Fast_GET_ITEM(seq, i);
        if (!PyString_Check(item)){
#ifdef Py_USING_UNICODE
            if (PyUnicode_Check(item)) {
                /* Defer to Unicode join.
                 * CAUTION:  There's no gurantee that the
                 * original sequence can be iterated over
                 * again, so we must pass seq here.
                 */
                PyObject *result;
                result = PyUnicode_Join((PyObject *)self, seq);
                Py_DECREF(seq);
                return result;
            }
#endif
            PyErr_Format(PyExc_TypeError,
                         "sequence item %zd: expected string,"
                         " %.80s found",
                         i, Py_TYPE(item)->tp_name);
            Py_DECREF(seq);
            return NULL;
        }
        sz += PyString_GET_SIZE(item);
        if (i != 0)
            sz += seplen;
        if (sz < old_sz || sz > PY_SSIZE_T_MAX) {
            PyErr_SetString(PyExc_OverflowError,
                "join() result is too long for a Python string");
            Py_DECREF(seq);
            return NULL;
        }
    }

    /* Allocate result space. */
    res = PyString_FromStringAndSize((char*)NULL, sz);
    if (res == NULL) {
        Py_DECREF(seq);
        return NULL;
    }

    /* Catenate everything. */
    p = PyString_AS_STRING(res);
    for (i = 0; i < seqlen; ++i) {
        size_t n;
        item = PySequence_Fast_GET_ITEM(seq, i);
        n = PyString_GET_SIZE(item);
        Py_MEMCPY(p, PyString_AS_STRING(item), n);
        p += n;
        if (i < seqlen - 1) {
            Py_MEMCPY(p, sep, seplen);
            p += seplen;
        }
    }

    Py_DECREF(seq);
    return res;
}

PyObject *
_PyString_Join(PyObject *sep, PyObject *x)
{
    assert(sep != NULL && PyString_Check(sep));
    assert(x != NULL);
    return string_join((PyStringObject *)sep, x);
}

/* helper macro to fixup start/end slice values */
#define ADJUST_INDICES(start, end, len)         \
    if (end > len)                          \
        end = len;                          \
    else if (end < 0) {                     \
        end += len;                         \
        if (end < 0)                        \
        end = 0;                        \
    }                                       \
    if (start < 0) {                        \
        start += len;                       \
        if (start < 0)                      \
        start = 0;                      \
    }

Py_LOCAL_INLINE(Py_ssize_t)
string_find_internal(PyStringObject *self, PyObject *args, int dir)
{
    PyObject *subobj;
    const char *sub;
    Py_ssize_t sub_len;
    Py_ssize_t start=0, end=PY_SSIZE_T_MAX;

    if (!stringlib_parse_args_finds("find/rfind/index/rindex",
                                    args, &subobj, &start, &end))
        return -2;

    if (PyString_Check(subobj)) {
        sub = PyString_AS_STRING(subobj);
        sub_len = PyString_GET_SIZE(subobj);
    }
#ifdef Py_USING_UNICODE
    else if (PyUnicode_Check(subobj))
        return PyUnicode_Find(
            (PyObject *)self, subobj, start, end, dir);
#endif
    else if (PyObject_AsCharBuffer(subobj, &sub, &sub_len))
        /* XXX - the "expected a character buffer object" is pretty
           confusing for a non-expert.  remap to something else ? */
        return -2;

    if (dir > 0)
        return stringlib_find_slice(
            PyString_AS_STRING(self), PyString_GET_SIZE(self),
            sub, sub_len, start, end);
    else
        return stringlib_rfind_slice(
            PyString_AS_STRING(self), PyString_GET_SIZE(self),
            sub, sub_len, start, end);
}


PyDoc_STRVAR(find__doc__,
"S.find(sub [,start [,end]]) -> int\n\
\n\
Return the lowest index in S where substring sub is found,\n\
such that sub is contained within S[start:end].  Optional\n\
arguments start and end are interpreted as in slice notation.\n\
\n\
Return -1 on failure.");

static PyObject *
string_find(PyStringObject *self, PyObject *args)
{
    Py_ssize_t result = string_find_internal(self, args, +1);
    if (result == -2)
        return NULL;
    return PyInt_FromSsize_t(result);
}


PyDoc_STRVAR(index__doc__,
"S.index(sub [,start [,end]]) -> int\n\
\n\
Like S.find() but raise ValueError when the substring is not found.");

static PyObject *
string_index(PyStringObject *self, PyObject *args)
{
    Py_ssize_t result = string_find_internal(self, args, +1);
    if (result == -2)
        return NULL;
    if (result == -1) {
        PyErr_SetString(PyExc_ValueError,
                        "substring not found");
        return NULL;
    }
    return PyInt_FromSsize_t(result);
}


PyDoc_STRVAR(rfind__doc__,
"S.rfind(sub [,start [,end]]) -> int\n\
\n\
Return the highest index in S where substring sub is found,\n\
such that sub is contained within S[start:end].  Optional\n\
arguments start and end are interpreted as in slice notation.\n\
\n\
Return -1 on failure.");

static PyObject *
string_rfind(PyStringObject *self, PyObject *args)
{
    Py_ssize_t result = string_find_internal(self, args, -1);
    if (result == -2)
        return NULL;
    return PyInt_FromSsize_t(result);
}


PyDoc_STRVAR(rindex__doc__,
"S.rindex(sub [,start [,end]]) -> int\n\
\n\
Like S.rfind() but raise ValueError when the substring is not found.");

static PyObject *
string_rindex(PyStringObject *self, PyObject *args)
{
    Py_ssize_t result = string_find_internal(self, args, -1);
    if (result == -2)
        return NULL;
    if (result == -1) {
        PyErr_SetString(PyExc_ValueError,
                        "substring not found");
        return NULL;
    }
    return PyInt_FromSsize_t(result);
}


Py_LOCAL_INLINE(PyObject *)
do_xstrip(PyStringObject *self, int striptype, PyObject *sepobj)
{
    char *s = PyString_AS_STRING(self);
    Py_ssize_t len = PyString_GET_SIZE(self);
    char *sep = PyString_AS_STRING(sepobj);
    Py_ssize_t seplen = PyString_GET_SIZE(sepobj);
    Py_ssize_t i, j;

    i = 0;
    if (striptype != RIGHTSTRIP) {
        while (i < len && memchr(sep, Py_CHARMASK(s[i]), seplen)) {
            i++;
        }
    }

    j = len;
    if (striptype != LEFTSTRIP) {
        do {
            j--;
        } while (j >= i && memchr(sep, Py_CHARMASK(s[j]), seplen));
        j++;
    }

    if (i == 0 && j == len && PyString_CheckExact(self)) {
        Py_INCREF(self);
        return (PyObject*)self;
    }
    else
        return PyString_FromStringAndSize(s+i, j-i);
}


Py_LOCAL_INLINE(PyObject *)
do_strip(PyStringObject *self, int striptype)
{
    char *s = PyString_AS_STRING(self);
    Py_ssize_t len = PyString_GET_SIZE(self), i, j;

    i = 0;
    if (striptype != RIGHTSTRIP) {
        while (i < len && isspace(Py_CHARMASK(s[i]))) {
            i++;
        }
    }

    j = len;
    if (striptype != LEFTSTRIP) {
        do {
            j--;
        } while (j >= i && isspace(Py_CHARMASK(s[j])));
        j++;
    }

    if (i == 0 && j == len && PyString_CheckExact(self)) {
        Py_INCREF(self);
        return (PyObject*)self;
    }
    else
        return PyString_FromStringAndSize(s+i, j-i);
}


Py_LOCAL_INLINE(PyObject *)
do_argstrip(PyStringObject *self, int striptype, PyObject *args)
{
    PyObject *sep = NULL;

    if (!PyArg_ParseTuple(args, (char *)stripformat[striptype], &sep))
        return NULL;

    if (sep != NULL && sep != Py_None) {
        if (PyString_Check(sep))
            return do_xstrip(self, striptype, sep);
#ifdef Py_USING_UNICODE
        else if (PyUnicode_Check(sep)) {
            PyObject *uniself = PyUnicode_FromObject((PyObject *)self);
            PyObject *res;
            if (uniself==NULL)
                return NULL;
            res = _PyUnicode_XStrip((PyUnicodeObject *)uniself,
                striptype, sep);
            Py_DECREF(uniself);
            return res;
        }
#endif
        PyErr_Format(PyExc_TypeError,
#ifdef Py_USING_UNICODE
                     "%s arg must be None, str or unicode",
#else
                     "%s arg must be None or str",
#endif
                     STRIPNAME(striptype));
        return NULL;
    }

    return do_strip(self, striptype);
}


PyDoc_STRVAR(strip__doc__,
"S.strip([chars]) -> string or unicode\n\
\n\
Return a copy of the string S with leading and trailing\n\
whitespace removed.\n\
If chars is given and not None, remove characters in chars instead.\n\
If chars is unicode, S will be converted to unicode before stripping");

static PyObject *
string_strip(PyStringObject *self, PyObject *args)
{
    if (PyTuple_GET_SIZE(args) == 0)
        return do_strip(self, BOTHSTRIP); /* Common case */
    else
        return do_argstrip(self, BOTHSTRIP, args);
}


PyDoc_STRVAR(lstrip__doc__,
"S.lstrip([chars]) -> string or unicode\n\
\n\
Return a copy of the string S with leading whitespace removed.\n\
If chars is given and not None, remove characters in chars instead.\n\
If chars is unicode, S will be converted to unicode before stripping");

static PyObject *
string_lstrip(PyStringObject *self, PyObject *args)
{
    if (PyTuple_GET_SIZE(args) == 0)
        return do_strip(self, LEFTSTRIP); /* Common case */
    else
        return do_argstrip(self, LEFTSTRIP, args);
}


PyDoc_STRVAR(rstrip__doc__,
"S.rstrip([chars]) -> string or unicode\n\
\n\
Return a copy of the string S with trailing whitespace removed.\n\
If chars is given and not None, remove characters in chars instead.\n\
If chars is unicode, S will be converted to unicode before stripping");

static PyObject *
string_rstrip(PyStringObject *self, PyObject *args)
{
    if (PyTuple_GET_SIZE(args) == 0)
        return do_strip(self, RIGHTSTRIP); /* Common case */
    else
        return do_argstrip(self, RIGHTSTRIP, args);
}


PyDoc_STRVAR(lower__doc__,
"S.lower() -> string\n\
\n\
Return a copy of the string S converted to lowercase.");

/* _tolower and _toupper are defined by SUSv2, but they're not ISO C */
#ifndef _tolower
#define _tolower tolower
#endif

static PyObject *
string_lower(PyStringObject *self)
{
    char *s;
    Py_ssize_t i, n = PyString_GET_SIZE(self);
    PyObject *newobj;

    newobj = PyString_FromStringAndSize(NULL, n);
    if (!newobj)
        return NULL;

    s = PyString_AS_STRING(newobj);

    Py_MEMCPY(s, PyString_AS_STRING(self), n);

    for (i = 0; i < n; i++) {
        int c = Py_CHARMASK(s[i]);
        if (isupper(c))
            s[i] = _tolower(c);
    }

    return newobj;
}

PyDoc_STRVAR(upper__doc__,
"S.upper() -> string\n\
\n\
Return a copy of the string S converted to uppercase.");

#ifndef _toupper
#define _toupper toupper
#endif

static PyObject *
string_upper(PyStringObject *self)
{
    char *s;
    Py_ssize_t i, n = PyString_GET_SIZE(self);
    PyObject *newobj;

    newobj = PyString_FromStringAndSize(NULL, n);
    if (!newobj)
        return NULL;

    s = PyString_AS_STRING(newobj);

    Py_MEMCPY(s, PyString_AS_STRING(self), n);

    for (i = 0; i < n; i++) {
        int c = Py_CHARMASK(s[i]);
        if (islower(c))
            s[i] = _toupper(c);
    }

    return newobj;
}

PyDoc_STRVAR(title__doc__,
"S.title() -> string\n\
\n\
Return a titlecased version of S, i.e. words start with uppercase\n\
characters, all remaining cased characters have lowercase.");

static PyObject*
string_title(PyStringObject *self)
{
    char *s = PyString_AS_STRING(self), *s_new;
    Py_ssize_t i, n = PyString_GET_SIZE(self);
    int previous_is_cased = 0;
    PyObject *newobj;

    newobj = PyString_FromStringAndSize(NULL, n);
    if (newobj == NULL)
        return NULL;
    s_new = PyString_AsString(newobj);
    for (i = 0; i < n; i++) {
        int c = Py_CHARMASK(*s++);
        if (islower(c)) {
            if (!previous_is_cased)
                c = toupper(c);
            previous_is_cased = 1;
        } else if (isupper(c)) {
            if (previous_is_cased)
                c = tolower(c);
            previous_is_cased = 1;
        } else
            previous_is_cased = 0;
        *s_new++ = c;
    }
    return newobj;
}

PyDoc_STRVAR(capitalize__doc__,
"S.capitalize() -> string\n\
\n\
Return a copy of the string S with only its first character\n\
capitalized.");

static PyObject *
string_capitalize(PyStringObject *self)
{
    char *s = PyString_AS_STRING(self), *s_new;
    Py_ssize_t i, n = PyString_GET_SIZE(self);
    PyObject *newobj;

    newobj = PyString_FromStringAndSize(NULL, n);
    if (newobj == NULL)
        return NULL;
    s_new = PyString_AsString(newobj);
    if (0 < n) {
        int c = Py_CHARMASK(*s++);
        if (islower(c))
            *s_new = toupper(c);
        else
            *s_new = c;
        s_new++;
    }
    for (i = 1; i < n; i++) {
        int c = Py_CHARMASK(*s++);
        if (isupper(c))
            *s_new = tolower(c);
        else
            *s_new = c;
        s_new++;
    }
    return newobj;
}


PyDoc_STRVAR(count__doc__,
"S.count(sub[, start[, end]]) -> int\n\
\n\
Return the number of non-overlapping occurrences of substring sub in\n\
string S[start:end].  Optional arguments start and end are interpreted\n\
as in slice notation.");

static PyObject *
string_count(PyStringObject *self, PyObject *args)
{
    PyObject *sub_obj;
    const char *str = PyString_AS_STRING(self), *sub;
    Py_ssize_t sub_len;
    Py_ssize_t start = 0, end = PY_SSIZE_T_MAX;

    if (!stringlib_parse_args_finds("count", args, &sub_obj, &start, &end))
        return NULL;

    if (PyString_Check(sub_obj)) {
        sub = PyString_AS_STRING(sub_obj);
        sub_len = PyString_GET_SIZE(sub_obj);
    }
#ifdef Py_USING_UNICODE
    else if (PyUnicode_Check(sub_obj)) {
        Py_ssize_t count;
        count = PyUnicode_Count((PyObject *)self, sub_obj, start, end);
        if (count == -1)
            return NULL;
        else
            return PyInt_FromSsize_t(count);
    }
#endif
    else if (PyObject_AsCharBuffer(sub_obj, &sub, &sub_len))
        return NULL;

    ADJUST_INDICES(start, end, PyString_GET_SIZE(self));

    return PyInt_FromSsize_t(
        stringlib_count(str + start, end - start, sub, sub_len, PY_SSIZE_T_MAX)
        );
}

PyDoc_STRVAR(swapcase__doc__,
"S.swapcase() -> string\n\
\n\
Return a copy of the string S with uppercase characters\n\
converted to lowercase and vice versa.");

static PyObject *
string_swapcase(PyStringObject *self)
{
    char *s = PyString_AS_STRING(self), *s_new;
    Py_ssize_t i, n = PyString_GET_SIZE(self);
    PyObject *newobj;

    newobj = PyString_FromStringAndSize(NULL, n);
    if (newobj == NULL)
        return NULL;
    s_new = PyString_AsString(newobj);
    for (i = 0; i < n; i++) {
        int c = Py_CHARMASK(*s++);
        if (islower(c)) {
            *s_new = toupper(c);
        }
        else if (isupper(c)) {
            *s_new = tolower(c);
        }
        else
            *s_new = c;
        s_new++;
    }
    return newobj;
}


PyDoc_STRVAR(translate__doc__,
"S.translate(table [,deletechars]) -> string\n\
\n\
Return a copy of the string S, where all characters occurring\n\
in the optional argument deletechars are removed, and the\n\
remaining characters have been mapped through the given\n\
translation table, which must be a string of length 256 or None.\n\
If the table argument is None, no translation is applied and\n\
the operation simply removes the characters in deletechars.");

static PyObject *
string_translate(PyStringObject *self, PyObject *args)
{
    register char *input, *output;
    const char *table;
    register Py_ssize_t i, c, changed = 0;
    PyObject *input_obj = (PyObject*)self;
    const char *output_start, *del_table=NULL;
    Py_ssize_t inlen, tablen, dellen = 0;
    PyObject *result;
    int trans_table[256];
    PyObject *tableobj, *delobj = NULL;

    if (!PyArg_UnpackTuple(args, "translate", 1, 2,
                          &tableobj, &delobj))
        return NULL;

    if (PyString_Check(tableobj)) {
        table = PyString_AS_STRING(tableobj);
        tablen = PyString_GET_SIZE(tableobj);
    }
    else if (tableobj == Py_None) {
        table = NULL;
        tablen = 256;
    }
#ifdef Py_USING_UNICODE
    else if (PyUnicode_Check(tableobj)) {
        /* Unicode .translate() does not support the deletechars
           parameter; instead a mapping to None will cause characters
           to be deleted. */
        if (delobj != NULL) {
            PyErr_SetString(PyExc_TypeError,
            "deletions are implemented differently for unicode");
            return NULL;
        }
        return PyUnicode_Translate((PyObject *)self, tableobj, NULL);
    }
#endif
    else if (PyObject_AsCharBuffer(tableobj, &table, &tablen))
        return NULL;

    if (tablen != 256) {
        PyErr_SetString(PyExc_ValueError,
          "translation table must be 256 characters long");
        return NULL;
    }

    if (delobj != NULL) {
        if (PyString_Check(delobj)) {
            del_table = PyString_AS_STRING(delobj);
            dellen = PyString_GET_SIZE(delobj);
        }
#ifdef Py_USING_UNICODE
        else if (PyUnicode_Check(delobj)) {
            PyErr_SetString(PyExc_TypeError,
            "deletions are implemented differently for unicode");
            return NULL;
        }
#endif
        else if (PyObject_AsCharBuffer(delobj, &del_table, &dellen))
            return NULL;
    }
    else {
        del_table = NULL;
        dellen = 0;
    }

    inlen = PyString_GET_SIZE(input_obj);
    result = PyString_FromStringAndSize((char *)NULL, inlen);
    if (result == NULL)
        return NULL;
    output_start = output = PyString_AsString(result);
    input = PyString_AS_STRING(input_obj);

    if (dellen == 0 && table != NULL) {
        /* If no deletions are required, use faster code */
        for (i = inlen; --i >= 0; ) {
            c = Py_CHARMASK(*input++);
            if (Py_CHARMASK((*output++ = table[c])) != c)
                changed = 1;
        }
        if (changed || !PyString_CheckExact(input_obj))
            return result;
        Py_DECREF(result);
        Py_INCREF(input_obj);
        return input_obj;
    }

    if (table == NULL) {
        for (i = 0; i < 256; i++)
            trans_table[i] = Py_CHARMASK(i);
    } else {
        for (i = 0; i < 256; i++)
            trans_table[i] = Py_CHARMASK(table[i]);
    }

    for (i = 0; i < dellen; i++)
        trans_table[(int) Py_CHARMASK(del_table[i])] = -1;

    for (i = inlen; --i >= 0; ) {
        c = Py_CHARMASK(*input++);
        if (trans_table[c] != -1)
            if (Py_CHARMASK(*output++ = (char)trans_table[c]) == c)
                continue;
        changed = 1;
    }
    if (!changed && PyString_CheckExact(input_obj)) {
        Py_DECREF(result);
        Py_INCREF(input_obj);
        return input_obj;
    }
    /* Fix the size of the resulting string */
    if (inlen > 0 && _PyString_Resize(&result, output - output_start))
        return NULL;
    return result;
}


/* find and count characters and substrings */

#define findchar(target, target_len, c)                         \
  ((char *)memchr((const void *)(target), c, target_len))

/* String ops must return a string.  */
/* If the object is subclass of string, create a copy */
Py_LOCAL(PyStringObject *)
return_self(PyStringObject *self)
{
    if (PyString_CheckExact(self)) {
        Py_INCREF(self);
        return self;
    }
    return (PyStringObject *)PyString_FromStringAndSize(
        PyString_AS_STRING(self),
        PyString_GET_SIZE(self));
}

Py_LOCAL_INLINE(Py_ssize_t)
countchar(const char *target, int target_len, char c, Py_ssize_t maxcount)
{
    Py_ssize_t count=0;
    const char *start=target;
    const char *end=target+target_len;

    while ( (start=findchar(start, end-start, c)) != NULL ) {
        count++;
        if (count >= maxcount)
            break;
        start += 1;
    }
    return count;
}


/* Algorithms for different cases of string replacement */

/* len(self)>=1, from="", len(to)>=1, maxcount>=1 */
Py_LOCAL(PyStringObject *)
replace_interleave(PyStringObject *self,
                   const char *to_s, Py_ssize_t to_len,
                   Py_ssize_t maxcount)
{
    char *self_s, *result_s;
    Py_ssize_t self_len, result_len;
    Py_ssize_t count, i, product;
    PyStringObject *result;

    self_len = PyString_GET_SIZE(self);

    /* 1 at the end plus 1 after every character */
    count = self_len+1;
    if (maxcount < count)
        count = maxcount;

    /* Check for overflow */
    /*   result_len = count * to_len + self_len; */
    product = count * to_len;
    if (product / to_len != count) {
        PyErr_SetString(PyExc_OverflowError,
                        "replace string is too long");
        return NULL;
    }
    result_len = product + self_len;
    if (result_len < 0) {
        PyErr_SetString(PyExc_OverflowError,
                        "replace string is too long");
        return NULL;
    }

    if (! (result = (PyStringObject *)
                     PyString_FromStringAndSize(NULL, result_len)) )
        return NULL;

    self_s = PyString_AS_STRING(self);
    result_s = PyString_AS_STRING(result);

    /* TODO: special case single character, which doesn't need memcpy */

    /* Lay the first one down (guaranteed this will occur) */
    Py_MEMCPY(result_s, to_s, to_len);
    result_s += to_len;
    count -= 1;

    for (i=0; i<count; i++) {
        *result_s++ = *self_s++;
        Py_MEMCPY(result_s, to_s, to_len);
        result_s += to_len;
    }

    /* Copy the rest of the original string */
    Py_MEMCPY(result_s, self_s, self_len-i);

    return result;
}

/* Special case for deleting a single character */
/* len(self)>=1, len(from)==1, to="", maxcount>=1 */
Py_LOCAL(PyStringObject *)
replace_delete_single_character(PyStringObject *self,
                                char from_c, Py_ssize_t maxcount)
{
    char *self_s, *result_s;
    char *start, *next, *end;
    Py_ssize_t self_len, result_len;
    Py_ssize_t count;
    PyStringObject *result;

    self_len = PyString_GET_SIZE(self);
    self_s = PyString_AS_STRING(self);

    count = countchar(self_s, self_len, from_c, maxcount);
    if (count == 0) {
        return return_self(self);
    }

    result_len = self_len - count;  /* from_len == 1 */
    assert(result_len>=0);

    if ( (result = (PyStringObject *)
                    PyString_FromStringAndSize(NULL, result_len)) == NULL)
        return NULL;
    result_s = PyString_AS_STRING(result);

    start = self_s;
    end = self_s + self_len;
    while (count-- > 0) {
        next = findchar(start, end-start, from_c);
        if (next == NULL)
            break;
        Py_MEMCPY(result_s, start, next-start);
        result_s += (next-start);
        start = next+1;
    }
    Py_MEMCPY(result_s, start, end-start);

    return result;
}

/* len(self)>=1, len(from)>=2, to="", maxcount>=1 */

Py_LOCAL(PyStringObject *)
replace_delete_substring(PyStringObject *self,
                         const char *from_s, Py_ssize_t from_len,
                         Py_ssize_t maxcount) {
    char *self_s, *result_s;
    char *start, *next, *end;
    Py_ssize_t self_len, result_len;
    Py_ssize_t count, offset;
    PyStringObject *result;

    self_len = PyString_GET_SIZE(self);
    self_s = PyString_AS_STRING(self);

    count = stringlib_count(self_s, self_len,
                            from_s, from_len,
                            maxcount);

    if (count == 0) {
        /* no matches */
        return return_self(self);
    }

    result_len = self_len - (count * from_len);
    assert (result_len>=0);

    if ( (result = (PyStringObject *)
          PyString_FromStringAndSize(NULL, result_len)) == NULL )
        return NULL;

    result_s = PyString_AS_STRING(result);

    start = self_s;
    end = self_s + self_len;
    while (count-- > 0) {
        offset = stringlib_find(start, end-start,
                                from_s, from_len,
                                0);
        if (offset == -1)
            break;
        next = start + offset;

        Py_MEMCPY(result_s, start, next-start);

        result_s += (next-start);
        start = next+from_len;
    }
    Py_MEMCPY(result_s, start, end-start);
    return result;
}

/* len(self)>=1, len(from)==len(to)==1, maxcount>=1 */
Py_LOCAL(PyStringObject *)
replace_single_character_in_place(PyStringObject *self,
                                  char from_c, char to_c,
                                  Py_ssize_t maxcount)
{
    char *self_s, *result_s, *start, *end, *next;
    Py_ssize_t self_len;
    PyStringObject *result;

    /* The result string will be the same size */
    self_s = PyString_AS_STRING(self);
    self_len = PyString_GET_SIZE(self);

    next = findchar(self_s, self_len, from_c);

    if (next == NULL) {
        /* No matches; return the original string */
        return return_self(self);
    }

    /* Need to make a new string */
    result = (PyStringObject *) PyString_FromStringAndSize(NULL, self_len);
    if (result == NULL)
        return NULL;
    result_s = PyString_AS_STRING(result);
    Py_MEMCPY(result_s, self_s, self_len);

    /* change everything in-place, starting with this one */
    start =  result_s + (next-self_s);
    *start = to_c;
    start++;
    end = result_s + self_len;

    while (--maxcount > 0) {
        next = findchar(start, end-start, from_c);
        if (next == NULL)
            break;
        *next = to_c;
        start = next+1;
    }

    return result;
}

/* len(self)>=1, len(from)==len(to)>=2, maxcount>=1 */
Py_LOCAL(PyStringObject *)
replace_substring_in_place(PyStringObject *self,
                           const char *from_s, Py_ssize_t from_len,
                           const char *to_s, Py_ssize_t to_len,
                           Py_ssize_t maxcount)
{
    char *result_s, *start, *end;
    char *self_s;
    Py_ssize_t self_len, offset;
    PyStringObject *result;

    /* The result string will be the same size */

    self_s = PyString_AS_STRING(self);
    self_len = PyString_GET_SIZE(self);

    offset = stringlib_find(self_s, self_len,
                            from_s, from_len,
                            0);
    if (offset == -1) {
        /* No matches; return the original string */
        return return_self(self);
    }

    /* Need to make a new string */
    result = (PyStringObject *) PyString_FromStringAndSize(NULL, self_len);
    if (result == NULL)
        return NULL;
    result_s = PyString_AS_STRING(result);
    Py_MEMCPY(result_s, self_s, self_len);

    /* change everything in-place, starting with this one */
    start =  result_s + offset;
    Py_MEMCPY(start, to_s, from_len);
    start += from_len;
    end = result_s + self_len;

    while ( --maxcount > 0) {
        offset = stringlib_find(start, end-start,
                                from_s, from_len,
                                0);
        if (offset==-1)
            break;
        Py_MEMCPY(start+offset, to_s, from_len);
        start += offset+from_len;
    }

    return result;
}

/* len(self)>=1, len(from)==1, len(to)>=2, maxcount>=1 */
Py_LOCAL(PyStringObject *)
replace_single_character(PyStringObject *self,
                         char from_c,
                         const char *to_s, Py_ssize_t to_len,
                         Py_ssize_t maxcount)
{
    char *self_s, *result_s;
    char *start, *next, *end;
    Py_ssize_t self_len, result_len;
    Py_ssize_t count, product;
    PyStringObject *result;

    self_s = PyString_AS_STRING(self);
    self_len = PyString_GET_SIZE(self);

    count = countchar(self_s, self_len, from_c, maxcount);
    if (count == 0) {
        /* no matches, return unchanged */
        return return_self(self);
    }

    /* use the difference between current and new, hence the "-1" */
    /*   result_len = self_len + count * (to_len-1)  */
    product = count * (to_len-1);
    if (product / (to_len-1) != count) {
        PyErr_SetString(PyExc_OverflowError, "replace string is too long");
        return NULL;
    }
    result_len = self_len + product;
    if (result_len < 0) {
        PyErr_SetString(PyExc_OverflowError, "replace string is too long");
        return NULL;
    }

    if ( (result = (PyStringObject *)
          PyString_FromStringAndSize(NULL, result_len)) == NULL)
        return NULL;
    result_s = PyString_AS_STRING(result);

    start = self_s;
    end = self_s + self_len;
    while (count-- > 0) {
        next = findchar(start, end-start, from_c);
        if (next == NULL)
            break;

        if (next == start) {
            /* replace with the 'to' */
            Py_MEMCPY(result_s, to_s, to_len);
            result_s += to_len;
            start += 1;
        } else {
            /* copy the unchanged old then the 'to' */
            Py_MEMCPY(result_s, start, next-start);
            result_s += (next-start);
            Py_MEMCPY(result_s, to_s, to_len);
            result_s += to_len;
            start = next+1;
        }
    }
    /* Copy the remainder of the remaining string */
    Py_MEMCPY(result_s, start, end-start);

    return result;
}

/* len(self)>=1, len(from)>=2, len(to)>=2, maxcount>=1 */
Py_LOCAL(PyStringObject *)
replace_substring(PyStringObject *self,
                  const char *from_s, Py_ssize_t from_len,
                  const char *to_s, Py_ssize_t to_len,
                  Py_ssize_t maxcount) {
    char *self_s, *result_s;
    char *start, *next, *end;
    Py_ssize_t self_len, result_len;
    Py_ssize_t count, offset, product;
    PyStringObject *result;

    self_s = PyString_AS_STRING(self);
    self_len = PyString_GET_SIZE(self);

    count = stringlib_count(self_s, self_len,
                            from_s, from_len,
                            maxcount);

    if (count == 0) {
        /* no matches, return unchanged */
        return return_self(self);
    }

    /* Check for overflow */
    /*    result_len = self_len + count * (to_len-from_len) */
    product = count * (to_len-from_len);
    if (product / (to_len-from_len) != count) {
        PyErr_SetString(PyExc_OverflowError, "replace string is too long");
        return NULL;
    }
    result_len = self_len + product;
    if (result_len < 0) {
        PyErr_SetString(PyExc_OverflowError, "replace string is too long");
        return NULL;
    }

    if ( (result = (PyStringObject *)
          PyString_FromStringAndSize(NULL, result_len)) == NULL)
        return NULL;
    result_s = PyString_AS_STRING(result);

    start = self_s;
    end = self_s + self_len;
    while (count-- > 0) {
        offset = stringlib_find(start, end-start,
                                from_s, from_len,
                                0);
        if (offset == -1)
            break;
        next = start+offset;
        if (next == start) {
            /* replace with the 'to' */
            Py_MEMCPY(result_s, to_s, to_len);
            result_s += to_len;
            start += from_len;
        } else {
            /* copy the unchanged old then the 'to' */
            Py_MEMCPY(result_s, start, next-start);
            result_s += (next-start);
            Py_MEMCPY(result_s, to_s, to_len);
            result_s += to_len;
            start = next+from_len;
        }
    }
    /* Copy the remainder of the remaining string */
    Py_MEMCPY(result_s, start, end-start);

    return result;
}


Py_LOCAL(PyStringObject *)
replace(PyStringObject *self,
    const char *from_s, Py_ssize_t from_len,
    const char *to_s, Py_ssize_t to_len,
    Py_ssize_t maxcount)
{
    if (maxcount < 0) {
        maxcount = PY_SSIZE_T_MAX;
    } else if (maxcount == 0 || PyString_GET_SIZE(self) == 0) {
        /* nothing to do; return the original string */
        return return_self(self);
    }

    if (maxcount == 0 ||
        (from_len == 0 && to_len == 0)) {
        /* nothing to do; return the original string */
        return return_self(self);
    }

    /* Handle zero-length special cases */

    if (from_len == 0) {
        /* insert the 'to' string everywhere.   */
        /*    >>> "Python".replace("", ".")     */
        /*    '.P.y.t.h.o.n.'                   */
        return replace_interleave(self, to_s, to_len, maxcount);
    }

    /* Except for "".replace("", "A") == "A" there is no way beyond this */
    /* point for an empty self string to generate a non-empty string */
    /* Special case so the remaining code always gets a non-empty string */
    if (PyString_GET_SIZE(self) == 0) {
        return return_self(self);
    }

    if (to_len == 0) {
        /* delete all occurances of 'from' string */
        if (from_len == 1) {
            return replace_delete_single_character(
                self, from_s[0], maxcount);
        } else {
            return replace_delete_substring(self, from_s, from_len, maxcount);
        }
    }

    /* Handle special case where both strings have the same length */

    if (from_len == to_len) {
        if (from_len == 1) {
            return replace_single_character_in_place(
                self,
                from_s[0],
                to_s[0],
                maxcount);
        } else {
            return replace_substring_in_place(
                self, from_s, from_len, to_s, to_len, maxcount);
        }
    }

    /* Otherwise use the more generic algorithms */
    if (from_len == 1) {
        return replace_single_character(self, from_s[0],
                                        to_s, to_len, maxcount);
    } else {
        /* len('from')>=2, len('to')>=1 */
        return replace_substring(self, from_s, from_len, to_s, to_len, maxcount);
    }
}

PyDoc_STRVAR(replace__doc__,
"S.replace(old, new[, count]) -> string\n\
\n\
Return a copy of string S with all occurrences of substring\n\
old replaced by new.  If the optional argument count is\n\
given, only the first count occurrences are replaced.");

static PyObject *
string_replace(PyStringObject *self, PyObject *args)
{
    Py_ssize_t count = -1;
    PyObject *from, *to;
    const char *from_s, *to_s;
    Py_ssize_t from_len, to_len;

    if (!PyArg_ParseTuple(args, "OO|n:replace", &from, &to, &count))
        return NULL;

    if (PyString_Check(from)) {
        from_s = PyString_AS_STRING(from);
        from_len = PyString_GET_SIZE(from);
    }
#ifdef Py_USING_UNICODE
    if (PyUnicode_Check(from))
        return PyUnicode_Replace((PyObject *)self,
                                 from, to, count);
#endif
    else if (PyObject_AsCharBuffer(from, &from_s, &from_len))
        return NULL;

    if (PyString_Check(to)) {
        to_s = PyString_AS_STRING(to);
        to_len = PyString_GET_SIZE(to);
    }
#ifdef Py_USING_UNICODE
    else if (PyUnicode_Check(to))
        return PyUnicode_Replace((PyObject *)self,
                                 from, to, count);
#endif
    else if (PyObject_AsCharBuffer(to, &to_s, &to_len))
        return NULL;

    return (PyObject *)replace((PyStringObject *) self,
                               from_s, from_len,
                               to_s, to_len, count);
}

/** End DALKE **/

/* Matches the end (direction >= 0) or start (direction < 0) of self
 * against substr, using the start and end arguments. Returns
 * -1 on error, 0 if not found and 1 if found.
 */
Py_LOCAL(int)
_string_tailmatch(PyStringObject *self, PyObject *substr, Py_ssize_t start,
                  Py_ssize_t end, int direction)
{
    Py_ssize_t len = PyString_GET_SIZE(self);
    Py_ssize_t slen;
    const char* sub;
    const char* str;

    if (PyString_Check(substr)) {
        sub = PyString_AS_STRING(substr);
        slen = PyString_GET_SIZE(substr);
    }
#ifdef Py_USING_UNICODE
    else if (PyUnicode_Check(substr))
        return PyUnicode_Tailmatch((PyObject *)self,
                                   substr, start, end, direction);
#endif
    else if (PyObject_AsCharBuffer(substr, &sub, &slen))
        return -1;
    str = PyString_AS_STRING(self);

    ADJUST_INDICES(start, end, len);

    if (direction < 0) {
        /* startswith */
        if (start+slen > len)
            return 0;
    } else {
        /* endswith */
        if (end-start < slen || start > len)
            return 0;

        if (end-slen > start)
            start = end - slen;
    }
    if (end-start >= slen)
        return ! memcmp(str+start, sub, slen);
    return 0;
}


PyDoc_STRVAR(startswith__doc__,
"S.startswith(prefix[, start[, end]]) -> bool\n\
\n\
Return True if S starts with the specified prefix, False otherwise.\n\
With optional start, test S beginning at that position.\n\
With optional end, stop comparing S at that position.\n\
prefix can also be a tuple of strings to try.");

static PyObject *
string_startswith(PyStringObject *self, PyObject *args)
{
    Py_ssize_t start = 0;
    Py_ssize_t end = PY_SSIZE_T_MAX;
    PyObject *subobj;
    int result;

    if (!stringlib_parse_args_finds("startswith", args, &subobj, &start, &end))
        return NULL;
    if (PyTuple_Check(subobj)) {
        Py_ssize_t i;
        for (i = 0; i < PyTuple_GET_SIZE(subobj); i++) {
            result = _string_tailmatch(self,
                            PyTuple_GET_ITEM(subobj, i),
                            start, end, -1);
            if (result == -1)
                return NULL;
            else if (result) {
                Py_RETURN_TRUE;
            }
        }
        Py_RETURN_FALSE;
    }
    result = _string_tailmatch(self, subobj, start, end, -1);
    if (result == -1) {
        if (PyErr_ExceptionMatches(PyExc_TypeError))
            PyErr_Format(PyExc_TypeError, "startswith first arg must be str, "
                         "unicode, or tuple, not %s", Py_TYPE(subobj)->tp_name);
        return NULL;
    }
    else
        return PyBool_FromLong(result);
}


PyDoc_STRVAR(endswith__doc__,
"S.endswith(suffix[, start[, end]]) -> bool\n\
\n\
Return True if S ends with the specified suffix, False otherwise.\n\
With optional start, test S beginning at that position.\n\
With optional end, stop comparing S at that position.\n\
suffix can also be a tuple of strings to try.");

static PyObject *
string_endswith(PyStringObject *self, PyObject *args)
{
    Py_ssize_t start = 0;
    Py_ssize_t end = PY_SSIZE_T_MAX;
    PyObject *subobj;
    int result;

    if (!stringlib_parse_args_finds("endswith", args, &subobj, &start, &end))
        return NULL;
    if (PyTuple_Check(subobj)) {
        Py_ssize_t i;
        for (i = 0; i < PyTuple_GET_SIZE(subobj); i++) {
            result = _string_tailmatch(self,
                            PyTuple_GET_ITEM(subobj, i),
                            start, end, +1);
            if (result == -1)
                return NULL;
            else if (result) {
                Py_RETURN_TRUE;
            }
        }
        Py_RETURN_FALSE;
    }
    result = _string_tailmatch(self, subobj, start, end, +1);
    if (result == -1) {
        if (PyErr_ExceptionMatches(PyExc_TypeError))
            PyErr_Format(PyExc_TypeError, "endswith first arg must be str, "
                         "unicode, or tuple, not %s", Py_TYPE(subobj)->tp_name);
        return NULL;
    }
    else
        return PyBool_FromLong(result);
}


PyDoc_STRVAR(encode__doc__,
"S.encode([encoding[,errors]]) -> object\n\
\n\
Encodes S using the codec registered for encoding. encoding defaults\n\
to the default encoding. errors may be given to set a different error\n\
handling scheme. Default is 'strict' meaning that encoding errors raise\n\
a UnicodeEncodeError. Other possible values are 'ignore', 'replace' and\n\
'xmlcharrefreplace' as well as any other name registered with\n\
codecs.register_error that is able to handle UnicodeEncodeErrors.");

static PyObject *
string_encode(PyStringObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"encoding", "errors", 0};
    char *encoding = NULL;
    char *errors = NULL;
    PyObject *v;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|ss:encode",
                                     kwlist, &encoding, &errors))
        return NULL;
    v = PyString_AsEncodedObject((PyObject *)self, encoding, errors);
    if (v == NULL)
        goto onError;
    if (!PyString_Check(v) && !PyUnicode_Check(v)) {
        PyErr_Format(PyExc_TypeError,
                     "encoder did not return a string/unicode object "
                     "(type=%.400s)",
                     Py_TYPE(v)->tp_name);
        Py_DECREF(v);
        return NULL;
    }
    return v;

 onError:
    return NULL;
}


PyDoc_STRVAR(decode__doc__,
"S.decode([encoding[,errors]]) -> object\n\
\n\
Decodes S using the codec registered for encoding. encoding defaults\n\
to the default encoding. errors may be given to set a different error\n\
handling scheme. Default is 'strict' meaning that encoding errors raise\n\
a UnicodeDecodeError. Other possible values are 'ignore' and 'replace'\n\
as well as any other name registered with codecs.register_error that is\n\
able to handle UnicodeDecodeErrors.");

static PyObject *
string_decode(PyStringObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"encoding", "errors", 0};
    char *encoding = NULL;
    char *errors = NULL;
    PyObject *v;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|ss:decode",
                                     kwlist, &encoding, &errors))
        return NULL;
    v = PyString_AsDecodedObject((PyObject *)self, encoding, errors);
    if (v == NULL)
        goto onError;
    if (!PyString_Check(v) && !PyUnicode_Check(v)) {
        PyErr_Format(PyExc_TypeError,
                     "decoder did not return a string/unicode object "
                     "(type=%.400s)",
                     Py_TYPE(v)->tp_name);
        Py_DECREF(v);
        return NULL;
    }
    return v;

 onError:
    return NULL;
}


PyDoc_STRVAR(expandtabs__doc__,
"S.expandtabs([tabsize]) -> string\n\
\n\
Return a copy of S where all tab characters are expanded using spaces.\n\
If tabsize is not given, a tab size of 8 characters is assumed.");

static PyObject*
string_expandtabs(PyStringObject *self, PyObject *args)
{
    const char *e, *p, *qe;
    char *q;
    Py_ssize_t i, j, incr;
    PyObject *u;
    int tabsize = 8;

    if (!PyArg_ParseTuple(args, "|i:expandtabs", &tabsize))
        return NULL;

    /* First pass: determine size of output string */
    i = 0; /* chars up to and including most recent \n or \r */
    j = 0; /* chars since most recent \n or \r (use in tab calculations) */
    e = PyString_AS_STRING(self) + PyString_GET_SIZE(self); /* end of input */
    for (p = PyString_AS_STRING(self); p < e; p++)
    if (*p == '\t') {
        if (tabsize > 0) {
            incr = tabsize - (j % tabsize);
            if (j > PY_SSIZE_T_MAX - incr)
                goto overflow1;
            j += incr;
        }
    }
    else {
        if (j > PY_SSIZE_T_MAX - 1)
            goto overflow1;
        j++;
        if (*p == '\n' || *p == '\r') {
            if (i > PY_SSIZE_T_MAX - j)
                goto overflow1;
            i += j;
            j = 0;
        }
    }

    if (i > PY_SSIZE_T_MAX - j)
        goto overflow1;

    /* Second pass: create output string and fill it */
    u = PyString_FromStringAndSize(NULL, i + j);
    if (!u)
        return NULL;

    j = 0; /* same as in first pass */
    q = PyString_AS_STRING(u); /* next output char */
    qe = PyString_AS_STRING(u) + PyString_GET_SIZE(u); /* end of output */

    for (p = PyString_AS_STRING(self); p < e; p++)
    if (*p == '\t') {
        if (tabsize > 0) {
            i = tabsize - (j % tabsize);
            j += i;
            while (i--) {
                if (q >= qe)
                    goto overflow2;
                *q++ = ' ';
            }
        }
    }
    else {
        if (q >= qe)
            goto overflow2;
        *q++ = *p;
        j++;
        if (*p == '\n' || *p == '\r')
            j = 0;
    }

    return u;

  overflow2:
    Py_DECREF(u);
  overflow1:
    PyErr_SetString(PyExc_OverflowError, "new string is too long");
    return NULL;
}

Py_LOCAL_INLINE(PyObject *)
pad(PyStringObject *self, Py_ssize_t left, Py_ssize_t right, char fill)
{
    PyObject *u;

    if (left < 0)
        left = 0;
    if (right < 0)
        right = 0;

    if (left == 0 && right == 0 && PyString_CheckExact(self)) {
        Py_INCREF(self);
        return (PyObject *)self;
    }

    u = PyString_FromStringAndSize(NULL,
                                   left + PyString_GET_SIZE(self) + right);
    if (u) {
        if (left)
            memset(PyString_AS_STRING(u), fill, left);
        Py_MEMCPY(PyString_AS_STRING(u) + left,
               PyString_AS_STRING(self),
               PyString_GET_SIZE(self));
        if (right)
            memset(PyString_AS_STRING(u) + left + PyString_GET_SIZE(self),
               fill, right);
    }

    return u;
}

PyDoc_STRVAR(ljust__doc__,
"S.ljust(width[, fillchar]) -> string\n"
"\n"
"Return S left-justified in a string of length width. Padding is\n"
"done using the specified fill character (default is a space).");

static PyObject *
string_ljust(PyStringObject *self, PyObject *args)
{
    Py_ssize_t width;
    char fillchar = ' ';

    if (!PyArg_ParseTuple(args, "n|c:ljust", &width, &fillchar))
        return NULL;

    if (PyString_GET_SIZE(self) >= width && PyString_CheckExact(self)) {
        Py_INCREF(self);
        return (PyObject*) self;
    }

    return pad(self, 0, width - PyString_GET_SIZE(self), fillchar);
}


PyDoc_STRVAR(rjust__doc__,
"S.rjust(width[, fillchar]) -> string\n"
"\n"
"Return S right-justified in a string of length width. Padding is\n"
"done using the specified fill character (default is a space)");

static PyObject *
string_rjust(PyStringObject *self, PyObject *args)
{
    Py_ssize_t width;
    char fillchar = ' ';

    if (!PyArg_ParseTuple(args, "n|c:rjust", &width, &fillchar))
        return NULL;

    if (PyString_GET_SIZE(self) >= width && PyString_CheckExact(self)) {
        Py_INCREF(self);
        return (PyObject*) self;
    }

    return pad(self, width - PyString_GET_SIZE(self), 0, fillchar);
}


PyDoc_STRVAR(center__doc__,
"S.center(width[, fillchar]) -> string\n"
"\n"
"Return S centered in a string of length width. Padding is\n"
"done using the specified fill character (default is a space)");

static PyObject *
string_center(PyStringObject *self, PyObject *args)
{
    Py_ssize_t marg, left;
    Py_ssize_t width;
    char fillchar = ' ';

    if (!PyArg_ParseTuple(args, "n|c:center", &width, &fillchar))
        return NULL;

    if (PyString_GET_SIZE(self) >= width && PyString_CheckExact(self)) {
        Py_INCREF(self);
        return (PyObject*) self;
    }

    marg = width - PyString_GET_SIZE(self);
    left = marg / 2 + (marg & width & 1);

    return pad(self, left, marg - left, fillchar);
}

PyDoc_STRVAR(zfill__doc__,
"S.zfill(width) -> string\n"
"\n"
"Pad a numeric string S with zeros on the left, to fill a field\n"
"of the specified width.  The string S is never truncated.");

static PyObject *
string_zfill(PyStringObject *self, PyObject *args)
{
    Py_ssize_t fill;
    PyObject *s;
    char *p;
    Py_ssize_t width;

    if (!PyArg_ParseTuple(args, "n:zfill", &width))
        return NULL;

    if (PyString_GET_SIZE(self) >= width) {
        if (PyString_CheckExact(self)) {
            Py_INCREF(self);
            return (PyObject*) self;
        }
        else
            return PyString_FromStringAndSize(
            PyString_AS_STRING(self),
            PyString_GET_SIZE(self)
            );
    }

    fill = width - PyString_GET_SIZE(self);

    s = pad(self, fill, 0, '0');

    if (s == NULL)
        return NULL;

    p = PyString_AS_STRING(s);
    if (p[fill] == '+' || p[fill] == '-') {
        /* move sign to beginning of string */
        p[0] = p[fill];
        p[fill] = '0';
    }

    return (PyObject*) s;
}

PyDoc_STRVAR(isspace__doc__,
"S.isspace() -> bool\n\
\n\
Return True if all characters in S are whitespace\n\
and there is at least one character in S, False otherwise.");

static PyObject*
string_isspace(PyStringObject *self)
{
    register const unsigned char *p
        = (unsigned char *) PyString_AS_STRING(self);
    register const unsigned char *e;

    /* Shortcut for single character strings */
    if (PyString_GET_SIZE(self) == 1 &&
        isspace(*p))
        return PyBool_FromLong(1);

    /* Special case for empty strings */
    if (PyString_GET_SIZE(self) == 0)
        return PyBool_FromLong(0);

    e = p + PyString_GET_SIZE(self);
    for (; p < e; p++) {
        if (!isspace(*p))
            return PyBool_FromLong(0);
    }
    return PyBool_FromLong(1);
}


PyDoc_STRVAR(isalpha__doc__,
"S.isalpha() -> bool\n\
\n\
Return True if all characters in S are alphabetic\n\
and there is at least one character in S, False otherwise.");

static PyObject*
string_isalpha(PyStringObject *self)
{
    register const unsigned char *p
        = (unsigned char *) PyString_AS_STRING(self);
    register const unsigned char *e;

    /* Shortcut for single character strings */
    if (PyString_GET_SIZE(self) == 1 &&
        isalpha(*p))
        return PyBool_FromLong(1);

    /* Special case for empty strings */
    if (PyString_GET_SIZE(self) == 0)
        return PyBool_FromLong(0);

    e = p + PyString_GET_SIZE(self);
    for (; p < e; p++) {
        if (!isalpha(*p))
            return PyBool_FromLong(0);
    }
    return PyBool_FromLong(1);
}


PyDoc_STRVAR(isalnum__doc__,
"S.isalnum() -> bool\n\
\n\
Return True if all characters in S are alphanumeric\n\
and there is at least one character in S, False otherwise.");

static PyObject*
string_isalnum(PyStringObject *self)
{
    register const unsigned char *p
        = (unsigned char *) PyString_AS_STRING(self);
    register const unsigned char *e;

    /* Shortcut for single character strings */
    if (PyString_GET_SIZE(self) == 1 &&
        isalnum(*p))
        return PyBool_FromLong(1);

    /* Special case for empty strings */
    if (PyString_GET_SIZE(self) == 0)
        return PyBool_FromLong(0);

    e = p + PyString_GET_SIZE(self);
    for (; p < e; p++) {
        if (!isalnum(*p))
            return PyBool_FromLong(0);
    }
    return PyBool_FromLong(1);
}


PyDoc_STRVAR(isdigit__doc__,
"S.isdigit() -> bool\n\
\n\
Return True if all characters in S are digits\n\
and there is at least one character in S, False otherwise.");

static PyObject*
string_isdigit(PyStringObject *self)
{
    register const unsigned char *p
        = (unsigned char *) PyString_AS_STRING(self);
    register const unsigned char *e;

    /* Shortcut for single character strings */
    if (PyString_GET_SIZE(self) == 1 &&
        isdigit(*p))
        return PyBool_FromLong(1);

    /* Special case for empty strings */
    if (PyString_GET_SIZE(self) == 0)
        return PyBool_FromLong(0);

    e = p + PyString_GET_SIZE(self);
    for (; p < e; p++) {
        if (!isdigit(*p))
            return PyBool_FromLong(0);
    }
    return PyBool_FromLong(1);
}


PyDoc_STRVAR(islower__doc__,
"S.islower() -> bool\n\
\n\
Return True if all cased characters in S are lowercase and there is\n\
at least one cased character in S, False otherwise.");

static PyObject*
string_islower(PyStringObject *self)
{
    register const unsigned char *p
        = (unsigned char *) PyString_AS_STRING(self);
    register const unsigned char *e;
    int cased;

    /* Shortcut for single character strings */
    if (PyString_GET_SIZE(self) == 1)
        return PyBool_FromLong(islower(*p) != 0);

    /* Special case for empty strings */
    if (PyString_GET_SIZE(self) == 0)
        return PyBool_FromLong(0);

    e = p + PyString_GET_SIZE(self);
    cased = 0;
    for (; p < e; p++) {
        if (isupper(*p))
            return PyBool_FromLong(0);
        else if (!cased && islower(*p))
            cased = 1;
    }
    return PyBool_FromLong(cased);
}


PyDoc_STRVAR(isupper__doc__,
"S.isupper() -> bool\n\
\n\
Return True if all cased characters in S are uppercase and there is\n\
at least one cased character in S, False otherwise.");

static PyObject*
string_isupper(PyStringObject *self)
{
    register const unsigned char *p
        = (unsigned char *) PyString_AS_STRING(self);
    register const unsigned char *e;
    int cased;

    /* Shortcut for single character strings */
    if (PyString_GET_SIZE(self) == 1)
        return PyBool_FromLong(isupper(*p) != 0);

    /* Special case for empty strings */
    if (PyString_GET_SIZE(self) == 0)
        return PyBool_FromLong(0);

    e = p + PyString_GET_SIZE(self);
    cased = 0;
    for (; p < e; p++) {
        if (islower(*p))
            return PyBool_FromLong(0);
        else if (!cased && isupper(*p))
            cased = 1;
    }
    return PyBool_FromLong(cased);
}


PyDoc_STRVAR(istitle__doc__,
"S.istitle() -> bool\n\
\n\
Return True if S is a titlecased string and there is at least one\n\
character in S, i.e. uppercase characters may only follow uncased\n\
characters and lowercase characters only cased ones. Return False\n\
otherwise.");

static PyObject*
string_istitle(PyStringObject *self, PyObject *uncased)
{
    register const unsigned char *p
        = (unsigned char *) PyString_AS_STRING(self);
    register const unsigned char *e;
    int cased, previous_is_cased;

    /* Shortcut for single character strings */
    if (PyString_GET_SIZE(self) == 1)
        return PyBool_FromLong(isupper(*p) != 0);

    /* Special case for empty strings */
    if (PyString_GET_SIZE(self) == 0)
        return PyBool_FromLong(0);

    e = p + PyString_GET_SIZE(self);
    cased = 0;
    previous_is_cased = 0;
    for (; p < e; p++) {
        register const unsigned char ch = *p;

        if (isupper(ch)) {
            if (previous_is_cased)
                return PyBool_FromLong(0);
            previous_is_cased = 1;
            cased = 1;
        }
        else if (islower(ch)) {
            if (!previous_is_cased)
                return PyBool_FromLong(0);
            previous_is_cased = 1;
            cased = 1;
        }
        else
            previous_is_cased = 0;
    }
    return PyBool_FromLong(cased);
}


PyDoc_STRVAR(splitlines__doc__,
"S.splitlines([keepends]) -> list of strings\n\
\n\
Return a list of the lines in S, breaking at line boundaries.\n\
Line breaks are not included in the resulting list unless keepends\n\
is given and true.");

static PyObject*
string_splitlines(PyStringObject *self, PyObject *args)
{
    int keepends = 0;

    if (!PyArg_ParseTuple(args, "|i:splitlines", &keepends))
        return NULL;

    return stringlib_splitlines(
        (PyObject*) self, PyString_AS_STRING(self), PyString_GET_SIZE(self),
        keepends
    );
}

PyDoc_STRVAR(sizeof__doc__,
"S.__sizeof__() -> size of S in memory, in bytes");

static PyObject *
string_sizeof(PyStringObject *v)
{
    Py_ssize_t res;
    res = PyStringObject_SIZE + PyString_GET_SIZE(v) * Py_TYPE(v)->tp_itemsize;
    return PyInt_FromSsize_t(res);
}

static PyObject *
string_getnewargs(PyStringObject *v)
{
    return Py_BuildValue("(s#)", v->ob_sval, Py_SIZE(v));
}


#include "stringlib/string_format.h"

PyDoc_STRVAR(format__doc__,
"S.format(*args, **kwargs) -> string\n\
\n\
Return a formatted version of S, using substitutions from args and kwargs.\n\
The substitutions are identified by braces ('{' and '}').");

static PyObject *
string__format__(PyObject* self, PyObject* args)
{
    PyObject *format_spec;
    PyObject *result = NULL;
    PyObject *tmp = NULL;

    /* If 2.x, convert format_spec to the same type as value */
    /* This is to allow things like u''.format('') */
    if (!PyArg_ParseTuple(args, "O:__format__", &format_spec))
        goto done;
    if (!(PyString_Check(format_spec) || PyUnicode_Check(format_spec))) {
        PyErr_Format(PyExc_TypeError, "__format__ arg must be str "
                     "or unicode, not %s", Py_TYPE(format_spec)->tp_name);
        goto done;
    }
    tmp = PyObject_Str(format_spec);
    if (tmp == NULL)
        goto done;
    format_spec = tmp;

    result = _PyBytes_FormatAdvanced(self,
                                     PyString_AS_STRING(format_spec),
                                     PyString_GET_SIZE(format_spec));
done:
    Py_XDECREF(tmp);
    return result;
}

PyDoc_STRVAR(p_format__doc__,
"S.__format__(format_spec) -> string\n\
\n\
Return a formatted version of S as described by format_spec.");


static PyMethodDef
string_methods[] = {
    /* Counterparts of the obsolete stropmodule functions; except
       string.maketrans(). */
    {"join", (PyCFunction)string_join, METH_O, join__doc__},
    {"split", (PyCFunction)string_split, METH_VARARGS, split__doc__},
    {"rsplit", (PyCFunction)string_rsplit, METH_VARARGS, rsplit__doc__},
    {"lower", (PyCFunction)string_lower, METH_NOARGS, lower__doc__},
    {"upper", (PyCFunction)string_upper, METH_NOARGS, upper__doc__},
    {"islower", (PyCFunction)string_islower, METH_NOARGS, islower__doc__},
    {"isupper", (PyCFunction)string_isupper, METH_NOARGS, isupper__doc__},
    {"isspace", (PyCFunction)string_isspace, METH_NOARGS, isspace__doc__},
    {"isdigit", (PyCFunction)string_isdigit, METH_NOARGS, isdigit__doc__},
    {"istitle", (PyCFunction)string_istitle, METH_NOARGS, istitle__doc__},
    {"isalpha", (PyCFunction)string_isalpha, METH_NOARGS, isalpha__doc__},
    {"isalnum", (PyCFunction)string_isalnum, METH_NOARGS, isalnum__doc__},
    {"capitalize", (PyCFunction)string_capitalize, METH_NOARGS,
     capitalize__doc__},
    {"count", (PyCFunction)string_count, METH_VARARGS, count__doc__},
    {"endswith", (PyCFunction)string_endswith, METH_VARARGS,
     endswith__doc__},
    {"partition", (PyCFunction)string_partition, METH_O, partition__doc__},
    {"find", (PyCFunction)string_find, METH_VARARGS, find__doc__},
    {"index", (PyCFunction)string_index, METH_VARARGS, index__doc__},
    {"lstrip", (PyCFunction)string_lstrip, METH_VARARGS, lstrip__doc__},
    {"replace", (PyCFunction)string_replace, METH_VARARGS, replace__doc__},
    {"rfind", (PyCFunction)string_rfind, METH_VARARGS, rfind__doc__},
    {"rindex", (PyCFunction)string_rindex, METH_VARARGS, rindex__doc__},
    {"rstrip", (PyCFunction)string_rstrip, METH_VARARGS, rstrip__doc__},
    {"rpartition", (PyCFunction)string_rpartition, METH_O,
     rpartition__doc__},
    {"startswith", (PyCFunction)string_startswith, METH_VARARGS,
     startswith__doc__},
    {"strip", (PyCFunction)string_strip, METH_VARARGS, strip__doc__},
    {"swapcase", (PyCFunction)string_swapcase, METH_NOARGS,
     swapcase__doc__},
    {"translate", (PyCFunction)string_translate, METH_VARARGS,
     translate__doc__},
    {"title", (PyCFunction)string_title, METH_NOARGS, title__doc__},
    {"ljust", (PyCFunction)string_ljust, METH_VARARGS, ljust__doc__},
    {"rjust", (PyCFunction)string_rjust, METH_VARARGS, rjust__doc__},
    {"center", (PyCFunction)string_center, METH_VARARGS, center__doc__},
    {"zfill", (PyCFunction)string_zfill, METH_VARARGS, zfill__doc__},
    {"format", (PyCFunction) do_string_format, METH_VARARGS | METH_KEYWORDS, format__doc__},
    {"__format__", (PyCFunction) string__format__, METH_VARARGS, p_format__doc__},
    {"_formatter_field_name_split", (PyCFunction) formatter_field_name_split, METH_NOARGS},
    {"_formatter_parser", (PyCFunction) formatter_parser, METH_NOARGS},
    {"encode", (PyCFunction)string_encode, METH_VARARGS | METH_KEYWORDS, encode__doc__},
    {"decode", (PyCFunction)string_decode, METH_VARARGS | METH_KEYWORDS, decode__doc__},
    {"expandtabs", (PyCFunction)string_expandtabs, METH_VARARGS,
     expandtabs__doc__},
    {"splitlines", (PyCFunction)string_splitlines, METH_VARARGS,
     splitlines__doc__},
    {"__sizeof__", (PyCFunction)string_sizeof, METH_NOARGS,
     sizeof__doc__},
    {"__getnewargs__",          (PyCFunction)string_getnewargs, METH_NOARGS},
    {NULL,     NULL}                         /* sentinel */
};

static PyObject *
str_subtype_new(PyTypeObject *type, PyObject *args, PyObject *kwds);

static PyObject *
string_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *x = NULL;
    static char *kwlist[] = {"object", 0};

    if (type != &PyString_Type)
        return str_subtype_new(type, args, kwds);
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O:str", kwlist, &x))
        return NULL;
    if (x == NULL)
        return PyString_FromString("");
    return PyObject_Str(x);
}

static PyObject *
str_subtype_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *tmp, *pnew;
    Py_ssize_t n;

    assert(PyType_IsSubtype(type, &PyString_Type));
    tmp = string_new(&PyString_Type, args, kwds);
    if (tmp == NULL)
        return NULL;
    assert(PyString_CheckExact(tmp));
    n = PyString_GET_SIZE(tmp);
    pnew = type->tp_alloc(type, n);
    if (pnew != NULL) {
        Py_MEMCPY(PyString_AS_STRING(pnew), PyString_AS_STRING(tmp), n+1);
        ((PyStringObject *)pnew)->ob_shash =
            ((PyStringObject *)tmp)->ob_shash;
        ((PyStringObject *)pnew)->ob_sstate = SSTATE_NOT_INTERNED;
    }
    Py_DECREF(tmp);
    return pnew;
}

static PyObject *
basestring_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyErr_SetString(PyExc_TypeError,
                    "The basestring type cannot be instantiated");
    return NULL;
}

static PyObject *
string_mod(PyObject *v, PyObject *w)
{
    if (!PyString_Check(v)) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }
    return PyString_Format(v, w);
}

PyDoc_STRVAR(basestring_doc,
"Type basestring cannot be instantiated; it is the base for str and unicode.");

static PyNumberMethods string_as_number = {
    0,                          /*nb_add*/
    0,                          /*nb_subtract*/
    0,                          /*nb_multiply*/
    0,                          /*nb_divide*/
    string_mod,                 /*nb_remainder*/
};


PyTypeObject PyBaseString_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "basestring",
    0,
    0,
    0,                                          /* tp_dealloc */
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
    0,                                          /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    basestring_doc,                             /* tp_doc */
    0,                                          /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    0,                                          /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    &PyBaseObject_Type,                         /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_alloc */
    basestring_new,                             /* tp_new */
    0,                                          /* tp_free */
};

PyDoc_STRVAR(string_doc,
"str(object) -> string\n\
\n\
Return a nice string representation of the object.\n\
If the argument is a string, the return value is the same object.");

PyTypeObject PyString_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "str",
    PyStringObject_SIZE,
    sizeof(char),
    string_dealloc,                             /* tp_dealloc */
    (printfunc)string_print,                    /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
    0,                                          /* tp_compare */
    string_repr,                                /* tp_repr */
    &string_as_number,                          /* tp_as_number */
    &string_as_sequence,                        /* tp_as_sequence */
    &string_as_mapping,                         /* tp_as_mapping */
    (hashfunc)string_hash,                      /* tp_hash */
    0,                                          /* tp_call */
    string_str,                                 /* tp_str */
    PyObject_GenericGetAttr,                    /* tp_getattro */
    0,                                          /* tp_setattro */
    &string_as_buffer,                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_CHECKTYPES |
        Py_TPFLAGS_BASETYPE | Py_TPFLAGS_STRING_SUBCLASS |
        Py_TPFLAGS_HAVE_NEWBUFFER,              /* tp_flags */
    string_doc,                                 /* tp_doc */
    0,                                          /* tp_traverse */
    0,                                          /* tp_clear */
    (richcmpfunc)string_richcompare,            /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    string_methods,                             /* tp_methods */
    0,                                          /* tp_members */
    0,                                          /* tp_getset */
    &PyBaseString_Type,                         /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_alloc */
    string_new,                                 /* tp_new */
    PyObject_Del,                               /* tp_free */
};

void
PyString_Concat(register PyObject **pv, register PyObject *w)
{
    register PyObject *v;
    if (*pv == NULL)
        return;
    if (w == NULL || !PyString_Check(*pv)) {
        Py_DECREF(*pv);
        *pv = NULL;
        return;
    }
    v = string_concat((PyStringObject *) *pv, w);
    Py_DECREF(*pv);
    *pv = v;
}

void
PyString_ConcatAndDel(register PyObject **pv, register PyObject *w)
{
    PyString_Concat(pv, w);
    Py_XDECREF(w);
}


/* The following function breaks the notion that strings are immutable:
   it changes the size of a string.  We get away with this only if there
   is only one module referencing the object.  You can also think of it
   as creating a new string object and destroying the old one, only
   more efficiently.  In any case, don't use this if the string may
   already be known to some other part of the code...
   Note that if there's not enough memory to resize the string, the original
   string object at *pv is deallocated, *pv is set to NULL, an "out of
   memory" exception is set, and -1 is returned.  Else (on success) 0 is
   returned, and the value in *pv may or may not be the same as on input.
   As always, an extra byte is allocated for a trailing \0 byte (newsize
   does *not* include that), and a trailing \0 byte is stored.
*/

int
_PyString_Resize(PyObject **pv, Py_ssize_t newsize)
{
    register PyObject *v;
    register PyStringObject *sv;
    v = *pv;
    if (!PyString_Check(v) || Py_REFCNT(v) != 1 || newsize < 0 ||
        PyString_CHECK_INTERNED(v)) {
        *pv = 0;
        Py_DECREF(v);
        PyErr_BadInternalCall();
        return -1;
    }
    /* XXX UNREF/NEWREF interface should be more symmetrical */
    _Py_DEC_REFTOTAL;
    _Py_ForgetReference(v);
    *pv = (PyObject *)
        PyObject_REALLOC((char *)v, PyStringObject_SIZE + newsize);
    if (*pv == NULL) {
        PyObject_Del(v);
        PyErr_NoMemory();
        return -1;
    }
    _Py_NewReference(*pv);
    sv = (PyStringObject *) *pv;
    Py_SIZE(sv) = newsize;
    sv->ob_sval[newsize] = '\0';
    sv->ob_shash = -1;          /* invalidate cached hash value */
    return 0;
}

/* Helpers for formatstring */

Py_LOCAL_INLINE(PyObject *)
getnextarg(PyObject *args, Py_ssize_t arglen, Py_ssize_t *p_argidx)
{
    Py_ssize_t argidx = *p_argidx;
    if (argidx < arglen) {
        (*p_argidx)++;
        if (arglen < 0)
            return args;
        else
            return PyTuple_GetItem(args, argidx);
    }
    PyErr_SetString(PyExc_TypeError,
                    "not enough arguments for format string");
    return NULL;
}

/* Format codes
 * F_LJUST      '-'
 * F_SIGN       '+'
 * F_BLANK      ' '
 * F_ALT        '#'
 * F_ZERO       '0'
 */
#define F_LJUST (1<<0)
#define F_SIGN  (1<<1)
#define F_BLANK (1<<2)
#define F_ALT   (1<<3)
#define F_ZERO  (1<<4)

/* Returns a new reference to a PyString object, or NULL on failure. */

static PyObject *
formatfloat(PyObject *v, int flags, int prec, int type)
{
    char *p;
    PyObject *result;
    double x;

    x = PyFloat_AsDouble(v);
    if (x == -1.0 && PyErr_Occurred()) {
        PyErr_Format(PyExc_TypeError, "float argument required, "
                     "not %.200s", Py_TYPE(v)->tp_name);
        return NULL;
    }

    if (prec < 0)
        prec = 6;

    p = PyOS_double_to_string(x, type, prec,
                              (flags & F_ALT) ? Py_DTSF_ALT : 0, NULL);

    if (p == NULL)
        return NULL;
    result = PyString_FromStringAndSize(p, strlen(p));
    PyMem_Free(p);
    return result;
}

/* _PyString_FormatLong emulates the format codes d, u, o, x and X, and
 * the F_ALT flag, for Python's long (unbounded) ints.  It's not used for
 * Python's regular ints.
 * Return value:  a new PyString*, or NULL if error.
 *  .  *pbuf is set to point into it,
 *     *plen set to the # of chars following that.
 *     Caller must decref it when done using pbuf.
 *     The string starting at *pbuf is of the form
 *         "-"? ("0x" | "0X")? digit+
 *     "0x"/"0X" are present only for x and X conversions, with F_ALT
 *         set in flags.  The case of hex digits will be correct,
 *     There will be at least prec digits, zero-filled on the left if
 *         necessary to get that many.
 * val          object to be converted
 * flags        bitmask of format flags; only F_ALT is looked at
 * prec         minimum number of digits; 0-fill on left if needed
 * type         a character in [duoxX]; u acts the same as d
 *
 * CAUTION:  o, x and X conversions on regular ints can never
 * produce a '-' sign, but can for Python's unbounded ints.
 */
PyObject*
_PyString_FormatLong(PyObject *val, int flags, int prec, int type,
                     char **pbuf, int *plen)
{
    PyObject *result = NULL;
    char *buf;
    Py_ssize_t i;
    int sign;           /* 1 if '-', else 0 */
    int len;            /* number of characters */
    Py_ssize_t llen;
    int numdigits;      /* len == numnondigits + numdigits */
    int numnondigits = 0;

    switch (type) {
    case 'd':
    case 'u':
        result = Py_TYPE(val)->tp_str(val);
        break;
    case 'o':
        result = Py_TYPE(val)->tp_as_number->nb_oct(val);
        break;
    case 'x':
    case 'X':
        numnondigits = 2;
        result = Py_TYPE(val)->tp_as_number->nb_hex(val);
        break;
    default:
        assert(!"'type' not in [duoxX]");
    }
    if (!result)
        return NULL;

    buf = PyString_AsString(result);
    if (!buf) {
        Py_DECREF(result);
        return NULL;
    }

    /* To modify the string in-place, there can only be one reference. */
    if (Py_REFCNT(result) != 1) {
        PyErr_BadInternalCall();
        return NULL;
    }
    llen = PyString_Size(result);
    if (llen > INT_MAX) {
        PyErr_SetString(PyExc_ValueError, "string too large in _PyString_FormatLong");
        return NULL;
    }
    len = (int)llen;
    if (buf[len-1] == 'L') {
        --len;
        buf[len] = '\0';
    }
    sign = buf[0] == '-';
    numnondigits += sign;
    numdigits = len - numnondigits;
    assert(numdigits > 0);

    /* Get rid of base marker unless F_ALT */
    if ((flags & F_ALT) == 0) {
        /* Need to skip 0x, 0X or 0. */
        int skipped = 0;
        switch (type) {
        case 'o':
            assert(buf[sign] == '0');
            /* If 0 is only digit, leave it alone. */
            if (numdigits > 1) {
                skipped = 1;
                --numdigits;
            }
            break;
        case 'x':
        case 'X':
            assert(buf[sign] == '0');
            assert(buf[sign + 1] == 'x');
            skipped = 2;
            numnondigits -= 2;
            break;
        }
        if (skipped) {
            buf += skipped;
            len -= skipped;
            if (sign)
                buf[0] = '-';
        }
        assert(len == numnondigits + numdigits);
        assert(numdigits > 0);
    }

    /* Fill with leading zeroes to meet minimum width. */
    if (prec > numdigits) {
        PyObject *r1 = PyString_FromStringAndSize(NULL,
                                numnondigits + prec);
        char *b1;
        if (!r1) {
            Py_DECREF(result);
            return NULL;
        }
        b1 = PyString_AS_STRING(r1);
        for (i = 0; i < numnondigits; ++i)
            *b1++ = *buf++;
        for (i = 0; i < prec - numdigits; i++)
            *b1++ = '0';
        for (i = 0; i < numdigits; i++)
            *b1++ = *buf++;
        *b1 = '\0';
        Py_DECREF(result);
        result = r1;
        buf = PyString_AS_STRING(result);
        len = numnondigits + prec;
    }

    /* Fix up case for hex conversions. */
    if (type == 'X') {
        /* Need to convert all lower case letters to upper case.
           and need to convert 0x to 0X (and -0x to -0X). */
        for (i = 0; i < len; i++)
            if (buf[i] >= 'a' && buf[i] <= 'x')
                buf[i] -= 'a'-'A';
    }
    *pbuf = buf;
    *plen = len;
    return result;
}

Py_LOCAL_INLINE(int)
formatint(char *buf, size_t buflen, int flags,
          int prec, int type, PyObject *v)
{
    /* fmt = '%#.' + `prec` + 'l' + `type`
       worst case length = 3 + 19 (worst len of INT_MAX on 64-bit machine)
       + 1 + 1 = 24 */
    char fmt[64];       /* plenty big enough! */
    char *sign;
    long x;

    x = PyInt_AsLong(v);
    if (x == -1 && PyErr_Occurred()) {
        PyErr_Format(PyExc_TypeError, "int argument required, not %.200s",
                     Py_TYPE(v)->tp_name);
        return -1;
    }
    if (x < 0 && type == 'u') {
        type = 'd';
    }
    if (x < 0 && (type == 'x' || type == 'X' || type == 'o'))
        sign = "-";
    else
        sign = "";
    if (prec < 0)
        prec = 1;

    if ((flags & F_ALT) &&
        (type == 'x' || type == 'X')) {
        /* When converting under %#x or %#X, there are a number
         * of issues that cause pain:
         * - when 0 is being converted, the C standard leaves off
         *   the '0x' or '0X', which is inconsistent with other
         *   %#x/%#X conversions and inconsistent with Python's
         *   hex() function
         * - there are platforms that violate the standard and
         *   convert 0 with the '0x' or '0X'
         *   (Metrowerks, Compaq Tru64)
         * - there are platforms that give '0x' when converting
         *   under %#X, but convert 0 in accordance with the
         *   standard (OS/2 EMX)
         *
         * We can achieve the desired consistency by inserting our
         * own '0x' or '0X' prefix, and substituting %x/%X in place
         * of %#x/%#X.
         *
         * Note that this is the same approach as used in
         * formatint() in unicodeobject.c
         */
        PyOS_snprintf(fmt, sizeof(fmt), "%s0%c%%.%dl%c",
                      sign, type, prec, type);
    }
    else {
        PyOS_snprintf(fmt, sizeof(fmt), "%s%%%s.%dl%c",
                      sign, (flags&F_ALT) ? "#" : "",
                      prec, type);
    }

    /* buf = '+'/'-'/'' + '0'/'0x'/'' + '[0-9]'*max(prec, len(x in octal))
     * worst case buf = '-0x' + [0-9]*prec, where prec >= 11
     */
    if (buflen <= 14 || buflen <= (size_t)3 + (size_t)prec) {
        PyErr_SetString(PyExc_OverflowError,
            "formatted integer is too long (precision too large?)");
        return -1;
    }
    if (sign[0])
        PyOS_snprintf(buf, buflen, fmt, -x);
    else
        PyOS_snprintf(buf, buflen, fmt, x);
    return (int)strlen(buf);
}

Py_LOCAL_INLINE(int)
formatchar(char *buf, size_t buflen, PyObject *v)
{
    /* presume that the buffer is at least 2 characters long */
    if (PyString_Check(v)) {
        if (!PyArg_Parse(v, "c;%c requires int or char", &buf[0]))
            return -1;
    }
    else {
        if (!PyArg_Parse(v, "b;%c requires int or char", &buf[0]))
            return -1;
    }
    buf[1] = '\0';
    return 1;
}

/* fmt%(v1,v2,...) is roughly equivalent to sprintf(fmt, v1, v2, ...)

   FORMATBUFLEN is the length of the buffer in which the ints &
   chars are formatted. XXX This is a magic number. Each formatting
   routine does bounds checking to ensure no overflow, but a better
   solution may be to malloc a buffer of appropriate size for each
   format. For now, the current solution is sufficient.
*/
#define FORMATBUFLEN (size_t)120

PyObject *
PyString_Format(PyObject *format, PyObject *args)
{
    char *fmt, *res;
    Py_ssize_t arglen, argidx;
    Py_ssize_t reslen, rescnt, fmtcnt;
    int args_owned = 0;
    PyObject *result, *orig_args;
#ifdef Py_USING_UNICODE
    PyObject *v, *w;
#endif
    PyObject *dict = NULL;
    if (format == NULL || !PyString_Check(format) || args == NULL) {
        PyErr_BadInternalCall();
        return NULL;
    }
    orig_args = args;
    fmt = PyString_AS_STRING(format);
    fmtcnt = PyString_GET_SIZE(format);
    reslen = rescnt = fmtcnt + 100;
    result = PyString_FromStringAndSize((char *)NULL, reslen);
    if (result == NULL)
        return NULL;
    res = PyString_AsString(result);
    if (PyTuple_Check(args)) {
        arglen = PyTuple_GET_SIZE(args);
        argidx = 0;
    }
    else {
        arglen = -1;
        argidx = -2;
    }
    if (Py_TYPE(args)->tp_as_mapping && !PyTuple_Check(args) &&
        !PyObject_TypeCheck(args, &PyBaseString_Type))
        dict = args;
    while (--fmtcnt >= 0) {
        if (*fmt != '%') {
            if (--rescnt < 0) {
                rescnt = fmtcnt + 100;
                reslen += rescnt;
                if (_PyString_Resize(&result, reslen))
                    return NULL;
                res = PyString_AS_STRING(result)
                    + reslen - rescnt;
                --rescnt;
            }
            *res++ = *fmt++;
        }
        else {
            /* Got a format specifier */
            int flags = 0;
            Py_ssize_t width = -1;
            int prec = -1;
            int c = '\0';
            int fill;
            int isnumok;
            PyObject *v = NULL;
            PyObject *temp = NULL;
            char *pbuf;
            int sign;
            Py_ssize_t len;
            char formatbuf[FORMATBUFLEN];
                 /* For format{int,char}() */
#ifdef Py_USING_UNICODE
            char *fmt_start = fmt;
            Py_ssize_t argidx_start = argidx;
#endif

            fmt++;
            if (*fmt == '(') {
                char *keystart;
                Py_ssize_t keylen;
                PyObject *key;
                int pcount = 1;

                if (dict == NULL) {
                    PyErr_SetString(PyExc_TypeError,
                             "format requires a mapping");
                    goto error;
                }
                ++fmt;
                --fmtcnt;
                keystart = fmt;
                /* Skip over balanced parentheses */
                while (pcount > 0 && --fmtcnt >= 0) {
                    if (*fmt == ')')
                        --pcount;
                    else if (*fmt == '(')
                        ++pcount;
                    fmt++;
                }
                keylen = fmt - keystart - 1;
                if (fmtcnt < 0 || pcount > 0) {
                    PyErr_SetString(PyExc_ValueError,
                               "incomplete format key");
                    goto error;
                }
                key = PyString_FromStringAndSize(keystart,
                                                 keylen);
                if (key == NULL)
                    goto error;
                if (args_owned) {
                    Py_DECREF(args);
                    args_owned = 0;
                }
                args = PyObject_GetItem(dict, key);
                Py_DECREF(key);
                if (args == NULL) {
                    goto error;
                }
                args_owned = 1;
                arglen = -1;
                argidx = -2;
            }
            while (--fmtcnt >= 0) {
                switch (c = *fmt++) {
                case '-': flags |= F_LJUST; continue;
                case '+': flags |= F_SIGN; continue;
                case ' ': flags |= F_BLANK; continue;
                case '#': flags |= F_ALT; continue;
                case '0': flags |= F_ZERO; continue;
                }
                break;
            }
            if (c == '*') {
                v = getnextarg(args, arglen, &argidx);
                if (v == NULL)
                    goto error;
                if (!PyInt_Check(v)) {
                    PyErr_SetString(PyExc_TypeError,
                                    "* wants int");
                    goto error;
                }
                width = PyInt_AsLong(v);
                if (width < 0) {
                    flags |= F_LJUST;
                    width = -width;
                }
                if (--fmtcnt >= 0)
                    c = *fmt++;
            }
            else if (c >= 0 && isdigit(c)) {
                width = c - '0';
                while (--fmtcnt >= 0) {
                    c = Py_CHARMASK(*fmt++);
                    if (!isdigit(c))
                        break;
                    if ((width*10) / 10 != width) {
                        PyErr_SetString(
                            PyExc_ValueError,
                            "width too big");
                        goto error;
                    }
                    width = width*10 + (c - '0');
                }
            }
            if (c == '.') {
                prec = 0;
                if (--fmtcnt >= 0)
                    c = *fmt++;
                if (c == '*') {
                    v = getnextarg(args, arglen, &argidx);
                    if (v == NULL)
                        goto error;
                    if (!PyInt_Check(v)) {
                        PyErr_SetString(
                            PyExc_TypeError,
                            "* wants int");
                        goto error;
                    }
                    prec = PyInt_AsLong(v);
                    if (prec < 0)
                        prec = 0;
                    if (--fmtcnt >= 0)
                        c = *fmt++;
                }
                else if (c >= 0 && isdigit(c)) {
                    prec = c - '0';
                    while (--fmtcnt >= 0) {
                        c = Py_CHARMASK(*fmt++);
                        if (!isdigit(c))
                            break;
                        if ((prec*10) / 10 != prec) {
                            PyErr_SetString(
                                PyExc_ValueError,
                                "prec too big");
                            goto error;
                        }
                        prec = prec*10 + (c - '0');
                    }
                }
            } /* prec */
            if (fmtcnt >= 0) {
                if (c == 'h' || c == 'l' || c == 'L') {
                    if (--fmtcnt >= 0)
                        c = *fmt++;
                }
            }
            if (fmtcnt < 0) {
                PyErr_SetString(PyExc_ValueError,
                                "incomplete format");
                goto error;
            }
            if (c != '%') {
                v = getnextarg(args, arglen, &argidx);
                if (v == NULL)
                    goto error;
            }
            sign = 0;
            fill = ' ';
            switch (c) {
            case '%':
                pbuf = "%";
                len = 1;
                break;
            case 's':
#ifdef Py_USING_UNICODE
                if (PyUnicode_Check(v)) {
                    fmt = fmt_start;
                    argidx = argidx_start;
                    goto unicode;
                }
#endif
                temp = _PyObject_Str(v);
#ifdef Py_USING_UNICODE
                if (temp != NULL && PyUnicode_Check(temp)) {
                    Py_DECREF(temp);
                    fmt = fmt_start;
                    argidx = argidx_start;
                    goto unicode;
                }
#endif
                /* Fall through */
            case 'r':
                if (c == 'r')
                    temp = PyObject_Repr(v);
                if (temp == NULL)
                    goto error;
                if (!PyString_Check(temp)) {
                    PyErr_SetString(PyExc_TypeError,
                      "%s argument has non-string str()");
                    Py_DECREF(temp);
                    goto error;
                }
                pbuf = PyString_AS_STRING(temp);
                len = PyString_GET_SIZE(temp);
                if (prec >= 0 && len > prec)
                    len = prec;
                break;
            case 'i':
            case 'd':
            case 'u':
            case 'o':
            case 'x':
            case 'X':
                if (c == 'i')
                    c = 'd';
                isnumok = 0;
                if (PyNumber_Check(v)) {
                    PyObject *iobj=NULL;

                    if (PyInt_Check(v) || (PyLong_Check(v))) {
                        iobj = v;
                        Py_INCREF(iobj);
                    }
                    else {
                        iobj = PyNumber_Int(v);
                        if (iobj==NULL) iobj = PyNumber_Long(v);
                    }
                    if (iobj!=NULL) {
                        if (PyInt_Check(iobj)) {
                            isnumok = 1;
                            pbuf = formatbuf;
                            len = formatint(pbuf,
                                            sizeof(formatbuf),
                                            flags, prec, c, iobj);
                            Py_DECREF(iobj);
                            if (len < 0)
                                goto error;
                            sign = 1;
                        }
                        else if (PyLong_Check(iobj)) {
                            int ilen;

                            isnumok = 1;
                            temp = _PyString_FormatLong(iobj, flags,
                                prec, c, &pbuf, &ilen);
                            Py_DECREF(iobj);
                            len = ilen;
                            if (!temp)
                                goto error;
                            sign = 1;
                        }
                        else {
                            Py_DECREF(iobj);
                        }
                    }
                }
                if (!isnumok) {
                    PyErr_Format(PyExc_TypeError,
                        "%%%c format: a number is required, "
                        "not %.200s", c, Py_TYPE(v)->tp_name);
                    goto error;
                }
                if (flags & F_ZERO)
                    fill = '0';
                break;
            case 'e':
            case 'E':
            case 'f':
            case 'F':
            case 'g':
            case 'G':
                temp = formatfloat(v, flags, prec, c);
                if (temp == NULL)
                    goto error;
                pbuf = PyString_AS_STRING(temp);
                len = PyString_GET_SIZE(temp);
                sign = 1;
                if (flags & F_ZERO)
                    fill = '0';
                break;
            case 'c':
#ifdef Py_USING_UNICODE
                if (PyUnicode_Check(v)) {
                    fmt = fmt_start;
                    argidx = argidx_start;
                    goto unicode;
                }
#endif
                pbuf = formatbuf;
                len = formatchar(pbuf, sizeof(formatbuf), v);
                if (len < 0)
                    goto error;
                break;
            default:
                PyErr_Format(PyExc_ValueError,
                  "unsupported format character '%c' (0x%x) "
                  "at index %zd",
                  c, c,
                  (Py_ssize_t)(fmt - 1 -
                               PyString_AsString(format)));
                goto error;
            }
            if (sign) {
                if (*pbuf == '-' || *pbuf == '+') {
                    sign = *pbuf++;
                    len--;
                }
                else if (flags & F_SIGN)
                    sign = '+';
                else if (flags & F_BLANK)
                    sign = ' ';
                else
                    sign = 0;
            }
            if (width < len)
                width = len;
            if (rescnt - (sign != 0) < width) {
                reslen -= rescnt;
                rescnt = width + fmtcnt + 100;
                reslen += rescnt;
                if (reslen < 0) {
                    Py_DECREF(result);
                    Py_XDECREF(temp);
                    return PyErr_NoMemory();
                }
                if (_PyString_Resize(&result, reslen)) {
                    Py_XDECREF(temp);
                    return NULL;
                }
                res = PyString_AS_STRING(result)
                    + reslen - rescnt;
            }
            if (sign) {
                if (fill != ' ')
                    *res++ = sign;
                rescnt--;
                if (width > len)
                    width--;
            }
            if ((flags & F_ALT) && (c == 'x' || c == 'X')) {
                assert(pbuf[0] == '0');
                assert(pbuf[1] == c);
                if (fill != ' ') {
                    *res++ = *pbuf++;
                    *res++ = *pbuf++;
                }
                rescnt -= 2;
                width -= 2;
                if (width < 0)
                    width = 0;
                len -= 2;
            }
            if (width > len && !(flags & F_LJUST)) {
                do {
                    --rescnt;
                    *res++ = fill;
                } while (--width > len);
            }
            if (fill == ' ') {
                if (sign)
                    *res++ = sign;
                if ((flags & F_ALT) &&
                    (c == 'x' || c == 'X')) {
                    assert(pbuf[0] == '0');
                    assert(pbuf[1] == c);
                    *res++ = *pbuf++;
                    *res++ = *pbuf++;
                }
            }
            Py_MEMCPY(res, pbuf, len);
            res += len;
            rescnt -= len;
            while (--width >= len) {
                --rescnt;
                *res++ = ' ';
            }
            if (dict && (argidx < arglen) && c != '%') {
                PyErr_SetString(PyExc_TypeError,
                           "not all arguments converted during string formatting");
                Py_XDECREF(temp);
                goto error;
            }
            Py_XDECREF(temp);
        } /* '%' */
    } /* until end */
    if (argidx < arglen && !dict) {
        PyErr_SetString(PyExc_TypeError,
                        "not all arguments converted during string formatting");
        goto error;
    }
    if (args_owned) {
        Py_DECREF(args);
    }
    if (_PyString_Resize(&result, reslen - rescnt))
        return NULL;
    return result;

#ifdef Py_USING_UNICODE
 unicode:
    if (args_owned) {
        Py_DECREF(args);
        args_owned = 0;
    }
    /* Fiddle args right (remove the first argidx arguments) */
    if (PyTuple_Check(orig_args) && argidx > 0) {
        PyObject *v;
        Py_ssize_t n = PyTuple_GET_SIZE(orig_args) - argidx;
        v = PyTuple_New(n);
        if (v == NULL)
            goto error;
        while (--n >= 0) {
            PyObject *w = PyTuple_GET_ITEM(orig_args, n + argidx);
            Py_INCREF(w);
            PyTuple_SET_ITEM(v, n, w);
        }
        args = v;
    } else {
        Py_INCREF(orig_args);
        args = orig_args;
    }
    args_owned = 1;
    /* Take what we have of the result and let the Unicode formatting
       function format the rest of the input. */
    rescnt = res - PyString_AS_STRING(result);
    if (_PyString_Resize(&result, rescnt))
        goto error;
    fmtcnt = PyString_GET_SIZE(format) - \
             (fmt - PyString_AS_STRING(format));
    format = PyUnicode_Decode(fmt, fmtcnt, NULL, NULL);
    if (format == NULL)
        goto error;
    v = PyUnicode_Format(format, args);
    Py_DECREF(format);
    if (v == NULL)
        goto error;
    /* Paste what we have (result) to what the Unicode formatting
       function returned (v) and return the result (or error) */
    w = PyUnicode_Concat(result, v);
    Py_DECREF(result);
    Py_DECREF(v);
    Py_DECREF(args);
    return w;
#endif /* Py_USING_UNICODE */

 error:
    Py_DECREF(result);
    if (args_owned) {
        Py_DECREF(args);
    }
    return NULL;
}

void
PyString_InternInPlace(PyObject **p)
{
    register PyStringObject *s = (PyStringObject *)(*p);
    PyObject *t;
    if (s == NULL || !PyString_Check(s))
        Py_FatalError("PyString_InternInPlace: strings only please!");
    /* If it's a string subclass, we don't really know what putting
       it in the interned dict might do. */
    if (!PyString_CheckExact(s))
        return;
    if (PyString_CHECK_INTERNED(s))
        return;
    if (interned == NULL) {
        interned = PyDict_New();
        if (interned == NULL) {
            PyErr_Clear(); /* Don't leave an exception */
            return;
        }
    }
    t = PyDict_GetItem(interned, (PyObject *)s);
    if (t) {
        Py_INCREF(t);
        Py_DECREF(*p);
        *p = t;
        return;
    }

    if (PyDict_SetItem(interned, (PyObject *)s, (PyObject *)s) < 0) {
        PyErr_Clear();
        return;
    }
    /* The two references in interned are not counted by refcnt.
       The string deallocator will take care of this */
    Py_REFCNT(s) -= 2;
    PyString_CHECK_INTERNED(s) = SSTATE_INTERNED_MORTAL;
}

void
PyString_InternImmortal(PyObject **p)
{
    PyString_InternInPlace(p);
    if (PyString_CHECK_INTERNED(*p) != SSTATE_INTERNED_IMMORTAL) {
        PyString_CHECK_INTERNED(*p) = SSTATE_INTERNED_IMMORTAL;
        Py_INCREF(*p);
    }
}


PyObject *
PyString_InternFromString(const char *cp)
{
    PyObject *s = PyString_FromString(cp);
    if (s == NULL)
        return NULL;
    PyString_InternInPlace(&s);
    return s;
}

void
PyString_Fini(void)
{
    int i;
    for (i = 0; i < UCHAR_MAX + 1; i++) {
        Py_XDECREF(characters[i]);
        characters[i] = NULL;
    }
    Py_XDECREF(nullstring);
    nullstring = NULL;
}

void _Py_ReleaseInternedStrings(void)
{
    PyObject *keys;
    PyStringObject *s;
    Py_ssize_t i, n;
    Py_ssize_t immortal_size = 0, mortal_size = 0;

    if (interned == NULL || !PyDict_Check(interned))
        return;
    keys = PyDict_Keys(interned);
    if (keys == NULL || !PyList_Check(keys)) {
        PyErr_Clear();
        return;
    }

    /* Since _Py_ReleaseInternedStrings() is intended to help a leak
       detector, interned strings are not forcibly deallocated; rather, we
       give them their stolen references back, and then clear and DECREF
       the interned dict. */

    n = PyList_GET_SIZE(keys);
    fprintf(stderr, "releasing %" PY_FORMAT_SIZE_T "d interned strings\n",
        n);
    for (i = 0; i < n; i++) {
        s = (PyStringObject *) PyList_GET_ITEM(keys, i);
        switch (s->ob_sstate) {
        case SSTATE_NOT_INTERNED:
            /* XXX Shouldn't happen */
            break;
        case SSTATE_INTERNED_IMMORTAL:
            Py_REFCNT(s) += 1;
            immortal_size += Py_SIZE(s);
            break;
        case SSTATE_INTERNED_MORTAL:
            Py_REFCNT(s) += 2;
            mortal_size += Py_SIZE(s);
            break;
        default:
            Py_FatalError("Inconsistent interned string state.");
        }
        s->ob_sstate = SSTATE_NOT_INTERNED;
    }
    fprintf(stderr, "total size of all interned strings: "
                    "%" PY_FORMAT_SIZE_T "d/%" PY_FORMAT_SIZE_T "d "
                    "mortal/immortal\n", mortal_size, immortal_size);
    Py_DECREF(keys);
    PyDict_Clear(interned);
    Py_DECREF(interned);
    interned = NULL;
}
