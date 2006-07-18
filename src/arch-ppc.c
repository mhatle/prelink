/* Copyright (C) 2001, 2002 Red Hat, Inc.
   Written by Jakub Jelinek <jakub@redhat.com>, 2001.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <locale.h>
#include <error.h>
#include <argp.h>
#include <stdlib.h>

#include "prelink.h"
#include "layout.h"

static int
ppc_adjust_dyn (DSO *dso, int n, GElf_Dyn *dyn, GElf_Addr start,
		 GElf_Addr adjust)
{
  if (dyn->d_tag == DT_PLTGOT)
    {
      int i;

      for (i = 1; i < dso->ehdr.e_shnum; ++i)
	if (! strcmp (strptr (dso, dso->ehdr.e_shstrndx,
			      dso->shdr[i].sh_name), ".got"))
	  {
	    Elf32_Addr data;

	    data = read_ube32 (dso, dso->shdr[i].sh_addr + 4);
	    /* .got[1] points to _DYNAMIC, it needs to be adjusted.  */
	    if (data == dso->shdr[n].sh_addr && data >= start)
	      write_be32 (dso, dso->shdr[i].sh_addr + 4, data + adjust);
	    break;
	  }
    }

  return 0;
}

static int
ppc_adjust_rel (DSO *dso, GElf_Rel *rel, GElf_Addr start,
		 GElf_Addr adjust)
{
  error (0, 0, "%s: PowerPC doesn't support REL relocs", dso->filename);
  return 1;
}

static int
ppc_adjust_rela (DSO *dso, GElf_Rela *rela, GElf_Addr start,
		  GElf_Addr adjust)
{
  if (GELF_R_TYPE (rela->r_info) == R_PPC_RELATIVE)
    {
      if ((Elf32_Word) rela->r_addend >= start)
	rela->r_addend += (Elf32_Sword) adjust;
    }
  return 0;
}

static int
ppc_prelink_rel (struct prelink_info *info, GElf_Rel *rel,
		   GElf_Addr reladdr)
{
  error (0, 0, "%s: PowerPC doesn't support REL relocs", info->dso->filename);
  return 1;
}

static void
ppc_fixup_plt (DSO *dso, GElf_Rela *rela, GElf_Addr value)
{
  Elf32_Sword disp = value - rela->r_offset;

  if (disp >= -0x2000000 && disp < 0x2000000)
    {
      /* b value  */
      write_be32 (dso, rela->r_offset, 0x48000000 | (disp & 0x3fffffc));
    }
  else if ((Elf32_Addr) value >= -0x2000000 || value < 0x2000000)
    {
      /* ba value  */
      write_be32 (dso, rela->r_offset, 0x48000002 | (value & 0x3fffffc));
    }
  else
    {
      Elf32_Addr plt = dso->info[DT_PLTGOT];

      if (rela->r_offset - plt < (8192 * 2 + 18) * 4)
	{
	  Elf32_Word index = (rela->r_offset - plt - 18 * 4) / (4 * 2);
	  Elf32_Word count = dso->info[DT_PLTRELSZ] / sizeof (Elf32_Rela);
	  Elf32_Addr data;

	  data = plt + (18 + 2 * count
			+ (count > 8192 ? (count - 8192) * 2 : 0)) * 4;
	  write_be32 (dso, data + 4 * index, value);
	  /* li %r11, 4*index
	     b .plt+0  */
	  write_be32 (dso, rela->r_offset,
		      0x39600000 | ((index * 4) & 0xffff));
	  write_be32 (dso, rela->r_offset + 4,
		      0x48000000 | (plt - rela->r_offset - 4));
	}
      else
	{
	  /* lis %r12, %hi(finaladdr)
	     addi %r12, %r12, %lo(finaladdr)
	     mtctr %r12
	     bctr  */
	  write_be32 (dso, rela->r_offset,
		      0x39800000 | (((value + 0x8000) >> 16) & 0xffff));
	  write_be32 (dso, rela->r_offset + 4, 0x398c0000 | (value & 0xffff));
	  write_be32 (dso, rela->r_offset + 8, 0x7d8903a6);
	  write_be32 (dso, rela->r_offset + 12, 0x4e800420);
	}
    } 
}

