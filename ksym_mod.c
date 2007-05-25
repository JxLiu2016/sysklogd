/*
    ksym_mod.c - functions for building symbol lookup tables for klogd
    Copyright (c) 1995, 1996  Dr. G.W. Wettstein <greg@wind.rmcc.com>
    Copyright (c) 1996 Enjellic Systems Development

    This file is part of the sysklogd package, a kernel and system log daemon.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/*
 * This file implements functions which are useful for building
 * a symbol lookup table based on the in kernel symbol table
 * maintained by the Linux kernel.
 *
 * Proper logging of kernel panics generated by loadable modules
 * tends to be difficult.  Since the modules are loaded dynamically
 * their addresses are not known at kernel load time.  A general
 * protection fault (Oops) cannot be properly deciphered with 
 * classic methods using the static symbol map produced at link time.
 *
 * One solution to this problem is to have klogd attempt to translate
 * addresses from module when the fault occurs.  By referencing the
 * the kernel symbol table proper resolution of these symbols is made
 * possible.
 *
 * At least that is the plan.
 *
 * Wed Aug 21 09:20:09 CDT 1996:  Dr. Wettstein
 *	The situation where no module support has been compiled into a
 *	kernel is now detected.  An informative message is output indicating
 *	that the kernel has no loadable module support whenever kernel
 *	module symbols are loaded.
 *
 *	An informative message is printed indicating the number of kernel
 *	modules and the number of symbols loaded from these modules.
 *
 * Sun Jun 15 16:23:29 MET DST 1997: Michael Alan Dorman
 *	Some more glibc patches made by <mdorman@debian.org>.
 *
 * Sat Jan 10 15:00:18 CET 1998: Martin Schulze <joey@infodrom.north.de>
 *	Fixed problem with klogd not being able to be built on a kernel
 *	newer than 2.1.18.  It was caused by modified structures
 *	inside the kernel that were included.  I have worked in a
 *	patch from Alessandro Suardi <asuardi@uninetcom.it>.
 *
 * Sun Jan 25 20:57:34 CET 1998: Martin Schulze <joey@infodrom.north.de>
 *	Another patch for Linux/alpha by Christopher C Chimelis
 *	<chris@classnet.med.miami.edu>.
 *
 * Thu Mar 19 23:39:29 CET 1998: Manuel Rodrigues <pmanuel@cindy.fe.up.pt>
 *	Changed lseek() to llseek() in order to support > 2GB address
 *	space which provided by kernels > 2.1.70.
 *
 * Mon Apr 13 18:18:45 CEST 1998: Martin Schulze <joey@infodrom.north.de>
 *	Removed <sys/module.h> as it's no longer part of recent glibc
 *	versions.  Added prototyp for llseek() which has been
 *	forgotton in <unistd.h> from glibc.  Added more log
 *	information if problems occurred while reading a system map
 *	file, by submission from Mark Simon Phillips <M.S.Phillips@nortel.co.uk>.
 *
 * Sun Jan  3 18:38:03 CET 1999: Martin Schulze <joey@infodrom.north.de>
 *	Corrected return value of AddModule if /dev/kmem can't be
 *	loaded.  This will prevent klogd from segfaulting if /dev/kmem
 *	is not available.  Patch from Topi Miettinen <tom@medialab.sonera.net>.
 *
 * Tue Sep 12 23:11:13 CEST 2000: Martin Schulze <joey@infodrom.ffis.de>
 *	Changed llseek() to lseek64() in order to skip a libc warning.
 *
 * Wed Mar 31 17:35:01 CEST 2004: Martin Schulze <joey@infodrom.org>
 *	Removed references to <linux/module.h> since it doesn't work
 *	anymore with its recent content from Linux 2.4/2.6, created
 *	module.h locally instead.
 */


/* Includes. */
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include "module.h"
#if !defined(__GLIBC__)
#include <linux/time.h>
#include <linux/linkage.h>
#else /* __GLIBC__ */
#include <linux/linkage.h>
extern __off64_t lseek64 __P ((int __fd, __off64_t __offset, int __whence));
extern int get_kernel_syms __P ((struct kernel_sym *__table));
#endif /* __GLIBC__ */
#include <stdarg.h>
#include <paths.h>
#include <linux/version.h>

