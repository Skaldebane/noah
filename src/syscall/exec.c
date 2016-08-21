#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "noah.h"
#include "x86/page.h"
#include "x86/vmemparam.h"
#include "elf.h"

extern uint64_t brk_min;

void init_userstack(int argc, char *argv[], char **envp, Elf64_Auxv *aux);

int
load_elf_interp(const char *path, ulong load_addr)
{
  char *data;
  Elf64_Ehdr *h;
  uint64_t map_top = 0;
  int fd;
  struct stat st;

  if ((fd = do_open(path, O_RDONLY, 0)) < 0) {
    fprintf(stderr, "could not open file: %s\n", path);
    return -1;
  }

  fstat(fd, &st);

  data = mmap(0, st.st_size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);

  h = (Elf64_Ehdr *)data;

  assert(IS_ELF(*h));

  if (! (h->e_type == ET_EXEC || h->e_type == ET_DYN))
    return -1;
  if (h->e_machine != EM_X86_64)
    return -1;

  Elf64_Phdr *p = (Elf64_Phdr *)(data + h->e_phoff);

  for (int i = 0; i < h->e_phnum; i++) {
    if (p[i].p_type != PT_LOAD) {
      continue;
    }

    ulong p_vaddr = p[i].p_vaddr + load_addr;

    ulong mask = PAGE_SIZE(PAGE_4KB) - 1;
    ulong vaddr = p_vaddr & ~mask;
    ulong offset = p_vaddr & mask;
    ulong size = roundup(p[i].p_memsz + offset, PAGE_SIZE(PAGE_4KB));

    int prot = 0;
    if (p[i].p_flags & PF_X) prot |= L_PROT_EXEC;
    if (p[i].p_flags & PF_W) prot |= L_PROT_WRITE;
    if (p[i].p_flags & PF_R) prot |= L_PROT_READ;

    do_mmap(vaddr, size, prot, L_MAP_PRIVATE | L_MAP_FIXED | L_MAP_ANONYMOUS, -1, 0);

    memcpy(guest_to_host(vaddr) + offset, data + p[i].p_offset, p[i].p_filesz);

    map_top = MAX(map_top, roundup(vaddr + size, PAGE_SIZE(PAGE_4KB)));
  }

  hv_vmx_vcpu_write_vmcs(vcpuid, VMCS_GUEST_RIP, load_addr + h->e_entry);
  brk_min = map_top;

  return 0;
}

void
load_elf(const Elf64_Ehdr *ehdr, int argc, char *argv[], char **envp)
{
  uint64_t map_top = 0;

  assert(IS_ELF(*ehdr));

  if (ehdr->e_type != ET_EXEC) {
    fprintf(stderr, "not an executable file");
    return;
  }
  if (ehdr->e_machine != EM_X86_64) {
    fprintf(stderr, "not an x64 executable");
    return;
  }

  Elf64_Phdr *p = (Elf64_Phdr *)((char *)ehdr + ehdr->e_phoff);

  uint64_t load_base = 0;
  bool load_base_set = false;

  for (int i = 0; i < ehdr->e_phnum; i++) {
    if (p[i].p_type != PT_LOAD) {
      continue;
    }

    ulong mask = PAGE_SIZE(PAGE_4KB) - 1;
    ulong vaddr = p[i].p_vaddr & ~mask;
    ulong offset = p[i].p_vaddr & mask;
    ulong size = roundup(p[i].p_memsz + offset, PAGE_SIZE(PAGE_4KB));

    int prot = 0;
    if (p[i].p_flags & PF_X) prot |= L_PROT_EXEC;
    if (p[i].p_flags & PF_W) prot |= L_PROT_WRITE;
    if (p[i].p_flags & PF_R) prot |= L_PROT_READ;

    do_mmap(vaddr, size, prot, L_MAP_PRIVATE | L_MAP_FIXED | L_MAP_ANONYMOUS, -1, 0);

    memcpy(guest_to_host(vaddr) + offset, (char *)ehdr + p[i].p_offset, p[i].p_filesz);

    if (! load_base_set) {
      load_base = p[i].p_vaddr - p[i].p_offset;
      load_base_set = true;
    }
    map_top = MAX(map_top, roundup(vaddr + size, PAGE_SIZE(PAGE_4KB)));
  }

  assert(load_base_set);

  int i;
  bool interp = false;
  for (i = 0; i < ehdr->e_phnum; i++) {
    if (p[i].p_type == PT_INTERP) {
      interp = true;
      break;
    }
  }
  if (interp) {
    char interp_path[p[i].p_filesz + 1];
    memcpy(interp_path, (char *)ehdr + p[i].p_offset, p[i].p_filesz);
    interp_path[p[i].p_filesz] = 0;

    load_elf_interp(interp_path, map_top);
  }
  else {
    hv_vmx_vcpu_write_vmcs(vcpuid, VMCS_GUEST_RIP, ehdr->e_entry);
    brk_min = map_top;
  }

  Elf64_Auxv aux[] = {
    { AT_PHDR, load_base + ehdr->e_phoff },
    { AT_PHENT, ehdr->e_phentsize },
    { AT_PHNUM, ehdr->e_phnum },
    { AT_PAGESZ, PAGE_SIZE(PAGE_4KB) },
    { AT_BASE, interp ? map_top : 0 },
    { AT_ENTRY, ehdr->e_entry },
    { AT_NULL, 0 },
  };

  init_userstack(argc, argv, envp, aux);
}

