// -*- related-file-name: "../include/click/args.hh" -*-
/*
 * args.{cc,hh} -- type-safe argument & configuration string parsing
 * Eddie Kohler
 *
 * Copyright (c) 2011 Regents of the University of California
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>
#include <click/glue.hh>
#include <click/args.hh>
#include <click/error.hh>
#include <click/router.hh>
#if !CLICK_TOOL
# include <click/nameinfo.hh>
# include <click/packet_anno.hh>
#endif
#if CLICK_USERLEVEL || CLICK_TOOL
# include <pwd.h>
#endif
#include <stdarg.h>
CLICK_DECLS

const ArgContext blank_args;

inline void
Args::initialize(const Vector<String> *conf)
{
    _conf = conf ? new Vector<String>(*conf) : 0;
    _slots = 0;
    _simple_slotbuf[0] = 0;
    _my_conf = !!_conf;
    _status = true;
    _simple_slotpos = 0;
    if (_conf)
	reset();
}

Args::Args(ErrorHandler *errh)
    : ArgContext(errh)
{
    initialize(0);
}

Args::Args(const Vector<String> &conf, ErrorHandler *errh)
    : ArgContext(errh)
{
    initialize(&conf);
}

#if !CLICK_TOOL
Args::Args(const Element *context, ErrorHandler *errh)
    : ArgContext(context, errh)
{
    initialize(0);
}

Args::Args(const Vector<String> &conf,
	   const Element *context, ErrorHandler *errh)
    : ArgContext(context, errh)
{
    initialize(&conf);
}
#endif

Args::Args(const Args &x)
    : ArgContext(x),
      _my_conf(false), _simple_slotpos(0), _conf(0), _slots(0)
{
    _simple_slotbuf[0] = 0;
    *this = x;
}

Args::~Args()
{
    if (_my_conf)
	delete _conf;
    while (Slot *s = _slots) {
	_slots = s->_next;
	delete s;
    }
}

Args &
Args::operator=(const Args &x)
{
    if (&x != this) {
	if ((_slots || _simple_slotbuf[0] || x._slots || x._simple_slotbuf[0])
	    && _errh)
	    _errh->warning("internal warning: ignoring assignment of Args slots");

	if (_my_conf)
	    delete _conf;
	if (x._my_conf) {
	    _conf = new Vector<String>(*x._conf);
	    _my_conf = true;
	} else {
	    _conf = x._conf;
	    _my_conf = false;
	}
	_kwpos = x._kwpos;
#if !CLICK_TOOL
	_context = x._context;
#endif
	_errh = x._errh;

	_arg_keyword = x._arg_keyword;
	_read_status = x._read_status;
	_status = x._status;
    }
    return *this;
}

void
Args::reset_from(int i)
{
    _kwpos.clear();
    if (_conf) {
	_kwpos.reserve(_conf->size());
	for (String *it = _conf->begin() + i; it != _conf->end(); ++it) {
	    const char *s = it->begin(), *ends = it->end();
	    while (s != ends &&
		   (isalnum((unsigned char) *s) || *s == '_'
		    || *s == ':' || *s == '.' || *s == '?' || *s == '!'))
		++s;
	    const char *t = s;
	    while (t != ends && isspace((unsigned char) *t))
		++t;
	    if (s == it->begin() || s == t || t == ends)
		_kwpos.push_back(0);
	    else
		_kwpos.push_back(s - it->begin());
	}
    }
}

Args &
Args::bind(Vector<String> &conf)
{
    if (_my_conf)
	delete _conf;
    _conf = &conf;
    _my_conf = false;
    return reset();
}

Args &
Args::push_back(const String &arg)
{
    if (!_conf) {
	_conf = new Vector<String>();
	_my_conf = true;
    }
    if (_conf) {
	int old_size = _conf->size();
	_conf->push_back(arg);
	reset_from(old_size);
    }
    return *this;
}

Args &
Args::push_back_args(const String &str)
{
    if (!_conf) {
	_conf = new Vector<String>();
	_my_conf = true;
    }
    if (_conf) {
	int old_size = _conf->size();
	cp_argvec(str, *_conf);
	reset_from(old_size);
    }
    return *this;
}

Args &
Args::push_back_words(const String &str)
{
    if (!_conf) {
	_conf = new Vector<String>();
	_my_conf = true;
    }
    if (_conf) {
	int old_size = _conf->size();
	cp_spacevec(str, *_conf);
	reset_from(old_size);
    }
    return *this;
}

inline int
Args::simple_slot_size(int size)
{
#if HAVE_INDIFFERENT_ALIGNMENT
    return 1 + size + SIZEOF_VOID_P;
#else
    if (size & (size - 1))
	// not power of 2, assume no alignment
	size = 1 + size;
    else
	size = size + size;
    if (size & (SIZEOF_VOID_P - 1))
	size += SIZEOF_VOID_P - (size & (SIZEOF_VOID_P - 1));
    return size + SIZEOF_VOID_P;
#endif
}

inline void
Args::simple_slot_info(int offset, int size, void *&slot, void **&pointer)
{
#if HAVE_INDIFFERENT_ALIGNMENT
    slot = &_simple_slotbuf[offset + 1];
    pointer = reinterpret_cast<void **>(&_simple_slotbuf[offset + 1 + size]);
#else
    if (size & (size - 1))
	offset += 1;
    else
	offset += size;
    slot = &_simple_slotbuf[offset];
    offset += size;
    if (offset & (SIZEOF_VOID_P - 1))
	offset += SIZEOF_VOID_P - (offset & (SIZEOF_VOID_P - 1));
    pointer = reinterpret_cast<void **>(&_simple_slotbuf[offset]);
#endif
}

void *
Args::simple_slot(void *ptr, size_t size)
{
    int offset = _simple_slotpos;
    while (offset < simple_slotbuf_size && _simple_slotbuf[offset] != 0)
	offset += simple_slot_size(_simple_slotbuf[offset]);

    if (size < (size_t) simple_slotbuf_size) {
	int new_offset = offset + simple_slot_size((int) size);
	if (new_offset <= simple_slotbuf_size) {
	    void *slot, **pointer;
	    simple_slot_info(offset, (int) size, slot, pointer);
	    _simple_slotbuf[offset] = (int) size;
	    *pointer = ptr;
	    if (new_offset < simple_slotbuf_size)
		_simple_slotbuf[new_offset] = 0;
	    return slot;
	}
    }

    BytesSlot *store = new BytesSlot(ptr, size);
    if (store && store->_slot) {
	store->_next = _slots;
	_slots = store;
	return store->_slot;
    } else {
	delete store;
	error("out of memory");
	return 0;
    }
}

String
ArgContext::error_prefix() const
{
    return _arg_keyword ? String(_arg_keyword) + ": " : String();
}

void
ArgContext::error(const char *fmt, ...) const
{
    va_list val;
    va_start(val, fmt);
    xmessage(ErrorHandler::e_error, fmt, val);
    va_end(val);
}

void
ArgContext::warning(const char *fmt, ...) const
{
    va_list val;
    va_start(val, fmt);
    xmessage(ErrorHandler::e_warning_annotated, fmt, val);
    va_end(val);
}

void
ArgContext::message(const char *fmt, ...) const
{
    va_list val;
    va_start(val, fmt);
    xmessage(ErrorHandler::e_info, fmt, val);
    va_end(val);
}

void
ArgContext::xmessage(const String &anno, const String &str) const
{
    PrefixErrorHandler perrh(_errh, _arg_keyword ? String(_arg_keyword) + ": " : "");
    perrh.xmessage(anno, str);
    if (perrh.nerrors())
	_read_status = false;
}

void
ArgContext::xmessage(const String &anno, const char *fmt, va_list val) const
{
    PrefixErrorHandler perrh(_errh, _arg_keyword ? String(_arg_keyword) + ": " : "");
    perrh.xmessage(anno, fmt, val);
    if (perrh.nerrors())
	_read_status = false;
}


String
Args::find(const char *keyword, int flags, Slot *&slot_status)
{
    _arg_keyword = keyword;
    _read_status = true;
    slot_status = _slots;

    // Find last matching keyword.
    int keyword_length = keyword ? strlen(keyword) : 0;
    int got = -1, got_kwpos = -1, position = -1;
    for (int i = 0; i < _kwpos.size(); ++i) {
	if (position == -1 && _kwpos[i] != -1)
	    position = (_kwpos[i] >= 0 ? i : -2);
	if (_kwpos[i] == keyword_length
	    && memcmp((*_conf)[i].data(), keyword, keyword_length) == 0) {
	    got = i;
	    got_kwpos = _kwpos[i];
	    _kwpos[i] = -2;
	}
    }

    // Check positional arguments.
    // If the current argument lacks a keyword, assign it to this position.
    // But if the requested keyword is mandatory but so far lacking, take the
    // current argument even if it appears to have a keyword.
    if ((flags & positional) && position >= 0 && _kwpos[position] >= 0
	&& ((got < 0 && (flags & mandatory)) || _kwpos[position] == 0)) {
	if (got < position) {
	    got = position;
	    got_kwpos = 0;
	}
	_kwpos[position] = -1;
    }

    if (got < 0) {
	if (flags & mandatory) {
	    error("required argument missing");
	    _status = false;
	}
	_read_status = false;
	return String();
    }
    if (flags & deprecated)
	warning("argument deprecated");

    if (got_kwpos) {
	const char *s = (*_conf)[got].begin() + got_kwpos,
	    *ends = (*_conf)[got].end();
	while (s < ends && isspace((unsigned char) *s))
	    ++s;
	return (*_conf)[got].substring(s, ends);
    } else
	return (*_conf)[got];
}

void
Args::postparse(bool ok, Slot *slot_status)
{
    if (!ok && _read_status)
	error("syntax error");
    _arg_keyword = 0;

    if (_read_status) {
	while (_simple_slotpos < simple_slotbuf_size
	       && _simple_slotbuf[_simple_slotpos] != 0)
	    _simple_slotpos +=
		simple_slot_size(_simple_slotbuf[_simple_slotpos]);
    } else {
	_status = false;
	if (_simple_slotpos < simple_slotbuf_size)
	    _simple_slotbuf[_simple_slotpos] = 0;
	while (_slots != slot_status) {
	    Slot *slot = _slots;
	    _slots = _slots->_next;
	    delete slot;
	}
    }
}

Args &
Args::strip()
{
    int delta = 0;
    for (int i = 0; i < _kwpos.size(); ++i)
	if (_kwpos[i] < 0)
	    ++delta;
	else if (delta > 0) {
	    (*_conf)[i - delta] = (*_conf)[i];
	    _kwpos[i - delta] = _kwpos[i];
	}
    if (_conf)
	_conf->resize(_kwpos.size() - delta);
    _kwpos.resize(_kwpos.size() - delta);
    return *this;
}

void
Args::check_complete()
{
    bool too_many_positional = false;
    for (int i = 0; i < _kwpos.size(); ++i)
	if (_kwpos[i] == 0) {
	    too_many_positional = true;
	    _status = false;
	} else if (_kwpos[i] > 0) {
	    if (_errh)
		_errh->error("%.*s: unknown argument", _kwpos[i], (*_conf)[i].data());
	    _status = false;
	}
    if (too_many_positional && _errh)
	_errh->error("too many arguments");
}

int
Args::execute()
{
    if (!_status)
	return -EINVAL;
    while (Slot *s = _slots) {
	_slots = s->_next;
	s->store();
	delete s;
    }
    for (int offset = 0; offset < _simple_slotpos;
	 offset += simple_slot_size(_simple_slotbuf[offset])) {
	void *slot, **pointer;
	simple_slot_info(offset, _simple_slotbuf[offset], slot, pointer);
	memcpy(*pointer, slot, _simple_slotbuf[offset]);
    }
    _simple_slotpos = _simple_slotbuf[0] = 0;
    return 0;
}

int
Args::consume()
{
    if (_status)
	strip();
    return execute();
}

int
Args::complete()
{
    check_complete();
    return execute();
}


const char *
IntArg::parse(const char *begin, const char *end, bool is_signed,
	      value_type &value)
{
    const char *s = begin;
    bool negative = false;
    if (s < end && is_signed && *s == '-') {
	negative = true;
	++s;
    } else if (s < end && *s == '+')
	++s;

    int b = base;
    if ((b == 0 || b == 16) && s + 2 < end
	&& *s == '0' && (s[1] == 'x' || s[1] == 'X')) {
	s += 2;
	b = 16;
    } else if (b == 0 && s + 2 < end
	       && *s == '0' && (s[1] == 'b' || s[1] == 'B')) {
	s += 2;
	b = 2;
    } else if (b == 0 && s < end && *s == '0')
	b = 8;
    else if (b == 0)
	b = 10;
    else if (b < 2 || b > 36) {
	status = status_notsup;
	return begin;
    }

    const char *firstdigit = s, *lastdigit = s;
    value_type v = 0;
    status = status_ok;
    for (; s < end; ++s) {
	// find digit
	int digit;
	if (*s >= '0' && *s <= '9')
	    digit = *s - '0';
	else if (b > 10 && *s >= 'A' && *s <= 'Z')
	    digit = *s - 'A' + 10;
	else if (b > 10 && *s >= 'a' && *s <= 'z')
	    digit = *s - 'a' + 10;
	else if (*s == '_' && lastdigit + 1 == s)
	    // skip underscores between digits
	    continue;
	else
	    digit = 36;
	if (digit >= b)
	    break;
	lastdigit = s;
	// check for overflow without divide
	if (v <= threshold)
	    v = v * b + digit;
	else {
	    // Let v = (msv * threshold) + lsv, where lsv < threshold.
	    // Then new_value = (msv * threshold * b)
	    //   + (lsv * threshold * b) + digit.
	    value_type msv = (v >> threshold_shift) * b;
	    value_type lsv = (v & (threshold - 1)) * b + digit;
	    if (lsv >= threshold) {
		msv += lsv >> threshold_shift;
		lsv &= threshold - 1;
	    }
	    if (msv >= (1 << threshold_high_bits))
		status = status_range;
	    else
		v = (msv << threshold_shift) + lsv;
	}
    }

    if (s == firstdigit && firstdigit > begin + 1)
	// Happens in cases like "0x!" or "+0x": parse the initial "0".
	firstdigit = lastdigit = begin + (*begin == '0' ? 0 : 1);
    else if (s == firstdigit) {
	status = status_inval;
	return begin;
    }

    if (is_signed) {
	value_type boundary =
	    value_type(integer_traits<signed_value_type>::const_max) + negative;
	if (v > boundary)
	    status = status_range;
	if (status == status_range)
	    v = boundary;
	if (negative)
	    v = -v;
    } else if (status == status_range)
	v = integer_traits<value_type>::const_max;
    value = v;

    return lastdigit + 1;
}

void
IntArg::report_error(const ArgContext &args, bool is_signed, value_type value) const
{
    if (args.errh() && status == status_range) {
	const char *fmt;
	if (is_signed && signed_value_type(value) < 0) {
	    fmt = "out of range, bound -%s";
	    value = -value;
	} else
	    fmt = "out of range, bound %s";
	args.error(fmt, String(value).c_str());
    }
}

IntArg::value_type
IntArg::saturated(int signed_size, value_type value) const
{
    if (signed_size < 0)
	return (value_type(1) << (8 * -signed_size - 1)) - (signed_value_type(value) >= 0);
    else
	return ~value_type(0) >> (8 * (sizeof(value_type) - signed_size));
}


namespace {
typedef IntArg::value_type value_type;
typedef IntArg::signed_value_type signed_value_type;

const char *
preparse_fraction(const char *begin, const char *end, bool is_signed,
		  int &integer_digits)
{
    const char *s = begin;
    if (s != end && ((is_signed && *s == '-') || *s == '+'))
	++s;

    const char *firstdigit = s, *lastdigit = firstdigit - 1;
    bool decimalpoint = false;
    for (; s != end; ++s) {
	if (*s == '_' && lastdigit == s)
	    /* OK */;
	else if (*s == '.' && !decimalpoint
		 && (lastdigit == s || firstdigit == s))
	    decimalpoint = true;
	else if (*s >= '0' && *s <= '9') {
	    if (!decimalpoint)
		++integer_digits;
	    lastdigit = s + 1;
	} else
	    break;
    }

    // error if no digits at all
    if (lastdigit == firstdigit - 1)
	return begin;

    // optional exponent
    if (s != end && (*s == 'E' || *s == 'e') && s + 1 != end) {
	const char *echar = s;
	s += (s[1] == '-' || s[1] == '+' ? 2 : 1);
	if (s != end && isdigit((unsigned char) *s)) {
	    int exponent = *s - '0';
	    // XXX overflow
	    for (++s; s != end && isdigit((unsigned char) *s); ++s)
		exponent = 10 * exponent + *s - '0';
	    integer_digits += (echar[1] == '-' ? -exponent : exponent);
	} else
	    s = echar;
    }

    return s;
}