static int
ppc_prelink_rela (struct prelink_info *info, GElf_Rela *rela,
		    GElf_Addr relaaddr)
{
  DSO *dso = info->dso;
  GElf_Addr value;

  if (GELF_R_TYPE (rela->r_info) == R_PPC_NONE)
    return 0;
  else if (GELF_R_TYPE (rela->r_info) == R_PPC_RELATIVE)
    {
      write_be32 (dso, rela->r_offset, rela->r_addend);
      return 0;
    }
  value = info->resolve (info, GELF_R_SYM (rela->r_info),
			 GELF_R_TYPE (rela->r_info));
  value += rela->r_addend;
  switch (GELF_R_TYPE (rela->r_info))    
    {
    case R_PPC_GLOB_DAT:
    case R_PPC_ADDR32:
    case R_PPC_UADDR32:
      write_be32 (dso, rela->r_offset, value);
      break;
    case R_PPC_JMP_SLOT:
      ppc_fixup_plt (dso, rela, value);
      break;
    case R_PPC_ADDR16:
    case R_PPC_UADDR16:
    case R_PPC_ADDR16_LO:
      write_be16 (dso, rela->r_offset, value);
      break;
    case R_PPC_ADDR16_HI:
      write_be16 (dso, rela->r_offset, value >> 16);
      break;
    case R_PPC_ADDR16_HA:
      write_be16 (dso, rela->r_offset, (value + 0x8000) >> 16);
      break;
    case R_PPC_ADDR24:
      write_be32 (dso, rela->r_offset,
		  (value & 0x03fffffc)
		  | (read_ube32 (dso, rela->r_offset) & 0xfc000003));
      break;
    case R_PPC_ADDR14:
      write_be32 (dso, rela->r_offset,
		  (value & 0xfffc)
		  | (read_ube32 (dso, rela->r_offset) & 0xffff0003));
      break;
    case R_PPC_ADDR14_BRTAKEN:
    case R_PPC_ADDR14_BRNTAKEN:
      write_be32 (dso, rela->r_offset,
		  (value & 0xfffc)
		  | (read_ube32 (dso, rela->r_offset) & 0xffdf0003)
		  | (((GELF_R_TYPE (rela->r_info) == R_PPC_ADDR14_BRTAKEN)
		      ^ (value >> 10)) & 0x00200000));
      break;
    case R_PPC_REL24:
      write_be32 (dso, rela->r_offset,
		  ((value - rela->r_offset) & 0x03fffffc)
		  | (read_ube32 (dso, rela->r_offset) & 0xfc000003));
      break;
    case R_PPC_REL32:
      write_be32 (dso, rela->r_offset, value - rela->r_offset);
      break;
    case R_PPC_COPY:
      if (dso->ehdr.e_type == ET_EXEC)
	/* COPY relocs are handled specially in generic code.  */
	return 0;
      error (0, 0, "%s: R_PPC_COPY reloc in shared library?", dso->filename);
      return 1;
    default:
      error (0, 0, "%s: Unknown ppc relocation type %d", dso->filename,
	     (int) GELF_R_TYPE (rela->r_info));
      return 1;
    }
  return 0;
}

static int
ppc_apply_conflict_rela (struct prelink_info *info, GElf_Rela *rela,
			  char *buf)
{
  switch (GELF_R_TYPE (rela->r_info))    
    {
    case R_PPC_ADDR32:
      buf_write_be32 (buf, rela->r_addend);
      break;
    case R_PPC_ADDR16_LO:
      buf_write_be16 (buf, rela->r_addend);
      break;
    default:
      abort ();
    }
  return 0;
}

