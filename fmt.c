#include "mpz.h"

extern PyObject * to_int(PyObject *self);

#if defined(ON_CPYTHON) && PY_VERSION_HEX >= 0x030D00A0

/*
    get_integer consumes 0 or more decimal digit characters from an
    input string, updates *result with the corresponding positive
    integer, and returns the number of digits consumed.

    returns -1 on error.
*/
static int
get_integer(PyObject *str, Py_ssize_t *ppos, Py_ssize_t end,
            Py_ssize_t *result)
{
    Py_ssize_t accumulator = 0, digitval, pos = *ppos;
    int numdigits = 0;
    int kind = PyUnicode_KIND(str);
    const void *data = PyUnicode_DATA(str);

    for (; pos < end; pos++, numdigits++) {
        digitval = Py_UNICODE_TODECIMAL(PyUnicode_READ(kind, data, pos));
        if (digitval < 0) {
            break;
        }
        /*
           Detect possible overflow before it happens:

              accumulator * 10 + digitval > PY_SSIZE_T_MAX if and only if
              accumulator > (PY_SSIZE_T_MAX - digitval) / 10.
        */
        if (accumulator > (PY_SSIZE_T_MAX - digitval) / 10) {
            PyErr_Format(PyExc_ValueError,
                         "Too many decimal digits in format string");
            *ppos = pos;
            return -1;
        }
        accumulator = accumulator * 10 + digitval;
    }
    *ppos = pos;
    *result = accumulator;
    return numdigits;
}

/************************************************************************/
/*********** standard format specifier parsing **************************/
/************************************************************************/

/* returns true if this character is a specifier alignment token */
Py_LOCAL_INLINE(int)
is_alignment_token(Py_UCS4 c)
{
    switch (c) {
    case '<':
    case '>':
    case '=':
    case '^':
        return 1;
    default:
        return 0;
    }
}

/* returns true if this character is a sign element */
Py_LOCAL_INLINE(int)
is_sign_element(Py_UCS4 c)
{
    switch (c) {
    case ' ':
    case '+':
    case '-':
        return 1;
    default:
        return 0;
    }
}

typedef struct {
    Py_UCS4 fill_char;
    Py_UCS4 align;
    int alternate;
    Py_UCS4 sign;
    Py_ssize_t width;
    Py_UCS4 thousands_separators;
    Py_UCS4 type;
} InternalFormatSpec;

/*
  ptr points to the start of the format_spec, end points just past its end.
  fills in format with the parsed information.
  returns 1 on success, 0 on failure.
  if failure, sets the exception
*/
static int
parse_internal_render_format_spec(PyObject *obj,
                                  PyObject *format_spec,
                                  Py_ssize_t start, Py_ssize_t end,
                                  InternalFormatSpec *format,
                                  Py_UCS4 default_type,
                                  Py_UCS4 default_align)
{
    Py_ssize_t pos = start;
    int kind = PyUnicode_KIND(format_spec);
    const void *data = PyUnicode_DATA(format_spec);
    /* end-pos is used throughout this code to specify the length of
       the input string */
#define READ_spec(index) PyUnicode_READ(kind, data, index)

    Py_ssize_t consumed;
    int align_specified = 0;
    int fill_char_specified = 0;

    format->fill_char = ' ';
    format->align = default_align;
    format->alternate = 0;
    format->sign = '\0';
    format->width = -1;
    format->thousands_separators = '\0';
    format->type = default_type;

    /* If the second char is an alignment token,
       then parse the fill char */
    if (end-pos >= 2 && is_alignment_token(READ_spec(pos+1))) {
        format->align = READ_spec(pos+1);
        format->fill_char = READ_spec(pos);
        fill_char_specified = 1;
        align_specified = 1;
        pos += 2;
    }
    else if (end-pos >= 1 && is_alignment_token(READ_spec(pos))) {
        format->align = READ_spec(pos);
        align_specified = 1;
        ++pos;
    }
    /* Parse the various sign options */
    if (end-pos >= 1 && is_sign_element(READ_spec(pos))) {
        format->sign = READ_spec(pos);
        ++pos;
    }
    /* If the next character is #, we're in alternate mode.  This only
       applies to integers. */
    if (end-pos >= 1 && READ_spec(pos) == '#') {
        format->alternate = 1;
        ++pos;
    }
    /* The special case for 0-padding (backwards compat) */
    if (!fill_char_specified && end-pos >= 1 && READ_spec(pos) == '0') {
        format->fill_char = '0';
        if (!align_specified && default_align == '>') {
            format->align = '=';
        }
        ++pos;
    }
    consumed = get_integer(format_spec, &pos, end, &format->width);
    if (consumed == -1) {
        /* Overflow error. Exception already set. */
        return 0;
    }
    /* If consumed is 0, we didn't consume any characters for the
       width. In that case, reset the width to -1, because
       get_integer() will have set it to zero. -1 is how we record
       that the width wasn't specified. */
    if (consumed == 0) {
        format->width = -1;
    }
    /* Underscore signifies add thousands separators */
    if (end-pos && READ_spec(pos) == '_') {
        format->thousands_separators = '_';
        ++pos;
    }
    /* Finally, parse the type field. */
    if (end-pos > 1) {
        /* More than one char remains, so this is an invalid format
           specifier. */
        /* Create a temporary object that contains the format spec we're
           operating on.  It's format_spec[start:end] (in Python syntax). */
        PyObject* actual_format_spec = PyUnicode_FromKindAndData(kind,
            (char*)data + kind*start, end-start);
        if (actual_format_spec != NULL) {
            PyErr_Format(PyExc_ValueError,
                         ("Invalid format specifier '%U' for object "
                          "of type '%.200s'"), actual_format_spec,
                         PyType_GetFullyQualifiedName(Py_TYPE(obj)));
            Py_DECREF(actual_format_spec);
        }
        return 0;
    }
    if (end-pos == 1) {
        format->type = READ_spec(pos);
        ++pos;
    }

    assert (format->align <= 127);
    assert (format->sign <= 127);
    return 1;
}

