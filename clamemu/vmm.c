/*
 *  ClamAV bytecode emulator VMM
 *
 *  Copyright (C) 2010 - 2011 Sourcefire, Inc.
 *
 *  Authors: Török Edvin
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

/* a very simple VMM:
 *  - last page cached
 *  - 16 pages of LRU-like shared cache for data and code
 *  - rely on OS's page cache for the rest, no need to duplicate that cache
 *  management here
 */

#include "cltypes.h"
#include "vmm.h"
#include "pe.h"
#include <errno.h>
#include <string.h>

#define MINALIGN 512
typedef struct {
    unsigned file_offset:23;/* divided by 4k */
    unsigned flag_rwx:3;
    unsigned modified:1;/* 0 - original input file, 1 - stored in temporary file (modified} */
    unsigned init:1;/* 1 - has real data, 0 - zeroinit */
    unsigned cached_page_idx:4;/* 0 - not cached; 1-15 cache idx */
} page_t;

typedef struct {
    uint8_t flag_rwx;
    uint8_t dirty;
    uint16_t reserved0;
    uint32_t reserved1;
    uint8_t data[4096];
} cached_page_t;

#define IMAGE_SCN_CNT_UNINITIALIZED_DATA 128
#define IMAGE_SCN_MEM_EXECUTE 0x20000000
#define IMAGE_SCN_MEM_READ 0x40000000
#define IMAGE_SCN_MEM_WRITE 0x80000000

struct emu_vmm {
    cached_page_t cached[15];
    unsigned cached_idx;/* idx where we need to read new page (oldest page in LRU) */
    unsigned lastused_page;
    unsigned lastused_page_idx;
    uint32_t imagebase;
    page_t *page_flags;
    unsigned n_pages;
    char *tempfile;
    int infd;
    int tmpfd;
    uint32_t tmpfd_written;/* in MINALIGN blocks */
};

static int map_pages(emu_vmm_t *v, struct cli_pe_hook_data *pedata, struct cli_exe_section *sections)
{
    unsigned i;
    switch (pedata->file_hdr.Machine) {
	default:
	    cli_dbgmsg("emu: unhandled architecture\n");
	    return -1;
	case 0x14c:
	case 0x14d:
	case 0x14e:
	    break;
    }
    if (pedata->opt32.SectionAlignment < 4096) {
	if (pedata->opt32.FileAlignment != pedata->opt32.SectionAlignment)
	    cli_dbgmsg("warning filealign and sectionalign mismatch, mapping probably incorrect: %d != %d\n",
		       pedata->opt32.FileAlignment, pedata->opt32.SectionAlignment);
    }
    if (pedata->opt32.FileAlignment < 512) {
	cli_dbgmsg("File alignment too small: %d, mapping will be probably incorrect\n", pedata->opt32.FileAlignment);
    }
    /* map file header, if not overlapping */
    for (i=0;i*4096 < sections[0].rva;i++) {
	v->page_flags[i].file_offset = i * 4096 / MINALIGN;
	v->page_flags[i].flag_rwx = 1 << flag_r;
    }
    for (i=0;i < pedata->nsections; i++) {
	const struct cli_exe_section *section = &sections[i];
	uint32_t rva = section->rva;
	uint32_t pages = (section->vsz + 4095) / 4096;
	uint32_t raw = section->raw;
	unsigned j;
	unsigned flag_rwx;
	unsigned zeroinit;

	if (i && sections[i].urva - sections[i-1].urva != sections[i-1].vsz) { 
	    cli_dbgmsg(" holes / overlapping / virtual disorder (broken executable)\n");
	    return -1;
	}

	zeroinit = section->chr & IMAGE_SCN_CNT_UNINITIALIZED_DATA;
	flag_rwx =
	    ((section->chr & IMAGE_SCN_MEM_EXECUTE) ? (1 << flag_x) : 0) |
	    ((section->chr & IMAGE_SCN_MEM_READ) ? (1 << flag_r): 0) |
	    ((section->chr & IMAGE_SCN_MEM_WRITE) ? (1 << flag_w): 0);
	for (j=0;j<pages;j++) {
	    uint32_t page = rva / 4096 + j;
	    if (page >= v->n_pages) {
		cli_dbgmsg("rva out of range: %x > %x\n", page*4096, v->n_pages*4096);
		return -1;
	    }
	    v->page_flags[page].init = !zeroinit;
	    /* 1 page can contain actually more than 1 section,
	     * but offset must be MINALIGN aligned, if not this will not work */
	    if (!zeroinit)
		v->page_flags[page].file_offset = (raw + j*4096)/MINALIGN;
	    v->page_flags[page].flag_rwx |= flag_rwx;
	}
	cli_dbgmsg("Mapped section RVA: %08x - %08x -> Raw: %08x%s - %08x, VA %08x - %08x\n",
		   rva, rva + pages * 4096, raw, raw%MINALIGN ? " (rounded!)" : "",
		   raw + pages*4096,
		   v->imagebase + rva, v->imagebase + rva + pages*4096);
    }
    return 0;
}