#include "klogd.h"
#include "ksyms.h"


#if !defined(__GLIBC__)
/*
 * The following bit uses some kernel/library magic to produce what
 * looks like a function call to user level code.  This function is
 * actually a system call in disguise.  The purpose of the getsyms
 * call is to return a current copy of the in-kernel symbol table.
 */
#define __LIBRARY__
#include <linux/unistd.h>
#define __NR_getsyms __NR_get_kernel_syms
_syscall1(int, getsyms, struct kernel_sym *, syms);
#undef __LIBRARY__
extern int getsyms(struct kernel_sym *);
#else /* __GLIBC__ */
#define getsyms get_kernel_syms
#endif /* __GLIBC__ */

extern int query_module(const char *, int, void *, size_t, size_t *);

/* Variables static to this module. */
struct sym_table
{
	unsigned long value;
	char *name;
};

struct Module
{
	struct sym_table *sym_array;
	int num_syms;

	char *name;
	struct module module;
#if LINUX_VERSION_CODE >= 0x20112
	struct module_info module_info;
#endif
};

static int num_modules = 0;
struct Module *sym_array_modules = NULL;

static int have_modules = 0;

#if defined(TEST)
static int debugging = 1;
#else
extern int debugging;
#endif


/* Function prototypes. */
static void FreeModules(void);
static int AddSymbol(struct Module *mp, unsigned long, const char *);
static int AddModule(char *);
static int symsort(const void *, const void *);


/**************************************************************************
 * Function:	InitMsyms
 *
 * Purpose:	This function is responsible for building a symbol
 *		table which can be used to resolve addresses for
 *		loadable modules.
 *
 * Arguements:	Void
 *
 * Return:	A boolean return value is assumed.
 *
 *		A false value indicates that something went wrong.
 *
 *		True if loading is successful.
 **************************************************************************/

extern int InitMsyms()

{
	auto size_t	rtn;
	auto int	tmp;

	auto char	**mod_table,
			**p;

	char			*modbuf = NULL,
				*newbuf;

	int			 modsize = 32,
				 result;


	/* Initialize the kernel module symbol table. */
	FreeModules();

	/*
	 * New style symbol table parser.  This uses the newer query_module
	 * function rather than the old obsolete hack of stepping thru
	 * /dev/kmem.
	 */

	/*
	 * First, we query for the list of loaded modules.  We may
	 * have to grow our buffer in size.
	 */
	do {
		modsize+=modsize;
		newbuf=realloc(modbuf, modsize);

		if (newbuf==NULL) {
			/* Well, that sucks. */
			Syslog(LOG_ERR, "Error loading kernel symbols " \
			       "- %s\n", strerror(errno));
			if (modbuf!=NULL) free(modbuf);
			return(0);
		}

		modbuf=newbuf;

		result=query_module(NULL, QM_MODULES, modbuf, modsize, &rtn);

		if (result<0 && errno!=ENOSPC) {
			Syslog(LOG_ERR, "Error querying loaded modules " \
			       "- %s\n", strerror(errno));
			free(modbuf);
			return(0);
		}
	} while (result<0);

	if ( rtn <= 0 ) {
		/* No modules??? */
		Syslog(LOG_INFO, "No module symbols loaded - "
		       "modules disabled?\n");
		free(modbuf);
		return(0);
	}
	if ( debugging )
		fprintf(stderr, "Loading kernel module symbols - "
			"Size of table: %d\n", rtn);

	mod_table = (char **) malloc(rtn * sizeof(char *));
	if ( mod_table == NULL )
	{
		Syslog(LOG_WARNING, " Failed memory allocation for kernel " \
		       "symbol table.\n");
		free(modbuf);
		return(0);
	}

	sym_array_modules = (struct Module *) malloc(rtn * sizeof(struct Module));
	if ( sym_array_modules == NULL )
	{
		Syslog(LOG_WARNING, " Failed memory allocation for kernel " \
		       "symbol table.\n");
		free(mod_table);
		free(modbuf);
		return(0);
	}

	/*
	 * Build a symbol table compatible with the other one used by
	 * klogd.
	 */
	newbuf=modbuf;
	for (tmp=rtn-1; tmp>=0; tmp--)
	{
		mod_table[tmp]=newbuf;
		newbuf+=(strlen(newbuf)+1);
 		if ( !AddModule(mod_table[tmp]) )
		{
			Syslog(LOG_WARNING, "Error adding kernel module table "
				"entry.\n");
			free(mod_table);
			free(modbuf);
			return(0);
		}
	}

	have_modules = 1;

	/* Sort the symbol tables in each module. */
	for (rtn = tmp= 0; tmp < num_modules; ++tmp)
	{
		rtn += sym_array_modules[tmp].num_syms;
		if ( sym_array_modules[tmp].num_syms < 2 )
			continue;
		qsort(sym_array_modules[tmp].sym_array, \
		      sym_array_modules[tmp].num_syms, \
		      sizeof(struct sym_table), symsort);
	}

	if ( rtn == 0 )
		Syslog(LOG_INFO, "No module symbols loaded.");
	else
		Syslog(LOG_INFO, "Loaded %d %s from %d module%s", rtn, \
		       (rtn == 1) ? "symbol" : "symbols", \
		       num_modules, (num_modules == 1) ? "." : "s.");
	free(mod_table);
	free(modbuf);
	return(1);
}