uint64_t
push(const void *data, size_t n)
{
  uint64_t size = roundup(n, 8);
  uint64_t rsp;

  hv_vcpu_read_register(vcpuid, HV_X86_RSP, &rsp);
  rsp -= size;
  hv_vcpu_write_register(vcpuid, HV_X86_RSP, rsp);

  char *stackmem = guest_to_host(rsp);

  if (data != 0) {
    memcpy(stackmem, data, n);
  } else {
    memset(stackmem, 0, n);
  }

  return rsp;
}

void
init_userstack(int argc, char *argv[], char **envp, Elf64_Auxv *aux)
{
  do_mmap(STACK_TOP - STACK_SIZE, STACK_SIZE, L_PROT_READ | L_PROT_WRITE, L_MAP_PRIVATE | L_MAP_FIXED | L_MAP_ANONYMOUS, -1, 0);

  hv_vcpu_write_register(vcpuid, HV_X86_RSP, STACK_TOP);
  hv_vcpu_write_register(vcpuid, HV_X86_RBP, STACK_TOP);

  char **renvp;
  for (renvp = envp; *renvp; ++renvp)
    ;

  uint64_t total = 0, args_total = 0;

  for (int i = 0; i < argc; ++i) {
    total += strlen(argv[i]) + 1;
  }
  args_total = total;
  for (char **e = envp; *e; ++e) {
    total += strlen(*e) + 1;
  }

  char buf[total];

  uint64_t off = 0;

  for (int i = 0; i < argc; ++i) {
    size_t len = strlen(argv[i]);
    memcpy(buf + off, argv[i], len + 1);
    off += len + 1;
  }
  for (char **e = envp; *e; ++e) {
    size_t len = strlen(*e);
    memcpy(buf + off, *e, len + 1);
    off += len + 1;
  }

  uint64_t args_start = push(buf, total);
  uint64_t args_end = args_start + args_total, env_end = args_start + total;

  push(0, sizeof(Elf64_Auxv));

  while (aux->a_tag != AT_NULL) {
    push(&aux->a_val, sizeof aux->a_val);
    push(&aux->a_tag, sizeof aux->a_tag);
    aux++;
  }

  push(0, sizeof(uint64_t));

  uint64_t ptr = env_end;
  for (char **e = renvp - 1; e >= envp; --e) {
    ptr -= strlen(*e) + 1;
    push(&ptr, sizeof ptr);
    assert(strcmp(buf + (ptr - args_start), *e) == 0);
  }

  push(0, sizeof(uint64_t));

  ptr = args_end;
  for (int i = argc - 1; i >= 0; --i) {
    ptr -= strlen(argv[i]) + 1;
    push(&ptr, sizeof ptr);
    assert(strcmp(buf + (ptr - args_start), argv[i]) == 0);
  }

  uint64_t argc64 = argc;
  push(&argc64, sizeof argc64);
}

void
do_exec(const char *elf_path, int argc, char *argv[], char **envp)
{
  int fd;
  struct stat st;
  Elf64_Ehdr *ehdr;

  if ((fd = open(elf_path, O_RDONLY)) < 0) {
    fprintf(stderr, "could not open file: %s\n", elf_path);
    return;
  }

  fstat(fd, &st);

  ehdr = mmap(0, st.st_size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);

  load_elf(ehdr, argc, argv, envp);
}