#if 0
const char *
parse_decimal_fraction(const char *begin, const char *end, bool is_signed,
		       value_type &ivalue,
		       int fraction_digits, value_type &fvalue, int &status)
{
    int integer_digits = 0;
    end = preparse_fraction(begin, end, is_signed, integer_digits);
    if (end == begin) {
	status = NumArg::status_inval;
	return begin;
    }

    status = NumArg::status_ok;
    const char *s = begin;

    constexpr value_type thresh = integer_traits<value_type>::const_max / 10;
    constexpr int thresh_digit = integer_traits<value_type>::const_max - thresh * 10;
    ivalue = fvalue = 0;
    while (integer_digits > 0 || fraction_digits > 0) {
	int digit;
	if (integer_digits < 0) {
	    digit = 0;
	    ++integer_digits;
	} else if (s == end || *s == 'E' || *s == 'e')
	    digit = 0;
	else if (*s >= '0' && *s <= '9') {
	    digit = *s - '0';
	    ++s;
	} else {
	    ++s;
	    continue;
	}
	if (integer_digits > 0) {
	    if (ivalue > thresh || (ivalue == thresh && digit > thresh_digit)) {
		ivalue = integer_traits<value_type>::const_max;
		status = NumArg::status_range;
	    } else
		ivalue = 10 * ivalue + digit;
	    --integer_digits;
	} else {
	    fvalue = 10 * fvalue + digit;
	    --fraction_digits;
	}
    }

    if (status == NumArg::status_range)
	fvalue = 0;

    return end;
}
#endif