/* describes the layout for an integer, see the comment in
   calc_number_widths() for details */
typedef struct {
    Py_ssize_t n_lpadding;
    Py_ssize_t n_prefix;
    Py_ssize_t n_spadding;
    Py_ssize_t n_rpadding;
    char sign;
    Py_ssize_t n_sign;      /* number of digits needed for sign (0/1) */
    /* These 2 are not the widths of fields, but are needed by
       STRINGLIB_GROUPING. */
    Py_ssize_t n_digits;    /* The number of digits, including separators. */
} NumberFieldWidths;

/* Not all fields of format are used.
   Should this take discrete params in order to be more clear
   about what it does?  Or is passing a single format parameter easier
   and more efficient enough to justify a little obfuscation?
   Return -1 on error. */
static Py_ssize_t
calc_number_widths(NumberFieldWidths *spec, Py_ssize_t n_prefix,
                   Py_UCS4 sign_char, Py_ssize_t n_start,
                   Py_ssize_t n_end,
                   const InternalFormatSpec *format, Py_UCS4 *maxchar)
{
    Py_ssize_t n_non_digit_non_padding;
    Py_ssize_t n_padding;

    spec->n_digits = n_end - n_start;
    spec->n_lpadding = 0;
    spec->n_prefix = n_prefix;
    spec->n_spadding = 0;
    spec->n_rpadding = 0;
    spec->sign = '\0';
    spec->n_sign = 0;

    /* the output will look like:
       |                                                           |
       | <lpadding> <sign> <prefix> <spadding> <digits> <rpadding> |
       |                                                           |

       sign is computed from format->sign and the actual
       sign of the number

       prefix is given (it's for the '0x' prefix)

       digits is already known

       the total width is either given, or computed from the
       actual digits

       only one of lpadding, spadding, and rpadding can be non-zero,
       and it's calculated from the width and other fields
    */

    /* compute the various parts we're going to write */
    switch (format->sign) {
    case '+':
        /* always put a + or - */
        spec->n_sign = 1;
        spec->sign = (sign_char == '-' ? '-' : '+');
        break;
    case ' ':
        spec->n_sign = 1;
        spec->sign = (sign_char == '-' ? '-' : ' ');
        break;
    default:
        /* Not specified, or the default (-) */
        if (sign_char == '-') {
            spec->n_sign = 1;
            spec->sign = '-';
        }
    }
    /* The number of chars used for non-digits and non-padding. */
    n_non_digit_non_padding = spec->n_sign + spec->n_prefix;
    /* Given the desired width and the total of digit and non-digit
       space we consume, see if we need any padding. format->width can
       be negative (meaning no padding), but this code still works in
       that case. */
    n_padding = format->width - (n_non_digit_non_padding + spec->n_digits);
    if (n_padding > 0) {
        /* Some padding is needed. Determine if it's left, space, or right. */
        switch (format->align) {
        case '<':
            spec->n_rpadding = n_padding;
            break;
        case '^':
            spec->n_lpadding = n_padding / 2;
            spec->n_rpadding = n_padding - spec->n_lpadding;
            break;
        case '=':
            spec->n_spadding = n_padding;
            break;
        case '>':
            spec->n_lpadding = n_padding;
            break;
        /* LCOV_EXCL_START */
        default:
            Py_UNREACHABLE();
        /* LCOV_EXCL_STOP */
        }
    }
    if (spec->n_lpadding || spec->n_spadding || spec->n_rpadding) {
        *maxchar = Py_MAX(*maxchar, format->fill_char);
    }

    return (spec->n_lpadding + spec->n_sign + spec->n_prefix
            + spec->n_spadding + spec->n_digits + spec->n_rpadding);
}