static int
ppc_apply_rel (struct prelink_info *info, GElf_Rel *rel, char *buf)
{
  error (0, 0, "%s: PowerPC doesn't support REL relocs", info->dso->filename);
  return 1;
}

static int
ppc_apply_rela (struct prelink_info *info, GElf_Rela *rela, char *buf)
{
  GElf_Addr value;

  value = info->resolve (info, GELF_R_SYM (rela->r_info),
			 GELF_R_TYPE (rela->r_info));
  value += rela->r_addend;
  switch (GELF_R_TYPE (rela->r_info))    
    {
    case R_PPC_NONE:
      break;
    case R_PPC_GLOB_DAT:
    case R_PPC_ADDR32:
    case R_PPC_UADDR32:
      buf_write_be32 (buf, value);
      break;
    case R_PPC_ADDR16_HA:
      value += 0x8000;
      /* FALLTHROUGH  */
    case R_PPC_ADDR16_HI:
      value = value >> 16;
      /* FALLTHROUGH  */
    case R_PPC_ADDR16:
    case R_PPC_UADDR16:
    case R_PPC_ADDR16_LO:
      buf_write_be16 (buf, value);
      break;
    case R_PPC_ADDR24:
      buf_write_be32 (buf, (value & 0x03fffffc)
			   | (buf_read_ube32 (buf) & 0xfc000003));
      break;
    case R_PPC_ADDR14:
      buf_write_be32 (buf, (value & 0xfffc)
			   | (buf_read_ube32 (buf) & 0xffff0003));
      break;
    case R_PPC_ADDR14_BRTAKEN:
    case R_PPC_ADDR14_BRNTAKEN:
      buf_write_be32 (buf, (value & 0xfffc)
			   | (buf_read_ube32 (buf) & 0xffdf0003)
			   | (((GELF_R_TYPE (rela->r_info)
				== R_PPC_ADDR14_BRTAKEN)
			       ^ (value >> 10)) & 0x00200000));
      break;
    case R_PPC_REL24:
      buf_write_be32 (buf, ((value - rela->r_offset) & 0x03fffffc)
			   | (buf_read_ube32 (buf) & 0xfc000003));
      break;
    case R_PPC_REL32:
      buf_write_be32 (buf, value - rela->r_offset);
      break;
    case R_PPC_RELATIVE:
      error (0, 0, "%s: R_PPC_RELATIVE in ET_EXEC object?",
	     info->dso->filename);
      return 1;
    default:
      return 1;
    }
  return 0;
}

static int
ppc_prelink_conflict_rel (DSO *dso, struct prelink_info *info,
			    GElf_Rel *rel, GElf_Addr reladdr)
{
  error (0, 0, "%s: PowerPC doesn't support REL relocs", dso->filename);
  return 1;
}