const char *
parse_fraction(const char *begin, const char *end,
	       bool is_signed, int exponent_delta,
	       value_type &ivalue, uint32_t &fvalue, int &status)
{
    int integer_digits = exponent_delta;
    end = preparse_fraction(begin, end, is_signed, integer_digits);
    if (end == begin) {
	status = NumArg::status_inval;
	return begin;
    }

    status = NumArg::status_ok;
    const char *s = begin;

    constexpr value_type thresh = integer_traits<value_type>::const_max / 10;
    constexpr int thresh_digit = integer_traits<value_type>::const_max - thresh * 10;
    ivalue = 0;
    while (integer_digits > 0) {
	int digit;
	if (s == end || *s == 'E' || *s == 'e')
	    digit = 0;
	else if (*s >= '0' && *s <= '9') {
	    digit = *s - '0';
	    ++s;
	} else {
	    ++s;
	    continue;
	}
	if (ivalue > thresh || (ivalue == thresh && digit > thresh_digit)) {
	    ivalue = integer_traits<value_type>::const_max;
	    status = NumArg::status_range;
	} else
	    ivalue = 10 * ivalue + digit;
	--integer_digits;
    }

    const char *x = s;
    while (x != end && *x != 'E' && *x != 'e')
	++x;
    uint64_t fwork = 0;
    while (x != s) {
	--x;
	if (*x == '0') {
	    fwork = fwork ? int_divide(fwork, 10) : 0;
	    ++integer_digits;
	} else if (*x >= '1' && *x <= '9') {
	    fwork = int_divide(fwork + (uint64_t(*x - '0') << 33), 10);
	    ++integer_digits;
	}
    }
    while (integer_digits <= 0 && fwork) {
	fwork = int_divide(fwork, 10);
	++integer_digits;
    }

    if (fwork > 0x1FFFFFFFEULL) {
	if (ivalue == integer_traits<value_type>::const_max)
	    status = NumArg::status_range;
	else
	    ++ivalue;
	fwork = 0;
    }

    if (status == NumArg::status_range || fwork < 2)
	fvalue = 0;
    else
	fvalue = (fwork + 1) >> 1;

    return end;
}
}