static int symsort(p1, p2)

     const void *p1;

     const void *p2;

{
	auto const struct sym_table	*sym1 = p1,
					*sym2 = p2;

	if ( sym1->value < sym2->value )
		return(-1);
	if ( sym1->value == sym2->value )
		return(0);
	return(1);
}


/**************************************************************************
 * Function:	FreeModules
 *
 * Purpose:	This function is used to free all memory which has been
 *		allocated for the modules and their symbols.
 *
 * Arguements:	None specified.
 *
 * Return:	void
 **************************************************************************/

static void FreeModules()

{
	auto int	nmods,
			nsyms;

	auto struct Module *mp;


	/* Check to see if the module symbol tables need to be cleared. */
	have_modules = 0;

	if (sym_array_modules != NULL) {
		for (nmods= 0; nmods < num_modules; ++nmods)
		{
			mp = &sym_array_modules[nmods];
			if ( mp->num_syms == 0 )
				continue;
	       
			for (nsyms= 0; nsyms < mp->num_syms; ++nsyms)
				free(mp->sym_array[nsyms].name);
			free(mp->sym_array);
		}

		free(sym_array_modules);
		sym_array_modules = NULL;
	}

	num_modules = 0;
	return;
}


/**************************************************************************
 * Function:	AddModule
 *
 * Purpose:	This function is responsible for adding a module to
 *		the list of currently loaded modules.
 *
 * Arguements:	(char *) symbol
 *
 *		symbol:->	The name of the module.
 *
 * Return:	int
 **************************************************************************/

static int AddModule(symbol)

     char *symbol;

