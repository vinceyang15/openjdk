/*
 * Copyright (c) 1997, 2017, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "classfile/systemDictionary.hpp"
#include "code/codeCache.hpp"
#include "code/icBuffer.hpp"
#include "code/nmethod.hpp"
#include "code/vtableStubs.hpp"
#include "compiler/compileBroker.hpp"
#include "compiler/disassembler.hpp"
#include "gc/shared/collectedHeap.hpp"
#include "interpreter/bytecodeHistogram.hpp"
#include "interpreter/interpreter.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "oops/oop.inline.hpp"
#include "prims/jvm.h"
#include "prims/privilegedStack.hpp"
#include "runtime/arguments.hpp"
#include "runtime/atomic.hpp"
#include "runtime/frame.hpp"
#include "runtime/java.hpp"
#include "runtime/os.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubCodeGenerator.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/vframe.hpp"
#include "runtime/vm_version.hpp"
#include "services/heapDumper.hpp"
#include "utilities/defaultStream.hpp"
#include "utilities/events.hpp"
#include "utilities/formatBuffer.hpp"
#include "utilities/macros.hpp"
#include "utilities/vmError.hpp"

#include <stdio.h>

#ifndef ASSERT
#  ifdef _DEBUG
   // NOTE: don't turn the lines below into a comment -- if you're getting
   // a compile error here, change the settings to define ASSERT
   ASSERT should be defined when _DEBUG is defined.  It is not intended to be used for debugging
   functions that do not slow down the system too much and thus can be left in optimized code.
   On the other hand, the code should not be included in a production version.
#  endif // _DEBUG
#endif // ASSERT


#ifdef _DEBUG
#  ifndef ASSERT
     configuration error: ASSERT must be defined in debug version
#  endif // ASSERT
#endif // _DEBUG


#ifdef PRODUCT
#  if -defined _DEBUG || -defined ASSERT
     configuration error: ASSERT et al. must not be defined in PRODUCT version
#  endif
#endif // PRODUCT

ATTRIBUTE_PRINTF(1, 2)
void warning(const char* format, ...) {
  if (PrintWarnings) {
    FILE* const err = defaultStream::error_stream();
    jio_fprintf(err, "%s warning: ", VM_Version::vm_name());
    va_list ap;
    va_start(ap, format);
    vfprintf(err, format, ap);
    va_end(ap);
    fputc('\n', err);
  }
  if (BreakAtWarning) BREAKPOINT;
}

#ifndef PRODUCT

#define is_token_break(ch) (isspace(ch) || (ch) == ',')

static const char* last_file_name = NULL;
static int         last_line_no   = -1;

// assert/guarantee/... may happen very early during VM initialization.
// Don't rely on anything that is initialized by Threads::create_vm(). For
// example, don't use tty.
bool error_is_suppressed(const char* file_name, int line_no) {
  // The following 1-element cache requires that passed-in
  // file names are always only constant literals.
  if (file_name == last_file_name && line_no == last_line_no)  return true;

  int file_name_len = (int)strlen(file_name);
  char separator = os::file_separator()[0];
  const char* base_name = strrchr(file_name, separator);
  if (base_name == NULL)
    base_name = file_name;

  // scan the SuppressErrorAt option
  const char* cp = SuppressErrorAt;
  for (;;) {
    const char* sfile;
    int sfile_len;
    int sline;
    bool noisy;
    while ((*cp) != '\0' && is_token_break(*cp))  cp++;
    if ((*cp) == '\0')  break;
    sfile = cp;
    while ((*cp) != '\0' && !is_token_break(*cp) && (*cp) != ':')  cp++;
    sfile_len = cp - sfile;
    if ((*cp) == ':')  cp++;
    sline = 0;
    while ((*cp) != '\0' && isdigit(*cp)) {
      sline *= 10;
      sline += (*cp) - '0';
      cp++;
    }
    // "file:line!" means the assert suppression is not silent
    noisy = ((*cp) == '!');
    while ((*cp) != '\0' && !is_token_break(*cp))  cp++;
    // match the line
    if (sline != 0) {
      if (sline != line_no)  continue;
    }
    // match the file
    if (sfile_len > 0) {
      const char* look = file_name;
      const char* look_max = file_name + file_name_len - sfile_len;
      const char* foundp;
      bool match = false;
      while (!match
             && (foundp = strchr(look, sfile[0])) != NULL
             && foundp <= look_max) {
        match = true;
        for (int i = 1; i < sfile_len; i++) {
          if (sfile[i] != foundp[i]) {
            match = false;
            break;
          }
        }
        look = foundp + 1;
      }
      if (!match)  continue;
    }
    // got a match!
    if (noisy) {
      fdStream out(defaultStream::output_fd());
      out.print_raw("[error suppressed at ");
      out.print_raw(base_name);
      char buf[16];
      jio_snprintf(buf, sizeof(buf), ":%d]", line_no);
      out.print_raw_cr(buf);
    } else {
      // update 1-element cache for fast silent matches
      last_file_name = file_name;
      last_line_no   = line_no;
    }
    return true;
  }

  if (!VMError::is_error_reported() && !SuppressFatalErrorMessage) {
    // print a friendly hint:
    fdStream out(defaultStream::output_fd());
    out.print_raw_cr("# To suppress the following error report, specify this argument");
    out.print_raw   ("# after -XX: or in .hotspotrc:  SuppressErrorAt=");
    out.print_raw   (base_name);
    char buf[16];
    jio_snprintf(buf, sizeof(buf), ":%d", line_no);
    out.print_raw_cr(buf);
  }
  return false;
}

#undef is_token_break

#else

// Place-holder for non-existent suppression check:
#define error_is_suppressed(file_name, line_no) (false)

#endif // !PRODUCT

void report_vm_error(const char* file, int line, const char* error_msg)
{
  report_vm_error(file, line, error_msg, "%s", "");
}

void report_vm_error(const char* file, int line, const char* error_msg, const char* detail_fmt, ...)
{
  if (Debugging || error_is_suppressed(file, line)) return;
  va_list detail_args;
  va_start(detail_args, detail_fmt);
  VMError::report_and_die(Thread::current_or_null(), file, line, error_msg, detail_fmt, detail_args);
  va_end(detail_args);
}

void report_vm_status_error(const char* file, int line, const char* error_msg,
                            int status, const char* detail) {
  report_vm_error(file, line, error_msg, "error %s(%d), %s", os::errno_name(status), status, detail);
}

void report_fatal(const char* file, int line, const char* detail_fmt, ...)
{
  if (Debugging || error_is_suppressed(file, line)) return;
  va_list detail_args;
  va_start(detail_args, detail_fmt);
  VMError::report_and_die(Thread::current_or_null(), file, line, "fatal error", detail_fmt, detail_args);
  va_end(detail_args);
}

void report_vm_out_of_memory(const char* file, int line, size_t size,
                             VMErrorType vm_err_type, const char* detail_fmt, ...) {
  if (Debugging) return;
  va_list detail_args;
  va_start(detail_args, detail_fmt);
  VMError::report_and_die(Thread::current_or_null(), file, line, size, vm_err_type, detail_fmt, detail_args);
  va_end(detail_args);

  // The UseOSErrorReporting option in report_and_die() may allow a return
  // to here. If so then we'll have to figure out how to handle it.
  guarantee(false, "report_and_die() should not return here");
}

void report_should_not_call(const char* file, int line) {
  report_vm_error(file, line, "ShouldNotCall()");
}

void report_should_not_reach_here(const char* file, int line) {
  report_vm_error(file, line, "ShouldNotReachHere()");
}

void report_unimplemented(const char* file, int line) {
  report_vm_error(file, line, "Unimplemented()");
}

#ifdef ASSERT
bool is_executing_unit_tests() {
  return ExecutingUnitTests;
}

void report_assert_msg(const char* msg, ...) {
  va_list ap;
  va_start(ap, msg);

  fprintf(stderr, "assert failed: %s\n", err_msg(FormatBufferDummy(), msg, ap).buffer());

  va_end(ap);
}
#endif // ASSERT

void report_untested(const char* file, int line, const char* message) {
#ifndef PRODUCT
  warning("Untested: %s in %s: %d\n", message, file, line);
#endif // !PRODUCT
}

void report_java_out_of_memory(const char* message) {
  static jint out_of_memory_reported = 0;

  // A number of threads may attempt to report OutOfMemoryError at around the
  // same time. To avoid dumping the heap or executing the data collection
  // commands multiple times we just do it once when the first threads reports
  // the error.
  if (Atomic::cmpxchg(1, &out_of_memory_reported, 0) == 0) {
    // create heap dump before OnOutOfMemoryError commands are executed
    if (HeapDumpOnOutOfMemoryError) {
      tty->print_cr("java.lang.OutOfMemoryError: %s", message);
      HeapDumper::dump_heap_from_oome();
    }

    if (OnOutOfMemoryError && OnOutOfMemoryError[0]) {
      VMError::report_java_out_of_memory(message);
    }

    if (CrashOnOutOfMemoryError) {
      tty->print_cr("Aborting due to java.lang.OutOfMemoryError: %s", message);
      fatal("OutOfMemory encountered: %s", message);
    }

    if (ExitOnOutOfMemoryError) {
      tty->print_cr("Terminating due to java.lang.OutOfMemoryError: %s", message);
      os::exit(3);
    }
  }
}

// ------ helper functions for debugging go here ------------

// All debug entries should be wrapped with a stack allocated
// Command object. It makes sure a resource mark is set and
// flushes the logfile to prevent file sharing problems.

class Command : public StackObj {
 private:
  ResourceMark rm;
  ResetNoHandleMark rnhm;
  HandleMark   hm;
  bool debug_save;
 public:
  static int level;
  Command(const char* str) {
    debug_save = Debugging;
    Debugging = true;
    if (level++ > 0)  return;
    tty->cr();
    tty->print_cr("\"Executing %s\"", str);
  }

  ~Command() {
        tty->flush();
        Debugging = debug_save;
        level--;
  }
};

int Command::level = 0;

#ifndef PRODUCT

extern "C" void blob(CodeBlob* cb) {
  Command c("blob");
  cb->print();
}


extern "C" void dump_vtable(address p) {
  Command c("dump_vtable");
  Klass* k = (Klass*)p;
  k->vtable().print();
}


extern "C" void nm(intptr_t p) {
  // Actually we look through all CodeBlobs (the nm name has been kept for backwards compatability)
  Command c("nm");
  CodeBlob* cb = CodeCache::find_blob((address)p);
  if (cb == NULL) {
    tty->print_cr("NULL");
  } else {
    cb->print();
  }
}


extern "C" void disnm(intptr_t p) {
  Command c("disnm");
  CodeBlob* cb = CodeCache::find_blob((address) p);
  if (cb != NULL) {
    nmethod* nm = cb->as_nmethod_or_null();
    if (nm != NULL) {
      nm->print();
    } else {
      cb->print();
    }
    Disassembler::decode(cb);
  }
}


extern "C" void printnm(intptr_t p) {
  char buffer[256];
  sprintf(buffer, "printnm: " INTPTR_FORMAT, p);
  Command c(buffer);
  CodeBlob* cb = CodeCache::find_blob((address) p);
  if (cb->is_nmethod()) {
    nmethod* nm = (nmethod*)cb;
    nm->print_nmethod(true);
  }
}


extern "C" void universe() {
  Command c("universe");
  Universe::print_on(tty);
}


extern "C" void verify() {
  // try to run a verify on the entire system
  // note: this may not be safe if we're not at a safepoint; for debugging,
  // this manipulates the safepoint settings to avoid assertion failures
  Command c("universe verify");
  bool safe = SafepointSynchronize::is_at_safepoint();
  if (!safe) {
    tty->print_cr("warning: not at safepoint -- verify may fail");
    SafepointSynchronize::set_is_at_safepoint();
  }
  // Ensure Eden top is correct before verification
  Universe::heap()->prepare_for_verify();
  Universe::verify();
  if (!safe) SafepointSynchronize::set_is_not_at_safepoint();
}


extern "C" void pp(void* p) {
  Command c("pp");
  FlagSetting fl(PrintVMMessages, true);
  FlagSetting f2(DisplayVMOutput, true);
  if (Universe::heap()->is_in(p)) {
    oop obj = oop(p);
    obj->print();
  } else {
    tty->print(PTR_FORMAT, p2i(p));
  }
}


// pv: print vm-printable object
extern "C" void pa(intptr_t p)   { ((AllocatedObj*) p)->print(); }
extern "C" void findpc(intptr_t x);

#endif // !PRODUCT

extern "C" void ps() { // print stack
  if (Thread::current_or_null() == NULL) return;
  Command c("ps");


  // Prints the stack of the current Java thread
  JavaThread* p = JavaThread::active();
  tty->print(" for thread: ");
  p->print();
  tty->cr();

  if (p->has_last_Java_frame()) {
    // If the last_Java_fp is set we are in C land and
    // can call the standard stack_trace function.
#ifdef PRODUCT
    p->print_stack();
  } else {
    tty->print_cr("Cannot find the last Java frame, printing stack disabled.");
#else // !PRODUCT
    p->trace_stack();
  } else {
    frame f = os::current_frame();
    RegisterMap reg_map(p);
    f = f.sender(&reg_map);
    tty->print("(guessing starting frame id=" PTR_FORMAT " based on current fp)\n", p2i(f.id()));
    p->trace_stack_from(vframe::new_vframe(&f, &reg_map, p));
    f.pd_ps();
#endif // PRODUCT
  }

}

extern "C" void pfl() {
  // print frame layout
  Command c("pfl");
  JavaThread* p = JavaThread::active();
  tty->print(" for thread: ");
  p->print();
  tty->cr();
  if (p->has_last_Java_frame()) {
    p->print_frame_layout();
  }
}

#ifndef PRODUCT

extern "C" void psf() { // print stack frames
  {
    Command c("psf");
    JavaThread* p = JavaThread::active();
    tty->print(" for thread: ");
    p->print();
    tty->cr();
    if (p->has_last_Java_frame()) {
      p->trace_frames();
    }
  }
}


extern "C" void threads() {
  Command c("threads");
  Threads::print(false, true);
}


extern "C" void psd() {
  Command c("psd");
  SystemDictionary::print();
}


extern "C" void safepoints() {
  Command c("safepoints");
  SafepointSynchronize::print_state();
}

#endif // !PRODUCT

extern "C" void pss() { // print all stacks
  if (Thread::current_or_null() == NULL) return;
  Command c("pss");
  Threads::print(true, PRODUCT_ONLY(false) NOT_PRODUCT(true));
}

#ifndef PRODUCT

extern "C" void debug() {               // to set things up for compiler debugging
  Command c("debug");
  WizardMode = true;
  PrintVMMessages = PrintCompilation = true;
  PrintInlining = PrintAssembly = true;
  tty->flush();
}


extern "C" void ndebug() {              // undo debug()
  Command c("ndebug");
  PrintCompilation = false;
  PrintInlining = PrintAssembly = false;
  tty->flush();
}


extern "C" void flush()  {
  Command c("flush");
  tty->flush();
}

extern "C" void events() {
  Command c("events");
  Events::print();
}

extern "C" Method* findm(intptr_t pc) {
  Command c("findm");
  nmethod* nm = CodeCache::find_nmethod((address)pc);
  return (nm == NULL) ? (Method*)NULL : nm->method();
}


extern "C" nmethod* findnm(intptr_t addr) {
  Command c("findnm");
  return  CodeCache::find_nmethod((address)addr);
}

// Another interface that isn't ambiguous in dbx.
// Can we someday rename the other find to hsfind?
extern "C" void hsfind(intptr_t x) {
  Command c("hsfind");
  os::print_location(tty, x, false);
}


extern "C" void find(intptr_t x) {
  Command c("find");
  os::print_location(tty, x, false);
}


extern "C" void findpc(intptr_t x) {
  Command c("findpc");
  os::print_location(tty, x, true);
}


// Need method pointer to find bcp, when not in permgen.
extern "C" void findbcp(intptr_t method, intptr_t bcp) {
  Command c("findbcp");
  Method* mh = (Method*)method;
  if (!mh->is_native()) {
    tty->print_cr("bci_from(%p) = %d; print_codes():",
                        mh, mh->bci_from(address(bcp)));
    mh->print_codes_on(tty);
  }
}

// int versions of all methods to avoid having to type type casts in the debugger

void pp(intptr_t p)          { pp((void*)p); }
void pp(oop p)               { pp((void*)p); }

void help() {
  Command c("help");
  tty->print_cr("basic");
  tty->print_cr("  pp(void* p)   - try to make sense of p");
  tty->print_cr("  pv(intptr_t p)- ((PrintableResourceObj*) p)->print()");
  tty->print_cr("  ps()          - print current thread stack");
  tty->print_cr("  pss()         - print all thread stacks");
  tty->print_cr("  pm(int pc)    - print Method* given compiled PC");
  tty->print_cr("  findm(intptr_t pc) - finds Method*");
  tty->print_cr("  find(intptr_t x)   - finds & prints nmethod/stub/bytecode/oop based on pointer into it");
  tty->print_cr("  pns(void* sp, void* fp, void* pc)  - print native (i.e. mixed) stack trace. E.g.");
  tty->print_cr("                   pns($sp, $rbp, $pc) on Linux/amd64 and Solaris/amd64 or");
  tty->print_cr("                   pns($sp, $ebp, $pc) on Linux/x86 or");
  tty->print_cr("                   pns($sp, 0, $pc)    on Linux/ppc64 or");
  tty->print_cr("                   pns($sp + 0x7ff, 0, $pc) on Solaris/SPARC");
  tty->print_cr("                 - in gdb do 'set overload-resolution off' before calling pns()");
  tty->print_cr("                 - in dbx do 'frame 1' before calling pns()");

  tty->print_cr("misc.");
  tty->print_cr("  flush()       - flushes the log file");
  tty->print_cr("  events()      - dump events from ring buffers");


  tty->print_cr("compiler debugging");
  tty->print_cr("  debug()       - to set things up for compiler debugging");
  tty->print_cr("  ndebug()      - undo debug");
}

extern "C" void pns(void* sp, void* fp, void* pc) { // print native stack
  Command c("pns");
  static char buf[O_BUFLEN];
  Thread* t = Thread::current_or_null();
  // Call generic frame constructor (certain arguments may be ignored)
  frame fr(sp, fp, pc);
  VMError::print_native_stack(tty, fr, t, buf, sizeof(buf));
}

#endif // !PRODUCT

//////////////////////////////////////////////////////////////////////////////
// Test multiple STATIC_ASSERT forms in various scopes.

#ifndef PRODUCT

// namespace scope
STATIC_ASSERT(true);
STATIC_ASSERT(true);
STATIC_ASSERT(1 == 1);
STATIC_ASSERT(0 == 0);

void test_multiple_static_assert_forms_in_function_scope() {
  STATIC_ASSERT(true);
  STATIC_ASSERT(true);
  STATIC_ASSERT(0 == 0);
  STATIC_ASSERT(1 == 1);
}

// class scope
struct TestMultipleStaticAssertFormsInClassScope {
  STATIC_ASSERT(true);
  STATIC_ASSERT(true);
  STATIC_ASSERT(0 == 0);
  STATIC_ASSERT(1 == 1);
};

#endif // !PRODUCT