bool
FixedPointArg::preparse(const String &str, bool is_signed, uint32_t &result)
{
    IntArg::value_type ivalue;
    uint32_t fvalue;
    const char *end = parse_fraction(str.begin(), str.end(),
				     is_signed, exponent_delta,
				     ivalue, fvalue, status);
    if (end != str.end())
	status = status_inval;
    if (status && status != status_range)
	return false;

    if (fvalue > (0xFFFFFFFFU - (1U << (31 - fraction_bits)))) {
	if (ivalue < integer_traits<IntArg::value_type>::const_max)
	    ++ivalue;
	else
	    status = status_range;
    }
    if (fraction_bits == 32 ? ivalue : ivalue >= (1U << (32 - fraction_bits)))
	status = status_range;
    if (status == status_range)
	result = 0xFFFFFFFFU;
    else if (fraction_bits == 32)
	result = fvalue;
    else
	result = (uint32_t(ivalue) << fraction_bits)
	    | ((fvalue + (1U << (31 - fraction_bits))) >> (32 - fraction_bits));
    return true;
}

bool
FixedPointArg::parse(const String &str, uint32_t &result, const ArgContext &args)
{
    uint32_t x;
    if (!preparse(str, false, x))
	return false;
    else if (status == status_range) {
	if (args.errh())
	    args.error("out of range, bound %s", cp_unparse_real2(x, fraction_bits).c_str());
	return false;
    } else {
	result = x;
	return true;
    }
}