/* Fill in the digit parts of a number's string representation,
   as determined in calc_number_widths().
   Return -1 on error, or 0 on success. */
static int
fill_number(PyUnicodeWriter *writer, const NumberFieldWidths *spec,
            PyObject *digits, Py_ssize_t d_start, PyObject *prefix,
            Py_ssize_t p_start, Py_UCS4 fill_char)
{
    /* Used to keep track of digits */
    Py_ssize_t d_pos = d_start;

    if (spec->n_lpadding) {
        for (Py_ssize_t i = 0; i < spec->n_lpadding; i++) {
            PyUnicodeWriter_WriteChar(writer, fill_char);
        }
    }
    if (spec->n_sign == 1) {
        PyUnicodeWriter_WriteChar(writer, (Py_UCS4)spec->sign);
    }
    if (spec->n_prefix) {
        PyUnicodeWriter_WriteSubstring(writer, prefix, p_start, spec->n_prefix + p_start);
    }
    if (spec->n_spadding) {
        for (Py_ssize_t i = 0; i < spec->n_spadding; i++) {
            PyUnicodeWriter_WriteChar(writer, fill_char);
        }
    }
    PyUnicodeWriter_WriteSubstring(writer, digits, d_pos,
                                   spec->n_digits + d_pos);
    d_pos += spec->n_digits;
    if (spec->n_rpadding) {
        for (Py_ssize_t i = 0; i < spec->n_rpadding; i++) {
            PyUnicodeWriter_WriteChar(writer, fill_char);
        }
    }
    return 0;
}

extern PyObject * MPZ_to_str(MPZ_Object *u, int base, char group,
                             int options, int width);
extern int OPT_PREFIX, OPT_SIGN;

