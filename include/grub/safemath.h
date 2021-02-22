/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2020  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  Arithmetic operations that protect against overflow.
 */

#ifndef GRUB_SAFEMATH_H
#define GRUB_SAFEMATH_H 1

#include <grub/compiler.h>

/* These appear in gcc 5.1 and clang 3.8. */
#if GNUC_PREREQ(5, 1) || CLANG_PREREQ(3, 8)

#define grub_add(a, b, res)	__builtin_add_overflow(a, b, res)
#define grub_sub(a, b, res)	__builtin_sub_overflow(a, b, res)
#define grub_mul(a, b, res)	__builtin_mul_overflow(a, b, res)

#else
/*
 * Copyright 2020 Rasmus Villemoes
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
/*
 * The code used in this header was taken from linux kernel commit
 * f0907827a8a9152aedac2833ed1b674a7b2a44f2
 * Rasmus Villemoes <linux@rasmusvillemoes.dk>, the original author of the
 * patch, was contacted directly, confirmed his authorship of the code, and
 * gave his permission on treating that dual license as MIT and including into
 * GRUB2 sources
 */

#include <grub/types.h>
#define is_signed_type(type)	(((type)(-1)) < (type)1)
#define __type_half_max(type)	((type)1 << (8*sizeof(type) - 1 - is_signed_type(type)))
#define type_max(T)		((T)((__type_half_max(T) - 1) + __type_half_max(T)))
#define type_min(T)		((T)((T)-type_max(T)-(T)1))

#define __unsigned_add_overflow(a, b, d) ({	\
	typeof(+(a)) __a = (a);			\
	typeof(+(b)) __b = (b);			\
	typeof(d) __d = (d);			\
	(void) (&__a == &__b);			\
	(void) (&__a == __d);			\
	*__d = __a + __b;			\
	*__d < __a;				\
})
#define __unsigned_sub_overflow(a, b, d) ({     \
	typeof(+(a)) __a = (a);			\
	typeof(+(b)) __b = (b);			\
	typeof(d) __d = (d);			\
	(void) (&__a == &__b);			\
	(void) (&__a == __d);			\
	*__d = __a - __b;			\
	__a < __b;				\
})
#define __unsigned_mul_overflow(a, b, d) ({		\
	typeof(+(a)) __a = (a);				\
	typeof(+(b)) __b = (b);				\
	typeof(d) __d = (d);				\
	(void) (&__a == &__b);				\
	(void) (&__a == __d);				\
	*__d = __a * __b;				\
	__builtin_constant_p(__b) ?			\
	  __b > 0 && __a > type_max(typeof(__a)) / __b :\
	  __a > 0 && __b > type_max(typeof(__b)) / __a; \
})

#define __signed_add_overflow(a, b, d) ({		\
	typeof(+(a)) __a = (a);				\
	typeof(+(b)) __b = (b);				\
	typeof(d) __d = (d);				\
	(void) (&__a == &__b);				\
	(void) (&__a == __d);				\
	*__d = (grub_uint64_t)__a + (grub_uint64_t)__b;	\
	(((~(__a ^ __b)) & (*__d ^ __a))		\
		& type_min(typeof(__a))) != 0;		\
})

#define __signed_sub_overflow(a, b, d) ({		\
	typeof(+(a)) __a = (a);				\
	typeof(+(b)) __b = (b);				\
	typeof(d) __d = (d);				\
	(void) (&__a == &__b);				\
	(void) (&__a == __d);				\
	*__d = (grub_uint64_t)__a - (grub_uint64_t)__b;	\
	((((__a ^ __b)) & (*__d ^ __a))			\
		& type_min(typeof(__a))) != 0;		\
})

#define __signed_mul_overflow(a, b, d) ({			\
	typeof(+(a)) __a = (a);					\
	typeof(+(b)) __b = (b);					\
	typeof(d) __d = (d);					\
	typeof(+(a)) __tmax = type_max(typeof(+(a)));		\
	typeof(+(a)) __tmin = type_min(typeof(+(a)));		\
	(void) (&__a == &__b);					\
	(void) (&__a == __d);					\
	*__d = (grub_uint64_t)__a * (grub_uint64_t)__b;		\
	(__b > 0   && (__a > __tmax/__b || __a < __tmin/__b)) ||\
	(__b < (typeof(__b))-1  &&				\
	 (__a > __tmin/__b || __a < __tmax/__b)) ||		\
	(__b == (typeof(__b))-1 && __a == __tmin);		\
})

#define grub_add(a, b, d)					\
	__builtin_choose_expr(is_signed_type(typeof(+(a))),	\
			__signed_add_overflow(a, b, d),		\
			__unsigned_add_overflow(a, b, d))

#define grub_sub(a, b, d)					\
	__builtin_choose_expr(is_signed_type(typeof(+(a))),	\
			__signed_sub_overflow(a, b, d),		\
			__unsigned_sub_overflow(a, b, d))

#define grub_mul(a, b, d)					\
	__builtin_choose_expr(is_signed_type(typeof(+(a))),	\
			__signed_mul_overflow(a, b, d),		\
			__unsigned_mul_overflow(a, b, d))

#endif

#endif /* GRUB_SAFEMATH_H */