bool
FixedPointArg::parse_saturating(const String &str, int32_t &result, const ArgContext &)
{
    uint32_t x;
    if (!preparse(str, true, x))
	return false;
    bool negative = str[0] == '-';
    if (status == status_ok
	&& x > uint32_t(integer_traits<int32_t>::const_max) + negative) {
	status = status_range;
	x = uint32_t(integer_traits<int32_t>::const_max) + negative;
    }
    result = negative ? -x : x;
    return true;
}

bool
FixedPointArg::parse(const String &str, int32_t &result, const ArgContext &args)
{
    int32_t x;
    if (!parse_saturating(str, x, args))
	return false;
    else if (status == status_range) {
	if (args.errh())
	    args.error("out of range, bound %s", cp_unparse_real2(int32_t(x), fraction_bits).c_str());
	return false;
    } else {
	result = x;
	return true;
    }
}


#if HAVE_FLOAT_TYPES
bool
DoubleArg::parse(const String &str, double &result, const ArgContext &args)
{
    if (str.length() == 0 || isspace((unsigned char) str[0])) {
    format_error:
	// check for space because strtod() accepts leading whitespace
	status = status_inval;
	return false;
    }

    errno = 0;
    char *endptr;
    double value = strtod(str.c_str(), &endptr);
    if (endptr != str.end())		// bad format; garbage after number
	goto format_error;

    if (errno == ERANGE) {
	status = status_range;
	if (args.errh()) {
	    const char *fmt;
	    if (value == 0)
		fmt = "underflow, rounded to %g";
	    else
		fmt = "out of range, bound %g";
	    args.error(fmt, value);
	}
	return false;
    }

    status = status_ok;
    result = value;
    return true;
}
#endif