static never_inline void vmm_pageout(emu_vmm_t *v, cached_page_t *c, page_t *p)
{
    uint32_t n;
    /* page has been modified, need to write out to tempfile */
    p->init = 1;
    if (!p->modified) {
	p->modified = 1;
	if (v->tmpfd == -1) {
	    cli_gentempfd(NULL, &v->tempfile, &v->tmpfd);
	    if (v->tmpfd == -1)
		return;
	}
	p->file_offset = v->tmpfd_written;
	v->tmpfd_written += 4096 / MINALIGN;
    }
    n = p->file_offset * MINALIGN;
    if (pwrite(v->tmpfd, c->data, 4096, n) != n) {
	cli_dbgmsg("pwrite failed at %x: %s\n", n, strerror(errno));
	return;
    }
}

static always_inline cached_page_t *vmm_pagein(emu_vmm_t *v, page_t *p)
{
    unsigned nextidx = v->cached_idx + 1;
    cached_page_t *c = &v->cached[v->cached_idx];
    v->cached_idx = nextidx >= 15 ? 0 : nextidx;

    if (c->dirty)
	vmm_pageout(v, c, p);

    c->flag_rwx = p->flag_rwx;
    c->dirty = 0;
    p->cached_page_idx = v->cached_idx + 1;
    if (UNLIKELY(!p->init)) {
	memset(c->data, 0, 4096);
    } else if (pread(p->modified ? v->tmpfd : v->infd, c->data, 4096, p->file_offset * MINALIGN) == -1) {
	cli_warnmsg("pread failed at %x: %s\n", p->file_offset * MINALIGN, strerror(errno));
	return NULL;
    }
    return c;
}

static always_inline cached_page_t *vmm_cache_2page(emu_vmm_t *v, uint32_t va)
{
    page_t *p;
    uint32_t page = va / 4096;
    unsigned idx;

    if (v->lastused_page == page)
	return &v->cached[v->lastused_page_idx];
    if (UNLIKELY(page > v->n_pages))
	return NULL; /* out of bounds */
    p = &v->page_flags[page];
    idx = p->cached_page_idx;
    if (LIKELY(idx)) {
	idx--;
	v->lastused_page = page;
	v->lastused_page_idx = idx;
	return &v->cached[idx];
    }
    /* cache in 2nd page */
    if (page+1 < v->n_pages)
	vmm_pagein(v, &v->page_flags[page+1]);
    /* now cache in the page we wanted */
    return vmm_pagein(v, p);
}

static always_inline int vmm_read(emu_vmm_t *v, uint32_t va, void *value, uint32_t len, uint8_t flags)
{
    /* caches at least 2 pages, so when we read an int32 that crosess page
     * boundary, we can do it fast */
    cached_page_t *p = vmm_cache_2page(v, va);
    if (LIKELY(p && (p->flag_rwx & flags))) {
	uint8_t *data = p->data + (va & 0xfff);
	memcpy(value, data, len);
	return 0;
    }
    return -EMU_ERR_VMM_READ;
}

int cli_emu_vmm_read_r(emu_vmm_t *v, uint32_t va, uint8_t *value, uint32_t len)
{
   return vmm_read(v, va, value, len, 1 << flag_r);
}

int cli_emu_vmm_read_x(emu_vmm_t *v, uint32_t va, uint8_t *value, uint32_t len)
{
    return vmm_read(v, va, value, len, 1 << flag_x);
}