static int
ppc_prelink_conflict_rela (DSO *dso, struct prelink_info *info,
			     GElf_Rela *rela, GElf_Addr relaaddr)
{
  GElf_Addr value;
  struct prelink_conflict *conflict;
  GElf_Rela *ret;
  int r_type;

  if (GELF_R_TYPE (rela->r_info) == R_PPC_RELATIVE
      || GELF_R_TYPE (rela->r_info) == R_PPC_NONE)
    /* Fast path: nothing to do.  */
    return 0;
  conflict = prelink_conflict (info, GELF_R_SYM (rela->r_info),
			       GELF_R_TYPE (rela->r_info));
  if (conflict == NULL)
    return 0;
  value = conflict_lookup_value (conflict);
  ret = prelink_conflict_add_rela (info);
  if (ret == NULL)
    return 1;
  ret->r_offset = rela->r_offset;
  value += rela->r_addend;
  r_type = GELF_R_TYPE (rela->r_info);
  switch (r_type)    
    {
    case R_PPC_GLOB_DAT:
    case R_PPC_ADDR32:
    case R_PPC_UADDR32:
      r_type = R_PPC_ADDR32;
      break;
    case R_PPC_JMP_SLOT:
      break;
    case R_PPC_ADDR16_HA:
      value += 0x8000;
      /* FALLTHROUGH  */
    case R_PPC_ADDR16_HI:
      value = value >> 16;
      /* FALLTHROUGH  */
    case R_PPC_ADDR16:
    case R_PPC_UADDR16:
    case R_PPC_ADDR16_LO:
      r_type = R_PPC_ADDR16_LO;
      value &= 0xffff;
      break;
    case R_PPC_ADDR24:
      r_type = R_PPC_ADDR32;
      value = (value & 0x03fffffc)
	      | (read_ube32 (dso, rela->r_offset) & 0xfc000003);
      break;
    case R_PPC_ADDR14:
      r_type = R_PPC_ADDR32;
      value = (value & 0xfffc)
	      | (read_ube32 (dso, rela->r_offset) & 0xffff0003);
      break;
    case R_PPC_ADDR14_BRTAKEN:
    case R_PPC_ADDR14_BRNTAKEN:
      r_type = R_PPC_ADDR32;
      value = (value & 0xfffc)
	      | (read_ube32 (dso, rela->r_offset) & 0xffdf0003)
	      | (((r_type == R_PPC_ADDR14_BRTAKEN)
		  ^ (value >> 10)) & 0x00200000);
      break;
    case R_PPC_REL24:
      r_type = R_PPC_ADDR32;
      value = ((value - rela->r_offset) & 0x03fffffc)
	      | (read_ube32 (dso, rela->r_offset) & 0xfc000003);
      break;
    case R_PPC_REL32:
      r_type = R_PPC_ADDR32;
      value -= rela->r_offset;
      break;
    default:
      error (0, 0, "%s: Unknown PowerPC relocation type %d", dso->filename,
	     r_type);
      return 1;
    }
  ret->r_info = GELF_R_INFO (0, r_type);
  ret->r_addend = (Elf32_Sword) value;
  return 0;
}

static int
ppc_rel_to_rela (DSO *dso, GElf_Rel *rel, GElf_Rela *rela)
{
  error (0, 0, "%s: PowerPC doesn't support REL relocs", dso->filename);
  return 1;
}

static int
ppc_need_rel_to_rela (DSO *dso, int first, int last)
{
  return 0;
}

static int
ppc_arch_prelink (DSO *dso)
{
  Elf32_Addr plt = dso->info[DT_PLTGOT];

  if (plt)
    {
      Elf32_Word count = dso->info[DT_PLTRELSZ] / sizeof (Elf32_Rela);
      Elf32_Addr data;

      data = plt + (18 + 2 * count
		    + (count > 8192 ? (count - 8192) * 2 : 0)) * 4;

      /* addis %r11, %r11, %hi(data)
	 lwz %r11, %r11, %lo(data)
	 mtctr %r11
	 bctr  */
      write_be32 (dso, plt,  0x3d6b0000 | (((data + 0x8000) >> 16) & 0xffff));
      write_be32 (dso, plt + 4, 0x816b0000 | (data & 0xffff));
      write_be32 (dso, plt + 8, 0x7d6903a6);
      write_be32 (dso, plt + 12, 0x4e800420);
    }
  return 0;
}