bool
BoolArg::parse(const String &str, bool &result, const ArgContext &)
{
    const char *s = str.data();
    int len = str.length();

    if (len == 1 && (s[0] == '0' || s[0] == 'n' || s[0] == 'f'))
	result = false;
    else if (len == 1 && (s[0] == '1' || s[0] == 'y' || s[0] == 't'))
	result = true;
    else if (len == 5 && memcmp(s, "false", 5) == 0)
	result = false;
    else if (len == 4 && memcmp(s, "true", 4) == 0)
	result = true;
    else if (len == 2 && memcmp(s, "no", 2) == 0)
	result = false;
    else if (len == 3 && memcmp(s, "yes", 3) == 0)
	result = true;
    else
	return false;

    return true;
}


void
UnitArg::check_units()
{
    const unsigned char *u = units_;
    Vector<String> suffixes;
    while (*u) {
	assert(*u >= 1 && *u <= 7 && *u != 4);
	const unsigned char *next = u + 2 + (*u & 3);
	assert(*next);
	const unsigned char *post = next + 1;
	while (*post > 7)
	    ++post;
	String suffix(next, post);
	for (String *it = suffixes.begin(); it != suffixes.end(); ++it)
	    assert(suffix.length() < it->length()
		   || it->substring(-suffix.length()) != suffix);
	suffixes.push_back(suffix);
	u = post;
    }
}

