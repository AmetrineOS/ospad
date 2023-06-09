/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck_gen_headers/config_debug.h>
#include <tilck_gen_headers/mod_console.h>
#include <tilck_gen_headers/modules_list.h>

#include <tilck/common/basic_defs.h>
#include <tilck/common/printk.h>
#include <tilck/common/compiler.h>
#include <tilck/common/build_info.h>

#include <tilck/kernel/process.h>
#include <tilck/kernel/process_int.h>
#include <tilck/kernel/hal.h>
#include <tilck/kernel/term.h>

char zero_page[PAGE_SIZE] ALIGNED_AT(PAGE_SIZE);

#if KERNEL_SYMBOLS
char symtab_buf[SYMTAB_MAX_SIZE] ATTR_SECTION(".Symtab") = {0};
char strtab_buf[STRTAB_MAX_SIZE] ATTR_SECTION(".Strtab") = {0};
#else
char symtab_buf[1] ATTR_SECTION(".Symtab") = {0};
char strtab_buf[1] ATTR_SECTION(".Strtab") = {0};
#endif

bool __use_framebuffer;

#if DEBUG_CHECKS
const ulong init_st_begin = (ulong)&kernel_initial_stack;
const ulong init_st_end   = (ulong)&kernel_initial_stack + KERNEL_STACK_SIZE;
#endif

static void print_banner_line(const u8 *s)
{
   printk(NO_PREFIX "\033(0");

   for (const u8 *p = s; *p; p++) {
      printk(NO_PREFIX "%c", *p);
   }

   printk(NO_PREFIX "\033(B");
   printk(NO_PREFIX "\n");
}

static void show_tilck_logo(void)
{
   // Clear screen
   printk(NO_PREFIX "\033[H\033[J");
   char *banner[] =
   {
      "",
      " aaaaaak  aaaaaaak aaaaaak   aaaaak  aaaaaak",
      "aa    aak aalqqqqj aa   aak aa   aak aa   aak",
      "aa    aax aaaaaaak aaaaaalj aaaaaaax aa   aax",
      "aa    aax      aax aalqqqj  aalqqaax aa   aax",
      "maaaaaalj aaaaaaax aax      aax  aax aaaaaalj",
      " mqqqqqj  mqqqqqqj mqj      mqj  mqj mqqqqqj",
      ""
   };
   struct term_params tparams;
   process_term_read_info(&tparams);
   const u32 cols = tparams.cols;
   const u32 padding = (u32)(cols / 2 - strlen(banner[1]) / 2);

   for (int i = 0; i < ARRAY_SIZE(banner); i++) {

      for (u32 j = 0; j < padding; j++)
         printk(NO_PREFIX " ");

      print_banner_line((u8 *)banner[i]);
   }
}

void
show_hello_message(void)
{
   show_tilck_logo();
}

WEAK const char *get_signal_name(int signum) {
   return "";
}

const struct build_info tilck_build_info ATTR_SECTION(".tilck_info") = {
   .commit = {0}, /* It will get patched after the build */
   .ver = VER_MAJOR_STR "." VER_MINOR_STR "." VER_PATCH_STR,
   .arch = ARCH_GCC_TC,
   .modules_list = ENABLED_MODULES_LIST,
};