static int
ppc_undo_prelink_rela (DSO *dso, GElf_Rela *rela, GElf_Addr relaaddr)
{
  switch (GELF_R_TYPE (rela->r_info))
    {
    case R_PPC_NONE:
      return 0;
    case R_PPC_RELATIVE:
    case R_PPC_GLOB_DAT:
    case R_PPC_ADDR32:
    case R_PPC_UADDR32:
    case R_PPC_REL32:
      write_be32 (dso, rela->r_offset, 0);
      break;
    case R_PPC_JMP_SLOT:
      /* .plt section will become SHT_NOBITS.  */
      return 0;
    case R_PPC_ADDR16:
    case R_PPC_UADDR16:
    case R_PPC_ADDR16_LO:
    case R_PPC_ADDR16_HI:
    case R_PPC_ADDR16_HA:
      write_be16 (dso, rela->r_offset, 0);
      break;
    case R_PPC_ADDR24:
    case R_PPC_REL24:
      write_be32 (dso, rela->r_offset,
		  read_ube32 (dso, rela->r_offset) & 0xfc000003);
      break;
    case R_PPC_ADDR14:
      write_be32 (dso, rela->r_offset,
		  read_ube32 (dso, rela->r_offset) & 0xffff0003);
      break;
    case R_PPC_ADDR14_BRTAKEN:
    case R_PPC_ADDR14_BRNTAKEN:
      write_be32 (dso, rela->r_offset,
		  read_ube32 (dso, rela->r_offset) & 0xffdf0003);
      break;
    case R_PPC_COPY:
      if (dso->ehdr.e_type == ET_EXEC)
	/* COPY relocs are handled specially in generic code.  */
	return 0;
      error (0, 0, "%s: R_PPC_COPY reloc in shared library?", dso->filename);
      return 1;
    default:
      error (0, 0, "%s: Unknown ppc relocation type %d", dso->filename,
	     (int) GELF_R_TYPE (rela->r_info));
      return 1;
    }
  return 0;
}
static int
ppc_reloc_size (int reloc_type)
{
  switch (reloc_type)
    {
    case R_PPC_ADDR16:
    case R_PPC_UADDR16:
    case R_PPC_ADDR16_LO:
    case R_PPC_ADDR16_HA:
    case R_PPC_ADDR16_HI:
      return 2;
    default:
      break;
    }
  return 4;
}

static int
ppc_reloc_class (int reloc_type)
{
  switch (reloc_type)
    {
    case R_PPC_COPY: return RTYPE_CLASS_COPY;
    case R_PPC_JMP_SLOT: return RTYPE_CLASS_PLT;
    default: return RTYPE_CLASS_VALID;
    }
}

/* Library memory regions in order of precedence:
   0xe800000 .. 0x10000000 top to bottom
   0x40000 .. 0xe800000 bottom to top
   0x18000000 .. 0x30000000 bottom to top  */

#define REG0S	0x0e800000
#define REG0E	0x10000000
#define REG1S	0x00040000
#define REG1E	REG0S
#define REG2S	0x18000000
#define REG2E	0x30000000

struct ppc_layout_data
{
  int cnt;
  struct prelink_entry e[3];
  Elf32_Addr mmap_start, first_start, last_start;
  struct
    {
      struct prelink_entry *e;
      Elf32_Addr base, end;
    } ents[0];
};

static inline void
list_append (struct prelink_entry *x, struct prelink_entry *e)
{
  x->prev->next = e;
  e->prev = x->prev;
  e->next = NULL;
  x->prev = e;
}

static int
addr_cmp (const void *A, const void *B)
{
  struct prelink_entry *a = * (struct prelink_entry **) A;
  struct prelink_entry *b = * (struct prelink_entry **) B;

  if (a->base < b->base)
    return -1;
  else if (a->base > b->base)
    return 1;
  if (a->end < b->end)
    return -1;
  else if (a->end > b->end)
    return 1;
  return 0;
}

static void
list_sort (struct prelink_entry *x)
{
  int cnt, i;
  struct prelink_entry *e;
  struct prelink_entry **a;

  if (x->next == NULL)
    return;
  for (cnt = 0, e = x->next; e != NULL; e = e->next)
    ++cnt;
  a = alloca (cnt * sizeof (*a));
  for (i = 0, e = x->next; e != NULL; e = e->next)
    a[i++] = e;
  qsort (a, cnt, sizeof (*a), addr_cmp);
  x->next = NULL;
  x->prev = x;
  for (i = 0; i < cnt; ++i)
    list_append (x, a[i]);
}

