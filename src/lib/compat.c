/*
 compat.c : Compatibility functions for OSes not having them

    Copyright (c) 2002 Timo Sirainen

    Permission is hereby granted, free of charge, to any person obtaining
    a copy of this software and associated documentation files (the
    "Software"), to deal in the Software without restriction, including
    without limitation the rights to use, copy, modify, merge, publish,
    distribute, sublicense, and/or sell copies of the Software, and to
    permit persons to whom the Software is furnished to do so, subject to
    the following conditions:

    The above copyright notice and this permission notice shall be
    included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
    OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
    IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
    CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
    TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
    SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "lib.h"

#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <syslog.h>

#ifndef INADDR_NONE
#  define INADDR_NONE INADDR_BROADCAST
#endif

#if !defined (HAVE_STRCASECMP) && !defined (HAVE_STRICMP)
int my_strcasecmp(const char *s1, const char *s2)
{
	while (*s1 != '\0' && i_toupper(*s1) == i_toupper(*s2)) {
		s1++; s2++;
	}

        return i_toupper(*s1) - i_toupper(*s2);
}

int my_strncasecmp(const char *s1, const char *s2, size_t max_chars)
{
	while (max_chars > 0 && *s1 != '\0' &&
	       i_toupper(*s1) == i_toupper(*s2)) {
		s1++; s2++;
	}

        return i_toupper(*s1) - i_toupper(*s2);
}
#endif

#ifndef HAVE_INET_ATON
int my_inet_aton(const char *cp, struct in_addr *inp)
{
	in_addr_t addr;

	addr = inet_addr(cp);
	if (addr == INADDR_NONE)
		return 0;

	inp->s_addr = addr;
        return 1;
}
#endif

#ifndef HAVE_VSYSLOG
void my_vsyslog(int priority, const char *format, va_list args)
{
	const char *str;
	char buf[1024];

#ifdef HAVE_VSNPRINTF
	int ret;

	ret = vsnprintf(buf, sizeof(buf), format, args);
	if (ret < 0 || (size_t)ret >= sizeof(buf))
		buf[sizeof(buf)-1] = '\0';
	str = buf;
#else
        va_list args2;

	VA_COPY(args2, args);

	if (printf_string_upper_bound(format, args) < sizeof(buf)) {
		vsprintf(buf, format, args);
		str = buf;
	} else {
		/* this may not be safe but not choice really.. */
		str = t_strdup_vprintf(format, args2);
	}
#endif
	syslog(priority, "%s", str);
}
#endif

#ifndef HAVE_GETPAGESIZE
int my_getpagesize(void)
{
#ifdef _SC_PAGESIZE
	return sysconf(_SC_PAGESIZE);
#else
#  warning Guessing page size to be 4096
	return 4096;
#endif
}
#endif

#ifndef HAVE_WRITEV
ssize_t my_writev(int fd, const struct iovec *iov, int iov_len)
{
	size_t written;
	ssize_t ret;
	int i;

	written = 0;
	for (i = 0; i < iov_len; i++, iov++) {
		ret = write(fd, iov->iov_base, iov->iov_len);
		if (ret < 0)
			return -1;

		written += ret;
		if ((size_t)ret != iov->iov_len)
			break;
	}

	if (written > SSIZE_T_MAX) {
		errno = ERANGE;
		return -1;
	}

	return (ssize_t)written;
}
#endif