const char *
UnitArg::parse(const char *begin, const char *end, int &power, int &factor) const
{
    const unsigned char *units = units_;

    while (*units) {
	const unsigned char *ubegin = units + 2 + (*units & 3);
	const unsigned char *uend = ubegin + 1;
	while (*uend > 7)
	    ++uend;

	if (uend - ubegin <= end - begin
	    && memcmp(ubegin, end - (uend - ubegin), uend - ubegin) == 0) {
	    factor = units[2];
	    if ((*units & 3) >= 2)
		factor = 256 * factor + units[3];
	    if ((*units & 3) >= 3)
		factor = 256 * factor + units[4];

	    power = units[1];
	    if (*units >= 4)
		power = -power;

	    end = end - (uend - ubegin);
	    if (prefix_chars_ && end > begin)
		for (const unsigned char *prefix_chars = prefix_chars_;
		     *prefix_chars; prefix_chars += 2)
		    if ((char) *prefix_chars == end[-1]) {
			power += prefix_chars[1] - 64;
			--end;
			break;
		    }

	    while (end > begin && isspace((unsigned char) end[-1]))
		--end;
	    return end;
	}

	units = uend;
    }

    power = 0;
    factor = 1;
    return end;
}


static const char byte_bandwidth_units[] = "\
\5\3\175baud\
\5\3\175bps\
\5\3\175b/s\
\1\0\1Bps\
\1\0\1B/s\
";
static const char byte_bandwidth_prefixes[] = "\
k\103K\103M\106G\111";

extern bool cp_real10(const String &str, int frac_digits, int exponent_delta, uint32_t *result);