static PyObject *
format_long_internal(MPZ_Object *value, const InternalFormatSpec *format)
{
    Py_UCS4 maxchar = 127;
    PyObject *tmp = NULL;
    Py_ssize_t inumeric_chars;
    Py_UCS4 sign_char = '\0';
    Py_ssize_t n_digits;       /* count of digits need from the computed string */
    Py_ssize_t n_prefix = 0;   /* Count of prefix chars, (e.g., '0x') */
    Py_ssize_t n_total;
    Py_ssize_t prefix = 0;
    NumberFieldWidths spec;
    int base;
    int leading_chars_to_skip = 0;  /* Number of characters added by
                                       MPZ_to_str that we want to
                                       skip over. */
    char group = 0;

    /* Compute the base and how many characters will be added by
       PyNumber_ToBase */
    switch (format->type) {
    case 'b':
        base = 2;
        leading_chars_to_skip = 2; /* 0b */
        break;
    case 'o':
        base = 8;
        leading_chars_to_skip = 2; /* 0o */
        break;
    case 'x':
        base = 16;
        leading_chars_to_skip = 2; /* 0x */
        break;
    case 'X':
        base = -16;
        leading_chars_to_skip = 2; /* 0x */
        break;
    default:  /* shouldn't be needed, but stops a compiler warning */
    case 'd':
        base = 10;
        break;
    }
    if (format->sign != '+' && format->sign != ' '
        && format->width == -1
        && !format->thousands_separators
        && MPZ_CheckExact(value))
    {
        /* Fast path */
        return MPZ_to_str(value, base, 0,
                          format->alternate ? OPT_PREFIX : 0, -1);
    }
    if (format->thousands_separators) {
        if (format->type == 'd') {
            group = 3;
        }
        else {
            group = 4;
        }
    }
    /* Do the hard part, converting to a string in a given base */
    tmp = MPZ_to_str(value, base, group,
                     OPT_PREFIX | ((format->sign == '+'
                                    || format->sign == ' ') ? OPT_SIGN : 0),
                     (format->fill_char == '0' && format->align == '=') ?
                     (int)format->width : -1);
    assert(PyUnicode_Check(tmp));
    if (tmp == NULL) {
        goto done; /* LCOV_EXCL_LINE */
    }
    /* The number of prefix chars is the same as the leading
       chars to skip */
    if (format->alternate) {
        n_prefix = leading_chars_to_skip;
    }
    inumeric_chars = 0;
    n_digits = PyUnicode_GetLength(tmp);
    prefix = inumeric_chars;
    /* Is a sign character present in the output?  If so, remember it
       and skip it */
    if (PyUnicode_ReadChar(tmp, inumeric_chars) == '-') {
        sign_char = '-';
        ++prefix;
        ++leading_chars_to_skip;
    }
    /* Skip over the leading chars (0x, 0b, etc.) */
    n_digits -= leading_chars_to_skip;
    inumeric_chars += leading_chars_to_skip;
    /* Calculate how much memory we'll need. */
    n_total = calc_number_widths(&spec, n_prefix, sign_char, inumeric_chars,
                                 inumeric_chars + n_digits,
                                 format, &maxchar);
    if (n_total == -1) {
        goto done; /* LCOV_EXCL_LINE */
    }
    /* Allocate the memory. */

    PyUnicodeWriter *writer = PyUnicodeWriter_Create(n_total);

    if (!writer || PyUnicodeWriter_WriteChar(writer, maxchar)) {
        goto done; /* LCOV_EXCL_LINE */
    }
    ((_PyUnicodeWriter *)writer)->pos = 0;
    /* Populate the memory. */
    if (fill_number(writer, &spec, tmp, inumeric_chars, tmp, prefix,
                    format->fill_char))
    {
        /* LCOV_EXCL_START */
        PyUnicodeWriter_Discard(writer);
        goto done;
        /* LCOV_EXCL_STOP */
    }
    return PyUnicodeWriter_Finish(writer);
done:
    Py_XDECREF(tmp);
    return NULL;
}

extern PyObject * to_float(PyObject *self);

PyObject *
__format__(PyObject *self, PyObject *format_spec)
{
    if (!PyUnicode_Check(format_spec)) {
        PyErr_Format(PyExc_TypeError,
                     "__format__() argument must be str, not %U",
                     PyType_GetFullyQualifiedName(Py_TYPE(format_spec)));
        return NULL;
    }

    Py_ssize_t end = PyUnicode_GetLength(format_spec);

    if (!end) {
       return PyObject_Str(self);
    }

    InternalFormatSpec format;
    unaryfunc cast = to_int;

    if (!parse_internal_render_format_spec(self, format_spec, 0, end,
                                           &format, 'd', '>'))
    {
        PyErr_Clear();
        cast = to_int;
        goto fallback;
    }

    switch (format.type) {
    case 'b':
    case 'd':
    case 'o':
    case 'x':
    case 'X':
        return format_long_internal((MPZ_Object *)self, &format);
    case 'c':
    case 'n':
        break;
    case 'e':
    case 'E':
    case 'f':
    case 'F':
    case 'g':
    case 'G':
    case '%':
        cast = to_float;
        break;
    default:
        {
            PyObject *type_name = PyType_GetFullyQualifiedName(Py_TYPE(self));

            /* %c might be out-of-range, hence the two cases. */
            if (format.type > 32 && format.type < 128) {
                PyErr_Format(PyExc_ValueError,
                             "Unknown format code '%c' for object of type '%U'",
                             (char)format.type, type_name);
            }
            else {
                PyErr_Format(PyExc_ValueError,
                             "Unknown format code '\\x%x' for object of type '%U'",
                             (unsigned int)format.type, type_name);
            }
        }
        return NULL;
    }

fallback:
    {
        PyObject *num = cast(self);

        if (!num) {
            return NULL; /* LCOV_EXCL_LINE */
        }

        PyObject *res = PyObject_CallMethod(num, "__format__", "O", format_spec);

        Py_DECREF(num);
        return res;
    }
}
#else
PyObject *
__format__(PyObject *self, PyObject *format_spec)
{
    PyObject *num = to_int(self);

    if (!num) {
        return NULL; /* LCOV_EXCL_LINE */
    }

    PyObject *res = PyObject_CallMethod(num, "__format__", "O", format_spec);

    Py_DECREF(num);
    return res;
}
#endif /* defined(ON_CPYTHON) && PY_VERSION_HEX >= 0x030D00A0 */