{
	size_t rtn;
	size_t i;
	const char *cbuf;
	int symsize=128;
	int result;
	struct module_symbol *symbuf=NULL,
			     *newbuf;

	auto struct Module *mp;


	/* Return if we have loaded the modules. */
	if ( have_modules )
		return(1);
	
	/* We already have space for the module. */
	mp = &sym_array_modules[num_modules];

	if (query_module(symbol, QM_INFO, &sym_array_modules[num_modules].module,
			 sizeof(struct module), &rtn)<0)
	{
		Syslog(LOG_WARNING, "Error reading module info for %s.\n",
		       symbol);
		return(0);
	}

	/* Save the module name. */
	mp->name = strdup(symbol);
	if ( mp->name == NULL )
		return(0);

	mp->num_syms = 0;
	mp->sym_array = NULL;
	++num_modules;

	/*
	 * First, we query for the list of exported symbols.  We may
	 * have to grow our buffer in size.
	 */
	do {
		symsize+=symsize;
		newbuf=realloc(symbuf, symsize);

		if (newbuf==NULL) {
			/* Well, that sucks. */
			Syslog(LOG_ERR, "Error loading kernel symbols " \
			       "- %s\n", strerror(errno));
			if (symbuf!=NULL) free(symbuf);
			return(0);
		}

		symbuf=newbuf;

		result=query_module(symbol, QM_SYMBOLS, symbuf, symsize, &rtn);

		if (result<0 && errno!=ENOSPC) {
			Syslog(LOG_ERR, "Error querying symbol list for %s " \
			       "- %s\n", symbol, strerror(errno));
			free(symbuf);
			return(0);
		}
	} while (result<0);

	if ( rtn < 0 ) {
		/* No symbols??? */
		Syslog(LOG_INFO, "No module symbols loaded - unknown error.\n");
		free(symbuf);
		return(0);
	}

	cbuf=(char *)symbuf;

	for (i=0; i<rtn; i++) {
		if (num_modules > 0)
			mp = &sym_array_modules[num_modules - 1];
		else
			mp = &sym_array_modules[0];

		AddSymbol(mp, symbuf[i].value,
			  cbuf+(unsigned long)(symbuf[i].name));
	}

	free(symbuf);
	return(1);
}


/**************************************************************************
 * Function:	AddSymbol
 *
 * Purpose:	This function is responsible for adding a symbol name
 *		and its address to the symbol table.
 *
 * Arguements:	(struct Module *) mp, (unsigned long) address, (char *) symbol
 *
 *		mp:->	A pointer to the module which the symbol is
 *			to be added to.
 *
 *		address:->	The address of the symbol.
 *
 *		symbol:->	The name of the symbol.
 *
 * Return:	int
 *
 *		A boolean value is assumed.  True if the addition is
 *		successful.  False if not.
 **************************************************************************/

static int AddSymbol(mp, address, symbol)

	struct Module *mp;     

	unsigned long address;
	
	const char *symbol;
	
{
	auto int tmp;


	/* Allocate space for the symbol table entry. */
	mp->sym_array = (struct sym_table *) realloc(mp->sym_array, \
        	(mp->num_syms+1) * sizeof(struct sym_table));
	if ( mp->sym_array == (struct sym_table *) 0 )
		return(0);

	/* Then the space for the symbol. */
	tmp = strlen(symbol);
	tmp += (strlen(mp->name) + 1);
	mp->sym_array[mp->num_syms].name = (char *) malloc(tmp + 1);
	if ( mp->sym_array[mp->num_syms].name == (char *) 0 )
		return(0);
	memset(mp->sym_array[mp->num_syms].name, '\0', tmp + 1);
	
	/* Stuff interesting information into the module. */
	mp->sym_array[mp->num_syms].value = address;
	strcpy(mp->sym_array[mp->num_syms].name, mp->name);
	strcat(mp->sym_array[mp->num_syms].name, ":");
	strcat(mp->sym_array[mp->num_syms].name, symbol);
	++mp->num_syms;

	return(1);
}


/**************************************************************************
 * Function:	LookupModuleSymbol
 *
 * Purpose:	Find the symbol which is related to the given address from
 *		a kernel module.
 *
 * Arguements:	(long int) value, (struct symbol *) sym
 *
 *		value:->	The address to be located.
 * 
 *		sym:->		A pointer to a structure which will be
 *				loaded with the symbol's parameters.
 *
 * Return:	(char *)
 *
 *		If a match cannot be found a diagnostic string is printed.
 *		If a match is found the pointer to the symbolic name most
 *		closely matching the address is returned.
 **************************************************************************/

extern char * LookupModuleSymbol(value, sym)

	unsigned long value;

	struct symbol *sym;
	
{
	auto int	nmod,
			nsym;

	auto struct sym_table *last;

	auto struct Module *mp;