static int
ppc_layout_libs_pre (struct layout_libs *l)
{
  Elf32_Addr mmap_start = l->mmap_start - REG1S;
  Elf32_Addr first_start = REG0S, last_start = REG2S;
  struct prelink_entry *e, e0, *next = NULL;
  struct ppc_layout_data *pld;
  int cnt;

  mmap_start = REG0E - (mmap_start & 0xff0000);
  for (cnt = 0, e = l->list; e != NULL; e = e->next, ++cnt)
    {
      if (e->base < mmap_start && e->end > mmap_start)
	mmap_start = (e->end + 0xffff) & ~0xffff;
      if (e->base < REG0S && e->end > REG0S && first_start > e->base)
	first_start = e->base;
      if (e->base < REG0E && e->end > REG2S && last_start < e->end)
	last_start = e->end;
    }
  if (mmap_start > REG0E)
    mmap_start = REG0E;

  pld = calloc (sizeof (*pld) + cnt * sizeof (pld->ents[0]), 1);
  if (pld == NULL)
    error (EXIT_FAILURE, ENOMEM, "Cannot lay libraries out");

  l->arch_data = pld;
  memset (&e0, 0, sizeof (e0));
  e0.prev = &e0;
  pld->cnt = cnt;
  pld->e[0].u.tmp = -1;
  pld->e[0].base = REG1S + REG0E - mmap_start;
  pld->e[0].end = pld->e[0].base;
  pld->e[0].prev = &pld->e[0];
  pld->e[1].u.tmp = -1;
  pld->e[1].base = pld->e[0].end + mmap_start - REG0S;
  pld->e[1].end = pld->e[1].base;
  pld->e[1].prev = &pld->e[1];
  pld->e[2].u.tmp = -1;
  pld->e[2].base = pld->e[1].end + first_start - REG1S;
  pld->e[2].end = pld->e[1].base;
  pld->e[2].prev = &pld->e[2];
  for (cnt = 0, e = l->list; e != NULL; e = next, ++cnt)
    {
      next = e->next;
      pld->ents[cnt].e = e;
      pld->ents[cnt].base = e->base;
      pld->ents[cnt].end = e->end;
      if (e->end <= REG0S)
	{
	  if (e->base < REG1S)
	    e->base = REG1S;
	  else if (e->base > first_start)
	    e->base = first_start;
	  if (e->end < REG1S)
	    e->end = REG1S;
	  else if (e->end > first_start)
	    e->end = first_start;
	  e->base += pld->e[1].end - REG1S;
	  e->end += pld->e[1].end - REG1S;
	  list_append (&pld->e[1], e);
	}
      else if (e->base < mmap_start)
	{
	  if (e->base < REG0S)
	    e->base = REG0S;
	  if (e->end > mmap_start)
	    e->end = mmap_start;
	  e->base = pld->e[0].end + mmap_start - e->end;
	  e->end = pld->e[0].end + mmap_start - pld->ents[cnt].base;
	  list_append (&pld->e[0], e);
	}
      else if (e->base < REG0E)
	{
	  if (e->end > REG0E)
	    e->end = REG0E;
	  e->base = REG1S + REG0E - e->end;
	  e->end = REG1S + REG0E - pld->ents[cnt].base;
	  list_append (&e0, e);
	}
      else if (e->end >= last_start)
	{
	  if (e->base < last_start)
	    e->base = last_start;
	  e->base += pld->e[2].end - last_start;
	  e->end += pld->e[2].end - last_start;
	  list_append (&pld->e[2], e);
	}
    }

  list_sort (&pld->e[0]);
  if (e0.next == NULL)
    l->list = &pld->e[0];
  else
    {
      list_sort (&e0);
      l->list = e0.next;
      l->list->prev = pld->e[0].prev;
      e0.prev->next = &pld->e[0];
      pld->e[0].prev = e0.prev;
    }

  e0.prev = l->list->prev;
  l->list->prev = pld->e[1].prev;
  e0.prev->next = &pld->e[1];
  pld->e[1].prev = e0.prev;

  e0.prev = l->list->prev;
  l->list->prev = pld->e[2].prev;
  e0.prev->next = &pld->e[2];
  pld->e[2].prev = e0.prev;

  pld->mmap_start = mmap_start;
  pld->first_start = first_start;
  pld->last_start = last_start;

  l->mmap_start = REG1S;
  l->mmap_fin = pld->e[2].end + REG2E - last_start;
  l->mmap_end = l->mmap_fin;
  l->fakecnt = 3;
  l->fake = pld->e;

  return 0;
}

