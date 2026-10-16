#ifndef _IPXE_BASICOS_UMALLOC_H
#define _IPXE_BASICOS_UMALLOC_H

/** @file
 *
 * BASIC-OS user memory allocation (pool of RAM above 64 MB, inside the identity map)
 */

FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );

#ifdef UMALLOC_BASICOS
#define UMALLOC_PREFIX_basicos
#else
#define UMALLOC_PREFIX_basicos __basicos_
#endif

#endif /* _IPXE_BASICOS_UMALLOC_H */