	sym->size = 0;
	sym->offset = 0;
	if ( num_modules == 0 )
		return((char *) 0);
	
	for(nmod= 0; nmod < num_modules; ++nmod)
	{
		mp = &sym_array_modules[nmod];

		/*
		 * Run through the list of symbols in this module and
		 * see if the address can be resolved.
		 */
		for(nsym= 1, last = &mp->sym_array[0];
		    nsym < mp->num_syms;
		    ++nsym)
		{
			if ( mp->sym_array[nsym].value > value )
			{		
				sym->offset = value - last->value;
				sym->size = mp->sym_array[nsym].value - \
					last->value;
				return(last->name);
			}
			last = &mp->sym_array[nsym];
		}


		/*
		 * At this stage of the game we still cannot give up the
		 * ghost.  There is the possibility that the address is
		 * from a module which has no symbols registered with
		 * the kernel.  The solution is to compare the address
		 * against the starting address and extant of the module
		 * If it is in this range we can at least return the
		 * name of the module.
		 */
#if LINUX_VERSION_CODE < 0x20112
		if ( (void *) value >= mp->module.addr &&
		     (void *) value <= (mp->module.addr + \
					mp->module.size * 4096) )
#else
		if ( value >= mp->module_info.addr &&
		     value <= (mp->module_info.addr + \
					mp->module.size * 4096) )
#endif
		{
			/*
			 * A special case needs to be checked for.  The above
			 * conditional tells us that we are within the
			 * extant of this module but symbol lookup has
			 * failed.
			 *
			 * We need to check to see if any symbols have
			 * been defined in this module.  If there have been
			 * symbols defined the assumption must be made that
			 * the faulting address lies somewhere beyond the
			 * last symbol.  About the only thing we can do
			 * at this point is use an offset from this
			 * symbol.
			 */
			if ( mp->num_syms > 0 )
			{
				last = &mp->sym_array[mp->num_syms - 1];
#if LINUX_VERSION_CODE < 0x20112
				sym->size = (int) mp->module.addr + \
					(mp->module.size * 4096) - value;
#else
				sym->size = (int) mp->module_info.addr + \
					(mp->module.size * 4096) - value;
#endif
				sym->offset = value - last->value;
				return(last->name);
			}

			/*
			 * There were no symbols defined for this module.
			 * Return the module name and the offset of the
			 * faulting address in the module.
			 */
			sym->size = mp->module.size * 4096;
#if LINUX_VERSION_CODE < 0x20112
			sym->offset = (void *) value - mp->module.addr;
#else
			sym->offset = value - mp->module_info.addr;
#endif
			return(mp->name);
		}
	}

	/* It has been a hopeless exercise. */
	return((char *) 0);
}


/*
 * Setting the -DTEST define enables the following code fragment to
 * be compiled.  This produces a small standalone program which will
 * dump the current kernel symbol table.
 */
#if defined(TEST)

#include <stdarg.h>


extern int main(int, char **);


int main(argc, argv)

	int argc;

	char *argv[];

{
	auto int lp, syms;


	if ( !InitMsyms() )
	{
		fprintf(stderr, "Cannot load module symbols.\n");
		return(1);
	}

	printf("Number of modules: %d\n\n", num_modules);

	for(lp= 0; lp < num_modules; ++lp)
	{
		printf("Module #%d = %s, Number of symbols = %d\n", lp + 1, \
		       sym_array_modules[lp].name, \
		       sym_array_modules[lp].num_syms);

		for (syms= 0; syms < sym_array_modules[lp].num_syms; ++syms)
		{
			printf("\tSymbol #%d\n", syms + 1);
			printf("\tName: %s\n", \
			       sym_array_modules[lp].sym_array[syms].name);
			printf("\tAddress: %lx\n\n", \
			       sym_array_modules[lp].sym_array[syms].value);
		}
	}

	FreeModules();
	return(0);
}

extern void Syslog(int priority, char *fmt, ...)

{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stdout, "Pr: %d, ", priority);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
	fputc('\n', stdout);

	return;
}

#endif