bool
BandwidthArg::parse(const String &str, uint32_t &result, const ArgContext &args)
{
    int power, factor;
    const char *unit_end = UnitArg(byte_bandwidth_units, byte_bandwidth_prefixes).parse(str.begin(), str.end(), power, factor);

    value_type ix;
    uint32_t fx;
    const char *xend = parse_fraction(str.begin(), unit_end,
				      false, power, ix, fx, status);
    if (status == status_inval || xend != unit_end) {
	status = status_inval;
	return false;
    }
    if (uint32_t(ix) != ix)
	status = status_range;
    if (factor) {
	uint32_t addend;
#if HAVE_INT64_TYPES
	uint64_t fscale = uint64_t(fx) * factor;
	addend = (fscale + (1ULL << 31)) >> 32;
#else
	uint32_t ax[2] = { fx, 0 }, bx[2] = { 0x80000000U, 0 };
	Bigint<uint32_t>::multiply_add(bx, ax, 2, factor);
	addend = bx[1];
#endif
	const uint32_t threshold = 0xFFFFFFFFU / factor;
	if (ix > threshold
	    || (ix == threshold && addend > 0xFFFFFFFFU - threshold * factor))
	    status = status_range;
	else
	    ix = ix * factor + addend;
    }
    if (status == status_range) {
	args.error("out of range");
	result = 0xFFFFFFFFU;
	return false;
    } else {
	if (unit_end == str.end() && ix)
	    status = status_unitless;
	result = ix;
	return true;
    }
}


#if CLICK_USERLEVEL || CLICK_TOOL
bool
FilenameArg::parse(const String &str, String &result, const ArgContext &)
{
    String fn;
    if (!cp_string(str, &fn) || !fn)
	return false;

    // expand home directory substitutions
    if (fn[0] == '~') {
	if (fn.length() == 1 || fn[1] == '/') {
	    const char *home = getenv("HOME");
	    if (home)
		fn = String(home) + fn.substring(1);
	} else {
	    int off = 1;
	    while (off < fn.length() && fn[off] != '/')
		off++;
	    String username = fn.substring(1, off - 1);
	    struct passwd *pwd = getpwnam(username.c_str());
	    if (pwd && pwd->pw_dir)
		fn = String(pwd->pw_dir) + fn.substring(off);
	}
    }

    // replace double slashes with single slashes
    int len = fn.length();
    for (int i = 0; i < len - 1; i++)
	if (fn[i] == '/' && fn[i+1] == '/') {
	    fn = fn.substring(0, i) + fn.substring(i + 1);
	    i--;
	    len--;
	}

    // return
    result = fn;
    return true;
}
#endif


#if !CLICK_TOOL
bool
AnnoArg::parse(const String &str, int &result, const ArgContext &args)
{
    int32_t annoval;
    if (!NameInfo::query_int(NameInfo::T_ANNOTATION, args.context(),
			     str, &annoval))
	return false;
    if (size > 0) {
	if (ANNOTATIONINFO_SIZE(annoval) && ANNOTATIONINFO_SIZE(annoval) != size)
	    return false;
# if !HAVE_INDIFFERENT_ALIGNMENT
	if ((size == 2 || size == 4 || size == 8)
	    && (ANNOTATIONINFO_OFFSET(annoval) % size) != 0)
	    return false;
# endif
	if (ANNOTATIONINFO_OFFSET(annoval) + size > Packet::anno_size)
	    return false;
	annoval = ANNOTATIONINFO_OFFSET(annoval);
    } else if (ANNOTATIONINFO_OFFSET(annoval) >= Packet::anno_size)
	return false;
    result = annoval;
    return true;
}

bool
ElementArg::parse(const String &str, Element *&result, const ArgContext &args)
{
    const Element *context = args.context();
    result = context->router()->find(str, context);
    if (!result)
	args.error("does not name an element");
    return result;
}

bool
ElementCastArg::parse(const String &str, Element *&result, const ArgContext &args)
{
    if (ElementArg::parse(str, result, args)
	&& !(result = reinterpret_cast<Element *>(result->cast(type))))
	args.error("element type mismatch, expected %s", type);
    return result;
}
#endif

CLICK_ENDDECLS