int cli_emu_vmm_read8(emu_vmm_t *v, uint32_t va, uint32_t *value)
{
    uint8_t a;
    int rc;
    rc = vmm_read(v, va, &a, 1, 1 << flag_r);
    *value = a;
    return rc;
}

int cli_emu_vmm_read16(emu_vmm_t *v, uint32_t va, uint32_t *value)
{
    uint16_t a;
    int rc;
    rc = vmm_read(v, va, &a, 2, 1 << flag_r);
    *value = le16_to_host(a);
    return rc;
}

int cli_emu_vmm_read32(emu_vmm_t *v, uint32_t va, uint32_t *value)
{
    uint32_t a;
    int rc;
    rc = vmm_read(v, va, &a, 4, 1 << flag_r);
    *value = le32_to_host(a);
    return rc;
}

int cli_emu_vmm_write(emu_vmm_t *v, uint32_t va, const void *value, uint32_t len)
{
    /* caches at least 2 pages, so when we read an int32 that crosess page
     * boundary, we can do it fast */
    cached_page_t *p = vmm_cache_2page(v, va);
    if (LIKELY(p && (p->flag_rwx & (1 << flag_w)))) {
	uint8_t *data = p->data + (va & 0xfff);
	memcpy(data, value, len);
	p->dirty = 1;
	return 0;
    }
    return -EMU_ERR_VMM_WRITE;
}

int cli_emu_vmm_write8(emu_vmm_t *v, uint32_t va, uint32_t value)
{
    uint8_t a = value;
    return cli_emu_vmm_write(v, va, &a, 1);
}

int cli_emu_vmm_write16(emu_vmm_t *v, uint32_t va, uint32_t value)
{
    uint16_t a = value;
    return cli_emu_vmm_write(v, va, &a, 2);
}

int cli_emu_vmm_write32(emu_vmm_t *v, uint32_t va, uint32_t value)
{
    uint32_t a = value;
    return cli_emu_vmm_write(v, va, &a, 4);
}

int cli_emu_vmm_prot_set(emu_vmm_t *v, uint32_t va, uint32_t len, uint8_t rwx)
{
    uint32_t page = va / 4096;
    if (page >= v->n_pages) {
	cli_dbgmsg("vmm_prot_set out of bounds: %x > %x\n", va, v->n_pages*4096);
	return -EMU_ERR_GENERIC;
    }
    /* this also acts as allocation function, by default all pages are zeroinit
     * anyway */
    v->page_flags[page].flag_rwx = rwx;
}

int cli_emu_vmm_prot_get(emu_vmm_t *v, uint32_t va, uint32_t len)
{
    uint32_t page = va / 4096;
    if (page >= v->n_pages) {
	cli_dbgmsg("vmm_prot_get out of bounds: %x > %x\n", va, v->n_pages*4096);
	return -EMU_ERR_GENERIC;
    }
    return v->page_flags[page].flag_rwx;
}

emu_vmm_t *cli_emu_vmm_new(struct cli_pe_hook_data *pedata, struct cli_exe_section *sections, int fd)
{
    emu_vmm_t *v;
    if (le16_to_host(pedata->opt64.Magic) == 0x020b) {
	cli_dbgmsg("PE32+ emulation not supported\n");
	return NULL;
    }
    if (!pedata->nsections) {
	cli_dbgmsg("no sections, nothing to emulate\n");
	return NULL;
    }

    v = cli_calloc(1, sizeof(*v));
    if (!v)
	return NULL;
    v->imagebase = pedata->opt32.ImageBase;
    v->infd = fd;
    v->tmpfd = -1;
    v->n_pages = (sections[pedata->nsections-1].rva + sections[pedata->nsections-1].vsz+4095) / 4096;
    v->page_flags = cli_calloc(v->n_pages, sizeof(*v->page_flags));
    if (!v->page_flags) {
	cli_emu_vmm_free(v);
	return NULL;
    }
    v->lastused_page = ~0u;

    if (map_pages(v, pedata, sections) == -1) {
	cli_emu_vmm_free(v);
	return NULL;
    }
    return v;
}

void cli_emu_vmm_free(emu_vmm_t *v)
{
    if (!v)
	return;
    if (v->tmpfd != -1) {
	ftruncate(v->tmpfd, 0);
	close(v->tmpfd);
	unlink(v->tempfile);
    }
    free(v->page_flags);
    free(v);
}