static int
ppc_layout_libs_post (struct layout_libs *l)
{
  struct prelink_entry *e;
  struct ppc_layout_data *pld = (struct ppc_layout_data *) l->arch_data;
  Elf32_Addr base, end;
  int i;

  /* First fix up base and end fields we saved.  */
  for (i = 0; i < pld->cnt; ++i)
    {
      pld->ents[i].e->base = pld->ents[i].base;
      pld->ents[i].e->end = pld->ents[i].end;
      pld->ents[i].e->done |= 0x40;
    }
  pld->e[0].done |= 0x40;
  pld->e[1].done |= 0x40;
  pld->e[2].done |= 0x40;

  /* Now fix up the newly created items.  */
  for (e = l->list; e != NULL; e = e->next)
    if (e->done & 0x40)
      e->done &= ~0x40;
    else
      {
	base = e->base;
	end = e->end;
	if (e->base < pld->e[0].base)
	  {
	    e->base = REG1S + REG0E - end;
	    e->end = REG1S + REG0E - base;
	  }
	else if (e->base < pld->e[1].base)
	  {
	    e->base = pld->e[0].end + pld->mmap_start - end;
	    e->end = pld->e[0].end + pld->mmap_start - base;
	  }
	else if (e->base < pld->e[2].base)
	  {
	    e->base -= pld->e[1].end - REG1S;
	    e->end -= pld->e[1].end - REG1S;
	  }
	else
	  {
	    e->base -= pld->e[2].end - pld->last_start;
	    e->end -= pld->e[2].end - pld->last_start;
	  }
      }

  for (i = 0; i < pld->cnt; ++i)
    pld->ents[i].e->done &= ~0x40;

  free (l->arch_data);
  return 0;
}

PL_ARCH = {
  .class = ELFCLASS32,
  .machine = EM_PPC,
  .alternate_machine = { EM_NONE },
  .R_JMP_SLOT = R_PPC_JMP_SLOT,
  .R_COPY = R_PPC_COPY,
  .R_RELATIVE = R_PPC_RELATIVE,
  .dynamic_linker = "/lib/ld.so.1",
  .adjust_dyn = ppc_adjust_dyn,
  .adjust_rel = ppc_adjust_rel,
  .adjust_rela = ppc_adjust_rela,
  .prelink_rel = ppc_prelink_rel,
  .prelink_rela = ppc_prelink_rela,
  .prelink_conflict_rel = ppc_prelink_conflict_rel,
  .prelink_conflict_rela = ppc_prelink_conflict_rela,
  .apply_conflict_rela = ppc_apply_conflict_rela,
  .apply_rel = ppc_apply_rel,
  .apply_rela = ppc_apply_rela,
  .rel_to_rela = ppc_rel_to_rela,
  .need_rel_to_rela = ppc_need_rel_to_rela,
  .reloc_size = ppc_reloc_size,
  .reloc_class = ppc_reloc_class,
  .max_reloc_size = 4,
  .arch_prelink = ppc_arch_prelink,
  .undo_prelink_rela = ppc_undo_prelink_rela,
  .layout_libs_pre = ppc_layout_libs_pre,
  .layout_libs_post = ppc_layout_libs_post,
  /* This will need some changes in layout.c.
     PowerPC prefers addresses right below REG0E
     and can use the region above REG2S if libs don't fit.  */
  .mmap_base = REG1S,
  .mmap_end =  REG2E,
  .max_page_size = 0x10000,
  .page_size = 0x1000
};
